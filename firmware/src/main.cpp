/*
 * RNK Runtime 0.1 pour LilyGO T-Watch 2020 V3
 *
 * Lit un fichier layout.json produit par l'Atelier RNK, l'affiche sur l'écran 240x240
 * et le met à jour. Reçoit de nouveaux cadrans par USB série (115200 bauds).
 *
 * Protocole série (une commande par ligne) :
 *   RNK:PING                          -> RNK:PONG <version>
 *   RNK:INFO                          -> RNK:INFO ...
 *   RNK:TIME a m j h mi s             -> règle l'horloge, répond RNK:OK
 *   RNK:BEGIN <octets> <crc32 hex>    -> RNK:READY puis <octets> bruts, puis "\nRNK:END\n"
 *                                     -> RNK:OK <pages> ou RNK:ERR <raison>
 */
#define LILYGO_WATCH_2020_V3
#include <LilyGoWatch.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"

#define FW_VERSION      "rnk-runtime-0.3"
#define LAYOUT_PATH     "/layout.json"
#define MAX_LAYOUT      (160 * 1024)
#define SLEEP_AFTER_MS  20000UL     // extinction après inactivité, sur batterie uniquement
#define RX_TIMEOUT_MS   8000UL
#define MAX_PAGES       16
#define SVC_UUID        "52a10001-8f4b-4c1e-9d6b-2f5a7c3e1b00"
#define RX_UUID         "52a10002-8f4b-4c1e-9d6b-2f5a7c3e1b00"   // téléphone -> montre (écriture)
#define TX_UUID         "52a10003-8f4b-4c1e-9d6b-2f5a7c3e1b00"   // montre -> téléphone (notification)

static const float DEG = 0.01745329252f;

TTGOClass *ttgo = nullptr;
TFT_eSprite *spr = nullptr;

/* ---------- Données affichées ---------- */
struct Data {
  int hour = 0, minute = 0, second = 0, day = 1, month = 1, year = 2026, wday = 0;
  int batt = 0;
  bool usb = false;
} D;

static const char *JOURS[] = {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"};
static const char *MOIS[] = {"JAN", "FEV", "MAR", "AVR", "MAI", "JUIN", "JUIL", "AOU", "SEP", "OCT", "NOV", "DEC"};

static int dayOfWeek(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* ---------- Mise en page ---------- */
DynamicJsonDocument *docPtr = nullptr;
JsonArray pages;
int pageCount = 0;
int curPage = 0;
int lastCadran = 0;
bool pageLive[MAX_PAGES];
bool pageAnim[MAX_PAGES];   // page avec une image dont la couleur change : redessin rapide
bool dirty = true;
uint8_t brightness = 160;

struct Theme { uint16_t bg, panel, accent, accent2, text, dim, track, line, metal; };

static uint16_t hex565(const char *s) {
  if (!s || s[0] != '#' || strlen(s) < 7) return 0;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  return spr->color565((v >> 16) & 255, (v >> 8) & 255, v & 255);
}
static uint16_t hexShade(const char *s, float k) {
  if (!s || s[0] != '#' || strlen(s) < 7) return 0;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  int r = ((v >> 16) & 255) * k, g = ((v >> 8) & 255) * k, b = (v & 255) * k;
  return spr->color565(constrain(r, 0, 255), constrain(g, 0, 255), constrain(b, 0, 255));
}

static Theme loadTheme(JsonObject page) {
  Theme t;
  JsonObject o = page["theme"];
  t.bg = hex565(o["bg"] | "#000000");
  t.panel = hex565(o["panel"] | "#101010");
  t.accent = hex565(o["accent"] | "#a82328");
  t.accent2 = hex565(o["accent2"] | "#7d1a1e");
  t.text = hex565(o["text"] | "#cfc6b8");
  t.dim = hex565(o["dim"] | "#3b3b3e");
  t.track = hex565(o["track"] | "#241415");
  t.line = hex565(o["line"] | "#5e1a1d");
  t.metal = hex565(o["metal"] | "#3a3a3d");
  return t;
}

/* Couleur d'un champ de l'élément ; false si "none". */
static bool ec(JsonObject e, const char *k, uint16_t &out) {
  const char *s = e[k] | "none";
  if (!strcmp(s, "none") || s[0] != '#') return false;
  out = hex565(s);
  return true;
}

/* ---------- Primitives ---------- */
static inline int R(float v) { return (int)lroundf(v); }

static void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  if (w <= 1.2f) { spr->drawLine(R(x0), R(y0), R(x1), R(y1), c); return; }
  float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
  if (l < 0.01f) { spr->fillCircle(R(x0), R(y0), R(w / 2), c); return; }
  float nx = -dy / l * w / 2, ny = dx / l * w / 2;
  spr->fillTriangle(R(x0 + nx), R(y0 + ny), R(x0 - nx), R(y0 - ny), R(x1 + nx), R(y1 + ny), c);
  spr->fillTriangle(R(x0 - nx), R(y0 - ny), R(x1 - nx), R(y1 - ny), R(x1 + nx), R(y1 + ny), c);
}
static void polar(float cx, float cy, float r, float deg, float &x, float &y) {
  x = cx + r * sinf(deg * DEG);
  y = cy - r * cosf(deg * DEG);
}
static void arcStroke(float cx, float cy, float r, float a0, float a1, float w, uint16_t c) {
  float step = fmaxf(1.0f, 140.0f / r);
  float px, py, qx, qy;
  polar(cx, cy, r, a0, px, py);
  for (float a = a0 + step; ; a += step) {
    bool last = a >= a1;
    if (last) a = a1;
    polar(cx, cy, r, a, qx, qy);
    thickLine(px, py, qx, qy, w, c);
    if (w >= 3) spr->fillCircle(R(qx), R(qy), R(w / 2) > 0 ? R(w / 2) : 1, c);
    px = qx; py = qy;
    if (last) break;
  }
}

/* Police selon la taille demandée dans l'éditeur (px de la police condensée). */
static const GFXfont *fontFor(float size) {
  if (size < 13) return &FreeSans9pt7b;
  if (size < 18) return &FreeSansBold9pt7b;
  if (size < 26) return &FreeSansBold12pt7b;
  if (size < 38) return &FreeSansBold18pt7b;
  return &FreeSansBold24pt7b;
}
static void drawText(const String &s, float x, float y, float w, float h, const char *anchor, float size, uint16_t c) {
  spr->setFreeFont(fontFor(size));
  spr->setTextColor(c);
  float ax;
  if (!strcmp(anchor, "start")) { spr->setTextDatum(ML_DATUM); ax = x; }
  else if (!strcmp(anchor, "end")) { spr->setTextDatum(MR_DATUM); ax = x + w; }
  else { spr->setTextDatum(MC_DATUM); ax = x + w / 2; }
  spr->drawString(s, R(ax), R(y + h / 2));
}
static void centerText(const String &s, float cx, float cy, float size, uint16_t c) {
  spr->setFreeFont(fontFor(size));
  spr->setTextColor(c);
  spr->setTextDatum(MC_DATUM);
  spr->drawString(s, R(cx), R(cy));
}

/* ---------- Données du téléphone (Bluetooth) ---------- */
struct Cry { String sym, price, chg; };
struct Phone {
  bool link = false;
  unsigned long lastData = 0;
  float speed = 0, km = 0, avg = 0;
  int durS = 0;
  String artist, title;
  int posS = 0, trackS = 0;
  bool playing = false;
  Cry cry[8];
  int ncry = 0;
} PH;
static bool phoneFresh() { return PH.link && millis() - PH.lastData < 15000UL; }
static String durText(int s) { char b[12]; if (s >= 3600) snprintf(b, sizeof b, "%02d:%02d", s / 3600, (s % 3600) / 60); else snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); return String(b); }
static String trackText(int s) { char b[12]; snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60); return String(b); }
static int splitBar(const String &l, String *out, int maxn) {
  int n = 0, st = 0, len = (int)l.length();
  for (int i = 0; i <= len; i++) {
    if (i == len || l.charAt(i) == '|') { if (n < maxn) out[n++] = l.substring(st, i); st = i + 1; }
  }
  return n;
}
static const Cry *findCry(const char *sym) {
  for (int i = 0; i < PH.ncry; i++) if (PH.cry[i].sym == sym) return &PH.cry[i];
  return nullptr;
}

/* ---------- Liaisons de données ---------- */
static String two(int n) { char b[8]; snprintf(b, sizeof b, "%02d", n); return String(b); }
static String bindText(const char *b, JsonObject e) {
  if (!strcmp(b, "heure")) return two(D.hour);
  if (!strcmp(b, "minute")) return two(D.minute);
  if (!strcmp(b, "seconde")) return two(D.second);
  if (!strcmp(b, "hm")) return two(D.hour) + ":" + two(D.minute);
  if (!strcmp(b, "date")) return String(JOURS[D.wday]) + " " + two(D.day) + " " + MOIS[D.month - 1];
  if (!strcmp(b, "jour")) return JOURS[D.wday];
  if (!strcmp(b, "journ")) return two(D.day);
  if (!strcmp(b, "mois")) return MOIS[D.month - 1];
  if (!strcmp(b, "batt")) return String(D.batt) + "%";
  bool ph = phoneFresh();
  if (!strcmp(b, "speed")) return ph ? String((int)roundf(PH.speed)) : String("--");
  if (!strcmp(b, "km")) return ph ? String(PH.km, 1) : String("--");
  if (!strcmp(b, "dur")) return ph ? durText(PH.durS) : String("--");
  if (!strcmp(b, "kmh")) return ph ? String(PH.avg, 1) : String("--");
  if (!strcmp(b, "artiste")) return ph && PH.artist.length() ? PH.artist : String("--");
  if (!strcmp(b, "titre")) return ph && PH.title.length() ? PH.title : String("--");
  if (!strcmp(b, "tpos")) return ph ? trackText(PH.posS) : String("--");
  if (!strcmp(b, "tdur")) return ph ? trackText(PH.trackS) : String("--");
  return String((const char *)(e["text"] | ""));
}
static float numBind(const char *b, JsonObject e) {
  if (!strcmp(b, "minute")) return D.minute;
  if (!strcmp(b, "seconde")) return D.second;
  if (!strcmp(b, "heure12")) return (D.hour % 12) + D.minute / 60.0f;
  if (!strcmp(b, "battery")) return D.batt;
  if (!strcmp(b, "speed")) return phoneFresh() ? PH.speed : 0;
  return e["value"] | 0.0f;
}

/* ---------- Glyphes d'icônes (traits simples) ---------- */
static void glyph(const char *n, float cx, float cy, float s, uint16_t c) {
  float h = s / 2;
  if (!strcmp(n, "play")) spr->fillTriangle(R(cx - h * .5f), R(cy - h * .8f), R(cx - h * .5f), R(cy + h * .8f), R(cx + h * .8f), R(cy), c);
  else if (!strcmp(n, "pause")) { thickLine(cx - h * .4f, cy - h * .7f, cx - h * .4f, cy + h * .7f, s * .18f, c); thickLine(cx + h * .4f, cy - h * .7f, cx + h * .4f, cy + h * .7f, s * .18f, c); }
  else if (!strcmp(n, "next")) { spr->fillTriangle(R(cx - h * .7f), R(cy - h * .7f), R(cx - h * .7f), R(cy + h * .7f), R(cx + h * .5f), R(cy), c); thickLine(cx + h * .7f, cy - h * .7f, cx + h * .7f, cy + h * .7f, s * .14f, c); }
  else if (!strcmp(n, "prev")) { spr->fillTriangle(R(cx + h * .7f), R(cy - h * .7f), R(cx + h * .7f), R(cy + h * .7f), R(cx - h * .5f), R(cy), c); thickLine(cx - h * .7f, cy - h * .7f, cx - h * .7f, cy + h * .7f, s * .14f, c); }
  else if (!strcmp(n, "gps")) spr->fillTriangle(R(cx), R(cy - h * .9f), R(cx + h * .7f), R(cy + h * .8f), R(cx - h * .7f), R(cy + h * .8f), c);
  else if (!strcmp(n, "mic")) { spr->drawRoundRect(R(cx - h * .3f), R(cy - h * .8f), R(h * .6f), R(h * 1.1f), R(h * .3f), c); arcStroke(cx, cy - h * .1f, h * .6f, 90, 270, 1.5f, c); thickLine(cx, cy + h * .5f, cx, cy + h * .9f, 1.5f, c); }
  else if (!strcmp(n, "gear")) { spr->drawCircle(R(cx), R(cy), R(h * .6f), c); spr->drawCircle(R(cx), R(cy), R(h * .28f), c); for (int a = 0; a < 360; a += 45) { float x0, y0, x1, y1; polar(cx, cy, h * .6f, a, x0, y0); polar(cx, cy, h * .92f, a, x1, y1); thickLine(x0, y0, x1, y1, 2, c); } }
  else if (!strcmp(n, "dots")) { for (int i = -1; i <= 1; i++) spr->fillCircle(R(cx + i * h * .6f), R(cy), R(fmaxf(1.5f, h * .14f)), c); }
  else if (!strcmp(n, "chart")) { for (int i = 0; i < 4; i++) { float bh = h * (0.7f + 0.5f * ((i * 3) % 4) / 3.0f); thickLine(cx - h * .7f + i * h * .47f, cy + h * .7f, cx - h * .7f + i * h * .47f, cy + h * .7f - bh, s * .11f, c); } }
  else if (!strcmp(n, "sun")) { spr->drawCircle(R(cx), R(cy), R(h * .38f), c); for (int a = 0; a < 360; a += 45) { float x0, y0, x1, y1; polar(cx, cy, h * .6f, a, x0, y0); polar(cx, cy, h * .9f, a, x1, y1); thickLine(x0, y0, x1, y1, 1.5f, c); } }
  else if (!strcmp(n, "screen")) { spr->drawRoundRect(R(cx - h * .5f), R(cy - h * .85f), R(h), R(h * 1.7f), 3, c); thickLine(cx - h * .15f, cy + h * .62f, cx + h * .15f, cy + h * .62f, 1.5f, c); }
  else if (!strcmp(n, "vibrate")) { spr->drawRoundRect(R(cx - h * .4f), R(cy - h * .65f), R(h * .8f), R(h * 1.3f), 2, c); thickLine(cx - h * .8f, cy - h * .3f, cx - h * .8f, cy + h * .3f, 1.5f, c); thickLine(cx + h * .8f, cy - h * .3f, cx + h * .8f, cy + h * .3f, 1.5f, c); }
  else if (!strcmp(n, "bluetooth")) { thickLine(cx - h * .45f, cy - h * .4f, cx + h * .45f, cy + h * .4f, 1.5f, c); thickLine(cx + h * .45f, cy + h * .4f, cx, cy + h * .85f, 1.5f, c); thickLine(cx, cy + h * .85f, cx, cy - h * .85f, 1.5f, c); thickLine(cx, cy - h * .85f, cx + h * .45f, cy - h * .4f, 1.5f, c); thickLine(cx + h * .45f, cy - h * .4f, cx - h * .45f, cy + h * .4f, 1.5f, c); }
  else if (!strcmp(n, "palette")) { spr->drawCircle(R(cx), R(cy), R(h * .8f), c); spr->fillCircle(R(cx - h * .3f), R(cy), 1, c); spr->fillCircle(R(cx), R(cy - h * .35f), 1, c); spr->fillCircle(R(cx + h * .3f), R(cy), 1, c); }
  else if (!strcmp(n, "system")) { thickLine(cx - h * .8f, cy - h * .4f, cx + h * .8f, cy - h * .4f, 1.5f, c); thickLine(cx - h * .8f, cy + h * .4f, cx + h * .8f, cy + h * .4f, 1.5f, c); spr->fillCircle(R(cx + h * .3f), R(cy - h * .4f), 2, c); spr->fillCircle(R(cx - h * .3f), R(cy + h * .4f), 2, c); }
  else if (!strcmp(n, "chevron")) { thickLine(cx - h * .3f, cy - h * .6f, cx + h * .3f, cy, 1.5f, c); thickLine(cx + h * .3f, cy, cx - h * .3f, cy + h * .6f, 1.5f, c); }
  else if (!strcmp(n, "clock")) { spr->drawCircle(R(cx), R(cy), R(h * .8f), c); thickLine(cx, cy, cx, cy - h * .5f, 1.5f, c); thickLine(cx, cy, cx + h * .35f, cy + h * .2f, 1.5f, c); }
  else if (!strcmp(n, "heart")) { spr->fillCircle(R(cx - h * .3f), R(cy - h * .2f), R(h * .35f), c); spr->fillCircle(R(cx + h * .3f), R(cy - h * .2f), R(h * .35f), c); spr->fillTriangle(R(cx - h * .62f), R(cy - h * .05f), R(cx + h * .62f), R(cy - h * .05f), R(cx), R(cy + h * .75f), c); }
  else if (!strcmp(n, "shuffle")) { thickLine(cx - h * .8f, cy - h * .4f, cx + h * .8f, cy + h * .4f, 1.5f, c); thickLine(cx - h * .8f, cy + h * .4f, cx + h * .8f, cy - h * .4f, 1.5f, c); }
  else if (!strcmp(n, "spotify")) { spr->drawCircle(R(cx), R(cy), R(h * .85f), c); for (int i = 0; i < 3; i++) arcStroke(cx, cy + h * (0.9f - i * .3f), h * (0.8f - i * .12f), -35, 35, 1.5f, c); }
  else spr->drawCircle(R(cx), R(cy), R(h * .6f), c);
}

