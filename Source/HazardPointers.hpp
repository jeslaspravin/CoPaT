/*!
 * \file HazardPointers.hpp
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, Since 2022
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "CoPaTConfig.h"
#include "CoPaTTypes.h"
#include "Platform/PlatformThreadingFunctions.h"
#include "SyncPrimitives.h"

#include <mutex>
#include <chrono>
#include <vector>

COPAT_NS_INLINED
namespace copat
{

template <typename HazardType>
struct HazardPointerDeleter
{
    void operator()(HazardType *ptr) { memDelete(ptr); }
};

/**
 *  HazardRecord is supposed to be held by a single thread and must not be shared to another thread
 */
struct alignas(2 * CACHE_LINE_SIZE) HazardRecord
{
public:
    constexpr static const uintptr_t RESET_VALUE = 0;
    constexpr static const uintptr_t FREE_VALUE = ~(uintptr_t)(0);

    std::atomic<uintptr_t> hazardPtr{ FREE_VALUE };

public:
    template <typename T>
    T *setHazardPtr(T *ptr)
    {
        hazardPtr.store((uintptr_t)ptr, std::memory_order::release);
        // We need fence here to ensure that instructions never gets reordered at compiler
        std::atomic_signal_fence(std::memory_order::seq_cst);
        // We can read relaxed as the HazardRecord will be acquired by a thread and not shared so setting will technically happen in a thread
        // only
        return (T *)hazardPtr.load(std::memory_order::relaxed);
    }

    void reset() { hazardPtr.store(RESET_VALUE, std::memory_order::release); }
    void free() { hazardPtr.store(FREE_VALUE, std::memory_order::release); }

    constexpr static bool isUseable(uintptr_t ptr) { return ptr == RESET_VALUE; }
    constexpr static bool isFree(uintptr_t ptr) { return ptr == FREE_VALUE; }
    constexpr static bool isValid(uintptr_t ptr) { return ptr != FREE_VALUE && ptr != RESET_VALUE; }
};

struct alignas(2 * CACHE_LINE_SIZE) HazardPointersChunk
{
public:
    constexpr static const size_t RECORDS_PER_CHUNK = 32;

    // pNext will be in different cache line from allocated chunk even if they are next to each other
    std::atomic<HazardPointersChunk *> pNext{ nullptr };
    // records will be in different cache line from pNext and each record will be in different line as well
    HazardRecord records[RECORDS_PER_CHUNK];
};

template <typename Type, size_t MinPerThreadDeleteQSize = 4>
class HazardPointersManager
{
public:
    using HazardType = Type;
    using HazardPointerType = HazardType *;

    struct HazardPointer
    {
        HazardRecord *record{ nullptr };

        HazardPointer(HazardPointersManager &manager)
            : record(manager.acquireRecord())
        {}
        HazardPointer(HazardPointer &&other)
            : record(other.record)
        {
            other.record = nullptr;
        }
        HazardPointer &operator=(HazardPointer &&other)
        {
            record = other.record;
            other.record = nullptr;
            return *this;
        }

        ~HazardPointer()
        {
            if (record)
            {
                record->free();
            }
        }

        // No copying allowed
        HazardPointer() = delete;
        HazardPointer(const HazardPointer &) = delete;
        HazardPointer &operator=(const HazardPointer &) = delete;
    };

private:
    // 2 seconds once at very minimum
    constexpr static const std::chrono::steady_clock::duration::rep COLLECT_INTERVAL
        = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(2)).count();
    constexpr static const size_t MIN_PER_THREAD_DELETE_QSIZE = MinPerThreadDeleteQSize;

    struct HazardPtrPerThreadData
    {
        std::vector<HazardPointerType> deletingPtrs;
        std::chrono::time_point<std::chrono::steady_clock> lastCollect;
    };

    // Necessary for proper clean up
    std::vector<HazardPtrPerThreadData *> allPerThreadData;
    SpinLock perThreadDataLock;
    u32 perThreadSlot;

    HazardPointersChunk head;

public:
    HazardPointersManager()
    {
        bool tlsCreated = PlatformThreadingFuncs::createTlsSlot(perThreadSlot);
        COPAT_ASSERT(tlsCreated);
    }
    ~HazardPointersManager()
    {
        for (HazardPtrPerThreadData *threadData : allPerThreadData)
        {
            for (HazardPointerType hazPtr : threadData->deletingPtrs)
            {
                HazardPointerDeleter<HazardType>{}(hazPtr);
            }
            memDelete(threadData);
        }
        allPerThreadData.clear();
        PlatformThreadingFuncs::releaseTlsSlot(perThreadSlot);

        HazardPointersChunk *chunk = &head;
        std::vector<HazardPointersChunk *> chunks;
        while (HazardPointersChunk *nextChunk = chunk->pNext.load(std::memory_order::relaxed))
        {
            // Destruction must happen after external synchronization so relaxed if fine
            chunks.emplace_back(nextChunk);
            chunk = nextChunk;
        }

        head.pNext = nullptr;
        for (HazardPointersChunk *chunk : chunks)
        {
            memDelete(chunk);
        }
    }

    void enqueueDelete(HazardPointerType hazardPtr)
    {
        HazardPtrPerThreadData &threadData = getPerThreadData();
        threadData.deletingPtrs.emplace_back(hazardPtr);

        // If we waited long enough and deleting queue is more than minimum to collect? we start collect
        if ((std::chrono::steady_clock::now().time_since_epoch() - threadData.lastCollect.time_since_epoch()).count() >= COLLECT_INTERVAL
            && threadData.deletingPtrs.size() >= MIN_PER_THREAD_DELETE_QSIZE)
        {
            gcCollect();
        }
    }

    HazardPointerType dequeueDelete()
    {
        HazardPtrPerThreadData &threadData = getPerThreadData();
        if (threadData.deletingPtrs.empty())
        {
            return nullptr;
        }

        HazardPointerType hazPtr = threadData.deletingPtrs.back();
        threadData.deletingPtrs.pop_back();
        return hazPtr;
    }

