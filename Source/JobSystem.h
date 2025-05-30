/*!
 * \file JobSystem.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2025
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
        /* Notifies on value change false -> true */
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
    /* To not dead lock at shutdown, Check JobSystem::shutdown for more details */
    [[nodiscard]] bool tryShutdown() noexcept;

    /* true, if there is job in any priority <= current priority */
    bool hasJob(EJobThreadType enqueueToThread, EJobPriority priority) const noexcept
    {
        const u32 threadIdx = threadTypeToIdx(enqueueToThread);
        bool bAnyJobs = false;
        for (EJobPriority p = Priority_Critical; p <= priority; p = EJobPriority(p + 1))
        {
            bAnyJobs = bAnyJobs || !getThreadJobsQueue(threadIdx, p).empty();
        }
        return bAnyJobs;
    }
    void waitForThreadSync(EJobThreadType enqueueToThread, SpecialQHazardToken *fromThreadTokens) noexcept;
    void enqueueJob(
        std::coroutine_handle<> coro, EJobThreadType enqueueToThread, EJobPriority priority, SpecialQHazardToken *fromThreadTokens
    ) noexcept
    {
        /* We must not enqueue at shutdown */
        COPAT_ASSERT(!allSpecialsExitEvent.try_wait());
        const u32 threadIdx = threadTypeToIdx(enqueueToThread);
        const u32 queueArrayIdx = pAndTTypeToIdx(threadIdx, priority);
        if (fromThreadTokens != nullptr)
        {
            specialQueues[queueArrayIdx].enqueue(coro.address(), fromThreadTokens[queueArrayIdx]);
        }
        else
        {
            specialQueues[queueArrayIdx].enqueue(coro.address());
        }

        specialJobEvents[threadIdx].notify();
    }
    void *dequeueJob(u32 threadIdx, EJobPriority priority) noexcept { return getThreadJobsQueue(threadIdx, priority).dequeue(); }

    void waitForJob(u32 threadIdx) noexcept { return specialJobEvents[threadIdx].wait(); }
    void onSpecialThreadExit() noexcept { allSpecialsExitEvent.count_down(); }

    /**
     * Allocates COUNT number of enqueue tokens, One for each special thread to be used for en queuing job from any threads for each priority
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
                    SpecialQHazardToken(getThreadJobsQueue(threadIdx, priority).getHazardToken());
            }
        }
        return tokens;
    }

private:
    static constexpr u32 threadTypeToIdx(EJobThreadType threadType) { return u32(threadType) - (u32(EJobThreadType::MainThread) + 1); }
    static constexpr EJobThreadType idxToThreadType(u32 threadIdx) { return EJobThreadType(threadIdx + 1 + u32(EJobThreadType::MainThread)); }
    // Priority and thread type idx combined to get index in linear array
    static constexpr u32 pAndTTypeToIdx(u32 threadIdx, EJobPriority priority) { return threadIdx * Priority_MaxPriority + priority; }

    SpecialThreadQueueType &getThreadJobsQueue(u32 threadIdx, EJobPriority priority) noexcept
    {
        return specialQueues[pAndTTypeToIdx(threadIdx, priority)];
    }
    const SpecialThreadQueueType &getThreadJobsQueue(u32 threadIdx, EJobPriority priority) const noexcept
    {
        return specialQueues[pAndTTypeToIdx(threadIdx, priority)];
    }

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

    void initialize(JobSystem *, SpecialThreadQueueType::QueueSharedContext &) noexcept {}
    void run() noexcept {}
    [[nodiscard]] bool tryShutdown() noexcept { return true; }

    bool hasJob(EJobThreadType, EJobPriority) const noexcept { return false; }
    void waitForThreadSync(EJobThreadType, SpecialQHazardToken *) noexcept {}
    void enqueueJob(std::coroutine_handle<>, EJobThreadType, EJobPriority, SpecialQHazardToken *) noexcept {}
    void *dequeueJob(u32, EJobPriority) noexcept { return nullptr; }

    void waitForJob(u32) noexcept {}
    void onSpecialThreadExit() noexcept {}

    SpecialQHazardToken *allocateEnqTokens() noexcept { return nullptr; }
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

    std::atomic_uint_fast32_t hazardTokensTop{ 0 };
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
    /* To not dead lock at shutdown, Check JobSystem::shutdown for more details */
    [[nodiscard]] bool tryShutdown() noexcept;

    /* true, if there is job in any priority <= current priority */
    bool hasJob(u32 threadIdx, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) const noexcept;
    /* skipThreadIdx if invalid all worker threads will be waited for */
    void waitForThreadSync(WorkerQHazardToken *fromThreadTokens, u32 skipThreadIdx) noexcept;
    void enqueueJob(std::coroutine_handle<> coro, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept;
    void *dequeueJob(u32 threadIdx, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept;
    void *stealJob(u32 stealFromIdx, EJobPriority stealPriority, WorkerQHazardToken *fromThreadTokens) noexcept;

    void waitForJob(u32 workerIdx) noexcept;
    void onWorkerThreadExit() noexcept;

    /**
     * Allocates workersCount number of enqueue tokens, One for each worker thread to be used for en queuing job from any threads for each
     * priority
     */
    WorkerQHazardToken *allocateEnqTokens() noexcept;
    u32 getWorkersCount() const { return workersCount; }

private:
    // Priority and thread type idx combined to get index in linear array
    static u32 pAndTTypeToIdx(u32 threadIdx, EJobPriority priority) { return threadIdx * Priority_MaxPriority + priority; }

    WorkerThreadQueueType &getThreadJobsQueue(u32 workerIdx, EJobPriority priority) const noexcept
    {
        return workerQs[pAndTTypeToIdx(workerIdx, priority)];
    }
    u32 workerQsCount() const { return workersCount * Priority_MaxPriority; }
    /* One token per thread for each worker thread queues and the priorities */
    u32 hazardTokensCount() const;
};

/**
 * Additional copat threads that do not belong to special or workers and needs individual control
 */
using INTERNAL_DoJobSystemThreadFuncType = void (JobSystem::*)(class JobSystemThread &);
class JobSystemThread
{
public:
    struct InitInfo
    {
        JobSystem *jobSystem;
        SpecialThreadQueueType::QueueSharedContext &qSharedContent;
        const TChar *threadName;
        EJobThreadType threadType;
    };

public:
    JobSystem *ownerJobSystem = nullptr;

    /* It is okay to have as array as each queue will be aligned 2x the Cache line size */
    SpecialThreadQueueType queues[Priority_MaxPriority];
    JobReceivedEvent jobReceiveEvent;
    /* Could just be std::atomic_flag */
    std::latch exitEvent{ 1 };

    const TChar *threadName;
    EJobThreadType threadType;

public:
    void initialize(InitInfo info);
    void run(INTERNAL_DoJobSystemThreadFuncType doThreadJob, bool bSetAffinity);
    /* To not dead lock at shutdown, Check JobSystem::shutdown for more details */
    [[nodiscard]] bool tryShutdown() noexcept;

    /* true, if there is job in any priority <= current priority */
    bool hasJob(EJobPriority priority) const noexcept;
    void waitForThreadSync(SpecialQHazardToken *fromThreadTokens) noexcept;
    void enqueueJob(std::coroutine_handle<> coro, EJobPriority priority, SpecialQHazardToken *fromThreadTokens) noexcept;
    void *dequeueJob(EJobPriority priority) noexcept;

    void waitForJob();
    void onJobSytemThreadExit();

    SpecialQHazardToken getEnqToken(EJobPriority priority);

private:
    SpecialThreadQueueType &getThreadJobsQueue(EJobPriority priority);
    const SpecialThreadQueueType &getThreadJobsQueue(EJobPriority priority) const;
};

#define THREADCONSTRAINT_ENUM_TO_FLAGBIT(ConstraintName)                                                                                     \
    (copat::JobSystem::BitMasksStart << (copat::JobSystem::ConstraintName - copat::JobSystem::BitMasksStart))
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
        TlDataCreateFunc createTlData = {};
        /* Must be synchronized externally */
        TlDataDeleteFunc deleteTlData = {};
    };

    struct PerThreadData
    {
        EJobThreadType threadType;
        u32 threadIdx;
        SpecialQHazardToken mainQTokens[Priority_MaxPriority];
        SpecialQHazardToken supervisorTokens[Priority_MaxPriority];
        WorkerQHazardToken *workerQsTokens;
        SpecialQHazardToken *specialQsTokens;
        void *tlUserData;
#if COPAT_DEBUG_JOBS
        void *currentJobHndl = nullptr;
#endif

        /* Constructor is necessary as mainQTokens and supervisorTokens cannot be default constructed */
        PerThreadData(SpecialThreadQueueType *mainQs, JobSystemThread &supervisorThread);
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
        EnableSupervisor,
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

    JobSystemThread supervisorThread;

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

    void initialize(InitInterface initIxx, void *inUserData) noexcept;
    /* Overload to match the previous initialize */
    void initialize(MainThreadTickFunc &&mainTickFunc, void *inUserData) noexcept
    {
        initialize(InitInterface{ .mainThreadTick = std::move(mainTickFunc) }, inUserData);
    }
    /* Initialize with no main or user data */
    void initialize() noexcept { initialize(InitInterface{}, nullptr); }
    void joinMain() noexcept { runMain(); }
    void exitMain() noexcept { bExitMain[0].test_and_set(std::memory_order::release); }
    void shutdown() noexcept;
    bool isRunning() const noexcept { return bExitMain[1].test(std::memory_order::relaxed); }

    /* Helper to wait for a thread/job system to reach a this point. Will return immediately if called on this thread
     * Jobs added after this point will not be waited on.
     * Be very careful when waiting for MainThread as there is high chance for dead lock in that case. */
    void waitForThreadSync(EJobThreadType waitForThread) noexcept;
    /* true, if there is job in any priority <= current priority in current thread.
     * Not allowed to inspect other threads due to MpSc queues. */
    bool hasJob(EJobPriority priority) const noexcept;
    void enqueueJob(
        std::coroutine_handle<> coro, EJobThreadType enqueueToThread = EJobThreadType::WorkerThreads,
        EJobPriority priority = EJobPriority::Priority_Normal, std::source_location src = std::source_location::current()
    ) noexcept;

    EJobThreadType getCurrentThreadType() const noexcept
    {
        if (PerThreadData *tlData = getPerThreadData())
        {
            return tlData->threadType;
        }
        return EJobThreadType::MaxThreads;
    }
    /* Will return non zero index only for workers, for others always returns 0 */
    u32 getCurrentThreadIdx() const noexcept
    {
        if (PerThreadData *tlData = getPerThreadData())
        {
            return tlData->threadType == EJobThreadType::WorkerThreads ? tlData->threadIdx : 0u;
        }
        return 0u;
    }
    EJobThreadType enqToThreadType(EJobThreadType forThreadType) const noexcept { return enqIndirection[u32(forThreadType)]; }
    bool isInThread(EJobThreadType threadType) const { return getCurrentThreadType() == enqToThreadType(threadType); }
    void *getTlUserData() const
    {
        if (PerThreadData *tlData = getPerThreadData())
        {
            return tlData->tlUserData;
        }
        return nullptr;
    }

    u32 getWorkersCount() const noexcept { return workerThreadsPool.getWorkersCount(); }
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
    void doJobSysThreadJobs(class JobSystemThread &jsThread) noexcept;
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
                coroPtr = specialThreadsPool.dequeueJob(SpecialThreadIdx, priority);
            }
            while (coroPtr)
            {
                COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialJob"));
#if COPAT_DEBUG_JOBS
                pushNextDeq({
                    .fromThreadIdx = SpecialThreadIdx,
                    .execThreadIdx = SpecialThreadIdx,
                    .threadType = SpecialThreadType,
                    .jobHndl = coroPtr,
                    .jobSys = this,
                });
                tlData->currentJobHndl = coroPtr;
#endif
                /* Resume job/task */
                std::coroutine_handle<>::from_address(coroPtr).resume();

                coroPtr = nullptr;
                for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
                     priority = EJobPriority(priority + 1))
                {
                    coroPtr = specialThreadsPool.dequeueJob(SpecialThreadIdx, priority);
                }
            }
