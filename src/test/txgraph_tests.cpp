// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <txgraph.h>

#include <random.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

BOOST_AUTO_TEST_SUITE(txgraph_tests)

namespace {

/** The number used as acceptable_cost argument in these tests. High enough that everything
 *  should be optimal, always. */
constexpr uint64_t HIGH_ACCEPTABLE_COST = 100'000'000;

std::strong_ordering PointerComparator(const TxGraph::Ref& a, const TxGraph::Ref& b) noexcept
{
    return (&a) <=> (&b);
}

std::strong_ordering ReversePointerComparator(const TxGraph::Ref& a, const TxGraph::Ref& b) noexcept
{
    return (&b) <=> (&a);
}

} // namespace

BOOST_AUTO_TEST_CASE(txgraph_trim_zigzag)
{
    // T     T     T     T     T     T     T     T     T     T     T     T     T     T (50 T's)
    //  \   / \   / \   / \   / \   / \   / \   / \   / \   / \   / \   / \   / \   /
    //   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /   \ /
    //    B     B     B     B     B     B     B     B     B     B     B     B     B    (49 B's)
    //
    /** The maximum cluster count used in this test. */
    static constexpr int MAX_CLUSTER_COUNT = 50;
    /** The number of "bottom" transactions, which are in the mempool already. */
    static constexpr int NUM_BOTTOM_TX = 49;
    /** The number of "top" transactions, which come from disconnected blocks. These are re-added
     *  to the mempool and, while connecting them to the already-in-mempool transactions, we
     *   discover the resulting cluster is oversized. */
    static constexpr int NUM_TOP_TX = 50;
    /** The total number of transactions in the test. */
    static constexpr int NUM_TOTAL_TX = NUM_BOTTOM_TX + NUM_TOP_TX;
    static_assert(NUM_TOTAL_TX > MAX_CLUSTER_COUNT);
    /** Set a very large cluster size limit so that only the count limit is triggered. */
    static constexpr int32_t MAX_CLUSTER_SIZE = 100'000 * 100;

    // Create a new graph for the test.
    auto graph = MakeTxGraph(MAX_CLUSTER_COUNT, MAX_CLUSTER_SIZE, HIGH_ACCEPTABLE_COST, PointerComparator);

    // Add all transactions and store their Refs.
    std::vector<TxGraph::Ref> refs;
    refs.reserve(NUM_TOTAL_TX);
    // First all bottom transactions: the i'th bottom transaction is at position i.
    for (unsigned int i = 0; i < NUM_BOTTOM_TX; ++i) {
        graph->AddTransaction(refs.emplace_back(), FeePerWeight{200 - i, 100});
    }
    // Then all top transactions: the i'th top transaction is at position NUM_BOTTOM_TX + i.
    for (unsigned int i = 0; i < NUM_TOP_TX; ++i) {
        graph->AddTransaction(refs.emplace_back(), FeePerWeight{100 - i, 100});
    }

    // Create the zigzag dependency structure.
    // Each transaction in the bottom row depends on two adjacent transactions from the top row.
    graph->SanityCheck();
    for (unsigned int i = 0; i < NUM_BOTTOM_TX; ++i) {
        graph->AddDependency(/*parent=*/refs[NUM_BOTTOM_TX + i], /*child=*/refs[i]);
        graph->AddDependency(/*parent=*/refs[NUM_BOTTOM_TX + i + 1], /*child=*/refs[i]);
    }

    // Check that the graph is now oversized. This also forces the graph to
    // group clusters and compute the oversized status.
    graph->SanityCheck();
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), NUM_TOTAL_TX);
    BOOST_CHECK(graph->IsOversized(TxGraph::Level::TOP));

    // Call Trim() to remove transactions and bring the cluster back within limits.
    auto removed_refs = graph->Trim();
    graph->SanityCheck();
    BOOST_CHECK(!graph->IsOversized(TxGraph::Level::TOP));

    // We only need to trim the middle bottom transaction to end up with 2 clusters each within cluster limits.
    BOOST_CHECK_EQUAL(removed_refs.size(), 1);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), MAX_CLUSTER_COUNT * 2 - 2);
    for (unsigned int i = 0; i < refs.size(); ++i) {
        BOOST_CHECK_EQUAL(graph->Exists(refs[i], TxGraph::Level::TOP), i != (NUM_BOTTOM_TX / 2));
    }
}

