////////////////////////////////////////////////////////////////////////
// genToArt
//
// This application is intended to invoke a configurable set of fragment
// generators, and funnel the result to an invocation of art. This is
// not an MPI application (see caveat below), so is not intended for
// high performance production DAQ scenarios -- for that, see the pmt
// application driver and its associated applcations boardreader and
// eventbuilder.
////////////////////////////////////////////////////////////////////////

#include "art/Framework/Art/artapp.h"
#include "canvas/Utilities/Exception.h"
#include "artdaq/Application/MPI2/MPISentry.hh"
#include "artdaq-core/Generators/FragmentGenerator.hh"
#include "artdaq-core/Data/Fragment.hh"
#include "artdaq/DAQdata/GenericFragmentSimulator.hh"
#include "artdaq/Application/CommandableFragmentGenerator.hh"
#include "artdaq/DAQrate/SharedMemoryEventManager.hh"
#include "artdaq-core/Generators/makeFragmentGenerator.hh"
#include "artdaq-core/Core/SimpleMemoryReader.hh"
#include "artdaq-core/Utilities/SimpleLookupPolicy.hh"
#include "cetlib/container_algorithms.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/make_ParameterSet.h"

#include <boost/program_options.hpp>

#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace bpo = boost::program_options;

namespace
{
	/**
	 * \brief Compare two integers magnitudes
	 * \param a First integer
	 * \param b Second integer
	 * \return Result of |a| < |b|
	 */
	static bool abs_compare(int a, int b)
	{
		return (std::abs(a) < std::abs(b));
	}

	/**
	 * \brief Process the command line
	 * \param argc Number of arguments
	 * \param argv Array of arguments as strings
	 * \param[out] vm Output boost::program_options::variables_map
	 * \return 0 if success, -1 if excpetion, 1 if help was requested, and 2 if missing required arguments
	 */
	int process_cmd_line(int argc, char** argv,
						 bpo::variables_map& vm)
	{
		std::ostringstream descstr;
		descstr << argv[0]
			<< " <-c <config-file>> <other-options> [<source-file>]+";
		bpo::options_description desc(descstr.str());
		desc.add_options()
			("config,c", bpo::value<std::string>(), "Configuration file.")
			("help,h", "produce help message");
		try
		{
			bpo::store(bpo::command_line_parser(argc, argv).options(desc).run(), vm);
			bpo::notify(vm);
		}
		catch (bpo::error const& e)
		{
			std::cerr << "Exception from command line processing in " << argv[0]
				<< ": " << e.what() << "\n";
			return -1;
		}
		if (vm.count("help"))
		{
			std::cout << desc << std::endl;
			return 1;
		}
		if (!vm.count("config"))
		{
			std::cerr << "Exception from command line processing in " << argv[0]
				<< ": no configuration file given.\n"
				<< "For usage and an options list, please do '"
				<< argv[0] << " --help"
				<< "'.\n";
			return 2;
		}
		return 0;
	}

	/**
	 * \brief ThrottledGenerator: ensure that we only get one fragment per type
	 * at a time from the generator.
	 */
	class ThrottledGenerator
	{
	public:
		/**
		 * \brief ThrottledGenerator Constructor
		 * \param generator Name of the generator plugin to load
		 * \param ps ParameterSet for configuring the FragmentGenerator
		 */
		ThrottledGenerator(std::string const& generator,
						   fhicl::ParameterSet const& ps);

		/**
		 * \brief Get the next fragment from the generator
		 * \param[out] newFrags New Fragment objects are added to this list
		 * \return Whether there is more data forthcoming
		 */
		bool getNext(artdaq::FragmentPtrs& newFrags);

		/**
		 * \brief Get the number of Fragment IDs handled by this generator
		 * \return
		 */
		size_t numFragIDs() const;

