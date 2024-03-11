// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <logging.h>
#include <node/miner.h>
#include <policy/fees.h>
#include <policy/mempool_fees.h>
#include <policy/policy.h>
#include <validation.h>
#include <validationinterface.h>


using node::GetCustomBlockFeeRateHistogram;

MemPoolPolicyEstimator::MemPoolPolicyEstimator() {}

MempoolFeeEstimationResult MemPoolPolicyEstimator::EstimateFeeWithMemPool(Chainstate& chainstate, const CTxMemPool& mempool, unsigned int confTarget, const bool force, std::string& err_message) const
{
    std::optional<MempoolFeeEstimationResult> cached_fee{std::nullopt};
    std::map<uint64_t, MempoolFeeEstimationResult> blocks_fee_rates_estimates;
    MempoolFeeEstimationResult fee_rate_estimate_result;

    if (confTarget > MAX_CONF_TARGET) {
        err_message = strprintf("Confirmation target %s is above maximum limit of %s, mempool conditions might change and estimates above %s are unreliable.\n", confTarget, MAX_CONF_TARGET, MAX_CONF_TARGET);
        return fee_rate_estimate_result;
    }

    if (!mempool.GetLoadTried()) {
        err_message = "Mempool not finished loading, can't get accurate fee rate estimate.";
        return fee_rate_estimate_result;
    }

    if (!RoughlySynced()) {
        err_message = "Mempool transactions roughly not in sync with previously mined blocks, fee rate estimate won't be reliable.";
        return fee_rate_estimate_result;
    }

    if (!force) {
        cached_fee = cache.get(confTarget);
    }

    if (!cached_fee) {
        std::vector<std::tuple<CFeeRate, uint64_t>> mempool_fee_stats;
        // Always get stats for MAX_CONF_TARGET blocks because current algo
        // fast enough to run that far while we're locked and in here
        {
            LOCK2(cs_main, mempool.cs);
            mempool_fee_stats = GetCustomBlockFeeRateHistogram(chainstate, &mempool, MAX_BLOCK_WEIGHT * MAX_CONF_TARGET);
        }
        if (mempool_fee_stats.empty()) {
            err_message = "No transactions available in the mempool yet.";
            return fee_rate_estimate_result;
        }
        blocks_fee_rates_estimates = EstimateBlockFeeRatesWithMempool(mempool_fee_stats, MAX_CONF_TARGET);
        cache.update(blocks_fee_rates_estimates);
        const auto it = blocks_fee_rates_estimates.find(confTarget);
        if (it != blocks_fee_rates_estimates.end()) {
            fee_rate_estimate_result = it->second;
        }

    } else {
        fee_rate_estimate_result = *cached_fee;
    }

    if (fee_rate_estimate_result.empty()) {
        err_message = "Insufficient mempool transactions to perform an estimate.";
    }
    return fee_rate_estimate_result;
}

void MemPoolPolicyEstimator::EstimateFeeWithMemPool(const ChainstateManager& chainman, const CTxMemPool& mempool, const CBlockPolicyEstimator* fee_estimator) const
{
    std::string err_message;
    LOCK(cs_main);
    CBlockIndex* block = chainman.ActiveTip();
    MempoolFeeEstimationResult estimate = EstimateFeeWithMemPool(chainman.ActiveChainstate(), mempool, /*confTarget=*/1, /*force=*/true, err_message);
    FeeCalculation feeCalc;
    CFeeRate block_estimate = fee_estimator->estimateSmartFee(/*conf_target=*/1, &feeCalc, /*conservative=*/false);
    if (estimate.empty()) {
        LogInfo("At block %s, height %s, failed to get mempool based fee rate estimate; error: %s \n", block->phashBlock->GetHex(), block->nHeight, err_message);
    } else {
        LogInfo("At block %s, height %s, mempool based fee rate estimate for next block has a 75th percentile fee rate %s, 50th percentile fee rate %s, 25th percentile fee rate %s, 5th percentile fee rate %s, block estimate for next block is %s \n",
                    block->phashBlock->GetHex(), block->nHeight, estimate.p75.GetFeePerK(), estimate.p50.GetFeePerK(), estimate.p25.GetFeePerK(), estimate.p5.GetFeePerK(), block_estimate.GetFeePerK());
    }
}

std::map<uint64_t, MempoolFeeEstimationResult> MemPoolPolicyEstimator::EstimateBlockFeeRatesWithMempool(
    const std::vector<std::tuple<CFeeRate, uint64_t>>& mempool_fee_stats, unsigned int confTarget) const
{
    std::map<uint64_t, MempoolFeeEstimationResult> blocks_fee_rates_estimates;
    if (mempool_fee_stats.empty()) return blocks_fee_rates_estimates;

    auto start = mempool_fee_stats.crbegin();
    auto cur = mempool_fee_stats.crbegin();
    auto end = mempool_fee_stats.crend();

    size_t block_number{confTarget};
    size_t block_weight{0};

    while (block_number >= 1 && cur != end) {
        size_t tx_weight = std::get<1>(*cur) * WITNESS_SCALE_FACTOR;
        block_weight += tx_weight;
        auto next_cur = std::next(cur);
        if (block_weight >= DEFAULT_BLOCK_MAX_WEIGHT || next_cur == end) {
            blocks_fee_rates_estimates[block_number] = CalculateBlockPercentiles(start, cur);
            block_number--;
            block_weight = 0;
            start = next_cur;
        }
        cur = next_cur;
    }
    return blocks_fee_rates_estimates;
}

