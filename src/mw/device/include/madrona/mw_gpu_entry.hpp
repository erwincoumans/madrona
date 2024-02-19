#pragma once

#include <madrona/taskgraph.hpp>
#include <madrona/memory.hpp>
#include <madrona/mw_gpu/host_print.hpp>
#include <madrona/mw_gpu/tracing.hpp>
#include <madrona/render/ecs.hpp>

namespace madrona {
namespace mwGPU {
namespace entryKernels {

template <typename ContextT, typename WorldT, typename ConfigT, typename InitT>
__launch_bounds__(madrona::consts::numMegakernelThreads, 1)
__global__ void initECS(HostAllocInit alloc_init, void *print_channel,
                        void **exported_columns, void *cfg)
{
    HostAllocator *host_alloc = mwGPU::getHostAllocator();
    new (host_alloc) HostAllocator(alloc_init);

    auto host_print = (HostPrint *)GPUImplConsts::get().hostPrintAddr;
    new (host_print) HostPrint(print_channel);

    TmpAllocator &tmp_alloc = TmpAllocator::get();
    new (&tmp_alloc) TmpAllocator();

#ifdef MADRONA_TRACING
    new (&DeviceTracing::get()) DeviceTracing();
#endif

    StateManager *state_mgr = mwGPU::getStateManager();
    new (state_mgr) StateManager(0);

    ECSRegistry ecs_registry(state_mgr, exported_columns);
    WorldT::registerTypes(ecs_registry, *(ConfigT *)cfg);
}

template <typename ContextT, typename WorldT, typename ConfigT, typename InitT>
__launch_bounds__(madrona::consts::numMegakernelThreads, 1)
__global__ void initWorlds(int32_t num_worlds,
                           const ConfigT *cfg,
                           const InitT *user_inits)
{
    int32_t world_idx = threadIdx.x + blockDim.x * blockIdx.x;

    if (world_idx >= num_worlds) {
        return;
    }

    WorldBase *world = TaskGraph::getWorld(world_idx);

    ContextT ctx = TaskGraph::makeContext<ContextT>(WorldID {
        world_idx,
    });

    new (world) WorldT(ctx, *cfg, user_inits[world_idx]);
}

template <typename ContextT, typename WorldT, typename ConfigT, typename InitT>
__launch_bounds__(madrona::consts::numMegakernelThreads, 1)
__global__ void initTasks(void *cfg)
{
    TaskGraph::Builder builder(1024, 1024 * 2, 1024 * 5);
    WorldT::setupTasks(builder, *(ConfigT *)cfg);

    builder.build((TaskGraph *)mwGPU::GPUImplConsts::get().taskGraph);
}

}

template <auto init_ecs, auto init_worlds, auto init_tasks>
struct MWGPUEntryInstantiate {};

template <typename ContextT, typename WorldT, typename ConfigT, typename InitT>
struct alignas(16) MWGPUEntry : MWGPUEntryInstantiate<
    entryKernels::initECS<ContextT, WorldT, ConfigT, InitT>,
    entryKernels::initWorlds<ContextT, WorldT, ConfigT, InitT>,
    entryKernels::initTasks<ContextT, WorldT, ConfigT, InitT>>
{};

}
}

struct BVHParams {
    uint32_t numWorlds;
    void *instances;
    void *views;
    int32_t *instanceOffsets;
    int32_t *instanceCounts;
    int32_t *viewOffsets;
    uint32_t *mortonCodes;
    void *hostAllocator;
    void *tmpAllocator;
};

extern "C" __global__ void initBVHParams(BVHParams *params,
                                         uint32_t num_worlds)
{
    using namespace madrona;
    using namespace madrona::render;

    // Need to get the pointers to instances, views, offsets, etc...
    StateManager *mgr = mwGPU::getStateManager();
    mwGPU::HostAllocator *host_alloc = mwGPU::getHostAllocator();
    mwGPU::TmpAllocator *tmp_alloc = &mwGPU::TmpAllocator::get();

    printf("Hello from initBVHParams: %p\n", (void *)params);

    params->numWorlds = num_worlds;

    params->instances = (void *)mgr->getArchetypeComponent<
        RenderableArchetype, InstanceData>();

    params->views = (void *)mgr->getArchetypeComponent<
        RenderCameraArchetype, PerspectiveCameraData>();

    params->instanceOffsets = (int32_t *)mgr->getArchetypeWorldOffsets<
        RenderableArchetype>();

    params->instanceCounts = (int32_t *)mgr->getArchetypeWorldCounts<
        RenderableArchetype>();

    params->viewOffsets = (int32_t *)mgr->getArchetypeWorldOffsets<
        RenderCameraArchetype>();

    params->mortonCodes = (uint32_t *)mgr->getArchetypeComponent<
        RenderableArchetype, MortonCode>();

    params->hostAllocator = (void *)host_alloc;
    params->tmpAllocator = (void *)tmp_alloc;

    // params->hostChannel = (void *)host_alloc->getHostChannel();
}

// This macro forces MWGPUEntry to be instantiated, which in turn instantiates
// the entryKernels::* __global__ entry points. static_assert with a trivially
// true check leaves no side effects in the scope where this macro is called.
#define MADRONA_BUILD_MWGPU_ENTRY(ContextT, WorldT, ConfigT, InitT) \
    static_assert(alignof(::madrona::mwGPU::MWGPUEntry<ContextT, WorldT, \
        ConfigT, InitT>) == 16);
