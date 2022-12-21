#include <madrona/taskgraph.hpp>
#include <madrona/crash.hpp>
#include <madrona/memory.hpp>

#include "megakernel_consts.hpp"

namespace madrona {

TaskGraph::Builder::Builder(uint32_t max_num_nodes,
                            uint32_t max_num_dependencies)
    : nodes_((StagedNode *)rawAlloc(sizeof(StagedNode) * max_num_nodes)),
      num_nodes_(0),
      all_dependencies_((NodeID *)rawAlloc(sizeof(NodeID) * max_num_dependencies)),
      num_dependencies_(0)
{}

TaskGraph::Builder::~Builder()
{
    rawDealloc(nodes_);
    rawDealloc(all_dependencies_);
}


TaskGraph::NodeID TaskGraph::Builder::sortArchetypeNode(
    uint32_t archetype_id,
    uint32_t component_id,
    Span<const NodeID> dependencies)
{
    using namespace mwGPU;

    StateManager *state_mgr = getStateManager();
    int32_t column_idx =
        state_mgr->getArchetypeColumnIndex(archetype_id, component_id);

    // Optimize for sorts on the WorldID column, where the 
    // max # of worlds is known
    int32_t num_passes;
    if (column_idx == 1) {
        int32_t num_worlds = GPUImplConsts::get().numWorlds;
        // num_worlds + 1 to leave room for columns with WorldID == -1
        int32_t num_bits = 32 - __builtin_clz(num_worlds + 1);

        num_passes = utils::divideRoundUp(num_bits, 8);
    } else {
        num_passes = 4;
    }

    NodeInfo setup_node;
    setup_node.type = NodeType::SortArchetypeSetup;
    setup_node.funcID = UserFuncID<SortArchetypeEntry::Setup>::id;
    setup_node.data.sortArchetypeFirst.archetypeID = archetype_id;
    setup_node.data.sortArchetypeFirst.columnIDX = column_idx;
    setup_node.data.sortArchetypeFirst.numPasses = num_passes;

    auto upsweep_id = registerNode(histogram_node, dependencies);

    NodeInfo histogram_node;
    histogram_node.type = NodeType::SortArchetypeHistogram;
    histogram_node.funcID = UserFuncID<SortArchetypeEntry::Histogram>::id;
    histogram_node.data.sortArchetypeFirst.archetypeID = archetype_id;
    histogram_node.data.sortArchetypeFirst.columnIDX = column_idx;
    histogram_node.data.sortArchetypeFirst.numPasses = num_passes;

    auto upsweep_id = registerNode(histogram_node, dependencies);

    NodeInfo prefix_sum_node;
    prefix_sum_node.type = NodeType::SortArchetypePrefixSum;
    prefix_sum_node.funcID = UserFuncID<SortArchetypeEntry::PrefixSum>::id;
    prefix_sum_node.data.sortArchetypeSubpass.archetypeID = archetype_id;

    auto cur_id = registerNode(prefix_sum_node, {upsweep_id});

    for (int pass_idx = 0; pass_idx < num_passes; pass_idx++) {
        NodeInfo onesweep_node;
        onesweep_node.type = NodeType::SortArchetypeOnesweep;
        onesweep_node.funcID = UserFuncID<SortArchetypeEntry::Onesweep>::id;
        onesweep_node.data.sortArchetypeSubpass.archetypeID = archetype_id;
        onesweep_node.data.sortArchetypeSubpass.passIDX = pass_idx;
        
        cur_id = registerNode(onesweep_node, {cur_id});
    }

    return cur_id;
}

TaskGraph::NodeID TaskGraph::Builder::compactArchetypeNode(
    uint32_t archetype_id, Span<const NodeID> dependencies)
{
    uint32_t func_id =
        mwGPU::UserFuncID<mwGPU::CompactArchetypeEntry>::id;
    
    NodeInfo node_info;
    node_info.type = NodeType::CompactArchetype;
    node_info.funcID = func_id;
    node_info.data.compactArchetype.archetypeID = archetype_id;
    
    return registerNode(node_info, dependencies);
}

TaskGraph::NodeID TaskGraph::Builder::recycleEntitiesNode(
    Span<const NodeID> dependencies)
{
    uint32_t func_id =
        mwGPU::UserFuncID<mwGPU::RecycleEntitiesEntry>::id;

    NodeInfo node_info;
    node_info.type = NodeType::RecycleEntities;
    node_info.funcID = func_id;

    return registerNode(node_info, dependencies);
}

TaskGraph::NodeID TaskGraph::Builder::resetTmpAllocatorNode(
    Span<const NodeID> dependencies)
{
    uint32_t func_id = 
        mwGPU::UserFuncID<mwGPU::ResetTmpAllocatorEntry>::id;

    NodeInfo node_info;
    node_info.type = NodeType::ResetTmpAllocator;
    node_info.funcID = func_id;

    return registerNode(node_info, dependencies);
}

TaskGraph::NodeID TaskGraph::Builder::registerNode(
    const NodeInfo &node_info,
    Span<const NodeID> dependencies)
{
    uint32_t offset = num_dependencies_;
    uint32_t num_deps = dependencies.size();

    num_dependencies_ += num_deps;

    for (int i = 0; i < (int)num_deps; i++) {
        all_dependencies_[offset + i] = dependencies[i];
    }

    uint32_t node_idx = num_nodes_++;

    nodes_[node_idx] = StagedNode {
        node_info,
        offset,
        num_deps,
    };

    return NodeID {
        node_idx,
    };
}

void TaskGraph::Builder::build(TaskGraph *out)
{
    assert(nodes_[0].numDependencies == 0);
    NodeState *sorted_nodes = 
        (NodeState *)rawAlloc(sizeof(NodeState) * num_nodes_);
    bool *queued = (bool *)rawAlloc(num_nodes_ * sizeof(bool));
    new (&sorted_nodes[0]) NodeState {
        nodes_[0].node,
        0,
        0,
    };
    queued[0] = true;

    uint32_t num_remaining_nodes = num_nodes_ - 1;
    uint32_t *remaining_nodes =
        (uint32_t *)rawAlloc(num_remaining_nodes * sizeof(uint32_t));

    for (int64_t i = 1; i < (int64_t)num_nodes_; i++) {
        queued[i]  = false;
        remaining_nodes[i - 1] = i;
    }

    uint32_t sorted_idx = 1;

    while (num_remaining_nodes > 0) {
        uint32_t cur_node_idx;
        for (cur_node_idx = 0; queued[cur_node_idx]; cur_node_idx++) {}

        StagedNode &cur_node = nodes_[cur_node_idx];

        bool dependencies_satisfied = true;
        for (uint32_t dep_offset = 0; dep_offset < cur_node.numDependencies;
             dep_offset++) {
            uint32_t dep_node_idx =
                all_dependencies_[cur_node.dependencyOffset + dep_offset].id;
            if (!queued[dep_node_idx]) {
                dependencies_satisfied = false;
                break;
            }
        }

        if (dependencies_satisfied) {
            queued[cur_node_idx] = true;
            new (&sorted_nodes[sorted_idx++]) NodeState {
                cur_node.node,
                0,
                0,
                0,
            };
            num_remaining_nodes--;
        }
    }

    rawDealloc(remaining_nodes);
    rawDealloc(queued);

    new (out) TaskGraph(sorted_nodes, num_nodes_);
}

TaskGraph::TaskGraph(NodeState *nodes, uint32_t num_nodes)
    : sorted_nodes_(nodes),
      num_nodes_(num_nodes),
      cur_node_idx_(num_nodes),
      init_barrier_(MADRONA_MWGPU_NUM_MEGAKERNEL_BLOCKS)
{}

TaskGraph::~TaskGraph()
{
    rawDealloc(sorted_nodes_);
}

struct TaskGraph::BlockState {
    WorkerState state;
    uint32_t nodeIdx;
    uint32_t numInvocations;
    uint32_t funcID;
    uint32_t runOffset;
};

static __shared__ TaskGraph::BlockState sharedBlockState;

void TaskGraph::init()
{
    int thread_idx = threadIdx.x;
    if (thread_idx != 0) {
        return;
    }

    int block_idx = blockIdx.x;

    if (block_idx == 0) {
        NodeState &first_node = sorted_nodes_[0];

        uint32_t new_num_invocations = computeNumInvocations(first_node);
        assert(new_num_invocations != 0);
        first_node.curOffset.store(0, std::memory_order_relaxed);
        first_node.numRemaining.store(new_num_invocations,
                                    std::memory_order_relaxed);
        first_node.totalNumInvocations.store(new_num_invocations,
            std::memory_order_relaxed);

        cur_node_idx_.store(0, std::memory_order_release);
    } 

    init_barrier_.arrive_and_wait();
}

void TaskGraph::setBlockState()
{
    uint32_t node_idx = cur_node_idx_.load(std::memory_order_acquire);
    if (node_idx == num_nodes_) {
        sharedBlockState.state = WorkerState::Exit;
        return;
    }

    NodeState &cur_node = sorted_nodes_[node_idx];

    uint32_t cur_offset = 
        cur_node.curOffset.load(std::memory_order_relaxed);

    uint32_t total_invocations =
        cur_node.totalNumInvocations.load(std::memory_order_relaxed);

    if (cur_offset >= total_invocations) {
        sharedBlockState.state = WorkerState::Loop;
        return;
    }

    cur_offset = cur_node.curOffset.fetch_add(consts::numMegakernelThreads,
        std::memory_order_relaxed);

    if (cur_offset >= total_invocations) {
        sharedBlockState.state = WorkerState::Loop;
        return;
    }

    sharedBlockState.state = WorkerState::Run;
    sharedBlockState.nodeIdx = node_idx;
    sharedBlockState.numInvocations = total_invocations;
    sharedBlockState.funcID = cur_node.info.funcID;
    sharedBlockState.runOffset = cur_offset;
}

uint32_t TaskGraph::computeNumInvocations(NodeState &node)
{
    switch (node.info.type) {
        case NodeType::ParallelFor: {
            StateManager *state_mgr = mwGPU::getStateManager();
            QueryRef *query_ref = node.info.data.parallelFor.query;
            return state_mgr->numMatchingEntities(query_ref);
        }
        case NodeType::ClearTemporaries: {
            return 1_u32;
        }
        case NodeType::CompactArchetype: {
            StateManager *state_mgr = mwGPU::getStateManager();
            bool needs_compact = state_mgr->isDirty(
                node.info.data.compactArchetype.archetypeID);

            if (!needs_compact) {
                return 0;
            }

            return mwGPU::getStateManager()->numArchetypeRows(
                node.info.data.compactArchetype.archetypeID);
        }
        case NodeType::SortArchetypeSetup: {
            StateManager *state_mgr = mwGPU::getStateManager();
            bool need_sort = state_mgr->archetypeSetupSortState(
                node.info.data.sortArchetypeHistogram.archetypeID,
                node.info.data.sortArchetypeHistogram.columnIDX,
                node.info.data.sortArchetypeHistogram.numPasses);

            return need_sort ? consts::numMegakernelThreads : 0;
        }
        case NodeType::SortArchetypeHistogram: {
            StateManager *state_mgr = mwGPU::getStateManager();
            const auto &sort_state = state_mgr->getCurrentSortState(
                node.info.data.sortArchetypeSubpass.archetypeID);

            return sort_state.numSortThreads;
        }
        case NodeType::SortArchetypePrefixSum: {
            return consts::numMegakernelThreads;
        }
        case NodeType::SortArchetypeOnesweep: {
            StateManager *state_mgr = mwGPU::getStateManager();
            const auto &sort_state = state_mgr->getCurrentSortState(
                node.info.data.sortArchetypeSubpass.archetypeID);

            return sort_state.numSortThreads;
        }
        case NodeType::RecycleEntities: {
            auto [recycle_base, num_deleted] =
                mwGPU::getStateManager()->fetchRecyclableEntities();

            if (num_deleted > 0) {
                node.info.data.recycleEntities.recycleBase = recycle_base;
            }

            return num_deleted;
        }
        case Nodetype::ResetTmpAllocator: {
            // Hack, as soon as we evaluate count just do the reset
            // and pretend no work had to be done
            TmpAllocator::get().reset();

            return 0;
        }
        default: {
            __builtin_unreachable();
        }
    };

    __builtin_unreachable();
}

void mwGPU::CompactArchetypeEntry::run(EntryData &data, int32_t invocation_idx)
{
#if 0
    uint32_t archetype_id = data.compactArchetype.archetypeID;
    StateManager *state_mgr = mwGPU::getStateManager();
#endif

    // Actually compact
    assert(false);
}

void mwGPU::SortArchetypeEntry::Histogram::run(EntryData &data,
                                               int32_t invocation_idx)
{
    uint32_t archetype_id = data.sortArchetypeHistogram.archetypeID;
    StateManager *state_mgr = mwGPU::getStateManager();

    state_mgr->sortArchetypeHistogram(archetype_id, invocation_idx);
}

void mwGPU::SortArchetypeEntry::PrefixSum::run(EntryData &data,
                                               int32_t invocation_idx)
{
    uint32_t archetype_id = data.sortArchetypeSubpass.archetypeID;
    StateManager *state_mgr = mwGPU::getStateManager();

    state_mgr->sortArchetypePrefixSum(archetype_id, invocation_idx);
}

void mwGPU::SortArchetypeEntry::Onesweep::run(EntryData &data,
                                              int32_t invocation_idx)
{
    uint32_t archetype_id = data.sortArchetypeSubpass.archetypeID;
    int32_t pass_idx = data.sortArchetypeSubpass.passIDX;
    StateManager *state_mgr = mwGPU::getStateManager();

    state_mgr->sortArchetypeOnesweep(archetype_id, pass_idx, invocation_idx);
}

void mwGPU::RecycleEntitiesEntry::run(EntryData &data, int32_t invocation_idx)
{
    mwGPU::getStateManager()->recycleEntities(
        invocation_idx, data.recycleEntities.recycleBase);
}

TaskGraph::WorkerState TaskGraph::getWork(mwGPU::EntryData **entry_data,
                                          uint32_t *run_func_id,
                                          int32_t *run_offset)
{
    int thread_idx = threadIdx.x;

    if (thread_idx == 0) {
        setBlockState();
    }

    __syncthreads();

    WorkerState worker_state = sharedBlockState.state;

    if (worker_state != WorkerState::Run) {
        return worker_state;
    }

    uint32_t num_invocations = sharedBlockState.numInvocations;
    uint32_t base_offset = sharedBlockState.runOffset;

    int32_t thread_offset = base_offset + thread_idx;
    if (thread_offset >= num_invocations) {
        return WorkerState::PartialRun;
    }

    *entry_data = &sorted_nodes_[sharedBlockState.nodeIdx].info.data;
    *run_func_id = sharedBlockState.funcID;
    *run_offset = thread_offset;

    return WorkerState::Run;
}

void TaskGraph::finishWork()
{
    int thread_idx = threadIdx.x;
    __syncthreads();

    if (thread_idx != 0) return;

    uint32_t num_finished = std::min(
        sharedBlockState.numInvocations - sharedBlockState.runOffset,
        consts::numMegakernelThreads);

    uint32_t node_idx = sharedBlockState.nodeIdx;
    NodeState &cur_node = sorted_nodes_[node_idx];

    uint32_t prev_remaining = cur_node.numRemaining.fetch_sub(num_finished,
        std::memory_order_acq_rel);

    if (prev_remaining == num_finished) {
        uint32_t next_node_idx = node_idx + 1;

        while (true) {
            if (next_node_idx < num_nodes_) {
                uint32_t new_num_invocations =
                    computeNumInvocations(sorted_nodes_[next_node_idx]);

                if (new_num_invocations == 0) {
                    next_node_idx++;
                    continue;
                }

                NodeState &next_node = sorted_nodes_[next_node_idx];
                next_node.curOffset.store(0, std::memory_order_relaxed);
                next_node.numRemaining.store(new_num_invocations,
                                            std::memory_order_relaxed);
                next_node.totalNumInvocations.store(new_num_invocations,
                    std::memory_order_relaxed);
            } 

            cur_node_idx_.store(next_node_idx, std::memory_order_release);
            break;
        }
    }
}

}