private:
    HazardPtrPerThreadData *createPerThreadData()
    {
        HazardPtrPerThreadData *perThreadData = memNew<HazardPtrPerThreadData>();
        perThreadData->lastCollect = std::chrono::steady_clock::now();

        std::scoped_lock<SpinLock> allThreadDataLock{ perThreadDataLock };
        allPerThreadData.emplace_back(perThreadData);
        return perThreadData;
    }
    HazardPtrPerThreadData &getPerThreadData()
    {
        HazardPtrPerThreadData *perThreadDataPtr = (HazardPtrPerThreadData *)PlatformThreadingFuncs::getTlsSlotValue(perThreadSlot);
        if (!perThreadDataPtr)
        {
            perThreadDataPtr = createPerThreadData();
            PlatformThreadingFuncs::setTlsSlotValue(perThreadSlot, perThreadDataPtr);
        }
        return *perThreadDataPtr;
    }

    HazardPointersChunk *addChunk(HazardPointersChunk *addTo)
    {
        HazardPointersChunk *newChunk = memNew<HazardPointersChunk>();
        HazardPointersChunk *expectedChunk = nullptr;
        if (addTo->pNext.compare_exchange_weak(expectedChunk, newChunk, std::memory_order::acq_rel, std::memory_order::relaxed))
        {
            return newChunk;
        }
        memDelete(newChunk);
        // nullptr cannot be returned so return addTo as it will do the work again and calls this from acquireRecord again
        return (expectedChunk != nullptr) ? expectedChunk : addTo;
    }

    /**
     * This will be most contented path if we are not obtaining per thread record and acquiring record every time
     *
     * - CAS could fail even if valid we can ignore that and continue with next record
     * - pNext load might not see latest and see nullptr. It is okay and gets corrected in addChunk
     * - addChunk might fail CAS but it is okay as that will get corrected in next spin using same chunk here
     */
    HazardRecord *acquireRecord()
    {
        HazardPointersChunk *chunk = &head;
        while (true)
        {
            while (chunk)
            {
                for (u32 i = 0; i != HazardPointersChunk::RECORDS_PER_CHUNK; ++i)
                {
                    uintptr_t expectedPtr = HazardRecord::FREE_VALUE;
                    if (chunk->records[i].hazardPtr.compare_exchange_weak(
                            expectedPtr, HazardRecord::RESET_VALUE, std::memory_order::acq_rel, std::memory_order::relaxed
                        ))
                    {
                        return &chunk->records[i];
                    }
                }
                if (HazardPointersChunk *nextChunk = chunk->pNext.load(std::memory_order::relaxed))
                {
                    chunk = nextChunk;
                }
                else
                {
                    break;
                }
            }
            chunk = addChunk(chunk);
        }
    }

    /**
     * This will be read only on multi consumer data and RW on thread specific data
     */
    void gcCollect()
    {
        HazardPtrPerThreadData &threadData = getPerThreadData();

        std::vector<HazardPointerType> referencedPtrs;
        HazardPointersChunk *chunk = &head;
        while (chunk)
        {
            for (u32 i = 0; i != HazardPointersChunk::RECORDS_PER_CHUNK; ++i)
            {
                uintptr_t hazPtr = chunk->records[i].hazardPtr.load(std::memory_order::acquire);
                if (HazardRecord::isValid(hazPtr))
                {
                    referencedPtrs.emplace_back((HazardPointerType)hazPtr);
                }
            }
            chunk = chunk->pNext.load(std::memory_order::acquire);
        }

        std::sort(referencedPtrs.begin(), referencedPtrs.end());
        for (auto itr = threadData.deletingPtrs.begin(); itr != threadData.deletingPtrs.end();)
        {
            if (std::binary_search(referencedPtrs.cbegin(), referencedPtrs.cend(), *itr))
            {
                ++itr;
            }
            else
            {
                HazardPointerDeleter<HazardType>{}(*itr);
                itr = threadData.deletingPtrs.erase(itr);
            }
        }
        threadData.lastCollect = std::chrono::steady_clock::now();
    }
};

} // namespace copat