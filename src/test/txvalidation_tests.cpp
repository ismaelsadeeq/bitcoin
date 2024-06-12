// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <key_io.h>
#include <policy/truc_policy.h>
#include <policy/packages.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <random.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>


BOOST_AUTO_TEST_SUITE(txvalidation_tests)

/**
 * Ensure that the mempool won't accept coinbase transactions.
 */
BOOST_FIXTURE_TEST_CASE(tx_mempool_reject_coinbase, TestChain100Setup)
{
    CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CMutableTransaction coinbaseTx;

    coinbaseTx.version = 1;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vout.resize(1);
    coinbaseTx.vin[0].scriptSig = CScript() << OP_11 << OP_EQUAL;
    coinbaseTx.vout[0].nValue = 1 * CENT;
    coinbaseTx.vout[0].scriptPubKey = scriptPubKey;

    BOOST_CHECK(CTransaction(coinbaseTx).IsCoinBase());

    LOCK(cs_main);

    unsigned int initialPoolSize = m_node.mempool->size();
    const MempoolAcceptResult result = m_node.chainman->ProcessTransaction(MakeTransactionRef(coinbaseTx));

    BOOST_CHECK(result.m_result_type == MempoolAcceptResult::ResultType::INVALID);

    // Check that the transaction hasn't been added to mempool.
    BOOST_CHECK_EQUAL(m_node.mempool->size(), initialPoolSize);

    // Check that the validation state reflects the unsuccessful attempt.
    BOOST_CHECK(result.m_state.IsInvalid());
    BOOST_CHECK_EQUAL(result.m_state.GetRejectReason(), "coinbase");
    BOOST_CHECK(result.m_state.GetResult() == TxValidationResult::TX_CONSENSUS);
}

// Generate a number of random, nonexistent outpoints.
static inline std::vector<COutPoint> random_outpoints(size_t num_outpoints) {
    std::vector<COutPoint> outpoints;
    for (size_t i{0}; i < num_outpoints; ++i) {
        outpoints.emplace_back(Txid::FromUint256(GetRandHash()), 0);
    }
    return outpoints;
}

static inline std::vector<CPubKey> random_keys(size_t num_keys) {
    std::vector<CPubKey> keys;
    keys.reserve(num_keys);
    for (size_t i{0}; i < num_keys; ++i) {
        CKey key;
        key.MakeNewKey(true);
        keys.emplace_back(key.GetPubKey());
    }
    return keys;
}

// Creates a placeholder tx (not valid) with 25 outputs. Specify the version and the inputs.
static inline CTransactionRef make_tx(const std::vector<COutPoint>& inputs, int32_t version)
{
    CMutableTransaction mtx = CMutableTransaction{};
    mtx.version = version;
    mtx.vin.resize(inputs.size());
    mtx.vout.resize(25);
    for (size_t i{0}; i < inputs.size(); ++i) {
        mtx.vin[i].prevout = inputs[i];
    }
    for (auto i{0}; i < 25; ++i) {
        mtx.vout[i].scriptPubKey = CScript() << OP_TRUE;
        mtx.vout[i].nValue = 10000;
    }
    return MakeTransactionRef(mtx);
}

