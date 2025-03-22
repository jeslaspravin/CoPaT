/*!
 * \file JobSystem.cpp
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2025
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#include "JobSystem.h"
#include "Platform/PlatformThreadingFunctions.h"

#include <thread>
#include <bit>

COPAT_NS_INLINED
namespace copat
{

static void getCoreCount(u32 &outCoreCount, u32 &outLogicalProcessorCount)
{
    PlatformThreadingFuncs::getCoreCount(outCoreCount, outLogicalProcessorCount);
    // Just a backup if user did not provide an implementation
    if (outCoreCount == 0 || outLogicalProcessorCount == 0)
    {
        outLogicalProcessorCount = std::thread::hardware_concurrency();
        outCoreCount = outLogicalProcessorCount / 2;
    }
}

static JobSystem::EThreadingConstraint getThreadingConstraint(u32 constraints)
{
    return JobSystem::EThreadingConstraint(constraints & (JobSystem::BitMasksStart - 1));
}

JobSystem *JobSystem::singletonInstance = nullptr;

JobSystem::JobSystem(u32 constraints, const TChar *jobSysName)
    : threadingConstraints(constraints)
    , workerThreadsPool(calculateWorkersCount())
{
    for (u32 i = 0; i < u32(EJobThreadType::MaxThreads); ++i)
    {
        enqIndirection[i] = EJobThreadType(i);
    }
    setJobSystemName(jobSysName);
}
JobSystem::JobSystem(u32 inWorkerCount, u32 constraints, const TChar *jobSysName)
    : threadingConstraints(constraints)
    , workerThreadsPool(inWorkerCount)
{
    for (u32 i = 0; i < u32(EJobThreadType::MaxThreads); ++i)
    {
        enqIndirection[i] = EJobThreadType(i);
    }
    setJobSystemName(jobSysName);
}

#define NO_SPECIALTHREADS_INDIR_SETUP(ThreadType) enqIndirection[u32(EJobThreadType::ThreadType)] = EJobThreadType::MainThread;
#define SPECIALTHREAD_INDIR_SETUP(ThreadType)                                                                                                \
    enqIndirection[u32(EJobThreadType::##ThreadType)]                                                                                        \
        = (threadingConstraints & NOSPECIALTHREAD_ENUM_TO_FLAGBIT(ThreadType)) ? EJobThreadType::MainThread : EJobThreadType::ThreadType;

void JobSystem::initialize(InitInterface initIxx, void *inUserData) noexcept
{
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatInit"));

    if (singletonInstance == nullptr)
    {
        singletonInstance = this;
    }

    if (!PlatformThreadingFuncs::createTlsSlot(tlsSlot))
    {
        return;
    }

    const EThreadingConstraint tConstraint = getThreadingConstraint(threadingConstraints);
    const bool bEnableSpecials
        = (tConstraint != EThreadingConstraint::SingleThreaded && tConstraint != EThreadingConstraint::NoSpecialThreads);
    const bool bEnableWorkers = (tConstraint != EThreadingConstraint::SingleThreaded && tConstraint != EThreadingConstraint::NoWorkerThreads);
    const bool bEnableSupervisor
        = (tConstraint != EThreadingConstraint::SingleThreaded
           && (threadingConstraints & THREADCONSTRAINT_ENUM_TO_FLAGBIT(EnableSupervisor)) != 0);

    /* Setup common data */
    userData = inUserData;
    mainThreadTick = std::move(initIxx.mainThreadTick);
    tlDataCreate = std::move(initIxx.createTlData);
    tlDataDelete = std::move(initIxx.deleteTlData);
    COPAT_ASSERT((tlDataCreate && tlDataDelete) || (!tlDataCreate && !tlDataDelete));

    /* Setup special threads */
    if (bEnableSpecials)
    {
        FOR_EACH_UDTHREAD_TYPES(SPECIALTHREAD_INDIR_SETUP);
        specialThreadsPool.initialize(this, qSharedContext);
    }
    else
    {
        FOR_EACH_UDTHREAD_TYPES(NO_SPECIALTHREADS_INDIR_SETUP);
    }
    /* Setup worker threads */
    if (bEnableWorkers)
    {
        workerThreadsPool.initialize(this, qSharedContext);
    }
    else
    {
        enqIndirection[u32(EJobThreadType::WorkerThreads)] = EJobThreadType::MainThread;
    }

    /* Setup additional threads */
    if (bEnableSupervisor)
    {
        /* Workers only environment only needs supervisor */
        COPAT_ASSERT(bEnableWorkers && !bEnableSpecials);
        supervisorThread.initialize({
            .jobSystem = this,
            .qSharedContent = qSharedContext,
            .threadName = COPAT_TCHAR("Supervisor"),
            .threadType = EJobThreadType::Supervisor,
        });
    }
    else
    {
        enqIndirection[u32(EJobThreadType::Supervisor)] = EJobThreadType::MainThread;
    }

    /* Now run the threads */
    if (bEnableSpecials)
    {
        specialThreadsPool.run();
    }
    if (bEnableWorkers)
    {
        const bool bSetThreadAffinity = (threadingConstraints & THREADCONSTRAINT_ENUM_TO_FLAGBIT(NoWorkerAffinity)) == 0;
        workerThreadsPool.run(&JobSystem::doWorkerJobs, bSetThreadAffinity);
    }
    if (bEnableSupervisor)
    {
        supervisorThread.run(&JobSystem::doJobSysThreadJobs, false);
    }

    /* Setup main thread */
    for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority; priority = EJobPriority(priority + 1))
    {
        mainThreadJobs[priority].setupQueue(qSharedContext);
    }
    TChar threadNameBuffer[257] = { 0 };
    COPAT_PRINTF(threadNameBuffer, COPAT_TCHAR("%s_MainThread"), jsName);
    PlatformThreadingFuncs::setCurrentThreadName(static_cast<const TChar *>(threadNameBuffer));
    PlatformThreadingFuncs::setCurrentThreadProcessor(0, 0);
    createPerThreadData(EJobThreadType::MainThread, 0);
}
#undef NO_SPECIALTHREADS_INDIR_SETUP
#undef SPECIALTHREAD_INDIR_SETUP

