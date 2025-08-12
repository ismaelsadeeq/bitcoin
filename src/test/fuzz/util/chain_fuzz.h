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
 * TestingSetup extension for chain validation fuzzing harnesses.
 * Provides a pre-mined REGTEST chain with mature UTXOs and helpers for
 * constructing fuzz-driven blocks and transactions. On construction: a REGTEST
 * chain of 2*COINBASE_MATURITY+1 blocks is active, the UTXO set is flushed to
 * disk, m_list_blocks and m_all_utxo_inputs are populated, and the mempool is empty.
 */
class ChainValidationFuzzSetup : public TestingSetup
{
    /**
     * Reset mock time to genesis, destroy and recreate the chainman, then
     * mine 2*COINBASE_MATURITY blocks with P2WSH_OP_TRUE coinbase outputs.
     */
    void ResetChainman();
    /**
     * Traverse the active chain from tip to genesis, populating
     * m_list_blocks (indexed by height) and m_all_utxo_inputs (one CTxIn
     * per spendable output) in ascending block-height order.
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
    /**
     * Apply up to 5 rounds of stateless vtx mutations to @p block:
     * duplicate a transaction, swap two transactions, clear all vtx,
     * insert a transaction with fuzz-driven inputs/outputs, or remove a
     * transaction. Recomputes hashMerkleRoot after all mutations.
     */
    void MutateBlock(CBlock& block, FuzzedDataProvider& fuzzed_data_provider) const;
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

public:
    ChainValidationFuzzSetup(ChainType chain_type, TestOpts opts);
    ~ChainValidationFuzzSetup() = default;
    /** Return the most recently appended block (the current chain tip). */
    std::shared_ptr<const CBlock> LastBlock() const { return m_list_blocks.back(); }
    /**
     * Return a CTxIn that spends output @p vout_index of @p tx.
     * scriptSig / scriptWitness are filled for P2WSH_OP_TRUE,
     * P2SH_OP_TRUE, m_taproot_op_true, and bare-script CScript().
     * Unrecognised or unspendable script types are left empty.
     */
    CTxIn MakeSpendingInput(const CTransaction& tx, unsigned vout_index) const;
    /**
     * Populate mtx.vout with 1–10 outputs whose values and script types
     * are drawn from @p fuzzed_data_provider. Script type is randomly selected
     * from {P2WSH_OP_TRUE, P2SH_OP_TRUE, m_taproot_op_true, bare CScript(),
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
     * Transactions: populated by AddBlockTransactions(); unless @p force_valid_block
     * is true, vtx may be further mutated by MutateBlock() and hashMerkleRoot may
     * be fuzz-replaced (50% probability).
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
 * Randomly overwrite any combination of @p header fields from
 * @p fuzzed_data_provider: nVersion, hashPrevBlock, nTime, nBits.
 */
void MutateBlockHeader(FuzzedDataProvider& fuzzed_data_provider, CBlockHeader& header);

#endif // BITCOIN_TEST_FUZZ_UTIL_CHAIN_FUZZ_H
