// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once


#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>
#endif

#include "tnm.h"
// TODO: reference additional headers your program requires here

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
