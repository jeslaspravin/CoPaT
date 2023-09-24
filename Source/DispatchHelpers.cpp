/*!
 * \file DispatchHelpers.cpp
 *
 * \author Jeslas
 * \date June 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2023
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
using DispatchInfo = DispatchInfoBase<void>;
using GroupDispatchInfo = GroupDispatchInfoBase<void>;

// Just copying the callback so a copy exists inside dispatch
DispatchAwaitableType dispatchOneTask(JobSystem &jobSys, EJobPriority jobPriority, DispatchInfo info) noexcept
{
    DispatchAwaitableType childDispatch{ nullptr };
    if (info.dispatchCount)
    {
        DispatchInfo newInfo = info;
        newInfo.dispatchCount--;
        newInfo.jobIdx++;

        childDispatch = std::move(dispatchOneTask(jobSys, jobPriority, newInfo));
    }
    info.callback(info.jobIdx);
    co_await childDispatch;
}

DispatchAwaitableType dispatchTaskGroup(JobSystem &jobSys, EJobPriority jobPriority, GroupDispatchInfo info) noexcept
{
    DispatchAwaitableType childDispatch{ nullptr };
    if (info.base.dispatchCount)
    {
        GroupDispatchInfo newInfo = info;
        newInfo.base.dispatchCount--;
        newInfo.grpIdx++;
        newInfo.base.jobIdx += newInfo.jobsPerGrp;

        childDispatch = std::move(dispatchTaskGroup(jobSys, jobPriority, newInfo));
    }
    const u32 endJobIdx = info.base.jobIdx + info.jobsPerGrp;
    for (u32 jobIdx = info.base.jobIdx; jobIdx < endJobIdx; ++jobIdx)
    {
        info.base.callback(jobIdx);
    }
    co_await childDispatch;
}
DispatchAwaitableType dispatchExtendedTaskGroup(JobSystem &jobSys, EJobPriority jobPriority, GroupDispatchInfo info) noexcept
{
    DispatchAwaitableType childDispatch{ nullptr };
    if (info.base.dispatchCount)
    {
        GroupDispatchInfo newInfo = info;
        newInfo.base.dispatchCount--;
        newInfo.grpIdx++;
        newInfo.base.jobIdx += newInfo.jobsPerGrp + 1;
        if (newInfo.extendedGrpsCount)
        {
            newInfo.extendedGrpsCount--;

            childDispatch = std::move(dispatchExtendedTaskGroup(jobSys, jobPriority, newInfo));
        }
        else
        {
            childDispatch = std::move(dispatchTaskGroup(jobSys, jobPriority, newInfo));
        }
    }
    const u32 endJobIdx = info.base.jobIdx + info.jobsPerGrp + 1;
    for (u32 jobIdx = info.base.jobIdx; jobIdx < endJobIdx; ++jobIdx)
    {
        info.base.callback(jobIdx);
    }
    co_await childDispatch;
}

DispatchAwaitableType dispatch(
    JobSystem *jobSys, const DispatchFunctionType &callback, u32 count, EJobPriority jobPriority /* = EJobPriority::Priority_Normal */
) noexcept
{
    if (count == 0)
    {
        return { nullptr };
    }

    // If there is no worker threads and the current thread is same as queuing thread then execute serially.
    // This means workers can enqueue to more workers, Beware of deadlocks when queuing from workers
    if (!jobSys
        || (jobSys->enqToThreadType(EJobThreadType::WorkerThreads) != EJobThreadType::WorkerThreads
            && jobSys->enqToThreadType(EJobThreadType::WorkerThreads) == jobSys->getCurrentThreadType()))
    {
        // No job system just call all functions serially
        for (u32 i = 0; i < count; ++i)
        {
            callback(i);
        }
        return { nullptr };
    }

    const u32 grpCount = jobSys->getWorkersCount();

    DispatchAwaitableType dispatchedJobs{ nullptr };
    // If dispatching count is less than or equal to max group count
    if (count <= grpCount)
    {
        dispatchedJobs = dispatchOneTask(*jobSys, jobPriority, { .callback = callback, .jobIdx = 0, .dispatchCount = (count - 1) });
    }
    else
    {
        u32 jobsPerGrp = count / grpCount;
        u32 grpsWithMoreJobCount = count % grpCount;
        if (grpsWithMoreJobCount > 0)
        {
            GroupDispatchInfo info{
                .base = {.callback = callback, .jobIdx = 0, .dispatchCount = (grpCount - 1)},
                .jobsPerGrp = jobsPerGrp,
                .extendedGrpsCount = (grpsWithMoreJobCount - 1),
                .grpIdx = 0
            };
            dispatchedJobs = dispatchExtendedTaskGroup(*jobSys, jobPriority, info);
        }
        else
        {
            GroupDispatchInfo info{
                .base = {.callback = callback, .jobIdx = 0, .dispatchCount = (grpCount - 1)},
                .jobsPerGrp = jobsPerGrp,
                .extendedGrpsCount = 0,
                .grpIdx = 0
            };
            dispatchedJobs = dispatchTaskGroup(*jobSys, jobPriority, info);
        }
    }
    return std::move(dispatchedJobs);
}

void parallelFor(
    JobSystem *jobSys, const DispatchFunctionType &callback, u32 count, EJobPriority jobPriority /*= EJobPriority::Priority_Normal */
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
    u32 jobsPerGrp = count / grpCount;
    jobsPerGrp += (count % grpCount) > 0;

    DispatchAwaitableType awaitable = dispatch(jobSys, callback, count - jobsPerGrp, jobPriority);
    for (u32 jobIdx = count - jobsPerGrp; jobIdx < count; ++jobIdx)
    {
        callback(jobIdx);
    }
    waitOnAwaitable(std::move(awaitable));
}

} // namespace copat
