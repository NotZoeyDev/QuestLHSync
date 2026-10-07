// QuestLHSync-Installer (Linux, packaged as an AppImage): installs and updates the SteamVR driver. The driver, the
// dashboard app and their libraries travel inside the AppImage (APPDIR/usr/share/questlhsync-payload); installing copies
// them to ~/.local/share/QuestLHSync/questlhsync, registers that folder with SteamVR (vrpathreg) and records the
// installed version in installed.json, so a newer AppImage offers an update. Looks like the dashboard page (same
// colours, fonts and layout), drawn by canvas_linux.h's software canvas in an SDL2 window.
//   questlhsync-installer              the window
//   questlhsync-installer --install    install without a window (also --uninstall, --status)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <dlfcn.h>
#include <zlib.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include "../common/datadir_linux.h"
#include "../common/qlhs_status.h"

namespace fs = std::filesystem;

static const int W = 960, H = 660, SS = 2;  // page size, supersampling
#include "../overlay/canvas_linux.h"

namespace col {
const uint32_t bg = 0x14161b, card = 0x1d2027, card2 = 0x252932, line = 0x30343e, text = 0xeceef2, dim = 0x9aa1ad,
               faint = 0x6b7280, green = 0x3ccf6e, amber = 0xffb224, red = 0xff5f57, blue = 0x5b8cff, grey = 0x8e93a0;
}

struct Fail { std::string msg; };

