#pragma once

#include "GenericThreadingFunctions.h"

#include <windows.h>

COPAT_NS_INLINED
namespace copat
{

class WindowsThreadingFunctions : public GenericThreadingFunctions
{
public:
    static bool createTlsSlot(u32 &outSlot)
    {
        u32 slotIdx = ::TlsAlloc();
        outSlot = slotIdx;
        return (slotIdx != TLS_OUT_OF_INDEXES);
    }

    static void releaseTlsSlot(u32 slot) { ::TlsFree(slot); }

    static bool setTlsSlotValue(u32 slot, void *value) { return !!::TlsSetValue(slot, value); }

    static void *getTlsSlotValue(u32 slot) { return ::TlsGetValue(slot); }

    static void setThreadName(const char *name, void *threadHandle)
    {
        std::wstring outStr;
        u32 bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
        outStr.resize(bufLen);
        bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, outStr.data(), bufLen);
        ::SetThreadDescription(threadHandle, outStr.c_str());
    }
    static void setCurrentThreadName(const char *name) { setThreadName(name, ::GetCurrentThread()); }

    static std::string getCurrentThreadName()
    {
        HANDLE threadHnd = ::GetCurrentThread();
        wchar_t *threadName;
        ::GetThreadDescription(threadHnd, &threadName);

        std::string outStr;
        u32 bufLen = ::WideCharToMultiByte(CP_UTF8, 0, threadName, -1, NULL, 0, NULL, NULL);
        outStr.resize(bufLen);
        bufLen = ::WideCharToMultiByte(CP_UTF8, 0, threadName, -1, outStr.data(), bufLen, NULL, NULL);
        return outStr;
    }
};

} // namespace copat
