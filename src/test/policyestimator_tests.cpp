// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/mempool_entry.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <test/util/random.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validationinterface.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, ChainTestingSetup)

static inline CTransactionRef make_tx(const std::vector<COutPoint>& outpoints, int num_outputs = 1)
{
    CMutableTransaction tx;
    tx.vin.reserve(outpoints.size());
    tx.vout.resize(num_outputs);
    for (const auto& outpoint : outpoints) {
        tx.vin.push_back(CTxIn(outpoint));
    }

    for (int i = 0; i < num_outputs; ++i) {
        auto scriptPubKey = CScript() << OP_11 << OP_EQUAL;
        tx.vout.push_back(CTxOut(COIN, scriptPubKey));
    }
    return MakeTransactionRef(tx);
}

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CBlockPolicyEstimator& feeEst = *Assert(m_node.fee_estimator);
    CTxMemPool& mpool = *Assert(m_node.mempool);
    m_node.validation_signals->RegisterValidationInterface(&feeEst);
    TestMemPoolEntryHelper entry;
    CAmount basefee(2000);
    CAmount deltaFee(100);
    std::vector<CAmount> feeV;
    feeV.reserve(10);

    // Populate vectors of increasing fees
    for (int j = 0; j < 10; j++) {
        feeV.push_back(basefee * (j+1));
    }

    // Store the hashes of transactions that have been
    // added to the mempool by their associate fee
    // txHashes[j] is populated with transactions either of
    // fee = basefee * (j+1)
    std::vector<uint256> txHashes[10];

    // Create a transaction template
    CScript garbage;
    for (unsigned int i = 0; i < 128; i++)
        garbage.push_back('X');
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].scriptSig = garbage;
    tx.vout.resize(1);
    tx.vout[0].nValue=0LL;
    CFeeRate baseRate(basefee, GetVirtualTransactionSize(CTransaction(tx)));

    // Create a fake block
    std::vector<CTransactionRef> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At a decay .9952 and 4 fee transactions per block
    // This makes the tx count about 2.5 per bucket, well above the 0.1 threshold
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                {
                    LOCK2(cs_main, mpool.cs);
                    mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                    // Since TransactionAddedToMempool callbacks are generated in ATMP,
                    // not addUnchecked, we cheat and create one manually here
                    const int64_t virtual_size = GetVirtualTransactionSize(*MakeTransactionRef(tx));
                    const NewMempoolTransactionInfo tx_info{NewMempoolTransactionInfo(MakeTransactionRef(tx),
                                                                                      feeV[j],
                                                                                      virtual_size,
                                                                                      entry.nHeight,
                                                                                      /*mempool_limit_bypassed=*/false,
                                                                                      /*submitted_in_package=*/false,
                                                                                      /*chainstate_is_current=*/true,
                                                                                      /*has_no_mempool_parents=*/true)};
                    m_node.validation_signals->TransactionAddedToMempool(tx_info, mpool.GetAndIncrementSequence());
                }
                uint256 hash = tx.GetHash();
                txHashes[j].push_back(hash);
            }
        }
        //Create blocks where higher fee txs are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee transactions
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee transactions
            while (txHashes[9-h].size()) {
                CTransactionRef ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(ptx);
                txHashes[9-h].pop_back();
            }
        }

        {
            LOCK(mpool.cs);
            mpool.removeForBlock(block, ++blocknum);
        }

        block.clear();
        // Check after just a few txs that combining buckets works as expected
        if (blocknum == 3) {
            // Wait for fee estimator to catch up
            m_node.validation_signals->SyncWithValidationInterfaceQueue();
            // At this point we should need to combine 3 buckets to get enough data points
            // So estimateFee(1) should fail and estimateFee(2) should return somewhere around
            // 9*baserate.  estimateFee(2) %'s are 100,100,90 = average 97%
            BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() < 9*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(feeEst.estimateFee(2).GetFeePerK() > 9*baseRate.GetFeePerK() - deltaFee);
        }
    }

    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    std::vector<CAmount> origFeeEst;
    // Highest feerate is 10*baseRate and gets in all blocks,
    // second highest feerate is 9*baseRate and gets in 9/10 blocks = 90%,
    // third highest feerate is 8*base rate, and gets in 8/10 blocks = 80%,
    // so estimateFee(1) would return 10*baseRate but is hardcoded to return failure
    // Second highest feerate has 100% chance of being included by 2 blocks,
    // so estimateFee(2) should return 9*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
        if (i > 2) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        int mult = 11-i;
        if (i % 2 == 0) { //At scale 2, test logic is only correct for even targets
            BOOST_CHECK(origFeeEst[i-1] < mult*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(origFeeEst[i-1] > mult*baseRate.GetFeePerK() - deltaFee);
        }
    }
    // Fill out rest of the original estimates
    for (int i = 10; i <= 48; i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
    }

    // Mine 50 more blocks with no transactions happening, estimates shouldn't change
    // We haven't decayed the moving average enough so we still have enough data points in every bucket
    while (blocknum < 250) {
        LOCK(mpool.cs);
        mpool.removeForBlock(block, ++blocknum);
    }

    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] + deltaFee);
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }


    // Mine 15 more blocks with lots of transactions happening and not getting mined
    // Estimates should go up
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                {
                    LOCK2(cs_main, mpool.cs);
                    mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                    // Since TransactionAddedToMempool callbacks are generated in ATMP,
                    // not addUnchecked, we cheat and create one manually here
                    const int64_t virtual_size = GetVirtualTransactionSize(*MakeTransactionRef(tx));
                    const NewMempoolTransactionInfo tx_info{NewMempoolTransactionInfo(MakeTransactionRef(tx),
                                                                                      feeV[j],
                                                                                      virtual_size,
                                                                                      entry.nHeight,
                                                                                      /*mempool_limit_bypassed=*/false,
                                                                                      /*submitted_in_package=*/false,
                                                                                      /*chainstate_is_current=*/true,
                                                                                      /*has_no_mempool_parents=*/true)};
                    m_node.validation_signals->TransactionAddedToMempool(tx_info, mpool.GetAndIncrementSequence());
                }
                uint256 hash = tx.GetHash();
                txHashes[j].push_back(hash);
            }
        }
        {
            LOCK(mpool.cs);
            mpool.removeForBlock(block, ++blocknum);
        }
    }

    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    for (int i = 1; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine all those transactions
    // Estimates should still not be below original
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            CTransactionRef ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(ptx);
            txHashes[j].pop_back();
        }
    }

    {
        LOCK(mpool.cs);
        mpool.removeForBlock(block, 266);
    }
    block.clear();

    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 400 more blocks where everything is mined every block
    // Estimates should be below original estimates
    while (blocknum < 665) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee txs
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                {
                    LOCK2(cs_main, mpool.cs);
                    mpool.addUnchecked(entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                    // Since TransactionAddedToMempool callbacks are generated in ATMP,
                    // not addUnchecked, we cheat and create one manually here
                    const int64_t virtual_size = GetVirtualTransactionSize(*MakeTransactionRef(tx));
                    const NewMempoolTransactionInfo tx_info{NewMempoolTransactionInfo(MakeTransactionRef(tx),
                                                                                      feeV[j],
                                                                                      virtual_size,
                                                                                      entry.nHeight,
                                                                                      /*mempool_limit_bypassed=*/false,
                                                                                      /*submitted_in_package=*/false,
                                                                                      /*chainstate_is_current=*/true,
                                                                                      /*has_no_mempool_parents=*/true)};
                    m_node.validation_signals->TransactionAddedToMempool(tx_info, mpool.GetAndIncrementSequence());
                }
                uint256 hash = tx.GetHash();
                CTransactionRef ptx = mpool.get(hash);
                if (ptx)
                    block.push_back(ptx);

            }
        }

        {
            LOCK(mpool.cs);
            mpool.removeForBlock(block, ++blocknum);
        }

        block.clear();
    }
    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 9; i++) { // At 9, the original estimate was already at the bottom (b/c scale = 2)
        BOOST_CHECK(feeEst.estimateFee(i).GetFeePerK() < origFeeEst[i-1] - deltaFee);
    }
}

