// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <kernel/types.h>
#include <primitives/block.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/fees/block_policy_estimator_args.h>
#include <policy/policy.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validationinterface.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(policyestimator_tests, ChainTestingSetup)

BOOST_AUTO_TEST_CASE(BlockPolicyEstimates)
{
    CBlockPolicyEstimator feeEst{FeeestPath(*m_node.args), DEFAULT_ACCEPT_STALE_FEE_ESTIMATES};
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
    std::vector<Txid> txHashes[10];

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

    // Helper to simulate BlockConnected firing before mempool is updated,
    // matching the real validation interface ordering.
    const auto fire_block_connected{[&](int height) {
        CBlockIndex index;
        index.nHeight = height;
        m_node.validation_signals->BlockConnected(
            kernel::ChainstateRole{}, std::make_shared<CBlock>(), &index);
    }};

    // Create a fake block
    std::vector<CTransactionRef> block;
    int blocknum = 0;

    // Loop through 200 blocks
    // At a decay .9952 and 4 fee chunks per block
    // This makes the chunk count about 2.5 per bucket, well above the 0.1 threshold.
    // Each transaction is its own chunk since they have no dependencies.
    // MempoolUpdated signals are fired automatically by TryAddToMempool via the
    // changeset Apply path — the fee estimator learns about new chunks that way.
    while (blocknum < 200) {
        for (int j = 0; j < 10; j++) { // For each fee
            for (int k = 0; k < 4; k++) { // add 4 fee chunks
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k; // make transaction unique
                {
                    LOCK2(cs_main, mpool.cs);
                    TryAddToMempool(mpool, entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                }
                txHashes[j].push_back(tx.GetHash());
            }
        }
        // Create blocks where higher fee chunks are included more often
        for (int h = 0; h <= blocknum%10; h++) {
            // 10/10 blocks add highest fee chunks
            // 9/10 blocks add 2nd highest and so on until ...
            // 1/10 blocks add lowest fee chunks
            while (txHashes[9-h].size()) {
                CTransactionRef ptx = mpool.get(txHashes[9-h].back());
                if (ptx)
                    block.push_back(ptx);
                txHashes[9-h].pop_back();
            }
        }

        // Fire BlockConnected before removeForBlock, matching real validation
        // ordering where BlockConnected is emitted before mempool changes.
        fire_block_connected(blocknum + 1);

        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        {
            LOCK(mpool.cs);
            mpool.removeForBlock(block, ++blocknum);
        }

        block.clear();
        // Check after just a few chunks that combining buckets works as expected
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
    // so estimateFee(1) would return 10*baseRate but is hardcoded to return failure.
    // Second highest feerate has 100% chance of being included by 2 blocks,
    // so estimateFee(2) should return 9*baseRate etc...
    for (int i = 1; i < 10;i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
        if (i > 2) { // Fee estimates should be monotonically decreasing
            BOOST_CHECK(origFeeEst[i-1] <= origFeeEst[i-2]);
        }
        int mult = 11-i;
        if (i % 2 == 0) { // At scale 2, test logic is only correct for even targets
            BOOST_CHECK(origFeeEst[i-1] < mult*baseRate.GetFeePerK() + deltaFee);
            BOOST_CHECK(origFeeEst[i-1] > mult*baseRate.GetFeePerK() - deltaFee);
        }
    }
    // Fill out rest of the original estimates
    for (int i = 10; i <= 48; i++) {
        origFeeEst.push_back(feeEst.estimateFee(i).GetFeePerK());
    }

    // Mine 50 more blocks with no chunks happening, estimates shouldn't change.
    // We haven't decayed the moving average enough so we still have enough data points in every bucket.
    while (blocknum < 250) {
        fire_block_connected(blocknum + 1);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
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

    // Mine 15 more blocks with lots of chunks happening and not getting mined.
    // Estimates should go up.
    while (blocknum < 265) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee chunks
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                {
                    LOCK2(cs_main, mpool.cs);
                    TryAddToMempool(mpool, entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                }
                txHashes[j].push_back(tx.GetHash());
            }
        }
        fire_block_connected(blocknum + 1);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
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

    // Mine all those chunks.
    // Estimates should still not be below original.
    for (int j = 0; j < 10; j++) {
        while(txHashes[j].size()) {
            CTransactionRef ptx = mpool.get(txHashes[j].back());
            if (ptx)
                block.push_back(ptx);
            txHashes[j].pop_back();
        }
    }

    fire_block_connected(266);
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    {
        LOCK(mpool.cs);
        mpool.removeForBlock(block, 266);
    }
    block.clear();
    blocknum = 266;

    // Wait for fee estimator to catch up
    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    BOOST_CHECK(feeEst.estimateFee(1) == CFeeRate(0));
    for (int i = 2; i < 10;i++) {
        BOOST_CHECK(feeEst.estimateFee(i) == CFeeRate(0) || feeEst.estimateFee(i).GetFeePerK() > origFeeEst[i-1] - deltaFee);
    }

    // Mine 400 more blocks where everything is mined every block.
    // Estimates should be below original estimates.
    while (blocknum < 665) {
        for (int j = 0; j < 10; j++) { // For each fee multiple
            for (int k = 0; k < 4; k++) { // add 4 fee chunks
                tx.vin[0].prevout.n = 10000*blocknum+100*j+k;
                {
                    LOCK2(cs_main, mpool.cs);
                    TryAddToMempool(mpool, entry.Fee(feeV[j]).Time(Now<NodeSeconds>()).Height(blocknum).FromTx(tx));
                }
                CTransactionRef ptx = mpool.get(tx.GetHash());
                if (ptx)
                    block.push_back(ptx);
            }
        }

        fire_block_connected(blocknum + 1);
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
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

BOOST_AUTO_TEST_SUITE_END()
