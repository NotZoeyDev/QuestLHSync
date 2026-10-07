// The dashboard page's canvas on Linux: what overlay_main.cpp gets from GDI on Windows (Canvas, Font, Painter, Wide, F),
// drawn in software. Shapes are anti-aliased through distance fields, text is rasterised by stb_truetype from a system
// font (Liberation, DejaVu or Noto Sans; found at run time, since the container SteamVR runs in has its own fonts).
// Included once, from overlay_main.cpp, after SS is defined.
#pragma once
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

enum { FW_NORMAL = 400, FW_SEMIBOLD = 600 };

struct Canvas {
  int w, h;
  std::vector<uint32_t> buf;  // 0x00RRGGBB
  uint32_t *px;
  Canvas(int w_, int h_) : w(w_), h(h_), buf((size_t)w_ * h_), px(buf.data()) {}
};

namespace fonts {
struct Face {
  std::vector<unsigned char> data;
  stbtt_fontinfo info;
  int id;
};

// every .ttf under the usual font folders (the container's own, and the host's as pressure-vessel mounts it)
inline const std::map<std::string, std::string> &Files() {
  static std::map<std::string, std::string> files = [] {
    std::map<std::string, std::string> m;
    std::vector<std::filesystem::path> dirs = {"/usr/share/fonts", "/usr/local/share/fonts", "/run/host/usr/share/fonts",
                                               "/run/host/fonts", "/run/host/usr/local/share/fonts"};
    if (const char *f = getenv("QLHS_FONT_DIR")) dirs.insert(dirs.begin(), f);  // fonts that travel with the program
    if (const char *h = getenv("HOME")) {
      dirs.push_back(std::filesystem::path(h) / ".local/share/fonts");
      dirs.push_back(std::filesystem::path(h) / ".fonts");
    }
    for (auto &d : dirs) {
      std::error_code ec;
      for (std::filesystem::recursive_directory_iterator it(d, std::filesystem::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".ttf") m.emplace(it->path().filename().string(), it->path().string());
    }
    return m;
  }();
  return files;
}

inline Face *Load(std::initializer_list<const char *> names) {
  static std::map<std::string, std::unique_ptr<Face>> cache;
  static int next = 1;
  for (const char *n : names) {
    auto c = cache.find(n);
    if (c != cache.end()) return c->second.get();
    auto f = Files().find(n);
    if (f == Files().end()) continue;
    FILE *fp = fopen(f->second.c_str(), "rb");
    if (!fp) continue;
    auto face = std::make_unique<Face>();
    unsigned char b[65536];
    size_t k;
    while ((k = fread(b, 1, sizeof b, fp)) > 0) face->data.insert(face->data.end(), b, b + k);
    fclose(fp);
    if (!stbtt_InitFont(&face->info, face->data.data(), 0)) continue;
    face->id = next++;
    Face *p = face.get();
    cache[n] = std::move(face);
    return p;
  }
  return nullptr;
}

struct Glyph {
  int x0 = 0, y0 = 0, w = 0, h = 0, adv = 0;
  std::vector<uint8_t> a;
};
}  // namespace fonts

struct Font {
  fonts::Face *face = nullptr;
  int px;
  float scale = 0, ascent = 0;
  // face names are Windows' (Segoe UI, Cascadia Mono): only mono and the weight carry over
  Font(int px_, int weight, const wchar_t *name = L"Segoe UI") : px(px_ * SS) {
    bool mono = wcsstr(name, L"Mono") != nullptr, bold = weight >= 600;
    if (mono) face = fonts::Load({"LiberationMono-Regular.ttf", "DejaVuSansMono.ttf", "NotoSansMono-Regular.ttf"});
    else if (bold) face = fonts::Load({"LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSans-Bold.ttf"});
    else face = fonts::Load({"LiberationSans-Regular.ttf", "DejaVuSans.ttf", "NotoSans-Regular.ttf"});
    if (!face) return;
    scale = stbtt_ScaleForMappingEmToPixels(&face->info, (float)px);
    int a, d, g;
    stbtt_GetFontVMetrics(&face->info, &a, &d, &g);
    ascent = a * scale;
  }
  const fonts::Glyph &Get(wchar_t ch) const {
    static std::map<uint64_t, fonts::Glyph> cache;
    uint64_t key = (uint64_t)face->id << 48 | (uint64_t)px << 32 | (uint32_t)ch;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    fonts::Glyph g;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&face->info, ch, &adv, &lsb);
    g.adv = (int)lroundf(adv * scale);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&face->info, ch, scale, scale, &x0, &y0, &x1, &y1);
    g.x0 = x0; g.y0 = y0; g.w = x1 - x0; g.h = y1 - y0;
    if (g.w > 0 && g.h > 0) {
      g.a.resize((size_t)g.w * g.h);
      stbtt_MakeCodepointBitmap(&face->info, g.a.data(), g.w, g.h, g.w, scale, scale, ch);
    }
    return cache.emplace(key, std::move(g)).first->second;
  }
  int Width(const std::wstring &s) const {
    int w = 0;
    if (face)
      for (wchar_t ch : s) w += Get(ch).adv;
    return w;
  }
};

struct Painter {
  Canvas &c;
  explicit Painter(Canvas &cv) : c(cv) {}

