// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/fees/block_policy_estimator.h>

#include <chain.h>
#include <common/system.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <kernel/mempool_entry.h>
#include <kernel/types.h>
#include <logging.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <primitives/block.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/serfloat.h>
#include <util/syserror.h>
#include <util/time.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

// The current format written, and the version required to read. Must be
// increased to at least 309900+1 on the next breaking change.
constexpr int CURRENT_FEES_FILE_VERSION{309900};

static constexpr double INF_FEERATE = 1e99;

std::string StringForFeeEstimateHorizon(FeeEstimateHorizon horizon)
{
    switch (horizon) {
    case FeeEstimateHorizon::SHORT_HALFLIFE: return "short";
    case FeeEstimateHorizon::MED_HALFLIFE: return "medium";
    case FeeEstimateHorizon::LONG_HALFLIFE: return "long";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

namespace {

struct EncodedDoubleFormatter
{
    template<typename Stream> void Ser(Stream &s, double v)
    {
        s << EncodeDouble(v);
    }

    template<typename Stream> void Unser(Stream& s, double& v)
    {
        uint64_t encoded;
        s >> encoded;
        v = DecodeDouble(encoded);
    }
};

} // namespace

/**
 * We will instantiate an instance of this class to track chunks that were
 * included in a block. We will lump chunks into a bucket according to their
 * approximate feerate and then track how long it took for those Chunks to be included in a block
 *
 * The tracking of unconfirmed (mempool) chunks is completely independent of the
 * historical tracking of chunks that have been confirmed in a block.
 */
class ChunkConfirmStats
{
private:
    //Define the buckets we will group chunks into
    const std::vector<double>& buckets;              // The upper-bound of the range for the bucket (inclusive)
    const std::map<double, unsigned int>& bucketMap; // Map of bucket upper-bound to index into all vectors by bucket

    // For each bucket X:
    // Count the total # of Chunks in each bucket
    // Track the historical moving average of this total over blocks
    std::vector<double> chunkCtAvg;

    // Count the total # of Chunks confirmed within Y blocks in each bucket
    // Track the historical moving average of these totals over blocks
    std::vector<std::vector<double>> confAvg; // confAvg[Y][X]

    // Track moving avg of Chunks which have been evicted from the mempool
    // after failing to be confirmed within Y blocks
    std::vector<std::vector<double>> failAvg; // failAvg[Y][X]

    // Sum the total feerate of all Chunk's in each bucket
    // Track the historical moving average of this total over blocks
    std::vector<double> m_feerate_avg;

    // Combine the conf counts with Chunk counts to calculate the confirmation % for each Y,X
    // Combine the total value with the Chunk counts to calculate the avg feerate per bucket

    double decay;

    // Resolution (# of blocks) with which confirmations are tracked
    unsigned int scale;

    // Mempool counts of outstanding chunks
    // For each bucket X, track the number of chunks in the mempool
    // that are unconfirmed for each possible confirmation value Y
    std::vector<std::vector<int> > unconfChunks;  //unconfChunks[Y][X]
    // chunks still unconfirmed after GetMaxConfirms for each bucket
    std::vector<int> oldUnconfChunks;

    void resizeInMemoryCounters(size_t newbuckets);

public:
    /**
     * Create new ChunkConfirmStats. This is called by BlockPolicyEstimator's
     * constructor with default values.
     * @param defaultBuckets contains the upper limits for the bucket boundaries
     * @param maxPeriods max number of periods to track
     * @param decay how much to decay the historical moving average per block
     */
    ChunkConfirmStats(const std::vector<double>& defaultBuckets, const std::map<double, unsigned int>& defaultBucketMap,
                   unsigned int maxPeriods, double decay, unsigned int scale);

    /** Roll the circular buffer for unconfirmed Chunks*/
    void ClearCurrent(unsigned int nBlockHeight);

    /**
     * Record a new chunk data point in the current block stats
     * @param blocksToConfirm the number of blocks it took this chunk to confirm
     * @param val the feerate of the chunk
     * @warning blocksToConfirm is 1-based and has to be >= 1
     */
    void Record(int blocksToConfirm, double val);

    /** Record a new chunk entering the mempool*/
    unsigned int NewChunk(unsigned int nBlockHeight, double val);

    /** Remove a chunk from mempool tracking stats*/
    void removeChunk(unsigned int entryHeight, unsigned int nBestSeenHeight,
                  unsigned int bucketIndex, bool inBlock);

    /** Update our estimates by decaying our historical moving average and updating
        with the data gathered from the current block */
    void UpdateMovingAverages();

    /**
     * Calculate a feerate estimate.  Find the lowest value bucket (or range of buckets
     * to make sure we have enough data points) whose chunks still have sufficient likelihood
     * of being confirmed within the target number of confirmations
     * @param confTarget target number of confirmations
     * @param sufficientChunkVal required average number of chunks per block in a bucket range
     * @param minSuccess the success probability we require
     * @param nBlockHeight the current block height
     */
    double EstimateMedianVal(int confTarget, double sufficientChunkVal,
                             double minSuccess, unsigned int nBlockHeight,
                             EstimationResult *result = nullptr) const;

    /** Return the max number of confirms we're tracking */
    unsigned int GetMaxConfirms() const { return scale * confAvg.size(); }

    /** Write state of estimation data to a file*/
    void Write(AutoFile& fileout) const;

    /**
     * Read saved state of estimation data from a file and replace all internal data structures and
     * variables with this state.
     */
    void Read(AutoFile& filein, size_t numBuckets);
};


ChunkConfirmStats::ChunkConfirmStats(const std::vector<double>& defaultBuckets,
                                const std::map<double, unsigned int>& defaultBucketMap,
                               unsigned int maxPeriods, double _decay, unsigned int _scale)
    : buckets(defaultBuckets), bucketMap(defaultBucketMap), decay(_decay), scale(_scale)
{
    assert(_scale != 0 && "_scale must be non-zero");
    confAvg.resize(maxPeriods);
    failAvg.resize(maxPeriods);
    for (unsigned int i = 0; i < maxPeriods; i++) {
        confAvg[i].resize(buckets.size());
        failAvg[i].resize(buckets.size());
    }

    chunkCtAvg.resize(buckets.size());
    m_feerate_avg.resize(buckets.size());

    resizeInMemoryCounters(buckets.size());
}

void ChunkConfirmStats::resizeInMemoryCounters(size_t newbuckets) {
    // newbuckets must be passed in because the buckets referred to during Read have not been updated yet.
    unconfChunks.resize(GetMaxConfirms());
    for (unsigned int i = 0; i < unconfChunks.size(); i++) {
        unconfChunks[i].resize(newbuckets);
    }
    oldUnconfChunks.resize(newbuckets);
}

// Roll the unconfirmed Chunks circular buffer
void ChunkConfirmStats::ClearCurrent(unsigned int nBlockHeight)
{
    for (unsigned int j = 0; j < buckets.size(); j++) {
        oldUnconfChunks[j] += unconfChunks[nBlockHeight % unconfChunks.size()][j];
        unconfChunks[nBlockHeight % unconfChunks.size()][j] = 0;
    }
}


void ChunkConfirmStats::Record(int blocksToConfirm, double feerate)
{
    // blocksToConfirm is 1-based
    if (blocksToConfirm < 1)
        return;
    int periodsToConfirm = (blocksToConfirm + scale - 1) / scale;
    unsigned int bucketindex = bucketMap.lower_bound(feerate)->second;
    for (size_t i = periodsToConfirm; i <= confAvg.size(); i++) {
        confAvg[i - 1][bucketindex]++;
    }
    chunkCtAvg[bucketindex]++;
    m_feerate_avg[bucketindex] += feerate;
}

void ChunkConfirmStats::UpdateMovingAverages()
{
    assert(confAvg.size() == failAvg.size());
    for (unsigned int j = 0; j < buckets.size(); j++) {
        for (unsigned int i = 0; i < confAvg.size(); i++) {
            confAvg[i][j] *= decay;
            failAvg[i][j] *= decay;
        }
        m_feerate_avg[j] *= decay;
        chunkCtAvg[j] *= decay;
    }
}

// returns -1 on error conditions
double ChunkConfirmStats::EstimateMedianVal(int confTarget, double sufficientChunkVal,
                                         double successBreakPoint, unsigned int nBlockHeight,
                                         EstimationResult *result) const
{
    // Counters for a bucket (or range of buckets)
    double nConf = 0; // Number of chunk's confirmed within the confTarget
    double totalNum = 0; // Total number of Chunk's that were ever confirmed
    int extraNum = 0;  // Number of Chunk's still in mempool for confTarget or longer
    double failNum = 0; // Number of Chunk's that were never confirmed but removed from the mempool after confTarget
    const int periodTarget = (confTarget + scale - 1) / scale;
    const int maxbucketindex = buckets.size() - 1;

    // We'll combine buckets until we have enough samples.
    // The near and far variables will define the range we've combined
    // The best variables are the last range we saw which still had a high
    // enough confirmation rate to count as success.
    // The cur variables are the current range we're counting.
    unsigned int curNearBucket = maxbucketindex;
    unsigned int bestNearBucket = maxbucketindex;
    unsigned int curFarBucket = maxbucketindex;
    unsigned int bestFarBucket = maxbucketindex;

    // We'll always group buckets into sets that meet sufficientChunkVal --
    // this ensures that we're using consistent groups between different
    // confirmation targets.
    double partialNum = 0;

    bool foundAnswer = false;
    unsigned int bins = unconfChunks.size();
    bool newBucketRange = true;
    bool passing = true;
    EstimatorBucket passBucket;
    EstimatorBucket failBucket;

    // Start counting from highest feerate chunks
    for (int bucket = maxbucketindex; bucket >= 0; --bucket) {
        if (newBucketRange) {
            curNearBucket = bucket;
            newBucketRange = false;
        }
        curFarBucket = bucket;
        nConf += confAvg[periodTarget - 1][bucket];
        partialNum += chunkCtAvg[bucket];
        totalNum += chunkCtAvg[bucket];
        failNum += failAvg[periodTarget - 1][bucket];
        for (unsigned int confct = confTarget; confct < GetMaxConfirms(); confct++)
            extraNum += unconfChunks[(nBlockHeight - confct) % bins][bucket];
        extraNum += oldUnconfChunks[bucket];
        // If we have enough data points in this range of buckets,
        // we can test for success
        // (Only count the confirmed data points, so that each confirmation count
        // will be looking at the same amount of data and same bucket breaks)

        if (partialNum < sufficientChunkVal / (1 - decay)) {
            // the buckets we've added in this round aren't sufficient
            // so keep adding
            continue;
        } else {
            partialNum = 0; // reset for the next range we'll add

            double curPct = nConf / (totalNum + failNum + extraNum);

            // Check to see if we are no longer getting confirmed at the success rate
            if (curPct < successBreakPoint) {
                if (passing == true) {
                    // First time we hit a failure record the failed bucket
                    unsigned int failMinBucket = std::min(curNearBucket, curFarBucket);
                    unsigned int failMaxBucket = std::max(curNearBucket, curFarBucket);
                    failBucket.start = failMinBucket ? buckets[failMinBucket - 1] : 0;
                    failBucket.end = buckets[failMaxBucket];
                    failBucket.withinTarget = nConf;
                    failBucket.totalConfirmed = totalNum;
                    failBucket.inMempool = extraNum;
                    failBucket.leftMempool = failNum;
                    passing = false;
                }
                continue;
            }
            // Otherwise update the cumulative stats, and the bucket variables
            // and reset the counters
            else {
                failBucket = EstimatorBucket(); // Reset any failed bucket, currently passing
                foundAnswer = true;
                passing = true;
                passBucket.withinTarget = nConf;
                nConf = 0;
                passBucket.totalConfirmed = totalNum;
                totalNum = 0;
                passBucket.inMempool = extraNum;
                passBucket.leftMempool = failNum;
                failNum = 0;
                extraNum = 0;
                bestNearBucket = curNearBucket;
                bestFarBucket = curFarBucket;
                newBucketRange = true;
            }
        }
    }

    double median = -1;
    double ChunkSum = 0;

    // Calculate the "average" feerate of the best bucket range that met success conditions
    // Find the bucket with the median chunk and then report the average feerate from that bucket
    // This is a compromise between finding the median which we can't since we don't save all Chunk's
    // and reporting the average which is less accurate
    unsigned int minBucket = std::min(bestNearBucket, bestFarBucket);
    unsigned int maxBucket = std::max(bestNearBucket, bestFarBucket);
    for (unsigned int j = minBucket; j <= maxBucket; j++) {
        ChunkSum += chunkCtAvg[j];
    }
    if (foundAnswer && ChunkSum != 0) {
        ChunkSum = ChunkSum / 2;
        for (unsigned int j = minBucket; j <= maxBucket; j++) {
            if (chunkCtAvg[j] < ChunkSum)
                ChunkSum -= chunkCtAvg[j];
            else { // we're in the right bucket
                median = m_feerate_avg[j] / chunkCtAvg[j];
                break;
            }
        }

        passBucket.start = minBucket ? buckets[minBucket-1] : 0;
        passBucket.end = buckets[maxBucket];
    }

    // If we were passing until we reached last few buckets with insufficient data, then report those as failed
    if (passing && !newBucketRange) {
        unsigned int failMinBucket = std::min(curNearBucket, curFarBucket);
        unsigned int failMaxBucket = std::max(curNearBucket, curFarBucket);
        failBucket.start = failMinBucket ? buckets[failMinBucket - 1] : 0;
        failBucket.end = buckets[failMaxBucket];
        failBucket.withinTarget = nConf;
        failBucket.totalConfirmed = totalNum;
        failBucket.inMempool = extraNum;
        failBucket.leftMempool = failNum;
    }

    float passed_within_target_perc = 0.0;
    float failed_within_target_perc = 0.0;
    if ((passBucket.totalConfirmed + passBucket.inMempool + passBucket.leftMempool)) {
        passed_within_target_perc = 100 * passBucket.withinTarget / (passBucket.totalConfirmed + passBucket.inMempool + passBucket.leftMempool);
    }
    if ((failBucket.totalConfirmed + failBucket.inMempool + failBucket.leftMempool)) {
        failed_within_target_perc = 100 * failBucket.withinTarget / (failBucket.totalConfirmed + failBucket.inMempool + failBucket.leftMempool);
    }

    LogDebug(BCLog::ESTIMATEFEE, "FeeEst: %d > %.0f%% decay %.5f: feerate: %g from (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out) Fail: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out)\n",
             confTarget, 100.0 * successBreakPoint, decay,
             median, passBucket.start, passBucket.end,
             passed_within_target_perc,
             passBucket.withinTarget, passBucket.totalConfirmed, passBucket.inMempool, passBucket.leftMempool,
             failBucket.start, failBucket.end,
             failed_within_target_perc,
             failBucket.withinTarget, failBucket.totalConfirmed, failBucket.inMempool, failBucket.leftMempool);


    if (result) {
        result->pass = passBucket;
        result->fail = failBucket;
        result->decay = decay;
        result->scale = scale;
    }
    return median;
}

void ChunkConfirmStats::Write(AutoFile& fileout) const
{
    fileout << Using<EncodedDoubleFormatter>(decay);
    fileout << scale;
    fileout << Using<VectorFormatter<EncodedDoubleFormatter>>(m_feerate_avg);
    fileout << Using<VectorFormatter<EncodedDoubleFormatter>>(chunkCtAvg);
    fileout << Using<VectorFormatter<VectorFormatter<EncodedDoubleFormatter>>>(confAvg);
    fileout << Using<VectorFormatter<VectorFormatter<EncodedDoubleFormatter>>>(failAvg);
}

void ChunkConfirmStats::Read(AutoFile& filein, size_t numBuckets)
{
    // Read data file and do some very basic sanity checking
    // buckets and bucketMap are not updated yet, so don't access them
    // If there is a read failure, we'll just discard this entire object anyway
    uint64_t maxConfirms, maxPeriods;

    // The current version will store the decay with each individual ChunkConfirmStats and also keep a scale factor
    filein >> Using<EncodedDoubleFormatter>(decay);
    if (decay <= 0 || decay >= 1) {
        throw std::runtime_error("Corrupt estimates file. Decay must be between 0 and 1 (non-inclusive)");
    }
    filein >> scale;
    if (scale == 0) {
        throw std::runtime_error("Corrupt estimates file. Scale must be non-zero");
    }

    filein >> Using<VectorFormatter<EncodedDoubleFormatter>>(m_feerate_avg);
    if (m_feerate_avg.size() != numBuckets) {
        throw std::runtime_error("Corrupt estimates file. Mismatch in feerate average bucket count");
    }
    filein >> Using<VectorFormatter<EncodedDoubleFormatter>>(chunkCtAvg);
    if (chunkCtAvg.size() != numBuckets) {
        throw std::runtime_error("Corrupt estimates file. Mismatch in Chunk count bucket count");
    }
    filein >> Using<VectorFormatter<VectorFormatter<EncodedDoubleFormatter>>>(confAvg);
    maxPeriods = confAvg.size();
    maxConfirms = scale * maxPeriods;

    if (maxConfirms <= 0 || maxConfirms > 6 * 24 * 7) { // one week
        throw std::runtime_error("Corrupt estimates file.  Must maintain estimates for between 1 and 1008 (one week) confirms");
    }
    for (unsigned int i = 0; i < maxPeriods; i++) {
        if (confAvg[i].size() != numBuckets) {
            throw std::runtime_error("Corrupt estimates file. Mismatch in feerate  conf average bucket count");
        }
    }

    filein >> Using<VectorFormatter<VectorFormatter<EncodedDoubleFormatter>>>(failAvg);
    if (maxPeriods != failAvg.size()) {
        throw std::runtime_error("Corrupt estimates file. Mismatch in confirms tracked for failures");
    }
    for (unsigned int i = 0; i < maxPeriods; i++) {
        if (failAvg[i].size() != numBuckets) {
            throw std::runtime_error("Corrupt estimates file. Mismatch in one of failure average bucket counts");
        }
    }

    // Resize the current block variables which aren't stored in the data file
    // to match the number of confirms and buckets
    resizeInMemoryCounters(numBuckets);

    LogDebug(BCLog::ESTIMATEFEE, "Reading estimates: %u buckets counting confirms up to %u blocks\n",
             numBuckets, maxConfirms);
}

unsigned int ChunkConfirmStats::NewChunk(unsigned int nBlockHeight, double val)
{
    unsigned int bucketindex = bucketMap.lower_bound(val)->second;
    unsigned int blockIndex = nBlockHeight % unconfChunks.size();
    unconfChunks[blockIndex][bucketindex]++;
    return bucketindex;
}

void ChunkConfirmStats::removeChunk(unsigned int entryHeight, unsigned int nBestSeenHeight, unsigned int bucketindex, bool inBlock)
{
    //nBestSeenHeight is not updated yet for the new block
    int blocksAgo = nBestSeenHeight - entryHeight;
    if (nBestSeenHeight == 0)  // the BlockPolicyEstimator hasn't seen any blocks yet
        blocksAgo = 0;
    if (blocksAgo < 0) {
        LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy error, blocks ago is negative for mempool Chunk\n");
        return;  //This can't happen because we call this with our best seen height, no entries can have higher
    }

    if (blocksAgo >= (int)unconfChunks.size()) {
        if (oldUnconfChunks[bucketindex] > 0) {
            oldUnconfChunks[bucketindex]--;
        } else {
            LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy error, mempool Chunk removed from >25 blocks,bucketIndex=%u already\n",
                     bucketindex);
        }
    }
    else {
        unsigned int blockIndex = entryHeight % unconfChunks.size();
        if (unconfChunks[blockIndex][bucketindex] > 0) {
            unconfChunks[blockIndex][bucketindex]--;
        } else {
            LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy error, mempool Chunk removed from blockIndex=%u,bucketIndex=%u already\n",
                     blockIndex, bucketindex);
        }
    }
    if (!inBlock && (unsigned int)blocksAgo >= scale) { // Only counts as a failure if not confirmed for entire period
        assert(scale != 0);
        unsigned int periodsAgo = blocksAgo / scale;
        for (size_t i = 0; i < periodsAgo && i < failAvg.size(); i++) {
            failAvg[i][bucketindex]++;
        }
    }
}

