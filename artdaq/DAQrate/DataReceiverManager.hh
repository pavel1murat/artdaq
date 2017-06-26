#ifndef ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH
#define ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH

#include <map>
#include <set>
#include <memory>
#include <thread>
#include <condition_variable>

#include "fhiclcpp/fwd.h"

#include "artdaq-core/Data/Fragment.hh"
#include "artdaq/TransferPlugins/TransferInterface.hh"
#include "artdaq/DAQrate/detail/FragCounter.hh"
#include "artdaq-utilities/Plugins/MetricManager.hh"
#include "artdaq/DAQrate/SharedMemoryEventManager.hh"

namespace artdaq
{
	class DataReceiverManager;
}

/**
 * \brief Receives Fragment objects from one or more DataSenderManager instances using TransferInterface plugins
 * DataReceiverMaanger runs a reception thread for each source, and can automatically suppress reception from
 * sources which are going faster than the others.
 */
class artdaq::DataReceiverManager
{
public:

	/**
	 * \brief DataReceiverManager Constructor
	 * \param ps ParameterSet used to configure the DataReceiverManager
	 *
	 * \verbatim
	 * DataReceiverManager accepts the following Parameters:
	 * "auto_suppression_enabled" (Default: true): Whether to suppress a source that gets too far ahead
	 * "max_receive_difference" (Default: 50): Threshold (in sequence ID) for suppressing a source
	 * "receive_timeout_usec" (Default: 100000): The timeout for receive operations
	 * "enabled_sources" (OPTIONAL): List of sources which are enabled. If not specified, all sources are assumed enabled
	 * "sources" (Default: blank table): FHiCL table containing TransferInterface configurations for each source.
	 *   NOTE: "source_rank" MUST be specified (and unique) for each source!
	 * \endverbatim
	 */
	explicit DataReceiverManager(const fhicl::ParameterSet& ps, std::shared_ptr<SharedMemoryEventManager> shm);

	/**
	 * \brief DataReceiverManager Destructor
	 */
	virtual ~DataReceiverManager();
	
	/**
	 * \brief Return the count of Fragment objects received by this DataReceiverManager
	 * \return The count of Fragment objects received by this DataReceiverManager
	 */
	size_t count() const;

	/**
	 * \brief Get the count of Fragment objects received by this DataReceiverManager from a given source
	 * \param rank Source rank to get count for
	 * \return The  count of Fragment objects received by this DataReceiverManager from the source
	 */
	size_t slotCount(size_t rank) const;

	/**
	 * \brief Get the total size of all data recieved by this DataReceiverManager
	 * \return The total size of all data received by this DataReceiverManager
	 */
	size_t byteCount() const;

	/**
	 * \brief Start receiver threads for all enabled sources
	 */
	void start_threads();

	/**
	 * \brief Get the list of enabled sources
	 * \return The list of enabled sources
	 */
	std::set<int> enabled_sources() const { return enabled_sources_; }

	std::set<int> running_sources() const { return running_sources_; }

	/**
	 * \brief Get a handle to the SharedMemoryEventManager connected to this DataReceiverManager
	 * \return shared_ptr to SharedMemoryEventManager instance
	 */
	std::shared_ptr<SharedMemoryEventManager> getSharedMemoryEventManager() const { return shm_manager_; }


	std::shared_ptr<detail::FragCounter> GetReceivedFragmentCount() { return std::shared_ptr<detail::FragCounter>(&recv_frag_count_); }

private:
	void runReceiver_(int);
		
	std::atomic<bool> stop_requested_;

	std::map<int, std::thread> source_threads_;
	std::map<int, std::unique_ptr<TransferInterface>> source_plugins_;
	std::set<int> enabled_sources_;
	std::set<int> running_sources_;

	detail::FragCounter recv_frag_count_; // Number of frags received per source.
	detail::FragCounter recv_frag_size_; // Number of bytes received per source.
	detail::FragCounter recv_seq_count_; // For counting sequence IDs

	size_t receive_timeout_;
	std::shared_ptr<SharedMemoryEventManager> shm_manager_;
};

inline
size_t
artdaq::DataReceiverManager::
count() const
{
	return recv_frag_count_.count();
}

inline
size_t
artdaq::DataReceiverManager::
slotCount(size_t rank) const
{
	return recv_frag_count_.slotCount(rank);
}

inline
size_t
artdaq::DataReceiverManager::
byteCount() const
{
	return recv_frag_size_.count();
}
#endif //ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH
