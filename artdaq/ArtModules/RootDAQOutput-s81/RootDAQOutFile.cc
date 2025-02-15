#include "artdaq/ArtModules/RootDAQOutput-s81/RootDAQOutFile.h"
#include "TRACE/tracemf.h"
// vim: set sw=2 expandtab :

#include "artdaq/DAQdata/Globals.hh"

#include "art/Framework/Core/OutputModule.h"
#include "art/Framework/IO/FileStatsCollector.h"
#include "art/Framework/Principal/EventPrincipal.h"
#include "art/Framework/Principal/RangeSetsSupported.h"
#include "art/Framework/Principal/ResultsPrincipal.h"
#include "art/Framework/Principal/RunPrincipal.h"
#include "art/Framework/Principal/SubRunPrincipal.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"
#include "art/Framework/Services/System/DatabaseConnection.h"
#include "art/Persistency/Provenance/ProcessHistoryRegistry.h"
#include "art_root_io/GetFileFormatEra.h"
#include "art_root_io/GetFileFormatVersion.h"
#include "art_root_io/RootDB/TKeyVFSOpenPolicy.h"
#include "art_root_io/RootFileBlock.h"
#include "art_root_io/checkDictionaries.h"
#include "art_root_io/detail/getObjectRequireDict.h"
#include "canvas/Persistency/Provenance/BranchChildren.h"
#include "canvas/Persistency/Provenance/BranchType.h"
#include "canvas/Persistency/Provenance/EventAuxiliary.h"
#include "canvas/Persistency/Provenance/EventID.h"
#include "canvas/Persistency/Provenance/FileFormatVersion.h"
#include "canvas_root_io/Utilities/DictionaryChecker.h"
#if ART_HEX_VERSION < 0x31100
#include "canvas/Persistency/Provenance/History.h"
#else
#include "canvas/Persistency/Provenance/Compatibility/History.h"
#endif
#include "canvas/Persistency/Provenance/Parentage.h"
#include "canvas/Persistency/Provenance/ParentageRegistry.h"
#include "canvas/Persistency/Provenance/ProcessHistoryID.h"
#include "canvas/Persistency/Provenance/ProductStatus.h"
#include "canvas/Persistency/Provenance/ResultsAuxiliary.h"
#include "canvas/Persistency/Provenance/RunAuxiliary.h"
#include "canvas/Persistency/Provenance/SubRunAuxiliary.h"
#include "canvas/Persistency/Provenance/rootNames.h"
#include "canvas/Utilities/Exception.h"
#include "cetlib/canonical_string.h"
#include "cetlib/container_algorithms.h"
#include "cetlib/sqlite/Ntuple.h"
#include "cetlib/sqlite/Transaction.h"
#include "cetlib/sqlite/create_table.h"
#include "cetlib/sqlite/exec.h"
#include "cetlib/sqlite/insert.h"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/ParameterSetID.h"
#include "fhiclcpp/ParameterSetRegistry.h"

#include "boost/date_time/posix_time/posix_time.hpp"

#define TRACE_NAME (app_name + "_RootDAQOutFile").c_str()

#include "Rtypes.h"
#include "TBranchElement.h"
#include "TClass.h"
#include "TFile.h"
#include "TTree.h"

#include "sqlite3.h"

#include <fcntl.h>        // posix_fadvise POSIX_FADV_DONTNEED
#include <sys/sysinfo.h>  // sysinfo(sysinfo*)
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

using art::BranchType;
using art::RootDAQOutFile;
using art::rootNames::metaBranchRootName;