void JobSystem::shutdown() noexcept
{
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatShutdown"));

    PerThreadData *mainThreadTlData = getPerThreadData();
    COPAT_ASSERT(mainThreadTlData && mainThreadTlData->threadType == EJobThreadType::MainThread);
    /* In order to prevent job en queuing back into one of the shutdown thread, redirect everything to main thread.
     * Will be slow but would not crash. Needs repeated draining of main thread queue to avoid dead lock. */
    std::fill(
        static_cast<EJobThreadType *>(enqIndirection), static_cast<EJobThreadType *>(enqIndirection) + u32(EJobThreadType::MaxThreads),
        EJobThreadType::MainThread
    );
    /* Clear main thread ticker, we do not want that getting called any more */
    mainThreadTick = {};

    /* Just setting bExitMain flag to expected when shutting down */
    bExitMain[0].test_and_set(std::memory_order::relaxed);
    bExitMain[1].test_and_set(std::memory_order::release);

    const EThreadingConstraint tConstraint = getThreadingConstraint(threadingConstraints);
    const bool bEnableSpecials
        = (tConstraint != EThreadingConstraint::SingleThreaded && tConstraint != EThreadingConstraint::NoSpecialThreads);
    const bool bEnableWorkers = (tConstraint != EThreadingConstraint::SingleThreaded && tConstraint != EThreadingConstraint::NoWorkerThreads);
    const bool bEnableSupervisor
        = (tConstraint != EThreadingConstraint::SingleThreaded
           && (threadingConstraints & THREADCONSTRAINT_ENUM_TO_FLAGBIT(EnableSupervisor)) != 0);

    if (bEnableSpecials)
    {
        while (!specialThreadsPool.tryShutdown())
        {
            /* Maybe threads are waiting for jobs indirected to main thread */
            runMain();
        }
    }
    if (bEnableWorkers)
    {
        while (!workerThreadsPool.tryShutdown())
        {
            /* Maybe threads are waiting for jobs indirected to main thread */
            runMain();
        }
    }
    if (bEnableSupervisor)
    {
        while (!supervisorThread.tryShutdown())
        {
            /* Maybe threads are waiting for jobs indirected to main thread */
            runMain();
        }
    }

    deletePerThreadData(mainThreadTlData);
    if (singletonInstance == this)
    {
        singletonInstance = nullptr;
    }

    CoPaTMemAlloc::memFree(jsName);
}

/**
 * Coroutine required to get coroutine handle for manual enqueueing.
 */
struct DummyCoroutine : public std::suspend_never
{
public:
    struct promise_type;
    std::coroutine_handle<promise_type> coro;

    struct promise_type
    {
    public:
        DummyCoroutine get_return_object() noexcept
        {
            return DummyCoroutine{ .coro = std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
        constexpr std::suspend_never final_suspend() const noexcept { return {}; }
        constexpr void return_void() const noexcept {}
        void unhandled_exception() const noexcept { COPAT_UNHANDLED_EXCEPT(); }
    };
};
template <u32 SpecialThreadsCount>
void copat::SpecialThreadsPool<SpecialThreadsCount>::waitForThreadSync(
    EJobThreadType enqueueToThread, SpecialQHazardToken *fromThreadTokens
) noexcept
{
    /* We must not enqueue at shutdown */
    COPAT_ASSERT(!allSpecialsExitEvent.try_wait());
    std::atomic_flag waitFlag;
    const DummyCoroutine coroHndl = [](std::atomic_flag &waitFlag) -> DummyCoroutine
    {
        waitFlag.test_and_set(std::memory_order::relaxed);
        waitFlag.notify_one();
        co_return;
    }(waitFlag);

    const u32 threadIdx = threadTypeToIdx(enqueueToThread);
    const u32 queueArrayIdx = pAndTTypeToIdx(threadIdx, EJobPriority::Priority_Low);
    if (fromThreadTokens != nullptr)
    {
        specialQueues[queueArrayIdx].enqueue(coroHndl.coro.address(), fromThreadTokens[queueArrayIdx]);
    }
    else
    {
        specialQueues[queueArrayIdx].enqueue(coroHndl.coro.address());
    }

    specialJobEvents[threadIdx].notify();
    waitFlag.wait(false, std::memory_order::relaxed);
}
void JobSystem::waitForThreadSync(EJobThreadType waitForThread) noexcept
{
    PerThreadData *threadData = getPerThreadData();
    const EJobThreadType redirThread = enqToThreadType(waitForThread);
    /* Return if we are in same thread. In worker thread we can skip current thread wait */
    if (isInThread(redirThread) && redirThread != EJobThreadType::WorkerThreads)
    {
        return;
    }

    switch (redirThread)
    {
    case copat::EJobThreadType::MainThread:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWaitForMain"));
        std::atomic_flag waitFlag;
        const DummyCoroutine coroHndl = [](std::atomic_flag &waitFlag) -> DummyCoroutine
        {
            waitFlag.test_and_set(std::memory_order::relaxed);
            waitFlag.notify_one();
            co_return;
        }(waitFlag);

        if (threadData != nullptr)
        {
            mainThreadJobs[EJobPriority::Priority_Low].enqueue(coroHndl.coro.address(), threadData->mainQTokens[EJobPriority::Priority_Low]);
        }
        else
        {
            mainThreadJobs[EJobPriority::Priority_Low].enqueue(coroHndl.coro.address());
        }
        waitFlag.wait(false, std::memory_order::relaxed);
        break;
    }
    case copat::EJobThreadType::WorkerThreads:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWaitForWorker"));

        if (threadData != nullptr)
        {
            const u32 skipThreadIdx = isInThread(copat::EJobThreadType::WorkerThreads) ? threadData->threadIdx : ~0u;
            workerThreadsPool.waitForThreadSync(threadData->workerQsTokens, skipThreadIdx);
        }
        else
        {
            workerThreadsPool.waitForThreadSync(nullptr, ~0u);
        }
        break;
    }
    case copat::EJobThreadType::Supervisor:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWaitForSupervisor"));