BOOST_AUTO_TEST_CASE(txgraph_trim_flower)
{
    // We will build an oversized flower-shaped graph: all transactions are spent by 1 descendant.
    //
    //   T   T   T   T   T   T   T   T (100 T's)
    //   |   |   |   |   |   |   |   |
    //   |   |   |   |   |   |   |   |
    //   \---+---+---+-+-+---+---+---/
    //                 |
    //                 B (1 B)
    //
    /** The maximum cluster count used in this test. */
    static constexpr int MAX_CLUSTER_COUNT = 50;
    /** The number of "top" transactions, which come from disconnected blocks. These are re-added
     *  to the mempool and, connecting them to the already-in-mempool transactions, we discover the
     *  resulting cluster is oversized. */
    static constexpr int NUM_TOP_TX = MAX_CLUSTER_COUNT * 2;
    /** The total number of transactions in this test. */
    static constexpr int NUM_TOTAL_TX = NUM_TOP_TX + 1;
    /** Set a very large cluster size limit so that only the count limit is triggered. */
    static constexpr int32_t MAX_CLUSTER_SIZE = 100'000 * 100;

    auto graph = MakeTxGraph(MAX_CLUSTER_COUNT, MAX_CLUSTER_SIZE, HIGH_ACCEPTABLE_COST, PointerComparator);

    // Add all transactions and store their Refs.
    std::vector<TxGraph::Ref> refs;
    refs.reserve(NUM_TOTAL_TX);

    // Add all transactions. They are in individual clusters.
    graph->AddTransaction(refs.emplace_back(), {1, 100});
    for (unsigned int i = 0; i < NUM_TOP_TX; ++i) {
        graph->AddTransaction(refs.emplace_back(), FeePerWeight{500 + i, 100});
    }
    graph->SanityCheck();

    // The 0th transaction spends all the top transactions.
    for (unsigned int i = 1; i < NUM_TOTAL_TX; ++i) {
        graph->AddDependency(/*parent=*/refs[i], /*child=*/refs[0]);
    }
    graph->SanityCheck();

    // Check that the graph is now oversized. This also forces the graph to
    // group clusters and compute the oversized status.
    BOOST_CHECK(graph->IsOversized(TxGraph::Level::TOP));

    // Call Trim() to remove transactions and bring the cluster back within limits.
    auto removed_refs = graph->Trim();
    graph->SanityCheck();
    BOOST_CHECK(!graph->IsOversized(TxGraph::Level::TOP));

    // Since only the bottom transaction connects these clusters, we only need to remove it.
    BOOST_CHECK_EQUAL(removed_refs.size(), 1);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), MAX_CLUSTER_COUNT * 2);
    BOOST_CHECK(!graph->Exists(refs[0], TxGraph::Level::TOP));
    for (unsigned int i = 1; i < refs.size(); ++i) {
        BOOST_CHECK(graph->Exists(refs[i], TxGraph::Level::TOP));
    }
}

