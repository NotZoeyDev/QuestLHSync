// questlhsync-xr: QuestLHSync for WiVRn / Monado (no SteamVR host to hook there), a desktop app: the dashboard page in a
// window, the solver on a worker thread. It does what the SteamVR driver does, from outside the runtime:
//   - the same headset link and solver (net.cpp, sync.cpp): lhsyncd's camera sightings of the base stations, and the
//     headset's poses, give the 4-DOF transform from the lighthouse universe to the headset's;
//   - the headset's and the lighthouse devices' poses come over OpenXR (XR_MNDX_xdev_space, a headless session: what
//     motoc uses), the base stations' from SteamVR's lighthousedb.json (Monado's steamvr_lh driver doesn't make them
//     devices);
//   - the transform is applied as the lighthouse devices' tracking origin offset, through libmonado.
// Both libraries are opened at run time, from the active OpenXR runtime's manifest (MND_libmonado_path).
//   questlhsync-xr [--host IP[,IP]] [--headset SERIAL] [--runtime manifest.json]   the window (--no-window: none)
//   questlhsync-xr --dump                                                          list devices and origins, then poses
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_MNDX_xdev_space.h>

#include "../common/datadir_linux.h"
#include "../common/qlhs_status.h"
#include "../driver/json.h"
#include "../driver/mathx.h"
#include "../driver/net.h"
#include "../driver/sync.h"
#include "monado/monado.h"

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

static const int W = 1280, H = 800, SS = 2;  // the dashboard page's size, supersampling
#include "../overlay/canvas_linux.h"
#include "../overlay/page.h"

static std::atomic<bool> g_quit{false}, g_blocking{false};  // blocking: inside a call that waits for a headset
static void OnSignal(int) {
  g_quit = true;
  if (g_blocking) _exit(0);  // WiVRn's xrCreateInstance waits until a headset connects
}
static FILE *g_logf;
static std::mutex g_log_m;

static void Log(const std::string &s) {
  std::lock_guard<std::mutex> g(g_log_m);
  time_t t = time(nullptr);
  tm tm;
  localtime_r(&t, &tm);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  fprintf(stderr, "%s %s\n", b, s.c_str());
  if (g_logf) {
    fprintf(g_logf, "%s %s\n", b, s.c_str());
    fflush(g_logf);
  }
}

static std::string Fmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

static std::string Slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ---------------------------------------------------------------- rigid transforms
struct Rig { Quat q; V3 p; };
static Rig operator*(const Rig &a, const Rig &b) { return {a.q * b.q, a.p + ToM3(a.q) * b.p}; }
static Rig Inv(const Rig &a) {
  Quat c{a.q.w, -a.q.x, -a.q.y, -a.q.z};
  return {c, ToM3(c) * (a.p * -1.0)};
}
static Rig FromXr(const XrPosef &p) {
  return {{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}, {p.position.x, p.position.y, p.position.z}};
}
static Rig FromMnd(const mnd_pose_t &p) {
  return {{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z}, {p.position.x, p.position.y, p.position.z}};
}
static mnd_pose_t ToMnd(const Rig &r) {
  mnd_pose_t p;
  p.orientation = {(float)r.q.x, (float)r.q.y, (float)r.q.z, (float)r.q.w};
  p.position = {(float)r.p.x, (float)r.p.y, (float)r.p.z};
  return p;
}

// ---------------------------------------------------------------- libmonado
struct Mnd {
  void *lib = nullptr;
  mnd_root_t *root = nullptr;
  mnd_result_t (*create)(mnd_root_t **) = nullptr;
  void (*destroy)(mnd_root_t **) = nullptr;
  mnd_result_t (*count)(mnd_root_t *, uint32_t *) = nullptr;
  mnd_result_t (*info)(mnd_root_t *, uint32_t, uint32_t *, const char **) = nullptr;
  mnd_result_t (*info_str)(mnd_root_t *, uint32_t, mnd_property_t, const char **) = nullptr;
  mnd_result_t (*info_u32)(mnd_root_t *, uint32_t, mnd_property_t, uint32_t *) = nullptr;
  mnd_result_t (*role)(mnd_root_t *, const char *, int32_t *) = nullptr;
  mnd_result_t (*get_ref)(mnd_root_t *, mnd_reference_space_type_t, mnd_pose_t *) = nullptr;
  mnd_result_t (*get_origin)(mnd_root_t *, uint32_t, mnd_pose_t *) = nullptr;
  mnd_result_t (*set_origin)(mnd_root_t *, uint32_t, const mnd_pose_t *) = nullptr;
  mnd_result_t (*origin_count)(mnd_root_t *, uint32_t *) = nullptr;
  mnd_result_t (*origin_name)(mnd_root_t *, uint32_t, const char **) = nullptr;

