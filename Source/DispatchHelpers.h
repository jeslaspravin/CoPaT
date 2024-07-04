/*!
 * \file DispatchHelpers.h
 *
 * \author Jeslas
 * \date June 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2024
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "JobSystem.h"
#include "CoroutineUtilities.h"

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

using DispatchAwaitableType = DispatchAwaitableTypeWithRet<void>;
using DispatchFunctionType = DispatchFunctionTypeWithRet<void>;

template <typename RetType>
using ConvergeAwaitable
    = JobSystemTaskType<std::vector<RetType>, JobSystemPromiseBase, false, EJobThreadType::WorkerThreads, EJobPriority::Priority_Normal>;

COPAT_EXPORT_SYM AwaitAllTasks<std::vector<DispatchAwaitableType>>
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
    /* Fundamental types used as reference and moving into result array is inefficient */
    using RetTypeRef = std::conditional_t<std::is_fundamental_v<RetType>, RetType, RetType &>;

    // Just copying the callback so a copy exists inside dispatch
    static AwaitableType dispatchOneTask(JobSystem &jobSys, EJobPriority jobPriority, FuncType callback, u32 jobIdx) noexcept
    {
        std::vector<RetType> retVal;
        /* Not using initializer list constructor as it copies while emplace move constructs */
        retVal.emplace_back(callback(jobIdx));
        co_return retVal;
    }
    static AwaitableType dispatchTaskGroup(JobSystem &jobSys, EJobPriority jobPriority, FuncType callback, u32 fromJobIdx, u32 count) noexcept
    {
        std::vector<RetType> retVal;
        retVal.reserve(count);

        const u32 endJobIdx = fromJobIdx + count;
        for (u32 jobIdx = fromJobIdx; jobIdx < endJobIdx; ++jobIdx)
        {
            retVal.emplace_back(callback(jobIdx));
        }
        co_return retVal;
    }

    static AwaitAllTasks<std::vector<AwaitableType>>
    dispatch(JobSystem *jobSys, const FuncType &callback, u32 count, EJobPriority jobPriority) noexcept
    {
        if (count == 0)
        {
            return {};
        }

        COPAT_ASSERT(jobSys);

        std::vector<AwaitableType> dispatchedJobs;
        /**
         * If there is no worker threads then we cannot actually diverge.
         * Queuing the jobs to the corresponding enqueue to thread.
         * Caller must ensure not to trigger dead lock when calling converge.
         */
        if (jobSys->enqToThreadType(EJobThreadType::WorkerThreads) != EJobThreadType::WorkerThreads)
        {
            dispatchedJobs.emplace_back(std::move(dispatchTaskGroup(*jobSys, jobPriority, callback, 0, count)));
            return awaitAllTasks(std::move(dispatchedJobs));
        }

        const u32 grpCount = jobSys->getWorkersCount();
        u32 jobsPerGrp = count / grpCount;
        // If dispatching count is less than max workers count
        if (jobsPerGrp == 0)
        {
            dispatchedJobs.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                dispatchedJobs.emplace_back(std::move(dispatchOneTask(*jobSys, jobPriority, callback, i)));
            }
        }
        else
        {
            dispatchedJobs.reserve(grpCount);
            u32 grpsWithMoreJobCount = count % grpCount;
            u32 jobIdx = 0;
            for (u32 i = 0; i < grpsWithMoreJobCount; ++i)
            {
                // Add one more job for all grps with more jobs
                dispatchedJobs.emplace_back(std::move(dispatchTaskGroup(*jobSys, jobPriority, callback, jobIdx, jobsPerGrp + 1)));
                jobIdx += jobsPerGrp + 1;
            }

            for (u32 i = grpsWithMoreJobCount; i < grpCount; ++i)
            {
                dispatchedJobs.emplace_back(std::move(dispatchTaskGroup(*jobSys, jobPriority, callback, jobIdx, jobsPerGrp)));
                jobIdx += jobsPerGrp;
            }
        }
        return awaitAllTasks(std::move(dispatchedJobs));
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
    using DispatcherType = DispatchWithReturn<RetType>;
    static_assert(
        std::is_same_v<DispatchFunctionTypeWithRet<RetType>, typename DispatcherType::FuncType>,
        "Type mismatch between dispatch and diverge functions"
    );
    return DispatcherType::dispatch(jobSys, std::forward<decltype(callback)>(callback), count, jobPriority);
}

