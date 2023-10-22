/*!
 * \file WindowsThreadingFunctions.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2023
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#pragma once

#include "GenericThreadingFunctions.h"

COPAT_NS_INLINED
namespace copat
{

class COPAT_EXPORT_SYM WindowsThreadingFunctions : public GenericThreadingFunctions
{
public:
    static bool createTlsSlot(u32 &outSlot) noexcept;

    static void releaseTlsSlot(u32 slot) noexcept;

    static bool setTlsSlotValue(u32 slot, void *value) noexcept;

    static void *getTlsSlotValue(u32 slot) noexcept;

    static void setThreadName(const char *name, void *threadHandle) noexcept;
    static void setCurrentThreadName(const char *name) noexcept;

    static std::string getCurrentThreadName() noexcept;

    static void getCoreCount(u32 &outCoreCount, u32 &outLogicalProcessorCount) noexcept;
    static bool setThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx, void *threadHandle) noexcept;
    static bool setCurrentThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx) noexcept;
    static bool setThreadGroupAffinity(u32 grpIdx, u64 affinityMask, void *threadHandle) noexcept;
};

} // namespace copat