BOOST_AUTO_TEST_CASE(txgraph_trim_huge)
{
    // The from-block transactions consist of 1000 fully linear clusters, each with 64
    // transactions. The mempool contains 11 transactions that together merge all of these into
    // a single cluster.
    //
    // (1000 chains of 64 transactions, 64000 T's total)
    //
    //      T          T          T          T          T          T          T          T
    //      |          |          |          |          |          |          |          |
    //      T          T          T          T          T          T          T          T
    //      |          |          |          |          |          |          |          |
    //      T          T          T          T          T          T          T          T
    //      |          |          |          |          |          |          |          |
    //      T          T          T          T          T          T          T          T
    //  (64 long)  (64 long)  (64 long)  (64 long)  (64 long)  (64 long)  (64 long)  (64 long)
    //      |          |          |          |          |          |          |          |
    //      |          |         / \         |         / \         |          |         /
    //      \----------+--------/   \--------+--------/   \--------+-----+----+--------/
    //                 |                     |                           |
    //                 B                     B                           B
    //
    //  (11 B's, each attaching to up to 100 chains of 64 T's)
    //
    /** The maximum cluster count used in this test. */
    static constexpr int MAX_CLUSTER_COUNT = 64;
    /** The number of "top" (from-block) chains of transactions. */
    static constexpr int NUM_TOP_CHAINS = 1000;
    /** The number of transactions per top chain. */
    static constexpr int NUM_TX_PER_TOP_CHAIN = MAX_CLUSTER_COUNT;
    /** The (maximum) number of dependencies per bottom transaction. */
    static constexpr int NUM_DEPS_PER_BOTTOM_TX = 100;
    /** The number of bottom transactions that are expected to be created. */
    static constexpr int NUM_BOTTOM_TX = (NUM_TOP_CHAINS - 1 + (NUM_DEPS_PER_BOTTOM_TX - 2)) / (NUM_DEPS_PER_BOTTOM_TX - 1);
    /** The total number of transactions created in this test. */
    static constexpr int NUM_TOTAL_TX = NUM_TOP_CHAINS * NUM_TX_PER_TOP_CHAIN + NUM_BOTTOM_TX;
    /** Set a very large cluster size limit so that only the count limit is triggered. */
    static constexpr int32_t MAX_CLUSTER_SIZE = 100'000 * 100;

    /** Refs to all top transactions. */
    std::vector<TxGraph::Ref> top_refs;
    /** Refs to all bottom transactions. */
    std::vector<TxGraph::Ref> bottom_refs;
    /** Indexes into top_refs for some transaction of each component, in arbitrary order.
     *  Initially these are the last transactions in each chains, but as bottom transactions are
     *  added, entries will be removed when they get merged, and randomized. */
    std::vector<size_t> top_components;

    FastRandomContext rng;
    auto graph = MakeTxGraph(MAX_CLUSTER_COUNT, MAX_CLUSTER_SIZE, HIGH_ACCEPTABLE_COST, PointerComparator);

    // Construct the top chains.
    for (int chain = 0; chain < NUM_TOP_CHAINS; ++chain) {
        for (int chaintx = 0; chaintx < NUM_TX_PER_TOP_CHAIN; ++chaintx) {
            // Use random fees, size 1.
            int64_t fee = rng.randbits<27>() + 100;
            FeePerWeight feerate{fee, 1};
            graph->AddTransaction(top_refs.emplace_back(), feerate);
            // Add internal dependencies linking the chain transactions together.
            if (chaintx > 0) {
                 graph->AddDependency(*(top_refs.rbegin()), *(top_refs.rbegin() + 1));
            }
        }
        // Remember the last transaction in each chain, to attach the bottom transactions to.
        top_components.push_back(top_refs.size() - 1);
    }
    graph->SanityCheck();

    // Not oversized so far (just 1000 clusters of 64).
    BOOST_CHECK(!graph->IsOversized(TxGraph::Level::TOP));

    // Construct the bottom transactions, and dependencies to the top chains.
    while (top_components.size() > 1) {
        // Construct the transaction.
        int64_t fee = rng.randbits<27>() + 100;
        FeePerWeight feerate{fee, 1};
        TxGraph::Ref bottom_tx;
        graph->AddTransaction(bottom_tx, feerate);
        // Determine the number of dependencies this transaction will have.
        int deps = std::min<int>(NUM_DEPS_PER_BOTTOM_TX, top_components.size());
        for (int dep = 0; dep < deps; ++dep) {
            // Pick an transaction in top_components to attach to.
            auto idx = rng.randrange(top_components.size());
            // Add dependency.
            graph->AddDependency(/*parent=*/top_refs[top_components[idx]], /*child=*/bottom_tx);
            // Unless this is the last dependency being added, remove from top_components, as
            // the component will be merged with that one.
            if (dep < deps - 1) {
                // Move entry top the back.
                if (idx != top_components.size() - 1) std::swap(top_components.back(), top_components[idx]);
                // And pop it.
                top_components.pop_back();
            }
        }
        bottom_refs.push_back(std::move(bottom_tx));
    }
    graph->SanityCheck();

    // Now we are oversized (one cluster of 64011).
    BOOST_CHECK(graph->IsOversized(TxGraph::Level::TOP));
    const auto total_tx_count = graph->GetTransactionCount(TxGraph::Level::TOP);
    BOOST_CHECK(total_tx_count == top_refs.size() + bottom_refs.size());
    BOOST_CHECK(total_tx_count == NUM_TOTAL_TX);

    // Call Trim() to remove transactions and bring the cluster back within limits.
    auto removed_refs = graph->Trim();
    BOOST_CHECK(!graph->IsOversized(TxGraph::Level::TOP));
    BOOST_CHECK(removed_refs.size() == total_tx_count - graph->GetTransactionCount(TxGraph::Level::TOP));
    graph->SanityCheck();

    // At least 99% of chains must survive.
    BOOST_CHECK(graph->GetTransactionCount(TxGraph::Level::TOP) >= (NUM_TOP_CHAINS * NUM_TX_PER_TOP_CHAIN * 99) / 100);
}

