#ifndef ARTDAQ_DAQRATE_SHAREDMEMORYEVENTMANAGER_HH
#define ARTDAQ_DAQRATE_SHAREDMEMORYEVENTMANAGER_HH

#include "TRACE/tracemf.h"  // Pre-empt TRACE/trace.h from Fragment.hh.
#include "artdaq-core/Data/Fragment.hh"

#include "artdaq-core/Core/SharedMemoryManager.hh"
#include "artdaq-core/Data/RawEvent.hh"
#include "artdaq-core/Utilities/configureMessageFacility.hh"
#include "artdaq/DAQrate/StatisticsHelper.hh"
#include "artdaq/DAQrate/detail/RequestSender.hh"
#include "artdaq/DAQrate/detail/TokenSender.hh"

#include "fhiclcpp/types/Atom.h"
#include "fhiclcpp/types/Comment.h"
#include "fhiclcpp/types/ConfigurationTable.h"
#include "fhiclcpp/types/OptionalTable.h"
#include "fhiclcpp/types/TableFragment.h"

#define ART_SUPPORTS_DUPLICATE_EVENTS 0

#include <sys/stat.h>
#include <deque>
#include <fstream>
#include <iomanip>
#include <set>

namespace artdaq {

/**
 * \brief art_config_file wraps a temporary file used to configure art
 */
class art_config_file
{
public:
	/**
	 * \brief art_config_file Constructor
	 * \param ps ParameterSet to write to temporary file
	 * \param shm_key Shared Memory key to use (if 0, child program will use parent PID to generate)
	 * \param broadcast_key Shared Memory key to use for broadcasts (if 0, child program will use parent PID to generate)
	 */
	explicit art_config_file(fhicl::ParameterSet ps, uint32_t shm_key = 0, uint32_t broadcast_key = 0)
	    : dir_name_("/tmp/partition_" + std::to_string(GetPartitionNumber()))
	    , file_name_(dir_name_ + "/artConfig_" + std::to_string(my_rank) + "_" + std::to_string(artdaq::TimeUtils::gettimeofday_us()) + ".fcl")
	{
		mkdir(dir_name_.c_str(), 0777);  // Allowed to fail if directory already exists

		std::ofstream of(file_name_, std::ofstream::trunc);
		if (of.fail())
		{
			// Probably a permissions error...
			dir_name_ = "/tmp/partition_" + std::to_string(GetPartitionNumber()) + "_" + std::to_string(getuid());
			mkdir(dir_name_.c_str(), 0777);  // Allowed to fail if directory already exists
			file_name_ = dir_name_ + "/artConfig_" + std::to_string(my_rank) + "_" + std::to_string(artdaq::TimeUtils::gettimeofday_us()) + ".fcl";

			of.open(file_name_, std::ofstream::trunc);
			if (of.fail())
			{
				TLOG(TLVL_ERROR, "ArtConfigFile") << "Failed to open configuration file after two attemps! ABORTING!";
				exit(46);
			}
		}
		of << ps.to_string();

		if (!ps.has_key("services") || !ps.has_key("services.message"))
		{
			of << " services.message: { " << generateMessageFacilityConfiguration(mf::GetApplicationName().c_str(), true, false, "-art") << "} ";
		}

		TLOG(TLVL_INFO, "ArtConfigFile") << "Inserting Shared memory keys (0x" << std::hex << shm_key << ", 0x" << std::hex << broadcast_key << ") into source config";
		if (shm_key > 0) of << " source.shared_memory_key: 0x" << std::hex << shm_key;
		if (broadcast_key > 0) of << " source.broadcast_shared_memory_key: 0x" << std::hex << broadcast_key;

		of.flush();
		of.close();
	}
	~art_config_file()
	{
		remove(file_name_.c_str());
		rmdir(dir_name_.c_str());  // Will only delete directory if no config files are left over
	}
	/**
	 * \brief Get the path of the temporary file
	 * \return The path of the temporary file
	 */
	std::string getFileName() const { return file_name_; }

private:
	art_config_file(art_config_file const&) = delete;
	art_config_file(art_config_file&&) = delete;
	art_config_file& operator=(art_config_file const&) = delete;
	art_config_file& operator=(art_config_file&&) = delete;

