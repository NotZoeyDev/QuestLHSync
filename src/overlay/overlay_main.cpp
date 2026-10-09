// QuestLHSync.exe (Linux: QuestLHSync): the SteamVR dashboard page. Started by the driver each session, exits with SteamVR.
// Reads the driver's status from the shared memory (qlhs_status.h), draws it at 2x (GDI on Windows, canvas_linux.h's
// software canvas on Linux) and downsamples (anti-aliased shapes and text), and sends the buttons back as commands.
//   QuestLHSync --preview out.png [locked|acquiring|still|frozen|searching|nohmd]   renders a sample page, no VR
#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#undef small  // rpcndr.h (via d3d11.h): "#define small char"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

#include <openvr.h>

#include "../common/qlhs_status.h"
#ifndef _WIN32
#include "../common/datadir_linux.h"
#include "../driver/platform.h"
#endif

static const int W = 1280, H = 800, SS = 2;  // page size, supersampling

// ---------------------------------------------------------------- canvas
#ifdef _WIN32
struct Canvas {
  int w, h;
  HDC dc;
  HBITMAP bmp;
  uint32_t *px;
  Canvas(int w_, int h_) : w(w_), h(h_) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc = CreateCompatibleDC(nullptr);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, nullptr, 0);
    SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
  }
  ~Canvas() { DeleteDC(dc); DeleteObject(bmp); }
};

