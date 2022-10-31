#include "artdaq/DAQdata/Globals.hh"

#include "cetlib/PluginTypeDeducer.h"
#include "cetlib/ProvideMakePluginMacros.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/types/ConfigurationTable.h"

#include "cetlib/compiler_macros.h"
#include "messagefacility/MessageService/ELdestination.h"
#include "messagefacility/Utilities/ELseverityLevel.h"
#include "messagefacility/Utilities/exception.h"

#define TRACE_NAME "ArtdaqMetric"

namespace mfplugins {
using mf::ELseverityLevel;
using mf::ErrorObj;
using mf::service::ELdestination;

/// <summary>
/// Message Facility destination which logs messages to a TRACE buffer
/// </summary>
class ELArtdaqMetric : public ELdestination
{
public:
	/**
	 * \brief Configuration Parameters for ELArtdaqMetric
	 */
	struct Config
	{
		/// ELDestination common parameters
		fhicl::TableFragment<ELdestination::Config> elDestConfig;
		/// "showDebug" (Default: False): Send metrics for Debug messages
		fhicl::Atom<bool> showDebug =
		    fhicl::Atom<bool>{fhicl::Name{"showDebug"}, fhicl::Comment{"Send Metrics for Debug messages"}, false};
		/// "showInfo" (Default: False): Send metrics for Info messages
		fhicl::Atom<bool> showInfo =
		    fhicl::Atom<bool>{fhicl::Name{"showInfo"}, fhicl::Comment{"Send Metrics for Info messages"}, false};
		/// "showWarning" (Default: true): Send metrics for Warning messages
		fhicl::Atom<bool> showWarning =
		    fhicl::Atom<bool>{fhicl::Name{"showWarning"}, fhicl::Comment{"Send Metrics for Warning messages"}, false};
		/// "showError" (Default: true): Send metrics for Error messages
		fhicl::Atom<bool> showError =
		    fhicl::Atom<bool>{fhicl::Name{"showError"}, fhicl::Comment{"Send Metrics for Error messages"}, false};
		/// "removeNumbers" (Default: true): Remove numbers from message to try to make more overlaps
		fhicl::Atom<bool> removeNumbers = fhicl::Atom<bool>(fhicl::Name{"removeNumbers"}, fhicl::Comment{"Remove numbers from messages"}, true);
		/// "messageLength" (Default: 20): Number of characters to use for metric title
		fhicl::Atom<size_t> messageLength = fhicl::Atom<size_t>(fhicl::Name{"messageLength"}, fhicl::Comment{"Maximum length of metric titles (0 for unlimited)"}, 20);
	};
	/// Used for ParameterSet validation
	using Parameters = fhicl::WrappedTable<Config>;

public:
	/// <summary>
	/// ELArtdaqMetric Constructor
	/// </summary>
	/// <param name="pset">ParameterSet used to configure ELArtdaqMetric</param>
	ELArtdaqMetric(Parameters const& pset);

	/**
	 * \brief Fill the "Prefix" portion of the message
	 * \param o Output stringstream
	 * \param msg MessageFacility object containing header information
	 */
	void fillPrefix(std::ostringstream& o, const ErrorObj& msg) override;

	/**
	 * \brief Fill the "User Message" portion of the message
	 * \param o Output stringstream
	 * \param msg MessageFacility object containing header information
	 */
	void fillUsrMsg(std::ostringstream& o, const ErrorObj& msg) override;

	/**
	 * \brief Fill the "Suffix" portion of the message (Unused)
	 */
	void fillSuffix(std::ostringstream& /*unused*/, const ErrorObj& /*msg*/) override {}

	/**
	 * \brief Serialize a MessageFacility message to the output
	 * \param o Stringstream object containing message data
	 * \param msg MessageFacility object containing header information
	 */
	void routePayload(const std::ostringstream& o, const ErrorObj& msg) override;

private:
	std::string makeMetricName_(const ErrorObj& msg);
	bool showDebug_{false};
	bool showInfo_{false};
	bool showWarning_{true};
	bool showError_{true};
	bool removeNumbers_{true};
	size_t messageLength_{20};
};

// END DECLARATION
//======================================================================
// BEGIN IMPLEMENTATION

//======================================================================
// ELArtdaqMetric c'tor
//======================================================================
ELArtdaqMetric::ELArtdaqMetric(Parameters const& pset)
    : ELdestination(pset().elDestConfig())
    , showDebug_(pset().showDebug())
    , showInfo_(pset().showInfo())
    , showWarning_(pset().showWarning())
    , showError_(pset().showError())
    , removeNumbers_(pset().removeNumbers())
    , messageLength_(pset().messageLength())
{
	TLOG(TLVL_INFO) << "ELArtdaqMetric MessageLogger destination plugin initialized.";
}

//======================================================================
// Message prefix filler ( overriddes ELdestination::fillPrefix )
//======================================================================
void ELArtdaqMetric::fillPrefix(std::ostringstream&, const ErrorObj&)
{
}

//======================================================================
// Message filler ( overriddes ELdestination::fillUsrMsg )
//======================================================================
void ELArtdaqMetric::fillUsrMsg(std::ostringstream& oss, const ErrorObj& msg)
{
	std::ostringstream tmposs;
	ELdestination::fillUsrMsg(tmposs, msg);

	// remove leading "\n" if present
	std::string tmpStr = tmposs.str().compare(0, 1, "\n") == 0 ? tmposs.str().erase(0, 1) : tmposs.str();

	// remove numbers
	std::string usrMsg;
	if (removeNumbers_)
	{
		std::copy_if(tmpStr.begin(), tmpStr.end(),
		             std::back_inserter(usrMsg), isxdigit);
	}
	else {
		usrMsg = tmpStr;
	}

	if (messageLength_ > 0 && usrMsg.size() > messageLength_) {
		usrMsg.resize(messageLength_);
	}

	oss << usrMsg;
}

//======================================================================
// Message router ( overriddes ELdestination::routePayload )
//======================================================================
void ELArtdaqMetric::routePayload(const std::ostringstream& oss, const ErrorObj& msg)
{
	const auto& xid = msg.xid();
	auto message = oss.str();

	auto level = xid.severity().getLevel();
	int lvlNum = -1;

	switch (level)
	{
		case mf::ELseverityLevel::ELsev_success:
		case mf::ELseverityLevel::ELsev_zeroSeverity:
		case mf::ELseverityLevel::ELsev_unspecified:
			if (showDebug_)
			{
				lvlNum = 3;
			}
			break;

		case mf::ELseverityLevel::ELsev_info:
			if (showInfo_)
			{
				lvlNum = 2;
			}
			break;

		case mf::ELseverityLevel::ELsev_warning:
			if (showWarning_)
			{
				lvlNum = 1;
			}
			break;
		default:
			if (showError_)
			{
				lvlNum = 0;
			}
			break;
	}

	if (lvlNum >= 0 && metricMan)
	{
		metricMan->sendMetric(message, 1, "messages/s", lvlNum, artdaq::MetricMode::Rate);
	}
}
}  // end namespace mfplugins

//======================================================================
//
// makePlugin function
//
//======================================================================

#ifndef EXTERN_C_FUNC_DECLARE_START
#define EXTERN_C_FUNC_DECLARE_START extern "C" {
#endif

EXTERN_C_FUNC_DECLARE_START
auto makePlugin(const std::string& /*unused*/, const fhicl::ParameterSet& pset)
{
	return std::make_unique<mfplugins::ELArtdaqMetric>(pset);
}
}

DEFINE_BASIC_PLUGINTYPE_FUNC(mf::service::ELdestination)