	std::string dir_name_;
	std::string file_name_;
};

/**
 * \brief The SharedMemoryEventManager is a SharedMemoryManger which tracks events as they are built
 */
class SharedMemoryEventManager : public SharedMemoryManager
{
public:
	static const std::string FRAGMENTS_RECEIVED_STAT_KEY;  ///< Key for Fragments Received MonitoredQuantity
	static const std::string EVENTS_RELEASED_STAT_KEY;     ///< Key for the Events Released MonitoredQuantity

	typedef RawEvent::run_id_t run_id_t;                     ///< Copy RawEvent::run_id_t into local scope
	typedef RawEvent::subrun_id_t subrun_id_t;               ///< Copy RawEvent::subrun_id_t into local scope
	typedef Fragment::sequence_id_t sequence_id_t;           ///< Copy Fragment::sequence_id_t into local scope
	typedef std::map<sequence_id_t, RawEvent_ptr> EventMap;  ///< An EventMap is a map of RawEvent_ptr objects, keyed by sequence ID

	/// <summary>
	/// Configuration of the SharedMemoryEventManager. May be used for parameter validation
	/// </summary>
	struct Config
	{
		/// "max_event_size_bytes" REQUIRED: Maximum event size(all Fragments), in bytes
		/// Either max_fragment_size_bytes or max_event_size_bytes must be specified
		fhicl::Atom<size_t> max_event_size_bytes{fhicl::Name{"max_event_size_bytes"}, fhicl::Comment{"Maximum event size (all Fragments), in bytes"}};
		/// "stale_buffer_timeout_usec" (Default: event_queue_wait_time * 1, 000, 000) : Maximum amount of time elapsed before a buffer is marked as abandoned.Time is reset each time an operation is performed on the buffer.
		fhicl::Atom<size_t> stale_buffer_timeout_usec{fhicl::Name{"stale_buffer_timeout_usec"}, fhicl::Comment{"Maximum amount of time elapsed before a buffer is marked as abandoned. Time is reset each time an operation is performed on the buffer."}, 5000000};
		/// "overwite_mode" (Default: false): Whether new data is allowed to overwrite buffers in the "Full" state
		fhicl::Atom<bool> overwrite_mode{fhicl::Name{"overwrite_mode"}, fhicl::Comment{"Whether buffers are allowed to be overwritten when safe (state == Full or Reading)"}, false};
		/// "restart_crashed_art_processes" (Default: true) : Whether to automatically restart art processes that fail for any reason
		fhicl::Atom<bool> restart_crashed_art_processes{fhicl::Name{"restart_crashed_art_processes"}, fhicl::Comment{"Whether to automatically restart art processes that fail for any reason"}, true};
		/// "shared_memory_key" (Default 0xBEE70000 + PID) : Key to use for shared memory access
		fhicl::Atom<uint32_t> shared_memory_key{fhicl::Name{"shared_memory_key"}, fhicl::Comment{"Key to use for shared memory access"}, 0xBEE70000 + getpid()};
		/// "buffer_count" REQUIRED: Number of events in the Shared Memory(incomplete + pending art)
		fhicl::Atom<size_t> buffer_count{fhicl::Name{"buffer_count"}, fhicl::Comment{"Number of events in the Shared Memory (incomplete + pending art)"}};
		/// "max_fragment_size_bytes" REQURIED: Maximum Fragment size, in bytes
		/// Either max_fragment_size_bytes or max_event_size_bytes must be specified
		fhicl::Atom<size_t> max_fragment_size_bytes{fhicl::Name{"max_fragment_size_bytes"}, fhicl::Comment{" Maximum Fragment size, in bytes"}};
		/// "event_queue_wait_time" (Default: 5) : Amount of time(in seconds) an event can exist in shared memory before being released to art.Used as input to default parameter of "stale_buffer_timeout_usec".
		fhicl::Atom<size_t> event_queue_wait_time{fhicl::Name{"event_queue_wait_time"}, fhicl::Comment{"Amount of time (in seconds) an event can exist in shared memory before being released to art. Used as input to default parameter of \"stale_buffer_timeout_usec\"."}, 5};
		/// "broadcast_mode" (Default: false) : When true, buffers are not marked Empty when read, but return to Full state.Buffers are overwritten in order received.
		fhicl::Atom<bool> broadcast_mode{fhicl::Name{"broadcast_mode"}, fhicl::Comment{"When true, buffers are not marked Empty when read, but return to Full state. Buffers are overwritten in order received."}, false};
		/// "art_analyzer_count" (Default: 1) : Number of art procceses to start
		fhicl::Atom<size_t> art_analyzer_count{fhicl::Name{"art_analyzer_count"}, fhicl::Comment{"Number of art procceses to start"}, 1};
		/// "expected_fragments_per_event" (REQUIRED) : Number of Fragments to expect per event
		fhicl::Atom<size_t> expected_fragments_per_event{fhicl::Name{"expected_fragments_per_event"}, fhicl::Comment{"Number of Fragments to expect per event"}};
		/// "maximum_oversize_fragment_count" (Default: 1): Maximum number of over-size Fragments to drop before throwing an exception. Default is 1, which means to throw an exception if any over-size Fragments are dropped. Set to 0 to disable.
		fhicl::Atom<int> maximum_oversize_fragment_count{fhicl::Name{"maximum_oversize_fragment_count"}, fhicl::Comment{"Maximum number of over-size Fragments to drop before throwing an exception.  Default is 1, which means to throw an exception if any over-size Fragments are dropped. Set to 0 to disable."}, 1};
		/// "update_run_ids_on_new_fragment" (Default: true) : Whether the run and subrun ID of an event should be updated whenever a Fragment is added.
		fhicl::Atom<bool> update_run_ids_on_new_fragment{fhicl::Name{"update_run_ids_on_new_fragment"}, fhicl::Comment{"Whether the run and subrun ID of an event should be updated whenever a Fragment is added."}, true};
		/// "use_sequence_id_for_event_number" (Default: true): Whether to use the artdaq Sequence ID (true) or the Timestamp (false) for art Event numbers
		fhicl::Atom<bool> use_sequence_id_for_event_number{fhicl::Name{"use_sequence_id_for_event_number"}, fhicl::Comment{"Whether to use the artdaq Sequence ID (true) or the Timestamp (false) for art Event numbers"}, true};
		/// "max_subrun_lookup_table_size" (Default: 100): The maximum number of entries to store in the sequence ID-SubRun ID lookup table
		fhicl::Atom<size_t> max_subrun_lookup_table_size{fhicl::Name{"max_subrun_lookup_table_size"}, fhicl::Comment{"The maximum number of entries to store in the sequence ID-SubRun ID lookup table"}, 100};
		/// "max_event_list_length" (Default: 100): The maximum number of entries to store in the released events list
		fhicl::Atom<size_t> max_event_list_length{fhicl::Name{"max_event_list_length"}, fhicl::Comment{" The maximum number of entries to store in the released events list"}, 100};
		/// "send_init_fragments" (Default: true): Whether Init Fragments are expected to be sent to art. If true, a Warning message is printed when an Init Fragment is requested but none are available.
		fhicl::Atom<bool> send_init_fragments{fhicl::Name{"send_init_fragments"}, fhicl::Comment{"Whether Init Fragments are expected to be sent to art. If true, a Warning message is printed when an Init Fragment is requested but none are available."}, true};
		/// "open_event_report_interval_ms" (Default: -1): Interval at which an open event report should be written
		fhicl::Atom<int> open_event_report_interval_ms{fhicl::Name{"open_event_report_interval_ms"}, fhicl::Comment{"Interval at which an open event report should be written"}, -1};
		/// "fragment_broadcast_timeout_ms" (Default: 3000): Amount of time broadcast fragments should live in the broadcast shared memory segment
		/// A "Broadcast shared memory segment" is used for all system-level fragments, such as Init, Start/End Run, Start/End Subrun and EndOfData
		fhicl::Atom<int> fragment_broadcast_timeout_ms{fhicl::Name{"fragment_broadcast_timeout_ms"}, fhicl::Comment{"Amount of time broadcast fragments should live in the broadcast shared memory segment"}, 3000};
		/// "art_command_line"  (Default: "art -c \#CONFIG_FILE\#"): Command line used to start analysis processes. Supports two special sequences: \#CONFIG_FILE\# will be replaced with the fhicl config file. \#PROCESS_INDEX\# will be replaced by the index of the art process.
		fhicl::Atom<std::string> art_command_line{fhicl::Name{"art_command_line"}, fhicl::Comment{"Command line used to start analysis processes. Supports two special sequences: #CONFIG_FILE# will be replaced with the fhicl config file. #PROCESS_INDEX# will be replaced by the index of the art process."}, "art -c #CONFIG_FILE#"};
		/// "art_index_offset" (Default: 0): Offset to add to art process index when replacing \#PROCESS_INDEX\#
		fhicl::Atom<size_t> art_index_offset{fhicl::Name{"art_index_offset"}, fhicl::Comment{"Offset to add to art process index when replacing #PROCESS_INDEX#"}, 0};
		/// "minimum_art_lifetime_s" (Default: 2 seconds): Amount of time that an art process should run to not be considered "DOA"
		fhicl::Atom<double> minimum_art_lifetime_s{fhicl::Name{"minimum_art_lifetime_s"}, fhicl::Comment{"Amount of time that an art process should run to not be considered \"DOA\""}, 2.0};
		/// "expected_art_event_processing_time_us" (Default: 100000 us): During shutdown, SMEM will wait for this amount of time while it is checking that the art threads are done reading buffers.
		///																 (TUNING: Should be slightly longer than the mean art processing time, but not so long that the Stop transition times out)
		fhicl::Atom<size_t> expected_art_event_processing_time_us{fhicl::Name{"expected_art_event_processing_time_us"}, fhicl::Comment{"During shutdown, SMEM will wait for this amount of time while it is checking that the art threads are done reading buffers."}, 100000};
		/// "broadcast_shared_memory_key" (Default: 0xCEE7000 + PID): Key to use for broadcast shared memory access
		fhicl::Atom<uint32_t> broadcast_shared_memory_key{fhicl::Name{"broadcast_shared_memory_key"}, fhicl::Comment{""}, 0xCEE70000 + getpid()};
		/// "broadcast_buffer_count" (Default: 10): Buffers in the broadcast shared memory segment
		fhicl::Atom<size_t> broadcast_buffer_count{fhicl::Name{"broadcast_buffer_count"}, fhicl::Comment{"Buffers in the broadcast shared memory segment"}, 10};
		/// "broadcast_buffer_size" (Default: 0x100000): Size of the buffers in the broadcast shared memory segment
		fhicl::Atom<size_t> broadcast_buffer_size{fhicl::Name{"broadcast_buffer_size"}, fhicl::Comment{"Size of the buffers in the broadcast shared memory segment"}, 0x100000};
		/// "use_art" (Default: true): Whether to start and manage art threads (Sets art_analyzer count to 0 and overwrite_mode to true when false)
		fhicl::Atom<bool> use_art{fhicl::Name{"use_art"}, fhicl::Comment{"Whether to start and manage art threads (Sets art_analyzer count to 0 and overwrite_mode to true when false)"}, true};
		/// "manual_art" (Default: false): Prints the startup command line for the art process so that the user may (for example) run it in GDB or valgrind
		fhicl::Atom<bool> manual_art{fhicl::Name{"manual_art"}, fhicl::Comment{"Prints the startup command line for the art process so that the user may (for example) run it in GDB or valgrind"}, false};
		/// Configuration of the RequestSender. See artdaq::RequestSender::Config
		fhicl::TableFragment<artdaq::RequestSender::Config> requestSenderConfig;
		/// Configuration of the TokenSender. See artdaq::TokenSender::Config
		fhicl::OptionalTable<artdaq::TokenSender::Config> tokenSenderConfig{fhicl::Name{"routing_token_config"}, fhicl::Comment{"Configuration for the Routing TokenSender"}};
	};
	/// Used for ParameterSet validation (if desired)
	using Parameters = fhicl::WrappedTable<Config>;