BOOST_FIXTURE_TEST_CASE(version3_tests, RegTestingSetup)
{
    // Test TRUC policy helper functions
    CTxMemPool& pool = *Assert(m_node.mempool);
    LOCK2(cs_main, pool.cs);
    TestMemPoolEntryHelper entry;
    std::set<Txid> empty_conflicts_set;
    CTxMemPool::setEntries empty_ancestors;

    auto mempool_tx_truc = make_tx(random_outpoints(1), /*version=*/3);
    pool.addUnchecked(entry.FromTx(mempool_tx_truc));
    auto mempool_tx_non_truc = make_tx(random_outpoints(1), /*version=*/2);
    pool.addUnchecked(entry.FromTx(mempool_tx_non_truc));
    // Default values.
    CTxMemPool::Limits m_limits{};

    // Cannot spend from an unconfirmed TRUC transaction unless this tx is also TRUC.
    {
        // mempool_tx_truc
        //      ^
        // tx_non_truc_from_truc
        auto tx_non_truc_from_truc = make_tx({COutPoint{mempool_tx_truc->GetHash(), 0}}, /*version=*/2);
        auto ancestors_non_truc_from_truc{pool.CalculateMemPoolAncestors(entry.FromTx(tx_non_truc_from_truc), m_limits)};
        const auto expected_error_str{strprintf("non-TRUC tx %s (wtxid=%s) cannot spend from TRUC tx %s (wtxid=%s)",
            tx_non_truc_from_truc->GetHash().ToString(), tx_non_truc_from_truc->GetWitnessHash().ToString(),
            mempool_tx_truc->GetHash().ToString(), mempool_tx_truc->GetWitnessHash().ToString())};
        auto result_non_truc_from_truc{SingleTrucChecks(tx_non_truc_from_truc, *ancestors_non_truc_from_truc, empty_conflicts_set, GetVirtualTransactionSize(*tx_non_truc_from_truc))};
        BOOST_CHECK_EQUAL(result_non_truc_from_truc->first, expected_error_str);
        BOOST_CHECK_EQUAL(result_non_truc_from_truc->second, nullptr);

        Package package_truc_non_truc{mempool_tx_truc, tx_non_truc_from_truc};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_non_truc_from_truc, GetVirtualTransactionSize(*tx_non_truc_from_truc), package_truc_non_truc, empty_ancestors), expected_error_str);
        CTxMemPool::setEntries entries_mempool_truc{pool.GetIter(mempool_tx_truc->GetHash().ToUint256()).value()};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_non_truc_from_truc, GetVirtualTransactionSize(*tx_non_truc_from_truc), {tx_non_truc_from_truc}, entries_mempool_truc), expected_error_str);

        // mempool_tx_truc  mempool_tx_non_truc
        //            ^    ^
        //    tx_non_truc_from_non_truc_and_truc
        auto tx_non_truc_from_non_truc_and_truc = make_tx({COutPoint{mempool_tx_truc->GetHash(), 0}, COutPoint{mempool_tx_non_truc->GetHash(), 0}}, /*version=*/2);
        auto ancestors_non_truc_from_both{pool.CalculateMemPoolAncestors(entry.FromTx(tx_non_truc_from_non_truc_and_truc), m_limits)};
        const auto expected_error_str_2{strprintf("non-TRUC tx %s (wtxid=%s) cannot spend from TRUC tx %s (wtxid=%s)",
            tx_non_truc_from_non_truc_and_truc->GetHash().ToString(), tx_non_truc_from_non_truc_and_truc->GetWitnessHash().ToString(),
            mempool_tx_truc->GetHash().ToString(), mempool_tx_truc->GetWitnessHash().ToString())};
        auto result_non_truc_from_both{SingleTrucChecks(tx_non_truc_from_non_truc_and_truc, *ancestors_non_truc_from_both, empty_conflicts_set, GetVirtualTransactionSize(*tx_non_truc_from_non_truc_and_truc))};
        BOOST_CHECK_EQUAL(result_non_truc_from_both->first, expected_error_str_2);
        BOOST_CHECK_EQUAL(result_non_truc_from_both->second, nullptr);

        Package package_truc_non_truc_non_truc{mempool_tx_truc, mempool_tx_non_truc, tx_non_truc_from_non_truc_and_truc};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_non_truc_from_non_truc_and_truc, GetVirtualTransactionSize(*tx_non_truc_from_non_truc_and_truc), package_truc_non_truc_non_truc, empty_ancestors), expected_error_str_2);
    }

    // TRUC cannot spend from an unconfirmed non-TRUC transaction.
    {
        // mempool_tx_non_truc
        //      ^
        // tx_truc_from_non_truc
        auto tx_truc_from_non_truc = make_tx({COutPoint{mempool_tx_non_truc->GetHash(), 0}}, /*version=*/3);
        auto ancestors_truc_from_non_truc{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_from_non_truc), m_limits)};
        const auto expected_error_str{strprintf("TRUC tx %s (wtxid=%s) cannot spend from non-TRUC tx %s (wtxid=%s)",
            tx_truc_from_non_truc->GetHash().ToString(), tx_truc_from_non_truc->GetWitnessHash().ToString(),
            mempool_tx_non_truc->GetHash().ToString(), mempool_tx_non_truc->GetWitnessHash().ToString())};
        auto result_truc_from_non_truc{SingleTrucChecks(tx_truc_from_non_truc, *ancestors_truc_from_non_truc,  empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_from_non_truc))};
        BOOST_CHECK_EQUAL(result_truc_from_non_truc->first, expected_error_str);
        BOOST_CHECK_EQUAL(result_truc_from_non_truc->second, nullptr);

        Package package_non_truc_truc{mempool_tx_non_truc, tx_truc_from_non_truc};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_from_non_truc, GetVirtualTransactionSize(*tx_truc_from_non_truc), package_non_truc_truc, empty_ancestors), expected_error_str);
        CTxMemPool::setEntries entries_mempool_non_truc{pool.GetIter(mempool_tx_non_truc->GetHash().ToUint256()).value()};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_from_non_truc, GetVirtualTransactionSize(*tx_truc_from_non_truc), {tx_truc_from_non_truc}, entries_mempool_non_truc), expected_error_str);

        // mempool_tx_truc  mempool_tx_non_truc
        //            ^    ^
        //    tx_truc_from_non_truc_and_truc
        auto tx_truc_from_non_truc_and_truc = make_tx({COutPoint{mempool_tx_truc->GetHash(), 0}, COutPoint{mempool_tx_non_truc->GetHash(), 0}}, /*version=*/3);
        auto ancestors_truc_from_both{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_from_non_truc_and_truc), m_limits)};
        const auto expected_error_str_2{strprintf("TRUC tx %s (wtxid=%s) cannot spend from non-TRUC tx %s (wtxid=%s)",
            tx_truc_from_non_truc_and_truc->GetHash().ToString(), tx_truc_from_non_truc_and_truc->GetWitnessHash().ToString(),
            mempool_tx_non_truc->GetHash().ToString(), mempool_tx_non_truc->GetWitnessHash().ToString())};
        auto result_truc_from_both{SingleTrucChecks(tx_truc_from_non_truc_and_truc, *ancestors_truc_from_both, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_from_non_truc_and_truc))};
        BOOST_CHECK_EQUAL(result_truc_from_both->first, expected_error_str_2);
        BOOST_CHECK_EQUAL(result_truc_from_both->second, nullptr);

        // tx_truc_from_non_truc_and_truc also violates TRUC_ANCESTOR_LIMIT.
        const auto expected_error_str_3{strprintf("tx %s (wtxid=%s) would have too many ancestors",
            tx_truc_from_non_truc_and_truc->GetHash().ToString(), tx_truc_from_non_truc_and_truc->GetWitnessHash().ToString())};
        Package package_truc_non_truc_truc{mempool_tx_truc, mempool_tx_non_truc, tx_truc_from_non_truc_and_truc};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_from_non_truc_and_truc, GetVirtualTransactionSize(*tx_truc_from_non_truc_and_truc), package_truc_non_truc_truc, empty_ancestors), expected_error_str_3);
    }
    // TRUC from TRUC is ok, and non-TRUC from non-TRUC is ok.
    {
        // mempool_tx_truc
        //      ^
        // tx_truc_from_truc
        auto tx_truc_from_truc = make_tx({COutPoint{mempool_tx_truc->GetHash(), 0}}, /*version=*/3);
        auto ancestors_truc{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_from_truc), m_limits)};
        BOOST_CHECK(SingleTrucChecks(tx_truc_from_truc, *ancestors_truc, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_from_truc))
                    == std::nullopt);

        Package package_truc_truc{mempool_tx_truc, tx_truc_from_truc};
        BOOST_CHECK(PackageTrucChecks(tx_truc_from_truc, GetVirtualTransactionSize(*tx_truc_from_truc), package_truc_truc, empty_ancestors) == std::nullopt);

        // mempool_tx_non_truc
        //      ^
        // tx_non_truc_from_non_truc
        auto tx_non_truc_from_non_truc = make_tx({COutPoint{mempool_tx_non_truc->GetHash(), 0}}, /*version=*/2);
        auto ancestors_non_truc{pool.CalculateMemPoolAncestors(entry.FromTx(tx_non_truc_from_non_truc), m_limits)};
        BOOST_CHECK(SingleTrucChecks(tx_non_truc_from_non_truc, *ancestors_non_truc, empty_conflicts_set, GetVirtualTransactionSize(*tx_non_truc_from_non_truc))
                    == std::nullopt);

        Package package_non_truc_non_truc{mempool_tx_non_truc, tx_non_truc_from_non_truc};
        BOOST_CHECK(PackageTrucChecks(tx_non_truc_from_non_truc, GetVirtualTransactionSize(*tx_non_truc_from_non_truc), package_non_truc_non_truc, empty_ancestors) == std::nullopt);
    }

    // Tx spending TRUC cannot have too many mempool ancestors
    // Configuration where the tx has multiple direct parents.
    {
        Package package_multi_parents;
        std::vector<COutPoint> mempool_outpoints;
        mempool_outpoints.emplace_back(mempool_tx_truc->GetHash(), 0);
        package_multi_parents.emplace_back(mempool_tx_truc);
        for (size_t i{0}; i < 2; ++i) {
            auto mempool_tx = make_tx(random_outpoints(i + 1), /*version=*/3);
            pool.addUnchecked(entry.FromTx(mempool_tx));
            mempool_outpoints.emplace_back(mempool_tx->GetHash(), 0);
            package_multi_parents.emplace_back(mempool_tx);
        }
        auto tx_truc_multi_parent = make_tx(mempool_outpoints, /*version=*/3);
        package_multi_parents.emplace_back(tx_truc_multi_parent);
        auto ancestors{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_multi_parent), m_limits)};
        BOOST_CHECK_EQUAL(ancestors->size(), 3);
        const auto expected_error_str{strprintf("tx %s (wtxid=%s) would have too many ancestors",
            tx_truc_multi_parent->GetHash().ToString(), tx_truc_multi_parent->GetWitnessHash().ToString())};
        auto result{SingleTrucChecks(tx_truc_multi_parent, *ancestors, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_multi_parent))};
        BOOST_CHECK_EQUAL(result->first, expected_error_str);
        BOOST_CHECK_EQUAL(result->second, nullptr);

        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_multi_parent, GetVirtualTransactionSize(*tx_truc_multi_parent), package_multi_parents, empty_ancestors),
                          expected_error_str);
    }

    // Configuration where the tx is in a multi-generation chain.
    {
        Package package_multi_gen;
        CTransactionRef middle_tx;
        auto last_outpoint{random_outpoints(1)[0]};
        for (size_t i{0}; i < 2; ++i) {
            auto mempool_tx = make_tx({last_outpoint}, /*version=*/3);
            pool.addUnchecked(entry.FromTx(mempool_tx));
            last_outpoint = COutPoint{mempool_tx->GetHash(), 0};
            package_multi_gen.emplace_back(mempool_tx);
            if (i == 1) middle_tx = mempool_tx;
        }
        auto tx_truc_multi_gen = make_tx({last_outpoint}, /*version=*/3);
        package_multi_gen.emplace_back(tx_truc_multi_gen);
        auto ancestors{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_multi_gen), m_limits)};
        const auto expected_error_str{strprintf("tx %s (wtxid=%s) would have too many ancestors",
            tx_truc_multi_gen->GetHash().ToString(), tx_truc_multi_gen->GetWitnessHash().ToString())};
        auto result{SingleTrucChecks(tx_truc_multi_gen, *ancestors, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_multi_gen))};
        BOOST_CHECK_EQUAL(result->first, expected_error_str);
        BOOST_CHECK_EQUAL(result->second, nullptr);

        // Middle tx is what triggers a failure for the grandchild:
        BOOST_CHECK_EQUAL(*PackageTrucChecks(middle_tx, GetVirtualTransactionSize(*middle_tx), package_multi_gen, empty_ancestors), expected_error_str);
        BOOST_CHECK(PackageTrucChecks(tx_truc_multi_gen, GetVirtualTransactionSize(*tx_truc_multi_gen), package_multi_gen, empty_ancestors) == std::nullopt);
    }

    // Tx spending TRUC cannot be too large in virtual size.
    auto many_inputs{random_outpoints(100)};
    many_inputs.emplace_back(mempool_tx_truc->GetHash(), 0);
    {
        auto tx_truc_child_big = make_tx(many_inputs, /*version=*/3);
        const auto vsize{GetVirtualTransactionSize(*tx_truc_child_big)};
        auto ancestors{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_child_big), m_limits)};
        const auto expected_error_str{strprintf("TRUC child tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
            tx_truc_child_big->GetHash().ToString(), tx_truc_child_big->GetWitnessHash().ToString(), vsize, TRUC_CHILD_MAX_VSIZE)};
        auto result{SingleTrucChecks(tx_truc_child_big, *ancestors, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_child_big))};
        BOOST_CHECK_EQUAL(result->first, expected_error_str);
        BOOST_CHECK_EQUAL(result->second, nullptr);

        Package package_child_big{mempool_tx_truc, tx_truc_child_big};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_child_big, GetVirtualTransactionSize(*tx_truc_child_big), package_child_big, empty_ancestors),
                          expected_error_str);
    }

    // Tx spending TRUC cannot have too many sigops.
    // This child has 10 P2WSH multisig inputs.
    auto multisig_outpoints{random_outpoints(10)};
    multisig_outpoints.emplace_back(mempool_tx_truc->GetHash(), 0);
    auto keys{random_keys(2)};
    CScript script_multisig;
    script_multisig << OP_1;
    for (const auto& key : keys) {
        script_multisig << ToByteVector(key);
    }
    script_multisig << OP_2 << OP_CHECKMULTISIG;
    {
        CMutableTransaction mtx_many_sigops = CMutableTransaction{};
        mtx_many_sigops.version = TRUC_VERSION;
        for (const auto& outpoint : multisig_outpoints) {
            mtx_many_sigops.vin.emplace_back(outpoint);
            mtx_many_sigops.vin.back().scriptWitness.stack.emplace_back(script_multisig.begin(), script_multisig.end());
        }
        mtx_many_sigops.vout.resize(1);
        mtx_many_sigops.vout.back().scriptPubKey = CScript() << OP_TRUE;
        mtx_many_sigops.vout.back().nValue = 10000;
        auto tx_many_sigops{MakeTransactionRef(mtx_many_sigops)};

        auto ancestors{pool.CalculateMemPoolAncestors(entry.FromTx(tx_many_sigops), m_limits)};
        // legacy uses fAccurate = false, and the maximum number of multisig keys is used
        const int64_t total_sigops{static_cast<int64_t>(tx_many_sigops->vin.size()) * static_cast<int64_t>(script_multisig.GetSigOpCount(/*fAccurate=*/false))};
        BOOST_CHECK_EQUAL(total_sigops, tx_many_sigops->vin.size() * MAX_PUBKEYS_PER_MULTISIG);
        const int64_t bip141_vsize{GetVirtualTransactionSize(*tx_many_sigops)};
        // Weight limit is not reached...
        BOOST_CHECK(SingleTrucChecks(tx_many_sigops, *ancestors, empty_conflicts_set, bip141_vsize) == std::nullopt);
        // ...but sigop limit is.
        const auto expected_error_str{strprintf("TRUC child tx %s (wtxid=%s) is too big: %u > %u virtual bytes",
            tx_many_sigops->GetHash().ToString(), tx_many_sigops->GetWitnessHash().ToString(),
            total_sigops * DEFAULT_BYTES_PER_SIGOP / WITNESS_SCALE_FACTOR, TRUC_CHILD_MAX_VSIZE)};
        auto result{SingleTrucChecks(tx_many_sigops, *ancestors, empty_conflicts_set,
                                        GetVirtualTransactionSize(*tx_many_sigops, /*nSigOpCost=*/total_sigops, /*bytes_per_sigop=*/ DEFAULT_BYTES_PER_SIGOP))};
        BOOST_CHECK_EQUAL(result->first, expected_error_str);
        BOOST_CHECK_EQUAL(result->second, nullptr);

        Package package_child_sigops{mempool_tx_truc, tx_many_sigops};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_many_sigops, total_sigops * DEFAULT_BYTES_PER_SIGOP / WITNESS_SCALE_FACTOR, package_child_sigops, empty_ancestors),
                          expected_error_str);
    }

    // Parent + child with TRUC in the mempool. Child is allowed as long as it is under TRUC_CHILD_MAX_VSIZE.
    auto tx_mempool_truc_child = make_tx({COutPoint{mempool_tx_truc->GetHash(), 0}}, /*version=*/3);
    {
        BOOST_CHECK(GetTransactionWeight(*tx_mempool_truc_child) <= TRUC_CHILD_MAX_VSIZE * WITNESS_SCALE_FACTOR);
        auto ancestors{pool.CalculateMemPoolAncestors(entry.FromTx(tx_mempool_truc_child), m_limits)};
        BOOST_CHECK(SingleTrucChecks(tx_mempool_truc_child, *ancestors, empty_conflicts_set, GetVirtualTransactionSize(*tx_mempool_truc_child)) == std::nullopt);
        pool.addUnchecked(entry.FromTx(tx_mempool_truc_child));

        Package package_truc_1p1c{mempool_tx_truc, tx_mempool_truc_child};
        BOOST_CHECK(PackageTrucChecks(tx_mempool_truc_child, GetVirtualTransactionSize(*tx_mempool_truc_child), package_truc_1p1c, empty_ancestors) == std::nullopt);
    }

    // A TRUC transaction cannot have more than 1 descendant. Sibling is returned when exactly 1 exists.
    {
        auto tx_truc_child2 = make_tx({COutPoint{mempool_tx_truc->GetHash(), 1}}, /*version=*/3);

        // Configuration where parent already has 1 other child in mempool
        auto ancestors_1sibling{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_child2), m_limits)};
        const auto expected_error_str{strprintf("tx %s (wtxid=%s) would exceed descendant count limit",
            mempool_tx_truc->GetHash().ToString(), mempool_tx_truc->GetWitnessHash().ToString())};
        auto result_with_sibling_eviction{SingleTrucChecks(tx_truc_child2, *ancestors_1sibling, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_child2))};
        BOOST_CHECK_EQUAL(result_with_sibling_eviction->first, expected_error_str);
        // The other mempool child is returned to allow for sibling eviction.
        BOOST_CHECK_EQUAL(result_with_sibling_eviction->second, tx_mempool_truc_child);

        // If directly replacing the child, make sure there is no double-counting.
        BOOST_CHECK(SingleTrucChecks(tx_truc_child2, *ancestors_1sibling, {tx_mempool_truc_child->GetHash()}, GetVirtualTransactionSize(*tx_truc_child2))
                    == std::nullopt);

        Package package_truc_1p2c{mempool_tx_truc, tx_mempool_truc_child, tx_truc_child2};
        BOOST_CHECK_EQUAL(*PackageTrucChecks(tx_truc_child2, GetVirtualTransactionSize(*tx_truc_child2), package_truc_1p2c, empty_ancestors),
                          expected_error_str);

        // Configuration where parent already has 2 other children in mempool (no sibling eviction allowed). This may happen as the result of a reorg.
        pool.addUnchecked(entry.FromTx(tx_truc_child2));
        auto tx_truc_child3 = make_tx({COutPoint{mempool_tx_truc->GetHash(), 24}}, /*version=*/3);
        auto entry_mempool_parent = pool.GetIter(mempool_tx_truc->GetHash().ToUint256()).value();
        BOOST_CHECK_EQUAL(entry_mempool_parent->GetCountWithDescendants(), 3);
        auto ancestors_2siblings{pool.CalculateMemPoolAncestors(entry.FromTx(tx_truc_child3), m_limits)};

        auto result_2children{SingleTrucChecks(tx_truc_child3, *ancestors_2siblings, empty_conflicts_set, GetVirtualTransactionSize(*tx_truc_child3))};
        BOOST_CHECK_EQUAL(result_2children->first, expected_error_str);
        // The other mempool child is not returned because sibling eviction is not allowed.
        BOOST_CHECK_EQUAL(result_2children->second, nullptr);
    }

    // Sibling eviction: parent already has 1 other child, which also has its own child (no sibling eviction allowed). This may happen as the result of a reorg.
    {
        auto tx_mempool_grandparent = make_tx(random_outpoints(1), /*version=*/3);
        auto tx_mempool_sibling = make_tx({COutPoint{tx_mempool_grandparent->GetHash(), 0}}, /*version=*/3);
        auto tx_mempool_nibling = make_tx({COutPoint{tx_mempool_sibling->GetHash(), 0}}, /*version=*/3);
        auto tx_to_submit = make_tx({COutPoint{tx_mempool_grandparent->GetHash(), 1}}, /*version=*/3);

        pool.addUnchecked(entry.FromTx(tx_mempool_grandparent));
        pool.addUnchecked(entry.FromTx(tx_mempool_sibling));
        pool.addUnchecked(entry.FromTx(tx_mempool_nibling));

        auto ancestors_3gen{pool.CalculateMemPoolAncestors(entry.FromTx(tx_to_submit), m_limits)};
        const auto expected_error_str{strprintf("tx %s (wtxid=%s) would exceed descendant count limit",
            tx_mempool_grandparent->GetHash().ToString(), tx_mempool_grandparent->GetWitnessHash().ToString())};
        auto result_3gen{SingleTrucChecks(tx_to_submit, *ancestors_3gen, empty_conflicts_set, GetVirtualTransactionSize(*tx_to_submit))};
        BOOST_CHECK_EQUAL(result_3gen->first, expected_error_str);
        // The other mempool child is not returned because sibling eviction is not allowed.
        BOOST_CHECK_EQUAL(result_3gen->second, nullptr);
    }

    // Configuration where tx has multiple generations of descendants is not tested because that is
    // equivalent to the tx with multiple generations of ancestors.
}

BOOST_AUTO_TEST_SUITE_END()
