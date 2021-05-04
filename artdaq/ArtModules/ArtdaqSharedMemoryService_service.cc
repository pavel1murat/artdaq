#define TRACE_NAME "ArtdaqSharedMemoryService"

#include <cstdint>
#include <memory>

#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "artdaq-core/Core/SharedMemoryEventReceiver.hh"
#include "artdaq-core/Utilities/ExceptionHandler.hh"
#include "artdaq/ArtModules/ArtdaqSharedMemoryService.h"

#include "artdaq/DAQdata/Globals.hh"

#define build_key(seed) ((seed) + ((GetPartitionNumber() + 1) << 16) + (getppid() & 0xFFFF))

static fhicl::ParameterSet empty_pset;

ArtdaqSharedMemoryService::ArtdaqSharedMemoryService(fhicl::ParameterSet const& pset, art::ActivityRegistry& /*unused*/)
    : incoming_events_(nullptr)
    , evtHeader_(nullptr)
    , read_timeout_(pset.get<size_t>("read_timeout_us", static_cast<size_t>(pset.get<double>("waiting_time", 600.0) * 1000000)))
    , resume_after_timeout_(pset.get<bool>("resume_after_timeout", true))
{
	TLOG(TLVL_TRACE) << "ArtdaqSharedMemoryService CONSTRUCTOR";

	incoming_events_ = std::make_unique<artdaq::SharedMemoryEventReceiver>(
	    pset.get<int>("shared_memory_key", build_key(0xEE000000)),
	    pset.get<int>("broadcast_shared_memory_key", build_key(0xBB000000)));

	char const* artapp_env = getenv("ARTDAQ_APPLICATION_NAME");
	std::string artapp_str;
	if (artapp_env != nullptr)
	{
		artapp_str = std::string(artapp_env) + "_";
	}

	TLOG(TLVL_TRACE) << "Setting app_name";
	app_name = artapp_str + "art" + std::to_string(incoming_events_->GetMyId());
	//artdaq::configureMessageFacility(app_name.c_str()); // ELF 11/20/2020: MessageFacility already configured by initialization pset

	artapp_env = getenv("ARTDAQ_RANK");
	if (artapp_env != nullptr && my_rank < 0)
	{
		TLOG(TLVL_TRACE) << "Setting rank from envrionment";
		my_rank = strtol(artapp_env, nullptr, 10);
	}
	else
	{
		TLOG(TLVL_TRACE) << "Setting my_rank from shared memory";
		my_rank = incoming_events_->GetRank();
	}

	try
	{
		if (metricMan)
		{
			metricMan->initialize(pset.get<fhicl::ParameterSet>("metrics", fhicl::ParameterSet()), app_name);
			metricMan->do_start();
		}
	}
	catch (...)
	{
		artdaq::ExceptionHandler(artdaq::ExceptionHandlerRethrow::no, "Error loading metrics in ArtdaqSharedMemoryService()");
	}

	TLOG(TLVL_INFO) << "app_name is " << app_name << ", rank " << my_rank;
}

ArtdaqSharedMemoryService::~ArtdaqSharedMemoryService()
{
	artdaq::Globals::CleanUpGlobals();
}

std::unordered_map<artdaq::Fragment::type_t, std::unique_ptr<artdaq::Fragments>> ArtdaqSharedMemoryService::ReceiveEvent(bool broadcast)
{
	TLOG(TLVL_TRACE) << "ReceiveEvent BEGIN";
	std::unordered_map<artdaq::Fragment::type_t, std::unique_ptr<artdaq::Fragments>> recvd_fragments;

	while (recvd_fragments.empty())
	{
		TLOG(TLVL_TRACE) << "ReceiveEvent: Waiting for available buffer";
		bool got_event = false;
		auto start_time = std::chrono::steady_clock::now();
		auto read_timeout_to_use = read_timeout_ > 100000 ? 100000 : read_timeout_;
		if (!resume_after_timeout_ || broadcast) read_timeout_to_use = read_timeout_;
		while (!incoming_events_->IsEndOfData() && !got_event)
		{
			got_event = incoming_events_->ReadyForRead(broadcast, read_timeout_to_use);
			if (!got_event && (!resume_after_timeout_ || broadcast))  // Only try broadcasts once!
			{
				TLOG(TLVL_ERROR) << "Timeout occurred! No data received after " << read_timeout_to_use << " us. Returning empty Fragment list!";
				return recvd_fragments;
			}
			if (!got_event && artdaq::TimeUtils::GetElapsedTimeMicroseconds(start_time) > read_timeout_)
			{
				TLOG(TLVL_WARNING) << "Timeout occurred! No data received after " << artdaq::TimeUtils::GetElapsedTimeMicroseconds(start_time) << " us. Retrying.";
			}
		}
		if (incoming_events_->IsEndOfData())
		{
			TLOG(TLVL_INFO) << "End of Data signal received, exiting";
			return recvd_fragments;
		}

		TLOG(TLVL_TRACE) << "ReceiveEvent: Reading buffer header";
		auto errflag = false;
		auto hdrPtr = incoming_events_->ReadHeader(errflag);
		if (errflag || hdrPtr == nullptr)
		{  // Buffer was changed out from under reader!
			incoming_events_->ReleaseBuffer();
			continue;  //retry
			           //return recvd_fragments;
		}
		evtHeader_ = std::make_shared<artdaq::detail::RawEventHeader>(*hdrPtr);
		TLOG(TLVL_TRACE) << "ReceiveEvent: Getting Fragment types";
		auto fragmentTypes = incoming_events_->GetFragmentTypes(errflag);
		if (errflag)
		{  // Buffer was changed out from under reader!
			incoming_events_->ReleaseBuffer();
			continue;  //retry
			           //return recvd_fragments;
		}
		if (fragmentTypes.empty())
		{
			TLOG(TLVL_ERROR) << "Event has no Fragments! Aborting!";
			incoming_events_->ReleaseBuffer();
			return recvd_fragments;
		}

		for (auto const& type : fragmentTypes)
		{
			TLOG(TLVL_TRACE) << "receiveMessage: Getting all Fragments of type " << type;
			recvd_fragments[type] = incoming_events_->GetFragmentsByType(errflag, type);
			if (!recvd_fragments[type])
			{
				TLOG(TLVL_ERROR) << "Error retrieving Fragments from shared memory! (Most likely due to a buffer overwrite) Retrying...";
				incoming_events_->ReleaseBuffer();
				recvd_fragments.clear();
				continue;
			}
			/* Events coming out of the EventStore are not sorted but need to be
       sorted by sequence ID before they can be passed to art.
    */
			std::sort(recvd_fragments[type]->begin(), recvd_fragments[type]->end(), artdaq::fragmentSequenceIDCompare);
		}
		TLOG(TLVL_TRACE) << "receiveMessage: Releasing buffer";
		incoming_events_->ReleaseBuffer();
	}

	TLOG(TLVL_TRACE) << "receiveMessage END";
	return recvd_fragments;
}

DEFINE_ART_SERVICE_INTERFACE_IMPL(ArtdaqSharedMemoryService, ArtdaqSharedMemoryServiceInterface)
