#include "artdaq-core/Data/Fragments.hh"
#include "artdaq/DAQrate/DataReceiverManager.hh"
#include "artdaq/DAQrate/DataSenderManager.hh"

#include "artdaq/DAQdata/Debug.hh"
#include "artdaq/DAQdata/Globals.hh"
#include "artdaq-core/Data/Fragment.hh"
#include "trace.h"

#include <fhiclcpp/fwd.h>
#include <fhiclcpp/make_ParameterSet.h>

#include <iostream>
#include <sstream>
#include <mpi.h>
#include <stdlib.h> // for putenv

#define BUFFER_COUNT 10
#define MAX_PAYLOAD_SIZE 0x100000

uint64_t gettimeofday_us( void )
{   struct timeval tv;
    gettimeofday( &tv, NULL );
    return (uint64_t)tv.tv_sec*1000000+tv.tv_usec;
}

void do_sending(fhicl::ParameterSet ps, int sends_each_sender )
{
    TRACE( 7, "do_sending entered RawFragmentHeader::num_words()=%lu"
	  , artdaq::detail::RawFragmentHeader::num_words() );
 
    artdaq::DataSenderManager sender(ps);

    std::vector<artdaq::Fragment> frags(BUFFER_COUNT,artdaq::Fragment());
	uint64_t start_us=gettimeofday_us();
	uint64_t prev_us=start_us;
	uint64_t tot_wrds=0;

    for (int ii=0; ii<sends_each_sender; ++ii)
    {
	  unsigned data_size_wrds = MAX_PAYLOAD_SIZE / sizeof(artdaq::RawDataType) - artdaq::detail::RawFragmentHeader::num_words();
	if (data_size_wrds < 8) data_size_wrds=8;  // min size
	TRACE( 6, "sender rank %d #%u resize datsz=%u",my_rank,ii,data_size_wrds );
	frags[ii%BUFFER_COUNT].resize(data_size_wrds);
	TRACE( 7, "sender rank %d #%u resized bytes=%ld"
	      ,my_rank,ii,frags[ii%BUFFER_COUNT].sizeBytes() );

	unsigned sndDatSz=data_size_wrds;
	frags[ii%BUFFER_COUNT].setSequenceID(ii);
	frags[ii%BUFFER_COUNT].setFragmentID(my_rank);
	frags[ii%BUFFER_COUNT].setSystemType(artdaq::Fragment::DataFragmentType);

	artdaq::Fragment::iterator it=frags[ii%BUFFER_COUNT].dataBegin();
	*it   = my_rank;
	*++it = ii;
	*++it = sndDatSz;

	sender.sendFragment( std::move(frags[ii%BUFFER_COUNT]) );
	//usleep( (data_size_wrds*sizeof(artdaq::RawDataType))/233 );

	uint64_t now_us=gettimeofday_us();
	uint64_t delta_us=now_us-start_us;
	uint64_t delt_inst_us=now_us-prev_us;
	tot_wrds+=sndDatSz;
	TRACE( 8, "sender rank %d #%u sent datSz=%u rate(inst/ave)=%.1f/%.1f MB/s"
	      ,my_rank,ii,sndDatSz
	      , delt_inst_us?(double)sndDatSz*8/(now_us-prev_us):0.0
	      , delta_us?(double)tot_wrds*8/delta_us:0.0 );
	prev_us=now_us;
	frags[ii%BUFFER_COUNT] = artdaq::Fragment(); // replace/renew
	TRACE( 9, "sender rank %d frag replaced",my_rank );
    }

} // do_sending

void do_receiving(fhicl::ParameterSet ps, int totalReceives)
{
  TRACE( 7, "do_receiving entered" );
  artdaq::DataReceiverManager receiver(ps);
  receiver.start_threads();
  int counter = totalReceives;
  while (counter > 0) {
	TRACE(7, "Counter is %d, calling recvFragment", counter);
	int senderSlot = artdaq::TransferInterface::RECV_TIMEOUT;
    auto ignoreFragPtr = receiver.recvFragment(senderSlot);
	if(senderSlot != artdaq::TransferInterface::RECV_TIMEOUT) counter--;
	TRACE(7, "Recv Loop end, counter is %d", counter);
  }
}

int main(int argc, char * argv[])
{
  TRACE_CNTL("reset");
  TRACE( 10, "s_r_handles main enter" );

    std::cout << "argc:" << argc << std::endl;
    for (int i = 0; i < argc; ++i) {
      std::cout << "argv[" << i << "]: " << argv[i] << std::endl;
    }

  if (argc < 4 || 5 < argc) {
    std::cerr << argv[0] << " requires 3 or 4 arguments, " << argc - 1 << " provided" << std::endl;
	std::cerr << "Usage: " << argv[0] << " <my_rank> <total ranks> <sending_ranks> [sends_each_sender]" << std::endl;
    return 1;
  }
  my_rank = atoi(argv[1]);
  auto total_ranks = atoi(argv[2]);
  auto num_sending_ranks = atoi(argv[3]);
  
  int sends_each_sender=10; // besides "EOD" sends
  if (argc == 5) sends_each_sender = atoi(argv[4]);
  auto num_receiving_ranks = total_ranks - num_sending_ranks;
  if (my_rank == 0) {
    std::cout << "Total number of ranks:       " << total_ranks <<"\n";
    std::cout << "Number of sending ranks:     " << num_sending_ranks <<"\n";
    std::cout << "Number of receiving ranks:   " << num_receiving_ranks <<"\n";
    std::cout << "Number of sends_each_sender: " << sends_each_sender <<"\n";
  }
  configureDebugStream(my_rank, 0);


  int totalSends = 0;
  if(num_receiving_ranks > 0) {
	if(num_sending_ranks * sends_each_sender % num_receiving_ranks != 0) {
	  std::cout << "Adding sends so that sends_each_sender * num_sending_ranks is a multiple of num_receiving_ranks" << std::endl;
	  while(num_sending_ranks * sends_each_sender % num_receiving_ranks != 0) {
		sends_each_sender++;
	  }
	  std::cout << "sends_each_sender is now " << sends_each_sender << std::endl;
	}
	totalSends = num_sending_ranks * sends_each_sender / num_receiving_ranks;
  }

  fhicl::ParameterSet ps;
  std::stringstream ss;
  
  ss << "sources: {";
  for(int ii = 0; ii < num_sending_ranks; ++ii) {
	ss << "s" << ii << ": { transferPluginType: Shmem source_rank: " << ii << " max_fragment_size_words: " << MAX_PAYLOAD_SIZE << "}";
  }
  ss << "} destinations: {"; 
  for(int jj = num_sending_ranks; jj < total_ranks; ++jj) {
	ss << "d" << jj << ": { transferPluginType: Shmem destination_rank: " << jj << " max_fragment_size_words: " << MAX_PAYLOAD_SIZE << "}";
  }
  ss << "}";

  make_ParameterSet(ss.str(), ps);

  std::cout << "Going to configure with ParameterSet: " << ps.to_string() << std::endl;

  if (my_rank < num_sending_ranks) {
    do_sending(ps,sends_each_sender);
  }
  else {
    do_receiving(ps, totalSends);
  }
  TRACE( 11, "s_r_handles main return" );
  return 0;
}
