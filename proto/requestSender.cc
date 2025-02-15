#define TRACE_NAME "RequestSender"

#include <boost/program_options.hpp>
#include <memory>

#include "artdaq-core/Utilities/configureMessageFacility.hh"
#include "artdaq/Application/LoadParameterSet.hh"
#include "artdaq/DAQrate/detail/RequestReceiver.hh"
#include "artdaq/DAQrate/detail/RequestSender.hh"

#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/types/Atom.h"
#include "fhiclcpp/types/Comment.h"
#include "fhiclcpp/types/Name.h"
#include "fhiclcpp/types/TableFragment.h"

int main(int argc, char* argv[])
try
{
	artdaq::configureMessageFacility("RequestSender");

	struct Config
	{
		fhicl::TableFragment<artdaq::RequestSender::Config> senderConfig;
		fhicl::Atom<bool> use_receiver{fhicl::Name{"use_receiver"}, fhicl::Comment{"Whether to setup a RequestReceiver to verify that requests are being sent"}, false};
		fhicl::Atom<size_t> receiver_timeout_ms{fhicl::Name{"recevier_timeout_ms"}, fhicl::Comment{"Amount of time to wait for the receiver to receive a request message"}, 1000};
		fhicl::Table<artdaq::RequestReceiver::Config> receiver_config{fhicl::Name{"receiver_config"}, fhicl::Comment{"Configuration for RequestReceiver, if used"}};
		fhicl::Atom<int> num_requests{fhicl::Name{"num_requests"}, fhicl::Comment{"Number of requests to send"}};
		fhicl::Atom<artdaq::Fragment::sequence_id_t> starting_sequence_id{fhicl::Name{"starting_sequence_id"}, fhicl::Comment{"Sequence ID of first request"}, 1};
		fhicl::Atom<artdaq::Fragment::sequence_id_t> sequence_id_scale{fhicl::Name{"sequence_id_scale"}, fhicl::Comment{"Amount to increment Sequence ID for each request"}, 1};
		fhicl::Atom<artdaq::Fragment::timestamp_t> starting_timestamp{fhicl::Name{"starting_timestamp"}, fhicl::Comment{"Timestamp of first request"}, 1};
		fhicl::Atom<artdaq::Fragment::timestamp_t> timestamp_scale{fhicl::Name{"timestamp_scale"}, fhicl::Comment{"Amount to increment timestamp for each request"}, 1};
	};

	auto pset = LoadParameterSet<Config>(argc, argv, "sender", "This test application sends Data Request messages and optionally receives them to detect issues in the network transport");

	fhicl::ParameterSet tempPset;
	if (pset.has_key("daq"))
	{
		fhicl::ParameterSet daqPset = pset.get<fhicl::ParameterSet>("daq");
		for (auto& name : daqPset.get_pset_names())
		{
			auto thisPset = daqPset.get<fhicl::ParameterSet>(name);
			if (thisPset.has_key("send_requests"))
			{
				tempPset = thisPset;
			}
		}
	}
	else
	{
		tempPset = pset;
	}

	int rc = 0;

	artdaq::RequestSender sender(tempPset);

	std::unique_ptr<artdaq::RequestReceiver> receiver(nullptr);
	std::shared_ptr<artdaq::RequestBuffer> request_buffer(nullptr);
	int num_requests = tempPset.get<int>("num_requests", 1);
	if (tempPset.get<bool>("use_receiver", false))
	{
		auto receiver_pset = tempPset.get<fhicl::ParameterSet>("request_receiver", fhicl::ParameterSet());
		request_buffer = std::make_shared<artdaq::RequestBuffer>(receiver_pset.get<artdaq::Fragment::sequence_id_t>("request_increment", 1));
		receiver = std::make_unique<artdaq::RequestReceiver>(receiver_pset, request_buffer);
		receiver->startRequestReception();
	}

	auto seq = tempPset.get<artdaq::Fragment::sequence_id_t>("starting_sequence_id", 1);
	auto seq_scale = tempPset.get<artdaq::Fragment::sequence_id_t>("sequence_id_scale", 1);
	auto ts = tempPset.get<artdaq::Fragment::timestamp_t>("starting_timestamp", 1);
	auto ts_scale = tempPset.get<artdaq::Fragment::timestamp_t>("timestamp_scale", 1);
	auto tmo = tempPset.get<size_t>("recevier_timeout_ms", 1000);

	for (auto ii = 0; ii < num_requests; ++ii)
	{
		TLOG(TLVL_INFO) << "Sending request " << ii << " of " << num_requests << " with sequence id " << seq;
		sender.AddRequest(seq, ts);
		sender.SendRequest();

		if (request_buffer)
		{
			auto start_time = std::chrono::steady_clock::now();
			bool recvd = false;
			TLOG(TLVL_INFO) << "Starting receive loop for request " << ii;
			while (!recvd && artdaq::TimeUtils::GetElapsedTimeMilliseconds(start_time) < tmo)
			{
				auto reqs = request_buffer->GetRequests();
				if (reqs.count(seq) != 0u)
				{
					TLOG(TLVL_INFO) << "Received Request for Sequence ID " << seq << ", timestamp " << reqs[seq];
					request_buffer->RemoveRequest(seq);
					sender.RemoveRequest(seq);
					recvd = true;
				}
				else
				{
					usleep(10000);
				}
			}
			if (artdaq::TimeUtils::GetElapsedTimeMilliseconds(start_time) >= tmo)
			{
				TLOG(TLVL_ERROR) << "Timeout elapsed in requestSender";
				return -2;
			}
		}

		seq += seq_scale;
		ts += ts_scale;
	}

	return rc;
}
catch (...)
{
	return -1;
}
