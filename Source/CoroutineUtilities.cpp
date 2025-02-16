/*!
 * \file CoroutineUtilities.cpp
 *
 * \author Jeslas
 * \date April 2023
 * \copyright
 *  Copyright (C) Jeslas Pravin, 2022-2025
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#include "CoroutineUtilities.h"
#include "JobSystem.h"

COPAT_NS_INLINED
namespace copat
{
JobSystem *getDefaultJobSystem() { return JobSystem::get(); }

void SwitchJobSystemThreadAwaiter::enqueueToJs(std::coroutine_handle<> h, EJobPriority priority, std::source_location srcLoc) const noexcept
{
    COPAT_ASSERT(switchToJs);
    switchToJs->enqueueJob(h, switchToThread, priority, srcLoc);
}

} // namespace copat