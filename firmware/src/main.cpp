// T-Display-S3 USB display
// ------------------------------------------------------------------------------------
// A LilyGO T-Display-S3 (ESP32-S3, 1.9" ST7789 170x320, 8-bit parallel) that listens on
// its USB port (CDC serial) for one command per line and draws it: a header with title,
// status pill and link heartbeat; a dashboard of up to 6 tiles (big number, bar, arc
// gauge, sparkline, key/value, LED); a scrolling log; a ticker; and an alert overlay.
// Any line that isn't a command is printed to the log. Send HELP for the command list.
//
// Buttons: GPIO14 (KEY) cycles pages (auto / dashboard / log) and dismisses an alert;
//          GPIO0 (BOOT) toggles the backlight between bright and dim.
// ------------------------------------------------------------------------------------
#include <Arduino.h>
#include <esp_task_wdt.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const char *FW_VERSION = "1.0.0";
static const int WATCHDOG_SECONDS = 8;  // a stalled loop reboots the display instead of freezing it

// ---------------------------------------------------------------- display driver
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.freq_write = 20000000;
      cfg.pin_wr = 8;
      cfg.pin_rd = 9;
      cfg.pin_rs = 7;
      cfg.pin_d0 = 39; cfg.pin_d1 = 40; cfg.pin_d2 = 41; cfg.pin_d3 = 42;
      cfg.pin_d4 = 45; cfg.pin_d5 = 46; cfg.pin_d6 = 47; cfg.pin_d7 = 48;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 6;
      cfg.pin_rst = 5;
      cfg.pin_busy = -1;
      cfg.panel_width = 170;
      cfg.panel_height = 320;
      cfg.offset_x = 35;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 38;
      cfg.invert = false;
      cfg.freq = 22000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

static LGFX tft;
static LGFX_Sprite fb(&tft);  // full-screen frame buffer: flicker-free redraws

static const int PIN_POWER_ON = 15;  // LCD power enable (needed when running on battery)
static const int PIN_KEY_BOOT = 0;
static const int PIN_KEY_2 = 14;

// ---------------------------------------------------------------- palette (RGB888)
static const uint32_t C_BG = 0x060A0F;
static const uint32_t C_PANEL = 0x0D1520;
static const uint32_t C_EDGE = 0x1A2836;
static const uint32_t C_TEXT = 0xDDE7EE;
static const uint32_t C_DIM = 0x6B7C8C;
static const uint32_t C_ACCENT = 0x38D6FF;
static const uint32_t C_OK = 0x4BE38E;
static const uint32_t C_WARN = 0xFFC24B;
static const uint32_t C_CRIT = 0xFF5468;
static const uint32_t C_INFO = 0x8AB4FF;

enum Level : uint8_t { L_INFO = 0, L_OK, L_WARN, L_CRIT, L_DIM };

static uint32_t levelColor(uint8_t l) {
  switch (l) {
    case L_OK: return C_OK;
    case L_WARN: return C_WARN;
    case L_CRIT: return C_CRIT;
    case L_DIM: return C_DIM;
    default: return C_ACCENT;
  }
}

