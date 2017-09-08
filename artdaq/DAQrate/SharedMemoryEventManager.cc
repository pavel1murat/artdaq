#include "artdaq/DAQrate/SharedMemoryEventManager.hh"
#include "artdaq-core/Core/StatisticsCollection.hh"
#include "artdaq-core/Utilities/TraceLock.hh"
#include <iomanip>
#include <fstream>
#include <sys/wait.h>

artdaq::SharedMemoryEventManager::SharedMemoryEventManager(fhicl::ParameterSet pset, fhicl::ParameterSet art_pset)
	: SharedMemoryManager(pset.get<uint32_t>("shared_memory_key", seedAndRandom()),
						  pset.get<size_t>("buffer_count"),
						  pset.has_key("max_event_size_bytes") ? pset.get<size_t>("max_event_size_bytes") : pset.get<size_t>("expected_fragments_per_event") * pset.get<size_t>("max_fragment_size_bytes"),
						  pset.get<size_t>("stale_buffer_timeout_usec", 100 * 1000000),
						  !pset.get<bool>("broadcast_mode", false))
	, num_art_processes_(pset.get<size_t>("art_analyzer_count", 1))
	, num_fragments_per_event_(pset.get<size_t>("expected_fragments_per_event"))
	, queue_size_(pset.get<size_t>("buffer_count"))
	, run_id_(0)
	, subrun_id_(0)
	, update_run_ids_(pset.get<bool>("update_run_ids_on_new_fragment", true))
	, overwrite_mode_(!pset.get<bool>("use_art", true) || pset.get<bool>("overwrite_mode", false) || pset.get<bool>("broadcast_mode", false))
	, buffer_writes_pending_()
	, incomplete_event_report_interval_ms_(pset.get<int>("incomplete_event_report_interval_ms", -1))
	, last_incomplete_event_report_time_(std::chrono::steady_clock::now())
	, broadcast_timeout_ms_(pset.get<int>("fragment_broadcast_timeout_ms", 3000))
	, broadcast_count_(0)
	, subrun_event_count_(0)
	, config_file_name_(std::tmpnam(nullptr))
	, art_processes_()
	, restart_art_(false)
	, requests_(pset)
	, broadcasts_(pset.get<uint32_t>("broadcast_shared_memory_key", seedAndRandom()), pset.get<size_t>("broadcast_buffer_count", 10), pset.get<size_t>("broadcast_buffer_size", 0x100000), pset.get<int>("fragment_broadcast_timeout_ms", 3000) * 1000, false)
{
	TLOG_TRACE("SharedMemoryEventManager") << "BEGIN CONSTRUCTOR" << TLOG_ENDL;
	std::ofstream of(config_file_name_);

	if (pset.get<bool>("use_art", true) == false) num_art_processes_ = 0;
	configureArt_(art_pset);


	for (size_t ii = 0; ii < size(); ++ii)
	{
		buffer_writes_pending_[ii] = 0;
	}
	requests_.SendRoutingToken(size());

	if (!IsValid()) throw cet::exception("SharedMemoryEventManager") << "Unable to attach to Shared Memory!";

	TLOG_TRACE("SharedMemoryEventManager") << "Setting Writer rank to " << my_rank << TLOG_ENDL;
	SetRank(my_rank);
	TLOG_DEBUG("SharedMemoryEventManager") << "Writer Rank is " << GetRank() << TLOG_ENDL;


	TLOG_TRACE("SharedMemoryEventManager") << "END CONSTRUCTOR" << TLOG_ENDL;
}

artdaq::SharedMemoryEventManager::~SharedMemoryEventManager()
{
	TLOG_TRACE("SharedMemoryEventManager") << "DESTRUCTOR" << TLOG_ENDL;
	std::vector<int> ignored;
	endOfData(ignored);
	remove(config_file_name_.c_str());
	TLOG_TRACE("SharedMemoryEventManager") << "Destructor END" << TLOG_ENDL;
}

