#include "TRACE/tracemf.h"
#include "artdaq/DAQdata/Globals.hh"  // Before trace.h gets included in ConcurrentQueue (from GlobalQueue)
#define TRACE_NAME (app_name + "_RequestSender").c_str()
#include "artdaq/DAQrate/detail/RequestSender.hh"

#include "artdaq/DAQdata/TCPConnect.hh"

#include "fhiclcpp/ParameterSet.h"

#include <boost/thread.hpp>

#include <dlfcn.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace artdaq {
RequestSender::RequestSender(const fhicl::ParameterSet& pset)
    : send_requests_(pset.get<bool>("send_requests", false))
    , initialized_(false)
    , request_address_(pset.get<std::string>("request_address", "227.128.12.26"))
    , request_port_(pset.get<int>("request_port", 3001))
    , request_delay_(pset.get<size_t>("request_delay_ms", 0) * 1000)
    , request_shutdown_timeout_us_(pset.get<size_t>("request_shutdown_timeout_us", 100000))
    , request_socket_(-1)
    , multicast_out_addr_(pset.get<std::string>("multicast_interface_ip", pset.get<std::string>("output_address", "0.0.0.0")))
    , request_mode_(detail::RequestMessageMode::Normal)
    , min_request_interval_ms_(pset.get<size_t>("min_request_interval_ms", 100))
    , request_sending_(0)
    , run_number_(0)
{
	TLOG(TLVL_DEBUG) << "RequestSender CONSTRUCTOR pset=" << pset.to_string();
	setup_requests_();

	TLOG(TLVL_DEBUG + 35) << "artdaq::RequestSender::RequestSender ctor - reader_thread_ initialized";
	initialized_ = true;
}

RequestSender::~RequestSender()
{
	TLOG(TLVL_INFO) << "Shutting down RequestSender: Waiting for " << request_sending_.load() << " requests to be sent";

	auto start_time = std::chrono::steady_clock::now();

	while (request_sending_.load() > 0 && request_shutdown_timeout_us_ + request_delay_ > TimeUtils::GetElapsedTimeMicroseconds(start_time))
	{
		usleep(1000);
	}
	{
		std::lock_guard<std::mutex> lk(request_mutex_);
		std::lock_guard<std::mutex> lk2(request_send_mutex_);
	}
	TLOG(TLVL_INFO) << "Shutting down RequestSender: request_socket_: " << request_socket_;
	if (request_socket_ != -1)
	{
		if (shutdown(request_socket_, 2) != 0 && errno == ENOTSOCK)
		{
			TLOG(TLVL_ERROR) << "Shutdown of request_socket_ resulted in ENOTSOCK. NOT Closing file descriptor!";
		}
		else
		{
			close(request_socket_);
		}
		request_socket_ = -1;
	}
}

void RequestSender::SetRequestMode(detail::RequestMessageMode mode)
{
	{
		std::lock_guard<std::mutex> lk(request_mutex_);
		request_mode_ = mode;
	}
	SendRequest(true);
}

