// The dashboard page (and the PNG writer for the app's --preview and --icon), shared by the SteamVR dashboard app (overlay_main.cpp) and the WiVRn/Monado app (xr_main.cpp):
// the status text, the page drawn on a Canvas (2x, see Downsample), the dashboard thumbnail. Needs Canvas, Font, Painter,
// Wide, F, col, W, H and SS from the including file, and qlhs_status.h.
#pragma once
#include <cstdio>

namespace col {
const uint32_t bg = 0x14161b, card = 0x1d2027, card2 = 0x252932, line = 0x30343e, text = 0xeceef2, dim = 0x9aa1ad,
               faint = 0x6b7280, green = 0x3ccf6e, amber = 0xffb224, red = 0xff5f57, blue = 0x5b8cff, grey = 0x8e93a0;
}

// the line at the bottom right (what the page is configured by)
// what the app runs under, for the page's text
static std::wstring g_vr = L"SteamVR";
static std::wstring g_footer = L"Settings: steamvr.vrsettings → driver_questlhsync";

static std::wstring Ago(double s) {
  if (s < 0) return L"not yet";
  if (s < 1.5) return L"now";
  if (s < 90) return F(L"%.0f s ago", s);
  if (s < 5400) return F(L"%.0f min ago", s / 60);
  return F(L"%.1f h ago", s / 3600);
}

static std::wstring Dur(double s) {
  if (s < 90) return F(L"%.0f s", s);
  if (s < 5400) return F(L"%.0f min", s / 60);
  return F(L"%.1f h", s / 3600);
}

// ---------------------------------------------------------------- page
struct Button { int x, y, w, h, cmd; std::wstring label; bool primary; };

struct Page {
  std::vector<Button> buttons;
  int hover = -1;
};

static void StateText(const QlhsStatus &s, bool stale, std::wstring &title, std::wstring &sub, uint32_t &color) {
  if (stale) { title = L"Driver not running"; sub = g_vr == L"SteamVR" ? L"The QuestLHSync driver isn't updating. Restart SteamVR." : L"QuestLHSync isn't updating. Restart it."; color = col::red; return; }
  switch (s.state) {
    case QLHS_NO_HMD:
      title = L"Waiting for a Quest or Steam Frame";
      sub = s.hmd[0] ? L"SteamVR's headset isn't a Quest Pro, 3, 3S or Steam Frame, so the lighthouse space is left alone."
                     : L"Start your streaming app (Steam Link, Link, Air Link, Virtual Desktop, ALVR, WiVRn, ...).";
      color = col::grey;
      break;
    case QLHS_SEARCHING:
      title = L"Looking for the headset";
      sub = L"No headset with QuestLHSync's lhsyncd answers on the network. Is it awake and on the same Wi-Fi?";
      color = col::amber;
      break;
    case QLHS_CONNECTING: title = L"Connecting"; sub = L"Found the headset, opening the camera stream."; color = col::amber; break;
    case QLHS_NO_CAMERAS:
      title = L"No camera frames";
      sub = L"Connected, but the tracking cameras send nothing. Put the headset on (or wake it) and check lhsyncd.";
      color = col::red;
      break;
    case QLHS_NO_STATIONS:
      title = L"Waiting for base stations";
      sub = g_vr == L"SteamVR" ? L"SteamVR shows base stations only while a lighthouse device (tracker, controller) is on. Turn one on." : L"The base stations come from SteamVR's lighthousedb.json, filled in once SteamVR has seen them with a tracker or controller on.";
      color = col::amber;
      break;
    case QLHS_ACQUIRING:
      title = L"Finding the base stations";
      sub = s.head_still == 2 ? L"SteamVR isn't getting your head's motion. Is SteamVR's view in the headset, not a desktop view?"
            : s.head_still   ? L"The headset isn't moving, so its camera frames wait. Put it on and look around the room."
                             : L"Look around the room so the cameras catch both base stations' flashes.";
      color = col::amber;
      break;
    case QLHS_LOCKED:
      title = s.paused ? L"Locked, corrections paused" : L"Locked";
      sub = s.paused ? L"Lighthouse devices stay where they are until you resume."
                     : L"Lighthouse devices follow the headset's own tracking.";
      color = s.paused ? col::amber : col::green;
      break;
    default: title = L"Starting"; sub = L""; color = col::grey;
  }
}