#if COPAT_DEBUG_JOBS
            tlData->currentJobHndl = nullptr;
#endif

            if (bExitMain[1].test(std::memory_order::relaxed))
            {
                break;
            }

            specialThreadsPool.waitForJob(SpecialThreadIdx);
        }
        specialThreadsPool.onSpecialThreadExit();

        deletePerThreadData(tlData);
    }

private:
    /* Dumping codes */
#if COPAT_DEBUG_JOBS
    /* Assuming 128 threads en/dequeuing 256 jobs for a frame/dump */
    constexpr static const u32 MAX_ENQ_DEQ_DUMPS = 32768;
    std::vector<EnqueueDump> enQsDumpList{ MAX_ENQ_DEQ_DUMPS };
    std::vector<DequeueDump> dQsDumpList{ MAX_ENQ_DEQ_DUMPS };
    u64 enqDumpIdx = 0;
    u64 dqDumpIdx = 0;
    std::mutex dumpingMutex;

    void pushNextEnq(EnqueueDump &&dump);
    void pushNextDeq(DequeueDump &&dump);
    static JobSystem *dumpTlJobSysPtr();
#endif
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
bool SpecialThreadsPool<SpecialThreadsCount>::tryShutdown() noexcept
{
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialThreadsShutdown"));
    for (u32 i = 0; i < COUNT; ++i)
    {
        specialJobEvents[i].notify();
    }
    if (allSpecialsExitEvent.try_wait())
    {
        tokensAllocator.release();
        return true;
    }
    return false;
}

template <u32 SpecialThreadsCount>
template <u32 Idx>
void SpecialThreadsPool<SpecialThreadsCount>::runSpecialThread() noexcept
{
    constexpr static const EJobThreadType THREAD_TYPE = idxToThreadType(Idx);
    INTERNAL_DoSpecialThreadFuncType func = &JobSystem::doSpecialThreadJobs<Idx, THREAD_TYPE>;
    INTERNAL_runSpecialThread(func, THREAD_TYPE, Idx, ownerJobSystem);
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