bool artdaq::SharedMemoryEventManager::AddFragment(detail::RawFragmentHeader frag, void* dataPtr, bool skipCheck)
{
	TLOG_TRACE("SharedMemoryEventManager") << "AddFragment(Header, ptr) BEGIN frag.word_count=" << std::to_string(frag.word_count)
		<< ", sequence_id=" << std::to_string(frag.sequence_id) << TLOG_ENDL;
	auto buffer = getBufferForSequenceID_(frag.sequence_id, true, frag.timestamp);
	TLOG_TRACE("SharedMemoryEventManager") << "Using buffer " << std::to_string(buffer) << TLOG_ENDL;
	if (buffer == -1) return false;

	auto hdr = getEventHeader_(buffer);
	if (update_run_ids_)
	{
		hdr->run_id = run_id_;
		hdr->subrun_id = subrun_id_;
	}

	TLOG_TRACE("SharedMemoryEventManager") << "AddFragment before Write calls" << TLOG_ENDL;
	Write(buffer, dataPtr, frag.word_count * sizeof(RawDataType));

	if (!skipCheck)
	{
		TLOG_TRACE("SharedMemoryEventManager") << "Checking for complete event" << TLOG_ENDL;
		auto fragmentCount = GetFragmentCount(frag.sequence_id);
		hdr->is_complete = fragmentCount == num_fragments_per_event_ && buffer_writes_pending_[buffer] == 0;
		TLOG_TRACE("SharedMemoryEventManager") << "hdr->is_complete=" << std::boolalpha << hdr->is_complete
			<< ", fragmentCount=" << std::to_string(fragmentCount)
			<< ", num_fragments_per_event=" << std::to_string(num_fragments_per_event_)
			<< ", buffer_writes_pending_[buffer]=" << std::to_string(buffer_writes_pending_[buffer]) << TLOG_ENDL;

		if (hdr->is_complete)
		{
			TLOG_DEBUG("SharedMemoryEventManager") << "AddFragment: This fragment completes event " << std::to_string(hdr->sequence_id) << ". Releasing to art" << TLOG_ENDL;
			MarkBufferFull(buffer);
			requests_.RemoveRequest(frag.sequence_id);
			requests_.SendRoutingToken(1);
			subrun_event_count_++;
		}
	}
	requests_.SendRequest(true);

	TLOG_TRACE("SharedMemoryEventManager") << "AddFragment END" << TLOG_ENDL;
	return true;
}

bool artdaq::SharedMemoryEventManager::AddFragment(FragmentPtr frag, int64_t timeout_usec, FragmentPtr& outfrag)
{
	TLOG_TRACE("SharedMemoryEventManager") << "AddFragment(FragmentPtr) BEGIN" << TLOG_ENDL;
	auto hdr = *reinterpret_cast<detail::RawFragmentHeader*>(frag->headerAddress());
	auto data = frag->headerAddress();
	auto start = std::chrono::steady_clock::now();
	bool sts = false;
	while (!sts && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() < timeout_usec)
	{
		sts = AddFragment(hdr, data);
		if (!sts) usleep(1000);
	}
	if (!sts)
	{
		outfrag = std::move(frag);
	}
	return sts;
}

artdaq::RawDataType* artdaq::SharedMemoryEventManager::WriteFragmentHeader(detail::RawFragmentHeader frag)
{
	TLOG_ARB(14, "SharedMemoryEventManager") << "WriteFragmentHeader BEGIN" << TLOG_ENDL;
	auto buffer = getBufferForSequenceID_(frag.sequence_id, true, frag.timestamp);

	if (buffer == -1) return nullptr;

	buffer_writes_pending_[buffer]++;
	TraceLock lk(buffer_mutexes_[buffer], 50, "WriteFragmentHeader");
	Write(buffer, &frag, frag.num_words() * sizeof(RawDataType));

	auto pos = reinterpret_cast<RawDataType*>(GetWritePos(buffer));
	IncrementWritePos(buffer, (frag.word_count - frag.num_words()) * sizeof(RawDataType));

	TLOG_ARB(14, "SharedMemoryEventManager") << "WriteFragmentHeader END" << TLOG_ENDL;
	return pos;

}