namespace {

void create_table(sqlite3* const db,
                  std::string const& name,
                  std::vector<std::string> const& columns,
                  std::string const& suffix = {})
{
	if (columns.empty())
	{
		throw art::Exception(art::errors::LogicError)  // NOLINT(cert-err60-cpp)
		    << "Number of sqlite columns specified for table: " << name << '\n'
		    << "is zero.\n";
	}
	std::string ddl = "DROP TABLE IF EXISTS " + name +
	                  "; "
	                  "CREATE TABLE " +
	                  name + "(" + columns.front();
	for_each(columns.begin() + 1, columns.end(), [&ddl](auto const& col) {
		ddl += "," + col;
	});
	ddl += ") ";
	ddl += suffix;
	ddl += ";";
	cet::sqlite::exec(db, ddl);
}

void insert_eventRanges_row(sqlite3_stmt* stmt,
                            art::SubRunNumber_t const sr,
                            art::EventNumber_t const b,
                            art::EventNumber_t const e)
{
	sqlite3_bind_int64(stmt, 1, sr);
	sqlite3_bind_int64(stmt, 2, b);
	sqlite3_bind_int64(stmt, 3, e);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}

void insert_rangeSets_eventSets_row(sqlite3_stmt* stmt,
                                    unsigned const rsid,
                                    unsigned const esid)
{
	sqlite3_bind_int64(stmt, 1, rsid);
	sqlite3_bind_int64(stmt, 2, esid);
	sqlite3_step(stmt);
	sqlite3_reset(stmt);
}

unsigned
getNewRangeSetID(sqlite3* db,
                 art::BranchType const bt,
                 art::RunNumber_t const r)
{
	cet::sqlite::insert_into(db, art::BranchTypeToString(bt) + "RangeSets")
	    .values(r);
	return sqlite3_last_insert_rowid(db);
}

std::vector<unsigned>
getExistingRangeSetIDs(sqlite3* db, art::RangeSet const& rs)
{
	std::vector<unsigned> rangeSetIDs;
	cet::transform_all(rs, back_inserter(rangeSetIDs), [db](auto const& range) {
		cet::sqlite::query_result<unsigned> r;
		using std::to_string;
		r << cet::sqlite::select("ROWID")
		         .from(db, "EventRanges")
		         .where("SubRun=" + to_string(range.subRun()) +
		                " AND "
		                "begin=" +
		                to_string(range.begin()) +
		                " AND "
		                "end=" +
		                to_string(range.end()));
		return unique_value(r);
	});
	return rangeSetIDs;
}

void insertIntoEventRanges(sqlite3* db, art::RangeSet const& rs)
{
	cet::sqlite::Transaction txn{db};
	sqlite3_stmt* stmt{nullptr};
	std::string const ddl{
	    "INSERT INTO EventRanges(SubRun, begin, end) "
	    "VALUES(?, ?, ?);"};
	sqlite3_prepare_v2(db, ddl.c_str(), -1, &stmt, nullptr);
	for (auto const& range : rs)
	{
		insert_eventRanges_row(stmt, range.subRun(), range.begin(), range.end());
	}
	sqlite3_finalize(stmt);
	txn.commit();
}

void insertIntoJoinTable(sqlite3* db,
                         art::BranchType const bt,
                         unsigned const rsID,
                         std::vector<unsigned> const& eventRangesIDs)
{
	cet::sqlite::Transaction txn{db};
	sqlite3_stmt* stmt{nullptr};
	std::string const ddl{
	    "INSERT INTO " + art::BranchTypeToString(bt) +
	    "RangeSets_EventRanges(RangeSetsID, EventRangesID) Values(?,?);"};
	sqlite3_prepare_v2(db, ddl.c_str(), -1, &stmt, nullptr);
	cet::for_all(eventRangesIDs, [stmt, rsID](auto const eventRangeID) {
		insert_rangeSets_eventSets_row(stmt, rsID, eventRangeID);
	});
	sqlite3_finalize(stmt);
	txn.commit();
}

void maybeInvalidateRangeSet(BranchType const bt,
                             art::RangeSet const& principalRS,
                             art::RangeSet& productRS)
{
	assert(principalRS.is_sorted());
	assert(productRS.is_sorted());
	if (!productRS.is_valid())
	{
		return;
	}
	if (bt == art::InRun && productRS.is_full_run())
	{
		return;
	}
	if (bt == art::InSubRun && productRS.is_full_subRun())
	{
		return;
	}
	if (productRS.ranges().empty())
	{
		return;
	}
	auto const r = productRS.run();
	auto const& productFront = productRS.ranges().front();
	if (!principalRS.contains(r, productFront.subRun(), productFront.begin()))
	{
		productRS = art::RangeSet::invalid();
	}
}

// The purpose of 'maybeInvalidateRangeSet' is to support the
// following situation.  Suppose process 1 creates three files with
// one Run product each, all corresponding to the same Run.  Let's
// call the individual Run product instances in the three separate
// files as A, B, and C.  Now suppose that the three files serve as
// inputs to process 2, where a concatenation is being performed AND
// ALSO an output file switch.  Process 2 results in two output
// files, and now, in process 3, we concatenate the outputs from
// process 2.  The situation would look like this:
//
//  Process 1:   [A]     [B]     [C]
//                 \     / \     /
//  Process 2:     [A + B] [B + C]
//                   \ /     \ /
//        D=agg(A,B)  |       |  E=agg(B,C)
//                     \     /
//  Process 3:         [D + E]
//
// Notice the complication in process 3: product 'B' will be
// aggregated twice: once with A, and once with C.  Whenever the
// output from process 3 is read as input to another process, the
// fetched product will be equivalent to A+2B+C.
//
// To avoid this situation, we compare the RangeSet of the product
// with the RangeSet of the in-memory RunAuxiliary.  If the
// beginning of B's RangeSet is not contained within the auxiliary's
// RangeSet, then a dummy product with an invalid RangeSet is
// written to disk.  Instead of the diagram above, we have:
//
//  Process 1:   [A]     [B]     [C]
//                 \     / \     /
//  Process 2:     [A + B] [x + C]
//                   \ /     \ /
//        D=agg(A,B)  |       |  E=agg(x,C)=C
//                     \     /
//  Process 3:         [D + E]
//
// where 'x' represent a dummy product.  Upon aggregating D and E,
// we obtain the correctly formed A+B+C product.
template<BranchType BT>
art::RangeSet
getRangeSet(art::OutputHandle const& oh,
            art::RangeSet const& principalRS,
            bool const producedInThisProcess)
{
	if constexpr (!art::detail::range_sets_supported(BT))
	{
		{
		}
		return art::RangeSet::invalid();
	}

	auto rs = oh.isValid() ? oh.rangeOfValidity() : art::RangeSet::invalid();
	// Because a user can specify (e.g.):
	//   r.put(move(myProd), art::runFragment(myRangeSet));
	// products that are produced in this process can have valid, yet
	// arbitrary RangeSets.  We therefore never invalidate a RangeSet
	// that corresponds to a product produced in this process.
	//
	// It is possible for a user to specify a RangeSet which does not
	// correspond AT ALL to the in-memory auxiliary RangeSet.  In that
	// case, users should not expect to be able to retrieve products
	// for which no corresponding events or sub-runs were processed.
	if (!producedInThisProcess)
	{
		maybeInvalidateRangeSet(BT, principalRS, rs);
	}
	return rs;
}

template<BranchType BT>
void setProductRangeSetID(art::RangeSet const& rs,
                          sqlite3* db,
                          art::EDProduct* product,
                          std::map<unsigned, unsigned>& checksumToIndexLookup)
{
	if constexpr (!art::detail::range_sets_supported(BT))
	{
		{
		}
		return;
	}

	if (!rs.is_valid())
	{  // Invalid range-sets not written to DB
		return;
	}
	// Set range sets for SubRun and Run products
	auto it = checksumToIndexLookup.find(rs.checksum());
	if (it != checksumToIndexLookup.cend())
	{
		product->setRangeSetID(it->second);
	}
	else
	{
		unsigned const rsID = getNewRangeSetID(db, BT, rs.run());
		product->setRangeSetID(rsID);
		checksumToIndexLookup.emplace(rs.checksum(), rsID);
		insertIntoEventRanges(db, rs);
		auto const& eventRangesIDs = getExistingRangeSetIDs(db, rs);
		insertIntoJoinTable(db, BT, rsID, eventRangesIDs);
	}
}

bool maxCriterionSpecified(art::ClosingCriteria const& cc)
{
	auto fp = std::mem_fn(&art::ClosingCriteria::fileProperties);
	return (fp(cc).nEvents() !=
	        art::ClosingCriteria::Defaults::unsigned_max()) ||
	       (fp(cc).nSubRuns() !=
	        art::ClosingCriteria::Defaults::unsigned_max()) ||
	       (fp(cc).nRuns() != art::ClosingCriteria::Defaults::unsigned_max()) ||
	       (fp(cc).size() != art::ClosingCriteria::Defaults::size_max()) ||
	       (fp(cc).age().count() !=
	        art::ClosingCriteria::Defaults::seconds_max());
}

}  // unnamed namespace

