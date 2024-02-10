/*!
 * \file JobSystem.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2024
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "FAAArrayQueue.hpp"

#include <coroutine>
#include <semaphore>
#include <latch>

COPAT_NS_INLINED
namespace copat
{
class JobSystem;

using SpecialThreadQueueType = FAAArrayMPSCQueue<void>;
using SpecialQHazardToken = SpecialThreadQueueType::HazardToken;
using WorkerThreadQueueType = FAAArrayQueue<void>;
using WorkerQHazardToken = WorkerThreadQueueType::HazardToken;

/**
 * Just to not leak thread include
 */
using INTERNAL_DoSpecialThreadFuncType = void (JobSystem::*)();
void INTERNAL_runSpecialThread(INTERNAL_DoSpecialThreadFuncType threadFunc, EJobThreadType threadType, u32 threadIdx, JobSystem *jobSystem);

struct alignas(2 * CACHE_LINE_SIZE) JobReceivedEvent
{
    std::atomic_flag flag;

    void notify() noexcept
    {
        flag.test_and_set(std::memory_order::release);
        flag.notify_one();
    }

    void wait() noexcept
    {
        flag.wait(false, std::memory_order::acquire);
        // Relaxed is fine this will not be reordered before wait acquire
        flag.clear(std::memory_order::relaxed);
    }
};

#define SPECIALTHREAD_NAME_FIRST(ThreadType) COPAT_TCHAR(#ThreadType)
#define SPECIALTHREAD_NAME(ThreadType) , COPAT_TCHAR(#ThreadType)

template <u32 SpecialThreadsCount>
class SpecialThreadsPool
{
public:
    constexpr static const u32 COUNT = SpecialThreadsCount;
    constexpr static const TChar *NAMES[]
        = { FOR_EACH_UDTHREAD_TYPES_UNIQUE_FIRST_LAST(SPECIALTHREAD_NAME_FIRST, SPECIALTHREAD_NAME, SPECIALTHREAD_NAME) };

    JobSystem *ownerJobSystem = nullptr;

    // It is okay to have as array as each queue will be aligned 2x the Cache line size
    SpecialThreadQueueType specialQueues[COUNT * Priority_MaxPriority];
    JobReceivedEvent specialJobEvents[COUNT];
    std::latch allSpecialsExitEvent{ COUNT };

private:
    struct EnqueueTokensAllocator
    {
        SpecialQHazardToken *hazardTokens{ nullptr };
        u32 totalTokens{ 0 };
        std::atomic_int_fast32_t stackTop{ 0 };

        void initialize(u32 totalThreads) noexcept;
        void release() noexcept;

        SpecialQHazardToken *allocate() noexcept;
    };

    EnqueueTokensAllocator tokensAllocator;

public:
    void initialize(JobSystem *jobSystem, SpecialThreadQueueType::QueueSharedContext &qSharedContext) noexcept;
    void run() noexcept;
    void shutdown() noexcept;

    void enqueueJob(
        std::coroutine_handle<> coro, EJobThreadType enqueueToThread, EJobPriority priority, SpecialQHazardToken *fromThreadTokens
    ) noexcept
    {
        // We must not enqueue at shutdown
        COPAT_ASSERT(!allSpecialsExitEvent.try_wait());
        const u32 threadIdx = threadTypeToIdx(enqueueToThread);
        const u32 queueArrayIdx = pAndTTypeToIdx(threadIdx, priority);
        if (fromThreadTokens)
        {
            specialQueues[queueArrayIdx].enqueue(coro.address(), fromThreadTokens[queueArrayIdx]);
        }
        else
        {
            specialQueues[queueArrayIdx].enqueue(coro.address());
        }

        specialJobEvents[threadIdx].notify();
    }

    SpecialThreadQueueType *getThreadJobsQueue(u32 threadIdx, EJobPriority priority) noexcept
    {
        return &specialQueues[pAndTTypeToIdx(threadIdx, priority)];
    }
    void waitForJob(u32 threadIdx) noexcept { return specialJobEvents[threadIdx].wait(); }
    void onSpecialThreadExit() noexcept { allSpecialsExitEvent.count_down(); }