void artdaq::SharedMemoryEventManager::DoneWritingFragment(detail::RawFragmentHeader frag)
{
	TLOG_TRACE("SharedMemoryEventManager") << "DoneWritingFragment BEGIN" << TLOG_ENDL;
	auto buffer = getBufferForSequenceID_(frag.sequence_id, false, frag.timestamp);
	if (buffer == -1) Detach(true, "SharedMemoryEventManager", "getBufferForSequenceID_ returned -1 when it REALLY shouldn't have! Check program logic!");
	buffer_writes_pending_[buffer]--;
	auto hdr = getEventHeader_(buffer);
	if (update_run_ids_)
	{
		hdr->run_id = run_id_;
		hdr->subrun_id = subrun_id_;
	}
	hdr->is_complete = GetFragmentCount(frag.sequence_id) == num_fragments_per_event_ && buffer_writes_pending_[buffer] == 0;


	if (hdr->is_complete)
	{
		TLOG_DEBUG("SharedMemoryEventManager") << "DoneWritingFragment: This fragment completes event " << std::to_string(hdr->sequence_id) << ". Releasing to art" << TLOG_ENDL;
		MarkBufferFull(buffer);
		requests_.RemoveRequest(frag.sequence_id);
		requests_.SendRoutingToken(1);
		subrun_event_count_++;
		if (metricMan)
		{
			auto full = ReadReadyCount();
			auto empty = WriteReadyCount(overwrite_mode_);
			auto total = size();
			metricMan->sendMetric("Shared Memory Full Buffers", full, "buffers", 2);
			metricMan->sendMetric("Shared Memory Available Buffers", empty, "buffers", 2);
			metricMan->sendMetric("Shared Memory Full %", full * 100 / static_cast<double>(total), "%", 2);
			metricMan->sendMetric("Shared Memory Available %", empty * 100 / static_cast<double>(total), "%", 2);
		}
	}
	requests_.SendRequest(true);
	TLOG_TRACE("SharedMemoryEventManager") << "DoneWritingFragment END" << TLOG_ENDL;
}

//bool artdaq::SharedMemoryEventManager::CheckSpace(detail::RawFragmentHeader frag)
//{
//	if (ReadyForWrite(false)) return true;
//
//	auto buffer = getBufferForSequenceID_(frag.sequence_id, frag.timestamp);
//
//	return buffer != -1;
//}

size_t artdaq::SharedMemoryEventManager::GetOpenEventCount()
{
	return GetBuffersOwnedByManager().size();
}

size_t artdaq::SharedMemoryEventManager::GetFragmentCount(Fragment::sequence_id_t seqID, Fragment::type_t type)
{
	auto buffer = getBufferForSequenceID_(seqID, false);
	if (buffer == -1) return 0;
	ResetReadPos(buffer);
	IncrementReadPos(buffer, sizeof(detail::RawEventHeader));

	size_t count = 0;

	while (MoreDataInBuffer(buffer))
	{
		auto fragHdr = reinterpret_cast<artdaq::detail::RawFragmentHeader*>(GetReadPos(buffer));
		IncrementReadPos(buffer, fragHdr->word_count * sizeof(RawDataType));
		if (type != Fragment::InvalidFragmentType && fragHdr->type != type) continue;
		TLOG_TRACE("GetFragmentCount") << "Adding Fragment with size=" << std::to_string(fragHdr->word_count) << " to Fragment count" << TLOG_ENDL;
		++count;
	}

	return count;
}

void perror_exit(const char *msg, ...)
{
	char buf[1024];
	va_list ap; va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);
	va_end(ap);
	TRACE(0, buf);
	perror(buf); exit(1);
}

/* fd is in/out
	if fd[x]=-1 then dup that index (stdin/out/err) otherwise inherent from parent */
pid_t fork_execv(const char *cmd, char* const *argv)
{
	pid_t pid = fork();
	if (pid == 0)
	{ /* child */
		execvp(cmd, argv);
		exit(1);
	}
	TRACE(2, "fork_execl pid=%d", pid);
	return pid;
}

void artdaq::SharedMemoryEventManager::RunArt()
{
	while (restart_art_)
	{
		send_init_frag_();
		TLOG_INFO("SharedMemoryEventManager") << "Starting art process with config file " << config_file_name_ << TLOG_ENDL;
		std::vector<char*> args{ (char*)"art", (char*)"-c", &config_file_name_[0], NULL };

		pid_t pid = fork_execv("art", &args[0]);
		art_process_pids_.insert(pid);
		int status;
		waitpid(pid, &status, 0);
		art_process_pids_.erase(pid);
		art_process_return_codes_.push_back(status);
		if (status == 0)
		{
			TLOG_DEBUG("SharedMemoryEventManager") << "art process " << pid << " exited normally, " << (restart_art_ ? "restarting" : "not restarting") << TLOG_ENDL;
		}
		else
		{
			TLOG_WARNING("SharedMemoryEventManager") << "art process " << pid << " exited with status code " << status << ", " << (restart_art_ ? "restarting" : "not restarting") << TLOG_ENDL;
		}

		//art_process_return_codes_.push_back(system(("art -c " + config_file_name_).c_str()));
	}
}