/* ---------- Dessin des éléments ---------- */
static void drawFrame(JsonObject e, const Theme &t, JsonObject page) {
  float w = e["w"] | 240.0f, h = e["h"] | 240.0f, r = e["r"] | 14.0f;
  const char *style = e["style"] | "metal";
  int b = !strcmp(style, "metal") ? 8 : (!strcmp(style, "fin") ? 3 : 0);
  if (b > 0) spr->fillRoundRect(0, 0, R(w), R(h), R(r + b), t.metal);
  spr->fillRoundRect(b, b, R(w - 2 * b), R(h - 2 * b), R(r), t.bg);
  if (e["border"] | true) spr->drawRoundRect(b + 5, b + 5, R(w - 2 * b - 10), R(h - 2 * b - 10), R(fmaxf(r - 4, 0)), t.line);
  if ((e["screws"] | true) && b >= 8) {
    uint16_t sc = hexShade((const char *)(page["theme"]["metal"] | "#3a3a3d"), 0.9f);
    int xs[2] = {22, R(w) - 22}, ys[2] = {22, R(h) - 22};
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { spr->fillCircle(xs[i], ys[j], 3, sc); spr->drawCircle(xs[i], ys[j], 3, TFT_BLACK); }
  }
}

static void drawGauge(JsonObject e, float ox, float oy) {
  float w = e["w"] | 182.0f, h = e["h"] | 182.0f;
  float cx = ox + w / 2, cy = oy + h / 2, Rr = fminf(w, h) / 2 - 1;
  float mn = e["min"] | 0.0f, mx = e["max"] | 60.0f, start = e["start"] | -120.0f, sweep = e["sweep"] | 240.0f;
  float span = mx - mn; if (span == 0) span = 1;
  uint16_t tc = 0xFFFF, zc = 0xF800, lc = 0xFFFF;
  ec(e, "tcolor", tc); ec(e, "zcolor", zc); ec(e, "lcolor", lc);
  bool hasZone = !e["zone"].isNull() && (float)e["zone"] < mx;
  float zone = e["zone"] | 0.0f;
  if (e["arc"] | true) arcStroke(cx, cy, Rr, start, start + sweep, 1.5f, tc);
  if (hasZone) arcStroke(cx, cy, Rr, start + (zone - mn) / span * sweep, start + sweep, 3.5f, zc);
  float major = e["major"] | 15.0f, mid = e["mid"] | 5.0f, minor = e["minor"] | 1.0f;
  if (minor <= 0) minor = 1;
  int n = 0;
  for (float v = mn; v <= mx + 1e-4f && n < 400; v += minor, n++) {
    float rel = v - mn, ang = start + rel / span * sweep;
    bool isMaj = major > 0 && fabsf(rel / major - roundf(rel / major)) < 1e-4f;
    bool isMid = mid > 0 && fabsf(rel / mid - roundf(rel / mid)) < 1e-4f;
    float len = isMaj ? 14 : (isMid ? 9 : 5), sw = isMaj ? 3 : (isMid ? 2 : 1);
    float x0, y0, x1, y1;
    polar(cx, cy, Rr, ang, x0, y0); polar(cx, cy, Rr - len, ang, x1, y1);
    thickLine(x0, y0, x1, y1, sw, (hasZone && v >= zone - 1e-4f) ? zc : tc);
  }
  float ls = e["labelStep"] | 0.0f, labelIn = e["labelIn"] | 28.0f, lsize = e["lsize"] | 19.0f;
  bool ends = e["ends"] | true, p2 = e["pad2"] | true;
  n = 0;
  if (ls > 0) {
    for (float u = mn; u <= mx + 1e-4f && n < 60; u += ls, n++) {
      float ag = start + (u - mn) / span * sweep, px, py, fs = lsize;
      bool isEnd = fabsf(u - mn) < 1e-4f || fabsf(u - mx) < 1e-4f;
      if (ends && isEnd && sweep < 330) { polar(cx, cy, Rr, ag, px, py); py += 18; fs = lsize - 2; }
      else polar(cx, cy, Rr - labelIn, ag, px, py);
      char b[12];
      if (p2) snprintf(b, sizeof b, "%02d", (int)roundf(u)); else snprintf(b, sizeof b, "%d", (int)roundf(u));
      centerText(b, px, py, fs, (hasZone && u >= zone - 1e-4f) ? zc : lc);
    }
  }
}