	/**
	 * \brief SharedMemoryEventManager Constructor
	 * \param pset ParameterSet used to configure SharedMemoryEventManager. See artdaq::SharedMemoryEventManager::Config for description of parameters
	 * \param art_pset ParameterSet used to configure art. See art::Config for description of expected document format
	 */
	SharedMemoryEventManager(const fhicl::ParameterSet& pset, fhicl::ParameterSet art_pset);
	/**
	 * \brief SharedMemoryEventManager Destructor
	 */
	virtual ~SharedMemoryEventManager();

private:
	/**
	 * \brief Add a Fragment to the SharedMemoryEventManager
	 * \param frag Header of the Fragment (seq ID and size info)
	 * \param dataPtr Pointer to the fragment's data (i.e. Fragment::headerAddress())
	 * \return Whether the Fragment was successfully added
	 */
	bool AddFragment(detail::RawFragmentHeader frag, void* dataPtr);

public:
	/**
	 * \brief Copy a Fragment into the SharedMemoryEventManager
	 * \param frag FragmentPtr object
	 * \param timeout_usec Timeout for adding Fragment to the Shared Memory
	 * \param [out] outfrag Rejected Fragment if timeout occurs
	 * \return Whether the Fragment was successfully added
	 */
	bool AddFragment(FragmentPtr frag, size_t timeout_usec, FragmentPtr& outfrag);