        if (threadData != nullptr)
        {
            supervisorThread.waitForThreadSync(threadData->supervisorTokens);
        }
        else
        {
            supervisorThread.waitForThreadSync(nullptr);
        }
        break;
    }
    case copat::EJobThreadType::MaxThreads:
        COPAT_ASSERT(false);
        break;
    /* Everything else could be handled as special thread */
    default:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWaitForSpecial"));
        if (threadData != nullptr)
        {
            // Special thread queue token must not be null in this case
            COPAT_ASSERT(threadData->specialQsTokens);
            specialThreadsPool.waitForThreadSync(redirThread, threadData->specialQsTokens);
        }
        else
        {
            specialThreadsPool.waitForThreadSync(redirThread, nullptr);
        }
        break;
    }
    }
}

bool JobSystem::hasJob(EJobPriority priority) const noexcept
{
    PerThreadData *threadData = getPerThreadData();
    if (threadData == nullptr)
    {
        return false;
    }
    const EJobThreadType thread = threadData->threadType;

    switch (thread)
    {
    case copat::EJobThreadType::MainThread:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("MainHasJob"));
        return !mainThreadJobs[priority].empty();
    }
    case copat::EJobThreadType::WorkerThreads:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("WorkerHasJob"));
        return workerThreadsPool.hasJob(threadData->threadIdx, priority, threadData->workerQsTokens);
    }
    case copat::EJobThreadType::Supervisor:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("SupervisorHasJob"));
        return supervisorThread.hasJob(priority);
    }
    case copat::EJobThreadType::MaxThreads:
        COPAT_ASSERT(false);
        break;
    /* Everything else could be handled as special thread */
    default:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("SpecialHasJob"));
        return specialThreadsPool.hasJob(thread, priority);
    }
    }
    return false;
}
void JobSystem::enqueueJob(
    std::coroutine_handle<> coro, EJobThreadType enqueueToThread /*= EJobThreadType::WorkerThreads*/,
    EJobPriority priority /*= EJobPriority::Priority_Normal*/, [[maybe_unused]] std::source_location src
) noexcept
{
    PerThreadData *threadData = getPerThreadData();
    const EJobThreadType redirThread = enqToThreadType(enqueueToThread);

#if COPAT_DEBUG_JOBS
    if (threadData)
    {
        pushNextEnq({
            .fromThreadIdx = threadData->threadIdx,
            .fromThreadType = threadData->threadType,
            .fromJobSys = this,
            .toThreadType = enqueueToThread,
            .redirThreadType = redirThread,
            .toJobSys = this,
            .jobHndl = coro.address(),
            .parentJobHndl = threadData->currentJobHndl,
            .enqAtSrc = src,
        });
    }
    else
    {
        JobSystem *tlJs = dumpTlJobSysPtr();
        PerThreadData *tlData = tlJs ? tlJs->getPerThreadData() : nullptr;
        pushNextEnq({
            .fromThreadIdx = tlData ? tlData->threadIdx : 0,
            .fromThreadType = tlData ? tlData->threadType : EJobThreadType::MaxThreads,
            .fromJobSys = tlJs,
            .toThreadType = enqueueToThread,
            .redirThreadType = redirThread,
            .toJobSys = this,
            .jobHndl = coro.address(),
            .parentJobHndl = tlData ? tlData->currentJobHndl : nullptr,
            .enqAtSrc = src,
        });
    }
#endif

    switch (redirThread)
    {
    case copat::EJobThreadType::MainThread:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatEnqueueToMain"));

        if (threadData != nullptr)
        {
            mainThreadJobs[priority].enqueue(coro.address(), threadData->mainQTokens[priority]);
        }
        else
        {
            mainThreadJobs[priority].enqueue(coro.address());
        }
        break;
    }
    case copat::EJobThreadType::WorkerThreads:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatEnqueueToWorker"));

        if (threadData != nullptr)
        {
            workerThreadsPool.enqueueJob(coro, priority, threadData->workerQsTokens);
        }
        else
        {
            workerThreadsPool.enqueueJob(coro, priority, nullptr);
        }
        break;
    }
    case copat::EJobThreadType::Supervisor:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatEnqueueToSupervisor"));

        if (threadData != nullptr)
        {
            supervisorThread.enqueueJob(coro, priority, static_cast<SpecialQHazardToken *>(threadData->supervisorTokens));
        }
        else
        {
            supervisorThread.enqueueJob(coro, priority, nullptr);
        }
        break;
    }
    case copat::EJobThreadType::MaxThreads:
        COPAT_ASSERT(false);
        break;
    /* Everything else could be handled as special thread */
    default:
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatEnqueueToSpecial"));
        if (threadData != nullptr)
        {
            // Special thread queue token must not be null in this case
            COPAT_ASSERT(threadData->specialQsTokens);
            specialThreadsPool.enqueueJob(coro, redirThread, priority, threadData->specialQsTokens);
        }
        else
        {
            specialThreadsPool.enqueueJob(coro, redirThread, priority, nullptr);
        }
        break;
    }
    }
}

