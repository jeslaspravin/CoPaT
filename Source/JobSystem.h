/*!
 * \file JobSystem.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, Since 2022
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "FAAArrayQueue.hpp"
#include "SyncPrimitives.h"
#include "Platform/PlatformThreadingFunctions.h"

#include <coroutine>
#include <semaphore>
#include <latch>

COPAT_NS_INLINED
namespace copat
{

class COPAT_EXPORT_SYM JobSystem
{
public:
    constexpr static const u32 MAX_SUPPORTED_WORKERS = 128;

    using MainThreadTickFunc = FunctionType<void, void *>;

    using MainThreadQueueType = FAAArrayMPSCQueue<void>;
    using WorkerThreadQueueType = FAAArrayQueue<void>;

    struct PerThreadData
    {
        bool bIsMainThread;
        WorkerThreadQueueType::HazardToken enqDqToken;

        PerThreadData(WorkerThreadQueueType::HazardToken&& hazardToken)
            : bIsMainThread(false)
            , enqDqToken(std::forward<WorkerThreadQueueType::HazardToken>(hazardToken))
        {}
    };

private:
    WorkerThreadQueueType workerJobs;
    // Binary semaphore wont work if two jobs arrive at same time, and one of 2 just ends up waiting until another job arrives
    // std::binary_semaphore workerJobEvent{0};
    std::counting_semaphore<2 * MAX_SUPPORTED_WORKERS> workerJobEvent{ 0 };
    // To ensure that we do not trigger workerJobEvent when there is no free workers
    std::atomic_uint_fast64_t availableWorkersCount{ 0 };
    // For waiting until all workers are finished
    std::latch workersFinishedEvent{ calculateWorkersCount() };

    MainThreadQueueType mainThreadJobs;
    std::atomic_flag bExitMain;
    // Main thread tick function type, This function gets ticked in main thread for every loop and then main job queue will be emptied
    MainThreadTickFunc mainThreadTick;
    void *userData;

    u32 tlsSlot;

    static JobSystem *singletonInstance;

public:
    static JobSystem *get() { return singletonInstance; }

    void initialize(MainThreadTickFunc &&mainTickFunc, void *inUserData);
    void joinMain() { runMain(); }

    void exitMain() { bExitMain.test_and_set(std::memory_order::release); }

    void shutdown()
    {
        // Just setting bExitMain flag to expected when shutting down
        bExitMain.test_and_set(std::memory_order::relaxed);

        // Binary semaphore
        // while (!workersFinishedEvent.try_wait())
        //{
        //    workerJobEvent.release();
        //}

        // Counting semaphore
        // Drain the worker job events if any so we can release all workers
        while (workerJobEvent.try_acquire())
        {}
        workerJobEvent.release(calculateWorkersCount());
        workersFinishedEvent.wait();
    }

    void enqueueJob(std::coroutine_handle<> coro, bool bEnqueueToMain = false)
    {
        PerThreadData *threadData = getPerThreadData();
        if (bEnqueueToMain)
        {
            mainThreadJobs.enqueue(coro.address());
        }
        else
        {
            workerJobs.enqueue(coro.address(), threadData->enqDqToken);
            // We do not have to be very strict here as long as one or two is free and we get 0 or nothing is free and we release one or two
            // more it is fine
            if (availableWorkersCount.load(std::memory_order::relaxed) != 0)
            {
                workerJobEvent.release();
            }
        }
    }

private:
    PerThreadData *getPerThreadData()
    {
        PerThreadData *threadData = (PerThreadData *)PlatformThreadingFuncs::getTlsSlotValue(tlsSlot);
        if (!threadData)
        {
            PlatformThreadingFuncs::setTlsSlotValue(tlsSlot, memNew<PerThreadData>(std::move(workerJobs.getHazardToken())));
            threadData = (PerThreadData *)PlatformThreadingFuncs::getTlsSlotValue(tlsSlot);
        }
        return threadData;
    }

    u32 calculateWorkersCount() const
    {
        u32 count = std::thread::hardware_concurrency() / 2;
        count = count > 4 ? count : 4;
        return count > MAX_SUPPORTED_WORKERS ? MAX_SUPPORTED_WORKERS : count;
    }

    void runMain()
    {
        PerThreadData *tlData = getPerThreadData();
        tlData->bIsMainThread = true;
        while (true)
        {
            if (bool(mainThreadTick))
            {
                mainThreadTick(userData);
            }

            while (void *coroPtr = mainThreadJobs.dequeue())
            {
                std::coroutine_handle<>::from_address(coroPtr).resume();
            }

            if (bExitMain.test(std::memory_order::relaxed))
            {
                break;
            }
        }
        memDelete(tlData);
    }

    void doWorkerJobs()
    {
        PerThreadData *tlData = getPerThreadData();
        while (true)
        {
            while (void *coroPtr = workerJobs.dequeue(tlData->enqDqToken))
            {
                std::coroutine_handle<>::from_address(coroPtr).resume();
            }

            if (bExitMain.test(std::memory_order::relaxed))
            {
                break;
            }

            // Marking that we are becoming available
            availableWorkersCount.fetch_add(1, std::memory_order::memory_order_acq_rel);
            // Wait until job available
            workerJobEvent.acquire();
            // Starting work
            availableWorkersCount.fetch_sub(1, std::memory_order::memory_order_acq_rel);
        }
        workersFinishedEvent.count_down();
        memDelete(tlData);
    }
};

} // namespace copat