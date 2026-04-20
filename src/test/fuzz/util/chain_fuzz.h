// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_FUZZ_UTIL_CHAIN_FUZZ_H
#define BITCOIN_TEST_FUZZ_UTIL_CHAIN_FUZZ_H

#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cstdint>
#include <memory>
#include <vector>

class CBlockIndex;

/**
 * Chainstate subclass that lifts protected methods to public for testing.
 * Obtained via ChainValidationFuzzSetup::GetTestChainstate().
 */
struct TestChainstate : public Chainstate {
    using Chainstate::ActivateBestChainStep;
    using Chainstate::MaybeUpdateMempoolForReorg;
};

/**
 * TestingSetup extension for chain validation fuzzing harnesses.
 *
 * Provides a pre-mined REGTEST chain with mature UTXOs and helpers for
 * constructing fuzz-driven blocks and transactions.
 *
 * Construction sequence:
 *  1. Assert(EnableFuzzDeterminism()) — enables deterministic fuzzing mode.
 *  2. ResetChainman()  — resets mock time to genesis block time, destroys
 *                        and recreates the chainman, then mines
 *                        2*COINBASE_MATURITY blocks (coinbase_output_script=
 *                        P2WSH_OP_TRUE, include_dummy_extranonce=true) and
 *                        drains the ValidationInterface queue.
 *  3. BuildP2TROpTrueScript() — stores the resulting P2TR scriptPubKey into
 *                               m_taproot_op_true and the two-element witness
 *                               stack into m_taproot_op_true_witness.
 *  4. LoadCurrentChain()  — walks the chain tip-to-genesis under cs_main,
 *                           calling LoadCurrentBlock() per CBlockIndex, then
 *                           reverses m_all_utxo_inputs to ascending height order.
 *  5. AddExtraTxsInMempool() — submits 10 transactions (spending
 *                              m_all_utxo_inputs[1..10]), each with four
 *                              outputs: P2WSH_OP_TRUE (15 BTC), P2SH_OP_TRUE
 *                              (15 BTC), m_taproot_op_true (10 BTC), CScript()
 *                              (10 BTC). Each is fee-boosted by COIN via
 *                              PrioritiseTransaction.
 *  6. MineBlock(coinbase_output_script=P2WSH_OP_TRUE) — confirms the 10
 *                                                        mempool transactions.
 *  7. LoadCurrentBlock(tip) — appends the newly mined tip to m_list_blocks
 *                             under cs_main.
 *  8. ForceFlushStateToDisk() — flushes the UTXO set to disk.
 */
class ChainValidationFuzzSetup : public TestingSetup
{
    /**
     * Destroy and recreate the chainman, reset mock time to genesis, then
     * mine 2*COINBASE_MATURITY blocks with P2WSH_OP_TRUE coinbase outputs.
     * Called once from the constructor to establish the base chain.
     */
    void ResetChainman();
    /**
     * Traverse the active chain from tip to genesis, populating
     * m_list_blocks (indexed by height) and m_all_utxo_inputs (one CTxIn
     * per spendable output) in ascending block-height order.
     * Clears both containers before traversal.
     */
    void LoadCurrentChain();
    /**
     * Read @p current_block from BlockManager into m_list_blocks at index
     * nHeight, resizing the vector as needed. Append a ready-to-use CTxIn
     * for each non-unspendable output into m_all_utxo_inputs.
     */
    void LoadCurrentBlock(Chainstate& chainstate, CBlockIndex* current_block);
    /**
     * Submit 10 transactions to the mempool, each spending one entry from
     * m_all_utxo_inputs[1..10] and creating four outputs: P2WSH_OP_TRUE
     * (15 BTC), P2SH_OP_TRUE (15 BTC), m_taproot_op_true (10 BTC), and
     * bare-script CScript() (10 BTC). Each transaction is fee-boosted by
     * COIN via PrioritiseTransaction so the block assembler always selects
     * it regardless of its feerate.
     */
    void AddExtraTxsInMempool();