JobSystem::PerThreadData::PerThreadData(SpecialThreadQueueType *mainQs, JobSystemThread &supervisorThread)
    : threadType(EJobThreadType::WorkerThreads)
    , threadIdx(0)
    , mainQTokens{ mainQs[Priority_Critical].getHazardToken(), mainQs[Priority_Normal].getHazardToken(),
                   mainQs[Priority_Low].getHazardToken() }
    , supervisorTokens{ supervisorThread.getEnqToken(Priority_Critical), supervisorThread.getEnqToken(Priority_Normal),
                        supervisorThread.getEnqToken(Priority_Low) }
    , workerQsTokens(nullptr)
    , specialQsTokens(nullptr)
    , tlUserData(nullptr)
{}

#if COPAT_DEBUG_JOBS
thread_local JobSystem *tl_DUMP_JOBSYSTEM_PTR = nullptr;
#endif
copat::JobSystem::PerThreadData &JobSystem::createPerThreadData(EJobThreadType threadType, u32 threadIdx) noexcept
{
    PerThreadData *threadData = reinterpret_cast<PerThreadData *>(PlatformThreadingFuncs::getTlsSlotValue(tlsSlot));
    COPAT_ASSERT(threadData == nullptr);

    PerThreadData *newThreadData = memNew<PerThreadData>(mainThreadJobs, supervisorThread);
    newThreadData->threadType = threadType;
    newThreadData->threadIdx = threadIdx;
    newThreadData->workerQsTokens = workerThreadsPool.allocateEnqTokens();
    newThreadData->specialQsTokens = specialThreadsPool.allocateEnqTokens();
    newThreadData->tlUserData = tlDataCreate ? tlDataCreate(userData, threadType, threadIdx) : nullptr;

#if COPAT_DEBUG_JOBS
    tl_DUMP_JOBSYSTEM_PTR = this;
#endif

    PlatformThreadingFuncs::setTlsSlotValue(tlsSlot, newThreadData);
    threadData = reinterpret_cast<PerThreadData *>(PlatformThreadingFuncs::getTlsSlotValue(tlsSlot));
    COPAT_ASSERT(threadData == newThreadData);

    return *threadData;
}

void JobSystem::deletePerThreadData(PerThreadData *tlData) noexcept
{
    if (tlDataDelete && tlData->tlUserData != nullptr)
    {
        tlDataDelete(userData, tlData->threadType, tlData->threadIdx, tlData->tlUserData);
    }
    memDelete(tlData);
}

void JobSystem::runMain() noexcept
{
    /* Main thread data gets created and destroy in initialize and shutdown resp. */
    PerThreadData *tlData = getPerThreadData();
    COPAT_ASSERT(tlData->threadType == EJobThreadType::MainThread);

    while (true)
    {
        COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatMainTick"));
        if (bool(mainThreadTick))
        {
            mainThreadTick(userData);
        }

        // Execute all tasks in Higher priority to lower priority order
        void *coroPtr = nullptr;
        for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
             priority = EJobPriority(priority + 1))
        {
            coroPtr = mainThreadJobs[priority].dequeue();
        }
        while (coroPtr != nullptr)
        {
            COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatMainJob"));
#if COPAT_DEBUG_JOBS
            pushNextDeq({
                .fromThreadIdx = 0,
                .execThreadIdx = 0,
                .threadType = EJobThreadType::MainThread,
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
                coroPtr = mainThreadJobs[priority].dequeue();
            }
        }
#if COPAT_DEBUG_JOBS
        tlData->currentJobHndl = nullptr;
#endif

        if (bExitMain[0].test(std::memory_order::relaxed))
        {
            break;
        }
    }
}