  bool Open(const std::string &path) {
    lib = dlopen(path.c_str(), RTLD_NOW);
    if (!lib) { Log(Fmt("libmonado: %s", dlerror())); return false; }
#define SYM(f, n) f = (decltype(f))dlsym(lib, n)
    SYM(create, "mnd_root_create");
    SYM(destroy, "mnd_root_destroy");
    SYM(count, "mnd_root_get_device_count");
    SYM(info, "mnd_root_get_device_info");
    SYM(info_str, "mnd_root_get_device_info_string");
    SYM(info_u32, "mnd_root_get_device_info_u32");
    SYM(role, "mnd_root_get_device_from_role");
    SYM(get_ref, "mnd_root_get_reference_space_offset");
    SYM(get_origin, "mnd_root_get_tracking_origin_offset");
    SYM(set_origin, "mnd_root_set_tracking_origin_offset");
    SYM(origin_count, "mnd_root_get_tracking_origin_count");
    SYM(origin_name, "mnd_root_get_tracking_origin_name");
#undef SYM
    if (!create || !destroy || !count || !info || !info_str || !info_u32 || !role || !get_ref || !get_origin || !set_origin ||
        !origin_count || !origin_name) {
      Log("libmonado: too old (no tracking origin offsets)");
      return false;
    }
    return true;
  }
  bool Connect() {
    if (root) return true;
    return create(&root) == MND_SUCCESS && root;
  }
  void Disconnect() {
    if (root) destroy(&root);
    root = nullptr;
  }
};

// ---------------------------------------------------------------- OpenXR
#define XR_FNS(X)                                                                                                    \
  X(xrDestroyInstance) X(xrGetSystem) X(xrCreateSession) X(xrDestroySession) X(xrPollEvent) X(xrBeginSession)          \
  X(xrEndSession) X(xrCreateReferenceSpace) X(xrDestroySpace) X(xrLocateSpace) X(xrConvertTimespecTimeToTimeKHR)       \
  X(xrCreateXDevListMNDX) X(xrGetXDevListGenerationNumberMNDX) X(xrEnumerateXDevsMNDX) X(xrGetXDevPropertiesMNDX)      \
  X(xrDestroyXDevListMNDX) X(xrCreateXDevSpaceMNDX) X(xrGetInstanceProperties)

struct Xr {
  void *loader = nullptr;
  PFN_xrGetInstanceProcAddr gipa = nullptr;
  XrInstance inst = XR_NULL_HANDLE;
  XrSystemId sys = XR_NULL_SYSTEM_ID;
  XrSession ses = XR_NULL_HANDLE;
  XrSpace stage = XR_NULL_HANDLE;
  XrXDevListMNDX list = XR_NULL_HANDLE;
  bool running = false;
#define X(n) PFN_##n n = nullptr;
  XR_FNS(X)
#undef X

  bool LoadLoader() {
    if (loader) return true;
    for (const char *n : {"libopenxr_loader.so.1", "libopenxr_loader.so"})
      if ((loader = dlopen(n, RTLD_NOW))) break;
    if (!loader) { Log("no OpenXR loader (libopenxr_loader.so.1)"); return false; }
    gipa = (PFN_xrGetInstanceProcAddr)dlsym(loader, "xrGetInstanceProcAddr");
    return gipa != nullptr;
  }

  // the first runtime that works, as the loader finds it
  XrResult Connect() {
    if (!LoadLoader()) return XR_ERROR_RUNTIME_UNAVAILABLE;
    PFN_xrCreateInstance create = nullptr;
    gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
    if (!create) return XR_ERROR_RUNTIME_UNAVAILABLE;
    const char *exts[] = {XR_MND_HEADLESS_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
                          XR_MNDX_XDEV_SPACE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "QuestLHSync");
    strcpy(ci.applicationInfo.engineName, "QuestLHSync");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 3;
    ci.enabledExtensionNames = exts;
    XrResult r = create(&ci, &inst);
    if (XR_FAILED(r)) return r;
#define X(n) gipa(inst, #n, (PFN_xrVoidFunction *)&n);
    XR_FNS(X)
#undef X
    r = xrGetSystem2();
    if (XR_FAILED(r)) { Close(); return r; }
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO};
    sc.systemId = sys;
    r = xrCreateSession(inst, &sc, &ses);
    if (XR_FAILED(r)) { Close(); return r; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    rs.poseInReferenceSpace.orientation.w = 1;
    r = xrCreateReferenceSpace(ses, &rs, &stage);
    if (XR_FAILED(r)) { Close(); return r; }
    return XR_SUCCESS;
  }

  XrResult xrGetSystem2() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    return xrGetSystem(inst, &gi, &sys);
  }

  void Close() {
    if (list) xrDestroyXDevListMNDX(list);
    if (stage) xrDestroySpace(stage);
    if (ses) xrDestroySession(ses);
    if (inst) xrDestroyInstance(inst);
    list = XR_NULL_HANDLE; stage = XR_NULL_HANDLE; ses = XR_NULL_HANDLE; inst = XR_NULL_HANDLE;
    running = false;
  }

  // false when the session is over (runtime stopped, headset left)
  bool Poll() {
    for (;;) {
      XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
      XrResult r = xrPollEvent(inst, &ev);
      if (r == XR_EVENT_UNAVAILABLE) return true;
      if (XR_FAILED(r)) return false;
      if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) return false;
      if (ev.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
      auto *s = (XrEventDataSessionStateChanged *)&ev;
      if (s->state == XR_SESSION_STATE_READY && !running) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(ses, &bi))) running = true;
      } else if (s->state == XR_SESSION_STATE_STOPPING && running) {
        xrEndSession(ses);
        running = false;
      } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
        return false;
      }
    }
  }

  XrTime Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    XrTime t = 0;
    xrConvertTimespecTimeToTimeKHR(inst, &ts, &t);
    return t;
  }
};

