#include "artdaq/TransferPlugins/TransferInterface.hh"

namespace artdaq
{
	/**
	 * \brief NullTransfer does not send or receive data, but acts as if it did
	 */
	class NullTransfer : public TransferInterface
	{
	public:
		/**
		 * \brief NullTransfer constructor
		 * \param pset ParameterSet used to configure TransferInterface
		 * \param role Role of this NullTransfer instance (kSend or kReceive)
		 * 
		 * NullTransfer only requires the Parameters for configuring a TransferInterface
		 */
		NullTransfer(const fhicl::ParameterSet& pset, Role role);

		/**
		 * \brief NullTransfer default Destructor
		 */
		virtual ~NullTransfer() = default;

		/**
		 * \brief Pretend to receive a Fragment
		 * \return Source Rank (Success code)
		 * 
		 * WARNING: This function may create unintended side-effets. NullTransfer should
		 * only really be used in Role::kSend!
		 */
		int receiveFragment(artdaq::Fragment&, size_t) override
		{
			return source_rank();
		}

		/**
		 * \brief Pretend to copy a Fragment to a destination
		 * \return CopyStatus::kSuccess (No-Op)
		 */
		CopyStatus copyFragment(artdaq::Fragment&, size_t) override
		{
			return CopyStatus::kSuccess;
		}

		/**
		* \brief Pretend to move a Fragment to a destination
		* \return CopyStatus::kSuccess (No-Op)
		*/
		CopyStatus moveFragment(artdaq::Fragment&&, size_t) override
		{
			return CopyStatus::kSuccess;
		}
	};
}

artdaq::NullTransfer::NullTransfer(const fhicl::ParameterSet& pset, Role role)
	: TransferInterface(pset, role) {}

DEFINE_ARTDAQ_TRANSFER(artdaq::NullTransfer)
