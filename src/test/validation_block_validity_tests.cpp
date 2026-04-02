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

void AddSpend(CBlock& block, const COutPoint& outpoint, CAmount out_value,
              const CScript& out_script = CScript() << OP_TRUE)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout   = outpoint;
    tx.vin[0].scriptSig = CScript();
    tx.vout.resize(1);
    tx.vout[0].nValue       = out_value;
    tx.vout[0].scriptPubKey = out_script;
    block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    block.hashMerkleRoot = BlockMerkleRoot(block);
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

// TestBlockValidityWithSpentTxOuts — valid cases
BOOST_AUTO_TEST_CASE(tbvwu_valid_coinbase_only)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    std::vector<TxOutput> undo;
    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsValid());
}

BOOST_AUTO_TEST_CASE(tbvwu_valid_synthetic_spend)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    const CAmount val = 50 * COIN;
    const CScript op_true = CScript() << OP_TRUE;
    const COutPoint synthetic{Txid::FromUint256(uint256::ONE), 0};

    AddSpend(block, synthetic, val - 1000, op_true);

    std::vector<TxOutput> undo;
    undo.push_back({synthetic, Coin(CTxOut(val, op_true), 1, false)});

    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsValid());
}

// TestBlockValidityWithSpentTxOuts — undo data errors
BOOST_AUTO_TEST_CASE(tbvwu_missing_undo_coin)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    AddSpend(block, COutPoint{Txid::FromUint256(uint256::ONE), 0}, 1 * COIN);

    std::vector<TxOutput> undo; // Empty vector = missing coin

    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsInvalid());
}

BOOST_AUTO_TEST_CASE(tbvwu_wrong_amount_in_undo)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    const COutPoint utxo{Txid::FromUint256(uint256::ONE), 0};
    const CScript op_true = CScript() << OP_TRUE;

    AddSpend(block, utxo, 50 * COIN, op_true);

    std::vector<TxOutput> undo;
    undo.push_back({utxo, Coin(CTxOut(1, op_true), 1, false)});

    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsInvalid());
}

BOOST_AUTO_TEST_CASE(tbvwu_immature_coinbase)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    const CAmount val = 50 * COIN;
    const CScript op_true = CScript() << OP_TRUE;
    const COutPoint utxo{Txid::FromUint256(uint256::ONE), 0};

    AddSpend(block, utxo, val - 1000, op_true);

    std::vector<TxOutput> undo;
    undo.push_back({utxo, Coin(CTxOut(val, op_true), 50, true)}); // Coinbase at height 50

    LOCK(cs_main);
    // Tip height is 100. 101 - 50 = 51 < COINBASE_MATURITY
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsInvalid());
}

BOOST_AUTO_TEST_CASE(tbvwu_mature_coinbase)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    const CAmount val = 50 * COIN;
    const CScript op_true = CScript() << OP_TRUE;
    const COutPoint utxo{Txid::FromUint256(uint256::ONE), 0};

    AddSpend(block, utxo, val - 1000, op_true);

    std::vector<TxOutput> undo;
    undo.push_back({utxo, Coin(CTxOut(val, op_true), 1, true)}); // Mature height

    LOCK(cs_main);
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, undo, false, true).IsValid());
}

// Intra-block spend
BOOST_AUTO_TEST_CASE(tbvwu_intra_block_spend)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    const CScript op_true = CScript() << OP_TRUE;

    const COutPoint pre_existing{Txid::FromUint256(uint256::ONE), 0};
    const CAmount pre_val = 50 * COIN;
    std::vector<TxOutput> spent_tx_outputs;
    spent_tx_outputs.push_back({pre_existing, Coin(CTxOut(pre_val, op_true), 1, false)});
    CMutableTransaction tx_a;
    tx_a.vin.resize(1);
    tx_a.vin[0].prevout = pre_existing;
    tx_a.vout.resize(1);
    tx_a.vout[0].nValue       = pre_val - 1000;
    tx_a.vout[0].scriptPubKey = op_true;
    auto tx_a_ref = MakeTransactionRef(std::move(tx_a));

    auto a_outpoint{COutPoint(tx_a_ref->GetHash(), 0)};
    CMutableTransaction tx_b;
    tx_b.vin.resize(1);
    tx_b.vin[0].prevout = a_outpoint;
    tx_b.vout.resize(1);
    tx_b.vout[0].nValue       = tx_a_ref->vout[0].nValue - 1000;
    tx_b.vout[0].scriptPubKey = op_true;

    block.vtx.push_back(tx_a_ref);
    block.vtx.push_back(MakeTransactionRef(std::move(tx_b)));
    block.hashMerkleRoot = BlockMerkleRoot(block);

    LOCK(cs_main);
    // ConnectBlock will update the coins view with Tx A's outputs.
    // Tx B fails to find its input.
    BOOST_CHECK(TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, spent_tx_outputs, false, true).IsValid());

    // Adding the in-block spent to the utxo set will trigger bip30
    spent_tx_outputs.push_back({a_outpoint, Coin(CTxOut(pre_val - 1000, op_true), chainstate.m_chain.Tip()->nHeight, false)});
    auto validity = TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, spent_tx_outputs, false, true);
    BOOST_CHECK_EQUAL(validity.GetRejectReason(), "bad-txns-BIP30");
}

// Cross-function consistency
BOOST_AUTO_TEST_CASE(consistency_bad_coinbase_height)
{
    Chainstate& chainstate = m_node.chainman->ActiveChainstate();
    CBlock block = MakeBlock(m_node);
    CMutableTransaction coinbase(*block.vtx[0]);
    coinbase.vin[0].scriptSig = CScript() << 999999;
    block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    block.hashMerkleRoot = BlockMerkleRoot(block);

    std::vector<TxOutput> spent_tx_outputs;
    LOCK(cs_main);
    auto block_validation_state1 = TestBlockValidity(chainstate, block, false, true);
    auto block_validation_state2 = TestBlockValidityWithSpentTxOuts(chainstate, *chainstate.m_chain.Tip(), block, spent_tx_outputs, false, true);
    BOOST_CHECK(block_validation_state1.IsInvalid());
    BOOST_CHECK(block_validation_state2.IsInvalid());
    BOOST_CHECK_EQUAL(block_validation_state1.GetRejectReason(), block_validation_state2.GetRejectReason());
}

BOOST_AUTO_TEST_SUITE_END()
