#pragma once

#include "../CoPaTConfig.h"

#include <string>

COPAT_NS_INLINED
namespace copat
{

class GenericThreadingFunctions
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
};

} // namespace copat