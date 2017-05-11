#include "artdaq/Application/Commandable.hh"
#include "artdaq/Application/MPI2/MPISentry.hh"
#include "artdaq/DAQdata/configureMessageFacility.hh"
#include "artdaq/DAQrate/quiet_mpi.hh"
#include "artdaq/DAQdata/Globals.hh"
#include "artdaq/ExternalComms/xmlrpc_commander.hh"

#include "boost/program_options.hpp"
#include "boost/lexical_cast.hpp"

#include <iostream>

int main(int argc, char* argv[])
{
	// initialization
	int const wanted_threading_level{MPI_THREAD_FUNNELED};
	artdaq::MPISentry mpiSentry(&argc, &argv, wanted_threading_level);
	artdaq::configureMessageFacility("commandable");
	TLOG_DEBUG("Commandable::main")
		<< "MPI initialized with requested thread support level of "
		<< wanted_threading_level << ", actual support level = "
		<< mpiSentry.threading_level() << "." << TLOG_ENDL;
	TLOG_DEBUG("Commandable::main")
		<< "size = "
		<< mpiSentry.procs()
		<< ", rank = "
		<< mpiSentry.rank() << TLOG_ENDL;

	// handle the command-line arguments
	std::string usage = std::string(argv[0]) + " -p port_number <other-options>";
	boost::program_options::options_description desc(usage);

	desc.add_options()
		("port,p", boost::program_options::value<unsigned short>(), "Port number")
		("help,h", "produce help message");

	boost::program_options::variables_map vm;
	try
	{
		boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(desc).run(), vm);
		boost::program_options::notify(vm);
	}
	catch (boost::program_options::error const& e)
	{
		TLOG_ERROR("Option") << "exception from command line processing in " << argv[0] << ": " << e.what() << TLOG_ENDL;
		return 1;
	}

	if (vm.count("help"))
	{
		std::cout << desc << std::endl;
		return 0;
	}

	if (!vm.count("port"))
	{
		TLOG_ERROR("Option") << argv[0] << " port number not suplied" << std::endl << "For usage and an options list, please do '" << argv[0] << " --help'" << TLOG_ENDL;
		return 1;
	}

	artdaq::setMsgFacAppName("Commandable", vm["port"].as<unsigned short>());

	// create the Commandable object
	artdaq::Commandable commandable;

	// create the xmlrpc_commander and run it
	artdaq::xmlrpc_commander commander(vm["port"].as<unsigned short>(), commandable);
	commander.run();
}