// ---------------------------------------------------------------- devices
struct Dev {
  XrXDevIdMNDX id;
  std::string name, serial;
  XrSpace space = XR_NULL_HANDLE;
  int mnd = -1;                  // libmonado's device index
  uint32_t origin = UINT32_MAX;  // its tracking origin
  bool hmd = false, lighthouse = false;
};

static std::string Cut(const char *s) { return s ? s : ""; }

// every xdev with a space, matched to libmonado's device of the same serial (else name) for its tracking origin
static bool Enumerate(Xr &xr, Mnd &mnd, std::vector<Dev> &out, std::map<uint32_t, std::string> &origins) {
  for (auto &d : out)
    if (d.space) xr.xrDestroySpace(d.space);
  out.clear();
  origins.clear();
  if (xr.list) xr.xrDestroyXDevListMNDX(xr.list);
  xr.list = XR_NULL_HANDLE;
  XrCreateXDevListInfoMNDX li{XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX};
  if (XR_FAILED(xr.xrCreateXDevListMNDX(xr.ses, &li, &xr.list))) return false;
  uint32_t n = 0;
  xr.xrEnumerateXDevsMNDX(xr.list, 0, &n, nullptr);
  std::vector<XrXDevIdMNDX> ids(n);
  if (n) xr.xrEnumerateXDevsMNDX(xr.list, n, &n, ids.data());

  uint32_t nm = 0, no = 0;
  mnd.count(mnd.root, &nm);
  mnd.origin_count(mnd.root, &no);
  for (uint32_t i = 0; i < no; i++) {
    const char *nm_ = nullptr;
    if (mnd.origin_name(mnd.root, i, &nm_) == MND_SUCCESS) origins[i] = Cut(nm_);
  }
  int32_t head = -1;
  mnd.role(mnd.root, "head", &head);
  struct M { std::string name, serial; uint32_t origin; };
  std::vector<M> ms(nm);
  for (uint32_t i = 0; i < nm; i++) {
    uint32_t id;
    const char *name = nullptr, *serial = nullptr;
    mnd.info(mnd.root, i, &id, &name);
    if (mnd.info_str(mnd.root, i, MND_PROPERTY_SERIAL_STRING, &serial) != MND_SUCCESS) serial = nullptr;
    uint32_t o = UINT32_MAX;
    if (mnd.info_u32(mnd.root, i, MND_PROPERTY_TRACKING_ORIGIN_U32, &o) != MND_SUCCESS) o = UINT32_MAX;
    ms[i] = {Cut(name), Cut(serial), o};
  }
  for (XrXDevIdMNDX id : ids) {
    XrGetXDevInfoMNDX gi{XR_TYPE_GET_XDEV_INFO_MNDX};
    gi.id = id;
    XrXDevPropertiesMNDX pr{XR_TYPE_XDEV_PROPERTIES_MNDX};
    if (XR_FAILED(xr.xrGetXDevPropertiesMNDX(xr.list, &gi, &pr)) || !pr.canCreateSpace) continue;
    Dev d;
    d.id = id;
    d.name = pr.name;
    d.serial = pr.serial;
    XrCreateXDevSpaceInfoMNDX si{XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX};
    si.xdevList = xr.list;
    si.id = id;
    si.offset.orientation.w = 1;
    if (XR_FAILED(xr.xrCreateXDevSpaceMNDX(xr.ses, &si, &d.space))) continue;
    for (uint32_t i = 0; i < nm; i++)
      if ((!d.serial.empty() && ms[i].serial == d.serial) || (d.serial.empty() && ms[i].name == d.name)) {
        d.mnd = (int)i;
        d.origin = ms[i].origin;
        d.hmd = (int)i == head;
        break;
      }
    d.lighthouse = d.serial.rfind("LHR-", 0) == 0;
    out.push_back(std::move(d));
  }
  return true;
}

static bool Locate(Xr &xr, const Dev &d, XrTime t, Rig &out) {
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xr.xrLocateSpace(d.space, xr.stage, t, &loc))) return false;
  const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                    XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  if ((loc.locationFlags & need) != need) return false;
  out = FromXr(loc.pose);
  return true;
}

// ---------------------------------------------------------------- base stations (SteamVR's lighthousedb.json)
// known_universes[].base_stations[].target_pose.pose = qx qy qz qw tx ty tz, the universe in the station's frame (target
// 0): the station sits at -R^T t, turned by R^T. The lighthouse driver reports its devices in that universe tilted to
// gravity by the universe's own pitch and roll, Rx(-pitch) Rz(-roll). (Checked against stations the SteamVR driver
// measured: positions within 5 cm, orientations within 0.5 deg.)
struct Stations {
  std::map<std::string, std::pair<V3, M3>> raw;
  std::map<std::string, int> channel;
};