BOOST_AUTO_TEST_CASE(txgraph_trim_big_singletons)
{
    // Mempool consists of 100 singleton clusters; there are no dependencies. Some are oversized. Trim() should remove all of the oversized ones.
    static constexpr int MAX_CLUSTER_COUNT = 64;
    static constexpr int32_t MAX_CLUSTER_SIZE = 100'000;
    static constexpr int NUM_TOTAL_TX = 100;

    // Create a new graph for the test.
    auto graph = MakeTxGraph(MAX_CLUSTER_COUNT, MAX_CLUSTER_SIZE, HIGH_ACCEPTABLE_COST, PointerComparator);

    // Add all transactions and store their Refs.
    std::vector<TxGraph::Ref> refs;
    refs.reserve(NUM_TOTAL_TX);

    // Add all transactions. They are in individual clusters.
    for (unsigned int i = 0; i < NUM_TOTAL_TX; ++i) {
        // The 88th transaction is oversized.
        // Every 20th transaction is oversized.
        const FeePerWeight feerate{500 + i, (i == 88 || i % 20 == 0) ? MAX_CLUSTER_SIZE + 1 : 100};
        graph->AddTransaction(refs.emplace_back(), feerate);
    }
    graph->SanityCheck();

    // Check that the graph is now oversized. This also forces the graph to
    // group clusters and compute the oversized status.
    BOOST_CHECK(graph->IsOversized(TxGraph::Level::TOP));

    // Call Trim() to remove transactions and bring the cluster back within limits.
    auto removed_refs = graph->Trim();
    graph->SanityCheck();
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), NUM_TOTAL_TX - 6);
    BOOST_CHECK(!graph->IsOversized(TxGraph::Level::TOP));

    // Check that all the oversized transactions were removed.
    for (unsigned int i = 0; i < refs.size(); ++i) {
        BOOST_CHECK_EQUAL(graph->Exists(refs[i], TxGraph::Level::TOP), i != 88 && i % 20 != 0);
    }
}

