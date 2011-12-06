
#include "EventStore.hh"
#include "Fragment.hh"
#include "Perf.hh"
#include <utility>

using namespace std;

EventStore::EventStore(Config const& conf):sources_(conf.sources_)
{
}


void EventStore::operator()(Data const& ef)
{
  // find the event being built and put the fragment into it,
  // start new event if not already present
  // if the event is complete, delete it and report timing

  // make a perf record for a new event
  // make a perf record for the completion of an event

  FragHeader* fh = (FragHeader*)&ef[0];
  long event_id = fh->id_;
  pair<EventMap::iterator,bool> p = events_.insert(EventMap::value_type(event_id,0));

  if(p.second==true)
    PerfWriteEvent(EventMeas::START,event_id);

  ++((p.first)->second);

  if(p.first->second == sources_)
    {
      PerfWriteEvent(EventMeas::END,event_id);
      events_.erase(p.first);
    }  
}