void JobSystem::doWorkerJobs(u32 threadIdx) noexcept
{
    PerThreadData *tlData = &createPerThreadData(EJobThreadType::WorkerThreads, threadIdx);
    COPAT_ASSERT(tlData->threadType == EJobThreadType::WorkerThreads);

    auto randomNum = [seed = threadIdx]() mutable
    {
        /* Fast hash */
        seed = (seed ^ 61) ^ (seed >> 16);
        seed = seed + (seed << 3);
        seed = seed ^ (seed >> 4);
        seed = seed * 0x27d4eb2d;
        seed = seed ^ (seed >> 15);
        return seed;
    };
    const bool bEnableJobStealing = (threadingConstraints & THREADCONSTRAINT_ENUM_TO_FLAGBIT(NoJobStealing)) == 0;

    while (true)
    {
        /* Job finding block */
        {
            COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWorker"));

            // Execute all tasks in Higher priority to lower priority order
            void *coroPtr = nullptr;
            for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
                 priority = EJobPriority(priority + 1))
            {
                coroPtr = workerThreadsPool.dequeueJob(threadIdx, priority, tlData->workerQsTokens);
            }
            while (coroPtr != nullptr)
            {
                COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWorkerJob"));
#if COPAT_DEBUG_JOBS
                pushNextDeq({
                    .fromThreadIdx = threadIdx,
                    .execThreadIdx = threadIdx,
                    .threadType = EJobThreadType::WorkerThreads,
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
                    coroPtr = workerThreadsPool.dequeueJob(threadIdx, priority, tlData->workerQsTokens);
                }
            }

            /* Try stealing from one other thread and go to sleep */
            if (bEnableJobStealing)
            {
                COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatStealJob"));
                const u32 stealFromThreadIdx = randomNum() % getWorkersCount();
                for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
                     priority = EJobPriority(priority + 1))
                {
                    coroPtr = workerThreadsPool.stealJob(stealFromThreadIdx, priority, tlData->workerQsTokens);
                }
                while (coroPtr != nullptr)
                {
                    COPAT_PROFILER_SCOPE_VALUE(COPAT_PROFILER_CHAR("CopatStolenJob"), stealFromThreadIdx);
#if COPAT_DEBUG_JOBS
                    pushNextDeq({
                        .fromThreadIdx = stealFromThreadIdx,
                        .execThreadIdx = threadIdx,
                        .threadType = EJobThreadType::WorkerThreads,
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
                        coroPtr = workerThreadsPool.stealJob(stealFromThreadIdx, priority, tlData->workerQsTokens);
                    }
                }
            }
        }
#if COPAT_DEBUG_JOBS
        tlData->currentJobHndl = nullptr;
#endif

        /* Wait for new job unless exiting */
        if (bExitMain[1].test(std::memory_order::relaxed))
        {
            break;
        }
        workerThreadsPool.waitForJob(threadIdx);
    }
    workerThreadsPool.onWorkerThreadExit();
    deletePerThreadData(tlData);
}

void JobSystem::doJobSysThreadJobs(class JobSystemThread &jsThread) noexcept
{
    PerThreadData *tlData = &createPerThreadData(jsThread.threadType, 0);
    COPAT_ASSERT(tlData->threadType == jsThread.threadType);

    while (true)
    {
        // Execute all tasks in Higher priority to lower priority order
        void *coroPtr = nullptr;
        for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority && coroPtr == nullptr;
             priority = EJobPriority(priority + 1))
        {
            coroPtr = jsThread.dequeueJob(priority);
        }
        while (coroPtr != nullptr)
        {
            COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialJob"));
#if COPAT_DEBUG_JOBS
            pushNextDeq({
                .fromThreadIdx = 0,
                .execThreadIdx = 0,
                .threadType = jsThread.threadType,
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
                coroPtr = jsThread.dequeueJob(priority);
            }
        }
#if COPAT_DEBUG_JOBS
        tlData->currentJobHndl = nullptr;
#endif

        if (bExitMain[1].test(std::memory_order::relaxed))
        {
            break;
        }

        jsThread.waitForJob();
    }
    jsThread.onJobSytemThreadExit();

    deletePerThreadData(tlData);
}

u32 JobSystem::calculateWorkersCount() const noexcept
{
    u32 coreCount = 0;
    u32 logicalProcCount = 0;
    getCoreCount(coreCount, logicalProcCount);
    coreCount = coreCount > 4 ? coreCount : 4;
    return coreCount;
}

void JobSystem::setJobSystemName(const TChar *jobSysName)
{
    const u64 nameLen = COPAT_STRLEN(jobSysName);
    const u64 byteSize = sizeof(TChar) * (nameLen + 1);
    TChar *nameStr = reinterpret_cast<TChar *>(CoPaTMemAlloc::memAlloc(byteSize));
    ::memset(nameStr, 0, byteSize);
    ::memcpy(nameStr, jobSysName, sizeof(TChar) * nameLen);

    jsName = nameStr;
}

JobSystem::PerThreadData *JobSystem::getPerThreadData() const noexcept
{
    return reinterpret_cast<PerThreadData *>(PlatformThreadingFuncs::getTlsSlotValue(tlsSlot));
}

void INTERNAL_runSpecialThread(INTERNAL_DoSpecialThreadFuncType threadFunc, EJobThreadType threadType, u32 threadIdx, JobSystem *jobSystem)
{
    u32 coreCount = 0;
    u32 logicalProcCount = 0;
    getCoreCount(coreCount, logicalProcCount);

    std::thread specialThread{ [jobSystem, threadFunc]()
                               {
                                   (jobSystem->*threadFunc)();
                               } };
    TChar threadNameBuffer[257] = { 0 };
    COPAT_PRINTF(threadNameBuffer, COPAT_TCHAR("%s_%s"), jobSystem->getJobSystemName(), JobSystem::SpecialThreadsPoolType::NAMES[threadIdx]);
    PlatformThreadingFuncs::setThreadName(static_cast<const TChar *>(threadNameBuffer), specialThread.native_handle());
    if (coreCount > u32(threadType))
    {
        /* If not enough core just run as free thread */
        PlatformThreadingFuncs::setThreadProcessor(u32(threadType), 0, specialThread.native_handle());
    }
    /* Destroy when finishes */
    specialThread.detach();
}

//////////////////////////////////////////////////////////////////////////
// WorkerThreadsPool implementation
//////////////////////////////////////////////////////////////////////////

