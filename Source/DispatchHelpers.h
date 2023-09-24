/*!
 * \file DispatchHelpers.h
 *
 * \author Jeslas
 * \date June 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2023
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "JobSystem.h"

COPAT_NS_INLINED
namespace copat
{
class JobSystem;
class JobSystemPromiseBase;

template <typename RetType, typename BasePromiseType, bool EnqAtInitialSuspend, EJobThreadType EnqueueInThread, EJobPriority Priority>
class JobSystemTaskType;

template <typename AwaitingCollection>
class AwaitAllTasks;

struct NormalFuncAwaiter;

template <typename RetType>
using DispatchAwaitableTypeWithRet
    = JobSystemTaskType<RetType, JobSystemPromiseBase, true, EJobThreadType::WorkerThreads, EJobPriority::Priority_Normal>;
template <typename RetType>
using DispatchFunctionTypeWithRet = FunctionType<RetType, u32>;

template <typename RetType>
struct DispatchInfoBase
{
    /* Job to run */
    DispatchFunctionTypeWithRet<RetType> callback;

    /* This job index, Increment if dispatching more from this coroutine */
    u32 jobIdx;
    /* Count of dispatches left, excluding current coroutine. Decrement before each dispatch */
    u32 dispatchCount;
};

template <typename RetType>
struct GroupDispatchInfoBase
{
    DispatchInfoBase<RetType> base;
    u32 jobsPerGrp;
    /* Number of groups that has +1 jobs, Decrement before each extended grps dispatch */
    u32 extendedGrpsCount;
    /* Used to index outAwaitables, Increment before each group dispatch */
    u32 grpIdx;
};

using DispatchAwaitableType = DispatchAwaitableTypeWithRet<void>;
using DispatchFunctionType = DispatchFunctionTypeWithRet<void>;

COPAT_EXPORT_SYM DispatchAwaitableType
dispatch(JobSystem *jobSys, const DispatchFunctionType &callback, u32 count, EJobPriority jobPriority = EJobPriority::Priority_Normal) noexcept;

// Dispatch and wait immediately
COPAT_EXPORT_SYM void parallelFor(
    JobSystem *jobSys, const DispatchFunctionType &callback, u32 count, EJobPriority jobPriority = EJobPriority::Priority_Normal
) noexcept;

template <typename FuncType, typename... Args>
NormalFuncAwaiter fireAndForget(FuncType &&func, Args &&...args) noexcept
{
    FuncType funcCopy = std::forward<FuncType>(func);
    co_await funcCopy(std::forward<Args>(args)...);
}

template <typename RetType>
struct DispatchWithReturn
{
    using AwaitableType = DispatchAwaitableTypeWithRet<std::vector<RetType>>;
    using FuncType = DispatchFunctionTypeWithRet<RetType>;
    using DispatchInfoType = DispatchInfoBase<RetType>;
    using GroupDispatchInfoType = GroupDispatchInfoBase<RetType>;

