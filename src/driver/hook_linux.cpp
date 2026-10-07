// x86-64 Linux inline hooks with MinHook's calls (hook.h). The target's first instructions are copied (whole
// instructions, lengths from HDE) into a trampoline that jumps back behind them, and replaced by a 5-byte relative jump
// to a relay near the target, which jumps on to the detour (which can be anywhere in the address space).
// Only instructions that don't depend on their own address can be moved: a function starting with a relative jump or
// a rip-relative access is refused (MH_ERROR_UNSUPPORTED_FUNCTION), and so is one shorter than the jump.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "hde/hde64.h"
#include "hook.h"

namespace {
struct Hook {
  uint8_t *target;
  uint8_t saved[16];
  size_t stolen;
  uint8_t jump[8];  // what Enable writes at the target: jmp rel32, then the bytes after it, restored as they were
  bool on;
};

std::mutex g_m;
std::map<void *, Hook> g_hooks;
bool g_init;

// a page within +-1.5 GB of addr, for the relay and the trampoline
uint8_t *NearPage(uint8_t *addr) {
  const intptr_t page = sysconf(_SC_PAGESIZE), span = 0x60000000;
  intptr_t base = (intptr_t)addr & ~(page - 1);
  for (intptr_t d = 0; d < span; d += 16 * page)
    for (int sign : {1, -1}) {
      intptr_t want = base + sign * d;
      if (want <= 0x10000) continue;
      void *p = mmap((void *)want, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (p == MAP_FAILED) continue;
      if (llabs((intptr_t)p - (intptr_t)addr) < span) return (uint8_t *)p;
      munmap(p, page);
    }
  return nullptr;
}

void AbsJump(uint8_t *at, const void *to) {  // jmp [rip+0]; .quad to
  at[0] = 0xFF; at[1] = 0x25;
  memset(at + 2, 0, 4);
  memcpy(at + 6, &to, 8);
}

bool Protect(void *addr, size_t n, int prot) {
  long page = sysconf(_SC_PAGESIZE);
  uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(page - 1), e = ((uintptr_t)addr + n + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void *)a, e - a, prot) == 0;
}
}  // namespace

MH_STATUS MH_Initialize() {
  std::lock_guard<std::mutex> g(g_m);
  g_init = true;
  return MH_OK;
}

MH_STATUS MH_Uninitialize() {
  std::lock_guard<std::mutex> g(g_m);
  for (auto &kv : g_hooks)
    if (kv.second.on) {
      Hook &h = kv.second;
      if (Protect(h.target, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        memcpy(h.target, h.saved, 5);
        Protect(h.target, 8, PROT_READ | PROT_EXEC);
      }
      h.on = false;
    }
  // relays and trampolines stay mapped: a thread may still be inside one
  g_init = false;
  return MH_OK;
}

MH_STATUS MH_CreateHook(void *target_, void *detour, void **original) {
  std::lock_guard<std::mutex> g(g_m);
  if (!g_init) return MH_ERROR_NOT_INITIALIZED;
  auto *target = (uint8_t *)target_;
  if (g_hooks.count(target_)) return MH_ERROR_UNSUPPORTED_FUNCTION;
  size_t len = 0;
  while (len < 5) {
    hde64s s;
    if (!hde64_disasm(target + len, &s) || (s.flags & (F_ERROR | F_RELATIVE))) return MH_ERROR_UNSUPPORTED_FUNCTION;
    if (s.flags & F_MODRM && s.modrm_mod == 0 && s.modrm_rm == 5) return MH_ERROR_UNSUPPORTED_FUNCTION;  // rip-relative
    if (s.opcode == 0xC3 || s.opcode == 0xC2 || s.opcode == 0xCC) return MH_ERROR_UNSUPPORTED_FUNCTION;  // returns in it
    len += s.len;
  }
  if (len > 15) return MH_ERROR_UNSUPPORTED_FUNCTION;
  uint8_t *page = NearPage(target);
  if (!page) return MH_ERROR_MEMORY_ALLOC;
  uint8_t *relay = page, *tramp = page + 32;
  AbsJump(relay, detour);
  memcpy(tramp, target, len);
  AbsJump(tramp + len, target + len);
  Hook h{};
  h.target = target;
  memcpy(h.saved, target, 16);
  h.stolen = len;
  h.jump[0] = 0xE9;
  int32_t rel = (int32_t)(relay - (target + 5));
  memcpy(h.jump + 1, &rel, 4);
  memcpy(h.jump + 5, target + 5, 3);
  if (original) *original = tramp;
  g_hooks[target_] = h;
  return MH_OK;
}

MH_STATUS MH_EnableHook(void *target) {
  std::lock_guard<std::mutex> g(g_m);
  auto it = g_hooks.find(target);
  if (it == g_hooks.end()) return MH_ERROR_NOT_CREATED;
  Hook &h = it->second;
  if (h.on) return MH_OK;
  if (!Protect(h.target, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) return MH_ERROR_MEMORY_PROTECT;
  // one aligned-enough 8-byte store when the bytes sit in one cache line: a thread entering the function sees either
  // the old bytes or the whole jump
  if (((uintptr_t)h.target & 63) <= 56) {
    uint64_t v;
    memcpy(&v, h.jump, 8);
    __atomic_store_n((uint64_t *)h.target, v, __ATOMIC_RELEASE);
  } else {
    memcpy(h.target, h.jump, 5);
  }
  Protect(h.target, 8, PROT_READ | PROT_EXEC);
  h.on = true;
  return MH_OK;
}

MH_STATUS MH_DisableHook(void *target) {
  std::lock_guard<std::mutex> g(g_m);
  auto it = g_hooks.find(target);
  if (it == g_hooks.end()) return MH_ERROR_NOT_CREATED;
  Hook &h = it->second;
  if (!h.on) return MH_OK;
  if (!Protect(h.target, 8, PROT_READ | PROT_WRITE | PROT_EXEC)) return MH_ERROR_MEMORY_PROTECT;
  uint64_t v;
  memcpy(&v, h.saved, 8);
  if (((uintptr_t)h.target & 63) <= 56) __atomic_store_n((uint64_t *)h.target, v, __ATOMIC_RELEASE);
  else memcpy(h.target, h.saved, 5);
  Protect(h.target, 8, PROT_READ | PROT_EXEC);
  h.on = false;
  return MH_OK;
}