static COLORREF C(uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
#else
#include "canvas_linux.h"
#endif


#ifdef _WIN32
struct Font {
  HFONT f;
  Font(int px, int weight, const wchar_t *face = L"Segoe UI") {
    f = CreateFontW(-px * SS, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
  }
  ~Font() { DeleteObject(f); }
};

struct Painter {
  Canvas &c;
  explicit Painter(Canvas &cv) : c(cv) {}
  void Rect(int x, int y, int w, int h, uint32_t color, int r = 0) {
    HBRUSH b = CreateSolidBrush(C(color));
    HPEN p = CreatePen(PS_SOLID, 1, C(color));
    HGDIOBJ ob = SelectObject(c.dc, b), op = SelectObject(c.dc, p);
    if (r) RoundRect(c.dc, x * SS, y * SS, (x + w) * SS, (y + h) * SS, r * 2 * SS, r * 2 * SS);
    else Rectangle(c.dc, x * SS, y * SS, (x + w) * SS, (y + h) * SS);
    SelectObject(c.dc, ob); SelectObject(c.dc, op);
    DeleteObject(b); DeleteObject(p);
  }
  void Outline(int x, int y, int w, int h, uint32_t color, int r, int width = 1) {
    HPEN p = CreatePen(PS_SOLID, width * SS, C(color));
    HGDIOBJ ob = SelectObject(c.dc, GetStockObject(NULL_BRUSH)), op = SelectObject(c.dc, p);
    RoundRect(c.dc, x * SS, y * SS, (x + w) * SS, (y + h) * SS, r * 2 * SS, r * 2 * SS);
    SelectObject(c.dc, ob); SelectObject(c.dc, op);
    DeleteObject(p);
  }
  void Dot(int cx, int cy, int r, uint32_t color) { Rect(cx - r, cy - r, 2 * r, 2 * r, color, r); }
  int Measure(const std::wstring &s, const Font &f) {
    HGDIOBJ of = SelectObject(c.dc, f.f);
    SIZE sz;
    GetTextExtentPoint32W(c.dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(c.dc, of);
    return sz.cx / SS;
  }
  // a line, width in page pixels
  void Line(int x1, int y1, int x2, int y2, int width, uint32_t color) {
    HPEN pen = CreatePen(PS_SOLID, width * SS, C(color));
    HGDIOBJ op = SelectObject(c.dc, pen);
    MoveToEx(c.dc, x1 * SS, y1 * SS, nullptr);
    LineTo(c.dc, x2 * SS, y2 * SS);
    SelectObject(c.dc, op);
    DeleteObject(pen);
  }
  // text; align 0 left, 1 center, 2 right (x is the anchor). returns the width
  int Text(int x, int y, const std::wstring &s, const Font &f, uint32_t color, int align = 0, int maxw = 0) {
    HGDIOBJ of = SelectObject(c.dc, f.f);
    SetTextColor(c.dc, C(color));
    SIZE sz;
    std::wstring t = s;
    GetTextExtentPoint32W(c.dc, t.c_str(), (int)t.size(), &sz);
    if (maxw > 0)
      while (sz.cx > maxw * SS && t.size() > 2) {
        t = t.substr(0, t.size() - 2) + L"…";
        GetTextExtentPoint32W(c.dc, t.c_str(), (int)t.size(), &sz);
      }
    int px = x * SS - (align == 1 ? sz.cx / 2 : align == 2 ? sz.cx : 0);
    TextOutW(c.dc, px, y * SS, t.c_str(), (int)t.size());
    SelectObject(c.dc, of);
    return sz.cx / SS;
  }
};

static std::wstring Wide(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

static std::wstring F(const wchar_t *fmt, ...) {
  wchar_t b[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(b, _countof(b), _TRUNCATE, fmt, ap);
  va_end(ap);
  return b;
}
#endif

#include "page.h"

static QlhsStatus Sample(const char *kind) {
  QlhsStatus s{};
  s.magic = QLHS_MAGIC;
  s.version = QLHS_VERSION;
  strcpy(s.hmd, "Quest Pro (CreoleCast)");
  strcpy(s.hmd_system, "CreoleCast");
  strcpy(s.headset, "Quest Pro");
  strcpy(s.headset_addr, "192.168.1.50:47280");
  s.cam_fps = 75;
  s.spot_rate = 41;
  s.sight_rate = 38;
  s.rtt_ms = 4.2;
  s.expo_ms = 15.3;
  s.expo_learned = 1;
  s.nst = 2;
  strcpy(s.st[0].serial, "LHB-1A2B3C4D");
  s.st[0].anchor = 1; s.st[0].support = 432; s.st[0].last_seen = 0.4; s.st[0].dist = 3.4;
  strcpy(s.st[1].serial, "LHB-5E6F7A8B");
  s.st[1].measured = 1; s.st[1].support = 337; s.st[1].last_seen = 6.2; s.st[1].dist = 2.1;
  s.state = QLHS_LOCKED;
  s.locked = 1; s.cond = 1;
  s.yaw_deg = -5.76; s.t[0] = -1.071; s.t[1] = 2.124; s.t[2] = 2.432;
  s.med_deg = 0.14; s.nfit = 788; s.locked_for = 754; s.lag_cm = 0.3;
  const char *logs[] = {"04:53:10  seeded from saved stations: yaw -5.765 t [-1.0713, 2.1258, 2.4309] miss 0.0 cm",
                        "04:53:12  Quest Pro at 192.168.1.50: connected",
                        "04:53:12  camera calibration: online",
                        "04:53:41  timing for creolecast: 15.3 ms (best fit 15.3 ms on 212 fast-head sightings)",
                        "04:55:02  lighthouse frame: SteamVR's is 0.75 deg from the reference"};
  for (auto l : logs) { strcpy(s.log[s.nlog % 8], l); s.nlog++; }
  if (!strcmp(kind, "still")) { s.head_still = 1; s.sight_rate = 0; s.spot_rate = 12; kind = "acquiring"; }
  if (!strcmp(kind, "frozen")) { s.head_still = 2; s.sight_rate = 0; s.spot_rate = 12; kind = "acquiring"; }
  if (!strcmp(kind, "acquiring")) { s.state = QLHS_ACQUIRING; s.locked = 0; s.med_deg = -1; s.nfit = 0; s.yaw_deg = 0; s.locked_for = -1; s.st[1].last_seen = -1; s.st[1].support = 0; s.expo_learned = 0; s.expo_ms = 15; }
  if (!strcmp(kind, "searching")) { s.state = QLHS_SEARCHING; s.headset_addr[0] = 0; s.cam_fps = 0; s.rtt_ms = 0; s.locked = 0; s.nfit = 0; s.med_deg = -1; s.yaw_deg = 0; s.locked_for = -1; }
  if (!strcmp(kind, "nohmd")) { s.state = QLHS_NO_HMD; strcpy(s.hmd, "PlayStation VR2"); }
  if (!strcmp(kind, "paused")) s.paused = 1;
  return s;
}

// ---------------------------------------------------------------- main
static QlhsStatus *g_shm;

#ifdef _WIN32
static bool ReadStatus(QlhsStatus &out) {
  if (!g_shm) {
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, QLHS_SHM_NAME);
    if (!m) return false;
    g_shm = (QlhsStatus *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QlhsStatus));
    if (!g_shm) return false;
  }
#else
static bool ReadStatus(QlhsStatus &out) {
  if (!g_shm) {  // the driver's file; not made here
    int fd = open((QlhsDataDir() + "/" QLHS_SHM_FILE).c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(QlhsStatus)) {
      void *m = mmap(nullptr, sizeof(QlhsStatus), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (m != MAP_FAILED) g_shm = (QlhsStatus *)m;
    }
    close(fd);
    if (!g_shm) return false;
  }
#endif
  for (int i = 0; i < 100; i++) {
    int32_t s = g_shm->seq;
    if (s & 1) { Sleep(0); continue; }
    MemoryBarrier();
    memcpy(&out, (const void *)g_shm, sizeof out);
    MemoryBarrier();
    if (g_shm->seq == s) return out.magic == QLHS_MAGIC && out.version == QLHS_VERSION;
  }
  return false;
}

static void SendCmd(int cmd) {
  if (!g_shm) return;
  g_shm->cmd = cmd;
  MemoryBarrier();
  InterlockedIncrement((volatile LONG *)&g_shm->cmd_seq);
}

// ---------------------------------------------------------------- desktop window: the same page, scaled to fit
static Canvas *g_page;
static Page *g_pg;
static bool g_dirty = true, g_ui = false;  // g_ui: hover/click, redraw now
static int g_down = -1;

static int HitAt(float mx, float my) {
  for (size_t i = 0; i < g_pg->buttons.size(); i++) {
    auto &b = g_pg->buttons[i];
    if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return (int)i;
  }
  return -1;
}

#ifdef _WIN32
static int WinHit(HWND h, LPARAM l) {
  RECT r;
  GetClientRect(h, &r);
  if (r.right <= 0 || r.bottom <= 0) return -1;
  return HitAt((float)(short)LOWORD(l) * W / r.right, (float)(short)HIWORD(l) * H / r.bottom);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT r;
      GetClientRect(h, &r);
      SetStretchBltMode(dc, HALFTONE);
      SetBrushOrgEx(dc, 0, 0, nullptr);
      StretchBlt(dc, 0, 0, r.right, r.bottom, g_page->dc, 0, 0, g_page->w, g_page->h, SRCCOPY);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZING: {  // keep the page's shape
      RECT wr, cr, *r = (RECT *)l;
      GetWindowRect(h, &wr);
      GetClientRect(h, &cr);
      int fw = (wr.right - wr.left) - cr.right, fh = (wr.bottom - wr.top) - cr.bottom;
      int cw = r->right - r->left - fw, chh = r->bottom - r->top - fh;
      if (w == WMSZ_TOP || w == WMSZ_BOTTOM) cw = chh * W / H;
      else chh = cw * H / W;
      if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) r->top = r->bottom - chh - fh;
      else r->bottom = r->top + chh + fh;
      if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT) r->left = r->right - cw - fw;
      else r->right = r->left + cw + fw;
      return TRUE;
    }
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
      RECT *r = (RECT *)l;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      int hit = WinHit(h, l);
      if (hit != g_pg->hover) { g_pg->hover = hit; g_ui = true; }
      SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
      return 0;
    }
    case WM_MOUSELEAVE:
      if (g_pg->hover >= 0) { g_pg->hover = -1; g_ui = true; }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) return TRUE;  // WM_MOUSEMOVE sets it
      break;
    case WM_LBUTTONDOWN: g_down = WinHit(h, l); SetCapture(h); return 0;
    case WM_LBUTTONUP: {
      ReleaseCapture();
      int hit = WinHit(h, l);
      if (hit >= 0 && hit == g_down) { SendCmd(g_pg->buttons[hit].cmd); g_ui = true; }
      g_down = -1;
      return 0;
    }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;  // the dashboard page stays; the window is back next SteamVR start
  }
  return DefWindowProcW(h, m, w, l);
}