static std::string SteamConfigDir() {
  const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
  std::string base = x && *x ? x : std::string(h ? h : ".") + "/.config";
  JVal root;
  if (JParse(Slurp(base + "/openvr/openvrpaths.vrpath"), root))
    if (const JVal *a = root.get("config"))
      if (a->t == JVal::Arr && !a->a.empty() && a->a[0].t == JVal::Str) return a->a[0].s;
  return std::string(h ? h : ".") + "/.local/share/Steam/config";
}

static M3 Rxm(double a) {
  M3 r;
  double c = cos(a), s = sin(a);
  r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c;
  return r;
}
static M3 Rzm(double a) {
  M3 r;
  double c = cos(a), s = sin(a);
  r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c;
  return r;
}

static bool LoadStations(const std::string &path, Stations &out) {
  JVal db;
  if (!JParse(Slurp(path), db)) return false;
  const JVal *us = db.get("known_universes");
  if (!us || us->t != JVal::Arr) return false;
  const JVal *best = nullptr;
  size_t best_n = 0;
  double best_id = -1;
  for (const JVal &u : us->a) {
    const JVal *bs = u.get("base_stations");
    if (!bs || bs->t != JVal::Arr) continue;
    const JVal *id = u.get("id");
    double idn = id ? (id->t == JVal::Str ? atof(id->s.c_str()) : id->num()) : 0;
    if (bs->a.size() > best_n || (bs->a.size() == best_n && idn > best_id)) { best = &u; best_n = bs->a.size(); best_id = idn; }
  }
  if (!best) return false;
  double pitch = 0, roll = 0;
  if (const JVal *t = best->get("tilt")) {
    if (const JVal *p = t->get("pitch")) pitch = p->num();
    if (const JVal *r = t->get("roll")) roll = r->num();
  }
  M3 tilt = Rxm(-pitch) * Rzm(-roll);
  out.raw.clear();
  for (const JVal &b : best->get("base_stations")->a) {
    const JVal *sn = b.get("base_serial_number"), *tp = b.get("target_pose");
    if (!sn || !tp) continue;
    const JVal *ts = tp->get("target_serial_number"), *pp = tp->get("pose");
    std::vector<double> p = pp ? pp->nums() : std::vector<double>();
    if (p.size() != 7 || (ts && ts->num() != 0)) continue;
    M3 Ru = ToM3(Quat{p[3], p[0], p[1], p[2]});
    V3 t{p[4], p[5], p[6]};
    M3 Rs = T(Ru);                       // station's orientation in the universe
    V3 ps = (Rs * t) * -1.0;             // and its position: -R^T t
    out.raw[Fmt("LHB-%08X", (unsigned)sn->num())] = {tilt * ps, tilt * Rs};
  }
  out.channel.clear();
  if (const JVal *bs = db.get("base_stations"))
    for (const JVal &b : bs->a) {
      const JVal *cfg = b.get("config"), *dyn = b.get("dynamic_states");
      const JVal *sn = cfg ? cfg->get("serialNumber") : nullptr;
      if (!sn || !dyn || dyn->t != JVal::Arr) continue;
      for (const JVal &d : dyn->a)
        if (const JVal *st = d.get("dynamic_state"))
          if (const JVal *ch = st->get("sobChannel"))
            if (ch->num() >= 1 && ch->num() <= 16) out.channel[Fmt("LHB-%08X", (unsigned)sn->num())] = (int)ch->num();
    }
  return !out.raw.empty();
}

// ---------------------------------------------------------------- the runtime's manifest
static std::string ManifestPath(const std::string &given) {
  if (!given.empty()) return given;
  if (const char *e = getenv("XR_RUNTIME_JSON")) return e;
  const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
  std::string base = x && *x ? x : std::string(h ? h : ".") + "/.config";
  for (std::string p : {base + "/openxr/1/active_runtime.json", std::string("/etc/xdg/openxr/1/active_runtime.json"),
                        std::string("/usr/share/openxr/1/openxr_wivrn.json"), std::string("/usr/share/openxr/1/openxr_monado.json")})
    if (access(p.c_str(), R_OK) == 0) return p;
  return "";
}

static std::string LibMonadoPath(const std::string &manifest) {
  if (const char *e = getenv("QLHS_LIBMONADO")) return e;
  JVal j;
  if (!manifest.empty() && JParse(Slurp(manifest), j))
    if (const JVal *r = j.get("runtime"))
      if (const JVal *p = r->get("MND_libmonado_path"))
        if (p->t == JVal::Str) {
          std::string s = p->s;
          if (!s.empty() && s[0] != '/') {  // relative to the manifest's own folder: active_runtime.json is a symlink
            char real[4096];
            std::string m = realpath(manifest.c_str(), real) ? real : manifest;
            s = m.substr(0, m.rfind('/') + 1) + s;
            if (realpath(s.c_str(), real)) s = real;
          }
          return s;
        }
  return "libmonado.so.25";
}