static void Draw(Canvas &cv, Page &pg, const QlhsStatus &s, bool stale) {
  Painter p(cv);
  Font h1(40, FW_SEMIBOLD, L"Segoe UI Semibold"), h2(22, FW_SEMIBOLD, L"Segoe UI Semibold"), body(21, FW_NORMAL),
      small(17, FW_NORMAL), label(16, FW_SEMIBOLD, L"Segoe UI Semibold"), big(28, FW_SEMIBOLD, L"Segoe UI Semibold"),
      mono(17, FW_NORMAL, L"Cascadia Mono");
  p.Rect(0, 0, W, H, col::bg);

  // header
  int tw = p.Text(48, 34, L"QuestLHSync", h1, col::text);
  p.Text(48 + tw + 14, 50, L"by CreoleVR", h2, col::faint);
  p.Text(50, 88, L"Lighthouse tracking aligned to your headset, from its own cameras", small, col::dim);
  std::wstring title, sub;
  uint32_t sc;
  StateText(s, stale, title, sub, sc);
  {
    Font pill(22, FW_SEMIBOLD, L"Segoe UI Semibold");
    int tw = p.Measure(title, pill), pw = tw + 64, px = W - 48 - pw;
    p.Rect(px, 40, pw, 48, col::card2, 24);
    p.Dot(px + 26, 64, 7, sc);
    p.Text(px + 44, 50, title, pill, col::text);
  }

  // status line
  p.Rect(48, 124, W - 96, 52, col::card, 12);
  p.Rect(48, 124, 6, 52, sc, 3);
  p.Text(70, 137, sub, body, col::text, 0, W - 140);

  // cards
  int top = 196, ch = 300, gap = 20, cw = (W - 96 - 2 * gap) / 3;
  int x1 = 48, x2 = x1 + cw + gap, x3 = x2 + cw + gap;
  for (int x : {x1, x2, x3}) p.Rect(x, top, cw, ch, col::card, 14);

  auto row = [&](int x, int y, const wchar_t *k, const std::wstring &v, uint32_t vc = col::text) {
    p.Text(x + 22, y, k, small, col::dim);
    p.Text(x + cw - 22, y, v, small, vc, 2, cw - 150);
  };

  // headset card
  p.Text(x1 + 22, top + 18, L"HEADSET", label, col::faint);
  p.Text(x1 + 22, top + 44, s.headset[0] ? Wide(s.headset) : L"—", big, col::text, 0, cw - 44);
  row(x1, top + 100, L"Address", s.headset_addr[0] ? Wide(s.headset_addr) : L"—");
  row(x1, top + 134, L"Cameras", s.cam_fps > 0.5 ? F(L"%.0f frames/s", s.cam_fps) : L"—",
      s.cam_fps > 0.5 || s.state < QLHS_NO_CAMERAS ? col::text : col::red);
  row(x1, top + 168, L"Round trip", s.rtt_ms > 0 ? F(L"%.1f ms", s.rtt_ms) : L"—");
  row(x1, top + 202, L"Streamer", s.hmd_system[0] ? Wide(s.hmd_system) : L"—");
  row(x1, top + 236, L"Pose timing",
      s.headset_addr[0] ? F(L"%.1f ms %s", s.expo_ms, s.expo_learned ? L"(learned)" : L"(learning)") : L"—",
      s.expo_learned || !s.headset_addr[0] ? col::text : col::amber);
  row(x1, top + 270, L"Bright spots", s.cam_fps > 0.5 ? F(L"%.0f/s, %.0f/s used", s.spot_rate, s.sight_rate) : L"—",
      s.state == QLHS_ACQUIRING && s.sight_rate < 0.5 ? col::amber : col::text);

  // base stations card
  bool has = s.locked || s.nfit > 0 || s.yaw_deg != 0;  // an alignment: sightings belong to a station
  p.Text(x2 + 22, top + 18, L"BASE STATIONS", label, col::faint);
  if (s.nst == 0) {
    p.Text(x2 + 22, top + 52, L"None in " + g_vr + L" yet", body, col::dim);
    p.Text(x2 + 22, top + 84, L"They show up while a tracker or", small, col::faint);
    p.Text(x2 + 22, top + 108, L"controller is switched on.", small, col::faint);
  }
  for (int i = 0; i < std::min(s.nst, 4); i++) {
    const QlhsStation &st = s.st[i];
    int y = top + 50 + i * 60;
    bool fresh = st.last_seen >= 0 && st.last_seen < 15;
    p.Dot(x2 + 30, y + 15, 6, fresh ? col::green : st.last_seen >= 0 ? col::amber : col::faint);
    p.Text(x2 + 46, y, Wide(st.serial), body, col::text, 0, cw - 150);
    std::wstring role = st.anchor ? L"anchor" : st.measured ? L"measured" : L"";
    if (!role.empty()) p.Text(x2 + cw - 22, y + 3, role, small, col::blue, 2);
    std::wstring info = L"seen " + Ago(st.last_seen);
    if (has) info += F(L"  ·  %d sightings", st.support);
    if (st.dist > 0) info += F(L"  ·  %.1f m", st.dist);
    p.Text(x2 + 46, y + 28, info, small, col::dim, 0, cw - 68);
  }

  // alignment card
  p.Text(x3 + 22, top + 18, L"ALIGNMENT", label, col::faint);
  p.Text(x3 + 22, top + 44, has ? F(L"%.2f°", s.med_deg >= 0 ? s.med_deg : 0.0) : L"—", big,
         s.med_deg >= 0 && s.med_deg < 0.3 ? col::green : col::text);
  if (has) p.Text(x3 + 22 + 110, top + 56, L"median sighting error", small, col::dim);
  row(x3, top + 100, L"Yaw", has ? F(L"%+.2f°", s.yaw_deg) : L"—");
  row(x3, top + 134, L"Offset", has ? F(L"%.2f, %.2f, %.2f m", s.t[0], s.t[1], s.t[2]) : L"—");
  row(x3, top + 168, L"Sightings used", has ? F(L"%d", s.nfit) : L"—");
  row(x3, top + 202, L"Since acquired", s.locked_for >= 0 ? Dur(s.locked_for) : L"—");
  row(x3, top + 236, L"Settling", s.lag_cm > 0.05 ? F(L"%.1f cm to go", s.lag_cm) : has ? L"done" : L"—");

  // log
  int ly = top + ch + 20, lh = 150;
  p.Rect(48, ly, W - 96, lh, col::card, 14);
  p.Text(70, ly + 16, L"LOG", label, col::faint);
  uint32_t n = s.nlog;
  int shown = 0;
  for (uint32_t i = 0; i < 5 && i < n; i++) {
    uint32_t k = n - 1 - i;
    p.Text(70, ly + 44 + 20 * i, Wide(s.log[k % 8]), mono, i == 0 ? col::text : col::dim, 0, W - 150);
    shown++;
  }
  if (!shown) p.Text(70, ly + 44, L"—", mono, col::dim);

  // buttons
  pg.buttons.clear();
  int by = H - 66, bh = 46;
  pg.buttons.push_back({48, by, 240, bh, s.paused ? QLHS_CMD_RESUME : QLHS_CMD_PAUSE,
                        s.paused ? L"Resume corrections" : L"Pause corrections", s.paused != 0});
  pg.buttons.push_back({304, by, 210, bh, s.recording ? QLHS_CMD_RECORD_OFF : QLHS_CMD_RECORD_ON,
                        s.recording ? L"Stop recording" : L"Record session", false});
  for (size_t i = 0; i < pg.buttons.size(); i++) {
    auto &b = pg.buttons[i];
    bool hov = (int)i == pg.hover;
    uint32_t fill = b.primary ? col::blue : hov ? col::line : col::card2;
    p.Rect(b.x, b.y, b.w, b.h, fill, 10);
    if (b.cmd == QLHS_CMD_RECORD_OFF) p.Dot(b.x + 26, b.y + bh / 2, 6, col::red);
    p.Text(b.x + b.w / 2 + (b.cmd == QLHS_CMD_RECORD_OFF ? 10 : 0), b.y + 11, b.label, h2, col::text, 1);
  }
  p.Text(W - 48, by + 14, g_footer, small, col::faint, 2);
}

