#pragma once

#include "GenericThreadingFunctions.h"

COPAT_NS_INLINED
namespace copat
{

class COPAT_EXPORT_SYM WindowsThreadingFunctions : public GenericThreadingFunctions
{
public:
    static bool createTlsSlot(u32 &outSlot);

    static void releaseTlsSlot(u32 slot);

    static bool setTlsSlotValue(u32 slot, void *value);

    static void *getTlsSlotValue(u32 slot);

    static void setThreadName(const char *name, void *threadHandle);
    static void setCurrentThreadName(const char *name);

    static std::string getCurrentThreadName();
};

} // namespace copat