BOOST_AUTO_TEST_CASE(txgraph_chunk_chain)
{
    // Create a new graph for the test.
    auto graph = MakeTxGraph(50, 1000, HIGH_ACCEPTABLE_COST, PointerComparator);

    auto block_builder_checker = [&graph](std::vector<std::vector<TxGraph::Ref*>> expected_chunks) {
        std::vector<std::vector<TxGraph::Ref*>> chunks;
        auto builder = graph->GetBlockBuilder();
        FeePerWeight last_chunk_feerate;
        while (auto chunk = builder->GetCurrentChunk()) {
            FeePerWeight sum;
            for (TxGraph::Ref* ref : chunk->first) {
                // The reported chunk feerate must match the chunk feerate obtained by asking
                // it for each of the chunk's transactions individually.
                BOOST_CHECK(graph->GetMainChunkFeerate(*ref) == chunk->second);
                // Verify the chunk feerate matches the sum of the reported individual feerates.
                sum += graph->GetIndividualFeerate(*ref);
            }
            BOOST_CHECK(sum == chunk->second);
            chunks.push_back(std::move(chunk->first));
            last_chunk_feerate = chunk->second;
            builder->Include();
        }

        BOOST_CHECK(chunks == expected_chunks);
        auto& last_chunk = chunks.back();
        // The last chunk returned by the BlockBuilder must match GetWorstMainChunk, in reverse.
        std::reverse(last_chunk.begin(), last_chunk.end());
        auto [worst_chunk, worst_chunk_feerate] = graph->GetWorstMainChunk();
        BOOST_CHECK(last_chunk == worst_chunk);
        BOOST_CHECK(last_chunk_feerate == worst_chunk_feerate);
    };

    std::vector<TxGraph::Ref> refs;
    refs.reserve(4);

    FeePerWeight feerateA{2, 10};
    FeePerWeight feerateB{1, 10};
    FeePerWeight feerateC{2, 10};
    FeePerWeight feerateD{4, 10};

    // everytime adding a transaction, test the chunk status
    // [A]
    graph->AddTransaction(refs.emplace_back(), feerateA);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 1);
    block_builder_checker({{&refs[0]}});
    // [A, B]
    graph->AddTransaction(refs.emplace_back(), feerateB);
    graph->AddDependency(/*parent=*/refs[0], /*child=*/refs[1]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 2);
    block_builder_checker({{&refs[0]}, {&refs[1]}});

    // [A, BC]
    graph->AddTransaction(refs.emplace_back(), feerateC);
    graph->AddDependency(/*parent=*/refs[1], /*child=*/refs[2]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 3);
    block_builder_checker({{&refs[0]}, {&refs[1], &refs[2]}});

    // [ABCD]
    graph->AddTransaction(refs.emplace_back(), feerateD);
    graph->AddDependency(/*parent=*/refs[2], /*child=*/refs[3]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 4);
    block_builder_checker({{&refs[0], &refs[1], &refs[2], &refs[3]}});

    graph->SanityCheck();

    // D->C->A
    graph->RemoveTransaction(refs[1]);
    // txgraph is not responsible for removing the descendants or ancestors
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 3);
    // only A remains there
    graph->RemoveTransaction(refs[2]);
    graph->RemoveTransaction(refs[3]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 1);
    block_builder_checker({{&refs[0]}});
}

BOOST_AUTO_TEST_CASE(txgraph_staging)
{
    /* Create a new graph for the test.
     * The parameters are max_cluster_count, max_cluster_size, acceptable_iters
     */
    auto graph = MakeTxGraph(10, 1000, HIGH_ACCEPTABLE_COST, PointerComparator);

    std::vector<TxGraph::Ref> refs;
    refs.reserve(2);

    FeePerWeight feerateA{2, 10};
    FeePerWeight feerateB{1, 10};

    // everytime adding a transaction, test the chunk status
    // [A]
    graph->AddTransaction(refs.emplace_back(), feerateA);
    BOOST_CHECK_EQUAL(graph->HaveStaging(), false);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 1);

    graph->StartStaging();
    BOOST_CHECK_EQUAL(graph->HaveStaging(), true);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 1);

    // [A, B]
    graph->AddTransaction(refs.emplace_back(), feerateB);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::MAIN), 1);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 2);
    BOOST_CHECK_EQUAL(graph->Exists(refs[0], TxGraph::Level::TOP), true);
    BOOST_CHECK_EQUAL(graph->Exists(refs[1], TxGraph::Level::TOP), true);

    graph->AddDependency(/*parent=*/refs[0], /*child=*/refs[1]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::MAIN), 1);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 2);

    graph->CommitStaging();
    BOOST_CHECK_EQUAL(graph->HaveStaging(), false);

    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::MAIN), 2);

    graph->StartStaging();

    // [A]
    graph->RemoveTransaction(refs[1]);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::MAIN), 2);
    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::TOP), 1);

    graph->CommitStaging();

    BOOST_CHECK_EQUAL(graph->GetTransactionCount(TxGraph::Level::MAIN), 1);

    graph->SanityCheck();
}

