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
};

} // namespace copat