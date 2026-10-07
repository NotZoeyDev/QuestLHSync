// The few Win32 calls the driver's portable code uses, for Linux. On Windows the real ones come from <windows.h>.
#pragma once
#ifndef _WIN32
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

typedef int LONG;

static inline void Sleep(unsigned ms) {
  if (ms) usleep(ms * 1000);
  else sched_yield();
}
static inline void YieldProcessor() { __builtin_ia32_pause(); }
static inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline int localtime_s(struct tm *tm, const time_t *t) { return localtime_r(t, tm) ? 0 : -1; }
static inline uint64_t GetTickCount64() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#define MOVEFILE_REPLACE_EXISTING 1
static inline int MoveFileExA(const char *from, const char *to, int) { return rename(from, to) == 0; }  // replaces

#define QLHS_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define QLHS_EXPORT extern "C" __declspec(dllexport)
#endif
