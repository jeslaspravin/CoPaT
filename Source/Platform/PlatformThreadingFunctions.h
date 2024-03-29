/*!
 * \file PlatformThreadingFunctions.h
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

#ifndef OVERRIDE_PLATFORMTHREADINGFUNCTIONS

#ifdef _WIN32
#include "WindowsThreadingFunctions.h"
COPAT_NS_INLINED
namespace copat
{
using PlatformThreadingFuncs = WindowsThreadingFunctions;
} // namespace copat
#elif __unix__
#error Not supported platform
#elif __linux__
#error Not supported platform
#elif __APPLE__
#error Not supported platform
#endif

COPAT_NS_INLINED
namespace copat
{
inline GenericThreadingFunctions::GroupAffinityMaskBuilder::GroupAffinityMaskBuilder()
    : mask(0)
    , groupIdx(0)
{
    u32 nCore, nLogicalProcs;
    PlatformThreadingFuncs::getCoreCount(nCore, nLogicalProcs);
    coreNum = nCore;
    logicProcsPerCore = nLogicalProcs / nCore;
}
} // namespace copat

#else
COPAT_NS_INLINED
namespace copat
{
using PlatformThreadingFuncs = OVERRIDE_PLATFORMTHREADINGFUNCTIONS;
} // namespace copat
#endif