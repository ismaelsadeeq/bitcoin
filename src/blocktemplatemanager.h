#ifndef MINING_MANAGER_H
#define MINING_MANAGER_H

#include <kernel/cs_main.h>
#include <node/miner.h>
#include <sync.h>
#include <util/time.h>

#include <optional>
#include <chrono>
#include <vector>

class CTxMemPool;
class ChainstateManager;

class BlockTemplateManager {
public:
    BlockTemplateManager(CTxMemPool* mempool, ChainstateManager& chainman);
    std::shared_ptr<node::CBlockTemplate> GetBlockTemplate(const node::BlockAssembler::Options& options, const std::chrono::seconds& interval) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    bool TipChanged() EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    std::shared_ptr<node::CBlockTemplate> CreateBlockTemplateInternal(const node::BlockAssembler::Options& options) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    struct Template {
        node::BlockAssembler::Options m_options;
        NodeClock::time_point m_time_generated;
        std::shared_ptr<node::CBlockTemplate> m_block_template;

        bool IntervalElapsed(const std::chrono::seconds& interval) const;
        bool OptionsEqual(const node::BlockAssembler::Options& options) const;
    };

    int m_tracked_tip{0};
    std::vector<Template> m_block_templates;
    CTxMemPool* m_mempool{nullptr};
    ChainstateManager& m_chainman;

    mutable Mutex m_mutex;
};

#endif // MINING_MANAGER_H
