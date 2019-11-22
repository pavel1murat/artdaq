#ifndef artdaq_ArtModules_NetMonWrapper_hh
#define artdaq_ArtModules_NetMonWrapper_hh

#include "artdaq-core/Utilities/ExceptionHandler.hh"
#include "fhiclcpp/ParameterSet.h"

#include <TBufferFile.h>

#include <memory>
#include <string>

namespace art {
/**
	 * \brief This class wraps NetMonTransportService so that it can act as an ArtdaqInput
	 * template class.
	 * 
	 * JCF, May-27-2016
	 *
	 * This class is written with functionality such that it satisfies the
	 * requirements needed to be a template in the ArtdaqInput class
	 */
class NetMonWrapper
{
public:
	/**
		 * \brief NetMonWrapper Constructor
		 * \param ps ParameterSet for NetMonWrapper
		 */
	NetMonWrapper(fhicl::ParameterSet const& ps);

	/**
		 * \brief NetMonWrapper Destructor
		 */
	virtual ~NetMonWrapper() = default;

	/**
		 * \brief Receive a message from the NetMonTransportService
		 * \param[out] msg A pointer to the received message
		 */
	void receiveMessage(std::unique_ptr<TBufferFile>& msg);

	/**
		* \brief Receive an init message from the NetMonTransportService
		* \param[out] msg A pointer to the received message
		*/
	void receiveInitMessage(std::unique_ptr<TBufferFile>& msg);

private:
	fhicl::ParameterSet data_pset_;
	bool init_received_;
	double init_timeout_s_;
};
}  // namespace art

#endif /* artdaq_ArtModules_NetMonWrapper_hh */