bool CBlockPolicyEstimator::removeChunk(const uint256& chunk_hash)
{
    LOCK(m_cs_fee_estimator);
    return _removeChunk(chunk_hash, /*inBlock=*/false);
}

bool CBlockPolicyEstimator::_removeChunk(const uint256& chunk_hash, bool inBlock)
{
    AssertLockHeld(m_cs_fee_estimator);
    std::map<uint256, ChunkStatsInfo>::iterator pos = mapMemPoolChunks.find(chunk_hash);
    if (pos != mapMemPoolChunks.end()) {
        feeStats->removeChunk(pos->second.blockHeight, nBestSeenHeight, pos->second.bucketIndex, inBlock);
        shortStats->removeChunk(pos->second.blockHeight, nBestSeenHeight, pos->second.bucketIndex, inBlock);
        longStats->removeChunk(pos->second.blockHeight, nBestSeenHeight, pos->second.bucketIndex, inBlock);
        mapMemPoolChunks.erase(pos);
        return true;
    } else {
        return false;
    }
}

CBlockPolicyEstimator::CBlockPolicyEstimator(const fs::path& estimation_filepath, const bool read_stale_estimates)
    : m_estimation_filepath{estimation_filepath}
{
    static_assert(MIN_BUCKET_FEERATE > 0, "Min feerate must be nonzero");
    size_t bucketIndex = 0;

    for (double bucketBoundary = MIN_BUCKET_FEERATE; bucketBoundary <= MAX_BUCKET_FEERATE; bucketBoundary *= FEE_SPACING, bucketIndex++) {
        buckets.push_back(bucketBoundary);
        bucketMap[bucketBoundary] = bucketIndex;
    }
    buckets.push_back(INF_FEERATE);
    bucketMap[INF_FEERATE] = bucketIndex;
    assert(bucketMap.size() == buckets.size());

    feeStats = std::unique_ptr<ChunkConfirmStats>(new ChunkConfirmStats(buckets, bucketMap, MED_BLOCK_PERIODS, MED_DECAY, MED_SCALE));
    shortStats = std::unique_ptr<ChunkConfirmStats>(new ChunkConfirmStats(buckets, bucketMap, SHORT_BLOCK_PERIODS, SHORT_DECAY, SHORT_SCALE));
    longStats = std::unique_ptr<ChunkConfirmStats>(new ChunkConfirmStats(buckets, bucketMap, LONG_BLOCK_PERIODS, LONG_DECAY, LONG_SCALE));

    AutoFile est_file{fsbridge::fopen(m_estimation_filepath, "rb")};

    if (est_file.IsNull()) {
        LogInfo("%s is not found. Continue anyway.", fs::PathToString(m_estimation_filepath));
        return;
    }

    std::chrono::hours file_age = GetFeeEstimatorFileAge();
    if (file_age > MAX_FILE_AGE && !read_stale_estimates) {
        LogWarning("Fee estimation file %s too old (age=%lld > %lld hours) and will not be used to avoid serving stale estimates.", fs::PathToString(m_estimation_filepath), Ticks<std::chrono::hours>(file_age), Ticks<std::chrono::hours>(MAX_FILE_AGE));
        return;
    }

    if (!Read(est_file)) {
        LogWarning("Failed to read fee estimates from %s. Continue anyway.", fs::PathToString(m_estimation_filepath));
    }
}