MempoolFeeEstimationResult MemPoolPolicyEstimator::CalculateBlockPercentiles(
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator start,
    std::vector<std::tuple<CFeeRate, uint64_t>>::const_reverse_iterator end) const
{
    unsigned int total_weight = 0;
    const auto p5_Size = DEFAULT_BLOCK_MAX_WEIGHT / 20;
    const auto p25_Size = DEFAULT_BLOCK_MAX_WEIGHT / 4;
    const auto p50_Size = DEFAULT_BLOCK_MAX_WEIGHT / 2;
    const auto p75_Size = (3 * DEFAULT_BLOCK_MAX_WEIGHT) / 4;

    MempoolFeeEstimationResult res;

    for (auto rit = start; rit != end; ++rit) {
        total_weight += std::get<1>(*rit) * WITNESS_SCALE_FACTOR;
        if (total_weight >= p5_Size && res.p5 == CFeeRate(0)) {
            res.p5 = std::get<0>(*rit);
        }
        if (total_weight >= p25_Size && res.p25 == CFeeRate(0)) {
            res.p25 = std::get<0>(*rit);
        }
        if (total_weight >= p50_Size && res.p50 == CFeeRate(0)) {
            res.p50 = std::get<0>(*rit);
        }
        if (total_weight >= p75_Size && res.p75 == CFeeRate(0)) {
            res.p75 = std::get<0>(*rit);
        }
    }
    // Block weight should be at least half of default maximum block weight size
    // for estimates to be reliable.
    if (total_weight < (DEFAULT_BLOCK_MAX_WEIGHT / 2)) {
        MempoolFeeEstimationResult empty_res;
        return empty_res;
    }
    return res;
}

void MemPoolPolicyEstimator::MempoolTransactionsRemovedForBlock(const std::vector<RemovedMempoolTransactionInfo>& txs_removed_for_block, const std::vector<CTransactionRef>& expected_block_txs,
                                                                const std::vector<CTransactionRef>& block_txs, unsigned int nBlockHeight)
{
    std::set<Txid> block_transactions;
    uint32_t block_weight = 0;
    for (const auto& tx : block_txs) {
        block_transactions.insert(tx->GetHash());
        block_weight += GetTransactionWeight(*tx);
    }

    uint32_t removed_expected_txs_weight = 0;
    for (const auto& tx : expected_block_txs) {
        if (block_transactions.contains(tx->GetHash())) {
            removed_expected_txs_weight += GetTransactionWeight(*tx);
        }
    }

    uint32_t removed_txs_weight = 0;
    for (const auto& tx : txs_removed_for_block) {
        removed_txs_weight += GetTransactionWeight(*(tx.info.m_tx));
    }

    // If Most of the transactions in the block were in our mempool.
    // And most of the transactions we expect to be in the block are in the block.
    // The node's mempool is roughly in sync with miner.
    const uint32_t mid_block_weight = block_weight / 2;
    bool roughly_synced = (removed_txs_weight > mid_block_weight) && (removed_expected_txs_weight > mid_block_weight);
    const MemPoolPolicyEstimator::block_info new_block_info = {nBlockHeight, roughly_synced};
    UpdateTopBlocks(new_block_info);
}

void MemPoolPolicyEstimator::UpdateTopBlocks(const MemPoolPolicyEstimator::block_info& new_blk_info)
{
    if (AreTopBlocksInOrder()) {
        InsertNewBlock(new_blk_info);
    } else {
        top_blocks = {new_blk_info, {0, false}, {0, false}};
    }
}

void MemPoolPolicyEstimator::InsertNewBlock(const MemPoolPolicyEstimator::block_info& new_blk_info)
{
    auto empty_block_it = std::find_if(top_blocks.begin(), top_blocks.end(), [](const auto& blk) {
        return blk.height == 0;
    });

    if (empty_block_it != top_blocks.end()) {
        if (std::prev(empty_block_it)->height + 1 == new_blk_info.height) {
            *empty_block_it = new_blk_info;
        } else {
            top_blocks = {new_blk_info, {0, false}, {0, false}};
        }

    } else if (top_blocks.back().height + 1 == new_blk_info.height) {
        std::rotate(top_blocks.begin(), top_blocks.begin() + 1, top_blocks.end());
        top_blocks.back() = new_blk_info;

    } else {
        top_blocks = {new_blk_info, {0, false}, {0, false}};
    }
}

bool MemPoolPolicyEstimator::AreTopBlocksInOrder() const
{
    unsigned int curr_height = top_blocks[0].height;
    if (curr_height == 0) {
        return true;
    }
    for (size_t i = 1; i < top_blocks.size(); ++i) {
        if (top_blocks[i].height == 0) {
            return true;
        }
        ++curr_height;
        if (curr_height != top_blocks[i].height) {
            return false;
        }
    }
    return true;
}

bool MemPoolPolicyEstimator::RoughlySynced() const
{
    return AreTopBlocksInOrder() && std::all_of(top_blocks.begin(), top_blocks.end(), [](const auto& blk) {
               return blk.roughly_synced;
           });
}