    /**
     * Allocates COUNT number of enqueue tokens, One for each special thread to be used for enqueuing job from threads for each priority
     */
    SpecialQHazardToken *allocateEnqTokens() noexcept
    {
        SpecialQHazardToken *tokens = tokensAllocator.allocate();
        // Tokens can be null if special thread is disabled
        if (tokens == nullptr)
        {
            return tokens;
        }

        for (u32 threadIdx = 0; threadIdx < COUNT; ++threadIdx)
        {
            for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority; priority = EJobPriority(priority + 1))
            {
                new (tokens + pAndTTypeToIdx(threadIdx, priority))
                    SpecialQHazardToken(getThreadJobsQueue(threadIdx, priority)->getHazardToken());
            }
        }
        return tokens;
    }

private:
    static constexpr u32 threadTypeToIdx(EJobThreadType threadType) { return u32(threadType) - (u32(EJobThreadType::MainThread) + 1); }
    static constexpr EJobThreadType idxToThreadType(u32 threadIdx) { return EJobThreadType(threadIdx + 1 + u32(EJobThreadType::MainThread)); }
    // Priority and thread type idx combined to get index in linear array
    static constexpr u32 pAndTTypeToIdx(u32 threadIdx, EJobPriority priority) { return threadIdx * Priority_MaxPriority + priority; }

    template <u32 Idx>
    void runSpecialThread() noexcept;
    template <u32... Indices>
    void runSpecialThreads(std::integer_sequence<u32, Indices...>) noexcept;
};

#undef SPECIALTHREAD_NAME_FIRST
#undef SPECIALTHREAD_NAME

/**
 * For no special threads case
 */
template <>
class SpecialThreadsPool<0>
{
public:
    constexpr static const u32 COUNT = 0;
    constexpr static const TChar *NAMES[] = { COPAT_TCHAR("Dummy") };

    void initialize(JobSystem *, SpecialThreadQueueType::QueueSharedContext &) {}
    void run() {}
    void shutdown() {}

    void enqueueJob(std::coroutine_handle<>, EJobThreadType, EJobPriority, SpecialQHazardToken *) {}

    SpecialThreadQueueType *getThreadJobsQueue(u32, EJobPriority) { return nullptr; }
    void waitForJob(u32) {}
    void onSpecialThreadExit() {}

    SpecialQHazardToken *allocateEnqTokens() { return nullptr; }
};

/**
 * Just to not make WorkerThreadsPool friend of JobSystem
 */
using INTERNAL_DoWorkerThreadFuncType = void (JobSystem::*)(u32);

class WorkerThreadsPool
{
public:
    JobSystem *ownerJobSystem = nullptr;

    /* Cache to first pointer of worker queues and worker job events */
    WorkerThreadQueueType *workerQs = nullptr;
    JobReceivedEvent *workerJobEvents = nullptr;

    // For waiting until all workers are finishes and exits
    std::latch allWorkersExitEvent;
    u32 workersCount;

private:
    /* The index of the queue to which next enqueue job pushes the job into */
    std::atomic_uint_fast32_t nextEnqToQ{ 0 };

    std::atomic_int_fast32_t hazardTokensTop{ 0 };
    WorkerQHazardToken *hazardTokens = nullptr;
    /* Allocations for all worker queues, each worker's job wait events and each thread's enq/deq tokens */
    void *workerAllocations = nullptr;

public:
    WorkerThreadsPool(u32 inWorkersCount)
        : allWorkersExitEvent(inWorkersCount)
        , workersCount(inWorkersCount)
    {
        /* Is power of 2 */
        COPAT_ASSERT((inWorkersCount & (inWorkersCount - 1)) == 0);
    }

    void initialize(JobSystem *jobSystem, WorkerThreadQueueType::QueueSharedContext &qSharedContext) noexcept;
    void run(INTERNAL_DoWorkerThreadFuncType doWorkerJobFunc, bool bSetAffinity) noexcept;
    void shutdown() noexcept;

