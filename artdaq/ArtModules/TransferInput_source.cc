#include "artdaq/ArtModules/ArtdaqInputHelper.hh"
#include "artdaq/ArtModules/detail/TransferWrapper.hh"

namespace art {
/**
	 * \brief Trait definition (must precede source typedef).
	 */
template<>
struct Source_generator<ArtdaqInputHelper<artdaq::TransferWrapper>>
{
	static constexpr bool value = true;  ///< Dummy variable
};

/**
	 * \brief TransferInput is an art::Source using the artdaq::TransferWrapper class as the data source
	 */
typedef art::Source<ArtdaqInputHelper<artdaq::TransferWrapper>> TransferInput;
}  // namespace art

DEFINE_ART_INPUT_SOURCE(art::TransferInput)