void WorkerThreadsPool::initialize(JobSystem *jobSystem, WorkerThreadQueueType::QueueSharedContext &qSharedContext) noexcept
{
    COPAT_ASSERT(workersCount != 0);
    COPAT_ASSERT(jobSystem);
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatWorkerThreadsInit"));

    ownerJobSystem = jobSystem;

    /* Allocate all the data necessary */
    const u64 totalAllocSize = (sizeof(JobReceivedEvent) * workersCount) + (sizeof(WorkerThreadQueueType) * workerQsCount())
                               + (sizeof(WorkerQHazardToken) * hazardTokensCount());
    constexpr u64 allocAlignment = AlignmentOf<JobReceivedEvent, WorkerThreadQueueType, WorkerQHazardToken>;
    workerAllocations = CoPaTMemAlloc::memAlloc(totalAllocSize, allocAlignment);
    u64 workerAllocsAddr = reinterpret_cast<u64>(workerAllocations);

    static_assert(
        alignof(WorkerThreadQueueType) >= alignof(JobReceivedEvent) && alignof(WorkerThreadQueueType) >= alignof(WorkerQHazardToken),
        "[Alignment Error]Allocation order must be redone!"
    );
    /* Worker Queues are in the front */
    workerQs = reinterpret_cast<WorkerThreadQueueType *>(workerAllocsAddr);
    /* Worker job events are after Worker Queues */
    workerAllocsAddr += sizeof(WorkerThreadQueueType) * workerQsCount();
    workerJobEvents = reinterpret_cast<JobReceivedEvent *>(workerAllocsAddr);
    /* Hazard tokens are after both Worker Queues and Worker Job events */
    workerAllocsAddr += sizeof(JobReceivedEvent) * workersCount;
    hazardTokens = reinterpret_cast<WorkerQHazardToken *>(workerAllocsAddr);

    /* Initialize Qs, Job events. Hazard tokens will defer initialization until allocated */
    for (u32 i = 0; i < workerQsCount(); ++i)
    {
        new (workerQs + i) WorkerThreadQueueType();
        (workerQs + i)->setupQueue(qSharedContext);
    }
    for (u32 i = 0; i < workersCount; ++i)
    {
        new (workerJobEvents + i) JobReceivedEvent();
    }
}

void WorkerThreadsPool::run(INTERNAL_DoWorkerThreadFuncType doWorkerJobFunc, bool bSetAffinity) noexcept
{
    /* Run all threads */
    u32 coreCount = 0;
    u32 logicalProcCount = 0;
    getCoreCount(coreCount, logicalProcCount);
    const u32 htCount = logicalProcCount / coreCount;

    u32 nonWorkerCount = u32(EJobThreadType::WorkerThreads);
    u32 coresForWorkers = coreCount;
    if (coreCount <= nonWorkerCount)
    {
        /* If there is very limited cores then let OS handle better scheduling, we just distribute workers evenly */
        nonWorkerCount = 0;
    }
    else
    {
        coresForWorkers = coreCount - nonWorkerCount;
    }

    for (u32 i = 0; i < workersCount; ++i)
    {
        const u32 coreIdx = (i % coresForWorkers) + nonWorkerCount;
        const u32 htIdx = (i / coresForWorkers) % htCount;

        // Create and setup thread
        std::thread worker{ [this, doWorkerJobFunc, i]()
                            {
                                (ownerJobSystem->*doWorkerJobFunc)(i);
                            } };
        TChar threadNameBuffer[257] = { 0 };
        COPAT_PRINTF(threadNameBuffer, COPAT_TCHAR("%s_WorkerThread_%u"), ownerJobSystem->getJobSystemName(), i);
        PlatformThreadingFuncs::setThreadName(static_cast<const TChar *>(threadNameBuffer), worker.native_handle());
        /* If Worker is strictly tied to a logic processor */
        if (bSetAffinity)
        {
            PlatformThreadingFuncs::setThreadProcessor(coreIdx, htIdx, worker.native_handle());
        }
        else
        {
            PlatformThreadingFuncs::GroupAffinityMaskBuilder affinityBuilder;
            affinityBuilder.setAll();
            affinityBuilder.setGroupFrom(coreIdx);
            affinityBuilder.clearUpto(nonWorkerCount, 0);
            PlatformThreadingFuncs::setThreadGroupAffinity(
                affinityBuilder.getGroupIdx(), affinityBuilder.getAffinityMask(), worker.native_handle()
            );
        }

        /* Destroy when finishes */
        worker.detach();
    }
}

bool WorkerThreadsPool::tryShutdown() noexcept
{
    COPAT_ASSERT(workersCount != 0);
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatSpecialThreadsShutdown"));

    for (u32 i = 0; i < workersCount; ++i)
    {
        workerJobEvents[i].notify();
    }
    if (!allWorkersExitEvent.try_wait())
    {
        return false;
    }

    /* Only queues needs to be manually destructed */
    for (u32 i = 0; i < workerQsCount(); ++i)
    {
        workerQs[i].~FAAArrayQueue();
    }

    /* Free all the data in one go */
    CoPaTMemAlloc::memFree(workerAllocations);
    workerAllocations = nullptr;
    workerJobEvents = nullptr;
    workerQs = nullptr;

    return true;
}

