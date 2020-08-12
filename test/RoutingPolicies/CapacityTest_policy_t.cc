#define BOOST_TEST_MODULE CapacityTest_policy_t
#include <boost/test/unit_test.hpp>

#include "artdaq/RoutingPolicies/makeRoutingManagerPolicy.hh"
#include "fhiclcpp/ParameterSet.h"
#include "fhiclcpp/make_ParameterSet.h"

BOOST_AUTO_TEST_SUITE(CapacityTest_policy_t)

BOOST_AUTO_TEST_CASE(Simple)
{
	fhicl::ParameterSet ps;
	fhicl::make_ParameterSet("receiver_ranks: [1,2,3,4] tokens_used_per_table_percent: 50", ps);

	auto ct = artdaq::makeRoutingManagerPolicy("CapacityTest", ps);

	BOOST_REQUIRE_EQUAL(ct->GetReceiverCount(), 4);

	ct->AddReceiverToken(1, 10);
	ct->AddReceiverToken(2, 10);
	ct->AddReceiverToken(3, 10);
	ct->AddReceiverToken(4, 10);
	auto firstTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(firstTable.size(), 20);
	BOOST_REQUIRE_EQUAL(firstTable[0].destination_rank, 1);
	BOOST_REQUIRE_EQUAL(firstTable[0].sequence_id, 1);
	BOOST_REQUIRE_EQUAL(firstTable[firstTable.size() - 1].destination_rank, 2);
	BOOST_REQUIRE_EQUAL(firstTable[firstTable.size() - 1].sequence_id, 20);

	//ct->Reset();
	ct->AddReceiverToken(1, 1);
	ct->AddReceiverToken(3, 1);
	ct->AddReceiverToken(2, 1);
	ct->AddReceiverToken(4, 1);
	ct->AddReceiverToken(2, 1);
	auto secondTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(secondTable.size(), 13);
	BOOST_REQUIRE_EQUAL(secondTable[0].destination_rank, 1);
	BOOST_REQUIRE_EQUAL(secondTable[0].sequence_id, 21);
	BOOST_REQUIRE_EQUAL(secondTable[1].destination_rank, 2);
	BOOST_REQUIRE_EQUAL(secondTable[1].sequence_id, 22);
	BOOST_REQUIRE_EQUAL(secondTable[2].destination_rank, 2);
	BOOST_REQUIRE_EQUAL(secondTable[2].sequence_id, 23);
	BOOST_REQUIRE_EQUAL(secondTable[3].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[3].sequence_id, 24);
	BOOST_REQUIRE_EQUAL(secondTable[4].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[4].sequence_id, 25);
	BOOST_REQUIRE_EQUAL(secondTable[5].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[5].sequence_id, 26);
	BOOST_REQUIRE_EQUAL(secondTable[6].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[6].sequence_id, 27);
	BOOST_REQUIRE_EQUAL(secondTable[7].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[7].sequence_id, 28);
	BOOST_REQUIRE_EQUAL(secondTable[8].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[8].sequence_id, 29);
	BOOST_REQUIRE_EQUAL(secondTable[9].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[9].sequence_id, 30);
	BOOST_REQUIRE_EQUAL(secondTable[10].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[10].sequence_id, 31);
	BOOST_REQUIRE_EQUAL(secondTable[11].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[11].sequence_id, 32);
	BOOST_REQUIRE_EQUAL(secondTable[12].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(secondTable[12].sequence_id, 33);

	//ct->Reset();
	ct->AddReceiverToken(1, 0);
	auto thirdTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(thirdTable.size(), 6);
	BOOST_REQUIRE_EQUAL(thirdTable[0].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(thirdTable[0].sequence_id, 34);
	BOOST_REQUIRE_EQUAL(thirdTable[1].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(thirdTable[1].sequence_id, 35);
	BOOST_REQUIRE_EQUAL(thirdTable[2].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(thirdTable[2].sequence_id, 36);
	BOOST_REQUIRE_EQUAL(thirdTable[3].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(thirdTable[3].sequence_id, 37);
	BOOST_REQUIRE_EQUAL(thirdTable[4].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(thirdTable[4].sequence_id, 38);
	BOOST_REQUIRE_EQUAL(thirdTable[5].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(thirdTable[5].sequence_id, 39);

	ct->AddReceiverToken(1, 2);
	ct->AddReceiverToken(2, 1);
	ct->AddReceiverToken(3, 1);
	ct->AddReceiverToken(4, 2);
	auto fourthTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(fourthTable.size(), 6);
	BOOST_REQUIRE_EQUAL(fourthTable[0].destination_rank, 1);
	BOOST_REQUIRE_EQUAL(fourthTable[0].sequence_id, 40);
	BOOST_REQUIRE_EQUAL(fourthTable[1].destination_rank, 1);
	BOOST_REQUIRE_EQUAL(fourthTable[1].sequence_id, 41);
	BOOST_REQUIRE_EQUAL(fourthTable[2].destination_rank, 2);
	BOOST_REQUIRE_EQUAL(fourthTable[2].sequence_id, 42);
	BOOST_REQUIRE_EQUAL(fourthTable[3].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(fourthTable[3].sequence_id, 43);
	BOOST_REQUIRE_EQUAL(fourthTable[4].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(fourthTable[4].sequence_id, 44);
	BOOST_REQUIRE_EQUAL(fourthTable[5].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(fourthTable[5].sequence_id, 45);

	ct->AddReceiverToken(3, 1);
	auto fifthTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(fifthTable.size(), 4);
	BOOST_REQUIRE_EQUAL(fifthTable[0].destination_rank, 3);
	BOOST_REQUIRE_EQUAL(fifthTable[0].sequence_id, 46);
	BOOST_REQUIRE_EQUAL(fifthTable[1].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(fifthTable[1].sequence_id, 47);
	BOOST_REQUIRE_EQUAL(fifthTable[2].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(fifthTable[2].sequence_id, 48);
	BOOST_REQUIRE_EQUAL(fifthTable[3].destination_rank, 4);
	BOOST_REQUIRE_EQUAL(fifthTable[3].sequence_id, 49);

	ct->Reset();
	ct->AddReceiverToken(1, 2);
	auto sixthTable = ct->GetCurrentTable();
	BOOST_REQUIRE_EQUAL(sixthTable.size(), 1);
	BOOST_REQUIRE_EQUAL(sixthTable[0].destination_rank, 1);
	BOOST_REQUIRE_EQUAL(sixthTable[0].sequence_id, 1);
}

BOOST_AUTO_TEST_SUITE_END()
