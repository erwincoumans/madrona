#pragma once

#include <type_traits>

namespace madrona {

constexpr JobID JobID::none()
{
    return JobID {
        ~0u,
        ~0u,
    };
}

template <size_t N>
struct JobContainerBase::DepsArray {
    JobID dependencies[N];

    template <typename... DepTs>
    inline DepsArray(DepTs ...deps)
        : dependencies { deps ... }
    {}
};

template <> struct JobContainerBase::DepsArray<0> {
    template <typename... DepTs>
    inline DepsArray(DepTs...) {}
};

template <typename Fn, size_t N>
template <typename... DepTs>
JobContainer<Fn, N>::JobContainer(
#ifdef MADRONA_MW_MODE
                                  uint32_t world_id,
#endif
                                   Fn &&func,
                                   DepTs ...deps)
    : JobContainerBase {
          .id = JobID::none(), // Assigned in JobManager::queueJob
#ifdef MADRONA_MW_MODE
          .worldID = world_id,
#endif
          .numDependencies = N,
      },
      dependencies(deps...),
      fn(std::forward<Fn>(func))
{}

bool JobManager::isQueueEmpty(uint32_t head,
                              uint32_t correction,
                              uint32_t tail) const
{
    auto checkGEWrapped = [](uint32_t a, uint32_t b) {
        return a - b <= (1u << 31u);
    };

    return checkGEWrapped(head - correction, tail);
}

template <typename StartFn>
struct JobManager::EntryConfig {
    uint32_t numUserdataBytes;
    uint32_t userdataAlignment;
    void (*ctxInitCB)(void *, void *, WorkerInit &&);
    uint32_t numCtxBytes;
    uint32_t ctxAlignment;
    StartFn startData;
    void (*startCB)(Context *, void *);
};

template <typename ContextT, typename DataT, typename StartFn>
JobManager::EntryConfig<StartFn> JobManager::makeEntry(StartFn &&start_fn)
{
    static_assert(std::is_trivially_destructible_v<ContextT>,
                  "Context types with custom destructors are not supported");

    return {
        sizeof(DataT),
        alignof(DataT),
        [](void *ctx, void *data, WorkerInit &&init) {
            new (ctx) ContextT((DataT *)data, std::forward<WorkerInit>(init));
        },
        sizeof(ContextT),
        std::alignment_of_v<ContextT>,
        std::forward<StartFn>(start_fn),
        [](Context *ctx_base, void *data) {
            auto &ctx = *static_cast<ContextT *>(ctx_base);
            auto fn_ptr = (StartFn *)data;

            ctx.submit([fn = StartFn(*fn_ptr)](ContextT &ctx) {
                fn(ctx);
            }, false, ctx.currentJobID());
        },
    };
}

template <typename StartFn>
JobManager::JobManager(const EntryConfig<StartFn> &entry_cfg,
                       int desired_num_workers,
                       int num_io,
                       StateManager *state_mgr,
                       bool pin_workers)
    : JobManager(entry_cfg.numUserdataBytes,
                 entry_cfg.userdataAlignment,
                 entry_cfg.ctxInitCB,
                 entry_cfg.numCtxBytes,
                 entry_cfg.ctxAlignment,
                 entry_cfg.startCB,
                 (void *)&entry_cfg.startData,
                 desired_num_workers,
                 num_io,
                 state_mgr,
                 pin_workers)
{}

JobID JobManager::reserveProxyJobID(int thread_idx, JobID parent_id)
{
    return reserveProxyJobID(thread_idx, parent_id.id);
}

void JobManager::relinquishProxyJobID(int thread_idx, JobID job_id)
{
    return relinquishProxyJobID(thread_idx, job_id.id);
}

template <typename ContextT, typename ContainerT>
void JobManager::singleInvokeEntry(Context *ctx_base,
                                   JobContainerBase *data)
{
    ContextT &ctx = *static_cast<ContextT *>(ctx_base);
    auto container = static_cast<ContainerT *>(data);
    JobManager *job_mgr = ctx.job_mgr_;
    
    container->fn(ctx);

    job_mgr->jobFinished(ctx.worker_idx_, data->id.id);
    
    if constexpr (!std::is_trivially_destructible_v<ContainerT>) {
        container->~ContainerT();
    }
    // Important note: jobs may be freed by different threads
    job_mgr->deallocJob(ctx.worker_idx_, data, sizeof(ContainerT));
}

template <typename ContextT, typename ContainerT>
void JobManager::multiInvokeEntry(Context *ctx_base,
                                  JobContainerBase *data,
                                  uint64_t invocation_offset,
                                  uint64_t num_invocations,
                                  RunQueue *thread_queue)
{
    ContextT &ctx = *static_cast<ContextT *>(ctx_base);
    auto container = static_cast<ContainerT *>(data);
    JobManager *job_mgr = ctx.job_mgr_;

    auto shouldSplit = [](JobManager *job_mgr, RunQueue *queue) {
        uint32_t cur_tail = queue->tail.load(std::memory_order_relaxed);
        uint32_t cur_correction =
            queue->correction.load(std::memory_order_relaxed);
        uint32_t cur_head = queue->head.load(std::memory_order_relaxed);

        return job_mgr->isQueueEmpty(cur_head, cur_correction, cur_tail);
    };

    // This loop is never called with num_invocations == 0
    uint64_t invocation_idx = invocation_offset;
    uint64_t remaining_invocations = num_invocations;
    do {
        uint64_t cur_invocation = invocation_idx++;
        remaining_invocations -= 1;

        if (remaining_invocations > 0 && shouldSplit(job_mgr, thread_queue)) {
            job_mgr->splitJob(&multiInvokeEntry<ContextT, ContainerT>, data,
                invocation_idx, remaining_invocations, thread_queue);
            remaining_invocations = 0;
        }

        container->fn(ctx, cur_invocation);
    } while (remaining_invocations > 0);

    bool cleanup = job_mgr->markInvocationsFinished(ctx.worker_idx_, data,
        invocation_idx - invocation_offset);
    if (cleanup) {
        if constexpr (!std::is_trivially_destructible_v<ContainerT>) {
            container->~ContainerT();
        }
        // Important note: jobs may be freed by different threads
        job_mgr->deallocJob(ctx.worker_idx_, data, sizeof(ContainerT));
    }
}

template <typename ContextT, bool single_invoke, typename Fn,
          typename... DepTs>
JobID JobManager::queueJob(int thread_idx,
                           Fn &&fn,
                           uint32_t num_invocations,
                           JobID parent_id,
                           MADRONA_MW_COND(uint32_t world_id,)
                           JobPriority prio,
                           DepTs ...deps)
{
    static constexpr uint32_t num_deps = sizeof...(DepTs);
    using ContainerT = JobContainer<Fn, num_deps>;
#ifdef MADRONA_GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
    static_assert(num_deps == 0 ||
        offsetof(ContainerT, dependencies) == sizeof(JobContainerBase),
        "Dependencies at incorrect offset in container type");
#ifdef MADRONA_GCC
#pragma GCC diagnostic pop
#endif

    static constexpr uint64_t job_size = sizeof(ContainerT);
    static constexpr uint64_t job_alignment = alignof(ContainerT);
    static_assert(job_size <= JobManager::Alloc::maxJobSize,
                  "Job lambda capture is too large");
    static_assert(job_alignment <= JobManager::Alloc::maxJobAlignment,
        "Job lambda capture has too large an alignment requirement");
    static_assert(utils::isPower2(job_alignment));

    void *store = allocJob(thread_idx, job_size, job_alignment);

    auto container = new (store) ContainerT(MADRONA_MW_COND(world_id,)
        std::forward<Fn>(fn), deps...);

    void *entry;
    if constexpr (single_invoke) {
        SingleInvokeFn fn_ptr = &singleInvokeEntry<ContextT, ContainerT>;
        entry = (void *)fn_ptr;
    } else {
        MultiInvokeFn fn_ptr = &multiInvokeEntry<ContextT, ContainerT>;
        entry = (void *)fn_ptr;
    }

    return queueJob(thread_idx, entry, container, num_invocations,
                    parent_id.id, prio);
}

void * JobManager::allocJob(int worker_idx, uint32_t num_bytes,
                            uint32_t alignment)
{
    return job_allocs_[worker_idx].alloc(alloc_state_, num_bytes,
                                         alignment);
}

void JobManager::deallocJob(int worker_idx, void *ptr, uint32_t num_bytes)
{
    job_allocs_[worker_idx].dealloc(alloc_state_, ptr, num_bytes);
}

}