static HICON MakeIcon(int n) {
  std::vector<uint8_t> rgba;
  DrawIcon(rgba, n);
  std::vector<uint32_t> bgra((size_t)n * n);
  for (size_t i = 0; i < bgra.size(); i++)
    bgra[i] = 0xFF000000u | (rgba[i * 4] << 16) | (rgba[i * 4 + 1] << 8) | rgba[i * 4 + 2];
  std::vector<uint8_t> mask((size_t)((n + 15) / 16) * 2 * n, 0);
  ICONINFO ii{TRUE, 0, 0, CreateBitmap(n, n, 1, 1, mask.data()), CreateBitmap(n, n, 1, 32, bgra.data())};
  HICON ic = CreateIconIndirect(&ii);
  DeleteObject(ii.hbmMask);
  DeleteObject(ii.hbmColor);
  return ic;
}

static HWND MakeWindow(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = MakeIcon(64);
  wc.hIconSm = MakeIcon(32);
  wc.lpszClassName = L"QuestLHSyncWindow";
  RegisterClassExW(&wc);
  UINT dpi = GetDpiForSystem();
  RECT r{0, 0, MulDiv(W * 3 / 4, dpi, 96), MulDiv(H * 3 / 4, dpi, 96)};
  AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
  HWND h = CreateWindowExW(0, wc.lpszClassName, L"QuestLHSync by CreoleVR", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (h) ShowWindow(h, SW_SHOWNOACTIVATE);  // SteamVR may be starting under a game: don't take the focus
  return h;
}
#endif

// ---------------------------------------------------------------- SteamVR dashboard page
#ifdef _WIN32
struct Dash {
  vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
  // the page goes up as a D3D11 texture, two of them in turn: SetOverlayRaw makes a new texture on every call and
  // the page blinks while SteamVR swaps it in
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  ID3D11Texture2D *tex[2] = {};
  int flip = 0;

  bool Start() {
    if (vr::VROverlay()->CreateDashboardOverlay("questlhsync.dashboard", "QuestLHSync", &main, &thumb) !=
        vr::VROverlayError_None)
      return false;
    vr::VROverlay()->SetOverlayWidthInMeters(main, 2.4f);
    vr::VROverlay()->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{(float)W, (float)H};
    vr::VROverlay()->SetOverlayMouseScale(main, &scale);
    std::vector<uint8_t> icon;
    DrawIcon(icon, 256);
    vr::VROverlay()->SetOverlayRaw(thumb, icon.data(), 256, 256, 4);
    int32_t adapter = -1;
    vr::VRSystem()->GetDXGIOutputInfo(&adapter);
    IDXGIFactory1 *fac = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    if (adapter >= 0 && SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) fac->EnumAdapters1(adapter, &ad);
    D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                      D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (ad) ad->Release();
    if (fac) fac->Release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (auto &t : tex)
      if (dev) dev->CreateTexture2D(&td, nullptr, &t);
    return true;
  }
  void Show(std::vector<uint8_t> &rgba) {
    if (tex[0] && tex[1]) {
      ID3D11Texture2D *t = tex[flip ^= 1];
      ctx->UpdateSubresource(t, 0, nullptr, rgba.data(), W * 4, 0);
      ctx->Flush();
      vr::Texture_t vt{t, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
      vr::VROverlay()->SetOverlayTexture(main, &vt);
    } else {
      vr::VROverlay()->SetOverlayRaw(main, rgba.data(), W, H, 4);
    }
  }
  ~Dash() {
    for (auto t : tex)
      if (t) t->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
  }
};
#else
struct Dash {
  vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;

  bool Start() {
    if (vr::VROverlay()->CreateDashboardOverlay("questlhsync.dashboard", "QuestLHSync", &main, &thumb) !=
        vr::VROverlayError_None)
      return false;
    vr::VROverlay()->SetOverlayWidthInMeters(main, 2.4f);
    vr::VROverlay()->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t scale{(float)W, (float)H};
    vr::VROverlay()->SetOverlayMouseScale(main, &scale);
    std::vector<uint8_t> icon;
    DrawIcon(icon, 256);
    vr::VROverlay()->SetOverlayRaw(thumb, icon.data(), 256, 256, 4);
    return true;
  }
  void Show(std::vector<uint8_t> &rgba) { vr::VROverlay()->SetOverlayRaw(main, rgba.data(), W, H, 4); }
};
#endif

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc >= 3 && !wcscmp(argv[1], L"--preview")) {
    char path[MAX_PATH], kind[32] = "locked";
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, path, sizeof path, nullptr, nullptr);
    if (argc >= 4) WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, kind, sizeof kind, nullptr, nullptr);
    Canvas big(W * SS, H * SS);
    Page pg;
    std::vector<uint8_t> rgba;
    if (!strcmp(kind, "icon")) {
      DrawIcon(rgba, 256);
      return WritePng(path, rgba, 256, 256) ? 0 : 1;
    }
    Draw(big, pg, Sample(kind), !strcmp(kind, "stale"));
    Downsample(big, rgba);
    return WritePng(path, rgba, W, H) ? 0 : 1;
  }

  HANDLE mutex = CreateMutexW(nullptr, TRUE, QLHS_OVERLAY_MUTEX);
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // The driver starts us while SteamVR is still coming up, maybe with no headset yet (it can connect much later).
  // We live as long as vrserver runs; the dashboard page joins once the driver reports a headset (VR_Init without
  // one is refused, and would start SteamVR if it weren't running).
  auto server_up = [] {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W pe{sizeof pe};
    bool up = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !up; ok = Process32NextW(snap, &pe))
      up = !_wcsicmp(pe.szExeFile, L"vrserver.exe");
    CloseHandle(snap);
    return up;
  };

  Canvas big(W * SS, H * SS);
  Page pg;
  g_page = &big;
  g_pg = &pg;
  QlhsStatus st{}, prev{};
  ReadStatus(st);
  Draw(big, pg, st, false);
  HWND win = MakeWindow(inst);
  Dash dash;
  bool vr_up = false, quit = false;
  std::vector<uint8_t> rgba;
  double last_upd = 0;
  DWORD last_change = GetTickCount(), last_draw = 0, next_try = 0, last_check = 0;
  int down = -1;
  while (!quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    DWORD now = GetTickCount();
    if (now - last_check >= 1000) {  // also when vrserver crashed or was killed, with no Quit sent
      last_check = now;
      if (!server_up()) {
        vr_up = false;  // nothing left to shut down
        break;
      }
    }
    bool have = ReadStatus(st);
    if (!have) memset(&st, 0, sizeof st);
    if (st.state == QLHS_WIRED) break;  // a wired headset: nothing to show, and no dashboard page to join
    if (!vr_up && have && st.hmd[0] && now >= next_try) {
      vr::EVRInitError err = vr::VRInitError_None;
      vr::VR_Init(&err, vr::VRApplication_Overlay);
      if (err == vr::VRInitError_None && dash.Start()) vr_up = true;
      else {
        if (err == vr::VRInitError_None) vr::VR_Shutdown();
        next_try = now + 5000;
      }
    }
    if (vr_up) {
      vr::VREvent_t ev;
      while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
        if (ev.eventType == vr::VREvent_Quit) {
          vr::VRSystem()->AcknowledgeQuit_Exiting();
          quit = true;
        }
      while (vr::VROverlay()->PollNextOverlayEvent(dash.main, &ev, sizeof ev)) {
        float mx = ev.data.mouse.x, my = H - ev.data.mouse.y;  // overlay mouse y is from the bottom
        switch (ev.eventType) {
          case vr::VREvent_MouseMove: {
            int h = HitAt(mx, my);
            if (h != pg.hover) { pg.hover = h; g_ui = true; }
            break;
          }
          case vr::VREvent_MouseButtonDown: down = HitAt(mx, my); break;
          case vr::VREvent_MouseButtonUp: {
            int h = HitAt(mx, my);
            if (h >= 0 && h == down) { SendCmd(pg.buttons[h].cmd); g_ui = true; }
            down = -1;
            break;
          }
          case vr::VREvent_OverlayShown: g_dirty = true; break;
        }
      }
      if (quit) break;
    }
    if (st.updated != last_upd) { last_upd = st.updated; last_change = now; }
    bool stale = !have || now - last_change > 3000;
    if (memcmp(&st, &prev, sizeof st)) { prev = st; g_dirty = true; }
    bool win_vis = win && IsWindowVisible(win) && !IsIconic(win);
    bool dash_vis = vr_up && vr::VROverlay()->IsOverlayVisible(dash.main);
    if ((win_vis || dash_vis) && (g_ui || ((g_dirty || now - last_draw > 1000) && now - last_draw >= 200))) {
      Draw(big, pg, st, stale);
      if (win_vis) InvalidateRect(win, nullptr, FALSE);
      if (dash_vis) {
        Downsample(big, rgba);
        dash.Show(rgba);
      }
      last_draw = now;
      g_dirty = g_ui = false;
    }
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
  }
  if (vr_up) vr::VR_Shutdown();
  if (win) DestroyWindow(win);
  ReleaseMutex(mutex);
  return 0;
}
#else
// The page in a desktop window, scaled to fit (letterboxed) as on Windows; closing hides it, it's back next SteamVR start
struct Desk {
  SDL_Window *win = nullptr;
  SDL_Renderer *ren = nullptr;
  SDL_Texture *tex = nullptr;
  SDL_Cursor *hand = nullptr, *arrow = nullptr;
  bool repaint = true;