void artdaq::SharedMemoryEventManager::StartArt()
{
	restart_art_ = true;
	auto initialCount = GetAttachedCount();
	auto startTime = std::chrono::steady_clock::now();
	for (size_t ii = 0; ii < num_art_processes_; ++ii)
	{
		art_processes_.emplace_back([=] {RunArt(); });
	}

	while (static_cast<uint16_t>(GetAttachedCount() - initialCount) < num_art_processes_ &&
		   std::chrono::duration_cast<TimeUtils::seconds>(std::chrono::steady_clock::now() - startTime).count() < 5)
	{
		usleep(1000);
	}
	if (static_cast<uint16_t>(GetAttachedCount() - initialCount) < num_art_processes_)
	{
		TLOG_WARNING("SharedMemoryEventManager") << std::to_string(GetAttachedCount() - initialCount - num_art_processes_)
			<< " art processes have not started after 5s. Check art configuration!" << TLOG_ENDL;
	}
	else
	{
		TLOG_INFO("SharedMemoryEventManager") << std::setw(4) << std::fixed << "art initialization took "
			<< std::chrono::duration_cast<TimeUtils::seconds>(std::chrono::steady_clock::now() - startTime).count() << " seconds." << TLOG_ENDL;
	}

}

pid_t artdaq::SharedMemoryEventManager::StartArtProcess(fhicl::ParameterSet pset)
{
	restart_art_ = true;
	auto initialCount = GetAttachedCount();
	auto startTime = std::chrono::steady_clock::now();

	configureArt_(pset);
	auto oldPids = art_process_pids_;
	art_processes_.emplace_back([=] {RunArt(); });

	while (static_cast<uint16_t>(GetAttachedCount() - initialCount) < 1 &&
		   std::chrono::duration_cast<TimeUtils::seconds>(std::chrono::steady_clock::now() - startTime).count() < 5)
	{
		usleep(1000);
	}
	if (static_cast<uint16_t>(GetAttachedCount() - initialCount) < 1)
	{
		TLOG_WARNING("SharedMemoryEventManager") << std::to_string(GetAttachedCount() - initialCount - num_art_processes_)
			<< " art process has not started after 5s. Check art configuration!" << TLOG_ENDL;
		return 0;
	}
	else
	{
		TLOG_INFO("SharedMemoryEventManager") << std::setw(4) << std::fixed << "art initialization took "
			<< std::chrono::duration_cast<TimeUtils::seconds>(std::chrono::steady_clock::now() - startTime).count() << " seconds." << TLOG_ENDL;

		auto afterPids = art_process_pids_;
		std::set<pid_t> newPids;
		for (auto pid : afterPids)
		{
			if (oldPids.count(pid) == 0) { newPids.insert(pid); }
		}
		if (newPids.size() == 0) return 0;
		return *newPids.begin();
	}

}

void artdaq::SharedMemoryEventManager::ReconfigureArt(fhicl::ParameterSet art_pset, run_id_t newRun, int n_art_processes)
{
	TLOG_DEBUG("SharedMemoryEventManager") << "ReconfigureArt BEGIN" << TLOG_ENDL;
	if (restart_art_) // Art is running
	{
		std::vector<int> ignored;
		endOfData(ignored);
	}
	for (size_t ii = 0; ii < broadcasts_.size(); ++ii)
	{
		broadcasts_.MarkBufferEmpty(ii, true);
	}
	if (newRun == 0) newRun = run_id_ + 1;
	std::ofstream of(config_file_name_, std::ofstream::trunc);
	configureArt_(art_pset);
	if (n_art_processes != -1)
	{
		TLOG_INFO("SharedMemoryEventManager") << "Setting number of art processes to " << n_art_processes << TLOG_ENDL;
		num_art_processes_ = n_art_processes;
	}
	startRun(newRun);
	TLOG_DEBUG("SharedMemoryEventManager") << "ReconfigureArt END" << TLOG_ENDL;
}