BOOST_AUTO_TEST_CASE(txgraph_chunk_fee_bounds)
{
    using Bounds = TxGraph::ChunkFeeBounds;
    auto graph = MakeTxGraph(50, 1000, HIGH_ACCEPTABLE_COST, PointerComparator);
    auto settle = [&]() { graph->GetWorstMainChunk(); graph->SanityCheck(); };

    const auto all_chunks_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{0, 1});
    {
        const auto bounds = graph->GetChunkFeeBounds(all_chunks_bounds_id);
        BOOST_CHECK_EQUAL(bounds.lower_fee, 0);
        BOOST_CHECK_EQUAL(bounds.upper_fee, 0);
        BOOST_CHECK_EQUAL(bounds.weight, 0);
    }
    {
        const size_t memory_with_bounds{graph->GetMainMemoryUsage()};
        const auto memory_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{0, 1});
        BOOST_CHECK_GT(graph->GetMainMemoryUsage(), memory_with_bounds);
        graph->StopTrackingChunkFeeBounds(memory_bounds_id);
        BOOST_CHECK_EQUAL(graph->GetMainMemoryUsage(), memory_with_bounds);
    }

    std::vector<TxGraph::Ref> refs;
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{1000, 100}); // A
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{500, 100});  // B
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{100, 100});  // C
    settle();

    {
        const auto bounds = graph->GetChunkFeeBounds(all_chunks_bounds_id);
        BOOST_CHECK_EQUAL(bounds.lower_fee, 1600);
        BOOST_CHECK_EQUAL(bounds.upper_fee, 1600);
        BOOST_CHECK_EQUAL(bounds.weight, 300);
    }

    const auto exact_weight_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/200, FeePerWeight{0, 1});
    const auto weight_limited_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/250, FeePerWeight{0, 1});
    const auto fee_floor_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{3, 1});
    {
        const auto exact_weight_bounds = graph->GetChunkFeeBounds(exact_weight_bounds_id);
        BOOST_CHECK_EQUAL(exact_weight_bounds.lower_fee, 1500);
        BOOST_CHECK_EQUAL(exact_weight_bounds.upper_fee, 1500);
        BOOST_CHECK_EQUAL(exact_weight_bounds.weight, 200);
        const auto weight_limited_bounds = graph->GetChunkFeeBounds(weight_limited_bounds_id);
        BOOST_CHECK_EQUAL(weight_limited_bounds.lower_fee, 1500);
        BOOST_CHECK_EQUAL(weight_limited_bounds.upper_fee, 1550);
        BOOST_CHECK_EQUAL(weight_limited_bounds.weight, 200);
        const auto fee_floor_bounds = graph->GetChunkFeeBounds(fee_floor_bounds_id);
        BOOST_CHECK_EQUAL(fee_floor_bounds.lower_fee, 1500);
        BOOST_CHECK_EQUAL(fee_floor_bounds.upper_fee, 1500);
        BOOST_CHECK_EQUAL(fee_floor_bounds.weight, 200);
    }

    // D sorts before the selected chunks, forcing the weight-limited selection to trim.
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{2000, 100}); // D
    settle();
    {
        const auto weight_limited_bounds = graph->GetChunkFeeBounds(weight_limited_bounds_id);
        BOOST_CHECK_EQUAL(weight_limited_bounds.lower_fee, 3000);
        BOOST_CHECK_EQUAL(weight_limited_bounds.upper_fee, 3250);
        BOOST_CHECK_EQUAL(weight_limited_bounds.weight, 200);
        const auto all_chunks_bounds = graph->GetChunkFeeBounds(all_chunks_bounds_id);
        BOOST_CHECK_EQUAL(all_chunks_bounds.lower_fee, 3600);
        BOOST_CHECK_EQUAL(all_chunks_bounds.upper_fee, 3600);
        BOOST_CHECK_EQUAL(all_chunks_bounds.weight, 400);
    }

    // Removing a selected chunk lets the selection advance to the next eligible chunk.
    graph->RemoveTransaction(refs[0]); // A
    settle();
    {
        const auto weight_limited_bounds = graph->GetChunkFeeBounds(weight_limited_bounds_id);
        BOOST_CHECK_EQUAL(weight_limited_bounds.lower_fee, 2500);
        BOOST_CHECK_EQUAL(weight_limited_bounds.upper_fee, 2550);
        BOOST_CHECK_EQUAL(weight_limited_bounds.weight, 200);
    }

    graph->StopTrackingChunkFeeBounds(fee_floor_bounds_id);
    graph->StopTrackingChunkFeeBounds(exact_weight_bounds_id);
    BOOST_CHECK_EQUAL(graph->GetChunkFeeBounds(weight_limited_bounds_id).lower_fee, 2500);
    BOOST_CHECK_EQUAL(graph->GetChunkFeeBounds(all_chunks_bounds_id).lower_fee, 2600);

    int calls{0};
    Bounds last{};
    const auto callback_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/250, FeePerWeight{0, 1},
                                                               [&](const Bounds& new_bounds) { ++calls; last = new_bounds; });
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{50, 100}); // E
    settle();
    BOOST_CHECK_EQUAL(calls, 0);
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{3000, 100}); // F
    settle();
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_EQUAL(last.lower_fee, 5000);
    BOOST_CHECK_EQUAL(last.upper_fee, 5250);
    BOOST_CHECK_EQUAL(last.weight, 200);
    graph->StopTrackingChunkFeeBounds(callback_bounds_id);

    int weight_calls{0};
    Bounds weight_last{};
    const auto weight_callback_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{0, 1},
                                                                      [&](const Bounds& new_bounds) { ++weight_calls; weight_last = new_bounds; });
    const auto weight_initial_bounds = graph->GetChunkFeeBounds(weight_callback_bounds_id);
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{0, 100}); // G
    settle();
    BOOST_CHECK_EQUAL(weight_calls, 1);
    BOOST_CHECK_EQUAL(weight_last.lower_fee, weight_initial_bounds.lower_fee);
    BOOST_CHECK_EQUAL(weight_last.upper_fee, weight_initial_bounds.upper_fee);
    BOOST_CHECK_EQUAL(weight_last.weight, 600);
    graph->StopTrackingChunkFeeBounds(weight_callback_bounds_id);

    int stop_tracking_calls{0};
    TxGraph::ChunkFeeBoundsId stopping_bounds_id{0};
    stopping_bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{0, 1},
                                                    [&](const Bounds&) {
                                                        ++stop_tracking_calls;
                                                        graph->StopTrackingChunkFeeBounds(stopping_bounds_id);
                                                    });
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{0, 100}); // H
    settle();
    BOOST_CHECK_EQUAL(stop_tracking_calls, 1);

    graph->SanityCheck();
}