bool WorkerThreadsPool::hasJob(u32 threadIdx, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) const noexcept
{
    bool bAnyJobs = false;
    for (EJobPriority p = Priority_Critical; p <= priority; p = EJobPriority(p + 1))
    {
        const u32 qIdx = pAndTTypeToIdx(threadIdx, priority);
        bAnyJobs = bAnyJobs || !workerQs[qIdx].empty(fromThreadTokens[qIdx]);
    }
    return bAnyJobs;
}
void WorkerThreadsPool::waitForThreadSync(WorkerQHazardToken *fromThreadTokens, u32 skipThreadIdx) noexcept
{
    COPAT_ASSERT(!allWorkersExitEvent.try_wait());
    /* Reduce one if skipping thread is valid */
    std::latch allWorkerSync{ workersCount - (workersCount > skipThreadIdx ? 1 : 0) };
    auto coroFunc = [&allWorkerSync]() -> DummyCoroutine
    {
        allWorkerSync.count_down();
        co_return;
    };
    for (u32 threadIdx = 0; threadIdx < workersCount; ++threadIdx)
    {
        if (skipThreadIdx == threadIdx)
        {
            continue;
        }

        const DummyCoroutine coroHndl = coroFunc();
        const u32 qIdx = pAndTTypeToIdx(threadIdx, EJobPriority::Priority_Low);
        if (fromThreadTokens != nullptr)
        {
            workerQs[qIdx].enqueue(coroHndl.coro.address(), fromThreadTokens[qIdx]);
        }
        else
        {
            workerQs[qIdx].enqueue(coroHndl.coro.address());
        }
    }
    /* Notify all together */
    for (u32 threadIdx = 0; threadIdx < workersCount; ++threadIdx)
    {
        workerJobEvents[threadIdx].notify();
    }
    allWorkerSync.wait();
}

void WorkerThreadsPool::enqueueJob(std::coroutine_handle<> coro, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept
{
    COPAT_ASSERT(!allWorkersExitEvent.try_wait());

    /* Unsigned int overflow is a defined behavior, Equivalent to modulo by u32 max
     * Just the number matters ordering is not necessary here
     */
    const u32 threadIdx = nextEnqToQ.fetch_add(1, std::memory_order::relaxed) % workersCount;
    const u32 qIdx = pAndTTypeToIdx(threadIdx, priority);

    if (fromThreadTokens != nullptr)
    {
        workerQs[qIdx].enqueue(coro.address(), fromThreadTokens[qIdx]);
    }
    else
    {
        workerQs[qIdx].enqueue(coro.address());
    }
    workerJobEvents[threadIdx].notify();
}
void *WorkerThreadsPool::dequeueJob(u32 threadIdx, EJobPriority priority, WorkerQHazardToken *fromThreadTokens) noexcept
{
    COPAT_ASSERT(fromThreadTokens);
    const u32 qIdx = pAndTTypeToIdx(threadIdx, priority);
    return workerQs[qIdx].dequeue(fromThreadTokens[qIdx]);
}
void *WorkerThreadsPool::stealJob(u32 stealFromIdx, EJobPriority stealPriority, WorkerQHazardToken *fromThreadTokens) noexcept
{
    COPAT_ASSERT(fromThreadTokens);
    const u32 qIdx = pAndTTypeToIdx(stealFromIdx, stealPriority);
    return workerQs[qIdx].dequeue(fromThreadTokens[qIdx]);
}

void WorkerThreadsPool::waitForJob(u32 workerIdx) noexcept { workerJobEvents[workerIdx].wait(); }

void WorkerThreadsPool::onWorkerThreadExit() noexcept { allWorkersExitEvent.count_down(); }

WorkerQHazardToken *WorkerThreadsPool::allocateEnqTokens() noexcept
{
    u32 tokenIdx = hazardTokensTop.fetch_add(workersCount * Priority_MaxPriority, std::memory_order::acq_rel);
    // Tokens can be null if worker thread is disabled
    if (tokenIdx >= hazardTokensCount())
    {
        return nullptr;
    }
    WorkerQHazardToken *tokens = hazardTokens + tokenIdx;

    for (u32 threadIdx = 0; threadIdx < workersCount; ++threadIdx)
    {
        for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority; priority = EJobPriority(priority + 1))
        {
            new (tokens + pAndTTypeToIdx(threadIdx, priority))
                WorkerQHazardToken(workerQs[pAndTTypeToIdx(threadIdx, priority)].getHazardToken());
        }
    }
    return tokens;
}

u32 WorkerThreadsPool::hazardTokensCount() const { return ownerJobSystem ? ownerJobSystem->getTotalThreadsCount() * workerQsCount() : 0; }

//////////////////////////////////////////////////////////////////////////
// JobSystemThread implementation
//////////////////////////////////////////////////////////////////////////

void JobSystemThread::initialize(InitInfo info)
{
    COPAT_ASSERT(info.jobSystem);
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatJobSysThreadInit"));

    ownerJobSystem = info.jobSystem;

    for (EJobPriority priority = Priority_Critical; priority < Priority_MaxPriority; priority = EJobPriority(priority + 1))
    {
        getThreadJobsQueue(priority).setupQueue(info.qSharedContent);
    }

    threadName = info.threadName;
    threadType = info.threadType;
}

void JobSystemThread::run(INTERNAL_DoJobSystemThreadFuncType doThreadJob, bool bSetAffinity)
{
    u32 coreCount = 0;
    u32 logicalProcCount = 0;
    getCoreCount(coreCount, logicalProcCount);

    std::thread jsThread{ [this, doThreadJob]()
                          {
                              (ownerJobSystem->*doThreadJob)(*this);
                          } };
    TChar threadNameBuffer[257] = { 0 };
    COPAT_PRINTF(threadNameBuffer, COPAT_TCHAR("%s_%s"), ownerJobSystem->getJobSystemName(), threadName);
    PlatformThreadingFuncs::setThreadName(static_cast<const TChar *>(threadNameBuffer), jsThread.native_handle());
    if (bSetAffinity && coreCount > u32(threadType))
    {
        /* If not enough core just run as free thread */
        PlatformThreadingFuncs::setThreadProcessor(u32(threadType), 0, jsThread.native_handle());
    }
    /* Destroy when finishes */
    jsThread.detach();
}
bool JobSystemThread::tryShutdown() noexcept
{
    COPAT_PROFILER_SCOPE(COPAT_PROFILER_CHAR("CopatJobSysThreadShutdown"));

    jobReceiveEvent.notify();
    return exitEvent.try_wait();
}