bool artdaq::SharedMemoryEventManager::endOfData(std::vector<int>& readerReturnValues)
{
	init_fragment_.reset(nullptr);
	TLOG_TRACE("SharedMemoryEventManager") << "SharedMemoryEventManager::endOfData" << TLOG_ENDL;
	restart_art_ = false;

	size_t initialStoreSize = GetOpenEventCount();
	TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Flushing " << initialStoreSize
		<< " stale events from the SharedMemoryEventManager." << TLOG_ENDL;
	auto buffers = GetBuffersOwnedByManager();
	for (auto& buf : buffers)
	{
		MarkBufferFull(buf);
	}
	TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Done flushing, there are now " << GetOpenEventCount()
		<< " stale events in the SharedMemoryEventManager." << TLOG_ENDL;


	TLOG_TRACE("SharedMemoryEventManager") << "Waiting for " << std::to_string(ReadReadyCount() + (size() - WriteReadyCount(overwrite_mode_))) << " outstanding buffers..." << TLOG_ENDL;
	auto start = std::chrono::steady_clock::now();
	auto lastReadCount = ReadReadyCount() + (size() - WriteReadyCount(overwrite_mode_));

	// We will wait until no buffer has been read for 1 second.
	while (lastReadCount > 0 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 1000)
	{
		auto temp = ReadReadyCount() + (size() - WriteReadyCount(overwrite_mode_));
		if (temp != lastReadCount)
		{
			TLOG_TRACE("SharedMemoryEventManager") << "Waiting for " << std::to_string(temp) << " outstanding buffers..." << TLOG_ENDL;
			lastReadCount = temp;
			start = std::chrono::steady_clock::now();
		}
		if (lastReadCount > 0) usleep(1000);
	}

	TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Broadcasting EndOfData Fragment" << TLOG_ENDL;
	FragmentPtr outFrag = std::move(Fragment::eodFrag(GetBufferCount()));
	bool success = broadcastFragment_(std::move(outFrag), outFrag);
	if (!success)
	{
		TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Clearing buffers to make room for EndOfData Fragment" << TLOG_ENDL;
		for (size_t ii = 0; ii < size(); ++ii)
		{
			broadcasts_.MarkBufferEmpty(ii, true);
		}
		broadcastFragment_(std::move(outFrag), outFrag);
	}
	TLOG_TRACE("SharedMemoryEventManager") << "Sleeping for 50 ms to allow art processes time to shut down" << TLOG_ENDL;
	usleep(50000);

	for (auto& pid : art_process_pids_)
	{
		kill(pid, SIGQUIT);
	}

	TLOG_TRACE("SharedMemoryEventManager") << "Sleeping for 50 ms to allow art processes time to shut down" << TLOG_ENDL;
	usleep(50000);

	for (auto& pid : art_process_pids_)
	{
		kill(pid, SIGINT);
	}

	while (art_process_pids_.size() > 0)
	{
		kill(*art_process_pids_.begin(), SIGKILL);
		usleep(1000);
	}
	TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Getting return codes from art processes" << TLOG_ENDL;

	for (auto& proc : art_processes_)
	{
		if (proc.joinable()) proc.join();
	}
	readerReturnValues = art_process_return_codes_;
	if (readerReturnValues.size() == 0) readerReturnValues.push_back(0);

	ResetAttachedCount();

	TLOG_TRACE("SharedMemoryEventManager") << "endOfData: Clearing buffers" << TLOG_ENDL;
	for (size_t ii = 0; ii < size(); ++ii)
	{
		MarkBufferEmpty(ii, true);
	}

	TLOG_TRACE("SharedMemoryEventManager") << "endOfData END" << TLOG_ENDL;
	TLOG_INFO("SharedMemoryEventManager") << "EndOfData Complete. There were " << GetLastSeenBufferID() << " events sent to art" << TLOG_ENDL;
	return true;
}

