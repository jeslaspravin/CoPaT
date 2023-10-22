/*!
 * \file GenericThreadingFunctions.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2023
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "../CoPaTConfig.h"

#include <string>

COPAT_NS_INLINED
namespace copat
{

class COPAT_EXPORT_SYM GenericThreadingFunctions
{
protected:
    GenericThreadingFunctions() = default;

public:
    struct GroupAffinityMaskBuilder
    {
    public:
        GroupAffinityMaskBuilder();

        GroupAffinityMaskBuilder &setGroupFrom(u32 coreIdx)
        {
            u32 logicProcGlobalIdx = coreIdx * logicProcsPerCore;
            groupIdx = logicProcGlobalIdx / LOGIC_PROCS_PER_GROUP;
            return *this;
        }
        GroupAffinityMaskBuilder &setAll()
        {
            mask = ~(0ull);
            return *this;
        }
        GroupAffinityMaskBuilder &clearUpto(u32 coreIdx, u32 logicalProcessorIdx)
        {
            u32 logicProcGlobalIdx = coreIdx * logicProcsPerCore + logicalProcessorIdx;
            if (isLogicProcGlobalIdxInsideGrp(logicProcGlobalIdx))
            {
                u32 bitIdx = logicProcGlobalIdx % LOGIC_PROCS_PER_GROUP;
                u64 bitsToClear = (1ull << bitIdx) - 1;
                mask &= ~bitsToClear;
            }
            return *this;
        }

        u32 getGroupIdx() const { return groupIdx; }
        u64 getAffinityMask() const { return mask; }

    private:
        u64 mask;
        u32 groupIdx;
        u32 coreNum;
        u32 logicProcsPerCore;

        constexpr static const u32 LOGIC_PROCS_PER_GROUP = 64;
        bool isLogicProcGlobalIdxInsideGrp(u32 logicProcGlobalIdx) const
        {
            return logicProcGlobalIdx >= (groupIdx * LOGIC_PROCS_PER_GROUP)
                   && logicProcGlobalIdx < (groupIdx * LOGIC_PROCS_PER_GROUP + LOGIC_PROCS_PER_GROUP);
        }
    };

public:
    static bool createTlsSlot(u32 &outSlot) noexcept { return false; }
    static void releaseTlsSlot(u32 slot) noexcept {}
    static bool setTlsSlotValue(u32 slot, void *value) noexcept { return false; }

    static void *getTlsSlotValue(u32 slot) noexcept { return nullptr; }

    static void setThreadName(const char *name, void *threadHandle) noexcept {}
    static void setCurrentThreadName(const char *name) noexcept {}

    static std::string getCurrentThreadName() noexcept { return {}; }

    static void getCoreCount(u32 &outCoreCount, u32 &outLogicalProcessorCount) noexcept { outCoreCount = outLogicalProcessorCount = 0; }
    static bool setThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx, void *threadHandle) noexcept { return false; }
    static bool setCurrentThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx) noexcept { return false; }
    static bool setThreadGroupAffinity(u32 grpIdx, u64 affinityMask, void *threadHandle) noexcept { return false; }
};

} // namespace copat