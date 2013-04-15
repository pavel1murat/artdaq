#include "artdaq/ArtModules/NetMonTransportService.h"
#include "artdaq/DAQrate/RHandles.hh"
#include "artdaq/DAQrate/SHandles.hh"
#include "artdaq/DAQrate/GlobalQueue.hh"

#include "artdaq/DAQdata/Fragment.hh"
#include "artdaq/DAQdata/NetMonHeader.hh"
#include "artdaq/DAQdata/RawEvent.hh"

#include "art/Framework/Services/Registry/ActivityRegistry.h"
#include "art/Utilities/Exception.h"
#include "cetlib/container_algorithms.h"
#include "cetlib/exception.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/ParameterSetRegistry.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "TClass.h"
#include "TServerSocket.h"
#include "TMessage.h"
#include "TBuffer.h"
#include "TBufferFile.h"

#include <cassert>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace cet;
using namespace fhicl;
using namespace std;

static ParameterSet empty_pset;

NetMonTransportService::
~NetMonTransportService()
{
    disconnect();
}

NetMonTransportService::
NetMonTransportService(ParameterSet const& pset, art::ActivityRegistry&)
  : NetMonTransportServiceInterface(), server_sock_(0), sock_(0),
    mpi_buffer_count_(pset.get<size_t>("mpi_buffer_count", 5)),
    max_fragment_size_words_(pset.get<uint64_t>("max_fragment_size_words", 512 * 1024)),
    first_data_sender_rank_(pset.get<size_t>("first_data_sender_rank", 0)),
    first_data_receiver_rank_(pset.get<size_t>("first_data_receiver_rank", 0)),
    data_sender_count_(pset.get<size_t>("data_sender_count", 1)),
    data_receiver_count_(pset.get<size_t>("data_receiver_count", 1)),
    incoming_events_(artdaq::getGlobalQueue()),
    recvd_fragments_(nullptr)
{
  //mf::LogVerbatim("DEBUG") <<
  //     "-----> Begin NetMonTransportService::"
  //    "NetMonTransportService(ParameterSet const & pset, "
  //    "art::ActivityRegistry&)";
    string val = pset.to_indented_string();
    //  mf::LogVerbatim("DEBUG") << "Contents of parameter set:";
    //mf::LogVerbatim("DEBUG") << "";
    //mf::LogVerbatim("DEBUG") << val;
    vector<string> keys = pset.get_pset_keys();
    //for (vector<string>::iterator I = keys.begin(), E = keys.end();
    //        I != E; ++I) {
    //    mf::LogVerbatim("DEBUG") << "key: " << *I;
    //}
    //mf::LogVerbatim("DEBUG") << "this: 0x" << std::hex << this << std::dec;
    //mf::LogVerbatim("DEBUG") <<
    //    "-----> End   NetMonTransportService::"
    //    "NetMonTransportService(ParameterSet const & pset, "
    //    "art::ActivityRegistry&)";
}

void
NetMonTransportService::
connect()
{
    mf::LogVerbatim("DEBUG") << "NetMonTransportService::connect(): Called";
    mf::LogVerbatim("DEBUG") << "NetMonTransportService::connect(): Creating SHandle: " << data_receiver_count_ << " " << first_data_receiver_rank_;
    sender_ptr_.reset(new artdaq::SHandles(mpi_buffer_count_,
    					   max_fragment_size_words_,
    					   data_receiver_count_,
    					   first_data_receiver_rank_));
    mf::LogVerbatim("DEBUG") << "NetMonTransportService::connect(): Creating SHandle, done.";
}

void
NetMonTransportService::
listen()
{
  return;
}

void
NetMonTransportService::
disconnect()
{
    mf::LogVerbatim("DEBUG") << "NetMonTransportService::disconnect(): Called";

    if (sender_ptr_) sender_ptr_.reset(nullptr);
    if (receiver_ptr_) receiver_ptr_.reset(nullptr);
}

void
NetMonTransportService::
sendMessage(uint64_t sequenceId, uint8_t messageType, TBufferFile & msg)
{
  artdaq::NetMonHeader header;
  header.data_length = static_cast<uint64_t>(msg.Length());
  artdaq::Fragment fragment(std::ceil(msg.Length() / sizeof(artdaq::RawDataType) + 1), 
			    sequenceId, 0, messageType, header);

  std::cout << "NetMonTransportService::sendMessage(): Sending fragment of type " << (int)messageType << std::endl;

  memcpy(&*fragment.dataBegin(), msg.Buffer(), msg.Length());
  sender_ptr_->sendFragment(std::move(fragment));
}

void
NetMonTransportService::
receiveMessage(TBufferFile *&msg)
{
    if (recvd_fragments_ == nullptr) {
      mf::LogVerbatim("DEBUG") << "NetMonTransportService::receiveMessage(): No raw events, calling deqWait()...";
      std::shared_ptr<artdaq::RawEvent> popped_event;
      incoming_events_.deqWait(popped_event);

      if (popped_event == nullptr) {
	msg = nullptr;
	return;
      }

      mf::LogVerbatim("DEBUG") << "NetMonTransportService::receiveMessage(): Back from deqWait()";
      recvd_fragments_ = popped_event->releaseProduct();
      /* Events coming out of the EventStore are not sorted but need to be
	 sorted by sequence ID before they can be passed to art. 
      */
      std::sort (recvd_fragments_->begin(), recvd_fragments_->end(), 
		 artdaq::fragmentSequenceIDCompare);
    }

    artdaq::Fragment topFrag = std::move(recvd_fragments_->at(0));
    recvd_fragments_->erase(recvd_fragments_->begin());
    if (recvd_fragments_->size() == 0) {
      recvd_fragments_.reset(nullptr);
    }

    artdaq::NetMonHeader *header = topFrag.metadata<artdaq::NetMonHeader>();

    char *buffer = (char *)malloc(header->data_length);
    memcpy(buffer, &*topFrag.dataBegin(), header->data_length);
    msg = new TBufferFile(TBuffer::kRead, header->data_length, buffer, kTRUE, 0);

    mf::LogVerbatim("DEBUG") << "NetMonTransportService::receiveMessage(): Returning fragment " << topFrag.sequenceID() << " with " << header->data_length << " bytes.";
}

DEFINE_ART_SERVICE_INTERFACE_IMPL(NetMonTransportService,
                                  NetMonTransportServiceInterface)