void artdaq::SharedMemoryEventManager::startRun(run_id_t runID)
{
	init_fragment_.reset(nullptr);
	StartArt();
	run_id_ = runID;
	subrun_id_ = 1;
	requests_.SendRoutingToken(queue_size_);
	TLOG_DEBUG("SharedMemoryEventManager") << "Starting run " << run_id_
		<< ", max queue size = "
		<< queue_size_
		<< ", queue size = "
		<< GetOpenEventCount() << TLOG_ENDL;
	if (metricMan)
	{
		double runSubrun = run_id_ + ((double)subrun_id_ / 10000);
		metricMan->sendMetric("Run Number", runSubrun, "Run:Subrun", 1, false);
	}
}

void artdaq::SharedMemoryEventManager::startSubrun()
{
	++subrun_id_;
	if (metricMan)
	{
		double runSubrun = run_id_ + ((double)subrun_id_ / 10000);
		metricMan->sendMetric("Run Number", runSubrun, "Run:Subrun", 1, false);
	}
}

bool artdaq::SharedMemoryEventManager::endRun()
{
	FragmentPtr	endOfRunFrag(new
							 Fragment(static_cast<size_t>
							 (ceil(sizeof(my_rank) /
								   static_cast<double>(sizeof(Fragment::value_type))))));

	endOfRunFrag->setSystemType(Fragment::EndOfRunFragmentType);
	*endOfRunFrag->dataBegin() = my_rank;
	broadcastFragment_(std::move(endOfRunFrag), endOfRunFrag);

	return true;
}

bool artdaq::SharedMemoryEventManager::endSubrun()
{
	std::unique_ptr<artdaq::Fragment>
		endOfSubrunFrag(new
						Fragment(static_cast<size_t>
						(ceil(sizeof(my_rank) /
							  static_cast<double>(sizeof(Fragment::value_type))))));

	endOfSubrunFrag->setSystemType(Fragment::EndOfSubrunFragmentType);
	*endOfSubrunFrag->dataBegin() = my_rank;

	broadcastFragment_(std::move(endOfSubrunFrag), endOfSubrunFrag);

	TLOG_INFO("SharedMemoryEventManager") << "Subrun " << subrun_id_ << " in run " << run_id_ << " has ended. There were " << subrun_event_count_ << " events in this subrun." << TLOG_ENDL;
	subrun_event_count_ = 0;

	return true;
}

void artdaq::SharedMemoryEventManager::sendMetrics()
{
	auto events = GetBuffersOwnedByManager();
	if (metricMan)
	{
		metricMan->sendMetric("Incomplete Event Count", events.size(), "events", 1);
	}
	if (incomplete_event_report_interval_ms_ > 0 && GetOpenEventCount())
	{
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_incomplete_event_report_time_).count() < incomplete_event_report_interval_ms_) return;
		last_incomplete_event_report_time_ = std::chrono::steady_clock::now();
		std::ostringstream oss;
		oss << "Incomplete Events (" << num_fragments_per_event_ << "): ";
		for (auto& ev : events)
		{
			auto hdr = getEventHeader_(ev);
			oss << hdr->sequence_id << " (" << GetFragmentCount(hdr->sequence_id) << "), ";
		}
		TLOG_DEBUG("SharedMemoryEventManager") << oss.str() << TLOG_ENDL;
	}
}

bool artdaq::SharedMemoryEventManager::broadcastFragment_(FragmentPtr frag, FragmentPtr& outFrag)
{
	auto buffer = broadcasts_.GetBufferForWriting(false);
	auto start_time = std::chrono::steady_clock::now();
	while (buffer == -1 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() < broadcast_timeout_ms_)
	{
		usleep(10000);
		buffer = broadcasts_.GetBufferForWriting(false);
	}
	if (buffer == -1)
	{
		TLOG_ERROR("SharedMemoryEventManager") << "Broadcast of fragment type " << frag->typeString() << " failed due to timeout waiting for buffer!" << TLOG_ENDL;
		outFrag.swap(frag);
		return false;
	}

	auto hdr = reinterpret_cast<detail::RawEventHeader*>(broadcasts_.GetBufferStart(buffer));
	hdr->run_id = run_id_;
	hdr->subrun_id = subrun_id_;
	hdr->sequence_id = frag->sequenceID();
	hdr->is_complete = true;
	broadcasts_.IncrementWritePos(buffer, sizeof(detail::RawEventHeader));

	TLOG_TRACE("SharedMemoryEventManager") << "broadcastFragment_ before Write calls" << TLOG_ENDL;
	broadcasts_.Write(buffer, frag->headerAddress(), frag->size() * sizeof(RawDataType));

	broadcasts_.MarkBufferFull(buffer, -1);
	outFrag.swap(frag);
	return true;
}