extern "C" __global__ void madronaMWGPUComputeConstants(
    uint32_t num_worlds,
    uint32_t num_world_data_bytes,
    uint32_t world_data_alignment,
    madrona::mwGPU::GPUImplConsts *out_constants,
    size_t *job_system_buffer_size)
{
    using namespace madrona;
    using namespace madrona::mwGPU;

    uint64_t total_bytes = sizeof(TaskGraph);

    uint64_t state_mgr_offset = utils::roundUp(total_bytes,
        (uint64_t)alignof(StateManager));

    total_bytes = state_mgr_offset + sizeof(StateManager);

    uint64_t world_data_offset =
        utils::roundUp(total_bytes, (uint64_t)world_data_alignment);

    uint64_t total_world_bytes =
        (uint64_t)num_world_data_bytes * (uint64_t)num_worlds;

    total_bytes = world_data_offset + total_world_bytes;

    uint64_t host_allocator_offset =
        utils::roundUp(total_bytes, (uint64_t)alignof(mwGPU::HostAllocator));

    total_bytes = host_allocator_offset + sizeof(mwGPU::HostAllocator);

    uint64_t tmp_allocator_offset =
        utils::roundUp(total_bytes, (uint64_t)alignof(mwGPU::TmpAllocator));

    total_bytes = tmp_allocator_offset + sizeof(mwGPU::TmpAllocator);

    *out_constants = GPUImplConsts {
        /*.jobSystemAddr = */                  (void *)0ul,
        /* .taskGraph = */                     (void *)0ul,
        /* .stateManagerAddr = */              (void *)state_mgr_offset,
        /* .worldDataAddr =  */                (void *)world_data_offset,
        /* .hostAllocatorAddr = */             (void *)host_allocator_offset,
        /* .tmpAllocatorAddr */                (void *)tmp_allocator_offset,
        /* .rendererASInstancesAddrs = */      (void **)0ul,
        /* .rendererInstanceCountsAddr = */    (void *)0ul,
        /* .rendererBLASesAddr = */            (void *)0ul,
        /* .rendererViewDatasAddr = */         (void *)0ul,
        /* .numWorldDataBytes = */             num_world_data_bytes,
        /* .numWorlds = */                     num_worlds,
        /* .jobGridsOffset = */                (uint32_t)0,
        /* .jobListOffset = */                 (uint32_t)0,
        /* .maxJobsPerGrid = */                0,
        /* .sharedJobTrackerOffset = */        (uint32_t)0,
        /* .userJobTrackerOffset = */          (uint32_t)0,
    };

    *job_system_buffer_size = total_bytes;
}