BOOST_AUTO_TEST_CASE(ComputingTxAncestorsAndDescendants)
{
    TestMemPoolEntryHelper entry;

    // Test 20 unique transactions
    {
        std::vector<RemovedMempoolTransactionInfo> transactions;
        transactions.reserve(20);

        for (auto i = 0; i < 20; ++i) {
            const std::vector<COutPoint> outpoints{COutPoint(Txid::FromUint256(InsecureRand256()), 0)};
            const CTransactionRef tx = make_tx(outpoints);
            transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx)));
        }

        const auto txAncestorsAndDescendants = GetTxAncestorsAndDescendants(transactions);
        BOOST_CHECK_EQUAL(txAncestorsAndDescendants.size(), transactions.size());

        for (auto& tx : transactions) {
            const Txid txid = tx.info.m_tx->GetHash();
            const auto txiter = txAncestorsAndDescendants.find(txid);
            BOOST_CHECK(txiter != txAncestorsAndDescendants.end());
            const auto ancestors = std::get<0>(txiter->second);
            const auto descendants = std::get<1>(txiter->second);
            BOOST_CHECK(ancestors.size() == 1);
            BOOST_CHECK(descendants.size() == 1);
        }
    }

    // Test 3 Linear transactions clusters
    /*
            Linear Packages
            A     B     C    D
            |     |     |    |
            E     H     J    K
            |     |
            F     I
            |
            G
    */

    {
        std::vector<RemovedMempoolTransactionInfo> transactions;
        transactions.reserve(11);

        // Create transaction A B C D
        for (auto i = 0; i < 4; ++i) {
            const std::vector<COutPoint> outpoints{COutPoint(Txid::FromUint256(InsecureRand256()), 0)};
            const CTransactionRef tx = make_tx(outpoints);
            transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx)));
        }

        // Create cluster A children ---> E->F->G
        std::vector<COutPoint> outpoints{COutPoint(transactions[0].info.m_tx->GetHash(), 0)};
        for (auto i = 0; i < 3; ++i) {
            const CTransactionRef tx = make_tx(outpoints);
            transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx)));
            outpoints = {COutPoint(tx->GetHash(), 0)};
        }

        // Create cluster B children ---> H->I
        outpoints = {COutPoint(transactions[1].info.m_tx->GetHash(), 0)};
        for (size_t i = 0; i < 2; ++i) {
            const CTransactionRef tx = make_tx(outpoints);
            transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx)));
            outpoints = {COutPoint(tx->GetHash(), 0)};
        }

        // Create cluster C child ---> J
        outpoints = {COutPoint(transactions[2].info.m_tx->GetHash(), 0)};
        const CTransactionRef txJ = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(txJ)));

        // Create cluster B child ---> K
        outpoints = {COutPoint(transactions[3].info.m_tx->GetHash(), 0)};
        const CTransactionRef txK = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(txK)));

        const auto txAncestorsAndDescendants = GetTxAncestorsAndDescendants(transactions);

        BOOST_CHECK_EQUAL(txAncestorsAndDescendants.size(), transactions.size());

        // Check tx A topology;
        {
            const Txid txA_Id = transactions[0].info.m_tx->GetHash();
            const auto txA_Iter = txAncestorsAndDescendants.find(txA_Id);
            BOOST_CHECK(txA_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txA_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // A
            BOOST_CHECK(ancestors.find(txA_Id) != ancestors.end());

            const auto descendants = std::get<1>(txA_Iter->second);
            BOOST_CHECK(descendants.size() == 4); // A, E, F, G
            BOOST_CHECK(descendants.find(txA_Id) != descendants.end());
            for (auto i = 4; i <= 6; i++) {
                auto curr_tx = transactions[i];
                BOOST_CHECK(descendants.find(curr_tx.info.m_tx->GetHash()) != descendants.end());
            }
        }

        // Check tx G topology;
        {
            const Txid txG_Id = transactions[6].info.m_tx->GetHash();
            const auto txG_Iter = txAncestorsAndDescendants.find(txG_Id);
            BOOST_CHECK(txG_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txG_Iter->second);
            BOOST_CHECK(ancestors.size() == 4); // G, A, E, F
            BOOST_CHECK(ancestors.find(txG_Id) != ancestors.end());
            auto txA_Id = transactions[0].info.m_tx->GetHash();
            BOOST_CHECK(ancestors.find(txA_Id) != ancestors.end());
            for (auto i = 4; i <= 6; i++) {
                auto curr_tx = transactions[i];
                BOOST_CHECK(ancestors.find(curr_tx.info.m_tx->GetHash()) != ancestors.end());
            }

            const auto descendants = std::get<1>(txG_Iter->second);
            BOOST_CHECK(descendants.size() == 1); // G
            BOOST_CHECK(descendants.find(txG_Id) != descendants.end());
        }

        // Check tx B topology;
        {
            const Txid txB_Id = transactions[1].info.m_tx->GetHash();
            const auto txB_Iter = txAncestorsAndDescendants.find(txB_Id);
            BOOST_CHECK(txB_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txB_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // B
            BOOST_CHECK(ancestors.find(txB_Id) != ancestors.end());

            const auto descendants = std::get<1>(txB_Iter->second);
            BOOST_CHECK(descendants.size() == 3); // B, H, I
            BOOST_CHECK(descendants.find(txB_Id) != descendants.end());
            for (auto i = 7; i <= 8; i++) {
                auto curr_tx = transactions[i];
                BOOST_CHECK(descendants.find(curr_tx.info.m_tx->GetHash()) != descendants.end());
            }
        }

        // Check tx H topology;
        {
            const Txid txH_Id = transactions[7].info.m_tx->GetHash();
            const auto txH_Iter = txAncestorsAndDescendants.find(txH_Id);
            BOOST_CHECK(txH_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txH_Iter->second);
            BOOST_CHECK(ancestors.size() == 2); // H, B
            BOOST_CHECK(ancestors.find(txH_Id) != ancestors.end());
            BOOST_CHECK(ancestors.find(transactions[1].info.m_tx->GetHash()) != ancestors.end());

            const auto descendants = std::get<1>(txH_Iter->second);
            BOOST_CHECK(descendants.size() == 2); // H, I
            BOOST_CHECK(descendants.find(txH_Id) != descendants.end());
            BOOST_CHECK(descendants.find(transactions[8].info.m_tx->GetHash()) != descendants.end());
        }

        // Check tx C topology;
        {
            const Txid txC_Id = transactions[2].info.m_tx->GetHash();
            const auto txC_Iter = txAncestorsAndDescendants.find(txC_Id);
            BOOST_CHECK(txC_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txC_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // C
            BOOST_CHECK(ancestors.find(txC_Id) != ancestors.end());

            const auto descendants = std::get<1>(txC_Iter->second);
            BOOST_CHECK(descendants.size() == 2); // C, J
            BOOST_CHECK(descendants.find(txC_Id) != descendants.end());
            BOOST_CHECK(descendants.find(transactions[9].info.m_tx->GetHash()) != descendants.end());
        }

        // Check tx D topology;
        {
            const Txid txD_Id = transactions[3].info.m_tx->GetHash();
            const auto txD_Iter = txAncestorsAndDescendants.find(txD_Id);
            BOOST_CHECK(txD_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txD_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // D
            BOOST_CHECK(ancestors.find(txD_Id) != ancestors.end());

            const auto descendants = std::get<1>(txD_Iter->second);
            BOOST_CHECK(descendants.size() == 2); // D, K
            BOOST_CHECK(ancestors.find(txD_Id) != ancestors.end());
            BOOST_CHECK(descendants.find(transactions[10].info.m_tx->GetHash()) != descendants.end());
        }
    }

    /* Unique transactions with a cluster of transactions

           Cluster A                      Cluster B
              A                               B
            /   \                           /   \
           /     \                         /     \
          C       D                       I       J
        /   \     |                               |
       /     \    |                               |
      E       F   H                               K
      \       /
       \     /
          G
    */
    {
        std::vector<RemovedMempoolTransactionInfo> transactions;
        transactions.reserve(11);

        // Create transaction A B
        for (auto i = 0; i < 2; ++i) {
            const std::vector<COutPoint> outpoints{COutPoint(Txid::FromUint256(InsecureRand256()), 0)};
            const CTransactionRef tx = make_tx(outpoints, /*num_outputs=*/2);
            transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx)));
        }

        // Cluster 1 Topology
        // Create a child for A ---> C
        std::vector<COutPoint> outpoints{COutPoint(transactions[0].info.m_tx->GetHash(), 0)};
        const CTransactionRef tx_C = make_tx(outpoints, /*num_outputs=*/2);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx_C)));

        // Create a child for A ---> D
        outpoints = {COutPoint(transactions[0].info.m_tx->GetHash(), 1)};
        const CTransactionRef tx_D = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx_D)));

        // Create a child for C ---> E
        outpoints = {COutPoint(tx_C->GetHash(), 0)};
        const CTransactionRef tx_E = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx_E)));

        // Create a child for C ---> F
        outpoints = {COutPoint(tx_C->GetHash(), 1)};
        const CTransactionRef tx_F = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx_F)));

        // Create a child for E and F  ---> G
        outpoints = {COutPoint(tx_E->GetHash(), 0), COutPoint(tx_F->GetHash(), 0)};
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(make_tx(outpoints))));

        // Create a child for D ---> H
        outpoints = {COutPoint(tx_D->GetHash(), 0)};
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(make_tx(outpoints))));


        // Cluster 2
        // Create a child for B ---> I
        outpoints = {COutPoint(transactions[1].info.m_tx->GetHash(), 0)};
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(make_tx(outpoints))));

        // Create a child for B ---> J
        outpoints = {COutPoint(transactions[1].info.m_tx->GetHash(), 1)};
        const CTransactionRef tx_J = make_tx(outpoints);
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(tx_J)));

        // Create a child for J ---> K
        outpoints = {COutPoint(tx_J->GetHash(), 0)};
        transactions.push_back(RemovedMempoolTransactionInfo(entry.FromTx(make_tx(outpoints))));

        const auto txAncestorsAndDescendants = GetTxAncestorsAndDescendants(transactions);

        BOOST_CHECK_EQUAL(txAncestorsAndDescendants.size(), transactions.size());

        // Check tx A topology;
        {
            const Txid txA_Id = transactions[0].info.m_tx->GetHash();
            const auto txA_Iter = txAncestorsAndDescendants.find(txA_Id);
            BOOST_CHECK(txA_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txA_Iter->second);
            const auto descendants = std::get<1>(txA_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // A
            BOOST_CHECK(ancestors.find(txA_Id) != ancestors.end());

            BOOST_CHECK(descendants.size() == 7); // A, C, D, E, F, G, H
            BOOST_CHECK(descendants.find(txA_Id) != descendants.end());
            for (auto i = 2; i <= 7; i++) {
                BOOST_CHECK(descendants.find(transactions[i].info.m_tx->GetHash()) != descendants.end());
            }
        }

        // Check tx C topology;
        {
            const Txid txC_Id = transactions[2].info.m_tx->GetHash();
            const auto txC_Iter = txAncestorsAndDescendants.find(txC_Id);
            BOOST_CHECK(txC_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txC_Iter->second);
            const auto descendants = std::get<1>(txC_Iter->second);
            BOOST_CHECK(ancestors.size() == 2); // C, A
            BOOST_CHECK(ancestors.find(txC_Id) != ancestors.end());
            BOOST_CHECK(ancestors.find(transactions[0].info.m_tx->GetHash()) != ancestors.end());

            BOOST_CHECK(descendants.size() == 4); // C, E, F, G
            BOOST_CHECK(descendants.find(txC_Id) != descendants.end());
            for (auto i = 4; i <= 6; i++) {
                BOOST_CHECK(descendants.find(transactions[i].info.m_tx->GetHash()) != descendants.end());
            }
        }

        // Check tx B topology;
        {
            const Txid txB_Id = transactions[1].info.m_tx->GetHash();
            const auto txB_Iter = txAncestorsAndDescendants.find(txB_Id);
            BOOST_CHECK(txB_Iter != txAncestorsAndDescendants.end());

            const auto ancestors = std::get<0>(txB_Iter->second);
            BOOST_CHECK(ancestors.size() == 1); // B
            BOOST_CHECK(ancestors.find(txB_Id) != ancestors.end());

            const auto descendants = std::get<1>(txB_Iter->second);
            BOOST_CHECK(descendants.size() == 4); // B, I, J, K
            BOOST_CHECK(descendants.find(txB_Id) != descendants.end());

            for (auto i = 8; i <= 10; i++) {
                BOOST_CHECK(descendants.find(transactions[i].info.m_tx->GetHash()) != descendants.end());
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
