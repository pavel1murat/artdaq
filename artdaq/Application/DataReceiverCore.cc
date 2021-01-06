#include "art/Framework/Art/artapp.h"
#include "canvas/Utilities/Exception.h"

#include "artdaq/DAQdata/Globals.hh"  // include these 2 first -
#define TRACE_NAME (app_name + "_DataReceiverCore").c_str()
#include "artdaq-core/Utilities/ExceptionHandler.hh"

#include "artdaq/Application/DataReceiverCore.hh"
#include "artdaq/TransferPlugins/TransferInterface.hh"

#include <iomanip>
#include <memory>

artdaq::DataReceiverCore::DataReceiverCore()
    : stop_requested_(false)
    , pause_requested_(false)
    , run_is_paused_(false)

{
	TLOG(TLVL_DEBUG) << "Constructor";
}

artdaq::DataReceiverCore::~DataReceiverCore()
{
	TLOG(TLVL_DEBUG) << "Destructor";
}

bool artdaq::DataReceiverCore::initializeDataReceiver(fhicl::ParameterSet const& pset, fhicl::ParameterSet const& data_pset, fhicl::ParameterSet const& metric_pset)
{
	// other parameters
	verbose_ = data_pset.get<bool>("verbose", true);

	// TRACE here so that mftrace_module and mftrace_iteration are ready by mftrace...should set it for all subsequent traces
	TLOG(TLVL_INFO) << "Initializing Data Receiver";

	if (metric_pset.is_empty())
	{
		TLOG(TLVL_INFO) << "No metric plugins appear to be defined";
	}
	try
	{
		metricMan->initialize(metric_pset, app_name);
	}
	catch (...)
	{
		ExceptionHandler(ExceptionHandlerRethrow::no,
		                 "Error loading metrics in DataReceiverCore::initialize()");
	}

	fhicl::ParameterSet art_pset = pset;
	if (art_pset.has_key("art"))
	{
		art_pset = art_pset.get<fhicl::ParameterSet>("art");
	}
	else
	{
		art_pset.erase("daq");
	}

	auto art_source_pset = art_pset.get<fhicl::ParameterSet>("source");
	art_source_pset.put<fhicl::ParameterSet>("metrics", metric_pset);
	art_pset.erase("source");
	art_pset.put<fhicl::ParameterSet>("source", art_source_pset);

	fhicl::ParameterSet data_tmp = data_pset;
	if (data_pset.has_key("expected_events_per_bunch"))
	{
		data_tmp.put<int>("expected_fragments_per_event", data_pset.get<int>("expected_events_per_bunch"));
	}

	if (data_pset.has_key("rank"))
	{
		if (my_rank >= 0 && data_pset.get<int>("rank") != my_rank)
		{
			TLOG(TLVL_WARNING) << "Rank specified at startup is different than rank specified at configure! Using rank received at configure!";
		}
		my_rank = data_pset.get<int>("rank");
	}
	if (my_rank == -1)
	{
		TLOG(TLVL_ERROR) << "Rank not specified at startup or in configuration! Aborting";
		exit(1);
	}

	event_store_ptr_ = std::make_shared<SharedMemoryEventManager>(data_tmp, art_pset);
	art_pset_ = art_pset;
	TLOG(TLVL_DEBUG) << "Resulting art_pset_: \"" << art_pset_.to_string() << "\".";

	receiver_ptr_ = std::make_unique<artdaq::DataReceiverManager>(data_tmp, event_store_ptr_);

	return true;
}

bool artdaq::DataReceiverCore::start(art::RunID id)
{
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Starting run " << id.run();

	// 13-Jul-2018, KAB: added code to update the art_pset inside the event store
	// with configuration archive information
	// so that the config info will be stored in the output art/ROOT file.
	// (Note that we don't bother looping over the config_archive_entries if that
	// map is empty, but we *do* still update the art configuration with art_pset_
	// at each begin-run because the config archive may be non-empty one time through
	// and then empty the next time.)
	fhicl::ParameterSet temp_pset = art_pset_;
	if (!config_archive_entries_.empty())
	{
		fhicl::ParameterSet config_pset;
		for (auto& entry : config_archive_entries_)
		{
			config_pset.put(entry.first, entry.second);
		}
		temp_pset.put_or_replace("configuration_documents", config_pset);
	}
	event_store_ptr_->UpdateArtConfiguration(temp_pset);

	stop_requested_.store(false);
	pause_requested_.store(false);
	run_is_paused_.store(false);
	metricMan->do_start();
	event_store_ptr_->startRun(id.run());
	receiver_ptr_->start_threads();

	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Completed the Start transition for run " << event_store_ptr_->runID();
	return true;
}

