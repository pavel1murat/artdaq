#ifndef ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH
#define ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH

#include <map>
#include <set>
#include <memory>

#include <fhiclcpp/fwd.h>

#include "artdaq-core/Data/Fragment.hh"
#include "artdaq/TransferPlugins/TransferInterface.hh"
#include "artdaq/DAQrate/detail/FragCounter.hh"
#include "artdaq-utilities/Plugins/MetricManager.hh"

namespace artdaq {
  class DataReceiverManager;
}

class artdaq::DataReceiverManager {
public:

  DataReceiverManager(fhicl::ParameterSet);
  ~DataReceiverManager();

  // recvFragment() puts the next received fragment in frag, with the
  // source of that fragment as its return value.
  //
  // It is a precondition that a sources_sending() != 0.
  int recvFragment(Fragment & frag, size_t timeout_usec = 0);

  int calcSource();

  // How many fragments have been received using this DataReceiverManager object?
  size_t count() const;

  // How many fragments have been received from a particular destination.
  size_t slotCount(size_t rank) const;

  std::set<int> enabled_sources() const { return enabled_sources_; }

private:

  std::map<int, std::unique_ptr<artdaq::TransferInterface>> sources_;
  std::set<int> enabled_sources_;
  int current_source_;

  detail::FragCounter recv_frag_count_; // Number of frags received per source.
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
#endif //ARTDAQ_DAQRATE_DATATRANSFERMANAGER_HH
