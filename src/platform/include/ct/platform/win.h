#pragma once
// ct/platform/win.h
// The only place first-party code includes <windows.h>. Lean, no min/max macros, and the worst
// name collisions undefined so ct:: types can use names like GetObject or DrawText.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef STRICT
#define STRICT
#endif

#include <windows.h>

#undef GetObject
#undef DrawText
#undef CreateWindow
#undef min
#undef max