artdaq::detail::RawEventHeader* artdaq::SharedMemoryEventManager::getEventHeader_(int buffer)
{
	return reinterpret_cast<detail::RawEventHeader*>(GetBufferStart(buffer));
}

int artdaq::SharedMemoryEventManager::getBufferForSequenceID_(Fragment::sequence_id_t seqID, bool create_new, Fragment::timestamp_t timestamp)
{
	std::unique_lock<std::mutex> lk(sequence_id_mutex_);
	TLOG_ARB(14, "SharedMemoryEventManager") << "getBufferForSequenceID " << std::to_string(seqID) << " BEGIN" << TLOG_ENDL;
	auto buffers = GetBuffersOwnedByManager();
	for (auto& buf : buffers)
	{
		auto hdr = getEventHeader_(buf);
		if (hdr->sequence_id == seqID)
		{
			TLOG_ARB(14, "SharedMemoryEventManager") << "getBufferForSequenceID " << std::to_string(seqID) << " returning " << buf << TLOG_ENDL;
			return buf;
		}
	}
	if (!create_new) return -1;
	auto new_buffer = GetBufferForWriting(overwrite_mode_);
	if (new_buffer == -1) return -1;
	TraceLock(buffer_mutexes_[new_buffer], 34, "getBufferForSequenceID");
	auto hdr = getEventHeader_(new_buffer);
	hdr->is_complete = false;
	hdr->run_id = run_id_;
	hdr->subrun_id = subrun_id_;
	hdr->sequence_id = seqID;
	buffer_writes_pending_[new_buffer] = 0;
	if (timestamp != Fragment::InvalidTimestamp)
	{
		requests_.AddRequest(seqID, timestamp);
	}
	requests_.SendRequest();
	IncrementWritePos(new_buffer, sizeof(detail::RawEventHeader));
	TLOG_ARB(14, "SharedMemoryEventManager") << "getBufferForSequenceID " << std::to_string(seqID) << " returning newly initialized buffer " << new_buffer << TLOG_ENDL;
	return new_buffer;
}

void artdaq::SharedMemoryEventManager::configureArt_(fhicl::ParameterSet art_pset)
{
	std::ofstream of(config_file_name_, std::ofstream::trunc);
	of << art_pset.to_string();

	if (art_pset.has_key("services.NetMonTransportServiceInterface"))
	{
		of << " services.NetMonTransportServiceInterface.shared_memory_key: 0x" << std::hex << GetKey();
		of << " services.NetMonTransportServiceInterface.broadcast_shared_memory_key: 0x" << std::hex << GetBroadcastKey();
	}
	if (!art_pset.has_key("services.message"))
	{
		of << " services.message: { " << generateMessageFacilityConfiguration("art") << "} ";
	}
	of << " source.shared_memory_key: 0x" << std::hex << GetKey();
	of << " source.broadcast_shared_memory_key: 0x" << std::hex << GetBroadcastKey();
	of.close();
}

void artdaq::SharedMemoryEventManager::send_init_frag_()
{
	if (init_fragment_ != nullptr && TimeUtils::gettimeofday_us() > last_init_time_ + 10 * GetBufferTimeout())
	{
		TLOG_TRACE("SharedMemoryEventManager") << "Sending init Fragment to art..." << TLOG_ENDL;

#if 1
		std::fstream ostream("receiveInitMessage.bin", std::ios::out | std::ios::binary);
		ostream.write(reinterpret_cast<char*>(init_fragment_->dataBeginBytes()), init_fragment_->dataSizeBytes());
		ostream.close();
#endif

		broadcastFragment_(std::move(init_fragment_), init_fragment_);
		last_init_time_ = TimeUtils::gettimeofday_us();
		TLOG_TRACE("SharedMemoryEventManager") << "Init Fragment sent" << TLOG_ENDL;
	}
}

void artdaq::SharedMemoryEventManager::SetInitFragment(FragmentPtr frag)
{
	if (!init_fragment_ || init_fragment_ == nullptr)
	{
		init_fragment_.swap(frag);
		send_init_frag_();
	}
}