	/**
	 * \brief Get a pointer to a reserved memory area for the given Fragment header
	 * \param frag Fragment header (contains sequence ID and size information)
	 * \param dropIfNoBuffersAvailable Whether to drop the fragment (instead of returning nullptr) when no buffers are available (Default: false)
	 * \return Pointer to memory location for Fragment body (Header is copied into buffer here)
	 */
	RawDataType* WriteFragmentHeader(detail::RawFragmentHeader frag, bool dropIfNoBuffersAvailable = false);

	/**
	 * \brief Used to indicate that the given Fragment is now completely in the buffer. Will check for buffer completeness, and unset the pending flag.
	 * \param frag Fragment that is now completely in the buffer.
	 */
	void DoneWritingFragment(detail::RawFragmentHeader frag);

	/**
	 * \brief Returns the number of buffers which contain data but are not yet complete
	 * \return The number of buffers which contain data but are not yet complete
	 */
	size_t GetOpenEventCount() { return active_buffers_.size(); }

	/**
	 * \brief Returns the number of events which are complete but waiting on lower sequenced events to finish
	 * \return The number of events which are complete but waiting on lower sequenced events to finish
	 */
	size_t GetPendingEventCount() { return pending_buffers_.size(); }

	/**
	 * \brief Returns the number of buffers currently owned by this manager
	 * \return The number of buffers currently owned by this manager
	 */
	size_t GetLockedBufferCount() { return GetBuffersOwnedByManager().size(); }