    void enqueueJob(std::coroutine_handle<> coro, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept;
    void *dequeueJob(u32 threadIdx, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept;
    void *stealJob(u32 stealFromIdx, EJobPriority stealPriority, WorkerQHazardToken *fromThreadTokens) noexcept;

    void waitForJob(u32 workerIdx) noexcept;
    void onWorkerThreadExit() noexcept;

    WorkerQHazardToken *allocateEnqTokens() noexcept;
    u32 getWorkersCount() const { return workersCount; }

private:
    // Priority and thread type idx combined to get index in linear array
    static u32 pAndTTypeToIdx(u32 threadIdx, EJobPriority priority) { return threadIdx * Priority_MaxPriority + priority; }

    WorkerThreadQueueType *getThreadJobsQueue(u32 workerIdx, EJobPriority priority) const noexcept
    {
        return &workerQs[pAndTTypeToIdx(workerIdx, priority)];
    }
    u32 workerQsCount() const { return workersCount * Priority_MaxPriority; }
    /* One token per thread for each worker thread queues and the priorities */
    u32 hazardTokensCount() const;
};

#define THREADCONSTRAINT_ENUM_TO_FLAGBIT(ConstraintName)                                                                                       \
    (copat::JobSystem::BitMasksStart << (copat::JobSystem::##ConstraintName - copat::JobSystem::BitMasksStart))
#define NOSPECIALTHREAD_ENUM_TO_FLAGBIT(ThreadType) THREADCONSTRAINT_ENUM_TO_FLAGBIT(No##ThreadType)

class COPAT_EXPORT_SYM JobSystem
{
public:
    /* void func(void *userData) */
    using MainThreadTickFunc = FunctionType<void, void *>;
    /* void *func(void *userData, EJobThreadType threadType, u32 threadIdx) */
    using TlDataCreateFunc = FunctionType<void *, void *, EJobThreadType, u32>;
    /* void func(void *userData, EJobThreadType threadType, u32 threadIdx, void *tlData) */
    using TlDataDeleteFunc = FunctionType<void, void *, EJobThreadType, u32, void *>;
    using SpecialThreadsPoolType = SpecialThreadsPool<u32(EJobThreadType::WorkerThreads) - u32(EJobThreadType::MainThread) - 1>;

    struct InitInterface
    {
        /* Main thread tick function type, This function gets ticked in main thread for every loop and then main job queue will be emptied */
        MainThreadTickFunc mainThreadTick;
        /* Must be synchronized externally */
        TlDataCreateFunc createTlData;
        /* Must be synchronized externally */
        TlDataDeleteFunc deleteTlData;
    };

    struct PerThreadData
    {
        EJobThreadType threadType;
        u32 threadIdx;
        SpecialQHazardToken mainQTokens[Priority_MaxPriority];
        WorkerQHazardToken *workerQsTokens;
        SpecialQHazardToken *specialQsTokens;
        void *tlUserData;

        PerThreadData(SpecialThreadQueueType *mainQs, WorkerThreadsPool &workerThreadPool, SpecialThreadsPoolType &specialThreadPool);
    };

#define NOSPECIALTHREAD_ENUM(ThreadType) No##ThreadType,
    // clang-format off

    // Allows controlling the threading model of the application at runtime
    enum EThreadingConstraint : u32
    {
        // Normal with all special and worker threads
        NoConstraints,
        // No worker or special threads, Only main thread exists
        SingleThreaded,
        NoSpecialThreads,
        NoWorkerThreads,
        // Anything after 8 will be bit masked values. Bit shift is determined by (Flag - BitMasksStart)
        BitMasksStart = 8,
        // Flag if set worker threads will not be set to per logical processor affinity, instead use all processors in a group
        NoWorkerAffinity = BitMasksStart,
        NoJobStealing,
        // Each of below NoSpecialThread mask will not stop creating those threads but will be used only at Enqueue. This is just to avoid unnecessary complexity
        FOR_EACH_UDTHREAD_TYPES(NOSPECIALTHREAD_ENUM) 
        BitMasksEnd
    };

    // clang-format on
#undef NOSPECIALTHREAD_ENUM_FIRST
#undef NOSPECIALTHREAD_ENUM

private:
    static JobSystem *singletonInstance;

    const TChar *jsName = nullptr;
    u32 tlsSlot = 0;
    u32 threadingConstraints = EThreadingConstraint::NoConstraints;

    static_assert(
        std::is_same_v<SpecialThreadQueueType::QueueSharedContext, WorkerThreadQueueType::QueueSharedContext>,
        "Queues shared context are not the same type"
    );
    SpecialThreadQueueType::QueueSharedContext qSharedContext;

    SpecialThreadQueueType mainThreadJobs[Priority_MaxPriority];
    // 0 will be used by main thread loop itself while 1 will be used by worker threads to run until shutdown is called
    std::atomic_flag bExitMain[2];
    MainThreadTickFunc mainThreadTick;
    TlDataCreateFunc tlDataCreate;
    TlDataDeleteFunc tlDataDelete;
    void *userData = nullptr;

    WorkerThreadsPool workerThreadsPool;
    SpecialThreadsPoolType specialThreadsPool;

    EJobThreadType enqIndirection[u32(EJobThreadType::MaxThreads)];

public:
    /**
     * copat::JobSystem::JobSystem
     *
     * Access: public
     *
     * @param u32 constraints - EThreadingConstraint for constraints
     * @param const TChar * jobSysName - Name to prepend to thread name for debugging.
     */
    JobSystem(u32 constraints, const TChar *jobSysName);
    JobSystem(u32 inWorkerCount, u32 constraints, const TChar *jobSysName);

    static JobSystem *get() noexcept { return singletonInstance; }

    void initialize(InitInterface &&initIxx, void *inUserData) noexcept;
    /* Overload to match the previous initialize */
    void initialize(MainThreadTickFunc &&mainTickFunc, void *inUserData) noexcept
    {
        initialize(InitInterface{ .mainThreadTick = std::forward<MainThreadTickFunc>(mainTickFunc) }, inUserData);
    }
    /* Initialize with no main or user data */
    void initialize() noexcept { initialize(InitInterface{}, nullptr); }
    void joinMain() noexcept { runMain(); }
    void exitMain() noexcept { bExitMain[0].test_and_set(std::memory_order::release); }
    void shutdown() noexcept;

    void enqueueJob(
        std::coroutine_handle<> coro, EJobThreadType enqueueToThread = EJobThreadType::WorkerThreads,
        EJobPriority priority = EJobPriority::Priority_Normal
    ) noexcept;

    EJobThreadType getCurrentThreadType() const noexcept
    {
        if (PerThreadData *tlData = getPerThreadData())
        {
            return tlData->threadType;
        }
        return EJobThreadType::MaxThreads;
    }
    EJobThreadType enqToThreadType(EJobThreadType forThreadType) const { return EJobThreadType(enqIndirection[u32(forThreadType)]); }
    bool isInThread(EJobThreadType threadType) const { return getCurrentThreadType() == enqToThreadType(threadType); }
    void *getTlUserData() const
    {
        if (PerThreadData *tlData = getPerThreadData())
        {
            return tlData->tlUserData;
        }
        return nullptr;
    }

    u32 getWorkersCount() const { return workerThreadsPool.getWorkersCount(); }
    u32 getTotalThreadsCount() const { return getWorkersCount() + SpecialThreadsPoolType::COUNT + 1 /* Main thread */; }

    const TChar *getJobSystemName() const { return jsName; }

private:
    void setJobSystemName(const TChar *jobSysName);

    PerThreadData *getPerThreadData() const noexcept;
    PerThreadData &createPerThreadData(EJobThreadType threadType, u32 threadIdx) noexcept;
    void deletePerThreadData(PerThreadData *tlData) noexcept;

    u32 calculateWorkersCount() const noexcept;

    void runMain() noexcept;
    void doWorkerJobs(u32 threadIdx) noexcept;
    /* Necessary to be friend to run special thread jobs */
    friend SpecialThreadsPoolType;
    template <u32 SpecialThreadIdx, EJobThreadType SpecialThreadType>
    void doSpecialThreadJobs() noexcept
    {
        PerThreadData *tlData = &createPerThreadData(SpecialThreadType, SpecialThreadIdx);
        COPAT_ASSERT(tlData->threadType == SpecialThreadType);

        while (true)
        {
            // Execute all tasks in Higher priority to lower priority order
            void *coroPtr = nullptr;
            for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
                 priority = EJobPriority(priority + 1))
            {
                coroPtr = specialThreadsPool.getThreadJobsQueue(SpecialThreadIdx, priority)->dequeue();
            }
            while (coroPtr)
            {
                COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialJob"));
                std::coroutine_handle<>::from_address(coroPtr).resume();

                coroPtr = nullptr;
                for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
                     priority = EJobPriority(priority + 1))
                {
                    coroPtr = specialThreadsPool.getThreadJobsQueue(SpecialThreadIdx, priority)->dequeue();
                }
            }

            if (bExitMain[1].test(std::memory_order::relaxed))
            {
                break;
            }

            specialThreadsPool.waitForJob(SpecialThreadIdx);
        }
        specialThreadsPool.onSpecialThreadExit();

        deletePerThreadData(tlData);
    }
};

//////////////////////////////////////////////////////////////////////////
/// SpecialThreadsPool impl
//////////////////////////////////////////////////////////////////////////

template <u32 SpecialThreadsCount>
void SpecialThreadsPool<SpecialThreadsCount>::initialize(
    JobSystem *jobSystem, SpecialThreadQueueType::QueueSharedContext &qSharedContext
) noexcept
{
    COPAT_ASSERT(jobSystem);
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialThreadsInit"));

    ownerJobSystem = jobSystem;

    for (u32 threadIdx = 0; threadIdx < COUNT; ++threadIdx)
    {
        for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority; priority = EJobPriority(priority + 1))
        {
            specialQueues[pAndTTypeToIdx(threadIdx, priority)].setupQueue(qSharedContext);
        }
    }

    tokensAllocator.initialize(ownerJobSystem->getTotalThreadsCount());
}

template <u32 SpecialThreadsCount>
void copat::SpecialThreadsPool<SpecialThreadsCount>::run() noexcept
{
    runSpecialThreads(std::make_integer_sequence<u32, COUNT>{});
}

template <u32 SpecialThreadsCount>
void SpecialThreadsPool<SpecialThreadsCount>::shutdown() noexcept
{
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialThreadsShutdown"));
    for (u32 i = 0; i < COUNT; ++i)
    {
        specialJobEvents[i].notify();
    }
    allSpecialsExitEvent.wait();
    tokensAllocator.release();
}

template <u32 SpecialThreadsCount>
template <u32 Idx>
void SpecialThreadsPool<SpecialThreadsCount>::runSpecialThread() noexcept
{
    constexpr static const EJobThreadType threadType = idxToThreadType(Idx);
    INTERNAL_DoSpecialThreadFuncType func = &JobSystem::doSpecialThreadJobs<Idx, threadType>;
    INTERNAL_runSpecialThread(func, threadType, Idx, ownerJobSystem);
}

template <u32 SpecialThreadsCount>
template <u32... Indices>
void SpecialThreadsPool<SpecialThreadsCount>::runSpecialThreads(std::integer_sequence<u32, Indices...>) noexcept
{
    (runSpecialThread<Indices>(), ...);
}

template <u32 SpecialThreadsCount>
void SpecialThreadsPool<SpecialThreadsCount>::EnqueueTokensAllocator::initialize(u32 totalThreads) noexcept
{
    if (hazardTokens != nullptr)
    {
        CoPaTMemAlloc::memFree(hazardTokens);
        hazardTokens = nullptr;
    }
    totalTokens = totalThreads * COUNT * Priority_MaxPriority;
    hazardTokens = (SpecialQHazardToken *)(CoPaTMemAlloc::memAlloc(sizeof(SpecialQHazardToken) * totalTokens, alignof(SpecialQHazardToken)));
    stackTop.store(0, std::memory_order::relaxed);
}

template <u32 SpecialThreadsCount>
void SpecialThreadsPool<SpecialThreadsCount>::EnqueueTokensAllocator::release() noexcept
{
    if (hazardTokens != nullptr)
    {
        /* No need to free as everything will be freed when queues are destructed */
        CoPaTMemAlloc::memFree(hazardTokens);
        hazardTokens = nullptr;
    }
}

template <u32 SpecialThreadsCount>
SpecialQHazardToken *SpecialThreadsPool<SpecialThreadsCount>::EnqueueTokensAllocator::allocate() noexcept
{
    u32 tokenIdx = stackTop.fetch_add(COUNT * Priority_MaxPriority, std::memory_order::acq_rel);
    if (tokenIdx < totalTokens)
    {
        return hazardTokens + tokenIdx;
    }
    return nullptr;
}

} // namespace copat