CBlockPolicyEstimator::~CBlockPolicyEstimator() = default;

void CBlockPolicyEstimator::BlockConnected(const kernel::ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    if (role.historical) return;
    LOCK(m_cs_fee_estimator);
    if (pindex->nHeight <= (int)nBestSeenHeight) return;
    nBestSeenHeight = pindex->nHeight;
    feeStats->ClearCurrent(nBestSeenHeight);
    shortStats->ClearCurrent(nBestSeenHeight);
    longStats->ClearCurrent(nBestSeenHeight);
    feeStats->UpdateMovingAverages();
    shortStats->UpdateMovingAverages();
    longStats->UpdateMovingAverages();
}

void CBlockPolicyEstimator::MempoolUpdated(const MemPoolChunksUpdate& mempool_chunks)
{
    LOCK(m_cs_fee_estimator);
    if (mempool_chunks.reason == MemPoolRemovalReason::BLOCK) {
        Assume(mempool_chunks.block_height.has_value());
        processBlockChunks(mempool_chunks.old_chunks, mempool_chunks.block_height.value());
        processNewMemPoolChunks(mempool_chunks.new_chunks);
    } else {
        processNewMemPoolChunks(mempool_chunks.new_chunks);
        for (const auto& chunk : mempool_chunks.old_chunks) {
            _removeChunk(chunk.m_chunk_hash, false);
        }
    }
}