  void Open() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return;  // no display: the dashboard page alone
    win = SDL_CreateWindow("QuestLHSync by CreoleVR", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W * 3 / 4,
                           H * 3 / 4, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    if (!win) return;
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (ren) tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, W, H);  // R, G, B, A bytes
    if (!tex) { Close(); return; }
    SDL_RenderSetLogicalSize(ren, W, H);
    hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    static std::vector<uint8_t> icon;
    DrawIcon(icon, 64);
    if (SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(icon.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_ABGR8888))
      SDL_SetWindowIcon(win, s), SDL_FreeSurface(s);
  }
  void Close() {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    tex = nullptr; ren = nullptr; win = nullptr;
  }
  bool Visible() const {
    if (!win) return false;
    uint32_t f = SDL_GetWindowFlags(win);
    return (f & SDL_WINDOW_SHOWN) && !(f & SDL_WINDOW_MINIMIZED);
  }
  int Hit(int x, int y) {
    float lx, ly;
    SDL_RenderWindowToLogical(ren, x, y, &lx, &ly);
    return HitAt(lx, ly);
  }
  void Poll(Page &pg) {
    if (!win) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_WINDOWEVENT:
          switch (e.window.event) {
            case SDL_WINDOWEVENT_CLOSE: SDL_HideWindow(win); break;
            case SDL_WINDOWEVENT_EXPOSED:
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_SHOWN: repaint = true; break;
            case SDL_WINDOWEVENT_LEAVE:
              if (pg.hover >= 0) { pg.hover = -1; g_ui = true; }
              break;
          }
          break;
        case SDL_MOUSEMOTION: {
          int h = Hit(e.motion.x, e.motion.y);
          if (h != pg.hover) { pg.hover = h; g_ui = true; }
          SDL_SetCursor(h >= 0 ? hand : arrow);
          break;
        }
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) g_down = Hit(e.button.x, e.button.y);
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT) {
            int h = Hit(e.button.x, e.button.y);
            if (h >= 0 && h == g_down) { SendCmd(pg.buttons[h].cmd); g_ui = true; }
            g_down = -1;
          }
          break;
      }
    }
  }
  void Present(const std::vector<uint8_t> &rgba) {
    SDL_UpdateTexture(tex, nullptr, rgba.data(), W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
    repaint = false;
  }
  ~Desk() { Close(); }
};

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "--preview")) {
    const char *kind = argc >= 4 ? argv[3] : "locked";
    Canvas big(W * SS, H * SS);
    Page pg;
    std::vector<uint8_t> rgba;
    if (!strcmp(kind, "icon")) {
      DrawIcon(rgba, 256);
      return WritePng(argv[2], rgba, 256, 256) ? 0 : 1;
    }
    Draw(big, pg, Sample(kind), !strcmp(kind, "stale"));
    Downsample(big, rgba);
    return WritePng(argv[2], rgba, W, H) ? 0 : 1;
  }
  pid_t server = 0;  // the driver passes vrserver's pid: we go when it does
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], "--server")) server = (pid_t)atoi(argv[i + 1]);
  int lock = open((QlhsDataDir() + "/" QLHS_OVERLAY_LOCK).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 0;  // one dashboard app
  signal(SIGPIPE, SIG_IGN);
  // The driver starts us while SteamVR is still coming up, maybe with no headset yet (it can connect much later).
  // The dashboard page joins once the driver reports a headset (VR_Init without one is refused).
  auto server_up = [&] { return server <= 0 || kill(server, 0) == 0 || errno == EPERM; };

  Canvas big(W * SS, H * SS);
  Page pg;
  g_page = &big;
  g_pg = &pg;
  QlhsStatus st{}, prev{};
  ReadStatus(st);
  Draw(big, pg, st, false);
  Dash dash;
  Desk desk;
  desk.Open();
  bool vr_up = false, quit = false;
  std::vector<uint8_t> rgba;
  double last_upd = 0;
  uint64_t last_change = GetTickCount64(), last_draw = 0, next_try = 0, last_check = 0;
  int down = -1;
  while (!quit) {
    uint64_t now = GetTickCount64();
    if (now - last_check >= 1000) {  // also when vrserver crashed or was killed, with no Quit sent
      last_check = now;
      if (!server_up()) {
        vr_up = false;  // nothing left to shut down
        break;
      }
    }
    bool have = ReadStatus(st);
    if (!have) memset(&st, 0, sizeof st);
    if (!vr_up && have && st.hmd[0] && now >= next_try) {
      vr::EVRInitError err = vr::VRInitError_None;
      vr::VR_Init(&err, vr::VRApplication_Overlay);
      if (err == vr::VRInitError_None && dash.Start()) vr_up = true;
      else {
        if (err == vr::VRInitError_None) vr::VR_Shutdown();
        next_try = now + 5000;
      }
    }
    if (vr_up) {
      vr::VREvent_t ev;
      while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
        if (ev.eventType == vr::VREvent_Quit) {
          vr::VRSystem()->AcknowledgeQuit_Exiting();
          quit = true;
        }
      while (vr::VROverlay()->PollNextOverlayEvent(dash.main, &ev, sizeof ev)) {
        float mx = ev.data.mouse.x, my = H - ev.data.mouse.y;  // overlay mouse y is from the bottom
        switch (ev.eventType) {
          case vr::VREvent_MouseMove: {
            int h = HitAt(mx, my);
            if (h != pg.hover) { pg.hover = h; g_ui = true; }
            break;
          }
          case vr::VREvent_MouseButtonDown: down = HitAt(mx, my); break;
          case vr::VREvent_MouseButtonUp: {
            int h = HitAt(mx, my);
            if (h >= 0 && h == down) { SendCmd(pg.buttons[h].cmd); g_ui = true; }
            down = -1;
            break;
          }
          case vr::VREvent_OverlayShown: g_dirty = true; break;
        }
      }
      if (quit) break;
    }
    desk.Poll(pg);
    if (st.updated != last_upd) { last_upd = st.updated; last_change = now; }
    bool stale = !have || now - last_change > 3000;
    if (memcmp(&st, &prev, sizeof st)) { prev = st; g_dirty = true; }
    bool dash_vis = vr_up && vr::VROverlay()->IsOverlayVisible(dash.main), win_vis = desk.Visible();
    if ((win_vis || dash_vis) && (g_ui || ((g_dirty || now - last_draw > 1000) && now - last_draw >= 200))) {
      Draw(big, pg, st, stale);
      Downsample(big, rgba);
      if (dash_vis) dash.Show(rgba);
      if (win_vis) desk.Present(rgba);
      last_draw = now;
      g_dirty = g_ui = false;
    } else if (win_vis && desk.repaint && !rgba.empty()) {
      desk.Present(rgba);
    }
    usleep(30000);
  }
  if (vr_up) vr::VR_Shutdown();
  return 0;
}
#endif