void RequestSender::setup_requests_()
{
	if (send_requests_)
	{
		request_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (request_socket_ < 0)
		{
			TLOG(TLVL_ERROR) << "I failed to create the socket for sending Data Requests! err=" << strerror(errno);
			exit(1);
		}
		int sts = ResolveHost(request_address_.c_str(), request_port_, request_addr_);
		if (sts == -1)
		{
			TLOG(TLVL_ERROR) << "Unable to resolve Data Request address, err=" << strerror(errno);
			exit(1);
		}

		/*		if (multicast_out_addr_ == "0.0.0.0")
		{
		    char hostname[HOST_NAME_MAX];
		    sts = gethostname(hostname, HOST_NAME_MAX);
		    multicast_out_addr_ = std::string(hostname);
		    if (sts < 0)
		    {
		        TLOG(TLVL_ERROR) << "Could not get current hostname,  err=" << strerror(errno);
		        exit(1);
		    }
		}*/

		// For 0.0.0.0, use system-specified IP_MULTICAST_IF
		if (multicast_out_addr_ != "localhost" && multicast_out_addr_ != "0.0.0.0")
		{
			struct in_addr addr;
			sts = GetInterfaceForNetwork(multicast_out_addr_.c_str(), addr);
			// sts = ResolveHost(multicast_out_addr_.c_str(), addr);
			if (sts == -1)
			{
				TLOG(TLVL_ERROR) << "Unable to determine the  multicast interface address for " << multicast_out_addr_ << ", err=" << strerror(errno);
				exit(1);
			}
			char addr_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &(addr), addr_str, INET_ADDRSTRLEN);
			TLOG(TLVL_INFO) << "Successfully determined the multicast network interface for " << multicast_out_addr_ << ": " << addr_str;

			if (setsockopt(request_socket_, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof(addr)) == -1)
			{
				TLOG(TLVL_ERROR) << "Cannot set outgoing interface, err=" << strerror(errno);
				exit(1);
			}
		}
		int yes = 1;
		if (setsockopt(request_socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		{
			TLOG(TLVL_ERROR) << "Unable to enable port reuse on request socket, err=" << strerror(errno);
			exit(1);
		}
		if (setsockopt(request_socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &yes, sizeof(yes)) < 0)
		{
			TLOG(TLVL_ERROR) << "Unable to enable multicast loopback on request socket, err=" << strerror(errno);
			exit(1);
		}
		if (setsockopt(request_socket_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) == -1)
		{
			TLOG(TLVL_ERROR) << "Cannot set request socket to broadcast, err=" << strerror(errno);
			exit(1);
		}
	}
}

void RequestSender::do_send_request_()
{
	if (!send_requests_)
	{
		request_sending_--;
		return;
	}
	if (request_socket_ == -1)
	{
		setup_requests_();
	}

	TLOG(TLVL_DEBUG + 33) << "Waiting for " << request_delay_ << " microseconds.";
	std::this_thread::sleep_for(std::chrono::microseconds(request_delay_));

	TLOG(TLVL_DEBUG + 33) << "Creating RequestMessage";
	detail::RequestMessage message;
	message.setRank(my_rank);
	message.setRunNumber(run_number_);
	{
		std::lock_guard<std::mutex> lk(request_mutex_);
		for (auto& req : active_requests_)
		{
			TLOG(TLVL_DEBUG + 36) << "Adding a request with sequence ID " << req.first << ", timestamp " << req.second << " to request message";
			message.addRequest(req.first, req.second);
		}
		TLOG(TLVL_DEBUG + 33) << "Setting mode flag in Message Header to " << static_cast<int>(request_mode_);
		message.setMode(request_mode_);
	}
	char str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &(request_addr_.sin_addr), str, INET_ADDRSTRLEN);
	std::lock_guard<std::mutex> lk2(request_send_mutex_);
	TLOG(TLVL_DEBUG + 33) << "Sending request for " << message.size() << " events to multicast group " << str
	                      << ", port " << request_port_ << ", interface " << multicast_out_addr_;
	auto buf = message.GetMessage();
	auto sts = sendto(request_socket_, &buf[0], buf.size(), 0, reinterpret_cast<struct sockaddr*>(&request_addr_), sizeof(request_addr_));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	if (sts < 0 || static_cast<size_t>(sts) != buf.size())
	{
		TLOG(TLVL_ERROR) << "Error sending request message err=" << strerror(errno) << "sts=" << sts;
		request_socket_ = -1;
		request_sending_--;
		return;
	}
	TLOG(TLVL_DEBUG + 33) << "Done sending request sts=" << sts;
	request_sending_--;
}

void RequestSender::SendRequest(bool endOfRunOnly)
{
	while (!initialized_)
	{
		usleep(1000);
	}

	if (!send_requests_)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lk(request_mutex_);
		if (endOfRunOnly && request_mode_ != detail::RequestMessageMode::EndOfRun)
		{
			return;
		}
	}
	last_request_send_time_ = std::chrono::steady_clock::now();
	request_sending_++;
	boost::thread request([this] { do_send_request_(); });
	request.detach();
}

void RequestSender::AddRequest(Fragment::sequence_id_t seqID, Fragment::timestamp_t timestamp)
{
	while (!initialized_)
	{
		usleep(1000);
	}

	{
		std::lock_guard<std::mutex> lk(request_mutex_);
		if (active_requests_.count(seqID) == 0u)
		{
			TLOG(TLVL_DEBUG + 37) << "Adding request for sequence ID " << seqID << " and timestamp " << timestamp << " to request list.";
			active_requests_[seqID] = timestamp;
		}

		while (active_requests_.size() > detail::RequestMessage::max_request_count())
		{
			TLOG(TLVL_WARNING) << "Erasing request with seqID " << active_requests_.begin()->first << " due to over-large request list size! (" << active_requests_.size() << " / " << detail::RequestMessage::max_request_count() << ")";
			active_requests_.erase(active_requests_.begin());
		}
	}
	SendRequest(TimeUtils::GetElapsedTimeMilliseconds(last_request_send_time_) < min_request_interval_ms_);
}

void RequestSender::RemoveRequest(Fragment::sequence_id_t seqID)
{
	while (!initialized_)
	{
		usleep(1000);
	}
	std::lock_guard<std::mutex> lk(request_mutex_);
	TLOG(TLVL_DEBUG + 38) << "Removing request for sequence ID " << seqID << " from request list.";
	active_requests_.erase(seqID);
}
}  // namespace artdaq