	/**
	 * \brief Returns the number of events sent to art this run
	 * \return The number of events sent to art this run
	 */
	size_t GetArtEventCount() { return run_event_count_; }

	/**
	 * \brief Get the count of Fragments of a given type in an event
	 * \param seqID Sequence ID of Fragments
	 * \param type Type of fragments to count. Use InvalidFragmentType to count all fragments (default)
	 * \return Number of Fragments in event of given type
	 */
	size_t GetFragmentCount(Fragment::sequence_id_t seqID, Fragment::type_t type = Fragment::InvalidFragmentType);

	/**
	 * \brief Get the count of Fragments of a given type in a buffer
	 * \param buffer Buffer to count
	 * \param type Type of fragments to count. Use InvalidFragmentType to count all fragments (default)
	 * \return Number of Fragments in buffer of given type
	 */
	size_t GetFragmentCountInBuffer(int buffer, Fragment::type_t type = Fragment::InvalidFragmentType);

	void UpdateFragmentHeader(int buffer, detail::RawFragmentHeader hdr);

	/**
	 * \brief Run an art instance, recording the return codes and restarting it until the end flag is raised
	 */
	void RunArt(size_t process_index, const std::shared_ptr<std::atomic<pid_t>>& pid_out);
	/**
	 * \brief Start all the art processes
	 */
	void StartArt();

	/**
	 * \brief Start one art process
	 * \param pset ParameterSet to send to this art process
	 * \param process_index Index of this art process (when starting multiple)
	 * \return pid_t of the started process
	 */
	pid_t StartArtProcess(fhicl::ParameterSet pset, size_t process_index);

	/**
	 * \brief Shutdown a set of art processes
	 * \param pids PIDs of the art processes
	 */
	void ShutdownArtProcesses(std::set<pid_t>& pids);