static void rotPt(float cx, float cy, float px, float py, float ang, float &x, float &y) {
  float c = cosf(ang * DEG), s = sinf(ang * DEG), dx = px - cx, dy = py - cy;
  x = cx + dx * c - dy * s;
  y = cy + dx * s + dy * c;
}
static void tri(float cx, float cy, float ang, float ax, float ay, float bx, float by, float qx, float qy, uint16_t c) {
  float a1, a2, b1, b2, c1, c2;
  rotPt(cx, cy, ax, ay, ang, a1, a2); rotPt(cx, cy, bx, by, ang, b1, b2); rotPt(cx, cy, qx, qy, ang, c1, c2);
  spr->fillTriangle(R(a1), R(a2), R(b1), R(b2), R(c1), R(c2), c);
}
static void drawNeedle(JsonObject e, float ox, float oy, const Theme &t) {
  float w = e["w"] | 182.0f, h = e["h"] | 182.0f, cx = ox + w / 2, cy = oy + h / 2;
  float L = fminf(w, h) / 2 * (e["lenPct"] | 95.0f) / 100.0f;
  float mn = e["min"] | 0.0f, mx = e["max"] | 60.0f, start = e["start"] | -120.0f, sweep = e["sweep"] | 240.0f;
  float span = mx - mn; if (span == 0) span = 1;
  float v = numBind(e["bind"] | "fixe", e);
  float ang = start + constrain((v - mn) / span, 0.0f, 1.0f) * sweep;
  uint16_t col = 0xF800;
  ec(e, "color", col);
  const char *style = e["style"] | "bicolor";
  if (!strcmp(style, "fin")) {
    tri(cx, cy, ang, cx, cy - L, cx - 1.4f, cy, cx + 1.4f, cy, col);
    tri(cx, cy, ang, cx - 1.2f, cy, cx + 1.2f, cy, cx - 1.2f, cy + 14, col);
    tri(cx, cy, ang, cx + 1.2f, cy, cx + 1.2f, cy + 14, cx - 1.2f, cy + 14, col);
    float x, y; rotPt(cx, cy, cx, cy + 19, ang, x, y);
    spr->fillCircle(R(x), R(y), 4, col);
  } else if (!strcmp(style, "baton")) {
    float x, y; rotPt(cx, cy, cx, cy - L, ang, x, y);
    thickLine(cx, cy, x, y, 4, col);
  } else {
    uint16_t dark = col;
    const char *cs = e["color"] | "#a82328";
    dark = hexShade(cs, 0.7f);
    tri(cx, cy, ang, cx, cy - L, cx - 3.5f, cy, cx, cy + 14, dark);
    tri(cx, cy, ang, cx - 3.5f, cy, cx - 1.5f, cy + 14, cx, cy + 14, dark);
    tri(cx, cy, ang, cx, cy - L, cx + 3.5f, cy, cx, cy + 14, col);
    tri(cx, cy, ang, cx + 3.5f, cy, cx + 1.5f, cy + 14, cx, cy + 14, col);
  }
  if (e["hub"] | true) {
    if (!strcmp(style, "fin")) { spr->fillCircle(R(cx), R(cy), 8, t.metal); spr->drawCircle(R(cx), R(cy), 8, t.text); spr->fillCircle(R(cx), R(cy), 3, col); }
    else { spr->fillCircle(R(cx), R(cy), 9, col); spr->fillCircle(R(cx), R(cy), 3, t.bg); }
  }
}

static void drawBattery(JsonObject e, float ox, float oy) {
  float w = e["w"] | 208.0f, h = e["h"] | 208.0f;
  int n = max(1, (int)(e["count"] | 10)), filled = (int)roundf(constrain(D.batt, 0, 100) / 100.0f * n);
  uint16_t fc = 0xF800, ecol = 0x2104;
  ec(e, "fill", fc); ec(e, "empty", ecol);
  const char *shape = e["shape"] | "arc";
  if (!strcmp(shape, "bar")) {
    float gap = 3, sw = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) spr->fillRoundRect(R(ox + i * (sw + gap)), R(oy), R(sw), R(h), 2, i < filled ? fc : ecol);
    return;
  }
  float th = e["th"] | 10.0f, a0 = e["a0"] | 150.0f, a1 = e["a1"] | 210.0f;
  float cx = ox + w / 2, cy = oy + h / 2, rb = fminf(w, h) / 2 - th / 2, per = (a1 - a0) / n, gd = fminf(0.8f, per * 0.15f);
  for (int j = 0; j < n; j++) {
    float hi = a1 - j * per, lo = hi - per + gd;
    arcStroke(cx, cy, rb, lo, hi, th, j < filled ? fc : ecol);
  }
}

static void drawCounter(JsonObject e, float ox, float oy) {
  float w = e["w"] | 60.0f, h = e["h"] | 38.0f, size = e["size"] | 32.0f;
  String t = bindText(e["bind"] | "heure", e);
  uint16_t col = 0xFFFF, fill = 0, stroke = 0;
  ec(e, "color", col);
  bool hf = ec(e, "fill", fill), hs = ec(e, "stroke", stroke);
  if (!strcmp(e["style"] | "cadre", "clapets")) {
    String d = t; while (d.length() < 2) d = "0" + d;
    d = d.substring(d.length() - 2);
    float tw = (w - 4) / 2;
    for (int i = 0; i < 2; i++) {
      float x = ox + i * (tw + 4);
      if (hf) spr->fillRoundRect(R(x), R(oy), R(tw), R(h), 5, fill);
      if (hs) spr->drawRoundRect(R(x), R(oy), R(tw), R(h), 5, stroke);
      spr->drawFastHLine(R(x), R(oy + h / 2), R(tw), 0);
      centerText(String(d.charAt(i)), x + tw / 2, oy + h / 2 + 1, size, col);
    }
    return;
  }
  float r = e["r"] | 8.0f;
  if (hf) spr->fillRoundRect(R(ox), R(oy), R(w), R(h), R(r), fill);
  if (hs) { spr->drawRoundRect(R(ox), R(oy), R(w), R(h), R(r), stroke); spr->drawRoundRect(R(ox + 1), R(oy + 1), R(w - 2), R(h - 2), R(fmaxf(r - 1, 0)), stroke); }
  centerText(t, ox + w / 2, oy + h / 2 + 1, size, col);
}


