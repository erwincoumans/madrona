#pragma once

#include <madrona/fwd.hpp>
#include <madrona/utils.hpp>
#include <madrona/query.hpp>

#include <cstdint>

namespace madrona {

class SystemBase {
public:
    using EntryFn = void (*)(SystemBase *, void *, uint32_t);

    SystemBase(EntryFn entry_fn);
    std::atomic_uint32_t numInvocations; 
private:
    EntryFn entry_fn_;
friend class TaskGraph;
};

template <typename SystemT>
class CustomSystem : public SystemBase {
public:
    CustomSystem();

private:
    static void entry(SystemBase *sys, void *data,
                      uint32_t invocation_offset);
};

#if 0
template <typename SystemT, typename... ComponentTs>
class ParallelForSystem : public SystemBase {
public:
    ParallelForSystem();

private:
    using ContextT = utils::FirstArgTypeExtractor<decltype(SystemT::run)>;

    static void entry(Context &ctx, uint32_t invocation_offset, uint32_t num_invocations);

    Query<ComponentTs...> query_;
};

template <typename Fn, typename... ComponentTs>
class LambdaParallelForSystem : public ParallelForSystem<
        LambdaParallelForSystem<Fn, ComponentTs...>, ComponentTs...> {
    using ContextT = utils::FirstArgTypeExtractor<decltype(Fn::operator())>;
public:
    static LambdaParallelForSystem<Fn, ComponentTs...> * allocate(Context &ctx);
    static void deallocate(Context &ctx,
                           LambdaParallelForSystem<Fn, ComponentTs...> *lambda);

    void run(ContextT &ctx, uint32_t invocation_idx);

private:
    LambdaParallelForSystem(Fn &&fn);

    Fn fn;
};
#endif

}

#include "system.inl"