// ---------------------------------------------------------------- what the window shows
static std::mutex g_st_m;
static QlhsStatus g_st;
static bool g_st_have = false;
static std::atomic<int> g_cmd{0}, g_cmd_seq{0}, g_rc{0};
static std::atomic<bool> g_engine_done{false};

struct Options {
  std::vector<std::string> hosts;
  std::string preferred, runtime;
  bool dump = false;
};

static void Copy(char *dst, size_t n, const std::string &s) {
  strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = 0;
}

// ---------------------------------------------------------------- the engine: OpenXR in, the tracking origin out
static void Engine(const Options &o) {
  const bool dump = o.dump;
  std::string dir = QlhsDataDir();
  std::string manifest = ManifestPath(o.runtime);
  if (!getenv("XR_RUNTIME_JSON") && !manifest.empty()) setenv("XR_RUNTIME_JSON", manifest.c_str(), 0);
  std::string monado_path = LibMonadoPath(manifest);
  Log("QuestLHSync " QLHS_RELEASE " for WiVRn/Monado: runtime " + (manifest.empty() ? std::string("(loader's choice)") : manifest) +
      ", libmonado " + monado_path);
  bool wivrn = manifest.find("wivrn") != std::string::npos || monado_path.find("wivrn") != std::string::npos;

  Mnd mnd;
  if (!mnd.Open(monado_path)) { g_rc = 1; g_quit = true; g_engine_done = true; return; }

  SyncConfig cfg;
  cfg.dir = dir;
  Sync sync(cfg, [](const std::string &s) { Log(s); });
  HeadsetLink link(&sync, [](const std::string &s) { Log(s); }, [&](double t, const std::string &s) { sync.Rec(t, "%s", s.c_str()); });
  link.SetMemory(dir + "/headset.txt");
  link.SetHosts(o.hosts);
  link.SetPreferred(o.preferred);
  link.SetFamily("");  // whichever headset answers: WiVRn names none of them
  if (!dump) link.Start();

  Xr xr;
  std::vector<Dev> devs;
  std::map<uint32_t, std::string> origins;
  uint64_t gen = 0;
  Stations stations;
  time_t db_mtime = 0;
  Rig stage_off{}, lh_off{};   // reference space Stage's offset, the lighthouse tracking origin's
  std::set<uint32_t> lh_origins;
  double next_connect = 0, last_tick = 0, last_db = 0, last_gen = 0, last_stage = 0, last_status = 0, last_dump = 0, last_pub = 0, last_ext = 0;
  bool said_wait = false, applied_any = false, recording = false, paused_ = false;
  Rig applied{};
  int cmd_seen = 0;
  std::string hmd_name;
  std::vector<std::string> pending_log;  // the page's log lines

  auto say = [&](const std::string &s) {
    time_t t = time(nullptr);
    tm tm;
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &tm);
    pending_log.push_back(std::string(b) + "  " + s);
    if (pending_log.size() > 16) pending_log.erase(pending_log.begin());
  };
  auto note = [&](const std::string &s) { Log(s); say(s); };

  auto set_recording = [&](bool on) {
    if (on == recording) return;
    recording = on;
    if (!on) { sync.SetRecord(nullptr); note("recording off"); return; }
    std::string rd = dir + "/recordings";
    mkdir(rd.c_str(), 0755);
    time_t t = time(nullptr);
    tm tm;
    localtime_r(&t, &tm);
    char b[64];
    strftime(b, sizeof b, "/qlhs-xr-%Y%m%d-%H%M%S.txt", &tm);
    FILE *f = fopen((rd + b).c_str(), "w");
    if (!f) { note("can't open a recording file"); recording = false; return; }
    setvbuf(f, nullptr, _IOFBF, 1 << 16);
    sync.SetRecord(f);
    sync.Rec(QpcNow(), "I QuestLHSync recording (qlhs_replay reads it)");
    note(std::string("recording to recordings/") + (b + 1));
  };

  // the page's status, as the SteamVR driver publishes it
  auto publish = [&](double now, bool in_xr) {
    QlhsStatus t{};
    t.magic = QLHS_MAGIC;
    t.version = QLHS_VERSION;
    Sync::Status s = sync.GetStatus(now);
    bool have_hmd = false;
    for (auto &d : devs) if (d.hmd) { have_hmd = true; hmd_name = d.name; }
    auto ls = link.state();
    int state;
    if (!in_xr || !have_hmd) state = QLHS_NO_HMD;
    else if (ls == HeadsetLink::kSearching || ls == HeadsetLink::kIdle) state = QLHS_SEARCHING;
    else if (ls == HeadsetLink::kConnecting) state = QLHS_CONNECTING;
    else if (now - link.last_frame() > 3 && now - link.connected_at() > 5) state = QLHS_NO_CAMERAS;
    else if (s.nstations < 2) state = QLHS_NO_STATIONS;
    else if (s.locked) state = QLHS_LOCKED;
    else state = QLHS_ACQUIRING;
    t.state = state;
    t.updated = now;
    if (in_xr && have_hmd) {
      Copy(t.hmd, sizeof t.hmd, hmd_name.empty() ? "headset" : hmd_name);
      Copy(t.hmd_system, sizeof t.hmd_system, wivrn ? "WiVRn" : "Monado");
    }
    Copy(t.headset, sizeof t.headset, link.model());
    Copy(t.headset_addr, sizeof t.headset_addr, ls == HeadsetLink::kConnected ? link.addr() : "");
    Copy(t.headset_fw, sizeof t.headset_fw, link.fw());
    t.cam_fps = s.cam_fps;
    t.sight_rate = s.sight_rate;
    t.spot_rate = s.spot_rate;
    t.head_still = s.head_still;
    t.rtt_ms = s.rtt * 1000;
    t.expo_ms = s.expo * 1000;
    t.expo_learned = s.timing_learned;
    t.nst = (int)std::min<size_t>(s.st.size(), 8);
    for (int i = 0; i < t.nst; i++) {
      auto &e = s.st[i];
      Copy(t.st[i].serial, sizeof t.st[i].serial, e.serial);
      t.st[i].support = e.support;
      t.st[i].anchor = e.anchor;
      t.st[i].measured = e.measured;
      t.st[i].last_seen = e.last_seen;
      t.st[i].dist = e.dist;
    }
    t.locked = s.locked;
    t.cond = s.cond;
    t.yaw_deg = s.has_x ? s.x[0] * kDeg : 0;
    for (int i = 0; i < 3; i++) t.t[i] = s.has_x ? s.x[i + 1] : 0;
    t.med_deg = s.med;
    t.nfit = s.n;
    t.paused = paused_;
    t.locked_for = s.locked_for;
    t.lag_cm = s.lag_cm;
    t.recording = recording;
    {
      std::lock_guard<std::mutex> g(g_st_m);
      for (auto &l : pending_log) {
        Copy(g_st.log[g_st.nlog % 8], sizeof g_st.log[0], l);
        g_st.nlog++;
      }
      pending_log.clear();
      std::memcpy(t.log, g_st.log, sizeof t.log);
      t.nlog = g_st.nlog;
      g_st = t;
      g_st_have = true;
    }
  };

  while (!g_quit) {
    double now = QpcNow();
    if (!dump && now - last_pub >= 0.25) {
      last_pub = now;
      publish(now, xr.ses != XR_NULL_HANDLE);
    }
    if (int c = g_cmd_seq.load(); c != cmd_seen) {  // from the page's buttons
      cmd_seen = c;
      switch (g_cmd.load()) {
        case QLHS_CMD_PAUSE: paused_ = true; sync.SetPaused(true); note("corrections paused"); break;
        case QLHS_CMD_RESUME: paused_ = false; sync.SetPaused(false); note("corrections resumed"); break;
        case QLHS_CMD_RECORD_ON: set_recording(true); break;
        case QLHS_CMD_RECORD_OFF: set_recording(false); break;
      }
    }
    if (!xr.ses) {
      link.SetWanted(false);
      if (now < next_connect) { usleep(100000); continue; }
      next_connect = now + 3;
      if (!said_wait) { said_wait = true; note("waiting for a headset to connect to WiVRn/Monado"); }
      if (!dump) publish(now, false);  // the connect below waits for a headset, as long as that takes
      g_blocking = true;
      XrResult r = xr.Connect();
      g_blocking = false;
      if (XR_FAILED(r) || !mnd.Connect()) {
        if (!said_wait) note(Fmt("waiting for WiVRn/Monado with a headset connected (OpenXR result %d)", (int)r));
        said_wait = true;
        if (xr.inst) xr.Close();
        continue;
      }
      said_wait = false;
      note("connected to the OpenXR runtime");
      gen = ~0ull;
    }
    if (!xr.Poll()) {
      note("the OpenXR session ended: reconnecting");
      for (auto &d : devs) d.space = XR_NULL_HANDLE;  // died with the session
      devs.clear();
      xr.Close();
      mnd.Disconnect();
      link.SetWanted(false);
      next_connect = now + 2;
      continue;
    }
    if (!xr.running) { usleep(50000); continue; }

    if (now - last_gen >= 1) {  // the device list changed (a tracker found, a controller off)
      last_gen = now;
      uint64_t g = 0;
      bool have = xr.list && XR_SUCCEEDED(xr.xrGetXDevListGenerationNumberMNDX(xr.list, &g));
      if (!have || g != gen) {
        if (Enumerate(xr, mnd, devs, origins)) {
          if (xr.list) xr.xrGetXDevListGenerationNumberMNDX(xr.list, &gen);
          lh_origins.clear();
          for (auto &d : devs) {
            Log(Fmt("device %s (%s): origin %s%s%s", d.name.c_str(), d.serial.c_str(),
                    d.origin == UINT32_MAX ? "?" : origins[d.origin].c_str(), d.hmd ? ", the headset" : "",
                    d.lighthouse ? ", lighthouse" : ""));
            if (d.hmd) say("headset: " + d.name);
            else if (d.lighthouse) say("lighthouse device " + d.serial);
            if (d.lighthouse && d.origin != UINT32_MAX) lh_origins.insert(d.origin);
          }
          if (!lh_origins.empty()) {
            uint32_t og = *lh_origins.begin();
            mnd_pose_t cur;
            if (mnd.get_origin(mnd.root, og, &cur) == MND_SUCCESS) lh_off = FromMnd(cur);
          }
        }
      }
    }
    if (now - last_stage >= 0.25) {
      last_stage = now;
      mnd_pose_t s;
      if (mnd.get_ref(mnd.root, MND_SPACE_REFERENCE_TYPE_STAGE, &s) == MND_SUCCESS) stage_off = FromMnd(s);
      if (!lh_origins.empty() && mnd.get_origin(mnd.root, *lh_origins.begin(), &s) == MND_SUCCESS) {
        Rig cur = FromMnd(s);  // the live offset: something else (motoc, a profile script) may have set it
        double dp = norm(cur.p - lh_off.p), da = QuatDeg(cur.q, lh_off.q);
        if (applied_any && (dp > 0.0005 || da > 0.05) && now - last_ext >= 5) {
          last_ext = now;
          Log(Fmt("the lighthouse tracking origin's offset was changed by something else (%.1f cm, %.2f deg): rewriting ours",
                  dp * 100, da));
        }
        lh_off = cur;  // raw poses are divided by what is really applied; the next tick rewrites ours if it differs
        if (applied_any && (dp > 0.0005 || da > 0.05)) applied = cur;
      }
    }
    if (now - last_db >= 2) {
      last_db = now;
      std::string path = SteamConfigDir() + "/lighthouse/lighthousedb.json";
      struct stat sb;
      if (stat(path.c_str(), &sb) == 0 && sb.st_mtime != db_mtime) {  // SteamVR's lighthouse driver rewrites it as it learns
        db_mtime = sb.st_mtime;
        Stations st;
        if (LoadStations(path, st)) {
          stations = st;
          sync.SetStationsRaw(stations.raw);
          sync.SetChannels(stations.channel);
          note(Fmt("%zu base stations from lighthousedb.json", stations.raw.size()));
        }
      }
    }

    // poses at this moment
    XrTime xt = xr.Now();
    bool have_hmd = false;
    for (auto &d : devs) {
      if (!d.hmd && !d.lighthouse) continue;
      Rig loc;
      if (!Locate(xr, d, xt, loc)) continue;
      Rig g = stage_off * loc;  // in Monado's global space
      int idx = (int)d.id;
      if (d.hmd) {
        have_hmd = true;
        if (!dump) sync.OnHmdPose(now, g.q, g.p);
      } else {
        Rig raw = Inv(lh_off) * g;  // the lighthouse driver's own frame, whatever offset is applied now
        if (!dump) sync.OnBodyPose(idx, now, raw.p);
        if (dump && now - last_dump >= 1)
          Log(Fmt("  %s raw %.3f %.3f %.3f", d.serial.c_str(), raw.p.x, raw.p.y, raw.p.z));
      }
      if (dump && d.hmd && now - last_dump >= 1) Log(Fmt("  HMD %.3f %.3f %.3f", g.p.x, g.p.y, g.p.z));
    }
    if (dump) {
      if (now - last_dump >= 1) last_dump = now;
      usleep(8000);
      continue;
    }
    link.SetWanted(have_hmd);

    if (now - last_tick >= 0.05) {  // 20 Hz, as the driver
      last_tick = now;
      Transform x = sync.Tick(now);
      if (x.active && !lh_origins.empty()) {
        Rig want{x.q, x.t};
        bool far = norm(want.p) > 100;  // an anomaly: not worth applying
        double dp = norm(want.p - applied.p), da = QuatDeg(want.q, applied.q);
        if (!far && (!applied_any || dp > 0.0002 || da > 0.01)) {
          bool ok = true;
          mnd_pose_t mp = ToMnd(want);
          for (uint32_t og : lh_origins) ok = mnd.set_origin(mnd.root, og, &mp) == MND_SUCCESS && ok;
          if (ok) { applied = want; lh_off = want; applied_any = true; }
          else Log("couldn't set the lighthouse tracking origin's offset");
        }
      }
    }
    if (now - last_status >= 15) {
      last_status = now;
      Sync::Status s = sync.GetStatus(now);
      Log(Fmt("%s: %d base stations in sight, %.0f frames/s, %s", link.state() == HeadsetLink::kConnected ? "headset connected" : "no headset link",
              s.nstations, s.cam_fps, s.locked ? "locked" : "acquiring"));
    }
    usleep(4000);
  }
  if (recording) sync.SetRecord(nullptr);
  link.Stop();
  xr.Close();
  mnd.Disconnect();
  g_engine_done = true;
}