    /** Blocks indexed by height from genesis to tip.
     * Updated by LoadCurrentChain() and LoadCurrentBlock(). */
    std::vector<std::shared_ptr<CBlock>> m_list_blocks;
    /** Ready-to-use CTxIn for every spendable output in the chain, in
     * ascending block-height order. Inputs have scriptSig/witness pre-filled
     * for the four known script types; unknown types are left empty. */
    std::vector<CTxIn> m_all_utxo_inputs;
    /** P2TR scriptPubKey committing to a single OP_TRUE tapscript leaf
     * (leaf version 0xc0, internal key = 32 bytes of 0x01). */
    CScript m_taproot_op_true;
    /** Two-element witness stack for spending m_taproot_op_true:
     * [0] serialised OP_TRUE script, [1] control block. */
    std::vector<std::vector<uint8_t>> m_taproot_op_true_witness;
    /**
     * Set whenever WriteBlock or WriteAndActivateBlock adds a block to the
     * blockman. Because block_tree_db is in-memory (TestOpts default), the
     * only way to remove those extra block-index entries is to destroy and
     * recreate the chainman. Disconnecting the active-chain tip is not enough
     * since orphan entries remain in the index. RecreateAndReplayChain clears
     * this flag once the chainman is fresh again.
     */
    bool m_block_index_modified{false};

public:
    ChainValidationFuzzSetup(ChainType chain_type, TestOpts opts);
    ~ChainValidationFuzzSetup() = default;
    /** Return the most recently appended block (the current chain tip). */
    std::shared_ptr<const CBlock> LastBlock() const { return m_list_blocks.back(); }
    /** Return whether WriteBlock or WriteAndActivateBlock has modified the block index. */
    bool IsBlockIndexModified() const { return m_block_index_modified; }
    /** Evict all transactions from the mempool. */
    void ClearMemPool();
    /**
     * Return the active chainstate as a TestChainstate reference, giving
     * access to protected methods (ActivateBestChainStep,
     * MaybeUpdateMempoolForReorg) without additional wrapper functions.
     */
    TestChainstate& GetTestChainstate();
    /**
     * Destroy the chainman (wiping its in-memory block index), recreate it
     * from scratch, and replay every block in m_list_blocks so the chain and
     * UTXO set are restored to the post-initialization state.
     *
     * This is necessary — rather than simply disconnecting blocks — because
     * WriteBlock/WriteAndActivateBlock add entries to the in-memory block
     * index that cannot otherwise be removed. The in-memory block_tree_db is
     * lost when the chainman is destroyed, giving us a clean slate.
     */
    void RecreateAndReplayChain();
    // Helpers
    /** Write @p block to the block index and disk without activating it.
     * Returns nullptr if AddToBlockIndex does not advance m_best_header or
     * if the block fails ContextualCheckBlockHeader, CheckBlock, or
     * ContextualCheckBlock. Sets m_block_index_modified on success. */
    CBlockIndex* WriteBlock(const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /** Submit @p block via AcceptBlock (writing it to disk and the block index).
     * If the block is already in the index, validates it contextually and
     * returns nullptr if validation fails. Returns nullptr if AcceptBlock fails.
     * Sets m_block_index_modified on success. */
    CBlockIndex* WriteAndActivateBlock(const CBlock& block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /**
     * Return a CTxIn that spends output @p vout_index of @p tx.
     * scriptSig / scriptWitness are filled for P2WSH_OP_TRUE,
     * P2SH_OP_TRUE, m_taproot_op_true, and bare-script CScript().
     * Unrecognised or unspendable script types are left empty.
     */
    CTxIn MakeSpendingInput(const CTransaction& tx, unsigned vout_index) const;
    /**
     * Populate mtx.vout with 1–10 outputs whose values and script types
     * are drawn from @p fuzzed_data_provider. Script types cycle over
     * {P2WSH_OP_TRUE, P2SH_OP_TRUE, m_taproot_op_true, bare CScript(),
     * arbitrary byte vector}.
     */
    void ConsumeOutputs(FuzzedDataProvider& fuzzed_data_provider, CMutableTransaction& mtx) const;
    /**
     * Build and return a coinbase transaction for @p target_height.
     * The single vin has a null prevout and MAX_SEQUENCE_NONFINAL.
     * scriptSig is either a BIP34-compliant height push followed by OP_0
     * (50% probability) or an arbitrary byte vector up to 100 bytes.
     * Outputs are populated by ConsumeOutputs(). nVersion and nLockTime
     * are fuzz-mutated with 50% probability each.
     */
    CTransactionRef ConsumeCoinbaseTx(FuzzedDataProvider& fuzzed_data_provider, unsigned target_height) const;
    /**
     * Build and return a non-coinbase transaction with 0–10 inputs.
     * Each input is selected by index from the union of m_all_utxo_inputs
     * and @p additional_utxo, then optionally mutated by MutateTxInput().
     * Outputs are populated by ConsumeOutputs(). nVersion and nLockTime
     * are fuzz-mutated with 50% probability each. A CTxIn for every new
     * output is appended to @p additional_utxo so later transactions in
     * the same block can spend them.
     */
    CTransactionRef ConsumeNonCoinbaseTx(FuzzedDataProvider& fuzzed_data_provider, std::vector<CTxIn>& additional_utxo);
    /**
     * Fill @p block.vtx with a coinbase transaction and 0–5 non-coinbase
     * transactions drawn from @p fuzzed_data_provider. Generates the
     * segwit commitment in the coinbase if any non-coinbase transactions
     * are added. Coinbase outputs are not added to @p additional_utxo
     * because they are immature and cannot be spent within COINBASE_MATURITY
     * blocks.
     */
    void AddBlockTransactions(FuzzedDataProvider& fuzzed_data_provider, CBlock& block, std::vector<CTxIn>& additional_utxo, unsigned target_height);
    /**
     * Scan nNonce from 0 up to nonce_limit (2^20) and stop at the first
     * value for which CheckProofOfWork() passes against the chain's current
     * nBits and the resulting hash is not already in the block index.
     * Asserts if no valid nonce is found (should not happen at low difficulty).
     * Requires cs_main.
     */
    void FindValidNonce(CBlock& block) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /**
     * Build a complete block extending @p prev_block at @p target_height.
     *
     * Header: initialised by InitBlockHeader(); if @p force_valid_block is
     * false, MutateBlockHeader() may overwrite any header field.
     *
     * Transactions: populated by AddBlockTransactions(); hashMerkleRoot is
     * computed from vtx and may be further fuzz-replaced (50% probability)
     * unless @p force_valid_block is true.
     *
     * Nonce: FindValidNonce() is called when @p force_valid_block is true,
     * when the fuzzer elects a valid nonce, or when the current hash is
     * already present in the block index; otherwise nNonce is read directly
     * from @p fuzzed_data_provider.
     *
     * Requires cs_main.
     */
    CBlock ConsumeBlock(FuzzedDataProvider& fuzzed_data_provider, const CBlock& prev_block, unsigned target_height, std::vector<CTxIn>& additional_utxo, bool force_valid_block = false) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};

/**
 * Compute a P2TR output committing to a single OP_TRUE tapscript leaf.
 * Uses leaf version 0xc0 and a fixed internal key (32 bytes of 0x01).
 * Returns {scriptPubKey, witness_stack} where witness_stack is
 * [serialised OP_TRUE script, control block].
 */
std::pair<CScript, std::vector<std::vector<uint8_t>>> BuildP2TROpTrueScript();

/**
 * Randomly overwrite any combination of @p txin fields from
 * @p fuzzed_data_provider: nSequence, prevout.n, prevout.hash,
 * scriptSig (append), and scriptWitness.stack (replace).
 */
void MutateTxInput(FuzzedDataProvider& fuzzed_data_provider, CTxIn& txin);

/**
 * Return a block header that extends @p prev_block: copies nVersion and
 * nBits, sets hashPrevBlock to prev_block.GetHash(), and advances nTime
 * by 2 seconds.
 */
CBlockHeader InitBlockHeader(const CBlock& prev_block);

/**
 * Randomly overwrite any combination of @p header fields from
 * @p fuzzed_data_provider: nVersion, hashPrevBlock, nTime, nBits.
 */
void MutateBlockHeader(FuzzedDataProvider& fuzzed_data_provider, CBlockHeader& header);

#endif // BITCOIN_TEST_FUZZ_UTIL_CHAIN_FUZZ_H