static std::vector<int> VKey(const std::string &v) {
  std::vector<int> k;
  for (size_t i = 0; i < v.size();) {
    if (isdigit((unsigned char)v[i])) {
      int n = 0;
      while (i < v.size() && isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0');
      k.push_back(n);
    } else i++;
  }
  return k;
}

static bool Newer(const std::string &a, const std::string &b) {  // a newer than b
  auto x = VKey(a), y = VKey(b);
  size_t n = std::max(x.size(), y.size());
  x.resize(n); y.resize(n);
  return x > y;
}

static std::string Slurp(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------- paths and state
static fs::path g_root, g_driver, g_state, g_payload;
static std::string g_bundled;  // the version this AppImage carries

static const char *kLib = "bin/linux64/driver_questlhsync.so";

static fs::path ExeDir() {
  char b[4096];
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  return n > 0 ? fs::path(std::string(b, n)).parent_path() : fs::path(".");
}

static void InitPaths() {
  g_root = QlhsDataDir();
  g_driver = g_root / "questlhsync";
  g_state = g_root / "installed.json";
  if (const char *p = getenv("QLHS_PAYLOAD")) g_payload = p;
  else if (const char *a = getenv("APPDIR")) g_payload = fs::path(a) / "usr/share/questlhsync-payload";
  else g_payload = ExeDir() / "../share/questlhsync-payload";
  std::string v = Slurp(g_payload / "VERSION");
  while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
  g_bundled = v;
}

static std::string ReadInstalled() {
  std::error_code ec;
  if (!fs::exists(g_driver / kLib, ec)) return "";
  std::string t = Slurp(g_state);
  size_t k = t.find("\"version\"");
  if (k == std::string::npos) return "";
  size_t a = t.find('"', t.find(':', k) + 1), b = a == std::string::npos ? a : t.find('"', a + 1);
  return b == std::string::npos ? "" : t.substr(a + 1, b - a - 1);
}

// ---------------------------------------------------------------- SteamVR
static fs::path ConfigHome() {
  const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
  if (x && *x) return x;
  return fs::path(h ? h : ".") / ".config";
}

// the paths in openvrpaths.vrpath's `key` array ("runtime", "external_drivers", "config")
static std::vector<fs::path> VrPaths(const char *key) {
  std::string t = Slurp(ConfigHome() / "openvr" / "openvrpaths.vrpath");
  std::vector<fs::path> v;
  size_t k = t.find("\"" + std::string(key) + "\"");
  size_t a = k == std::string::npos ? k : t.find('[', k), e = a == std::string::npos ? a : t.find(']', a);
  if (e != std::string::npos) {
    for (size_t i = a; i < e; i++) {
      if (t[i] != '"') continue;
      std::string s;
      for (i++; i < e && t[i] != '"'; i++) {
        if (t[i] == '\\' && i + 1 < e) i++;
        s += t[i];
      }
      v.push_back(fs::path(s));
    }
  }
  return v;
}

static fs::path VrPathReg() {
  for (const fs::path &p : VrPaths("runtime")) {
    fs::path exe = p / "bin" / "vrpathreg.sh";
    std::error_code ec;
    if (fs::exists(exe, ec)) return exe;
  }
  throw Fail{"SteamVR not found: install and run it once, then try again"};
}

// other QuestLHSync driver folders registered with SteamVR (a source checkout, installed by hand)
static std::vector<fs::path> OtherCopies() {
  std::vector<fs::path> v;
  for (const fs::path &p : VrPaths("external_drivers")) {
    std::error_code ec;
    if (fs::exists(p / kLib, ec) && !fs::equivalent(p, g_driver, ec)) v.push_back(p);
  }
  return v;
}

static void Say(const std::wstring &m, uint32_t accent = 0);

// ---- steamvr.vrsettings: "steamvr"."activateMultipleDrivers" must be true or SteamVR won't load the driver alongside
// the headset's own. Edited as text so the rest of the file stays byte for byte as SteamVR wrote it.
static size_t SkipWs(const std::string &t, size_t i) {
  while (i < t.size() && isspace((unsigned char)t[i])) i++;
  return i;
}

static size_t SkipStr(const std::string &t, size_t i) {  // t[i] is '"'; returns the index after the closing quote
  for (i++; i < t.size(); i++) {
    if (t[i] == '\\') i++;
    else if (t[i] == '"') return i + 1;
  }
  return t.size();
}

static size_t SkipValue(const std::string &t, size_t i) {
  if (i >= t.size()) return i;
  if (t[i] == '"') return SkipStr(t, i);
  if (t[i] == '{' || t[i] == '[') {
    int depth = 0;
    for (; i < t.size(); i++) {
      if (t[i] == '"') { i = SkipStr(t, i) - 1; continue; }
      if (t[i] == '{' || t[i] == '[') depth++;
      else if (t[i] == '}' || t[i] == ']') {
        if (--depth == 0) return i + 1;
      }
    }
    return t.size();
  }
  while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && !isspace((unsigned char)t[i])) i++;
  return i;
}

// Looks for `key` among the members of the object opening at t[open]. On a hit sets [vs, ve) to its value. Returns
// false when absent (hasMembers tells whether the object has any) or the text is malformed (ok = false).
static bool ObjMember(const std::string &t, size_t open, const char *key, size_t &vs, size_t &ve, bool &hasMembers,
                      bool &ok) {
  ok = true;
  hasMembers = false;
  size_t i = open + 1;
  for (;;) {
    i = SkipWs(t, i);
    while (i < t.size() && t[i] == ',') i = SkipWs(t, i + 1);
    if (i >= t.size()) { ok = false; return false; }
    if (t[i] == '}') return false;
    if (t[i] != '"') { ok = false; return false; }
    hasMembers = true;
    size_t ks = i;
    i = SkipStr(t, i);
    std::string name = t.substr(ks + 1, i - ks - 2);
    i = SkipWs(t, i);
    if (i >= t.size() || t[i] != ':') { ok = false; return false; }
    i = SkipWs(t, i + 1);
    vs = i;
    ve = SkipValue(t, i);
    if (name == key) return true;
    i = ve;
  }
}

static std::string InsertMember(const std::string &t, size_t open, bool hasMembers, const std::string &member) {
  return t.substr(0, open + 1) + "\n\t" + member + (hasMembers ? "," : "\n") + t.substr(open + 1);
}

// returns true when the file was changed
static bool EnsureMultipleDrivers(const fs::path &file) {
  static const char *kKey = "activateMultipleDrivers";
  std::string t = Slurp(file);
  size_t root = SkipWs(t, 0);
  if (root >= t.size()) {  // missing or empty
    std::ofstream(file, std::ios::binary) << "{\n\t\"steamvr\" : {\n\t\t\"" << kKey << "\" : true\n\t}\n}\n";
    return true;
  }
  const Fail bad{"steamvr.vrsettings isn't valid JSON"};
  if (t[root] != '{') throw bad;
  size_t vs, ve;
  bool has, ok;
  std::string out;
  if (!ObjMember(t, root, "steamvr", vs, ve, has, ok)) {
    if (!ok) throw bad;
    out = InsertMember(t, root, has, std::string("\"steamvr\" : {\n\t\t\"") + kKey + "\" : true\n\t}");
  } else if (t[vs] != '{') {
    out = t.substr(0, vs) + "{\n\t\t\"" + kKey + "\" : true\n\t}" + t.substr(ve);
  } else {
    size_t ms, me;
    bool shas;
    if (!ObjMember(t, vs, kKey, ms, me, shas, ok)) {
      if (!ok) throw bad;
      out = InsertMember(t, vs, shas, std::string("\"") + kKey + "\" : true");
    } else if (t.compare(ms, me - ms, "true") == 0) {
      return false;
    } else {
      out = t.substr(0, ms) + "true" + t.substr(me);
    }
  }
  std::ofstream(file, std::ios::binary | std::ios::trunc) << out;
  return true;
}

static void EnsureSteamVrSettings() {
  try {
    for (const fs::path &dir : VrPaths("config")) {
      fs::path f = dir / "steamvr.vrsettings";
      if (EnsureMultipleDrivers(f)) Say(L"Set activateMultipleDrivers to true in " + Wide(f.c_str()));
      return;
    }
    Say(L"Couldn't find SteamVR's config folder: set \"activateMultipleDrivers\": true under \"steamvr\" in steamvr.vrsettings", col::amber);
  } catch (const Fail &e) {
    Say(Wide(e.msg.c_str()) + L": couldn't set activateMultipleDrivers", col::amber);
  }
}

// ---- processes
static std::vector<std::pair<pid_t, std::string>> Procs() {
  std::vector<std::pair<pid_t, std::string>> v;
  DIR *d = opendir("/proc");
  if (!d) return v;
  while (dirent *e = readdir(d)) {
    if (!isdigit((unsigned char)e->d_name[0])) continue;
    std::string c = Slurp(std::string("/proc/") + e->d_name + "/comm");
    while (!c.empty() && (c.back() == '\n' || c.back() == ' ')) c.pop_back();
    if (!c.empty()) v.push_back({(pid_t)atoi(e->d_name), c});
  }
  closedir(d);
  return v;
}

static bool SteamVrRunning() {
  for (auto &p : Procs())
    if (p.second == "vrserver") return true;
  return false;
}

// Stops SteamVR and our dashboard app (they hold the driver folder's files open) and waits for them to exit. SteamVR
// is asked to close first (the monitor, like closing its window): killed outright, Steam can go on thinking SteamVR runs
// and refuse to start it again. The dashboard app leaves with it.
static void StopSteamVr() {
  static const char *names[] = {"vrserver", "vrmonitor", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup.sh",
                                "QuestLHSync"};
  auto mine = [&] {
    std::vector<std::pair<pid_t, std::string>> v;
    for (auto &p : Procs())
      for (const char *n : names)
        if (p.second == n) { v.push_back(p); break; }
    return v;
  };
  auto procs = mine();
  if (procs.empty()) return;
  Say(L"Stopping SteamVR…");
  for (auto &p : procs)
    if (p.second == "vrmonitor" || p.second == "vrstartup.sh") kill(p.first, SIGTERM);
  for (int i = 0; i < 150 && !mine().empty(); i++) usleep(100000);
  for (auto &p : mine()) kill(p.first, SIGKILL);  // what didn't close in 15 s
  for (int i = 0; i < 50 && SteamVrRunning(); i++) usleep(100000);
  if (SteamVrRunning()) throw Fail{"couldn't stop SteamVR (vrserver is still running)"};
}

static void RunReg(const char *action, const fs::path &dir) {
  fs::path reg = VrPathReg();
  int pfd[2];
  if (pipe(pfd) != 0) throw Fail{"couldn't start vrpathreg"};
  pid_t pid = fork();
  if (pid < 0) throw Fail{"couldn't start vrpathreg"};
  if (pid == 0) {
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);
    close(pfd[0]);
    close(pfd[1]);
    unsetenv("LD_LIBRARY_PATH");  // the AppImage's own libraries are for the installer, not SteamVR's tools
    unsetenv("LD_PRELOAD");
    execl(reg.c_str(), reg.c_str(), action, dir.c_str(), (char *)nullptr);
    _exit(127);
  }
  close(pfd[1]);
  std::string output;
  char buf[512];
  ssize_t n;
  while ((n = read(pfd[0], buf, sizeof buf)) > 0) output.append(buf, n);
  close(pfd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  if (code != 0) {
    while (!output.empty() && isspace((unsigned char)output.back())) output.pop_back();
    throw Fail{std::string("vrpathreg ") + action + " failed (" + std::to_string(code) + ") " + output};
  }
}
// ---------------------------------------------------------------- GitHub releases
static const char *RELEASES = "https://github.com/CreoleVR/QuestLHSync/releases";

// libcurl is the system's (opened when needed, so the AppImage carries neither it nor a TLS library)
struct CurlApi {
  CURL *(*init)() = nullptr;
  CURLcode (*setopt)(CURL *, CURLoption, ...) = nullptr;
  CURLcode (*perform)(CURL *) = nullptr;
  CURLcode (*getinfo)(CURL *, CURLINFO, ...) = nullptr;
  void (*cleanup)(CURL *) = nullptr;
  const char *(*strerror)(CURLcode) = nullptr;
};

static const CurlApi *LoadCurl() {
  static CurlApi api;
  static bool tried = false, ok = false;
  if (!tried) {
    tried = true;
    void *h = nullptr;
    for (const char *n : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4"})
      if ((h = dlopen(n, RTLD_NOW))) break;
    if (h) {
      api.init = (decltype(api.init))dlsym(h, "curl_easy_init");
      api.setopt = (decltype(api.setopt))dlsym(h, "curl_easy_setopt");
      api.perform = (decltype(api.perform))dlsym(h, "curl_easy_perform");
      api.getinfo = (decltype(api.getinfo))dlsym(h, "curl_easy_getinfo");
      api.cleanup = (decltype(api.cleanup))dlsym(h, "curl_easy_cleanup");
      api.strerror = (decltype(api.strerror))dlsym(h, "curl_easy_strerror");
      ok = api.init && api.setopt && api.perform && api.getinfo && api.cleanup && api.strerror;
    }
  }
  return ok ? &api : nullptr;
}

struct HttpResult { long status = 0; std::string location; };

// GET url. follow=false returns the first response (used to read /releases/latest's redirect). sink gets the body, with
// the size the server announced when it did.
static HttpResult Http(const std::string &url, bool follow,
                       const std::function<void(const uint8_t *, size_t, uint64_t)> &sink = {}) {
  const CurlApi *c = LoadCurl();
  if (!c) throw Fail{"libcurl isn't installed (install curl): can't reach GitHub"};
  struct Ctx { const std::function<void(const uint8_t *, size_t, uint64_t)> *sink; uint64_t total; } ctx{&sink, 0};
  auto write = +[](char *p, size_t s, size_t n, void *u) -> size_t {
    auto *x = (Ctx *)u;
    if (*x->sink) (*x->sink)((const uint8_t *)p, s * n, x->total);
    return s * n;
  };
  auto progress = +[](void *u, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) -> int {
    ((Ctx *)u)->total = total > 0 ? (uint64_t)total : 0;
    return 0;
  };
  CURL *h = c->init();
  if (!h) throw Fail{"couldn't start libcurl"};
  c->setopt(h, CURLOPT_URL, url.c_str());
  c->setopt(h, CURLOPT_USERAGENT, "QuestLHSync-installer");
  c->setopt(h, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
  c->setopt(h, CURLOPT_MAXREDIRS, 10L);
  c->setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
  c->setopt(h, CURLOPT_TIMEOUT, follow ? 600L : 30L);
  c->setopt(h, CURLOPT_NOBODY, follow ? 0L : 1L);
  c->setopt(h, CURLOPT_NOPROGRESS, 0L);
  c->setopt(h, CURLOPT_XFERINFOFUNCTION, progress);
  c->setopt(h, CURLOPT_XFERINFODATA, &ctx);
  c->setopt(h, CURLOPT_WRITEFUNCTION, write);
  c->setopt(h, CURLOPT_WRITEDATA, &ctx);
  CURLcode rc = c->perform(h);
  HttpResult res;
  char *loc = nullptr;
  if (rc == CURLE_OK) {
    c->getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
    if (c->getinfo(h, CURLINFO_REDIRECT_URL, &loc) == CURLE_OK && loc) res.location = loc;
  }
  std::string why = rc == CURLE_OK ? "" : c->strerror(rc);
  c->cleanup(h);
  if (rc != CURLE_OK) throw Fail{"couldn't reach GitHub: " + why};
  return res;
}

static std::string LatestTag() {
  HttpResult r = Http(std::string(RELEASES) + "/latest", false);
  size_t k = r.location.rfind("/releases/tag/");
  if (r.status / 100 != 3 || k == std::string::npos)
    throw Fail{"couldn't read the latest release (HTTP " + std::to_string(r.status) + ")"};
  std::string tag = r.location.substr(k + 14);
  tag = tag.substr(0, tag.find_first_of("/?#"));
  if (tag.empty()) throw Fail{"couldn't read the latest release"};
  return tag;
}

static std::string PackageName(const std::string &tag) {
  return "QuestLHSync-linux-module-" + tag.substr(!tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? 1 : 0) + ".zip";
}

// the version an install would put in: GitHub's latest, unless this AppImage carries a newer (or GitHub isn't known)
static std::string Target(const std::string &latest) {
  if (latest.empty()) return g_bundled;
  if (g_bundled.empty() || Newer(latest, g_bundled)) return latest;
  return g_bundled;
}

// the release zip's questlhsync/ folder, unpacked to dest
static void ExtractDriver(const std::vector<uint8_t> &z, const fs::path &dest) {
  const Fail bad{"release zip is corrupt"};
  auto u16 = [&](size_t o) -> uint32_t {
    if (o + 2 > z.size()) throw bad;
    return z[o] | z[o + 1] << 8;
  };
  auto u32 = [&](size_t o) -> uint32_t { return u16(o) | u16(o + 2) << 16; };

  size_t eocd = std::string::npos;
  for (size_t back = 22; back <= z.size() && back <= 22 + 65535; back++)
    if (u32(z.size() - back) == 0x06054b50) { eocd = z.size() - back; break; }
  if (eocd == std::string::npos) throw bad;
  uint32_t count = u16(eocd + 10);
  size_t p = u32(eocd + 16);

  std::error_code ec;
  int files = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (u32(p) != 0x02014b50) throw bad;
    uint32_t method = u16(p + 10), crc = u32(p + 16), cs = u32(p + 20), us = u32(p + 24);
    uint32_t nl = u16(p + 28), el = u16(p + 30), cl = u16(p + 32);
    size_t lo = u32(p + 42);
    if (p + 46 + nl > z.size()) throw bad;
    std::string name((const char *)&z[p + 46], nl);
    p += 46 + nl + el + cl;
    if (name.rfind("questlhsync/", 0) != 0 || name.back() == '/') continue;
    std::string rel = name.substr(12);
    // refuse anything that could leave the folder
    if (rel.empty() || rel[0] == '/' || rel.find_first_of("\\:") != std::string::npos) throw Fail{"unsafe path in release zip"};
    for (size_t a = 0; a <= rel.size();) {
      size_t b = rel.find('/', a);
      if (b == std::string::npos) b = rel.size();
      std::string seg = rel.substr(a, b - a);
      if (seg.empty() || seg == "." || seg == "..") throw Fail{"unsafe path in release zip"};
      a = b + 1;
    }
    if (cs == 0xFFFFFFFFu || us == 0xFFFFFFFFu) throw Fail{"zip64 release zips aren't supported"};
    if (u32(lo) != 0x04034b50) throw bad;
    size_t data = lo + 30 + u16(lo + 26) + u16(lo + 28);
    if (data + cs > z.size()) throw bad;
    std::vector<uint8_t> out(us);
    if (method == 0) {
      if (cs != us) throw bad;
      if (us) memcpy(out.data(), &z[data], us);
    } else if (method == 8) {
      z_stream zs{};
      if (inflateInit2(&zs, -15) != Z_OK) throw bad;
      zs.next_in = (Bytef *)&z[data];
      zs.avail_in = cs;
      zs.next_out = out.data();
      zs.avail_out = us;
      int r = inflate(&zs, Z_FINISH);
      inflateEnd(&zs);
      if (r != Z_STREAM_END && !(r == Z_OK && us == 0)) throw bad;
    } else throw Fail{"release zip uses an unsupported compression"};
    if ((uint32_t)crc32(0, out.data(), us) != crc) throw bad;
    fs::path to = dest / rel;
    fs::create_directories(to.parent_path(), ec);
    std::ofstream f(to, std::ios::binary);
    f.write((const char *)out.data(), us);
    f.close();
    if (!f) throw Fail{"couldn't write " + to.string()};
    // what's under bin/ runs (the dashboard app) or is loaded (the libraries)
    fs::permissions(to, rel.rfind("bin/", 0) == 0 ? fs::perms(0755) : fs::perms(0644), ec);
    files++;
  }
  if (!files) throw Fail{"release zip has no questlhsync/ driver folder"};
}


// ---------------------------------------------------------------- shared state (UI thread <-> workers)
struct Shared {
  std::string latest;
  std::mutex m;
  std::string installed;
  std::wstring msg = L"Ready.";
  uint32_t accent = 0;  // banner bar colour, 0 = follow the state
  std::vector<std::pair<std::wstring, std::wstring>> logs;
  int pct = 0;
  bool busy = false, err = false;
} S;

static std::atomic<bool> g_dirty{true};
static bool g_cli = false;

static void Say(const std::wstring &m, uint32_t accent) {
  time_t t = time(nullptr);
  tm tm;
  localtime_r(&t, &tm);
  {
    std::lock_guard<std::mutex> g(S.m);
    S.msg = m;
    S.accent = accent;
    S.logs.push_back({F(L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec), m});
  }
  if (g_cli) {
    std::string u;
    for (wchar_t c : m) {  // UTF-8
      uint32_t x = (uint32_t)c;
      if (x < 0x80) u += (char)x;
      else if (x < 0x800) { u += (char)(0xC0 | x >> 6); u += (char)(0x80 | (x & 63)); }
      else { u += (char)(0xE0 | x >> 12); u += (char)(0x80 | ((x >> 6) & 63)); u += (char)(0x80 | (x & 63)); }
    }
    printf("%s\n", u.c_str());
    fflush(stdout);
  }
  g_dirty = true;
}

static void SetProgress(int pct) {
  {
    std::lock_guard<std::mutex> g(S.m);
    if (S.pct == pct) return;
    S.pct = pct;
  }
  g_dirty = true;
}

// runs fn on a thread (the window) or right here (no window); false when it failed
static bool RunBg(std::function<void()> fn) {
  auto body = [fn] {
    std::string err;
    try { fn(); }
    catch (const Fail &f) { err = f.msg; }
    catch (const std::exception &e) { err = e.what(); }
    std::string inst = ReadInstalled();
    {
      std::lock_guard<std::mutex> g(S.m);
      S.installed = inst;
      S.busy = false;
      S.err = !err.empty();
    }
    if (!err.empty()) Say(L"Error: " + Wide(err.c_str()), col::red);
    g_dirty = true;
    return err.empty();
  };
  {
    std::lock_guard<std::mutex> g(S.m);
    if (S.busy) return false;
    S.busy = true;
    S.err = false;
    S.pct = 0;
  }
  g_dirty = true;
  if (g_cli) return body();
  std::thread(body).detach();
  return true;
}

// the driver folder to install, unpacked beside the installed one (swapped in after SteamVR is stopped): from GitHub's
// latest release, or from this AppImage when that isn't reachable, has no Linux package yet or isn't newer. Returns the
// version.
static std::string Stage(const fs::path &nw) {
  std::error_code ec;
  bool have = fs::exists(g_payload / "questlhsync" / kLib, ec) && !g_bundled.empty();
  std::string latest;
  {
    std::lock_guard<std::mutex> g(S.m);
    latest = S.latest;
  }
  if (latest.empty()) {
    try {
      latest = LatestTag();
      std::lock_guard<std::mutex> g(S.m);
      S.latest = latest;
    } catch (const Fail &f) {
      if (!have) throw;
      Say(L"Couldn't reach GitHub (" + Wide(f.msg.c_str()) + L"): using this installer's " + Wide(g_bundled.c_str()), col::amber);
    }
  }
  fs::remove_all(nw, ec);
  if (!latest.empty() && (!have || Newer(latest, g_bundled))) {
    std::string pkg = PackageName(latest);
    try {
      Say(L"Downloading " + Wide(pkg.c_str()) + L"…");
      std::vector<uint8_t> zip;
      HttpResult r = Http(std::string(RELEASES) + "/download/" + latest + "/" + pkg, true,
                          [&](const uint8_t *d, size_t n, uint64_t total) {
                            zip.insert(zip.end(), d, d + n);
                            if (total) SetProgress((int)std::min<uint64_t>(99, zip.size() * 100 / total));
                          });
      if (r.status != 200 || zip.empty())
        throw Fail{"download failed: " + pkg + " not found (HTTP " + std::to_string(r.status) + ")"};
      ExtractDriver(zip, nw);
      return latest;
    } catch (const Fail &f) {
      fs::remove_all(nw, ec);
      if (!have) throw;
      Say(L"" + Wide(f.msg.c_str()) + L": using this installer's " + Wide(g_bundled.c_str()), col::amber);
      SetProgress(0);
    }
  }
  if (!have) throw Fail{"this installer carries no driver (" + g_payload.string() + ")"};
  std::vector<fs::path> files;
  for (auto &e : fs::recursive_directory_iterator(g_payload / "questlhsync")) files.push_back(e.path());
  size_t done = 0;
  for (const fs::path &p : files) {
    fs::path to = nw / fs::relative(p, g_payload / "questlhsync");
    if (fs::is_directory(p)) fs::create_directories(to);
    else {
      fs::create_directories(to.parent_path());
      fs::copy_file(p, to, fs::copy_options::overwrite_existing);  // keeps the permission bits (the app is executable)
    }
    SetProgress((int)std::min<size_t>(99, ++done * 100 / files.size()));
  }
  return g_bundled;
}

// swapped by renames: a file still in use leaves the old driver whole instead of half deleted
static void Swap(const fs::path &nw) {
  std::error_code ec;
  fs::path old = g_driver;
  old += ".old";
  fs::remove_all(old, ec);
  if (fs::exists(g_driver, ec)) {
    fs::rename(g_driver, old, ec);
    if (ec) {
      fs::remove_all(nw, ec);
      throw Fail{"couldn't replace the old driver files (is SteamVR running?)"};
    }
  }
  fs::rename(nw, g_driver, ec);
  if (ec) {
    fs::rename(old, g_driver, ec);
    throw Fail{"couldn't move the driver into place"};
  }
  fs::remove_all(old, ec);
}

static void Install() {
  RunBg([] {
    VrPathReg();  // fail early if SteamVR is missing
    std::error_code ec;
    fs::create_directories(g_root, ec);
    fs::path nw = g_driver;
    nw += ".new";
    std::string version = Stage(nw);
    StopSteamVr();
    Say(L"Installing " + Wide(version.c_str()) + L"…");
    Swap(nw);
    RunReg("adddriver", g_driver);
    EnsureSteamVrSettings();  // SteamVR is stopped, so it won't overwrite the file on exit
    for (const fs::path &old : OtherCopies()) {  // SteamVR would load one of the two: this one only from now on
      RunReg("removedriver", old);
      Say(L"Unregistered the other copy in " + Wide(old.c_str()) + L" (its files are left as they are)");
    }
    std::ofstream(g_state) << "{\"version\":\"" << version << "\"}";
    SetProgress(100);
    Say(L"Installed " + Wide(version.c_str()) + L". Start SteamVR.", col::green);
  });
}

static void Check() {
  RunBg([] {
    Say(L"Checking for updates…");
    std::string tag, cur;
    try {
      tag = LatestTag();
    } catch (const Fail &f) {
      if (g_bundled.empty()) throw;
      Say(L"Couldn't reach GitHub (" + Wide(f.msg.c_str()) + L"): this installer carries " + Wide(g_bundled.c_str()) + L".", col::amber);
      return;
    }
    {
      std::lock_guard<std::mutex> g(S.m);
      S.latest = tag;
      cur = S.installed;
    }
    std::string target = Target(tag);
    if (cur.empty()) Say(L"Latest release is " + Wide(target.c_str()) + L": ready to install.");
    else if (Newer(target, cur)) Say(L"Update available: " + Wide(cur.c_str()) + L" → " + Wide(target.c_str()), col::amber);
    else Say(L"QuestLHSync is up to date.", col::green);
  });
}

static void Uninstall() {
  RunBg([] {
    StopSteamVr();
    RunReg("removedriver", g_driver);
    std::error_code ec;
    fs::remove_all(g_driver, ec);
    fs::remove(g_state, ec);
    Say(L"QuestLHSync driver uninstalled.");
  });
}

// ---------------------------------------------------------------- page
struct Button { int x, y, w, h, id; std::wstring label; bool primary, enabled; };

static std::vector<Button> g_buttons;
static int g_hover = -1;
static bool g_svr = false;

static std::wstring Home(const fs::path &p) {  // ~/... for people
  std::string s = p.string();
  const char *h = getenv("HOME");
  if (h && *h && s.rfind(h, 0) == 0) s = "~" + s.substr(strlen(h));
  return Wide(s.c_str());
}

static void Draw(Canvas &cv) {
  std::wstring msg;
  std::string installed, latest;
  std::vector<std::pair<std::wstring, std::wstring>> logs;
  uint32_t accent;
  int pct;
  bool busy, err;
  {
    std::lock_guard<std::mutex> g(S.m);
    installed = S.installed; latest = S.latest; msg = S.msg; accent = S.accent;
    logs = S.logs; pct = S.pct; busy = S.busy; err = S.err;
  }
  std::string target = Target(latest);
  std::wstring winst = Wide(installed.c_str()), wtarget = Wide(target.c_str());
  bool outdated = !installed.empty() && !target.empty() && Newer(target, installed);

  Painter p(cv);
  Font h1(40, FW_SEMIBOLD), h2(22, FW_SEMIBOLD), body(21, FW_NORMAL), small(17, FW_NORMAL), label(16, FW_SEMIBOLD),
      big(28, FW_SEMIBOLD), mono(17, FW_NORMAL, L"Mono");
  p.Rect(0, 0, W, H, col::bg);

  // header
  int tw = p.Text(48, 34, L"QuestLHSync", h1, col::text);
  p.Text(48 + tw + 14, 50, L"by CreoleVR", h2, col::faint);
  p.Text(50, 88, L"Installer for the SteamVR driver", small, col::dim);

  std::wstring title;
  uint32_t sc;
  if (busy) { title = L"Working"; sc = col::amber; }
  else if (err) { title = L"Error"; sc = col::red; }
  else if (outdated) { title = L"Update available"; sc = col::amber; }
  else if (!installed.empty()) { title = L"Installed"; sc = col::green; }
  else { title = L"Not installed"; sc = col::grey; }
  {
    Font pill(22, FW_SEMIBOLD);
    int pw = p.Measure(title, pill) + 64, px = W - 48 - pw;
    p.Rect(px, 40, pw, 48, col::card2, 24);
    p.Dot(px + 26, 64, 7, sc);
    p.Text(px + 44, 50, title, pill, col::text);
  }

  // status line
  p.Rect(48, 124, W - 96, 52, col::card, 12);
  p.Rect(48, 124, 6, 52, accent ? accent : sc, 3);
  p.Text(70, 137, msg, body, col::text, 0, W - 140);

  // cards
  int top = 196, ch = 204, gap = 20, cw = (W - 96 - gap) / 2;
  int x1 = 48, x2 = x1 + cw + gap;
  for (int x : {x1, x2}) p.Rect(x, top, cw, ch, col::card, 14);
  auto row = [&](int x, int y, const wchar_t *k, const std::wstring &v, uint32_t vc = col::text) {
    p.Text(x + 22, y, k, small, col::dim);
    p.Text(x + cw - 22, y, v, small, vc, 2, cw - 150);
  };
  const std::wstring dash = L"—";

  p.Text(x1 + 22, top + 18, L"INSTALLED", label, col::faint);
  p.Text(x1 + 22, top + 44, installed.empty() ? dash : winst, big, col::text, 0, cw - 44);
  row(x1, top + 100, L"Folder", installed.empty() ? dash : Home(g_root));
  row(x1, top + 134, L"SteamVR", g_svr ? L"running" : L"closed", g_svr ? col::amber : col::text);
  row(x1, top + 168, L"Status", installed.empty() ? L"not installed" : outdated ? L"update available" : L"up to date",
      outdated ? col::amber : col::text);

  p.Text(x2 + 22, top + 18, L"LATEST RELEASE", label, col::faint);
  p.Text(x2 + 22, top + 44, target.empty() ? dash : wtarget, big, !target.empty() && !outdated ? col::green : col::text, 0, cw - 44);
  row(x2, top + 100, L"Source", !latest.empty() && target == latest ? L"GitHub releases" : L"inside this AppImage");
  row(x2, top + 134, L"Package", target.empty() ? dash : Wide(PackageName(target).c_str()));
  row(x2, top + 168, L"Registers via", L"vrpathreg");

  // log
  int ly = top + ch + 20, lh = 150;
  p.Rect(48, ly, W - 96, lh, col::card, 14);
  p.Text(70, ly + 16, L"LOG", label, col::faint);
  if (busy || pct) {
    int bw = 220, bx = W - 48 - 22 - bw;
    p.Rect(bx, ly + 22, bw, 8, col::card2, 4);
    if (pct) p.Rect(bx, ly + 22, std::max(8, bw * pct / 100), 8, col::blue, 4);
  }
  size_t n = logs.size();
  for (size_t i = 0; i < 5 && i < n; i++) {
    const auto &l = logs[n - 1 - i];
    p.Text(70, ly + 44 + 20 * (int)i, l.first + L"  " + l.second, mono, i == 0 ? col::text : col::dim, 0, W - 150);
  }
  if (!n) p.Text(70, ly + 44, dash, mono, col::dim);

  // buttons
  g_buttons.clear();
  int by = H - 66, bh = 46;
  std::wstring main_label = installed.empty() ? L"Install" : outdated ? L"Update to " + wtarget : L"Reinstall";
  g_buttons.push_back({48, by, outdated ? 260 : 200, bh, 0, main_label, installed.empty() || outdated, !busy && !target.empty()});
  int x = 48 + g_buttons[0].w + 16;
  g_buttons.push_back({x, by, 170, bh, 1, L"Uninstall", false, !busy && !installed.empty()});
  x += 170 + 16;
  g_buttons.push_back({x, by, 250, bh, 2, L"Check for updates", false, !busy});
  for (size_t i = 0; i < g_buttons.size(); i++) {
    auto &b = g_buttons[i];
    uint32_t fill = !b.enabled ? col::card : b.primary ? (int)i == g_hover ? 0x7aa0ff : col::blue : (int)i == g_hover ? col::line : col::card2;
    p.Rect(b.x, b.y, b.w, b.h, fill, 10);
    p.Text(b.x + b.w / 2, b.y + 11, b.label, h2, b.enabled ? col::text : col::faint, 1);
  }
}

static int HitTest(float x, float y) {
  for (size_t i = 0; i < g_buttons.size(); i++) {
    auto &b = g_buttons[i];
    if (b.enabled && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return (int)i;
  }
  return -1;
}

static void Click(SDL_Window *win, int id) {

  if (id == 2) return Check();
  if (SteamVrRunning()) g_svr = true;  // Install/Uninstall stop it themselves
  if (id == 0) return Install();
  const SDL_MessageBoxButtonData btn[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                          {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Uninstall"}};
  SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, win, "QuestLHSync", "Uninstall the QuestLHSync driver?", 2, btn, nullptr};
  int chosen = 0;
  if (SDL_ShowMessageBox(&box, &chosen) == 0 && chosen == 1) Uninstall();
}

int main(int argc, char **argv) {
  InitPaths();
  S.installed = ReadInstalled();
  if (argc >= 2) {
    std::string a = argv[1];
    g_cli = true;
    if (a == "--status") {
      std::string latest;
      try { latest = LatestTag(); } catch (const Fail &f) { latest = "(" + f.msg + ")"; }
      printf("installed: %s\nlatest: %s\nbundled: %s\nsteamvr: %s\n", S.installed.empty() ? "-" : S.installed.c_str(),
             latest.c_str(), g_bundled.empty() ? "-" : g_bundled.c_str(), SteamVrRunning() ? "running" : "closed");
      return 0;
    }
    if (a == "--install" || a == "--uninstall") {
      if (a == "--install") Install(); else Uninstall();
      std::lock_guard<std::mutex> g(S.m);
      return S.err ? 1 : 0;
    }
    printf("usage: %s [--install | --uninstall | --status]\n", argv[0]);
    return a == "--help" ? 0 : 2;
  }

  g_svr = SteamVrRunning();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "no display: %s\n(use --install or --uninstall without one)\n", SDL_GetError());
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Window *win = SDL_CreateWindow("QuestLHSync Installer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                     SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
  if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) { fprintf(stderr, "couldn't open a window: %s\n", SDL_GetError()); return 1; }
  SDL_RenderSetLogicalSize(ren, W, H);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, W * SS, H * SS);
  SDL_Cursor *hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND), *arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  Canvas cv(W * SS, H * SS);
  Say(L"Ready.");
  Check();
  uint32_t last_poll = 0;
  bool quit = false;
  while (!quit) {
    SDL_Event e;
    for (bool got = SDL_WaitEventTimeout(&e, 100); got; got = SDL_PollEvent(&e)) {
      float lx = 0, ly = 0;
      switch (e.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_LEAVE && g_hover != -1) { g_hover = -1; g_dirty = true; }
          else if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) g_dirty = true;
          break;
        case SDL_MOUSEMOTION: {
          SDL_RenderWindowToLogical(ren, e.motion.x, e.motion.y, &lx, &ly);
          int hv = HitTest(lx, ly);
          if (hv != g_hover) { g_hover = hv; g_dirty = true; }
          SDL_SetCursor(hv >= 0 ? hand : arrow);
          break;
        }
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT) {
            SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
            int hv = HitTest(lx, ly);
            if (hv >= 0) Click(win, g_buttons[hv].id);
          }
          break;
      }
    }
    uint32_t now = SDL_GetTicks();
    if (now - last_poll >= 1000) {
      last_poll = now;
      bool r = SteamVrRunning();
      if (r != g_svr) { g_svr = r; g_dirty = true; }
    }
    if (g_dirty.exchange(false)) {
      Draw(cv);
      SDL_UpdateTexture(tex, nullptr, cv.px, cv.w * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
    }
  }
  return 0;
}
