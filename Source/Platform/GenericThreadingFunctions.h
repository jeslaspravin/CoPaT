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
    static bool createTlsSlot(u32 &outSlot) { return false; }
    static void releaseTlsSlot(u32 slot) {}
    static bool setTlsSlotValue(u32 slot, void *value) { return false; }

    static void *getTlsSlotValue(u32 slot) { return nullptr; }

    static void setThreadName(const char *name, void *threadHandle) {}
    static void setCurrentThreadName(const char *name) {}

    static std::string getCurrentThreadName() { return {}; }

    static void getCoreCount(u32 &outCoreCount, u32 &outLogicalProcessorCount) { outCoreCount = outLogicalProcessorCount = 0; };
    static bool setThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx, void *threadHandle) { return false; }
    static bool setCurrentThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx) { return false; }
};

} // namespace copat