static uint8_t parseLevel(const String &s, uint8_t dflt = L_INFO) {
  String t = s;
  t.trim();
  t.toLowerCase();
  if (t == "ok" || t == "good" || t == "green") return L_OK;
  if (t == "warn" || t == "warning" || t == "yellow" || t == "amber") return L_WARN;
  if (t == "crit" || t == "critical" || t == "err" || t == "error" || t == "alarm" || t == "red") return L_CRIT;
  if (t == "off" || t == "dim" || t == "idle" || t == "grey" || t == "gray") return L_DIM;
  if (t == "info" || t == "blue" || t == "cyan") return L_INFO;
  return dflt;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {  // a -> b
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  uint8_t r = ((a >> 16) & 255) + (int)((((int)((b >> 16) & 255)) - (int)((a >> 16) & 255)) * t);
  uint8_t g = ((a >> 8) & 255) + (int)((((int)((b >> 8) & 255)) - (int)((a >> 8) & 255)) * t);
  uint8_t bl = (a & 255) + (int)((((int)(b & 255)) - (int)(a & 255)) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

// ---------------------------------------------------------------- state
enum WType : uint8_t { W_NONE = 0, W_BIG, W_BAR, W_GAUGE, W_SPARK, W_KV, W_LED };

struct Widget {
  WType type = W_NONE;
  char label[24] = "";
  char text[40] = "";  // value text / bar caption / kv value
  char unit[12] = "";
  float v = 0, vmin = 0, vmax = 100, shown = 0;
  float series[48];
  uint8_t n = 0;
  uint8_t level = L_INFO;
  uint32_t updated = 0;
};

static const int NTILES = 6;
static Widget tiles[NTILES];

static const int LOG_LINES = 40;
struct LogLine {
  char text[64];
  uint8_t level;
};
static LogLine logbuf[LOG_LINES];
static int logHead = 0, logCount = 0;

static char title[40] = "USB DISPLAY";
static char statusText[24] = "";
static uint8_t statusLevel = L_INFO;
static char ticker[160] = "";
static float tickerX = 320;

static bool alertOn = false;
static char alertText[120] = "";
static uint8_t alertLevel = L_CRIT;
static uint32_t alertUntil = 0;

enum Page : uint8_t { P_AUTO = 0, P_DASH, P_LOG };
static Page page = P_AUTO;
static uint8_t bright = 200;
static bool dim = false;
static uint32_t lastRx = 0;
static uint32_t rxLines = 0;
static uint32_t bootMs = 0;

// ---------------------------------------------------------------- helpers
static void copyStr(char *dst, size_t n, const String &s) {
  size_t i = 0;
  for (; i + 1 < n && i < s.length(); i++) {
    char c = s[i];
    dst[i] = (c >= 32 && c < 127) ? c : ' ';
  }
  dst[i] = 0;
}

static int splitFields(const String &s, String *out, int maxn) {
  int n = 0, start = 0;
  while (n < maxn) {
    int bar = s.indexOf('|', start);
    if (bar < 0 || n == maxn - 1) {
      out[n++] = s.substring(start);
      break;
    }
    out[n++] = s.substring(start, bar);
    start = bar + 1;
  }
  for (int i = 0; i < n; i++) out[i].trim();
  return n;
}

static Widget *tileFor(const String &label, WType type) {
  String key = label;
  key.trim();
  Widget *empty = nullptr, *oldest = &tiles[0];
  for (auto &w : tiles) {
    if (w.type != W_NONE && key.equalsIgnoreCase(w.label)) {
      if (w.type != type) {  // a label changing type starts fresh
        w = Widget();
        copyStr(w.label, sizeof(w.label), key);
        w.type = type;
      }
      return &w;
    }
    if (w.type == W_NONE && !empty) empty = &w;
    if (w.updated < oldest->updated) oldest = &w;
  }
  Widget *w = empty ? empty : oldest;  // full: replace the least recently updated
  *w = Widget();
  copyStr(w->label, sizeof(w->label), key);
  w->type = type;
  return w;
}

static int tileCount() {
  int n = 0;
  for (auto &w : tiles) n += w.type != W_NONE;
  return n;
}

static void logAdd(const String &line, uint8_t level) {
  // wrap on word boundaries by measured width, in the log's font
  fb.setFont(&fonts::DejaVu12);
  const int maxw = 300;
  String rest = line;
  do {
    String part = rest;
    if (fb.textWidth(part.c_str()) > maxw) {
      int cut = part.length();
      while (cut > 1 && fb.textWidth(part.substring(0, cut).c_str()) > maxw) cut--;
      int sp = part.lastIndexOf(' ', cut);
      if (sp > cut / 2) cut = sp;
      part = rest.substring(0, cut);
      rest = rest.substring(cut);
      rest.trim();
    } else {
      rest = "";
    }
    LogLine &l = logbuf[logHead];
    copyStr(l.text, sizeof(l.text), part);
    l.level = level;
    logHead = (logHead + 1) % LOG_LINES;
    if (logCount < LOG_LINES) logCount++;
  } while (rest.length());
}

static bool dashboardVisible() {
  if (page == P_DASH) return true;
  if (page == P_LOG) return false;
  return tileCount() > 0;  // auto: dashboard once there are tiles, else the log
}

// ---------------------------------------------------------------- drawing
static void drawPanel(int x, int y, int w, int h, uint32_t edge) {
  fb.fillRoundRect(x, y, w, h, 6, C_PANEL);
  fb.drawRoundRect(x, y, w, h, 6, edge);
}

static void drawHeader(uint32_t now) {
  fb.fillRect(0, 0, 320, 22, C_BG);
  fb.drawFastHLine(0, 22, 320, C_EDGE);
  // corner bracket accent
  fb.fillRect(0, 0, 3, 22, C_ACCENT);
  fb.setFont(&fonts::FreeSansBold9pt7b);
  fb.setTextDatum(textdatum_t::middle_left);
  fb.setTextColor(C_TEXT);
  fb.drawString(title, 9, 11);
  // link heartbeat: bright and pulsing while data arrives, grey after 60 s of silence
  bool live = lastRx && now - lastRx < 60000;
  float pulse = live ? 0.5f + 0.5f * sinf(now / 180.0f) : 0;
  int hx = 320 - 10, hy = 11;
  if (statusText[0]) {
    fb.setFont(&fonts::DejaVu12);
    int tw = fb.textWidth(statusText) + 14;
    uint32_t c = levelColor(statusLevel);
    int px = 320 - 22 - tw;
    fb.fillRoundRect(px, 3, tw, 16, 8, mix(C_BG, c, 0.22f));
    fb.drawRoundRect(px, 3, tw, 16, 8, c);
    fb.setTextDatum(textdatum_t::middle_center);
    fb.setTextColor(c);
    fb.drawString(statusText, px + tw / 2, 11);
  }
  uint32_t hc = live ? mix(C_PANEL, C_OK, 0.45f + 0.55f * pulse) : C_EDGE;
  fb.fillCircle(hx, hy, 6, mix(C_BG, hc, 0.35f));
  fb.fillCircle(hx, hy, 3, hc);
}

static void drawTicker(uint32_t now) {
  if (!ticker[0]) return;
  fb.fillRect(0, 153, 320, 17, C_BG);
  fb.drawFastHLine(0, 153, 320, C_EDGE);
  fb.setFont(&fonts::DejaVu12);
  fb.setTextDatum(textdatum_t::middle_left);
  fb.setTextColor(C_WARN);
  int w = fb.textWidth(ticker);
  fb.drawString(ticker, (int)tickerX, 162);
  if (tickerX + w < 320 - 40) fb.drawString(ticker, (int)tickerX + w + 40, 162);  // seamless wrap
  tickerX -= 1.4f;
  if (tickerX < -(w + 40)) tickerX += w + 40;
}

static void tileValueText(const Widget &w, char *out, size_t n) {
  if (w.text[0]) {
    snprintf(out, n, "%s", w.text);
  } else {
    float a = fabsf(w.shown);
    if (a >= 100 || fabsf(w.v - roundf(w.v)) < 0.001f) snprintf(out, n, "%.0f", w.shown);
    else if (a >= 10) snprintf(out, n, "%.1f", w.shown);
    else snprintf(out, n, "%.2f", w.shown);
  }
}

// Draw s within maxw pixels: the first font that fits wins; otherwise the last font,
// cut with "..".
static void drawFit(const char *s, int x, int y, int maxw, textdatum_t datum, const lgfx::IFont *f1,
                    const lgfx::IFont *f2 = nullptr, const lgfx::IFont *f3 = nullptr) {
  const lgfx::IFont *fl[3] = {f1, f2, f3};
  const lgfx::IFont *last = f1;
  fb.setTextDatum(datum);
  for (auto f : fl) {
    if (!f) continue;
    last = f;
    fb.setFont(f);
    if (fb.textWidth(s) <= maxw) {
      fb.drawString(s, x, y);
      return;
    }
  }
  fb.setFont(last);
  String t = s;
  while (t.length() > 1 && fb.textWidth((t + "..").c_str()) > maxw) t.remove(t.length() - 1);
  t.trim();
  t += "..";
  fb.drawString(t.c_str(), x, y);
}

static void drawTile(const Widget &w, int x, int y, int tw, int th, uint32_t now) {
  uint32_t c = levelColor(w.level);
  bool fresh = now - w.updated < 600;  // brief glow when a tile updates
  drawPanel(x, y, tw, th, fresh ? mix(C_EDGE, c, 0.8f) : C_EDGE);
  fb.fillRect(x + 6, y + 1, 18, 2, c);  // accent tick
  char val[40];
  tileValueText(w, val, sizeof(val));
  int cx = x + tw / 2, cy = y + th / 2 + 6;

  // sparkline: latest value top right, so the label gets what's left
  char last[24] = "";
  int labelW = tw - 14;
  if (w.type == W_SPARK && w.n) {
    snprintf(last, sizeof(last), "%.4g%s", w.series[w.n - 1], w.unit);
    fb.setFont(&fonts::DejaVu9);
    labelW -= fb.textWidth(last) + 6;
  }
  fb.setTextColor(C_DIM);
  drawFit(w.label, x + 7, y + 6, labelW, textdatum_t::top_left, &fonts::DejaVu9);

  switch (w.type) {
    case W_BIG: {
      fb.setTextColor(c);
      if (w.text[0]) {  // text value: largest font that fits
        drawFit(val, cx, cy, tw - 12, textdatum_t::middle_center, &fonts::DejaVu18, &fonts::DejaVu12, &fonts::DejaVu9);
        break;
      }
      const lgfx::IFont *vf = &fonts::Orbitron_Light_24;
      fb.setFont(&fonts::DejaVu9);
      int uw = w.unit[0] ? fb.textWidth(w.unit) + 3 : 0;
      fb.setFont(vf);
      if (fb.textWidth(val) + uw > tw - 12) {
        vf = &fonts::DejaVu18;
        fb.setFont(vf);
      }
      int vw = fb.textWidth(val);
      int left = cx - (vw + uw) / 2;
      fb.setTextDatum(textdatum_t::middle_left);
      fb.drawString(val, left, cy);
      if (uw) {
        fb.setFont(&fonts::DejaVu9);
        fb.setTextColor(C_DIM);
        fb.setTextDatum(textdatum_t::bottom_left);
        fb.drawString(w.unit, left + vw + 3, cy + 10);
      }
      break;
    }
    case W_BAR: {
      float f = (w.vmax > w.vmin) ? (w.shown - w.vmin) / (w.vmax - w.vmin) : 0;
      if (f < 0) f = 0;
      if (f > 1) f = 1;
      int bx = x + 8, bw = tw - 16, by = y + th - 18, bh = 9;
      fb.fillRoundRect(bx, by, bw, bh, 4, mix(C_BG, c, 0.15f));
      int fw = (int)(bw * f);
      if (fw > 3) {
        fb.fillRoundRect(bx, by, fw, bh, 4, c);
        fb.fillRect(bx + 2, by + 2, fw - 4 > 0 ? fw - 4 : 0, 2, mix(c, 0xFFFFFF, 0.35f));  // sheen
      }
      fb.setTextColor(C_TEXT);
      if (w.text[0]) {
        drawFit(w.text, bx, by - 3, bw, textdatum_t::bottom_left, &fonts::DejaVu12, &fonts::DejaVu9);
      } else {
        char pct[24];
        snprintf(pct, sizeof(pct), "%.0f%s", w.shown, w.unit[0] ? w.unit : "%");
        drawFit(pct, bx, by - 3, bw, textdatum_t::bottom_left, &fonts::DejaVu18, &fonts::DejaVu12);
      }
      break;
    }
    case W_GAUGE: {
      float f = (w.vmax > w.vmin) ? (w.shown - w.vmin) / (w.vmax - w.vmin) : 0;
      if (f < 0) f = 0;
      if (f > 1) f = 1;
      int r = th / 2 - 6;
      if (r > 26) r = 26;
      int gx = x + tw / 2, gy = y + th / 2 + 7;
      fb.fillArc(gx, gy, r - 5, r, 135, 405, mix(C_BG, c, 0.18f));
      if (f > 0.005f) fb.fillArc(gx, gy, r - 5, r, 135, 135 + 270 * f, c);
      float a = (135 + 270 * f) * DEG_TO_RAD;  // tip dot
      fb.fillCircle(gx + cosf(a) * (r - 2.5f), gy + sinf(a) * (r - 2.5f), 3, mix(c, 0xFFFFFF, 0.5f));
      fb.setTextColor(C_TEXT);
      drawFit(val, gx, gy, 2 * r - 12, textdatum_t::middle_center, &fonts::DejaVu12, &fonts::DejaVu9);
      if (w.unit[0]) {
        fb.setTextColor(C_DIM);
        drawFit(w.unit, gx, gy + r - 4, 2 * r - 8, textdatum_t::middle_center, &fonts::DejaVu9);
      }
      break;
    }
    case W_SPARK: {
      int px = x + 8, pw = tw - 16, py = y + 20, ph = th - 26;
      if (w.n >= 2) {
        float lo = w.series[0], hi = w.series[0];
        for (int i = 1; i < w.n; i++) {
          lo = min(lo, w.series[i]);
          hi = max(hi, w.series[i]);
        }
        if (hi - lo < 1e-6f) { hi += 1; lo -= 1; }
        float step = (float)pw / (w.n - 1);
        uint32_t fill = mix(C_PANEL, c, 0.22f);
        float prevx = 0, prevy = 0;
        for (int i = 0; i < w.n; i++) {
          float sx = px + i * step;
          float sy = py + ph - (w.series[i] - lo) / (hi - lo) * ph;
          if (i) {
            for (int fx = (int)prevx; fx <= (int)sx; fx++) {  // soft area under the line
              float t = (sx - prevx) > 0 ? (fx - prevx) / (sx - prevx) : 0;
              int fy = (int)(prevy + (sy - prevy) * t);
              fb.drawFastVLine(fx, fy, py + ph - fy, fill);
            }
            fb.drawLine((int)prevx, (int)prevy, (int)sx, (int)sy, c);  // two-pixel line, softer lower edge
            fb.drawLine((int)prevx, (int)prevy + 1, (int)sx, (int)sy + 1, mix(c, C_PANEL, 0.45f));
          }
          prevx = sx;
          prevy = sy;
        }
        fb.fillCircle((int)prevx, (int)prevy, 3, mix(c, 0xFFFFFF, 0.5f));
      }
      fb.setFont(&fonts::DejaVu9);
      fb.setTextDatum(textdatum_t::top_right);
      fb.setTextColor(C_TEXT);
      fb.drawString(last, x + tw - 7, y + 6);
      break;
    }
    case W_KV: {
      fb.setTextColor(c == C_ACCENT ? C_TEXT : c);
      drawFit(w.text, cx, cy, tw - 12, textdatum_t::middle_center, &fonts::DejaVu18, &fonts::DejaVu12, &fonts::DejaVu9);
      break;
    }
    case W_LED: {
      float pulse = (w.level == L_CRIT || w.level == L_WARN) ? 0.5f + 0.5f * sinf(now / 160.0f) : 1.0f;
      int lx = x + 20, ly = cy;
      for (int r = 13; r >= 5; r -= 2) fb.fillCircle(lx, ly, r, mix(C_PANEL, c, (0.12f + 0.08f * (13 - r) / 2) * pulse));
      fb.fillCircle(lx, ly, 5, mix(C_PANEL, c, 0.4f + 0.6f * pulse));
      fb.fillCircle(lx - 1, ly - 1, 1, 0xFFFFFF);
      fb.setTextColor(C_TEXT);
      if (w.text[0])
        drawFit(w.text, lx + 18, ly, x + tw - (lx + 18) - 6, textdatum_t::middle_left, &fonts::DejaVu12, &fonts::DejaVu9);
      break;
    }
    default:
      break;
  }
}

static void drawDashboard(uint32_t now) {
  int n = tileCount();
  int bottom = ticker[0] ? 151 : 168;
  int top = 26, gap = 4;
  int cols = n <= 2 ? n : (n <= 4 ? 2 : 3);
  if (cols < 1) cols = 1;
  int rows = (n + cols - 1) / cols;
  if (rows < 1) rows = 1;
  int tw = (320 - gap * (cols + 1)) / cols;
  int th = (bottom - top - gap * (rows - 1)) / rows;
  int i = 0;
  for (auto &w : tiles) {
    if (w.type == W_NONE) continue;
    int r = i / cols, col = i % cols;
    drawTile(w, gap + col * (tw + gap), top + r * (th + gap), tw, th, now);
    i++;
  }
}

static void drawLog(uint32_t now) {
  int bottom = ticker[0] ? 151 : 168;
  drawPanel(3, 25, 314, bottom - 25, C_EDGE);
  fb.setFont(&fonts::DejaVu12);
  fb.setTextDatum(textdatum_t::top_left);
  const int lh = 14;
  int visible = (bottom - 31) / lh;
  int shown = logCount < visible ? logCount : visible;
  for (int i = 0; i < shown; i++) {
    int idx = (logHead - shown + i + LOG_LINES) % LOG_LINES;
    const LogLine &l = logbuf[idx];
    uint32_t col = l.level == L_INFO ? C_TEXT : levelColor(l.level);
    if (i == shown - 1 && now - lastRx < 800) col = mix(col, 0xFFFFFF, 0.3f);  // newest line glints
    fb.setTextColor(col);
    fb.drawString(l.text, 9, 29 + i * lh);
  }
  if (!logCount) {
    fb.setTextColor(C_DIM);
    fb.drawString("Waiting for data on USB ...  (send HELP)", 9, 29);
  }
}

static void drawRings(int cx, int cy, uint32_t now, float scale) {
  // arc-reactor style idle animation
  float t = now / 1000.0f;
  for (int k = 0; k < 3; k++) {
    int r = (int)((22 + k * 12) * scale);
    float a = fmodf(t * (k % 2 ? -60 : 90) + k * 40, 360);
    uint32_t c = mix(C_BG, C_ACCENT, 0.9f - k * 0.25f);
    fb.fillArc(cx, cy, r - 3, r, a, a + 70, c);
    fb.fillArc(cx, cy, r - 3, r, a + 180, a + 250, c);
  }
  fb.fillCircle(cx, cy, (int)(9 * scale), mix(C_BG, C_ACCENT, 0.5f + 0.4f * sinf(t * 3)));
  fb.fillCircle(cx, cy, (int)(5 * scale), 0xFFFFFF);
}

static void drawAlert(uint32_t now) {
  uint32_t c = levelColor(alertLevel);
  float pulse = 0.5f + 0.5f * sinf(now / 140.0f);
  fb.fillRoundRect(14, 34, 292, 102, 10, mix(C_BG, c, 0.12f));
  for (int k = 0; k < 3; k++) fb.drawRoundRect(14 - k, 34 - k, 292 + 2 * k, 102 + 2 * k, 10 + k, mix(C_BG, c, 0.4f + 0.6f * pulse - k * 0.25f));
  // hazard stripes along the top
  for (int sx = 20; sx < 300; sx += 16) fb.fillTriangle(sx, 40, sx + 8, 40, sx + 4, 46, mix(C_PANEL, c, 0.7f));
  fb.setFont(&fonts::DejaVu18);
  fb.setTextColor(C_TEXT);
  fb.setTextDatum(textdatum_t::middle_center);
  // up to 3 wrapped lines
  String s = alertText, lines[3];
  int nl = 0;
  while (s.length() && nl < 3) {
    int cut = s.length();
    while (cut > 1 && fb.textWidth(s.substring(0, cut).c_str()) > 270) {
      int sp = s.lastIndexOf(' ', cut - 1);
      cut = sp > 0 ? sp : cut - 1;
    }
    lines[nl++] = s.substring(0, cut);
    s = s.substring(cut);
    s.trim();
  }
  int y0 = 88 - (nl - 1) * 11;
  for (int i = 0; i < nl; i++) fb.drawString(lines[i].c_str(), 160, y0 + i * 22);
}

static void render(uint32_t now) {
  fb.fillScreen(C_BG);
  drawHeader(now);
  bool idle = !tileCount() && !logCount;
  if (idle) {
    drawRings(160, 84, now, 1.0f);
    fb.setFont(&fonts::DejaVu9);
    fb.setTextDatum(textdatum_t::middle_center);
    fb.setTextColor(C_DIM);
    fb.drawString("READY  -  waiting for USB data  (send HELP)", 160, 140);
  } else if (dashboardVisible()) {
    drawDashboard(now);
  } else {
    drawLog(now);
  }
  drawTicker(now);
  if (alertOn) drawAlert(now);
  fb.pushSprite(0, 0);
}

static void splash() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1400) {
    uint32_t now = millis();
    float k = (now - t0) / 1400.0f;
    fb.fillScreen(C_BG);
    drawRings(160, 74, now, 0.6f + 0.4f * k);
    fb.setFont(&fonts::Orbitron_Light_24);
    fb.setTextDatum(textdatum_t::middle_center);
    fb.setTextColor(mix(C_BG, C_TEXT, k * 1.5f));
    fb.drawString("USB DISPLAY", 160, 138);
    fb.setFont(&fonts::DejaVu9);
    fb.setTextColor(mix(C_BG, C_DIM, k));
    char v[32];
    snprintf(v, sizeof(v), "T-Display-S3  v%s", FW_VERSION);
    fb.drawString(v, 160, 158);
    fb.pushSprite(0, 0);
  }
}

// ---------------------------------------------------------------- commands
static void sendScreenshot() {
  // "SHOT 320 170" + base64 of RGB565 (big-endian), 76 chars per line, then "END"
  static const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Serial.printf("SHOT %d %d\n", fb.width(), fb.height());
  uint8_t in[3];
  int nin = 0, col = 0;
  char line[80];
  auto flush3 = [&](int cnt) {
    uint32_t v = ((uint32_t)in[0] << 16) | ((cnt > 1 ? in[1] : 0) << 8) | (cnt > 2 ? in[2] : 0);
    line[col++] = B64[(v >> 18) & 63];
    line[col++] = B64[(v >> 12) & 63];
    line[col++] = cnt > 1 ? B64[(v >> 6) & 63] : '=';
    line[col++] = cnt > 2 ? B64[v & 63] : '=';
    if (col >= 76) {
      line[col] = 0;
      Serial.println(line);
      col = 0;
    }
  };
  for (int y = 0; y < fb.height(); y++) {
    for (int x = 0; x < fb.width(); x++) {
      uint16_t p = fb.readPixel(x, y);
      uint8_t bytes[2] = {(uint8_t)(p >> 8), (uint8_t)(p & 255)};
      for (uint8_t b : bytes) {
        in[nin++] = b;
        if (nin == 3) { flush3(3); nin = 0; }
      }
    }
  }
  if (nin) flush3(nin);
  if (col) { line[col] = 0; Serial.println(line); }
  Serial.println("END");
}

static void help() {
  Serial.println(
      "T-Display-S3 USB display: one command per line; fields separated by '|'.\n"
      "  TITLE text                       header title\n"
      "  STATUS level|text                status pill (level: ok warn crit info off)\n"
      "  BIG label|value|unit|level       big number tile\n"
      "  BAR label|value|max|text|level   progress bar (max 100 if empty)\n"
      "  GAUGE label|value|min|max|unit|level  arc gauge\n"
      "  SPARK label|v1,v2,...|unit|level sparkline (SPARK label|+v appends one)\n"
      "  KV label|value|level             key/value tile\n"
      "  LED label|level|text             status light tile\n"
      "  DEL label   CLEAR (tiles)   CLS (log)   RESET (everything)\n"
      "  LOG text  (or any other line)    append to the log; prefix ! crit, ? warn, + ok\n"
      "  ALERT level|text|seconds         overlay (0 s = until KEY pressed or ALERT off)\n"
      "  TICKER text                      scrolling line at the bottom (empty clears)\n"
      "  PAGE auto|dash|log   BRIGHT 0-255   ROT 1|3\n"
      "  PING   SHOT (screenshot as base64 RGB565)   HELP");
}

static float num(const String &s, float dflt) {
  String t = s;
  t.trim();
  return t.length() ? t.toFloat() : dflt;
}

static void handle(String line) {
  line.trim();
  if (!line.length()) return;
  lastRx = millis();
  rxLines++;
  int sp = line.indexOf(' ');
  String cmd = sp > 0 ? line.substring(0, sp) : line;
  String arg = sp > 0 ? line.substring(sp + 1) : "";
  arg.trim();
  String c = cmd;
  c.toUpperCase();
  String f[7];
  uint32_t now = millis();

  if (c == "TITLE") {
    copyStr(title, sizeof(title), arg);
  } else if (c == "STATUS") {
    int n = splitFields(arg, f, 2);
    if (n == 1) { statusLevel = L_INFO; copyStr(statusText, sizeof(statusText), f[0]); }
    else { statusLevel = parseLevel(f[0]); copyStr(statusText, sizeof(statusText), f[1]); }
  } else if (c == "BIG") {
    int n = splitFields(arg, f, 4);
    Widget *w = tileFor(f[0], W_BIG);
    String v = n > 1 ? f[1] : "";
    bool numeric = v.length() && (isdigit(v[0]) || v[0] == '-' || v[0] == '.');
    if (numeric) { w->v = v.toFloat(); w->text[0] = 0; }
    else copyStr(w->text, sizeof(w->text), v);
    copyStr(w->unit, sizeof(w->unit), n > 2 ? f[2] : "");
    w->level = n > 3 ? parseLevel(f[3]) : L_INFO;
    w->updated = now;
  } else if (c == "BAR") {
    int n = splitFields(arg, f, 5);
    Widget *w = tileFor(f[0], W_BAR);
    w->v = num(n > 1 ? f[1] : "", 0);
    w->vmin = 0;
    w->vmax = num(n > 2 ? f[2] : "", 100);
    copyStr(w->text, sizeof(w->text), n > 3 ? f[3] : "");
    w->level = n > 4 ? parseLevel(f[4]) : L_INFO;
    w->updated = now;
  } else if (c == "GAUGE") {
    int n = splitFields(arg, f, 6);
    Widget *w = tileFor(f[0], W_GAUGE);
    w->v = num(n > 1 ? f[1] : "", 0);
    w->vmin = num(n > 2 ? f[2] : "", 0);
    w->vmax = num(n > 3 ? f[3] : "", 100);
    copyStr(w->unit, sizeof(w->unit), n > 4 ? f[4] : "");
    w->text[0] = 0;
    w->level = n > 5 ? parseLevel(f[5]) : L_INFO;
    w->updated = now;
  } else if (c == "SPARK") {
    int n = splitFields(arg, f, 4);
    Widget *w = tileFor(f[0], W_SPARK);
    String vals = n > 1 ? f[1] : "";
    if (vals.startsWith("+")) {
      float v = vals.substring(1).toFloat();
      if (w->n == 48) { memmove(w->series, w->series + 1, 47 * sizeof(float)); w->n = 47; }
      w->series[w->n++] = v;
    } else if (vals.length()) {
      w->n = 0;
      int start = 0;
      while (start <= (int)vals.length() && w->n < 48) {
        int comma = vals.indexOf(',', start);
        String one = comma < 0 ? vals.substring(start) : vals.substring(start, comma);
        one.trim();
        if (one.length()) w->series[w->n++] = one.toFloat();
        if (comma < 0) break;
        start = comma + 1;
      }
    }
    if (n > 2) copyStr(w->unit, sizeof(w->unit), f[2]);
    w->level = n > 3 ? parseLevel(f[3]) : L_INFO;
    w->updated = now;
  } else if (c == "KV") {
    int n = splitFields(arg, f, 3);
    Widget *w = tileFor(f[0], W_KV);
    copyStr(w->text, sizeof(w->text), n > 1 ? f[1] : "");
    w->level = n > 2 ? parseLevel(f[2]) : L_INFO;
    w->updated = now;
  } else if (c == "LED") {
    int n = splitFields(arg, f, 3);
    Widget *w = tileFor(f[0], W_LED);
    w->level = n > 1 ? parseLevel(f[1], L_OK) : L_OK;
    copyStr(w->text, sizeof(w->text), n > 2 ? f[2] : "");
    w->updated = now;
  } else if (c == "DEL") {
    for (auto &w : tiles)
      if (w.type != W_NONE && arg.equalsIgnoreCase(w.label)) w = Widget();
  } else if (c == "CLEAR") {
    for (auto &w : tiles) w = Widget();
  } else if (c == "CLS") {
    logCount = 0;
    logHead = 0;
  } else if (c == "RESET") {
    for (auto &w : tiles) w = Widget();
    logCount = logHead = 0;
    ticker[0] = statusText[0] = 0;
    strcpy(title, "USB DISPLAY");
    alertOn = false;
    page = P_AUTO;
  } else if (c == "LOG") {
    uint8_t lv = L_INFO;
    if (arg.startsWith("!")) { lv = L_CRIT; arg = arg.substring(1); }
    else if (arg.startsWith("?")) { lv = L_WARN; arg = arg.substring(1); }
    else if (arg.startsWith("+")) { lv = L_OK; arg = arg.substring(1); }
    arg.trim();
    logAdd(arg, lv);
  } else if (c == "ALERT") {
    int n = splitFields(arg, f, 3);
    if (n == 1 && (f[0].equalsIgnoreCase("off") || !f[0].length())) {
      alertOn = false;
    } else {
      alertLevel = n > 1 ? parseLevel(f[0], L_CRIT) : L_CRIT;
      copyStr(alertText, sizeof(alertText), n > 1 ? f[1] : f[0]);
      float secs = num(n > 2 ? f[2] : "", 10);
      alertUntil = secs > 0 ? now + (uint32_t)(secs * 1000) : 0;
      alertOn = true;
    }
  } else if (c == "TICKER") {
    copyStr(ticker, sizeof(ticker), arg);
    tickerX = 320;
  } else if (c == "PAGE") {
    String p = arg;
    p.toLowerCase();
    page = p == "dash" ? P_DASH : p == "log" ? P_LOG : P_AUTO;
  } else if (c == "BRIGHT") {
    bright = (uint8_t)constrain(arg.toInt(), 0, 255);
    tft.setBrightness(dim ? bright / 6 : bright);
  } else if (c == "ROT") {
    tft.setRotation(arg.toInt() == 3 ? 3 : 1);
  } else if (c == "PING") {
    Serial.printf("PONG tdisplay-usb %s %dx%d uptime=%lus lines=%lu\n", FW_VERSION, fb.width(), fb.height(),
                  (unsigned long)((now - bootMs) / 1000), (unsigned long)rxLines);
  } else if (c == "SHOT") {
    sendScreenshot();
  } else if (c == "HELP") {
    help();
  } else {
    // not a command: show the whole line in the log
    uint8_t lv = L_INFO;
    String t = line;
    if (t.startsWith("!")) { lv = L_CRIT; t = t.substring(1); }
    else if (t.startsWith("?")) { lv = L_WARN; t = t.substring(1); }
    else if (t.startsWith("+")) { lv = L_OK; t = t.substring(1); }
    t.trim();
    logAdd(t, lv);
  }
}

// ---------------------------------------------------------------- main
static String rx;
static const unsigned MAX_LINE = 1024;  // longer lines are cut (the rest is ignored)

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);
  pinMode(PIN_KEY_2, INPUT_PULLUP);
  // A long command line arriving while a frame is drawing (~40 ms) must not overflow the
  // USB receive buffer: the 256-byte default stalls reception until the port is reopened.
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block when no host is reading
  bootMs = millis();
  tft.init();
  tft.setRotation(1);  // landscape 320x170, USB port on the left
  tft.setBrightness(bright);
  fb.setColorDepth(16);
  if (!fb.createSprite(tft.width(), tft.height())) {
    fb.setPsram(true);
    fb.createSprite(tft.width(), tft.height());
  }
  splash();
  esp_task_wdt_init(WATCHDOG_SECONDS, true);
  esp_task_wdt_add(NULL);  // this (loop) task
}