BOOST_AUTO_TEST_CASE(txgraph_chunk_fee_bounds_equal_feerate_boundary)
{
    auto graph = MakeTxGraph(50, 1000, HIGH_ACCEPTABLE_COST, ReversePointerComparator);
    auto settle = [&]() { graph->GetWorstMainChunk(); graph->SanityCheck(); };

    std::vector<TxGraph::Ref> refs;
    refs.reserve(4);
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{100, 100}); // A
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{100, 100}); // B
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{100, 100}); // C
    settle();

    const auto bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/200, FeePerWeight{0, 1});
    {
        const auto bounds = graph->GetChunkFeeBounds(bounds_id);
        BOOST_CHECK_EQUAL(bounds.lower_fee, 200);
        BOOST_CHECK_EQUAL(bounds.upper_fee, 200);
        BOOST_CHECK_EQUAL(bounds.weight, 200);
    }

    graph->AddTransaction(refs.emplace_back(), FeePerWeight{100, 100}); // D
    settle();
    {
        const auto bounds = graph->GetChunkFeeBounds(bounds_id);
        BOOST_CHECK_EQUAL(bounds.lower_fee, 200);
        BOOST_CHECK_EQUAL(bounds.upper_fee, 200);
        BOOST_CHECK_EQUAL(bounds.weight, 200);
    }

    graph->SanityCheck();
}

BOOST_AUTO_TEST_CASE(txgraph_chunk_fee_bounds_getter_settles)
{
    auto graph = MakeTxGraph(50, 1000, HIGH_ACCEPTABLE_COST, PointerComparator);
    auto settle = [&]() { graph->GetWorstMainChunk(); graph->SanityCheck(); };

    std::vector<TxGraph::Ref> refs;
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{1000, 100});
    graph->AddTransaction(refs.emplace_back(), FeePerWeight{500, 100});
    settle();

    const auto bounds_id = graph->TrackChunkFeeBounds(/*max_weight=*/1000, FeePerWeight{0, 1});
    graph->RemoveTransaction(refs[0]);

    const auto bounds = graph->GetChunkFeeBounds(bounds_id);
    BOOST_CHECK_EQUAL(bounds.lower_fee, 500);
    BOOST_CHECK_EQUAL(bounds.upper_fee, 500);
    BOOST_CHECK_EQUAL(bounds.weight, 100);

    graph->SanityCheck();
}

BOOST_AUTO_TEST_SUITE_END()