namespace art {

RootDAQOutFile::OutputItem::~OutputItem() = default;

RootDAQOutFile::OutputItem::OutputItem(BranchDescription bd)
    : branchDescription_{std::move(bd)}, product_{nullptr}
{}

std::string const&
RootDAQOutFile::OutputItem::branchName() const
{
	return branchDescription_.branchName();
}

bool RootDAQOutFile::OutputItem::operator<(OutputItem const& rh) const
{
	return branchDescription_ < rh.branchDescription_;
}

// Part of static interface.
bool RootDAQOutFile::shouldFastClone(bool const fastCloningSet,
                                     bool const fastCloning,
                                     bool const wantAllEvents,
                                     ClosingCriteria const& cc)
{
	bool result = fastCloning;
	mf::LogInfo("FastCloning")
	    << "Initial fast cloning configuration "
	    << (fastCloningSet ? "(user-set): " : "(from default): ") << std::boolalpha
	    << fastCloning;
	if (fastCloning && !wantAllEvents)
	{
		result = false;
		mf::LogWarning("FastCloning")
		    << "Fast cloning deactivated due to presence of\n"
		    << "event selection configuration.";
	}
	if (fastCloning && maxCriterionSpecified(cc) &&
	    cc.granularity() < Granularity::InputFile)
	{
		result = false;
		mf::LogWarning("FastCloning")
		    << "Fast cloning deactivated due to request to allow\n"
		    << "output file switching at an Event, SubRun, or Run boundary.";
	}
	return result;
}

RootDAQOutFile::RootDAQOutFile(OutputModule* om,
                               std::string const& fileName,
                               ClosingCriteria const& fileSwitchCriteria,
                               int const compressionLevel,
                               unsigned freePercent,
                               unsigned freeMB,
                               int64_t const saveMemoryObjectThreshold,
                               int64_t const treeMaxVirtualSize,
                               int const splitLevel,
                               int const basketSize,
                               DropMetaData dropMetaData,
                               bool const dropMetaDataForDroppedData,
                               bool const fastCloningRequested)
    : compressionLevel_{compressionLevel}
    , freePercent_{freePercent}
    , freeMB_{freeMB}
    , saveMemoryObjectThreshold_{saveMemoryObjectThreshold}
    , treeMaxVirtualSize_{treeMaxVirtualSize}
    , splitLevel_{splitLevel}
    , basketSize_{basketSize}
    , dropMetaData_{dropMetaData}
    , descriptionsToPersist_{{}}
    , selectedOutputItemList_{{}}
{
	om_ = om;
	file_ = fileName;
	fileSwitchCriteria_ = fileSwitchCriteria;
	status_ = OutputFileStatus::Closed;
	dropMetaDataForDroppedData_ = dropMetaDataForDroppedData;
	fastCloningEnabledAtConstruction_ = fastCloningRequested;
	wasFastCloned_ = false;
	filePtr_.reset(
	    TFile::Open(file_.c_str(), "recreate", "", compressionLevel));
	// Don't split metadata tree or event description tree
	metaDataTree_ = RootOutputTree::makeTTree(
	    filePtr_.get(), rootNames::metaDataTreeName(), 0);
	fileIndexTree_ = RootOutputTree::makeTTree(
	    filePtr_.get(), rootNames::fileIndexTreeName(), 0);
	parentageTree_ = RootOutputTree::makeTTree(
	    filePtr_.get(), rootNames::parentageTreeName(), 0);
	pEventAux_ = nullptr;
	pSubRunAux_ = nullptr;
	pRunAux_ = nullptr;
	pResultsAux_ = nullptr;
	pEventProductProvenanceVector_ = &eventProductProvenanceVector_;
	pSubRunProductProvenanceVector_ = &subRunProductProvenanceVector_;
	pRunProductProvenanceVector_ = &runProductProvenanceVector_;
	pResultsProductProvenanceVector_ = &resultsProductProvenanceVector_;
#if ART_HEX_VERSION < 0x31100
	// Create the tree that will carry (event) History objects.
	eventHistoryTree_ = art::RootOutputTree::makeTTree(
	    filePtr_.get(), art::rootNames::eventHistoryTreeName(), splitLevel);
	if (eventHistoryTree_ == nullptr)
	{
		throw art::Exception(art::errors::FatalRootError)  // NOLINT(cert-err60-cpp)
		    << "Failed to create the tree for History objects\n";
	}
	pHistory_ = new art::History;
	if (eventHistoryTree_->Branch(art::rootNames::eventHistoryBranchName().c_str(),
	                              &pHistory_,
	                              basketSize,
	                              0) == nullptr)
	{
		throw art::Exception(art::errors::FatalRootError)  // NOLINT(cert-err60-cpp)
		    << "Failed to create a branch for History in the output file\n";
	}
	delete pHistory_;
	pHistory_ = nullptr;
#endif
	treePointers_[0] =
	    std::make_unique<RootOutputTree>(filePtr_.get(),
	                                     InEvent,
	                                     pEventAux_,
	                                     pEventProductProvenanceVector_,
	                                     basketSize,
	                                     splitLevel,
	                                     treeMaxVirtualSize,
	                                     saveMemoryObjectThreshold);
	treePointers_[1] =
	    std::make_unique<RootOutputTree>(filePtr_.get(),
	                                     InSubRun,
	                                     pSubRunAux_,
	                                     pSubRunProductProvenanceVector_,
	                                     basketSize,
	                                     splitLevel,
	                                     treeMaxVirtualSize,
	                                     saveMemoryObjectThreshold);
	treePointers_[2] = std::make_unique<RootOutputTree>(filePtr_.get(),
	                                                    InRun,
	                                                    pRunAux_,
	                                                    pRunProductProvenanceVector_,
	                                                    basketSize,
	                                                    splitLevel,
	                                                    treeMaxVirtualSize,
	                                                    saveMemoryObjectThreshold);
	treePointers_[3] =
	    std::make_unique<RootOutputTree>(filePtr_.get(),
	                                     InResults,
	                                     pResultsAux_,
	                                     pResultsProductProvenanceVector_,
	                                     basketSize,
	                                     splitLevel,
	                                     treeMaxVirtualSize,
	                                     saveMemoryObjectThreshold);
	dataTypeReported_ = false;
	rootFileDB_.reset(
	    art::ServiceHandle<art::DatabaseConnection> {} -> get<TKeyVFSOpenPolicy>(
	        "RootFileDB",
	        filePtr_.get(),
	        SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE));
	subRunRSID_ = -1U;
	runRSID_ = -1U;
	beginTime_ = std::chrono::steady_clock::now();
	// Check that dictionaries for the auxiliaries exist
	root::DictionaryChecker checker;
	checker.checkDictionaries<EventAuxiliary>();
	checker.checkDictionaries<SubRunAuxiliary>();
	checker.checkDictionaries<RunAuxiliary>();
	checker.checkDictionaries<ResultsAuxiliary>();
	checker.reportMissingDictionaries();

	createDatabaseTables();
	TLOG(TLVL_DEBUG + 32) << "RootDAQOutFile ctor complete";
}

art::RootDAQOutFile::~RootDAQOutFile()
{
	struct sysinfo info;
	int sts = sysinfo(&info);
	auto free_percent = static_cast<unsigned>(info.freeram * 100 / info.totalram);
	auto free_MB = static_cast<unsigned>(info.freeram * info.mem_unit >> 20);                 // round down (1024.9 => 1024 MB)
	TRACE(TLVL_DEBUG + 32, "~RootDAQOutFile free %%%u %.1fMB (%u) buffers=%fGB mem_unit=%u",  // NOLINT
	      free_percent, static_cast<float>(info.freeram * info.mem_unit / (1024 * 1024.0)),
	      free_MB, static_cast<float>(info.bufferram * info.mem_unit / (1024 * 1024 * 1024.0)), info.mem_unit);
	if (free_percent < freePercent_ || free_MB < freeMB_)
	{
		TLOG(TLVL_DEBUG + 32) << "RootDAQOutFile Flush/DONTNEED";
		filePtr_->Flush();
		sts = posix_fadvise(filePtr_->GetFd(), 0, 0 /*len,0=all*/, POSIX_FADV_DONTNEED);
	}
	TLOG(TLVL_DEBUG + 32) << "~RootDAQOutFile complete sts=" << sts;
}

void art::RootDAQOutFile::createDatabaseTables()
{
	// Event ranges
	create_table(*rootFileDB_,
	             "EventRanges",
	             {"SubRun INTEGER",
	              "begin INTEGER",
	              "end INTEGER",
	              "UNIQUE (SubRun,begin,end) ON CONFLICT IGNORE"});
	// SubRun range sets
	using namespace cet::sqlite;
	create_table(*rootFileDB_, "SubRunRangeSets", column<int>{"Run"});
	create_table(*rootFileDB_,
	             "SubRunRangeSets_EventRanges",
	             {"RangeSetsID INTEGER",
	              "EventRangesID INTEGER",
	              "PRIMARY KEY(RangeSetsID,EventRangesID)"},
	             "WITHOUT ROWID");
	// Run range sets
	create_table(*rootFileDB_, "RunRangeSets", column<int>{"Run"});
	create_table(*rootFileDB_,
	             "RunRangeSets_EventRanges",
	             {"RangeSetsID INTEGER",
	              "EventRangesID INTEGER",
	              "PRIMARY KEY(RangeSetsID,EventRangesID)"},
	             "WITHOUT ROWID");
}

void RootDAQOutFile::setFileStatus(OutputFileStatus const ofs)
{
	std::lock_guard sentry{mutex_};
	status_ = ofs;
}

std::string const&
RootDAQOutFile::currentFileName() const
{
	std::lock_guard sentry{mutex_};
	return file_;
}

void RootDAQOutFile::selectProducts()
{
	std::lock_guard sentry{mutex_};
	auto selectProductsToWrite = [this](BranchType const bt) {
		auto& items = selectedOutputItemList_[bt];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
		for (auto const& pr : om_->keptProducts()[bt])
		{
			auto const& pd = pr.second;
			// Persist Results products only if they have been produced by
			// the current process.
			if (bt == InResults && !pd.produced())
			{
				continue;
			}
			checkDictionaries(pd);
			// Although the transient flag is already checked when
			// OutputModule::doSelectProducts is called, it can be flipped
			// to 'true' after the BranchDescription transients have been
			// fluffed, which happens during the checkDictionaries call.
			if (pd.transient())
			{
				continue;
			}
			items.emplace(pd);
		}
		for (auto const& val : items)
		{
			treePointers_[bt]->addOutputBranch(val.branchDescription_,
			                                   val.product_);
		}
	};
	for_each_branch_type(selectProductsToWrite);
}

void RootDAQOutFile::beginInputFile(RootFileBlock const* rfb,
                                    bool const fastCloneFromOutputModule)
{
	std::lock_guard sentry{mutex_};
	// FIXME: the logic here is nasty.
	bool shouldFastClone{fastCloningEnabledAtConstruction_ &&
	                     fastCloneFromOutputModule && (rfb != nullptr)};
	// Create output branches, and then redo calculation to determine if
	// fast cloning should be done.
	selectProducts();
	if (shouldFastClone &&
	    !treePointers_[InEvent]->checkSplitLevelAndBasketSize(rfb->tree()))
	{
		mf::LogWarning("FastCloning")
		    << "Fast cloning deactivated for this input file due to "
		    << "splitting level and/or basket size.";
		shouldFastClone = false;
	}
	else if ((rfb != nullptr) && rfb->tree() &&
	         rfb->tree()->GetCurrentFile()->GetVersion() < 60001)
	{
		mf::LogWarning("FastCloning")
		    << "Fast cloning deactivated for this input file due to "
		    << "ROOT version used to write it (< 6.00/01)\n"
		       "having a different splitting policy.";
		shouldFastClone = false;
	}
	if (shouldFastClone && rfb->fileFormatVersion().value_ < 10)
	{
		mf::LogWarning("FastCloning")
		    << "Fast cloning deactivated for this input file due to "
		    << "reading in file that has a different ProductID schema.";
		shouldFastClone = false;
	}
	if (shouldFastClone && !fastCloningEnabledAtConstruction_)
	{
		mf::LogWarning("FastCloning")
		    << "Fast cloning reactivated for this input file.";
	}
#if ART_HEX_VERSION < 0x31200
	treePointers_[InEvent]->beginInputFile(shouldFastClone);
#endif
	if (shouldFastClone)
	{
		auto tree = ((rfb != nullptr) && rfb->tree()) ? rfb->tree() : nullptr;
		wasFastCloned_ = treePointers_[InEvent]->fastCloneTree(tree);
	}
}

void RootDAQOutFile::incrementInputFileNumber()
{
	std::lock_guard sentry{mutex_};
	fp_.update_inputFile();
}

void RootDAQOutFile::respondToCloseInputFile(FileBlock const& /*unused*/)
{
	std::lock_guard sentry{mutex_};
	cet::for_all(treePointers_, [](auto const& p) { p->setEntries(); });
}

bool RootDAQOutFile::requestsToCloseFile()
{
	std::lock_guard sentry{mutex_};
	using namespace std::chrono;
	unsigned int constexpr oneK{1024u};
	fp_.updateSize(filePtr_->GetSize() / oneK);
	fp_.updateAge(duration_cast<seconds>(steady_clock::now() - beginTime_));
	return fileSwitchCriteria_.should_close(fp_);
}

void RootDAQOutFile::writeOne(art::EventPrincipal const& e)
{
	std::lock_guard sentry{mutex_};
	TLOG(TLVL_DEBUG + 33) << "Start of RootDAQOutFile::writeOne";
	// Auxiliary branch.
	// Note: pEventAux_ must be set before calling fillBranches
	// since it gets written out in that routine.
	pEventAux_ = &e.eventAux();
	// Because getting the data may cause an exception to be
	// thrown we want to do that first before writing anything
	// to the file about this event.
	fillBranches<InEvent>(e, pEventProductProvenanceVector_);
	// History branch.
#if ART_HEX_VERSION < 0x31100
	art::History historyForOutput{e.history()};
	historyForOutput.addEventSelectionEntry(om_->selectorConfig());
	pHistory_ = &historyForOutput;
	int sz = eventHistoryTree_->Fill();
	if (sz <= 0)
	{
		throw art::Exception(art::errors::FatalRootError)  // NOLINT(cert-err60-cpp)
		    << "Failed to fill the History tree for event: " << e.eventID()
		    << "\nTTree::Fill() returned " << sz << " bytes written." << std::endl;
	}
	pHistory_ = &e.history();
#endif
	// Add the dataType to the job report if it hasn't already been done
	if (!dataTypeReported_)
	{
		std::string dataType{"MC"};
		if (pEventAux_->isRealData())
		{
			dataType = "Data";
		}
		dataTypeReported_ = true;
	}
	// Add event to index
	fileIndex_.addEntry(pEventAux_->eventID(), fp_.eventEntryNumber());
	fp_.update_event();
	TLOG(TLVL_DEBUG + 33) << "End of RootDAQOutFile::writeOne";
}

void RootDAQOutFile::writeSubRun(art::SubRunPrincipal const& sr)
{
	std::lock_guard sentry{mutex_};
	pSubRunAux_ = &sr.subRunAux();
	pSubRunAux_->setRangeSetID(subRunRSID_);
	fillBranches<InSubRun>(sr, pSubRunProductProvenanceVector_);
	fileIndex_.addEntry(EventID::invalidEvent(pSubRunAux_->subRunID()),
	                    fp_.subRunEntryNumber());
	fp_.update_subRun(status_);
}

void RootDAQOutFile::writeRun(art::RunPrincipal const& r)
{
	std::lock_guard sentry{mutex_};
	pRunAux_ = &r.runAux();
	pRunAux_->setRangeSetID(runRSID_);
	fillBranches<InRun>(r, pRunProductProvenanceVector_);
	fileIndex_.addEntry(EventID::invalidEvent(pRunAux_->runID()),
	                    fp_.runEntryNumber());
	fp_.update_run(status_);
}

void RootDAQOutFile::writeParentageRegistry()
{
	std::lock_guard sentry{mutex_};
	auto pid = root::getObjectRequireDict<ParentageID>();
	ParentageID const* hash = &pid;
	if (parentageTree_->Branch(
	        rootNames::parentageIDBranchName().c_str(), &hash, basketSize_, 0) == nullptr)
	{
		throw Exception(errors::FatalRootError)  // NOLINT(cert-err60-cpp)
		    << "Failed to create a branch for ParentageIDs in the output file";
	}
	hash = nullptr;
	auto par = root::getObjectRequireDict<Parentage>();
	Parentage const* desc = &par;
	if (parentageTree_->Branch(
	        rootNames::parentageBranchName().c_str(), &desc, basketSize_, 0) == nullptr)
	{
		throw Exception(errors::FatalRootError)  // NOLINT(cert-err60-cpp)
		    << "Failed to create a branch for Parentages in the output file";
	}
	desc = nullptr;
	for (auto const& pr : ParentageRegistry::get())
	{
		hash = &pr.first;
		desc = &pr.second;
		parentageTree_->Fill();
	}
	parentageTree_->SetBranchAddress(rootNames::parentageIDBranchName().c_str(),
	                                 nullptr);
	parentageTree_->SetBranchAddress(rootNames::parentageBranchName().c_str(),
	                                 nullptr);
}

void RootDAQOutFile::writeFileFormatVersion()
{
	std::lock_guard sentry{mutex_};
	FileFormatVersion const ver{getFileFormatVersion(), getFileFormatEra()};
	auto const* pver = &ver;
	TBranch* b = metaDataTree_->Branch(
	    metaBranchRootName<FileFormatVersion>(), &pver, basketSize_, 0);
	// FIXME: Turn this into a throw!
	assert(b);
	b->Fill();
}

void RootDAQOutFile::writeFileIndex()
{
	std::lock_guard sentry{mutex_};
	fileIndex_.sortBy_Run_SubRun_Event();
	FileIndex::Element elem{};
	auto const* findexElemPtr = &elem;
	TBranch* b = fileIndexTree_->Branch(
	    metaBranchRootName<FileIndex::Element>(), &findexElemPtr, basketSize_, 0);
	// FIXME: Turn this into a throw!
	assert(b);
	for (auto& entry : fileIndex_)
	{
		findexElemPtr = &entry;
		b->Fill();
	}
	b->SetAddress(nullptr);
}

#if ART_HEX_VERSION < 0x31100
void RootDAQOutFile::writeEventHistory()
{
	std::lock_guard sentry{mutex_};
	art::RootOutputTree::writeTTree(eventHistoryTree_);
}
#endif

void RootDAQOutFile::writeProcessConfigurationRegistry()
{
	// We don't do this yet; currently we're storing a slightly
	// bloated ProcessHistoryRegistry.
}

void RootDAQOutFile::writeProcessHistoryRegistry()
{
	std::lock_guard sentry{mutex_};
	ProcessHistoryMap pHistMap;
	for (auto const& pr : ProcessHistoryRegistry::get())
	{
		pHistMap.emplace(pr);
	}
	auto const* p = &pHistMap;
	TBranch* b = metaDataTree_->Branch(
	    metaBranchRootName<ProcessHistoryMap>(), &p, basketSize_, 0);
	if (b == nullptr)
	{
		throw Exception(errors::LogicError)  // NOLINT(cert-err60-cpp)
		    << "Unable to locate required "
		       "ProcessHistoryMap branch in output "
		       "metadata tree.\n";
	}
	b->Fill();
}

void RootDAQOutFile::writeFileCatalogMetadata(
    FileStatsCollector const& stats,
    FileCatalogMetadata::collection_type const& md,
    FileCatalogMetadata::collection_type const& ssmd)
{
	std::lock_guard sentry{mutex_};
	using namespace cet::sqlite;
	Ntuple<std::string, std::string> fileCatalogMetadata{
	    *rootFileDB_, "FileCatalog_metadata", {{"Name", "Value"}}, true};
	Transaction txn{*rootFileDB_};
	for (auto const& kv : md)
	{
		fileCatalogMetadata.insert(kv.first, kv.second);
	}

	// Add our own specific information: File format and friends.
	fileCatalogMetadata.insert("file_format", "\"artroot\"");

	// File start time.
	namespace bpt = boost::posix_time;
	auto formatted_time = [](auto const& t) {
		return cet::canonical_string(bpt::to_iso_extended_string(t));
	};
	fileCatalogMetadata.insert("start_time",
	                           formatted_time(stats.outputFileOpenTime()));
	// File "end" time: now, since file is not actually closed yet.
	fileCatalogMetadata.insert(
	    "end_time",
	    formatted_time(boost::posix_time::second_clock::universal_time()));
	// Run/subRun information.
	if (!stats.seenSubRuns().empty())
	{
		auto I = find_if(md.crbegin(), md.crend(), [](auto const& p) {
			return p.first == "art.run_type";
		});
		if (I != md.crend())
		{
			std::ostringstream buf;
			buf << "[ ";
			for (auto const& srid : stats.seenSubRuns())
			{
				buf << "[ " << srid.run() << ", " << srid.subRun() << ", "
				    << cet::canonical_string(I->second) << " ], ";
			}
			// Rewind over last delimiter.
			buf.seekp(-2, std::ios_base::cur);
			buf << " ]";
			fileCatalogMetadata.insert("runs", buf.str());
		}
	}
	// Number of events.
	fileCatalogMetadata.insert("event_count",
	                           std::to_string(stats.eventsThisFile()));
	fileCatalogMetadata.insert("first_event",
	                           std::to_string(stats.lowestEventID().event()));
	fileCatalogMetadata.insert("last_event",
	                           std::to_string(stats.highestEventID().event()));
	// File parents.
	if (!stats.parents().empty())
	{
		std::ostringstream pstring;
		pstring << "[ ";
		for (auto const& parent : stats.parents())
		{
			pstring << cet::canonical_string(parent) << ", ";
		}
		// Rewind over last delimiter.
		pstring.seekp(-2, std::ios_base::cur);
		pstring << " ]";
		fileCatalogMetadata.insert("parents", pstring.str());
	}

	// The following need to be encapsulated in an art table
	// first_event and last_event.
	auto eidToTuple = [](EventID const& eid) -> std::string {
		std::ostringstream eidStr;
		eidStr << "[ " << eid.run() << ", " << eid.subRun() << ", " << eid.event()
		       << " ]";
		return eidStr.str();
	};
	fileCatalogMetadata.insert("art.first_event",
	                           eidToTuple(stats.lowestEventID()));
	fileCatalogMetadata.insert("art.last_event",
	                           eidToTuple(stats.highestEventID()));
	fileCatalogMetadata.insert("art.file_format_era",
	                           cet::canonical_string(getFileFormatEra()));
	fileCatalogMetadata.insert("art.file_format_version",
	                           std::to_string(getFileFormatVersion()));

	// Incoming stream-specific metadata overrides.
	for (auto const& kv : ssmd)
	{
		fileCatalogMetadata.insert(kv.first, kv.second);
	}
	txn.commit();
}

void RootDAQOutFile::writeParameterSetRegistry()
{
	std::lock_guard sentry{mutex_};
	fhicl::ParameterSetRegistry::exportTo(*rootFileDB_);
}

void RootDAQOutFile::writeProductDescriptionRegistry()
{
	std::lock_guard sentry{mutex_};
	// Make a local copy of the UpdateOutputCallbacks's ProductList,
	// removing any transient or pruned products.
	ProductRegistry reg;
	auto productDescriptionsToWrite = [this, &reg](BranchType const bt) {
		for (auto const& pr : descriptionsToPersist_[bt])  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
		{
			auto const& desc = pr.second;
			reg.productList_.emplace(BranchKey{desc}, desc);
		}
	};
	for_each_branch_type(productDescriptionsToWrite);
	ProductRegistry const* regp = &reg;
	TBranch* b = metaDataTree_->Branch(
	    metaBranchRootName<ProductRegistry>(), &regp, basketSize_, 0);
	// FIXME: Turn this into a throw!
	assert(b);
	b->Fill();
}

void RootDAQOutFile::writeProductDependencies()
{
	std::lock_guard sentry{mutex_};
	BranchChildren const* ppDeps = &om_->branchChildren();
	TBranch* b = metaDataTree_->Branch(
	    metaBranchRootName<BranchChildren>(), &ppDeps, basketSize_, 0);
	// FIXME: Turn this into a throw!
	assert(b);
	b->Fill();
}

void RootDAQOutFile::writeResults(art::ResultsPrincipal& resp)
{
	std::lock_guard sentry{mutex_};
	pResultsAux_ = &resp.resultsAux();
	fillBranches<InResults>(resp, pResultsProductProvenanceVector_);
}

void RootDAQOutFile::writeTTrees()
{
	TLOG(TLVL_DEBUG + 33) << "Start of RootDAQOutFile::writeTTrees";
	std::lock_guard sentry{mutex_};
	RootOutputTree::writeTTree(metaDataTree_);
	TLOG(TLVL_DEBUG + 33) << "RootDAQOutFile::writeTTrees after writing metaDataTree_";
	RootOutputTree::writeTTree(fileIndexTree_);
	TLOG(TLVL_DEBUG + 33) << "RootDAQOutFile::writeTTrees after writing fileIndexTree_";
	RootOutputTree::writeTTree(parentageTree_);
	TLOG(TLVL_DEBUG + 33) << "RootDAQOutFile::writeTTrees after writing parentageTree_";
	for_each_branch_type(
	    [this](BranchType const bt) { treePointers_[bt]->writeTree(); });
	TLOG(TLVL_DEBUG + 33) << "End of RootDAQOutFile::writeTTrees";
}

void RootDAQOutFile::setSubRunAuxiliaryRangeSetID(RangeSet const& ranges)
{
	std::lock_guard sentry{mutex_};
	subRunRSID_ = getNewRangeSetID(*rootFileDB_, InSubRun, ranges.run());
	insertIntoEventRanges(*rootFileDB_, ranges);
	auto const& eventRangesIDs = getExistingRangeSetIDs(*rootFileDB_, ranges);
	insertIntoJoinTable(*rootFileDB_, InSubRun, subRunRSID_, eventRangesIDs);
}

void RootDAQOutFile::setRunAuxiliaryRangeSetID(RangeSet const& ranges)
{
	std::lock_guard sentry{mutex_};
	runRSID_ = getNewRangeSetID(*rootFileDB_, InRun, ranges.run());
	insertIntoEventRanges(*rootFileDB_, ranges);
	auto const& eventRangesIDs = getExistingRangeSetIDs(*rootFileDB_, ranges);
	insertIntoJoinTable(*rootFileDB_, InRun, runRSID_, eventRangesIDs);
}

template<BranchType BT>
art::EDProduct const*
RootDAQOutFile::getProduct(OutputHandle const& oh,
                           RangeSet const& prunedProductRS,
                           std::string const& wrappedName)
{
	std::lock_guard sentry{mutex_};
	if constexpr (detail::range_sets_supported(BT))
	{
		{
		}
		if (!prunedProductRS.is_valid())
		{
			return dummyProductCache_.product(wrappedName);
		}
	}
	return oh.isValid() ? oh.wrapper() : dummyProductCache_.product(wrappedName);
}

template<BranchType BT>
void RootDAQOutFile::fillBranches(art::Principal const& principal,
                                  std::vector<ProductProvenance>* vpp)
{
	TLOG(TLVL_DEBUG + 33) << "Start of RootDAQOutFile::fillBranches";
	std::lock_guard sentry{mutex_};
	bool const fastCloning = ((BT == InEvent) && wasFastCloned_);
	std::map<unsigned, unsigned> checksumToIndex;
	auto const& principalRS = principal.seenRanges();
	std::set<ProductProvenance> keptprv;
	for (auto const& val : selectedOutputItemList_[BT])
	{
		auto const& bd = val.branchDescription_;
		auto const pid = bd.productID();
		descriptionsToPersist_[BT].emplace(pid, bd);
		bool const produced = bd.produced();
		bool const resolveProd = (produced || !fastCloning ||
		                          treePointers_[BT]->uncloned(bd.branchName()));
		// Update the kept provenance
		bool const keepProvenance =
		    ((dropMetaData_ == DropMetaData::DropNone) ||
		     (produced && (dropMetaData_ == DropMetaData::DropPrior)));
		auto const& oh = principal.getForOutput(pid, resolveProd);
		auto prov = keptprv.begin();
		if (keepProvenance)
		{
			if (oh.productProvenance())
			{
				prov = keptprv.insert(*oh.productProvenance()).first;
				if ((dropMetaData_ != DropMetaData::DropAll) &&
				    !dropMetaDataForDroppedData_)
				{
					{
						std::vector<ProductProvenance const*> stacked_pp;
						stacked_pp.push_back(&*oh.productProvenance());
						while (true)
						{
							if (stacked_pp.empty())
							{
								break;
							}
							auto current_pp = stacked_pp.back();
							stacked_pp.pop_back();
							for (auto const parent_bid :
							     current_pp->parentage().parents())
							{
								// Note: Suppose the parent ProductID corresponds to
								//       product that has been requested to be
								//       "dropped"--i.e. someone has specified "drop
								//       *_m1a_*_*" in their configuration, and
								//       although a given product matching this
								//       pattern will not be included in the
								//       selectedProducts_ list, one of the parents of
								//       a selected product can match the "dropping"
								//       pattern and its BranchDescription will still
								//       be written to disk since it is inserted into
								//       the descriptionsToPersist_ data member.
								auto parent_bd = principal.getProductDescription(parent_bid);
								if (!parent_bd)
								{
									// FIXME: Is this an error condition?
									continue;
								}
								descriptionsToPersist_[BT].emplace(parent_bid, *parent_bd);
								if (!parent_bd->produced())
								{
									// We got it from the input, nothing to do.
									continue;
								}
								auto parent_pp =
								    principal.branchToProductProvenance(parent_bid);
								if (!parent_pp || (dropMetaData_ != DropMetaData::DropNone))
								{
									continue;
								}
								if (!keptprv.insert(*parent_pp).second)
								{
									// Already there, done.
									continue;
								}
								if ((dropMetaData_ != DropMetaData::DropAll) &&
								    !dropMetaDataForDroppedData_)
								{
									stacked_pp.push_back(parent_pp.get());
								}
							}
						}
					}
				}
			}
			else
			{
				// No provenance: product was either not produced, or was
				// dropped; create provenance to remember that.
				auto status = productstatus::dropped();
				if (produced)
				{
					status = productstatus::neverCreated();
				}
				prov = keptprv.emplace(pid, status).first;
			}
		}
		// Resolve the product if we are going to attempt to write it out.
		if (resolveProd)
		{
			// Product was either produced, or we are not cloning the whole
			// file and the product branch was not cloned so we should be
			// able to get a pointer to it from the passed principal and
			// write it out.
			auto const& rs = getRangeSet<BT>(oh, principalRS, produced);
			if (detail::range_sets_supported(BT) && !rs.is_valid())
			{
				// At this point we are now going to write out a dummy product
				// whose Wrapper present flag is false because the range set
				// got invalidated to present double counting when combining
				// run or subrun products from multiple fragments.  We change
				// the provenance status that we are going to write out to
				// dummyToPreventDoubleCount to flag this case.  Note that the
				// requirement is only that the status not be
				// productstatus::present().  We use a special code to make it
				// easier for humans to tell what is going on.
				auto prov_bid = prov->productID();
				if (keptprv.erase(*prov) != 1ull)
				{
					throw Exception(errors::LogicError, "KeptProvenance::setStatus")  // NOLINT(cert-err60-cpp)
					    << "Attempt to set product status for product whose provenance "
					       "is not being recorded.\n";
				}
				prov =
				    keptprv
				        .emplace(prov_bid, productstatus::dummyToPreventDoubleCount())
				        .first;
			}
			auto const* product = getProduct<BT>(oh, rs, bd.wrappedName());
			setProductRangeSetID<BT>(
			    rs, *rootFileDB_, const_cast<art::EDProduct*>(product), checksumToIndex);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
			val.product_ = product;
		}
	}
	vpp->assign(keptprv.begin(), keptprv.end());
	for (auto const& val : *vpp)
	{
		if (val.productStatus() == productstatus::uninitialized())
		{
			throw Exception(errors::LogicError,  // NOLINT(cert-err60-cpp)
			                "RootDAQOutFile::fillBranches(principal, vpp):")
			    << "Attempt to write a product with uninitialized provenance!\n";
		}
	}

	TLOG(TLVL_DEBUG + 33) << "RootDAQOutFile::fillBranches before fillTree call";
	treePointers_[BT]->fillTree();
	TLOG(TLVL_DEBUG + 33) << "RootDAQOutFile::fillBranches after fillTree call";
	vpp->clear();
	TLOG(TLVL_DEBUG + 33) << "End of RootDAQOutFile::fillBranches";
}

}  // namespace art