	/**
	 * \brief Restart all art processes, using the given fhicl code to configure the new art processes
	 * \param art_pset ParameterSet used to configure art
	 * \param newRun New Run number for reconfigured art
	 * \param n_art_processes Number of art processes to start, -1 (default) leaves the number unchanged
	 */
	void ReconfigureArt(fhicl::ParameterSet art_pset, run_id_t newRun = 0, int n_art_processes = -1);

	/**
	 * \brief Indicate that the end of input has been reached to the art processes.
	 * \return True if the end proceeded correctly
	 *
	 * Put the end-of-data marker onto the RawEvent queue (if possible),
	 * wait for the reader function to exit, and fill in the reader return
	 * value.  This scenario returns true.  If the end-of-data marker
	 * can not be pushed onto the RawEvent queue, false is returned.
	 */
	bool endOfData();

	/**
	 * \brief Start a Run
	 * \param runID Run number of the new run
	 */
	void startRun(run_id_t runID);

	/**
	 * \brief Get the current Run number
	 * \return The current Run number
	 */
	run_id_t runID() const { return run_id_; }

	/**
	 * \brief Send an EndOfRunFragment to the art thread
	 * \return True if enqueue successful
	 */
	bool endRun();

	/**
	 * \brief Rollover the subrun after the specified event
	 * \param boundary sequence ID of the boundary (Event with seqID == boundary will be in new subrun)
	 * \param subrun Subrun number of subrun after boundary
	 */
	void rolloverSubrun(sequence_id_t boundary, subrun_id_t subrun);

	/**
	 * \brief Add a subrun transition immediately after the highest currently define sequence ID
	 */
	void rolloverSubrun();

	/**
	 * \brief Send metrics to the MetricManager, if one has been instantiated in the application
	 */
	void sendMetrics();

	/**
	 * \brief Set the RequestMessageMode for all outgoing data requests
	 * \param mode Mode to set
	 */
	void setRequestMode(detail::RequestMessageMode mode)
	{
		if (requests_) requests_->SetRequestMode(mode);
	}

	/**
	 * \brief Set the overwrite flag (non-reliable data transfer) for the Shared Memory
	 * \param overwrite Whether to allow the writer to overwrite data that has not yet been read
	 */
	void setOverwrite(bool overwrite) { overwrite_mode_ = overwrite; }

	/**
	 * \brief Set the stored Init fragment, if one has not yet been set already.
	 */
	void AddInitFragment(FragmentPtr& frag);

	/**
	 * \brief Gets the shared memory key of the broadcast SharedMemoryManager
	 * \return The shared memory key of the broadcast SharedMemoryManager
	 */
	uint32_t GetBroadcastKey() { return broadcasts_.GetKey(); }

	/**
	 * \brief Gets the address of the "dropped data" fragment. Used for testing.
	 * \param frag Fragment ID to get "dropped data" for
	 * \return Pointer to the data payload of the "dropped data" fragment
	 */
	RawDataType* GetDroppedDataAddress(detail::RawFragmentHeader frag)
	{
		for (auto it = dropped_data_.begin(); it != dropped_data_.end(); ++it)
		{
			if (frag.operator==(it->first))  // TODO, ELF 5/26/2023: Workaround until artdaq_core can be fixed for C++20
			{
				return it->second->dataBegin();
			}
		}
		return nullptr;
	}

	/**
	 * \brief Updates the internally-stored copy of the art configuration.
	 * \param art_pset ParameterSet used to configure art
	 *
	 * This method updates the internally-stored copy of the art configuration, but it does not
	 * restart art processes.  So, if this method is called while art processes are running, it will
	 * have no effect until the next restart, such as the next Start of run.  Typically, this
	 * method is intended to be called between runs, when no art processes are running.
	 */
	void UpdateArtConfiguration(fhicl::ParameterSet art_pset);

	/**
	 * \brief Check for buffers which are ready to be marked incomplete and released to art and issue tokens for any buffers which are avaialble
	 */
	void CheckPendingBuffers();

	/**
	 * \brief Get the subrun number that the given Sequence ID would be assigned to
	 * \param seqID Sequence ID to check
	 * \return Subrun number that the given sequence ID will be associated with
	 */
	subrun_id_t GetSubrunForSequenceID(Fragment::sequence_id_t seqID);

