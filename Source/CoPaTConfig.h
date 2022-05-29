#pragma once

/*!
 * \file CoPaTConfig.h
 *
 * \author Jeslas
 * \date May 2022
 * \copyright
 *  Copyright (C) Jeslas Pravin, Since 2022
 *  @jeslaspravin pravinjeslas@gmail.com
 *  License can be read in LICENSE file at this repository's root
 */

#include <stdint.h>
#include <functional>

/**
 * Thread types that are added by user
 */
//#define USER_DEFINED_THREADS() Thread1 = 1, Thread2, ... , ThreadN = WorkerThreads - 1
//#define USER_DEFINED_THREADS() RenderThread,

/**
 * Define Cache line size override here
 */
//#define OVERRIDE_CACHE_LINE_SIZE 64

/**
 * Override memory allocator type
 */
//#define OVERRIDE_MEMORY_ALLOCATOR AllocatorType

/**
 * Defines Export symbols macro in case using this inside shared library
 */
//#define OVERRIDE_EXPORT_SYM __declspec(dllexport)

/**
 * If you want to use your custom spin lock(Note that it must have proper class and function signature like in SpinLock in SyncPrimitives.h
 */
// #define OVERRIDE_SPINLOCK CustomSpinLock

/**
 * Define if we do not want to have everything inside namespace
 */
//#define WRAP_INSIDE_NS 0

/**
 * Override for assert macro.
 */
//#define OVERRIDE_ASSERT(expr) assert((expr))

/**
 * Override Function store type, expects signature FunctionType<ReturnType, Args...>
 */
// #define OVERRIDE_FUNCTION_TYPE std::function

/**
 * Override char specifier macro and to string macro.
 */
//#define OVERRIDE_TCHAR(expr) L##expr
//#define OVERRIDE_TOSTRING(expr) std::to_wstring(expr)

/**
 * Override PlatformThreadingFunctions.
 */
//#define OVERRIDE_PLATFORMTHREADINGFUNCTIONS YourPlatformFunctions

/**
 * Override uint32_t and uint64_t?
 */
//#define OVERRIDE_UINT32 uint32
//#define OVERRIDE_UINT64 uint64

//////////////////////////////////////////////////////////////////////////
/// Actual config
//////////////////////////////////////////////////////////////////////////

#ifndef USER_DEFINED_THREADS
#define USER_DEFINED_THREADS()
#endif

#if (defined OVERRIDE_CACHE_LINE_SIZE & OVERRIDE_CACHE_LINE_SIZE != 0)
#define CACHE_LINE_SIZE OVERRIDE_CACHE_LINE_SIZE
#else
#define CACHE_LINE_SIZE 64
#endif

#ifdef OVERRIDE_EXPORT_SYM
#define COPAT_EXPORT_SYM OVERRIDE_EXPORT_SYM
#else
// By default we use this library as static lib or embedded code
#define COPAT_EXPORT_SYM
#endif

#ifndef WRAP_INSIDE_NS
#define WRAP_INSIDE_NS 1
#endif

#if WRAP_INSIDE_NS
#define COPAT_NS_INLINED
#else
#define COPAT_NS_INLINED inline
#endif

#ifdef OVERRIDE_ASSERT
#define COPAT_ASSERT(expr) OVERRIDE_ASSERT(expr)
#else
#include <assert.h>
#define COPAT_ASSERT(expr) assert((expr))
#endif

#ifdef OVERRIDE_TCHAR
#define COPAT_TCHAR(x) OVERRIDE_TCHAR(x)
#else
#define COPAT_TCHAR(x) x
#endif

#ifdef OVERRIDE_TOSTRING
#define COPAT_TOSTRING(x) OVERRIDE_TOSTRING(x)
#else
#define COPAT_TOSTRING(x) std::to_string(x)
#endif

COPAT_NS_INLINED
namespace copat
{

#ifdef OVERRIDE_UINT32
using u32 = OVERRIDE_UINT32;
#else
using u32 = uint32_t;
#endif

#ifdef OVERRIDE_UINT64
using u64 = OVERRIDE_UINT64;
#else
using u64 = uint64_t;
#endif

#ifdef OVERRIDE_FUNCTION_TYPE
template <typename RetType, typename... Args>
using FunctionType = OVERRIDE_FUNCTION_TYPE<RetType, Args...>;
#else
template <typename RetType, typename... Args>
using FunctionType = std::function<RetType(Args...)>;
#endif

} // namespace copat