/* ---------- Images (logos) : transparence, teinte, couleur animée ---------- */
struct ImgBuf { const char *key; uint8_t *buf; size_t n; };
static ImgBuf imgCache[8];
static void imgFlush() {
  for (auto &c : imgCache) { if (c.buf) free(c.buf); c.buf = nullptr; c.key = nullptr; c.n = 0; }
}
static const uint8_t *imgDecode(const char *b64, size_t &outLen) {
  for (auto &c : imgCache) if (c.key == b64 && c.buf) { outLen = c.n; return c.buf; }
  size_t sl = strlen(b64), cap = sl / 4 * 3 + 4, ol = 0;
  uint8_t *d = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!d) d = (uint8_t *)malloc(cap);
  if (!d) return nullptr;
  if (mbedtls_base64_decode(d, cap, &ol, (const unsigned char *)b64, sl) != 0) { free(d); return nullptr; }
  for (auto &c : imgCache) if (!c.buf) { c.key = b64; c.buf = d; c.n = ol; outLen = ol; return d; }
  free(imgCache[0].buf); imgCache[0] = {b64, d, ol}; outLen = ol; return d;   // cache plein : on remplace le premier
}
static uint16_t lerp565(uint16_t a, uint16_t b, float f) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31, br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (int)((br - ar) * f), g = ag + (int)((bg - ag) * f), bl = ab + (int)((bb - ab) * f);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}
static uint16_t imgColor(JsonObject e) {
  uint16_t c1 = 0xFFFF, c2, c3;
  ec(e, "c1", c1); c2 = c1; c3 = c1;
  ec(e, "c2", c2); ec(e, "c3", c3);
  const char *an = e["anim"] | "fixe";
  if (!strcmp(an, "fixe")) return c1;
  float per = e["period"] | 4.0f; if (per < 0.5f) per = 0.5f;
  unsigned long pm = (unsigned long)(per * 1000.0f);
  float t = (float)(millis() % pm) / (float)pm * 3.0f;
  int i = (int)t; if (i > 2) i = 2;
  uint16_t cs[3] = {c1, c2, c3};
  if (!strcmp(an, "cycle")) return cs[i];
  return lerp565(cs[i], cs[(i + 1) % 3], t - i);
}
static void drawImageEl(JsonObject e, const Theme &t) {
  float x = e["x"] | 0.0f, y = e["y"] | 0.0f, w = e["w"] | 0.0f, h = e["h"] | 0.0f;
  const char *b64 = e["data"] | "";
  const char *fmt = e["fmt"] | "c";
  int iw = e["iw"] | 0, ih = e["ih"] | 0;
  int bpp = fmt[0] == 'c' ? 3 : (fmt[0] == 'l' ? 2 : 1);
  size_t n = 0;
  const uint8_t *d = (b64[0] && iw > 0 && ih > 0) ? imgDecode(b64, n) : nullptr;
  if (!d || n < (size_t)iw * ih * bpp) { spr->drawRoundRect(R(x), R(y), R(w), R(h), 6, t.dim); return; }
  uint16_t col = fmt[0] == 'c' ? 0 : imgColor(e);
  int dw = R(w), dh = R(h), ox = R(x), oy = R(y);
  for (int py = 0; py < dh; py++) {
    int sy = oy + py; if (sy < 0 || sy > 239) continue;
    int iy = py * ih / dh;
    for (int px = 0; px < dw; px++) {
      int sx = ox + px; if (sx < 0 || sx > 239) continue;
      size_t k = (size_t)iy * iw + (px * iw / dw);
      uint8_t al; uint16_t fg;
      if (fmt[0] == 'c') { al = d[k * 3 + 2]; fg = ((uint16_t)d[k * 3] << 8) | d[k * 3 + 1]; }
      else if (fmt[0] == 'l') { al = d[k * 2 + 1]; fg = lerp565(0, col, d[k * 2] / 255.0f); }
      else { al = d[k]; fg = col; }
      if (al < 16) continue;
      if (al >= 240) spr->drawPixel(sx, sy, fg);
      else spr->drawPixel(sx, sy, spr->alphaBlend(al, fg, spr->readPixel(sx, sy)));
    }
  }
}