bool artdaq::DataReceiverCore::stop()
{
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Stopping run " << event_store_ptr_->runID();
	bool endSucceeded = false;
	int attemptsToEnd;
	receiver_ptr_->stop_threads();

	// 21-Jun-2013, KAB - the stop_requested_ variable must be set
	// before the flush lock so that the processFragments loop will
	// exit (after the timeout), the lock will be released (in the
	// processFragments method), and this method can continue.
	stop_requested_.store(true);

	TLOG(TLVL_DEBUG) << "Ending run " << event_store_ptr_->runID();
	attemptsToEnd = 1;
	endSucceeded = event_store_ptr_->endRun();
	while (!endSucceeded && attemptsToEnd < 3)
	{
		++attemptsToEnd;
		TLOG(TLVL_DEBUG) << "Retrying EventStore::endRun()";
		endSucceeded = event_store_ptr_->endRun();
	}
	if (!endSucceeded)
	{
		TLOG(TLVL_ERROR)
		    << "EventStore::endRun in stop method failed after three tries.";
	}
	TLOG(TLVL_DEBUG) << "Done Ending run " << event_store_ptr_->runID();

	attemptsToEnd = 1;
	TLOG(TLVL_DEBUG) << "stop: Calling EventStore::endOfData";
	endSucceeded = event_store_ptr_->endOfData();
	while (!endSucceeded && attemptsToEnd < 3)
	{
		++attemptsToEnd;
		TLOG(TLVL_DEBUG) << "Retrying EventStore::endOfData()";
		endSucceeded = event_store_ptr_->endOfData();
	}

	run_is_paused_.store(false);
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Completed the Stop transition for run " << event_store_ptr_->runID();
	return true;
}

bool artdaq::DataReceiverCore::pause()
{
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Pausing run " << event_store_ptr_->runID();
	pause_requested_.store(true);
	run_is_paused_.store(true);
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Completed the Pause transition for run " << event_store_ptr_->runID();
	return true;
}

bool artdaq::DataReceiverCore::resume()
{
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Resuming run " << event_store_ptr_->runID();
	pause_requested_.store(false);
	metricMan->do_start();
	event_store_ptr_->rolloverSubrun();
	run_is_paused_.store(false);
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Completed the Resume transition for run " << event_store_ptr_->runID();
	return true;
}

bool artdaq::DataReceiverCore::shutdown()
{
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Starting Shutdown transition";

	/* We don't care about flushing data here.  The only way to transition to the
	   shutdown state is from a state where there is no data taking.  All we have
	   to do is signal the art input module that we're done taking data so that
	   it can wrap up whatever it needs to do. */

	TLOG(TLVL_DEBUG) << "shutdown: Shutting down DataReceiverManager";
	receiver_ptr_.reset(nullptr);

	bool endSucceeded = false;
	int attemptsToEnd = 1;
	TLOG(TLVL_DEBUG) << "shutdown: Calling EventStore::endOfData";
	endSucceeded = event_store_ptr_->endOfData();
	while (!endSucceeded && attemptsToEnd < 3)
	{
		++attemptsToEnd;
		TLOG(TLVL_DEBUG) << "Retrying EventStore::endOfData()";
		endSucceeded = event_store_ptr_->endOfData();
	}

	TLOG(TLVL_DEBUG) << "shutdown: Shutting down SharedMemoryEventManager";
	event_store_ptr_.reset();

	TLOG(TLVL_DEBUG) << "shutdown: Shutting down MetricManager";
	metricMan->shutdown();

	TLOG(TLVL_DEBUG) << "shutdown: Complete";
	TLOG((verbose_ ? TLVL_INFO : TLVL_DEBUG)) << "Completed Shutdown transition";
	return endSucceeded;
}

bool artdaq::DataReceiverCore::soft_initialize(fhicl::ParameterSet const& pset)
{
	TLOG(TLVL_DEBUG) << "soft_initialize method called with DAQ "
	                 << "ParameterSet = \"" << pset.to_string()
	                 << "\".";
	return true;
}

bool artdaq::DataReceiverCore::reinitialize(fhicl::ParameterSet const& pset)
{
	TLOG(TLVL_DEBUG) << "reinitialize method called with DAQ "
	                 << "ParameterSet = \"" << pset.to_string()
	                 << "\".";
	event_store_ptr_ = nullptr;
	return initialize(pset);
}

bool artdaq::DataReceiverCore::rollover_subrun(uint64_t boundary, uint32_t subrun)
{
	if (event_store_ptr_)
	{
		event_store_ptr_->rolloverSubrun(boundary, subrun);
		return true;
	}
	return false;
}

std::string artdaq::DataReceiverCore::report(std::string const& which) const
{
	if (which == "incomplete_event_count")
	{
		if (event_store_ptr_ != nullptr)
		{
			return std::to_string(event_store_ptr_->GetIncompleteEventCount());
		}

		return "-1";
	}
	if (which == "event_count")
	{
		if (receiver_ptr_ != nullptr)
		{
			return std::to_string(receiver_ptr_->GetReceivedFragmentCount()->count());
		}

		return "0";
	}

	// lots of cool stuff that we can do here
	// - report on the number of fragments received and the number
	//   of events built (in the current or previous run
	// - report on the number of incomplete events in the EventStore
	//   (if running)
	std::string tmpString;
	if (event_store_ptr_ != nullptr)
	{
		tmpString.append(app_name + " run number = " + std::to_string(event_store_ptr_->runID()) + ".\n");
	}
	tmpString.append("Command \"" + which + "\" is not currently supported.");
	return tmpString;
}
