#include <blocktemplatemanager.h>
#include <txmempool.h>
#include <validation.h>

#include <algorithm>

BlockTemplateManager::BlockTemplateManager(CTxMemPool* mempool, ChainstateManager& chainman)
    : m_mempool(mempool), m_chainman(chainman) {}

bool BlockTemplateManager::TipChanged()
{
    const CBlockIndex* tip{m_chainman.ActiveChain().Tip()};
    if (!tip) return false;
    bool changed = tip->nHeight != m_tracked_tip;
    if (changed) m_block_templates.clear();
    return changed;
}

bool BlockTemplateManager::Template::IntervalElapsed(const std::chrono::seconds& interval) const
{
    return (NodeClock::now() - m_time_generated) >= interval;
}

bool BlockTemplateManager::Template::OptionsEqual(const node::BlockAssembler::Options& other) const
{
    return m_options.use_mempool == other.use_mempool &&
           m_options.block_reserved_weight == other.block_reserved_weight &&
           m_options.coinbase_output_max_additional_sigops == other.coinbase_output_max_additional_sigops &&
           m_options.nBlockMaxWeight == other.nBlockMaxWeight &&
           m_options.blockMinFeeRate == other.blockMinFeeRate;
}

std::shared_ptr<node::CBlockTemplate> BlockTemplateManager::CreateBlockTemplateInternal(const node::BlockAssembler::Options& options)
{
    node::BlockAssembler assembler{m_chainman.ActiveChainstate(), m_mempool, options, node::BlockAssembler::ALLOW_OVERSIZED_BLOCKS};
    Template t;
    t.m_block_template = assembler.CreateNewBlock();
    t.m_options = options;
    t.m_time_generated = NodeClock::now();
    m_block_templates.emplace_back(t);

    m_tracked_tip = m_chainman.ActiveTip()->nHeight;
    return t.m_block_template;
}

std::shared_ptr<node::CBlockTemplate> BlockTemplateManager::GetBlockTemplate(const node::BlockAssembler::Options& options, const std::chrono::seconds& interval)
{
    LOCK2(cs_main, m_mutex);

    auto it = std::find_if(
        m_block_templates.begin(),
        m_block_templates.end(),
        [&](const Template& t) { return t.OptionsEqual(options); }
    );

    if (it == m_block_templates.end() || it->IntervalElapsed(interval)) {
        return CreateBlockTemplateInternal(options);
    }

    if (TipChanged()) {
        return CreateBlockTemplateInternal(options);
    }

    if (options.test_block_validity && !it->m_options.test_block_validity) {
        BlockValidationState state;
        if (BlockValidationState state{TestBlockValidity(m_chainman.ActiveChainstate(), it->m_block_template->block,
                               /*check_pow=*/false, /*check_merkle_root=*/false)}; !state.IsValid()) {
            throw std::runtime_error(strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
        it->m_options.test_block_validity = true;
    }

    return it->m_block_template;
}
