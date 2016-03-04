////////////////////////////////////////////////////////////////////////
// Class:       RandomDelayFilter
// Module Type: filter
// File:        RandomDelayFilter_module.cc
//
// Generated at Fri Mar  4 10:45:30 2016 by Eric Flumerfelt using artmod
// from cetpkgsupport v1_09_00.
////////////////////////////////////////////////////////////////////////

#include "art/Framework/Core/EDFilter.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Principal/Handle.h"
#include "art/Framework/Principal/Run.h"
#include "art/Framework/Principal/SubRun.h"
#include "art/Utilities/InputTag.h"
#include "fhiclcpp/ParameterSet.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include <memory>
#include <random>
#include <chrono>

namespace artdaq {
  class RandomDelayFilter;
}

class artdaq::RandomDelayFilter : public art::EDFilter {
public:
  explicit RandomDelayFilter(fhicl::ParameterSet const & p);
  // The destructor generated by the compiler is fine for classes
  // without bare pointers or other resource use.

  // Plugins should not be copied or assigned.
  RandomDelayFilter(RandomDelayFilter const &) = delete;
  RandomDelayFilter(RandomDelayFilter &&) = delete;
  RandomDelayFilter & operator = (RandomDelayFilter const &) = delete;
  RandomDelayFilter & operator = (RandomDelayFilter &&) = delete;

  bool filter(art::Event & e) override;

  void reconfigure(fhicl::ParameterSet const & p) override;

private:
  // Random Delay Parameters (min/max for uniform, mean/sigma for normal)
  double min_ms_;
  double max_ms_;
  double mean_ms_;
  double sigma_ms_;

  // Random Engine Setup
  bool isNormal_;
    std::mt19937 engine_;
    std::unique_ptr<std::uniform_real_distribution<double>> uniform_distn_;
    std::unique_ptr<std::normal_distribution<double>> normal_distn_;

};


artdaq::RandomDelayFilter::RandomDelayFilter(fhicl::ParameterSet const & p)
  : min_ms_(p.get<double>("minimum_delay_ms",0))
  , max_ms_(p.get<double>("maximum_delay_ms",1000))
  , mean_ms_(p.get<double>("mean_delay_ms", 500))
  , sigma_ms_(p.get<double>("sigma_delay_ms",100))
  , isNormal_(p.get<bool>("use_normal_distribution", false))
  , engine_(p.get<int64_t>("random_seed",271828))
  , uniform_distn_(new std::uniform_real_distribution<double>(min_ms_, max_ms_))
  , normal_distn_(new std::normal_distribution<double>(mean_ms_, sigma_ms_))
{
}

bool artdaq::RandomDelayFilter::filter(art::Event & e)
{
  double delay = isNormal_ ? (*normal_distn_)(engine_) : (*uniform_distn_)(engine_);
  mf::LogDebug("RandomDelayFilter") << "Simulating processing of event " << e.event() << " by delaying " << std::to_string(delay) << "ms.";

  auto i = 0;
  auto now = std::chrono::high_resolution_clock::now();
  while(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - now).count() < delay)
	{
	  i = i + 1 % std::numeric_limits<int>::max();
	}

  return true;
}

void artdaq::RandomDelayFilter::reconfigure(fhicl::ParameterSet const & p)
{
  min_ms_ = p.get<double>("minimum_delay_ms",0);
  max_ms_=p.get<double>("maximum_delay_ms",1000);
  mean_ms_=p.get<double>("mean_delay_ms", 500);
  sigma_ms_=p.get<double>("sigma_delay_ms",100);
  isNormal_=p.get<bool>("use_normal_distribution", false);
  engine_ = std::mt19937(p.get<int64_t>("random_seed",271828));
  uniform_distn_.reset(new std::uniform_real_distribution<double>(min_ms_, max_ms_));
  normal_distn_.reset(new std::normal_distribution<double>(mean_ms_, sigma_ms_));
}

DEFINE_ART_MODULE(artdaq::RandomDelayFilter)
