// Inline function hooks: MinHook on Windows, hook_linux.cpp's x86-64 equivalent (same calls) on Linux.
#pragma once
#ifdef _WIN32
#include "MinHook.h"
#else
enum MH_STATUS { MH_OK = 0, MH_ERROR_NOT_INITIALIZED = -1, MH_ERROR_UNSUPPORTED_FUNCTION = -2, MH_ERROR_MEMORY_ALLOC = -3,
                 MH_ERROR_MEMORY_PROTECT = -4, MH_ERROR_NOT_CREATED = -5 };
MH_STATUS MH_Initialize();
MH_STATUS MH_Uninitialize();
// pOriginal receives a function that runs the original; fails (MH_ERROR_UNSUPPORTED_FUNCTION) when the function's
// first bytes can't be moved (position-dependent instructions, or a body shorter than the 5 bytes the jump takes)
MH_STATUS MH_CreateHook(void *target, void *detour, void **original);
MH_STATUS MH_EnableHook(void *target);
MH_STATUS MH_DisableHook(void *target);
#endif
