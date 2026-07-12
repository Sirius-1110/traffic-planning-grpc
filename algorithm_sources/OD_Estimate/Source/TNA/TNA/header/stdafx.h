// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// 解决同一 DLL 内部类被误标记为 dllimport 的问题：在包含 tnm.h 前定义 _TNM_DLL
#if !defined(_TNM_DLL) && !defined(_TNM_DLL_LOADED)
#define _TNM_DLL
#endif
#include "tnm.h"

#ifndef _WIN32
#include <cctype>
#include <iomanip>
inline char* strupr(char* s) {
    for (char* p = s; *p; ++p) *p = toupper((unsigned char)*p);
    return s;
}
#endif

#ifndef __func__
#define __func__ __FUNCTION__
#endif

#ifdef _WIN32
#ifndef atoll
#define atoll _atoi64
#endif
#endif
