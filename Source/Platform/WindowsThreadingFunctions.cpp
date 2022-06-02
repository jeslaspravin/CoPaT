/*!
 * \file WindowsThreadingFunctions.cpp
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, Since 2022
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#include "WindowsThreadingFunctions.h"

#include <windows.h>

bool copat::WindowsThreadingFunctions::createTlsSlot(u32 &outSlot)
{
    u32 slotIdx = ::TlsAlloc();
    outSlot = slotIdx;
    return (slotIdx != TLS_OUT_OF_INDEXES);
}

void copat::WindowsThreadingFunctions::releaseTlsSlot(u32 slot) { ::TlsFree(slot); }

bool copat::WindowsThreadingFunctions::setTlsSlotValue(u32 slot, void *value) { return !!::TlsSetValue(slot, value); }

void *copat::WindowsThreadingFunctions::getTlsSlotValue(u32 slot) { return ::TlsGetValue(slot); }

void copat::WindowsThreadingFunctions::setThreadName(const char *name, void *threadHandle)
{
    std::wstring outStr;
    u32 bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    outStr.resize(bufLen);
    bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, outStr.data(), bufLen);
    ::SetThreadDescription(threadHandle, outStr.c_str());
}

void copat::WindowsThreadingFunctions::setCurrentThreadName(const char *name) { setThreadName(name, ::GetCurrentThread()); }

std::string copat::WindowsThreadingFunctions::getCurrentThreadName()
{
    std::string outStr;
    HANDLE threadHnd = ::GetCurrentThread();
    wchar_t *threadName;
    if (SUCCEEDED(::GetThreadDescription(threadHnd, &threadName)))
    {
        u32 bufLen = ::WideCharToMultiByte(CP_UTF8, 0, threadName, -1, NULL, 0, NULL, NULL);
        outStr.resize(bufLen);
        bufLen = ::WideCharToMultiByte(CP_UTF8, 0, threadName, -1, outStr.data(), bufLen, NULL, NULL);
        ::LocalFree(threadName);
    }
    return outStr;
}
