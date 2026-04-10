// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H
#define BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H

#include <node/miner.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <validation.h>

#include <optional>

using node::BlockAssembler;

/**
 * ValidationBlockValidityTestingSetup relies on TestChain100Setup, which
 * Mines 100 blocks during initialization.
 *
 */
struct ValidationBlockValidityTestingSetup : public TestChain100Setup {
    explicit ValidationBlockValidityTestingSetup(const ChainType chain_type = ChainType::REGTEST, TestOpts opts = {})
        : TestChain100Setup(chain_type, opts) {}

    Chainstate& m_chainstate{m_node.chainman->ActiveChainstate()};
    const Consensus::Params& m_params{m_node.chainman->GetParams().GetConsensus()};

    /** Create a new block template using the provided options. */
    CBlock MakeBlock(BlockAssembler::Options options = {});

    /** Verify a block's validity using TestBlockValidity. */
    BlockValidationState TestValidity(const CBlock& block, bool check_pow = false, bool check_merkle = true);

    /**
     * Populate and return a CBlockUndo by running SpendBlock against the current
     * chain tip. Returns std::nullopt if SpendBlock fails (e.g. missing inputs).
     */
    std::optional<CBlockUndo> PopulateBlockUndo(const CBlock& block);

    /**
     * Verify a block's validity using TestBlockValidityWithUndo.
     * The caller must supply blockundo (e.g. from PopulateBlockUndo or an undo file).
     */
    BlockValidationState TestValidityWithUndo(const CBlock& block, const CBlockUndo& blockundo, bool check_pow = false, bool check_merkle = true);

    /** Helper to add a spendable coin to the UTXO set for testing. */
    std::optional<COutPoint> AddCoin(const CScript& script_pub_key = CScript() << OP_TRUE, CAmount amount = 1 * COIN);

    /** Append a transaction that spends outpoint to block, emitting a single output. */
    void AddDummySpend(CBlock& block, const COutPoint& outpoint, CAmount out_value,
                       const CScript& out_script = CScript() << OP_TRUE)
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = outpoint;
        tx.vout.resize(1);
        tx.vout[0].nValue = out_value;
        tx.vout[0].scriptPubKey = out_script;
        block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    }
};

#endif // BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H