		/**
		 * \brief Send start signal to FragmentGenerator, if it's a CommandableFragmentGenerator
		 * \param run Run number  to pass to StartCmd
		 * \param timeout Timeout to pass to StartCmd
		 * \param timestamp Timestamp to pass to StartCmd
		 */
		void start(int run, uint64_t timeout, uint64_t timestamp) const
		{
			auto gen_ptr = dynamic_cast<artdaq::CommandableFragmentGenerator*>(generator_.get());
			if (gen_ptr != nullptr) gen_ptr->StartCmd(run, timeout, timestamp);
		}
		/**
		 * \brief Send stop signal to FragmentGenerator, if it's a CommandableFragmentGenerator
		 * \param timeout Timeout to pass to StopCmd
		 * \param timestamp Timestamp to pass to StopCmd
		 */
		void stop(uint64_t timeout, uint64_t timestamp) const
		{
			auto gen_ptr = dynamic_cast<artdaq::CommandableFragmentGenerator*>(generator_.get());
			if (gen_ptr != nullptr) gen_ptr->StopCmd(timeout, timestamp);
		}

	private:
		bool generateFragments_();

		std::unique_ptr<artdaq::FragmentGenerator> generator_;
		size_t const numFragIDs_;
		std::map<artdaq::Fragment::fragment_id_t,
			std::deque<artdaq::FragmentPtr>> frags_;
	};

	ThrottledGenerator::
		ThrottledGenerator(std::string const& generator,
						   fhicl::ParameterSet const& ps)
		:
		generator_(artdaq::makeFragmentGenerator(generator, ps))
		, numFragIDs_(generator_->fragmentIDs().size())
		, frags_()
	{
		assert(generator_);
	}

	bool
		ThrottledGenerator::
		getNext(artdaq::FragmentPtrs& newFrags)
	{
		if (frags_.size() && frags_.begin()->second.size())
		{ // Something stored.
			for (auto& fQp : frags_)
			{
				assert(fQp.second.size());
				newFrags.emplace_back(std::move(fQp.second.front()));
				fQp.second.pop_front();
			}
		}
		else
		{ // Need fresh fragments.
			return generateFragments_() && getNext(newFrags);
		}
		return true;
	}

	bool
		ThrottledGenerator::
		generateFragments_()
	{
		artdaq::FragmentPtrs incomingFrags;
		bool result{ false };
		while ((result = generator_->getNext(incomingFrags)) &&
			   incomingFrags.empty()) {
		}
		for (auto&& frag : incomingFrags)
		{
			frags_[frag->fragmentID()].emplace_back(std::move(frag));
		}
		return result;
	}

	size_t
		ThrottledGenerator::
		numFragIDs() const
	{
		return numFragIDs_;
	}

	//  artdaq::FragmentGenerator &
	//  ThrottledGenerator::
	//  generator() const
	//  {
	//    return *generator_;
	//  }