void CBlockPolicyEstimator::processNewMemPoolChunks(const std::vector<MemPoolChunk>& chunks)
{
    AssertLockHeld(m_cs_fee_estimator);
    // Don't track chunks until we've seen at least one block. Without a known
    // best height, entry heights would be recorded as 0 and blocksToConfirm
    // would be wildly overstated when the chunk is eventually mined.
    for (const auto& chunk : chunks) {
        const auto& chunk_hash = chunk.m_chunk_hash;
        if (mapMemPoolChunks.contains(chunk_hash)) {
            LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy error mempool Chunk %s already being tracked\n",
                     chunk_hash.ToString());
            continue;
        }
        const CFeeRate feeRate(chunk.m_fee_rate.fee, (chunk.m_fee_rate.size + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR);
        mapMemPoolChunks[chunk_hash].blockHeight = nBestSeenHeight;
        unsigned int bucketIndex = feeStats->NewChunk(nBestSeenHeight, static_cast<double>(feeRate.GetFeePerK()));
        mapMemPoolChunks[chunk_hash].bucketIndex = bucketIndex;
        unsigned int bucketIndex2 = shortStats->NewChunk(nBestSeenHeight, static_cast<double>(feeRate.GetFeePerK()));
        assert(bucketIndex == bucketIndex2);
        unsigned int bucketIndex3 = longStats->NewChunk(nBestSeenHeight, static_cast<double>(feeRate.GetFeePerK()));
        assert(bucketIndex == bucketIndex3);
    }
}