// ---------------------------------------------------------------- the window
static int HitAt(const Page &pg, float mx, float my) {
  for (size_t i = 0; i < pg.buttons.size(); i++) {
    auto &b = pg.buttons[i];
    if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return (int)i;
  }
  return -1;
}

static int Window() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    Log(Fmt("no display (%s): running without a window", SDL_GetError()));
    return -1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Window *win = SDL_CreateWindow("QuestLHSync for WiVRn", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W * 3 / 4,
                                     H * 3 / 4, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, 0) : nullptr;
  if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, W, H) : nullptr;  // R, G, B, A bytes
  if (!tex) {
    Log(Fmt("couldn't open a window (%s): running without one", SDL_GetError()));
    return -1;
  }
  SDL_RenderSetLogicalSize(ren, W, H);
  SDL_Cursor *hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND), *arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  std::vector<uint8_t> icon;
  DrawIcon(icon, 64);
  if (SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(icon.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_ABGR8888))
    SDL_SetWindowIcon(win, s), SDL_FreeSurface(s);

  Canvas big(W * SS, H * SS);
  Page pg;
  std::vector<uint8_t> rgba;
  QlhsStatus st{}, prev{};
  double last_upd = 0;
  uint64_t last_change = SDL_GetTicks64(), last_draw = 0;
  bool dirty = true;
  int down = -1;
  while (!g_quit) {
    SDL_Event e;
    for (bool got = SDL_WaitEventTimeout(&e, 50); got; got = SDL_PollEvent(&e)) {
      float lx = 0, ly = 0;
      switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_LEAVE && pg.hover >= 0) { pg.hover = -1; dirty = true; }
          else if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                   e.window.event == SDL_WINDOWEVENT_RESTORED) dirty = true;
          break;
        case SDL_MOUSEMOTION: {
          SDL_RenderWindowToLogical(ren, e.motion.x, e.motion.y, &lx, &ly);
          int h = HitAt(pg, lx, ly);
          if (h != pg.hover) { pg.hover = h; dirty = true; }
          SDL_SetCursor(h >= 0 ? hand : arrow);
          break;
        }
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) {
            SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
            down = HitAt(pg, lx, ly);
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT) {
            SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
            int h = HitAt(pg, lx, ly);
            if (h >= 0 && h == down) {
              g_cmd = pg.buttons[h].cmd;
              g_cmd_seq++;
              dirty = true;
            }
            down = -1;
          }
          break;
      }
    }
    uint64_t now = SDL_GetTicks64();
    bool have;
    {
      std::lock_guard<std::mutex> g(g_st_m);
      have = g_st_have;
      if (have) st = g_st;
    }
    if (!have) memset(&st, 0, sizeof st);
    if (st.updated != last_upd) { last_upd = st.updated; last_change = now; }
    // the engine waits inside the runtime's connect for a headset, as long as that takes: quiet then, not stuck
    bool stale = have && !g_blocking && now - last_change > 3000;
    if (memcmp(&st, &prev, sizeof st)) { prev = st; dirty = true; }
    uint32_t f = SDL_GetWindowFlags(win);
    bool visible = (f & SDL_WINDOW_SHOWN) && !(f & SDL_WINDOW_MINIMIZED);
    if (visible && (dirty || now - last_draw > 1000) && now - last_draw >= 100) {
      Draw(big, pg, st, stale);
      Downsample(big, rgba);
      SDL_UpdateTexture(tex, nullptr, rgba.data(), W * 4);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
      last_draw = now;
      dirty = false;
    }
  }
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
  Options o;
  bool window = true;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--icon") {  // the app's icon as a PNG (the AppImage's build)
      std::vector<uint8_t> icon;
      DrawIcon(icon, 256);
      return WritePng(val().c_str(), icon, 256, 256) ? 0 : 1;
    }
    if (a == "--dump") o.dump = true, window = false;
    else if (a == "--no-window") window = false;
    else if (a == "--host") {
      std::string h = val(), cur;
      for (char c : h + ",") {
        if (c == ',' || c == ' ') { if (!cur.empty()) o.hosts.push_back(cur); cur.clear(); }
        else cur += c;
      }
    } else if (a == "--headset") o.preferred = val();
    else if (a == "--runtime") o.runtime = val();
    else {
      printf("usage: %s [--host IP[,IP]] [--headset SERIAL] [--runtime manifest.json] [--no-window] [--dump] [--icon out.png]\n", argv[0]);
      return a == "--help" ? 0 : 2;
    }
  }
  g_logf = fopen((QlhsDataDir() + "/questlhsync-xr.log").c_str(), "a");
  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);
  signal(SIGPIPE, SIG_IGN);
  g_footer = L"Options: questlhsync-xr --help";
  g_vr = L"WiVRn";

  // the window and the engine: the engine's connect can wait for a headset forever, so it has a thread of its own
  std::thread eng(Engine, std::cref(o));
  if (window && Window() < 0) window = false;
  if (!window)
    while (!g_quit && !g_engine_done) usleep(100000);
  g_quit = true;
  if (g_blocking) _exit(g_rc);  // still inside the runtime's connect: nothing to clean up
  eng.join();
  return g_rc;
}