	/**
	 * \brief Run the test, instantiating configured generators and an EventStore
	 * \param pset ParameterSet used to configure genToArt
	 * \return Art return code, of 15 if EventStore::endOfData fails
	 *
	 * \verbatim
	 * genToArt accepts the following Parameters:
	 * "reset_sequenceID" (Default: true): Set the sequence IDs on generated Fragment objects to the expected value
	 * "genToArt" (REQUIRED): FHiCL table containing genToArt parameters
	 *   "fragment_receivers" (REQUIRED): List of FHiCL tables configuring the Fragment receivers
	 *     Each table should contain parameter "generator", the FragmentGenerator plugin to load, and any other parameters that generator requires
	 *   "event_builder" (Default: {}): ParameterSet for EventStore. See documentation for configuration parameters.
	 *   "run_number" (REQUIRED): Run number to use
	 *   "events_to_generate" (Default: -1): Number of events to generate
	 *
	 * \endverbatim
	 */
	int process_data(fhicl::ParameterSet const& pset)
	{
		auto const gta_pset = pset.get<fhicl::ParameterSet>("genToArt");

		// Make the generators based on the configuration.
		std::vector<ThrottledGenerator> generators;

		auto const fr_pset = gta_pset.get<std::vector<fhicl::ParameterSet>>("fragment_receivers");
		for (auto const& gen_ps : fr_pset)
		{
			generators.emplace_back(gen_ps.get<std::string>("generator"),
									gen_ps);
		}

		artdaq::FragmentPtrs frags;
		auto const eb_pset = gta_pset.get<fhicl::ParameterSet>("event_builder", {});
		size_t expected_frags_per_event = 0;
		for (auto& gen : generators)
		{
			gen.start(1000, 0, 0);
			expected_frags_per_event += gen.numFragIDs();
		}

		artdaq::SharedMemoryEventManager store(eb_pset, pset.to_string());
		store.startRun(gta_pset.get<int>("run_number", 1000));

		auto const events_to_generate =
			gta_pset.get<artdaq::Fragment::sequence_id_t>("events_to_generate", -1);
		auto const reset_sequenceID = pset.get<bool>("reset_sequenceID", true);
		bool done = false;
		for (artdaq::Fragment::sequence_id_t event_count = 1;
			(events_to_generate == static_cast<decltype(events_to_generate)>(-1)
			 || event_count <= events_to_generate) && (!done);
			 ++event_count)
		{
			for (auto& gen : generators)
			{
				done |= !gen.getNext(frags);
			}
			artdaq::Fragment::sequence_id_t current_sequence_id = -1;
			for (auto& val : frags)
			{
				if (reset_sequenceID)
				{
					val->setSequenceID(event_count);
				}
				if (current_sequence_id ==
					static_cast<artdaq::Fragment::sequence_id_t>(-1))
				{
					current_sequence_id = val->sequenceID();
				}
				else if (val->sequenceID() != current_sequence_id)
				{
					throw art::Exception(art::errors::DataCorruption)
						<< "Data corruption: apparently related fragments have "
						<< " different sequence IDs: "
						<< val->sequenceID()
						<< " and "
						<< current_sequence_id
						<< ".\n";
				}
				artdaq::FragmentPtr tempFrag;
				auto sts = store.AddFragment(std::move(val), 1000000, tempFrag);
				if (!sts)
				{
					TLOG_ERROR("genToArt") << "Fragment was not added after 1s. Check art thread status!" << TLOG_ENDL;
					exit(1);
				}
			}
			frags.clear();
		}
		for (auto& gen : generators)
		{
			gen.stop(0, 0);
		}

		std::vector<int> readerReturnValues;
		bool endSucceeded = store.endOfData(readerReturnValues);
		if (endSucceeded && readerReturnValues.size() > 0)
		{

			return *std::max_element(readerReturnValues.begin(), readerReturnValues.end(), abs_compare);
		}
		else if (endSucceeded)
		{
			return 0;
		}
		else
		{
			return 15;
		}
	}
}

int main(int argc, char* argv[]) try
{
	artdaq::configureMessageFacility("genToArt");
	// Needed in case plugins use eg MPI timing for performance.
	artdaq::MPISentry sentry(&argc, &argv);
	// Command line handling.
	bpo::variables_map vm;
	auto result = process_cmd_line(argc, argv, vm);
	if (result != 0)
	{
		return (result);
	}
	// Read FHiCL configuration file.
	fhicl::ParameterSet pset;
	if (getenv("FHICL_FILE_PATH") == nullptr)
	{
		std::cerr
			<< "INFO: environment variable FHICL_FILE_PATH was not set. Using \".\"\n";
		setenv("FHICL_FILE_PATH", ".", 0);
	}
	artdaq::SimpleLookupPolicy lookup_policy("FHICL_FILE_PATH");
	make_ParameterSet(vm["config"].as<std::string>(), lookup_policy, pset);
	return process_data(pset);
}
catch (std::string& x)
{
	std::cerr << "Exception (type string) caught in genToArt: " << x << '\n';
	return 1;
}
catch (char const* m)
{
	std::cerr << "Exception (type char const*) caught in genToArt: ";
	if (m)
	{
		std::cerr << m;
	}
	else
	{
		std::cerr << "[the value was a null pointer, so no message is available]";
	}
	std::cerr << '\n';
}
