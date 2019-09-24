#ifndef artdaq_DAQdata_GenericFragmentSimulator_hh
#define artdaq_DAQdata_GenericFragmentSimulator_hh

#include "artdaq-core/Data/Fragment.hh"
#include "artdaq-core/Generators/FragmentGenerator.hh"
#include "artdaq/DAQdata/Globals.hh"
#include "fhiclcpp/fwd.h"

#include <random>

namespace artdaq {
class GenericFragmentSimulator;
}

/**
 * \brief GenericFragmentSimulator creates simulated Generic events, with data
 * distributed according to a "histogram" provided in the configuration
 * data.
 *
 * With this implementation, a single call to getNext(frags) will return
 * a complete event (event ids are incremented automatically); fragment
 * ids are sequential.
 * Event size and content are both configurable; see the implementation for
 * details.
 */
class artdaq::GenericFragmentSimulator : public artdaq::FragmentGenerator
{
public:
	/// <summary>
	/// Configuration of the GenericFragmentSimulator. May be used for parameter validation
	/// </summary>
	struct Config
	{
		/// "content_selection" (Default: 0) : What type of data to fill in generated Fragment payloads
		///	* 0 : Use uninitialized memory
		///	* 1 : Use the Fragment ID
		///	* 2 : Use random data
		///	* 3 : Use the word 0xDEADBEEFDEADBEEF
		fhicl::Atom<size_t> content_selection{fhicl::Name{"content_selection"}, fhicl::Comment{"What type of data to fill in generated Fragment payloads"}, 0};
		/// "payload_size" (Default: 10240) : The size(in words) of the Fragment payload
		fhicl::Atom<size_t> payload_size{fhicl::Name{"payload_size"}, fhicl::Comment{"The size (in words) of the Fragment payload"}, 10240};
		/// "want_random_payload_size" (Default: false) : Whether payload size should be sampled from a random distribution
		fhicl::Atom<bool> want_random_payload_size{fhicl::Name{"want_random_payload_size"}, fhicl::Comment{"Whether payload size should be sampled from a random distribution"}, false};
		/// "random_seed" (Default: 314159) : Random seed for random number distributions
		fhicl::Atom<int64_t> random_seed{fhicl::Name{"random_seed"}, fhicl::Comment{"Random seed for random number distributions"}, 314159};
		/// "fragments_per_event" (Default: 5) : The number of Fragment objects to generate for each sequence ID
		fhicl::Atom<size_t> fragments_per_event{fhicl::Name{"fragments_per_event"}, fhicl::Comment{"The number of Fragment objects to generate for each sequence ID"}, 5};
		/// "starting_fragment_id" (Default: 0) : The first Fragment ID handled by this GenericFragmentSimulator.
		///	*   Fragment IDs will be starting_fragment_id to starting_fragment_id + fragments_per_event.
		fhicl::Atom<Fragment::fragment_id_t> starting_fragment_id{fhicl::Name{"starting_fragment_id"}, fhicl::Comment{"The first Fragment ID handled by this GenericFragmentSimulator."}, 0};
	};
	/// Used for ParameterSet validation (if desired)
	using Parameters = fhicl::WrappedTable<Config>;

	/**
	 * \brief GenericFragmentSimulator Constructor
	 * \param ps ParameterSet used to configure the GenericFragmentSimulator. See artdaq::GenericFragmentSimulator::Config
	 */
	explicit GenericFragmentSimulator(fhicl::ParameterSet const& ps);

	/**
	 * \brief What type of content should the GenericFragmentSimulator put in Fragment objects?
	 */
	enum class content_selector_t : uint8_t
	{
		EMPTY = 0,    ///< Nothing (Default-initialized Fragment)
		FRAG_ID = 1,  ///< Fill the payload with the Fragment ID
		RANDOM = 2,   ///< Use a random distribution to fill the payload
		DEAD_BEEF     ///< Fill the payload with 0xDEADBEEFDEADBEEF
	};

	// Not part of virtual interface: generate a specific fragment.
	using FragmentGenerator::getNext;

	/**
	 * \brief Generate a Fragment according to the value of the content_selectior_t enum
	 * \param sequence_id Sequence ID of generated Fragment
	 * \param fragment_id Fragment ID of generated Fragment
	 * \param[out] frag_ptr Generated Fragment
	 * \return True if no exception or assertion failure
	 */
	bool getNext(Fragment::sequence_id_t sequence_id,
	             Fragment::fragment_id_t fragment_id,
	             FragmentPtr& frag_ptr);

	/**
	 * \brief Get the next Fragment from the generator
	 * \param[out] output List of FragmentPtr objects to add the new Fragment to
	 * \return Whether data taking should continue
	 */
	bool getNext(FragmentPtrs& output) override
	{
		return getNext_(output);
	}

	/**
	 * \brief Get the Fragment IDs generated by this instance
	 * \return The Fragment IDs generated by this instance
	 */
	std::vector<Fragment::fragment_id_t> fragmentIDs() override
	{
		return fragmentIDs_();
	}

private:
	bool getNext_(FragmentPtrs& output);

	std::vector<Fragment::fragment_id_t> fragmentIDs_();

	std::size_t generateFragmentSize_();

	// Configuration
	content_selector_t const content_selection_;
	std::size_t const payload_size_spec_;  // Poisson mean if random size wanted.
	std::vector<Fragment::fragment_id_t> fragment_ids_;

	bool const want_random_payload_size_;

	// State
	std::size_t current_event_num_;
	std::mt19937 engine_;
	std::poisson_distribution<size_t> payload_size_generator_;
	std::uniform_int_distribution<uint64_t> fragment_content_generator_;
};

#endif /* artdaq_DAQdata_GenericFragmentSimulator_hh */
