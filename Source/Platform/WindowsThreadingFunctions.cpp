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

COPAT_NS_INLINED
namespace copat
{
bool WindowsThreadingFunctions::createTlsSlot(u32 &outSlot)
{
    u32 slotIdx = ::TlsAlloc();
    outSlot = slotIdx;
    return (slotIdx != TLS_OUT_OF_INDEXES);
}

void WindowsThreadingFunctions::releaseTlsSlot(u32 slot) { ::TlsFree(slot); }

bool WindowsThreadingFunctions::setTlsSlotValue(u32 slot, void *value) { return !!::TlsSetValue(slot, value); }

void *WindowsThreadingFunctions::getTlsSlotValue(u32 slot) { return ::TlsGetValue(slot); }

void WindowsThreadingFunctions::setThreadName(const char *name, void *threadHandle)
{
    std::wstring outStr;
    u32 bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    outStr.resize(bufLen);
    bufLen = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, outStr.data(), bufLen);
    ::SetThreadDescription(threadHandle, outStr.c_str());
}

void WindowsThreadingFunctions::setCurrentThreadName(const char *name) { setThreadName(name, ::GetCurrentThread()); }

std::string WindowsThreadingFunctions::getCurrentThreadName()
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

template <typename T>
void logicalProcessorInfoVisitor(T &&func, std::vector<uint8_t> &buffer, LOGICAL_PROCESSOR_RELATIONSHIP processorRelation)
{
    DWORD processorsInfoLen = 0;
    if (!::GetLogicalProcessorInformationEx(processorRelation, nullptr, &processorsInfoLen) && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        buffer.resize(processorsInfoLen);
        ::GetLogicalProcessorInformationEx(processorRelation, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(), &processorsInfoLen);

        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *procInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();
        for (u32 i = 0; i < processorsInfoLen; i += procInfo->Size)
        {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *procInfo = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + i);
            if (procInfo->Size == 0 || procInfo->Relationship != processorRelation)
            {
                continue;
            }

            func(procInfo);
        }
    }
}

void WindowsThreadingFunctions::getCoreCount(u32 &outCoreCount, u32 &outLogicalProcessorCount)
{
    outCoreCount = 0;
    outLogicalProcessorCount = 0;
    std::vector<uint8_t> buffer;
    logicalProcessorInfoVisitor(
        [&outCoreCount, &outLogicalProcessorCount](const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *procInfo)
        {
            outCoreCount++;
            for (int i = 0; i < procInfo->Processor.GroupCount; ++i)
            {
                outLogicalProcessorCount += uint32_t(::__popcnt64(procInfo->Processor.GroupMask[i].Mask));
            }
        },
        buffer, RelationProcessorCore
    );
}

bool WindowsThreadingFunctions::setThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx, void *threadHandle)
{
#if _WIN64
    u32 coreCount, logicalProcCount;
    getCoreCount(coreCount, logicalProcCount);

    const u32 hyperthread = logicalProcCount / coreCount;
    COPAT_ASSERT(hyperthread > logicalProcessorIdx);
    const u32 coreAffinityShift = coreIdx * hyperthread + logicalProcessorIdx;
    const u32 groupIndex = coreAffinityShift / 64;
    const u64 groupAffinityMask = 1ull << (coreAffinityShift % 64);

    ::GROUP_AFFINITY grpAffinity = {};
    grpAffinity.Group = WORD(groupIndex);
    grpAffinity.Mask = groupAffinityMask;

    ::GROUP_AFFINITY outgrpAffinity = {};

    return !!::SetThreadGroupAffinity((HANDLE)threadHandle, &grpAffinity, &outgrpAffinity);
#else
    // 32bit systems has some problem with GetLogicalProcessorInformationEx
    return false;
#endif
}

bool WindowsThreadingFunctions::setCurrentThreadProcessor(u32 coreIdx, u32 logicalProcessorIdx)
{
    return setThreadProcessor(coreIdx, logicalProcessorIdx, ::GetCurrentThread());
}

} // namespace copat