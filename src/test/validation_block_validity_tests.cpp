// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <node/miner.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <undo.h>
#include <validation.h>
#include <test/util/mining.h>

#include <limits>
#include <vector>
#include <utility>

using node::BlockAssembler;
BOOST_FIXTURE_TEST_SUITE(validation_block_validity_tests, TestChain100Setup)

namespace {

CBlock MakeBlock(const node::NodeContext& node, const BlockAssembler::Options options = BlockAssembler::Options{})
{
    auto block_ptr = PrepareBlock(node, options);
    BOOST_REQUIRE(block_ptr);
    return *block_ptr;
}

} // namespace

// TestBlockValidity — valid cases
BOOST_AUTO_TEST_CASE(tbv_valid_block)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    LOCK(cs_main);
    auto block_state{TestBlockValidity(chainstate, block, false, true)};
    BOOST_CHECK(block_state.IsValid());
}

BOOST_AUTO_TEST_CASE(tbv_does_not_advance_tip)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    LOCK(cs_main);
    const int h = chainstate.m_chain.Height();
    TestBlockValidity(chainstate, block, false, true);
    BOOST_CHECK_EQUAL(chainstate.m_chain.Height(), h);
}

// TestBlockValidity — check failures
BOOST_AUTO_TEST_CASE(tbv_wrong_prev)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    block.hashPrevBlock = uint256::ONE;
    LOCK(cs_main);
    auto s = TestBlockValidity(chainstate, block, false, true);
    BOOST_CHECK(s.IsInvalid());
    BOOST_CHECK_EQUAL(s.GetRejectReason(), "inconclusive-not-best-prevblk");
}

BOOST_AUTO_TEST_CASE(tbv_bad_merkle_root)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    block.hashMerkleRoot = uint256::ONE;
    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidity(chainstate, block, false, true).IsInvalid());
}

BOOST_AUTO_TEST_CASE(tbv_duplicate_txs_malleation)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    block.vtx.push_back(block.vtx[0]);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    LOCK(cs_main);
    auto s = TestBlockValidity(chainstate, block, false, true);
    BOOST_CHECK(s.IsInvalid());
    BOOST_CHECK_EQUAL(s.GetRejectReason(), "bad-txns-duplicate");
}

BOOST_AUTO_TEST_SUITE_END()