// 2x2 box filter + BGRX -> RGBA (alpha 255)
static void Downsample(const Canvas &big, std::vector<uint8_t> &rgba) {
  rgba.resize((size_t)W * H * 4);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      unsigned r = 0, g = 0, b = 0;
      for (int dy = 0; dy < SS; dy++)
        for (int dx = 0; dx < SS; dx++) {
          uint32_t v = big.px[(size_t)(y * SS + dy) * big.w + x * SS + dx];
          r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255;
        }
      uint8_t *o = &rgba[((size_t)y * W + x) * 4];
      o[0] = (uint8_t)(r / (SS * SS)); o[1] = (uint8_t)(g / (SS * SS)); o[2] = (uint8_t)(b / (SS * SS)); o[3] = 255;
    }
}

// dashboard thumbnail: a base station's laser fan reaching a headset
static void DrawIcon(std::vector<uint8_t> &rgba, int N) {
  Canvas cv(N * SS, N * SS);
  Painter p(cv);
  auto S = [&](int v) { return v * N / 256; };
  p.Rect(0, 0, N, N, col::bg);
  p.Rect(S(16), S(16), S(224), S(224), col::card2, S(48));
  p.Rect(S(54), S(66), S(70), S(70), col::text, S(18));      // base station
  p.Dot(S(89), S(101), S(20), col::bg);
  p.Dot(S(89), S(101), S(10), col::blue);
  for (int i = 0; i < 3; i++)  // laser lines
    p.Line(S(118), S(112 + i * 14), S(170), S(140 + i * 18), S(8), col::blue);
  p.Rect(S(140), S(150), S(86), S(52), col::green, S(20));   // headset visor
  p.Dot(S(166), S(176), S(9), col::bg);
  p.Dot(S(200), S(176), S(9), col::bg);
  rgba.resize((size_t)N * N * 4);
  for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
      unsigned r = 0, g = 0, b = 0;
      for (int dy = 0; dy < SS; dy++)
        for (int dx = 0; dx < SS; dx++) {
          uint32_t v = cv.px[(size_t)(y * SS + dy) * cv.w + x * SS + dx];
          r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255;
        }
      uint8_t *o = &rgba[((size_t)y * N + x) * 4];
      o[0] = (uint8_t)(r / 4); o[1] = (uint8_t)(g / 4); o[2] = (uint8_t)(b / 4); o[3] = 255;
    }
}

// ---------------------------------------------------------------- PNG (stored deflate) for --preview
static uint32_t Crc(const uint8_t *d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}

static bool WritePng(const char *path, const std::vector<uint8_t> &rgba, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba.begin() + (size_t)y * w * 4, rgba.begin() + (size_t)(y + 1) * w * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t o = 0; o < raw.size(); o += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - o);
    z.push_back((uint8_t)(o + n == raw.size()));
    z.push_back((uint8_t)(n & 255)); z.push_back((uint8_t)(n >> 8)); z.push_back((uint8_t)(~n & 255)); z.push_back((uint8_t)((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  uint32_t ad = (b << 16) | a;
  for (int i = 3; i >= 0; i--) z.push_back((ad >> (8 * i)) & 255);
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  auto be = [&](uint32_t v) { uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; fwrite(q, 1, 4, f); };
  auto chunk = [&](const char *t, const std::vector<uint8_t> &d) {
    be((uint32_t)d.size());
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    fwrite(td.data(), 1, td.size(), f);
    be(Crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
  };
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ih = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                             (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
  chunk("IHDR", ih);
  chunk("IDAT", z);
  chunk("IEND", {});
  fclose(f);
  return true;
}