void CBlockPolicyEstimator::processBlockChunks(const std::vector<MemPoolChunk>& block_chunks, unsigned int nBlockHeight)
{
    AssertLockHeld(m_cs_fee_estimator);
    unsigned int countedChunks = 0;
    for (const auto& chunk : block_chunks) {
        if (processBlockChunk(chunk, nBlockHeight))
            countedChunks++;
    }
    if (firstRecordedHeight == 0 && countedChunks > 0) {
        firstRecordedHeight = nBestSeenHeight;
        LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy first recorded height %u\n", firstRecordedHeight);
    }
    LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy estimates updated by %u of %u block chunks, mempool map size %u, max target %u from %s\n",
             countedChunks, block_chunks.size(), mapMemPoolChunks.size(),
             MaxUsableEstimate(), HistoricalBlockSpan() > BlockSpan() ? "historical" : "current");
}

bool CBlockPolicyEstimator::processBlockChunk(const MemPoolChunk& chunk, unsigned int nBlockHeight)
{
    AssertLockHeld(m_cs_fee_estimator);
    auto it = mapMemPoolChunks.find(chunk.m_chunk_hash);
    if (it == mapMemPoolChunks.end()) return false;

    int blocksToConfirm = nBlockHeight - it->second.blockHeight;
    if (blocksToConfirm <= 0) {
        LogDebug(BCLog::ESTIMATEFEE, "Blockpolicy error chunk had negative blocksToConfirm\n");
        return false;
    }
    CFeeRate feeRate(chunk.m_fee_rate.fee, (chunk.m_fee_rate.size + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR);
    feeStats->Record(blocksToConfirm, static_cast<double>(feeRate.GetFeePerK()));
    shortStats->Record(blocksToConfirm, static_cast<double>(feeRate.GetFeePerK()));
    longStats->Record(blocksToConfirm, static_cast<double>(feeRate.GetFeePerK()));
    _removeChunk(chunk.m_chunk_hash, /*inBlock=*/true);
    return true;
}

CFeeRate CBlockPolicyEstimator::estimateFee(int confTarget) const
{
    // It's not possible to get reasonable estimates for confTarget of 1
    if (confTarget <= 1)
        return CFeeRate(0);

    return estimateRawFee(confTarget, DOUBLE_SUCCESS_PCT, FeeEstimateHorizon::MED_HALFLIFE);
}

CFeeRate CBlockPolicyEstimator::estimateRawFee(int confTarget, double successThreshold, FeeEstimateHorizon horizon, EstimationResult* result) const
{
    ChunkConfirmStats* stats = nullptr;
    double sufficientChunks = SUFFICIENT_FEE_CHUNKS;
    switch (horizon) {
    case FeeEstimateHorizon::SHORT_HALFLIFE: {
        stats = shortStats.get();
        sufficientChunks = SUFFICIENT_CHUNKS_SHORT;
        break;
    }
    case FeeEstimateHorizon::MED_HALFLIFE: {
        stats = feeStats.get();
        break;
    }
    case FeeEstimateHorizon::LONG_HALFLIFE: {
        stats = longStats.get();
        break;
    }
    } // no default case, so the compiler can warn about missing cases
    assert(stats);

    LOCK(m_cs_fee_estimator);
    // Return failure if trying to analyze a target we're not tracking
    if (confTarget <= 0 || (unsigned int)confTarget > stats->GetMaxConfirms())
        return CFeeRate(0);
    if (successThreshold > 1)
        return CFeeRate(0);

    double median = stats->EstimateMedianVal(confTarget, sufficientChunks, successThreshold, nBestSeenHeight, result);

    if (median < 0)
        return CFeeRate(0);

    return CFeeRate(llround(median));
}

unsigned int CBlockPolicyEstimator::HighestTargetTracked(FeeEstimateHorizon horizon) const
{
    LOCK(m_cs_fee_estimator);
    switch (horizon) {
    case FeeEstimateHorizon::SHORT_HALFLIFE: {
        return shortStats->GetMaxConfirms();
    }
    case FeeEstimateHorizon::MED_HALFLIFE: {
        return feeStats->GetMaxConfirms();
    }
    case FeeEstimateHorizon::LONG_HALFLIFE: {
        return longStats->GetMaxConfirms();
    }
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

unsigned int CBlockPolicyEstimator::BlockSpan() const
{
    if (firstRecordedHeight == 0) return 0;
    assert(nBestSeenHeight >= firstRecordedHeight);

    return nBestSeenHeight - firstRecordedHeight;
}

unsigned int CBlockPolicyEstimator::HistoricalBlockSpan() const
{
    if (historicalFirst == 0) return 0;
    assert(historicalBest >= historicalFirst);

    if (nBestSeenHeight - historicalBest > OLDEST_ESTIMATE_HISTORY) return 0;

    return historicalBest - historicalFirst;
}

unsigned int CBlockPolicyEstimator::MaxUsableEstimate() const
{
    // Block spans are divided by 2 to make sure there are enough potential failing data points for the estimate
    return std::min(longStats->GetMaxConfirms(), std::max(BlockSpan(), HistoricalBlockSpan()) / 2);
}

/** Return a fee estimate at the required successThreshold from the shortest
 * time horizon which tracks confirmations up to the desired target.  If
 * checkShorterHorizon is requested, also allow short time horizon estimates
 * for a lower target to reduce the given answer */
double CBlockPolicyEstimator::estimateCombinedFee(unsigned int confTarget, double successThreshold, bool checkShorterHorizon, EstimationResult *result) const
{
    double estimate = -1;
    if (confTarget >= 1 && confTarget <= longStats->GetMaxConfirms()) {
        // Find estimate from shortest time horizon possible
        if (confTarget <= shortStats->GetMaxConfirms()) { // short horizon
            estimate = shortStats->EstimateMedianVal(confTarget, SUFFICIENT_CHUNKS_SHORT, successThreshold, nBestSeenHeight, result);
        }
        else if (confTarget <= feeStats->GetMaxConfirms()) { // medium horizon
            estimate = feeStats->EstimateMedianVal(confTarget, SUFFICIENT_FEE_CHUNKS, successThreshold, nBestSeenHeight, result);
        }
        else { // long horizon
            estimate = longStats->EstimateMedianVal(confTarget, SUFFICIENT_FEE_CHUNKS, successThreshold, nBestSeenHeight, result);
        }
        if (checkShorterHorizon) {
            EstimationResult tempResult;
            // If a lower confTarget from a more recent horizon returns a lower answer use it.
            if (confTarget > feeStats->GetMaxConfirms()) {
                double medMax = feeStats->EstimateMedianVal(feeStats->GetMaxConfirms(), SUFFICIENT_FEE_CHUNKS, successThreshold, nBestSeenHeight, &tempResult);
                if (medMax > 0 && (estimate == -1 || medMax < estimate)) {
                    estimate = medMax;
                    if (result) *result = tempResult;
                }
            }
            if (confTarget > shortStats->GetMaxConfirms()) {
                double shortMax = shortStats->EstimateMedianVal(shortStats->GetMaxConfirms(), SUFFICIENT_CHUNKS_SHORT, successThreshold, nBestSeenHeight, &tempResult);
                if (shortMax > 0 && (estimate == -1 || shortMax < estimate)) {
                    estimate = shortMax;
                    if (result) *result = tempResult;
                }
            }
        }
    }
    return estimate;
}

/** Ensure that for a conservative estimate, the DOUBLE_SUCCESS_PCT is also met
 * at 2 * target for any longer time horizons.
 */
double CBlockPolicyEstimator::estimateConservativeFee(unsigned int doubleTarget, EstimationResult *result) const
{
    double estimate = -1;
    EstimationResult tempResult;
    if (doubleTarget <= shortStats->GetMaxConfirms()) {
        estimate = feeStats->EstimateMedianVal(doubleTarget, SUFFICIENT_FEE_CHUNKS, DOUBLE_SUCCESS_PCT, nBestSeenHeight, result);
    }
    if (doubleTarget <= feeStats->GetMaxConfirms()) {
        double longEstimate = longStats->EstimateMedianVal(doubleTarget, SUFFICIENT_FEE_CHUNKS, DOUBLE_SUCCESS_PCT, nBestSeenHeight, &tempResult);
        if (longEstimate > estimate) {
            estimate = longEstimate;
            if (result) *result = tempResult;
        }
    }
    return estimate;
}

/** estimateSmartFee returns the max of the feerates calculated with a 60%
 * threshold required at target / 2, an 85% threshold required at target and a
 * 95% threshold required at 2 * target.  Each calculation is performed at the
 * shortest time horizon which tracks the required target.  Conservative
 * estimates, however, required the 95% threshold at 2 * target be met for any
 * longer time horizons also.
 */
CFeeRate CBlockPolicyEstimator::estimateSmartFee(int confTarget, FeeCalculation *feeCalc, bool conservative) const
{
    LOCK(m_cs_fee_estimator);

    if (feeCalc) {
        feeCalc->desiredTarget = confTarget;
        feeCalc->returnedTarget = confTarget;
        feeCalc->best_height = nBestSeenHeight;
    }

    double median = -1;
    EstimationResult tempResult;

    // Return failure if trying to analyze a target we're not tracking
    if (confTarget <= 0 || (unsigned int)confTarget > longStats->GetMaxConfirms()) {
        return CFeeRate(0);  // error condition
    }

    // It's not possible to get reasonable estimates for confTarget of 1
    if (confTarget == 1) confTarget = 2;

    unsigned int maxUsableEstimate = MaxUsableEstimate();
    if ((unsigned int)confTarget > maxUsableEstimate) {
        confTarget = maxUsableEstimate;
    }
    if (feeCalc) feeCalc->returnedTarget = confTarget;

    if (confTarget <= 1) return CFeeRate(0); // error condition

    assert(confTarget > 0); //estimateCombinedFee and estimateConservativeFee take unsigned ints
    /** true is passed to estimateCombined fee for target/2 and target so
     * that we check the max confirms for shorter time horizons as well.
     * This is necessary to preserve monotonically increasing estimates.
     * For non-conservative estimates we do the same thing for 2*target, but
     * for conservative estimates we want to skip these shorter horizons
     * checks for 2*target because we are taking the max over all time
     * horizons so we already have monotonically increasing estimates and
     * the purpose of conservative estimates is not to let short term
     * fluctuations lower our estimates by too much.
     *
     * Note: In certain rare edge cases, monotonically increasing estimates may
     * not be guaranteed. Specifically, given two targets N and M, where M > N,
     * if a sub-estimate for target N fails to return a valid fee rate, while
     * target M has valid fee rate for that sub-estimate, target M may result
     * in a higher fee rate estimate than target N.
     *
     * See: https://github.com/bitcoin/bitcoin/issues/11800#issuecomment-349697807
     */
    double halfEst = estimateCombinedFee(confTarget/2, HALF_SUCCESS_PCT, true, &tempResult);
    if (feeCalc) {
        feeCalc->est = tempResult;
        feeCalc->reason = FeeReason::HALF_ESTIMATE;
    }
    median = halfEst;
    double actualEst = estimateCombinedFee(confTarget, SUCCESS_PCT, true, &tempResult);
    if (actualEst > median) {
        median = actualEst;
        if (feeCalc) {
            feeCalc->est = tempResult;
            feeCalc->reason = FeeReason::FULL_ESTIMATE;
        }
    }
    double doubleEst = estimateCombinedFee(2 * confTarget, DOUBLE_SUCCESS_PCT, !conservative, &tempResult);
    if (doubleEst > median) {
        median = doubleEst;
        if (feeCalc) {
            feeCalc->est = tempResult;
            feeCalc->reason = FeeReason::DOUBLE_ESTIMATE;
        }
    }

    if (conservative || median == -1) {
        double consEst =  estimateConservativeFee(2 * confTarget, &tempResult);
        if (consEst > median) {
            median = consEst;
            if (feeCalc) {
                feeCalc->est = tempResult;
                feeCalc->reason = FeeReason::CONSERVATIVE;
            }
        }
    }

    if (median < 0) return CFeeRate(0); // error condition

    return CFeeRate(llround(median));
}

void CBlockPolicyEstimator::Flush() {
    FlushUnconfirmed();
    FlushFeeEstimates();
}

void CBlockPolicyEstimator::FlushFeeEstimates()
{
    AutoFile est_file{fsbridge::fopen(m_estimation_filepath, "wb")};
    if (est_file.IsNull() || !Write(est_file)) {
        LogWarning("Failed to write fee estimates to %s. Continue anyway.", fs::PathToString(m_estimation_filepath));
        (void)est_file.fclose();
        return;
    }
    if (est_file.fclose() != 0) {
        LogWarning("Failed to close fee estimates file %s: %s. Continuing anyway.", fs::PathToString(m_estimation_filepath), SysErrorString(errno));
        return;
    }
    LogDebug(BCLog::ESTIMATEFEE, "Flushed fee estimates to %s.", fs::PathToString(m_estimation_filepath));
}

bool CBlockPolicyEstimator::Write(AutoFile& fileout) const
{
    try {
        LOCK(m_cs_fee_estimator);
        fileout << CURRENT_FEES_FILE_VERSION;
        fileout << nBestSeenHeight;
        if (BlockSpan() > HistoricalBlockSpan()/2) {
            fileout << firstRecordedHeight << nBestSeenHeight;
        }
        else {
            fileout << historicalFirst << historicalBest;
        }
        fileout << Using<VectorFormatter<EncodedDoubleFormatter>>(buckets);
        feeStats->Write(fileout);
        shortStats->Write(fileout);
        longStats->Write(fileout);
    }
    catch (const std::exception&) {
        LogWarning("Unable to write policy estimator data (non-fatal)");
        return false;
    }
    return true;
}

bool CBlockPolicyEstimator::Read(AutoFile& filein)
{
    try {
        LOCK(m_cs_fee_estimator);
        int nVersionRequired;
        filein >> nVersionRequired;
        if (nVersionRequired > CURRENT_FEES_FILE_VERSION) {
            throw std::runtime_error{strprintf("File version (%d) too high to be read.", nVersionRequired)};
        }

        // Read fee estimates file into temporary variables so existing data
        // structures aren't corrupted if there is an exception.
        unsigned int nFileBestSeenHeight;
        filein >> nFileBestSeenHeight;

        if (nVersionRequired < CURRENT_FEES_FILE_VERSION) {
            LogWarning("Incompatible old fee estimation data (non-fatal). Version: %d", nVersionRequired);
        } else { // nVersionRequired == CURRENT_FEES_FILE_VERSION
            unsigned int nFileHistoricalFirst, nFileHistoricalBest;
            filein >> nFileHistoricalFirst >> nFileHistoricalBest;
            if (nFileHistoricalFirst > nFileHistoricalBest || nFileHistoricalBest > nFileBestSeenHeight) {
                throw std::runtime_error("Corrupt estimates file. Historical block range for estimates is invalid");
            }
            std::vector<double> fileBuckets;
            filein >> Using<VectorFormatter<EncodedDoubleFormatter>>(fileBuckets);
            size_t numBuckets = fileBuckets.size();
            if (numBuckets <= 1 || numBuckets > 1000) {
                throw std::runtime_error("Corrupt estimates file. Must have between 2 and 1000 feerate buckets");
            }

            std::unique_ptr<ChunkConfirmStats> fileFeeStats(new ChunkConfirmStats(buckets, bucketMap, MED_BLOCK_PERIODS, MED_DECAY, MED_SCALE));
            std::unique_ptr<ChunkConfirmStats> fileShortStats(new ChunkConfirmStats(buckets, bucketMap, SHORT_BLOCK_PERIODS, SHORT_DECAY, SHORT_SCALE));
            std::unique_ptr<ChunkConfirmStats> fileLongStats(new ChunkConfirmStats(buckets, bucketMap, LONG_BLOCK_PERIODS, LONG_DECAY, LONG_SCALE));
            fileFeeStats->Read(filein, numBuckets);
            fileShortStats->Read(filein, numBuckets);
            fileLongStats->Read(filein, numBuckets);

            // Fee estimates file parsed correctly
            // Copy buckets from file and refresh our bucketmap
            buckets = fileBuckets;
            bucketMap.clear();
            for (unsigned int i = 0; i < buckets.size(); i++) {
                bucketMap[buckets[i]] = i;
            }

            // Destroy old ChunkConfirmStats and point to new ones that already reference buckets and bucketMap
            feeStats = std::move(fileFeeStats);
            shortStats = std::move(fileShortStats);
            longStats = std::move(fileLongStats);

            nBestSeenHeight = nFileBestSeenHeight;
            historicalFirst = nFileHistoricalFirst;
            historicalBest = nFileHistoricalBest;
        }
    }
    catch (const std::exception& e) {
        LogWarning("Unable to read policy estimator data (non-fatal): %s", e.what());
        return false;
    }
    return true;
}

void CBlockPolicyEstimator::FlushUnconfirmed()
{
    const auto startclear{SteadyClock::now()};
    LOCK(m_cs_fee_estimator);
    size_t num_entries = mapMemPoolChunks.size();
    // Remove every entry in mapMemPoolChunks
    while (!mapMemPoolChunks.empty()) {
        auto mi = mapMemPoolChunks.begin();
        _removeChunk(mi->first, false); // this calls erase() on mapMemPoolChunks
    }
    const auto endclear{SteadyClock::now()};
    LogDebug(BCLog::ESTIMATEFEE, "Recorded %u unconfirmed Chunks from mempool in %.3fs\n", num_entries, Ticks<SecondsDouble>(endclear - startclear));
}

std::chrono::hours CBlockPolicyEstimator::GetFeeEstimatorFileAge()
{
    auto file_time{fs::last_write_time(m_estimation_filepath)};
    auto now{fs::file_time_type::clock::now()};
    return std::chrono::duration_cast<std::chrono::hours>(now - file_time);
}

static std::set<double> MakeFeeSet(const CFeeRate& min_incremental_fee,
                                   double max_filter_fee_rate,
                                   double fee_filter_spacing)
{
    std::set<double> fee_set;

    const CAmount min_fee_limit{std::max(CAmount(1), min_incremental_fee.GetFeePerK() / 2)};
    fee_set.insert(0);
    for (double bucket_boundary = min_fee_limit;
         bucket_boundary <= max_filter_fee_rate;
         bucket_boundary *= fee_filter_spacing) {

        fee_set.insert(bucket_boundary);
    }

    return fee_set;
}

FeeFilterRounder::FeeFilterRounder(const CFeeRate& minIncrementalFee, FastRandomContext& rng)
    : m_fee_set{MakeFeeSet(minIncrementalFee, MAX_FILTER_FEERATE, FEE_FILTER_SPACING)},
      insecure_rand{rng}
{
}

CAmount FeeFilterRounder::round(CAmount currentMinFee)
{
    AssertLockNotHeld(m_insecure_rand_mutex);
    std::set<double>::iterator it = m_fee_set.lower_bound(currentMinFee);
    if (it == m_fee_set.end() ||
        (it != m_fee_set.begin() &&
         WITH_LOCK(m_insecure_rand_mutex, return insecure_rand.rand32()) % 3 != 0)) {
        --it;
    }
    return static_cast<CAmount>(*it);
}