	/**
	 * \brief Get the current subrun number (Gets the last defined subrun)
	 * \return Number of the subrun that corresponds to events with the maximum possible sequence ID.
	 */
	subrun_id_t GetCurrentSubrun() { return GetSubrunForSequenceID(Fragment::InvalidSequenceID); }

private:
	SharedMemoryEventManager(SharedMemoryEventManager const&) = delete;
	SharedMemoryEventManager(SharedMemoryEventManager&&) = delete;
	SharedMemoryEventManager& operator=(SharedMemoryEventManager const&) = delete;
	SharedMemoryEventManager& operator=(SharedMemoryEventManager&&) = delete;

	size_t get_art_process_count_()
	{
		std::unique_lock<std::mutex> lk(art_process_mutex_);
		return art_processes_.size();
	}

	std::string buildStatisticsString_() const;

private:
	size_t num_art_processes_;
	size_t const num_fragments_per_event_;
	size_t const queue_size_;
	run_id_t run_id_;

	std::map<sequence_id_t, subrun_id_t> subrun_event_map_;
	size_t max_subrun_event_map_length_;
	static std::mutex subrun_event_map_mutex_;

	std::set<int> active_buffers_;
	std::set<int> pending_buffers_;
	std::unordered_map<Fragment::sequence_id_t, size_t> released_incomplete_events_;
	std::set<Fragment::sequence_id_t> released_events_;
	size_t max_event_list_length_;

	bool update_run_ids_;
	bool use_sequence_id_for_event_number_;
	bool overwrite_mode_;
	size_t init_fragment_count_;
	std::atomic<bool> running_;

	std::unordered_map<int, std::atomic<int>> buffer_writes_pending_;
	std::unordered_map<int, std::mutex> buffer_mutexes_;
	static std::mutex sequence_id_mutex_;

	int open_event_report_interval_ms_;
	std::chrono::steady_clock::time_point last_open_event_report_time_;
	std::chrono::steady_clock::time_point last_backpressure_report_time_;
	std::chrono::steady_clock::time_point last_fragment_header_write_time_;
	std::vector<std::chrono::steady_clock::time_point> event_timing_;

	StatisticsHelper statsHelper_;

	int broadcast_timeout_ms_;

	std::atomic<int> run_event_count_;
	std::atomic<int> run_incomplete_event_count_;
	std::atomic<int> subrun_event_count_;
	std::atomic<int> subrun_incomplete_event_count_;
	std::atomic<int> oversize_fragment_count_;
	int maximum_oversize_fragment_count_;

	mutable std::mutex art_process_mutex_;
	std::set<pid_t> art_processes_;
	std::atomic<bool> restart_art_;
	bool always_restart_art_;
	std::atomic<bool> manual_art_;
	fhicl::ParameterSet current_art_pset_;
	std::shared_ptr<art_config_file> current_art_config_file_;
	std::string art_cmdline_;
	size_t art_process_index_offset_;
	double minimum_art_lifetime_s_;
	size_t art_event_processing_time_us_;

	std::unique_ptr<RequestSender> requests_;
	std::unique_ptr<TokenSender> tokens_;
	fhicl::ParameterSet data_pset_;

	FragmentPtrs init_fragments_;
	std::set<Fragment::fragment_id_t> received_init_frags_;
	std::list<std::pair<detail::RawFragmentHeader, FragmentPtr>> dropped_data_;

	bool broadcastFragments_(FragmentPtrs& frags);

	detail::RawEventHeader* getEventHeader_(int buffer);

	int getBufferForSequenceID_(Fragment::sequence_id_t seqID, bool create_new, Fragment::timestamp_t timestamp = Fragment::InvalidTimestamp);
	bool hasFragments_(int buffer);
	void complete_buffer_(int buffer);
	bool bufferComparator(int bufA, int bufB);
	void check_pending_buffers_(std::unique_lock<std::mutex> const& lock);
	std::vector<char*> parse_art_command_line_(const std::shared_ptr<art_config_file>& config_file, size_t process_index);

	void send_init_frags_();
	SharedMemoryManager broadcasts_;
};
}  // namespace artdaq

#endif  // ARTDAQ_DAQRATE_SHAREDMEMORYEVENTMANAGER_HH
