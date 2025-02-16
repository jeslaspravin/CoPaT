/*!
 * \file DispatchHelpers.cpp
 *
 * \author Jeslas
 * \date June 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2025
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#include "DispatchHelpers.h"
#include "CoroutineAwaitAll.h"
#include "JobSystemCoroutine.h"
#include "CoroutineWait.h"

COPAT_NS_INLINED
namespace copat
{
// Just copying the callback so a copy exists inside dispatch
static DispatchAwaitableType dispatchOneTask(JobSystem *, EJobPriority, DispatchFunctionType callback, u64 jobIdx) noexcept
{
    callback(jobIdx);
    co_return;
}
static DispatchAwaitableType dispatchTaskGroup(JobSystem *, EJobPriority, DispatchFunctionType callback, u64 fromJobIdx, u64 count) noexcept
{
    const u64 endJobIdx = fromJobIdx + count;
    for (u64 jobIdx = fromJobIdx; jobIdx < endJobIdx; ++jobIdx)
    {
        callback(jobIdx);
    }
    co_return;
}

AwaitAllTasks<std::vector<DispatchAwaitableType>> dispatch(
    JobSystem *jobSys, const DispatchFunctionType &callback, u64 count, EJobPriority jobPriority /* = EJobPriority::Priority_Normal */
) noexcept
{
    if (count == 0)
    {
        return {};
    }

    /**
     * If there is no worker threads and the current thread is same as queuing thread then execute serially.
     * This means workers can enqueue to more workers, Beware of deadlocks when queuing from workers
     */
    if (jobSys != nullptr
        || (jobSys->enqToThreadType(EJobThreadType::WorkerThreads) != EJobThreadType::WorkerThreads
            && jobSys->enqToThreadType(EJobThreadType::WorkerThreads) == jobSys->getCurrentThreadType()))
    {
        /* No job system just call all functions serially */
        for (u64 i = 0; i < count; ++i)
        {
            callback(i);
        }
        return {};
    }

    std::vector<DispatchAwaitableType> dispatchedJobs;
    /**
     * If there is no worker threads then we cannot actually diverge.
     * Queuing the jobs to the corresponding enqueue to thread.
     * Caller must ensure not to trigger dead lock when calling converge.
     */
    if (jobSys->enqToThreadType(EJobThreadType::WorkerThreads) != EJobThreadType::WorkerThreads)
    {
        dispatchedJobs.emplace_back(dispatchTaskGroup(jobSys, jobPriority, callback, 0, count));
        return awaitAllTasks(std::move(dispatchedJobs));
    }

    const u32 grpCount = jobSys->getWorkersCount();
    const u64 jobsPerGrp = count / grpCount;
    // If dispatching count is less than max workers count
    if (jobsPerGrp == 0)
    {
        dispatchedJobs.reserve(count);
        for (u64 i = 0; i < count; ++i)
        {
            dispatchedJobs.emplace_back(dispatchOneTask(jobSys, jobPriority, callback, i));
        }
    }
    else
    {
        dispatchedJobs.reserve(grpCount);
        const u32 grpsWithMoreJobCount = count % grpCount;
        u64 jobIdx = 0;
        for (u32 i = 0; i < grpsWithMoreJobCount; ++i)
        {
            // Add one more job for all grps with more jobs
            dispatchedJobs.emplace_back(dispatchTaskGroup(jobSys, jobPriority, callback, jobIdx, jobsPerGrp + 1));
            jobIdx += jobsPerGrp + 1;
        }

        for (u32 i = grpsWithMoreJobCount; i < grpCount; ++i)
        {
            dispatchedJobs.emplace_back(dispatchTaskGroup(jobSys, jobPriority, callback, jobIdx, jobsPerGrp));
            jobIdx += jobsPerGrp;
        }
    }
    return awaitAllTasks(std::move(dispatchedJobs));
}

void parallelFor(
    JobSystem *jobSys, const DispatchFunctionType &callback, u64 count, EJobPriority jobPriority /*= EJobPriority::Priority_Normal */
) noexcept
{
    if (count == 0)
    {
        return;
    }

    COPAT_ASSERT(jobSys);

    const u32 grpCount = jobSys->getWorkersCount();

    // If dispatching count is less than max workers count then jobsPerGrp will be 1
    // Else it will be jobsPerGrp or jobsPerGrp + 1 depending on count equiv-distributable among workers
    u64 jobsPerGrp = count / grpCount;
    jobsPerGrp += static_cast<u64>((count % grpCount) > 0);

    AwaitAllTasks<std::vector<DispatchAwaitableType>> allAwaits = dispatch(jobSys, callback, count - jobsPerGrp, jobPriority);
    for (u64 jobIdx = count - jobsPerGrp; jobIdx < count; ++jobIdx)
    {
        callback(jobIdx);
    }
    waitOnAwaitable(std::move(allAwaits));
}

} // namespace copat