static void drawElement(JsonObject e, const Theme &t, JsonObject page) {
  const char *type = e["type"] | "";
  float x = e["x"] | 0.0f, y = e["y"] | 0.0f, w = e["w"] | 0.0f, h = e["h"] | 0.0f;
  uint16_t c1, c2;
  if (!strcmp(type, "frame")) drawFrame(e, t, page);
  else if (!strcmp(type, "gauge")) drawGauge(e, x, y);
  else if (!strcmp(type, "needle")) drawNeedle(e, x, y, t);
  else if (!strcmp(type, "battery")) drawBattery(e, x, y);
  else if (!strcmp(type, "counter")) drawCounter(e, x, y);
  else if (!strcmp(type, "text")) {
    uint16_t c = t.text; ec(e, "color", c);
    drawText(bindText(e["bind"] | "texte", e), x, y, w, h, e["anchor"] | "middle", e["size"] | 20.0f, c);
  } else if (!strcmp(type, "box")) {
    bool f = ec(e, "fill", c1), s = ec(e, "stroke", c2);
    float sw = e["sw"] | 1.5f;
    if (!strcmp(e["shape"] | "rect", "circle")) {
      if (f) spr->fillEllipse(R(x + w / 2), R(y + h / 2), R(w / 2), R(h / 2), c1);
      if (s) spr->drawEllipse(R(x + w / 2), R(y + h / 2), R(w / 2), R(h / 2), c2);
    } else {
      float r = e["r"] | 8.0f;
      if (f) spr->fillRoundRect(R(x), R(y), R(w), R(h), R(r), c1);
      if (s) { spr->drawRoundRect(R(x), R(y), R(w), R(h), R(r), c2); if (sw > 1.6f) spr->drawRoundRect(R(x + 1), R(y + 1), R(w - 2), R(h - 2), R(fmaxf(r - 1, 0)), c2); }
    }
  } else if (!strcmp(type, "line")) {
    uint16_t c = t.text; ec(e, "color", c);
    thickLine(x, y + h / 2, x + w, y + h / 2, e["sw"] | 2.0f, c);
  } else if (!strcmp(type, "bar")) {
    const char *bv = e["bindv"] | "fixe";
    float v = !strcmp(bv, "batterie") ? (float)D.batt : (float)(e["value"] | 40.0f);
    if (!strcmp(bv, "media")) v = (phoneFresh() && PH.trackS > 0) ? 100.0f * PH.posS / PH.trackS : 0;
    uint16_t fc = t.accent, ecol = t.track; ec(e, "fill", fc); ec(e, "empty", ecol);
    spr->fillRoundRect(R(x), R(y), R(w), R(h), R(h / 2), ecol);
    if (v > 0) spr->fillRoundRect(R(x), R(y), max(R(h), R(w * constrain(v, 0.0f, 100.0f) / 100.0f)), R(h), R(h / 2), fc);
  } else if (!strcmp(type, "wave")) {
    int n = max(1, (int)(e["bars"] | 9)); float bw = w / n; uint16_t c = t.accent; ec(e, "color", c);
    for (int i = 0; i < n; i++) { float k = fabsf(sinf((i + 1) * 1.3f)) * .8f + .2f, hh = h * k; spr->fillRoundRect(R(x + i * bw + bw * .2f), R(y + (h - hh) / 2), max(1, R(bw * .6f)), max(2, R(hh)), 1, c); }
  } else if (!strcmp(type, "icon")) {
    float ah = strlen(e["label"] | "") ? h - 16 : h;
    if (e["tile"] | true) {
      bool f = ec(e, "fill", c1), s = ec(e, "stroke", c2);
      float r = fminf(w, ah) * .24f;
      if (f) spr->fillRoundRect(R(x), R(y), R(w), R(ah), R(r), c1);
      if (s) { spr->drawRoundRect(R(x), R(y), R(w), R(ah), R(r), c2); spr->drawRoundRect(R(x + 1), R(y + 1), R(w - 2), R(ah - 2), R(r), c2); }
    }
    uint16_t col = t.text; ec(e, "color", col);
    float sz = fminf(w, ah) * ((e["tile"] | true) ? .52f : .9f);
    glyph(e["icon"] | "dots", x + w / 2, y + ah / 2, sz, col);
    if (strlen(e["label"] | "")) centerText(String((const char *)e["label"]), x + w / 2, y + h - 7, 11, t.text);
  } else if (!strcmp(type, "row")) {
    bool f = ec(e, "fill", c1), s = ec(e, "stroke", c2);
    if (f) spr->fillRoundRect(R(x), R(y), R(w), R(h), 7, c1);
    if (s) spr->drawRoundRect(R(x), R(y), R(w), R(h), 7, c2);
    uint16_t col = t.text; ec(e, "color", col);
    float is = fminf(h - 8, 18);
    glyph(e["icon"] | "sun", x + 9 + is / 2, y + h / 2, is, col);
    drawText(String((const char *)(e["text"] | "")), x + is + 18, y, w - is - 40, h, "start", e["size"] | 15.0f, col);
    glyph("chevron", x + w - 16, y + h / 2, 12, t.dim);
  } else if (!strcmp(type, "crypto")) {
    bool f = ec(e, "fill", c1), s = ec(e, "stroke", c2);
    if (f) spr->fillRoundRect(R(x), R(y), R(w), R(h), 10, c1);
    if (s) spr->drawRoundRect(R(x), R(y), R(w), R(h), 10, c2);
    uint16_t pc = t.accent; ec(e, "color", pc);
    spr->fillCircle(R(x + h / 2), R(y + h / 2), R(h * .34f), pc);
    String sym = String((const char *)(e["sym"] | "")), chg = String((const char *)(e["chg"] | ""));
    String price = String((const char *)(e["price"] | ""));
    const Cry *cr = phoneFresh() ? findCry(sym.c_str()) : nullptr;
    if (cr) { price = cr->price; chg = cr->chg; }
    if (sym.length()) centerText(sym.substring(0, 1), x + h / 2, y + h / 2 + 1, 18, t.bg);
    drawText(sym, x + h + 2, y + 2, 60, h * .45f, "start", 18, t.text);
    drawText(price, x + h + 2, y + h * .5f, 80, h * .45f, "start", 12, t.text);
    uint16_t cc = chg.startsWith("-") ? spr->color565(194, 90, 85) : spr->color565(74, 154, 98);
    drawText(chg, x + w * .6f - 60, y + h * .5f, 60, h * .45f, "end", 12, cc);
  } else if (!strcmp(type, "map")) {
    bool f = ec(e, "fill", c1), s = ec(e, "stroke", c2);
    if (f) spr->fillRoundRect(R(x), R(y), R(w), R(h), 8, c1);
    if (s) spr->drawRoundRect(R(x), R(y), R(w), R(h), 8, c2);
    for (int gx = 20; gx < w; gx += 20) spr->drawFastVLine(R(x + gx), R(y + 2), R(h - 4), t.dim);
    for (int gy = 20; gy < h; gy += 20) spr->drawFastHLine(R(x + 2), R(y + gy), R(w - 4), t.dim);
    uint16_t rc = t.accent; ec(e, "route", rc);
    float px[5] = {.2f, .2f, .52f, .58f, .85f}, py[5] = {.9f, .62f, .5f, .24f, .14f};
    for (int i = 0; i < 4; i++) thickLine(x + px[i] * w, y + py[i] * h, x + px[i + 1] * w, y + py[i + 1] * h, 3, rc);
    float ax = x + .2f * w, ay = y + .9f * h;
    spr->fillTriangle(R(ax), R(ay - 10), R(ax + 7), R(ay + 7), R(ax - 7), R(ay + 7), rc);
  } else if (!strcmp(type, "image")) {
    drawImageEl(e, t);
  }
}

static void renderPage(int idx) {
  JsonObject page = pages[idx];
  Theme t = loadTheme(page);
  spr->fillSprite(t.bg);
  for (JsonObject e : page["elements"].as<JsonArray>()) drawElement(e, t, page);
  spr->pushSprite(0, 0);
}

static void renderFallback() {
  spr->fillSprite(TFT_BLACK);
  spr->drawRoundRect(4, 4, 232, 232, 14, spr->color565(94, 26, 29));
  char b[8]; snprintf(b, sizeof b, "%02d:%02d", D.hour, D.minute);
  centerText(b, 120, 100, 40, TFT_WHITE);
  centerText("RNK Runtime", 120, 150, 18, spr->color565(168, 35, 40));
  centerText("Envoie un cadran depuis l'Atelier", 120, 180, 12, spr->color565(143, 140, 132));
  spr->pushSprite(0, 0);
}

/* ---------- Chargement du layout ---------- */
static bool loadLayout(const uint8_t *buf, size_t len) {
  DynamicJsonDocument *d = new DynamicJsonDocument(len * 3 + 16384);
  DeserializationError err = deserializeJson(*d, buf, len);
  if (err || !(*d)["pages"].is<JsonArray>() || (*d)["pages"].size() == 0) {
    Serial.printf("RNK:INFO json: %s\n", err.c_str());
    delete d;
    return false;
  }
  imgFlush();
  if (docPtr) delete docPtr;
  docPtr = d;
  pages = (*docPtr)["pages"].as<JsonArray>();
  pageCount = min((int)pages.size(), MAX_PAGES);
  for (int i = 0; i < MAX_PAGES; i++) { pageLive[i] = false; pageAnim[i] = false; }
  for (int i = 0; i < pageCount; i++)
    for (JsonObject e : pages[i]["elements"].as<JsonArray>()) {
      const char *b = e["bind"] | "";
      const char *bv = e["bindv"] | "";
      const char *ty = e["type"] | "";
      if (!strcmp(b, "seconde") || !strcmp(b, "speed") || !strcmp(b, "km") || !strcmp(b, "dur") || !strcmp(b, "kmh") ||
          !strcmp(b, "artiste") || !strcmp(b, "titre") || !strcmp(b, "tpos") || !strcmp(b, "tdur") ||
          !strcmp(bv, "media") || !strcmp(ty, "crypto")) pageLive[i] = true;
      if (!strcmp(ty, "image") && strcmp(e["tint"] | "none", "none") && strcmp(e["anim"] | "fixe", "fixe")) pageAnim[i] = true;
    }
  curPage = 0; lastCadran = 0;
  dirty = true;
  return true;
}
static bool loadFromFlash() {
  File f = SPIFFS.open(LAYOUT_PATH, "r");
  if (!f) return false;
  size_t n = f.size();
  if (n == 0 || n > MAX_LAYOUT) { f.close(); return false; }
  uint8_t *buf = (uint8_t *)ps_malloc(n + 1);
  if (!buf) buf = (uint8_t *)malloc(n + 1);
  if (!buf) { f.close(); return false; }
  f.read(buf, n); f.close();
  bool ok = loadLayout(buf, n);
  free(buf);
  return ok;
}

