// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <kernel/chainstatemanager_opts.h>
#include <kernel/mempool_removal_reason.h>
#include <kernel/types.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/fees/block_policy_estimator_args.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/mempool.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace {
const BasicTestingSetup* g_setup;

/** Exposes the protected BlockConnected callback for direct use in fuzz tests,
 *  matching the real validation ordering where BlockConnected fires before
 *  the mempool is updated. */
class FuzzablePolicyEstimator : public CBlockPolicyEstimator
{
public:
    using CBlockPolicyEstimator::CBlockPolicyEstimator;

    void SimulateBlockConnected(unsigned int height)
    {
        CBlockIndex index;
        index.nHeight = height;
        BlockConnected(kernel::ChainstateRole{}, std::make_shared<CBlock>(), &index);
    }
};
} // namespace

void initialize_policy_estimator()
{
    static const auto testing_setup = MakeNoLogFileContext<>();
    g_setup = testing_setup.get();
}

// Helper to build a MemPoolChunk from fuzzed data.
static MemPoolChunk ConsumeMemPoolChunk(FuzzedDataProvider& fuzzed_data_provider)
{
    const uint256 chunk_hash = ConsumeUInt256(fuzzed_data_provider);
    const CAmount fee = fuzzed_data_provider.ConsumeIntegralInRange<CAmount>(0, MAX_MONEY);
    const int32_t size = fuzzed_data_provider.ConsumeIntegralInRange<int32_t>(1, 400000);
    return MemPoolChunk{FeeFrac{fee, size}, chunk_hash};
}

FUZZ_TARGET(policy_estimator, .init = initialize_policy_estimator)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());
    bool good_data{true};

    FuzzablePolicyEstimator block_policy_estimator{FeeestPath(*g_setup->m_node.args), DEFAULT_ACCEPT_STALE_FEE_ESTIMATES};

    constexpr uint32_t MAX_HEIGHT{std::numeric_limits<uint32_t>::max() - 100};
    uint32_t current_height{0};

    LIMITED_WHILE(good_data && fuzzed_data_provider.ConsumeBool(), 10'000)
    {
        CallOneOf(
            fuzzed_data_provider,
            [&] {
                // Simulate a mempool change — new chunks added, old chunks removed.
                // Covers addition, RBF replacement, expiry, size limit, reorg, block connection.
                std::vector<MemPoolChunk> new_chunks;
                LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 100) {
                    new_chunks.push_back(ConsumeMemPoolChunk(fuzzed_data_provider));
                }
                std::vector<MemPoolChunk> old_chunks;
                LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 100) {
                    old_chunks.push_back(ConsumeMemPoolChunk(fuzzed_data_provider));
                }
                const auto reason = fuzzed_data_provider.PickValueInArray({
                    MemPoolRemovalReason::REPLACED,
                    MemPoolRemovalReason::EXPIRY,
                    MemPoolRemovalReason::SIZELIMIT,
                    MemPoolRemovalReason::REORG,
                    MemPoolRemovalReason::BLOCK,
                    MemPoolRemovalReason::CONFLICT,
                });
                std::optional<unsigned int> block_height;
                if (reason == MemPoolRemovalReason::BLOCK && current_height < MAX_HEIGHT) {
                    // Advance height and fire BlockConnected first, matching the
                    // real validation ordering where BlockConnected is emitted
                    // before mempool changes are applied.
                    current_height = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(
                        current_height + 1,
                        std::min(current_height + 100, MAX_HEIGHT));
                    block_policy_estimator.SimulateBlockConnected(current_height);
                    block_height = current_height;
                }
                block_policy_estimator.MempoolUpdated(
                    MemPoolChunksUpdate{old_chunks, new_chunks, reason, block_height});
            },
            [&] {
                // Simulate explicit chunk removal (e.g. prioritisation change).
                (void)block_policy_estimator.removeChunk(ConsumeUInt256(fuzzed_data_provider));
            },
            [&] {
                // Advance the chain without any mempool activity (e.g. empty block).
                if (current_height < MAX_HEIGHT) {
                    current_height = fuzzed_data_provider.ConsumeIntegralInRange<uint32_t>(
                        current_height + 1,
                        std::min(current_height + 100, MAX_HEIGHT));
                    block_policy_estimator.SimulateBlockConnected(current_height);
                }
            },
            [&] {
                block_policy_estimator.FlushUnconfirmed();
            });

        (void)block_policy_estimator.estimateFee(fuzzed_data_provider.ConsumeIntegral<int>());

        EstimationResult result;
        auto conf_target = fuzzed_data_provider.ConsumeIntegral<int>();
        auto success_threshold = fuzzed_data_provider.ConsumeFloatingPoint<double>();
        auto horizon = fuzzed_data_provider.PickValueInArray(ALL_FEE_ESTIMATE_HORIZONS);
        auto* result_ptr = fuzzed_data_provider.ConsumeBool() ? &result : nullptr;
        (void)block_policy_estimator.estimateRawFee(conf_target, success_threshold, horizon, result_ptr);

        FeeCalculation fee_calculation;
        conf_target = fuzzed_data_provider.ConsumeIntegral<int>();
        auto* fee_calc_ptr = fuzzed_data_provider.ConsumeBool() ? &fee_calculation : nullptr;
        auto conservative = fuzzed_data_provider.ConsumeBool();
        (void)block_policy_estimator.estimateSmartFee(conf_target, fee_calc_ptr, conservative);

        (void)block_policy_estimator.HighestTargetTracked(
            fuzzed_data_provider.PickValueInArray(ALL_FEE_ESTIMATE_HORIZONS));
    }
    {
        FuzzedFileProvider fuzzed_file_provider{fuzzed_data_provider};
        AutoFile fuzzed_auto_file{fuzzed_file_provider.open()};
        block_policy_estimator.Write(fuzzed_auto_file);
        block_policy_estimator.Read(fuzzed_auto_file);
        (void)fuzzed_auto_file.fclose();
    }
}