void loop() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n') {
      handle(rx);
      rx = "";
    } else if (ch != '\r' && rx.length() < MAX_LINE) {
      rx += ch;
    }
  }

  uint32_t now = millis();
  static uint32_t lastKey = 0, lastFrame = 0;
  static bool k2Was = false, k0Was = false;
  bool k2 = digitalRead(PIN_KEY_2) == LOW, k0 = digitalRead(PIN_KEY_BOOT) == LOW;
  if (now - lastKey > 180) {
    if (k2 && !k2Was) {
      if (alertOn) alertOn = false;
      else page = (Page)((page + 1) % 3);
      lastKey = now;
    }
    if (k0 && !k0Was) {
      dim = !dim;
      tft.setBrightness(dim ? bright / 6 : bright);
      lastKey = now;
    }
  }
  k2Was = k2;
  k0Was = k0;

  if (alertOn && alertUntil && now > alertUntil) alertOn = false;

  // ease displayed values toward their targets
  for (auto &w : tiles)
    if (w.type == W_BAR || w.type == W_GAUGE || w.type == W_BIG) w.shown += (w.v - w.shown) * 0.18f;

  if (now - lastFrame >= 33) {
    lastFrame = now;
    render(now);
  }
  esp_task_wdt_reset();
  delay(2);
}