template <typename RetType>
std::vector<RetType> converge(AwaitAllTasks<std::vector<DispatchAwaitableTypeWithRet<std::vector<RetType>>>> &&allAwaits) noexcept
{
    using DispatcherType = DispatchWithReturn<RetType>;
    using RetTypeRef = typename DispatcherType::RetTypeRef;
    static_assert(
        std::is_same_v<DispatchAwaitableTypeWithRet<std::vector<RetType>>, typename DispatcherType::AwaitableType>,
        "Type mismatch between dispatch and diverge functions"
    );
    std::vector<RetType> retVals;
    for (const auto &awaitable : waitOnAwaitable(allAwaits))
    {
        retVals.reserve(retVals.size() + awaitable.getReturnValue().size());
        for (RetTypeRef retVal : awaitable.getReturnValue())
        {
            retVals.emplace_back(std::move(retVal));
        }
    }
    return retVals;
}
template <typename RetType>
ConvergeAwaitable<RetType> awaitConverge(AwaitAllTasks<std::vector<DispatchAwaitableTypeWithRet<std::vector<RetType>>>> &&allAwaits) noexcept
{
    using DispatcherType = DispatchWithReturn<RetType>;
    using RetTypeRef = typename DispatcherType::RetTypeRef;
    static_assert(
        std::is_same_v<DispatchAwaitableTypeWithRet<std::vector<RetType>>, typename DispatcherType::AwaitableType>,
        "Type mismatch between dispatch and diverge functions"
    );
    static_assert(
        !IsAwaitableType_v<RetType>, "r-value converging awaitable might cause diverged lambda to destroy before final_suspend, Consider using "
                                     "l-value converge or lock wait converge!"
    );
    std::vector<RetType> retVals;
    /* Must be captured inside this coroutine for allAwaits to live */
    auto allAwaitsLocal = std::forward<std::remove_reference_t<decltype(allAwaits)>>(allAwaits);
    for (const auto &awaitable : co_await allAwaitsLocal)
    {
        retVals.reserve(retVals.size() + awaitable.getReturnValue().size());
        for (RetTypeRef retVal : awaitable.getReturnValue())
        {
            retVals.emplace_back(std::move(retVal));
        }
    }
    co_return retVals;
}
/* Diverge with sub diverge tend to face lambda lifetime issue, In such a case use this awaitConverge */
template <typename RetType>
ConvergeAwaitable<RetType> awaitConverge(AwaitAllTasks<std::vector<DispatchAwaitableTypeWithRet<std::vector<RetType>>>> &allAwaits) noexcept
{
    using DispatcherType = DispatchWithReturn<RetType>;
    using RetTypeRef = typename DispatcherType::RetTypeRef;
    static_assert(
        std::is_same_v<DispatchAwaitableTypeWithRet<std::vector<RetType>>, typename DispatcherType::AwaitableType>,
        "Type mismatch between dispatch and diverge functions"
    );
    std::vector<RetType> retVals;
    for (const auto &awaitable : co_await allAwaits)
    {
        retVals.reserve(retVals.size() + awaitable.getReturnValue().size());
        for (RetTypeRef retVal : awaitable.getReturnValue())
        {
            retVals.emplace_back(std::move(retVal));
        }
    }
    co_return retVals;
}

// diverge, converge immediately and returns the result
template <typename RetType>
std::vector<RetType> parallelForReturn(
    JobSystem *jobSys, const DispatchFunctionTypeWithRet<RetType> &callback, u32 count, EJobPriority jobPriority = EJobPriority::Priority_Normal
) noexcept
{
    using DispatcherType = DispatchWithReturn<RetType>;
    using AwaitableType = typename DispatcherType::AwaitableType;
    using RetTypeRef = typename DispatcherType::RetTypeRef;
    std::vector<RetType> retVals;
    if (count == 0)
    {
        return retVals;
    }

    /**
     * Cannot diverge and converge here if there is not workers.
     * Caller must call diverge and converge separately making sure not to dead lock.
     */
    COPAT_ASSERT(jobSys && jobSys->enqToThreadType(EJobThreadType::WorkerThreads) == EJobThreadType::WorkerThreads);

    const u32 grpCount = jobSys->getWorkersCount();

    // If dispatching count is less than max workers count then jobsPerGrp will be 1
    // Else it will be jobsPerGrp or jobsPerGrp + 1 depending on count equiv-distributable among workers
    u32 jobsPerGrp = count / grpCount;
    jobsPerGrp += (count % grpCount) > 0;

    AwaitAllTasks<std::vector<AwaitableType>> allAwaits = diverge(jobSys, callback, count - jobsPerGrp, jobPriority);

    std::vector<RetType> lastGrpRets;
    lastGrpRets.reserve(jobsPerGrp);
    for (u32 jobIdx = count - jobsPerGrp; jobIdx < count; ++jobIdx)
    {
        lastGrpRets.emplace_back(callback(jobIdx));
    }

    retVals = converge(std::move(allAwaits));
    for (RetTypeRef retVal : lastGrpRets)
    {
        retVals.emplace_back(std::move(retVal));
    }
    return retVals;
}

} // namespace copat
