#ifndef artdaq_DAQrate_SHandles_hh
#define artdaq_DAQrate_SHandles_hh

#include "artdaq/DAQdata/Fragment.hh"
#include "artdaq/DAQdata/Fragments.hh"
#include "artdaq/DAQrate/MPITag.hh"

#include <vector>

#include "artdaq/DAQrate/quiet_mpi.hh"

/*
  Protocol: want to do a send for each request object, then wait for for
  pending requests to complete, followed by a reset to allow another set
  of sends to be completed.

  This needs to be separated into a thing for sending and a thing for receiving.
  There probably needs to be a common class that both use.
 */

namespace artdaq {
  class SHandles;
}

class artdaq::SHandles {
public:
  typedef std::vector<MPI_Request> Requests;
  typedef std::vector<MPI_Status> Statuses;
  typedef std::vector<int> Flags; // busy flags

  SHandles(size_t buffer_count,
           uint64_t max_payload_size,
           size_t dest_count,
           size_t dest_start);

  size_t sendFragment(Fragment &&); // Return dest.
  void sendEODFrag(int dest, size_t nFragments);

  void waitAll();

private:

  int calcDest(Fragment::sequence_id_t) const;
  int findAvailable();
  void sendFragTo(Fragment && frag,
                  int dest);

  size_t const buffer_count_;
  uint64_t const max_payload_size_;
  int const dest_count_;
  int const dest_start_;
  int pos_; // next slot to check

  Requests reqs_;
  Statuses stats_;
  Flags flags_;

  Fragments payload_;
};

#endif /* artdaq_DAQrate_SHandles_hh */