/* ---------- Série : réception des cadrans ---------- */
static uint32_t crc32Calc(const uint8_t *d, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
  }
  return ~crc;
}
enum RxState { RX_IDLE, RX_DATA, RX_TAIL };
static RxState rx = RX_IDLE;
static uint8_t *rxBuf = nullptr;
static size_t rxLen = 0, rxGot = 0;
static uint32_t rxCrc = 0;
static unsigned long rxLast = 0;
static String lineBuf;
static unsigned long lastActivity = 0;

static void rxReset() { if (rxBuf) { free(rxBuf); rxBuf = nullptr; } rx = RX_IDLE; rxGot = 0; rxLen = 0; }

static void handleCommand(const String &l) {
  if (l == "RNK:PING") { Serial.println("RNK:PONG " FW_VERSION); }
  else if (l == "RNK:INFO") {
    Serial.printf("RNK:INFO batt=%d%% usb=%d pages=%d heap=%u psram=%u\n", D.batt, D.usb, pageCount, ESP.getFreeHeap(), ESP.getFreePsram());
  } else if (l.startsWith("RNK:TIME ")) {
    int y, mo, d, h, mi, s;
    if (sscanf(l.c_str() + 9, "%d %d %d %d %d %d", &y, &mo, &d, &h, &mi, &s) == 6) {
      ttgo->rtc->setDateTime(y, mo, d, h, mi, s);
      ttgo->rtc->syncToSystem();
      dirty = true;
      Serial.println("RNK:OK time");
    } else Serial.println("RNK:ERR time");
  } else if (l.startsWith("RNK:BEGIN ")) {
    unsigned long n = 0; char crc[16] = {0};
    if (sscanf(l.c_str() + 10, "%lu %15s", &n, crc) == 2 && n > 0 && n <= MAX_LAYOUT) {
      rxReset();
      rxBuf = (uint8_t *)ps_malloc(n + 1);
      if (!rxBuf) rxBuf = (uint8_t *)malloc(n + 1);
      if (!rxBuf) { Serial.println("RNK:ERR mem"); return; }
      rxLen = n; rxGot = 0; rxCrc = strtoul(crc, nullptr, 16);
      rx = RX_DATA; rxLast = millis();
      Serial.println("RNK:READY");
    } else Serial.println("RNK:ERR begin");
  } else if (l == "RNK:END") {
    // géré dans RX_TAIL
  }
}

static void finishReceive() {
  uint32_t c = crc32Calc(rxBuf, rxLen);
  if (c != rxCrc) { Serial.println("RNK:ERR crc"); rxReset(); return; }
  if (!loadLayout(rxBuf, rxLen)) { Serial.println("RNK:ERR json"); rxReset(); return; }
  File f = SPIFFS.open(LAYOUT_PATH, "w");
  if (f) { f.write(rxBuf, rxLen); f.close(); }
  Serial.printf("RNK:OK %d\n", pageCount);
  rxReset();
}

static void serialPump() {
  while (Serial.available()) {
    int ch = Serial.read();
    lastActivity = millis();
    if (rx == RX_DATA) {
      rxBuf[rxGot++] = (uint8_t)ch;
      rxLast = millis();
      if (rxGot >= rxLen) { rxBuf[rxLen] = 0; rx = RX_TAIL; lineBuf = ""; }
      continue;
    }
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (rx == RX_TAIL) {
        if (lineBuf == "RNK:END") finishReceive();
        lineBuf = "";
      } else {
        handleCommand(lineBuf);
        lineBuf = "";
      }
    } else if (lineBuf.length() < 64) lineBuf += (char)ch;
  }
  if (rx != RX_IDLE && millis() - rxLast > RX_TIMEOUT_MS) { Serial.println("RNK:ERR timeout"); rxReset(); }
}

/* ---------- Bluetooth (BLE) ----------
 * Le téléphone écrit des lignes terminées par \n, champs séparés par | :
 *   S|vitesse km/h|distance km|durée s|moyenne km/h
 *   C|SYMBOLE|prix|variation
 *   M|artiste|titre|position s|durée s|lecture 0/1
 *   T|année|mois|jour|heure|min|sec
 * La montre notifie E|action quand on appuie sur une icône.
 */
static BLEServer *bleSrv = nullptr;
static BLECharacteristic *bleTx = nullptr;
static bool bleOn = false;
static String bleRx;
static String bleQ[8];
static volatile int bqHead = 0, bqTail = 0;

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s) { PH.link = true; PH.lastData = millis(); }
  void onDisconnect(BLEServer *s) { PH.link = false; BLEDevice::startAdvertising(); }
};
class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    auto v = c->getValue();
    const char *p = v.c_str();
    size_t n = v.length();
    for (size_t i = 0; i < n; i++) {
      char ch = p[i];
      if (ch == '\n') {
        int nx = (bqHead + 1) % 8;
        if (nx != bqTail) { bleQ[bqHead] = bleRx; bqHead = nx; }
        bleRx = "";
      } else if (ch != '\r' && bleRx.length() < 200) bleRx += ch;
    }
  }
};
static void bleStart() {
  if (bleOn) return;
  BLEDevice::init("RNK Watch");
  bleSrv = BLEDevice::createServer();
  bleSrv->setCallbacks(new SrvCb());
  BLEService *svc = bleSrv->createService(SVC_UUID);
  BLECharacteristic *rxc = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rxc->setCallbacks(new RxCb());
  bleTx = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  bleOn = true;
}
static void bleStop() {
  if (!bleOn) return;
  BLEDevice::deinit(false);   // false : la mémoire reste disponible pour un redémarrage du Bluetooth
  bleSrv = nullptr; bleTx = nullptr; bleOn = false; PH.link = false;
}
static void sendEvt(const char *a) {
  Serial.printf("RNK:EVT %s\n", a);
  if (bleOn && bleTx && PH.link) {
    String s = String("E|") + a + "\n";
    bleTx->setValue((uint8_t *)s.c_str(), s.length());
    bleTx->notify();
  }
}
static void processPhoneLine(const String &l) {
  String f[8];
  int n = splitBar(l, f, 8);
  if (n < 1 || f[0].length() < 1) return;
  char t = f[0].charAt(0);
  PH.lastData = millis(); PH.link = true;
  if (t == 'S' && n >= 5) { PH.speed = f[1].toFloat(); PH.km = f[2].toFloat(); PH.durS = f[3].toInt(); PH.avg = f[4].toFloat(); }
  else if (t == 'C' && n >= 4) {
    int k = -1;
    for (int i = 0; i < PH.ncry; i++) if (PH.cry[i].sym == f[1]) k = i;
    if (k < 0 && PH.ncry < 8) k = PH.ncry++;
    if (k >= 0) { PH.cry[k].sym = f[1]; PH.cry[k].price = f[2]; PH.cry[k].chg = f[3]; }
  } else if (t == 'M' && n >= 6) { PH.artist = f[1]; PH.title = f[2]; PH.posS = f[3].toInt(); PH.trackS = f[4].toInt(); PH.playing = f[5].toInt() != 0; }
  else if (t == 'T' && n >= 7) {
    ttgo->rtc->setDateTime(f[1].toInt(), f[2].toInt(), f[3].toInt(), f[4].toInt(), f[5].toInt(), f[6].toInt());
    ttgo->rtc->syncToSystem();
    dirty = true;
  }
}
static void blePump() {
  while (bqTail != bqHead) {
    String l = bleQ[bqTail];
    bqTail = (bqTail + 1) % 8;
    processPhoneLine(l);
  }
}