bool JobSystemThread::hasJob(EJobPriority priority) const noexcept
{
    bool bAnyJobs = false;
    for (EJobPriority p = Priority_Critical; p <= priority; p = EJobPriority(p + 1))
    {
        bAnyJobs = bAnyJobs || !getThreadJobsQueue(p).empty();
    }
    return bAnyJobs;
}
void JobSystemThread::waitForThreadSync(SpecialQHazardToken *fromThreadTokens) noexcept
{
    std::atomic_flag waitFlag;
    const DummyCoroutine coroHndl = [](std::atomic_flag &waitFlag) -> DummyCoroutine
    {
        waitFlag.test_and_set(std::memory_order::relaxed);
        waitFlag.notify_one();
        co_return;
    }(waitFlag);

    if (fromThreadTokens != nullptr)
    {
        getThreadJobsQueue(EJobPriority::Priority_Low).enqueue(coroHndl.coro.address(), fromThreadTokens[EJobPriority::Priority_Low]);
    }
    else
    {
        getThreadJobsQueue(EJobPriority::Priority_Low).enqueue(coroHndl.coro.address());
    }

    jobReceiveEvent.notify();
    waitFlag.wait(false, std::memory_order::relaxed);
}

void JobSystemThread::enqueueJob(std::coroutine_handle<> coro, EJobPriority priority, SpecialQHazardToken *fromThreadTokens) noexcept
{
    /* We must not enqueue at shutdown */
    COPAT_ASSERT(!exitEvent.try_wait());
    if (fromThreadTokens != nullptr)
    {
        getThreadJobsQueue(priority).enqueue(coro.address(), fromThreadTokens[priority]);
    }
    else
    {
        getThreadJobsQueue(priority).enqueue(coro.address());
    }

    jobReceiveEvent.notify();
}
void *JobSystemThread::dequeueJob(EJobPriority priority) noexcept { return getThreadJobsQueue(priority).dequeue(); }

void JobSystemThread::waitForJob() { jobReceiveEvent.wait(); }
void JobSystemThread::onJobSytemThreadExit() { exitEvent.count_down(); }

SpecialQHazardToken JobSystemThread::getEnqToken(EJobPriority priority) { return getThreadJobsQueue(priority).getHazardToken(); }
SpecialThreadQueueType &JobSystemThread::getThreadJobsQueue(EJobPriority priority) { return queues[priority]; }
const SpecialThreadQueueType &JobSystemThread::getThreadJobsQueue(EJobPriority priority) const { return queues[priority]; }

} // namespace copat

#if COPAT_DEBUG_JOBS
namespace copat
{
void JobSystem::pushNextEnq(EnqueueDump &&dump)
{
    std::vector<EnqueueDump> eQDumps;
    std::vector<DequeueDump> dQDumps;

    dumpingMutex.lock();
    enqDumpIdx++;
    if (enqDumpIdx == MAX_ENQ_DEQ_DUMPS)
    {
        eQDumps = std::move(enQsDumpList);
        dQDumps = std::move(dQsDumpList);

        dQDumps.resize(dqDumpIdx + 1);

        enQsDumpList.resize(MAX_ENQ_DEQ_DUMPS);
        dQsDumpList.resize(MAX_ENQ_DEQ_DUMPS);
        enqDumpIdx = dqDumpIdx = 0;
    }
    dumpingMutex.unlock();

    if (!eQDumps.empty() || !dQDumps.empty())
    {
        COPAT_DEBUG_JOBS_DUMP(this, eQDumps.data(), eQDumps.size(), dQDumps.data(), dQDumps.size());
    }

    enQsDumpList[enqDumpIdx] = std::forward<EnqueueDump>(dump);
}
void JobSystem::pushNextDeq(DequeueDump &&dump)
{
    std::vector<EnqueueDump> eQDumps;
    std::vector<DequeueDump> dQDumps;

    dumpingMutex.lock();
    dqDumpIdx++;
    if (dqDumpIdx == MAX_ENQ_DEQ_DUMPS)
    {
        eQDumps = std::move(enQsDumpList);
        dQDumps = std::move(dQsDumpList);

        eQDumps.resize(enqDumpIdx + 1);

        enQsDumpList.resize(MAX_ENQ_DEQ_DUMPS);
        dQsDumpList.resize(MAX_ENQ_DEQ_DUMPS);
        enqDumpIdx = dqDumpIdx = 0;
    }
    dumpingMutex.unlock();

    if (!eQDumps.empty() || !dQDumps.empty())
    {
        COPAT_DEBUG_JOBS_DUMP(this, eQDumps.data(), eQDumps.size(), dQDumps.data(), dQDumps.size());
    }

    dQsDumpList[dqDumpIdx] = std::forward<DequeueDump>(dump);
}

JobSystem *JobSystem::dumpTlJobSysPtr() { return tl_DUMP_JOBSYSTEM_PTR; }

} // namespace copat
#endif