    // Just copying the callback so a copy exists inside dispatch
    static AwaitableType dispatchTask(JobSystem &jobSys, EJobPriority jobPriority, DispatchInfoType info) noexcept
    {
        AwaitableType childDispatch{ nullptr };
        if (info.dispatchCount)
        {
            DispatchInfoType newInfo = info;
            newInfo.dispatchCount--;
            newInfo.jobIdx++;

            childDispatch = std::move(dispatchTask(jobSys, jobPriority, newInfo));
        }

        /* Preallocate required size, before awaiting children */
        std::vector<RetType> retVal;
        retVal.reserve(1 + info.dispatchCount);

        /* Execute job */
        retVal.emplace_back(info.callback(info.jobIdx));

        /* Only await for return if dispatched */
        if (info.dispatchCount)
        {
            std::vector<RetType> childRets = co_await childDispatch;
            for (RetType &childRet : childRets)
            {
                retVal.emplace_back(std::move(childRet));
            }
        }

        co_return retVal;
    }
    static AwaitableType dispatchTaskGroup(JobSystem &jobSys, EJobPriority jobPriority, GroupDispatchInfoType info) noexcept
    {
        AwaitableType childDispatch{ nullptr };
        if (info.base.dispatchCount)
        {
            GroupDispatchInfoType newInfo = info;
            newInfo.base.dispatchCount--;
            newInfo.grpIdx++;
            newInfo.base.jobIdx += newInfo.jobsPerGrp;

            childDispatch = std::move(dispatchTaskGroup(jobSys, jobPriority, newInfo));
        }
        /* Preallocate required size, before awaiting children */
        std::vector<RetType> retVal;
        retVal.reserve(info.jobsPerGrp + info.base.dispatchCount * info.jobsPerGrp);

        /* Execute jobs */
        const u32 endJobIdx = info.base.jobIdx + info.jobsPerGrp;
        for (u32 jobIdx = info.base.jobIdx; jobIdx < endJobIdx; ++jobIdx)
        {
            retVal.emplace_back(info.base.callback(jobIdx));
        }

        /* Only await for return if dispatched */
        if (info.base.dispatchCount)
        {
            std::vector<RetType> childRets = co_await childDispatch;
            for (RetType &childRet : childRets)
            {
                retVal.emplace_back(std::move(childRet));
            }
        }

        co_return retVal;
    }
    static AwaitableType dispatchExtendedTaskGroup(JobSystem &jobSys, EJobPriority jobPriority, GroupDispatchInfoType info) noexcept
    {
        AwaitableType childDispatch{ nullptr };
        if (info.base.dispatchCount)
        {
            GroupDispatchInfoType newInfo = info;
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
        /* Preallocate required size, before awaiting children */
        std::vector<RetType> retVal;
        retVal.reserve(info.jobsPerGrp + 1 + info.base.dispatchCount * info.jobsPerGrp + info.extendedGrpsCount);

        /* Execute jobs */
        const u32 endJobIdx = info.base.jobIdx + info.jobsPerGrp + 1;
        for (u32 jobIdx = info.base.jobIdx; jobIdx < endJobIdx; ++jobIdx)
        {
            retVal.emplace_back(info.base.callback(jobIdx));
        }

        /* Only await for return if dispatched */
        if (info.base.dispatchCount)
        {
            std::vector<RetType> childRets = co_await childDispatch;
            for (RetType &childRet : childRets)
            {
                retVal.emplace_back(std::move(childRet));
            }
        }

        co_return retVal;
    }

    static AwaitableType dispatch(JobSystem *jobSys, const FuncType &callback, u32 count, EJobPriority jobPriority) noexcept
    {
        if (count == 0)
        {
            return { nullptr };
        }

        /**
         * If there is no worker threads then we cannot dispatch so fail.
         * Asserting here as there is no direct way to return as AwaitableType immediately.
         * TODO(Jeslas) : Find an work around if this becomes a problem.S
         * Maybe I could have a coroutine to execute immediately and wait at final_suspend?
         */
        COPAT_ASSERT(jobSys && jobSys->enqToThreadType(EJobThreadType::WorkerThreads) == EJobThreadType::WorkerThreads);

        const u32 grpCount = jobSys->getWorkersCount();

        AwaitableType dispatchedJobs{ nullptr };
        // If dispatching count is less than or equal to max group count
        if (count <= grpCount)
        {
            dispatchedJobs = dispatchTask(*jobSys, jobPriority, { .callback = callback, .jobIdx = 0, .dispatchCount = (count - 1) });
        }
        else
        {
            u32 jobsPerGrp = count / grpCount;
            u32 grpsWithMoreJobCount = count % grpCount;
            if (grpsWithMoreJobCount > 0)
            {
                GroupDispatchInfoType info{
                    .base = {.callback = callback, .jobIdx = 0, .dispatchCount = (grpCount - 1)},
                    .jobsPerGrp = jobsPerGrp,
                    .extendedGrpsCount = (grpsWithMoreJobCount - 1),
                    .grpIdx = 0
                };
                dispatchedJobs = dispatchExtendedTaskGroup(*jobSys, jobPriority, info);
            }
            else
            {
                GroupDispatchInfoType info{
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
};

/**
 * This function assumes that worker threads exists and enqueue will work without any issue
 */
template <typename RetType>
auto diverge(
    JobSystem *jobSys, const DispatchFunctionTypeWithRet<RetType> &callback, u32 count, EJobPriority jobPriority = EJobPriority::Priority_Normal
) noexcept
{
    static_assert(
        std::is_same_v<DispatchFunctionTypeWithRet<RetType>, typename DispatchWithReturn<RetType>::FuncType>,
        "Type mismatch between dispatch and diverge functions"
    );
    return DispatchWithReturn<RetType>::dispatch(jobSys, std::forward<decltype(callback)>(callback), count, jobPriority);
}

template <typename RetType>
std::vector<RetType> converge(DispatchAwaitableTypeWithRet<std::vector<RetType>> &&awaitable) noexcept
{
    static_assert(
        std::is_same_v<DispatchAwaitableTypeWithRet<std::vector<RetType>>, typename DispatchWithReturn<RetType>::AwaitableType>,
        "Type mismatch between dispatch and diverge functions"
    );
    return waitOnAwaitable(awaitable);
}

// diverge, converge immediately and returns the result
template <typename RetType>
std::vector<RetType> parallelForReturn(
    JobSystem *jobSys, const DispatchFunctionTypeWithRet<RetType> &callback, u32 count, EJobPriority jobPriority = EJobPriority::Priority_Normal
) noexcept
{
    using AwaitableType = typename DispatchWithReturn<RetType>::AwaitableType;
    std::vector<RetType> retVals;
    if (count == 0)
    {
        return retVals;
    }

    COPAT_ASSERT(jobSys);

    const u32 grpCount = jobSys->getWorkersCount();

    // If dispatching count is less than max workers count then jobsPerGrp will be 1
    // Else it will be jobsPerGrp or jobsPerGrp + 1 depending on count equiv-distributable among workers
    u32 jobsPerGrp = count / grpCount;
    jobsPerGrp += (count % grpCount) > 0;

    const u32 dispatchCount = count - jobsPerGrp;
    AwaitableType awaitable = diverge(jobSys, callback, dispatchCount, jobPriority);

    std::vector<RetType> lastGrpRets;
    lastGrpRets.reserve(jobsPerGrp);
    for (u32 jobIdx = count - jobsPerGrp; jobIdx < count; ++jobIdx)
    {
        lastGrpRets.emplace_back(callback(jobIdx));
    }

    /* Only converge if diverge is valid */
    if (dispatchCount)
    {
        retVals = converge(std::move(awaitable));
    }
    for (RetType &retVal : lastGrpRets)
    {
        retVals.emplace_back(std::move(retVal));
    }
    return retVals;
}

} // namespace copat