/* ---------- Navigation, actions, veille ---------- */
volatile bool pekIrq = false;
static void IRAM_ATTR onAxpIrq() { pekIrq = true; }

static int findPageByName(const char *name) {
  for (int i = 0; i < pageCount; i++) if (!strcmp(pages[i]["name"] | "", name)) return i;
  return -1;
}
static int firstAppPage() {
  for (int i = 0; i < pageCount; i++) if (!strcmp(pages[i]["kind"] | "", "appli")) return i;
  return -1;
}
static void gotoPage(int i) {
  if (i < 0 || i >= pageCount) return;
  curPage = i;
  if (!strcmp(pages[i]["kind"] | "", "cadran")) lastCadran = i;
  dirty = true;
}
static void nextCadran() {
  for (int k = 1; k <= pageCount; k++) {
    int i = (lastCadran + k) % pageCount;
    if (!strcmp(pages[i]["kind"] | "", "cadran")) { gotoPage(i); return; }
  }
}
static void toggleApps() {
  if (!strcmp(pages[curPage]["kind"] | "", "appli")) gotoPage(lastCadran);
  else { int a = firstAppPage(); if (a >= 0) gotoPage(a); }
}
static void doAction(const char *act, const char *target) {
  if (!strcmp(act, "goto")) gotoPage(findPageByName(target));
  else if (!strcmp(act, "bright")) { static const uint8_t lv[] = {40, 100, 160, 255}; int i = 0; while (i < 3 && lv[i] <= brightness) i++; brightness = lv[i]; ttgo->setBrightness(brightness); }
  else if (!strcmp(act, "sleep")) { lastActivity = 0; }
  else if (strcmp(act, "none")) sendEvt(act);   // play, next, prev, voice, bt, vibe : relayés au téléphone
}
static void onTap(int x, int y) {
  JsonArray els = pages[curPage]["elements"].as<JsonArray>();
  for (int i = (int)els.size() - 1; i >= 0; i--) {
    JsonObject e = els[i];
    const char *act = e["action"] | "none";
    if (!strcmp(act, "none")) continue;
    float ex = e["x"] | 0.0f, ey = e["y"] | 0.0f, ew = e["w"] | 0.0f, eh = e["h"] | 0.0f;
    if (x >= ex && x <= ex + ew && y >= ey && y <= ey + eh) { doAction(act, e["target"] | ""); return; }
  }
}
static bool touching = false;
static int tx0, ty0, tx1, ty1;
static unsigned long tUp = 0;
static void pollTouch() {
  int16_t x, y;
  if (digitalRead(TOUCH_INT) == LOW && ttgo->getTouch(x, y)) {
    if (!touching) { touching = true; tx0 = x; ty0 = y; }
    tx1 = x; ty1 = y; tUp = millis(); lastActivity = millis();
  } else if (touching && millis() - tUp > 80) {
    touching = false;
    int dx = tx1 - tx0, dy = ty1 - ty0;
    if (abs(dx) > 50 && abs(dy) < 70) {
      if (dx > 0) { if (!strcmp(pages[curPage]["kind"] | "", "appli")) gotoPage(lastCadran); else nextCadran(); }
      else toggleApps();
    } else if (abs(dx) < 12 && abs(dy) < 12) onTap(tx1, ty1);
  }
}
static void readClock() {
  RTC_Date t = ttgo->rtc->getDateTime();
  D.hour = t.hour; D.minute = t.minute; D.second = t.second; D.day = t.day; D.month = constrain(t.month, 1, 12); D.year = t.year;
  D.wday = dayOfWeek(D.year, D.month, D.day);
}
static void goSleep() {
  bleStop();
  ttgo->closeBL();
  ttgo->displaySleep();
  pekIrq = false;
  ttgo->power->clearIRQ();
  gpio_wakeup_enable((gpio_num_t)AXP202_INT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  // réveil
  ttgo->power->readIRQ();
  ttgo->power->clearIRQ();
  pekIrq = false;
  ttgo->displayWakeup();
  ttgo->rtc->syncToSystem();
  ttgo->openBL();
  ttgo->setBrightness(brightness);
  lastActivity = millis();
  dirty = true;
  bleStart();
}

/* ---------- Programme ---------- */
void setup() {
  Serial.begin(115200);
  ttgo = TTGOClass::getWatch();
  ttgo->begin();
  ttgo->openBL();
  ttgo->setBrightness(brightness);
  ttgo->power->adc1Enable(AXP202_BATT_VOL_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1, AXP202_ON);
  ttgo->power->enableIRQ(AXP202_PEK_SHORTPRESS_IRQ, true);
  ttgo->power->clearIRQ();
  pinMode(AXP202_INT, INPUT_PULLUP);
  attachInterrupt(AXP202_INT, onAxpIrq, FALLING);
  pinMode(TOUCH_INT, INPUT);

  spr = new TFT_eSprite(ttgo->tft);
  spr->setColorDepth(16);
  spr->createSprite(240, 240);

  SPIFFS.begin(true);
  if (!loadFromFlash()) Serial.println("RNK:INFO aucun layout en memoire");
  readClock();
  D.batt = ttgo->power->getBattPercentage();
  D.usb = ttgo->power->isVBUSPlug();
  lastActivity = millis();
  bleStart();
  Serial.println("RNK:PONG " FW_VERSION);
}

void loop() {
  static int lastSec = -1, lastMin = -1, lastBatt = -1, lastPage = -1;
  static unsigned long lastBattRead = 0, lastAnim = 0;
  serialPump();
  blePump();
  if (rx == RX_IDLE && pageCount > 0) pollTouch();

  if (pekIrq) {
    pekIrq = false;
    ttgo->power->readIRQ();
    if (ttgo->power->isPEKShortPressIRQ() && pageCount > 0) toggleApps();
    ttgo->power->clearIRQ();
    lastActivity = millis();
  }

  if (millis() - lastBattRead > 15000 || lastBattRead == 0) {
    lastBattRead = millis();
    D.batt = ttgo->power->getBattPercentage();
    D.usb = ttgo->power->isVBUSPlug();
  }
  readClock();
  bool need = dirty || curPage != lastPage || D.minute != lastMin || D.batt != lastBatt;
  if (!need && pageCount > 0 && pageLive[curPage] && D.second != lastSec) need = true;
  if (!need && pageCount > 0 && pageAnim[curPage] && millis() - lastAnim >= 120) need = true;
  if (need) lastAnim = millis();
  if (need && rx == RX_IDLE) {
    if (pageCount > 0) renderPage(curPage); else renderFallback();
    lastSec = D.second; lastMin = D.minute; lastBatt = D.batt; lastPage = curPage; dirty = false;
  }

  if (!D.usb && rx == RX_IDLE && millis() - lastActivity > SLEEP_AFTER_MS) goSleep();
  delay(20);
}
