// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H
#define BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H

#include <node/miner.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <optional>
#include <vector>

using node::BlockAssembler;

/**
 * ValidationBlockValidityTestingSetup relies on TestChain100Setup, which
 * Mines 100 blocks during initialization.
 *
 */
struct ValidationBlockValidityTestingSetup : public TestChain100Setup {
    using TestChain100Setup::TestChain100Setup;
    Chainstate& m_chainstate{m_node.chainman->ActiveChainstate()};
    const Consensus::Params& m_params{m_node.chainman->GetParams().GetConsensus()};

    /** Create a new block template using the provided options. */
    CBlock MakeBlock(BlockAssembler::Options options = {});

    /** Verify a block's validity using TestBlockValidity. */
    BlockValidationState TestValidity(const CBlock& block, bool check_pow = false, bool check_merkle = true);

    /** Verify a block's validity using TestBlockValidityWithSpentTxOuts against the active chain tip. */
    BlockValidationState TestValidityWithSpentOutputs(const CBlock& block, std::vector<TxOutput> block_spent_txouts, bool check_pow = false, bool check_merkle = true);

    /** Helper to add a spendable coin to the UTXO set for testing. */
    std::optional<COutPoint> AddCoin(const CScript& script_pub_key = CScript() << OP_TRUE, CAmount amount = 1 * COIN);

    /**
     * Collect spent outputs for all non-coinbase inputs in a block by looking
     * them up in the active chainstate. Inputs not found in the UTXO set are
     * silently omitted, which causes TestValidityWithSpentOutputs to see them
     * as missing - matching the behaviour of TestValidity.
     */
    std::vector<TxOutput> CollectSpentOutputs(const CBlock& block);

    void AddDummySpend(CBlock& block, const COutPoint& outpoint, CAmount out_value,
                       const CScript& out_script = CScript() << OP_TRUE)
    {
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout = outpoint;
        tx.vin[0].scriptSig = CScript();
        tx.vout.resize(1);
        tx.vout[0].nValue = out_value;
        tx.vout[0].scriptPubKey = out_script;
        block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    }
};

#endif // BITCOIN_TEST_UTIL_BLOCK_VALIDITY_H