  void Blend(int x, int y, uint32_t color, float a) {
    if (a <= 0 || x < 0 || y < 0 || x >= c.w || y >= c.h) return;
    if (a > 1) a = 1;
    uint32_t &d = c.px[(size_t)y * c.w + x];
    int r = (int)((d >> 16) & 255), g = (int)((d >> 8) & 255), b = (int)(d & 255);
    r += (int)lroundf(((int)((color >> 16) & 255) - r) * a);
    g += (int)lroundf(((int)((color >> 8) & 255) - g) * a);
    b += (int)lroundf(((int)(color & 255) - b) * a);
    d = (uint32_t)(r << 16 | g << 8 | b);
  }

  // coverage of every pixel near a shape from its signed distance (negative inside), at pixel centres
  template <class Dist>
  void Shape(float x0, float y0, float x1, float y1, uint32_t color, Dist dist) {
    for (int y = std::max(0, (int)floorf(y0) - 1); y <= std::min(c.h - 1, (int)ceilf(y1) + 1); y++)
      for (int x = std::max(0, (int)floorf(x0) - 1); x <= std::min(c.w - 1, (int)ceilf(x1) + 1); x++)
        Blend(x, y, color, 0.5f - dist(x + 0.5f, y + 0.5f));
  }

  static float RoundBox(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
  }

  void Rect(int x, int y, int w, int h, uint32_t color, int r = 0) {
    float X = (float)x * SS, Y = (float)y * SS, Wd = (float)w * SS, Ht = (float)h * SS;
    float rr = std::min((float)r * SS, std::min(Wd, Ht) / 2), cx = X + Wd / 2, cy = Y + Ht / 2;
    Shape(X, Y, X + Wd, Y + Ht, color, [&](float px, float py) { return RoundBox(px, py, cx, cy, Wd / 2, Ht / 2, rr); });
  }
  void Outline(int x, int y, int w, int h, uint32_t color, int r, int width = 1) {
    float X = (float)x * SS, Y = (float)y * SS, Wd = (float)w * SS, Ht = (float)h * SS, hw = width * SS / 2.f;
    float rr = std::min((float)r * SS, std::min(Wd, Ht) / 2), cx = X + Wd / 2, cy = Y + Ht / 2;
    Shape(X - hw, Y - hw, X + Wd + hw, Y + Ht + hw, color,
          [&](float px, float py) { return fabsf(RoundBox(px, py, cx, cy, Wd / 2, Ht / 2, rr)) - hw; });
  }
  void Dot(int cx, int cy, int r, uint32_t color) { Rect(cx - r, cy - r, 2 * r, 2 * r, color, r); }
  // a line with round ends, width in page pixels
  void Line(int x1, int y1, int x2, int y2, int width, uint32_t color) {
    float ax = (float)x1 * SS, ay = (float)y1 * SS, bx = (float)x2 * SS, by = (float)y2 * SS, hw = width * SS / 2.f;
    Shape(std::min(ax, bx) - hw, std::min(ay, by) - hw, std::max(ax, bx) + hw, std::max(ay, by) + hw, color,
          [&](float px, float py) {
            float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
            t = std::min(1.f, std::max(0.f, t));
            return std::hypot(px - ax - t * dx, py - ay - t * dy) - hw;
          });
  }
  int Measure(const std::wstring &s, const Font &f) { return f.Width(s) / SS; }
  // text; align 0 left, 1 center, 2 right (x is the anchor). returns the width
  int Text(int x, int y, const std::wstring &s, const Font &f, uint32_t color, int align = 0, int maxw = 0) {
    if (!f.face) return 0;
    std::wstring t = s;
    int w = f.Width(t);
    if (maxw > 0)
      while (w > maxw * SS && t.size() > 2) {
        t = t.substr(0, t.size() - 2) + L"…";
        w = f.Width(t);
      }
    int pen = x * SS - (align == 1 ? w / 2 : align == 2 ? w : 0), base = (int)lroundf(y * SS + f.ascent);
    for (wchar_t ch : t) {
      const fonts::Glyph &g = f.Get(ch);
      for (int gy = 0; gy < g.h; gy++)
        for (int gx = 0; gx < g.w; gx++)
          Blend(pen + g.x0 + gx, base + g.y0 + gy, color, g.a[(size_t)gy * g.w + gx] / 255.f);
      pen += g.adv;
    }
    return w / SS;
  }
};

// UTF-8 -> wchar_t (UTF-32)
static std::wstring Wide(const char *s) {
  std::wstring w;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t cp = *p;
    int n = cp < 0x80 ? 0 : cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (n) cp &= 0x3F >> n;
    p++;
    while (n-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
    w += (wchar_t)cp;
  }
  return w;
}

// Windows' wide printf takes %s for wide strings; glibc's wants %ls
static std::wstring F(const wchar_t *fmt, ...) {
  std::wstring f = fmt;
  for (size_t i = 0; (i = f.find(L"%s", i)) != std::wstring::npos; i += 3) f.replace(i, 2, L"%ls");
  wchar_t b[512];
  va_list ap;
  va_start(ap, fmt);
  vswprintf(b, sizeof b / sizeof *b, f.c_str(), ap);
  va_end(ap);
  return b;
}
