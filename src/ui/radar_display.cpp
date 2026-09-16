#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "ui/night_mode.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorSweep = 0x07E0;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagCallsign = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

/** Safety margin around tracked boxes to cover anti-aliased edges. */
constexpr int kDirtyPadPx = 2;

struct DirtyRect {
  int16_t l;
  int16_t t;
  int16_t r;
  int16_t b;
};

/** Bounded, non-overlapping set of screen boxes touched by the dynamic layer. */
class DirtyRegion {
 public:
  void clear() { count_ = 0; }
  size_t count() const { return count_; }
  const DirtyRect& at(size_t i) const { return rects_[i]; }

  void add(int l, int t, int r, int b) {
    DirtyRect box;
    box.l = static_cast<int16_t>(std::max(0, l - kDirtyPadPx));
    box.t = static_cast<int16_t>(std::max(0, t - kDirtyPadPx));
    box.r = static_cast<int16_t>(std::min(radar::kSize - 1, r + kDirtyPadPx));
    box.b = static_cast<int16_t>(std::min(radar::kSize - 1, b + kDirtyPadPx));
    if (box.l > box.r || box.t > box.b) {
      return;
    }

    absorbOverlaps(&box);
    if (count_ < kMaxRects) {
      rects_[count_++] = box;
      return;
    }
    mergeInto(&rects_[cheapestMergeIndex(box)], box);
  }

  void addAll(const DirtyRegion& other) {
    for (size_t i = 0; i < other.count_; ++i) {
      const DirtyRect& r = other.rects_[i];
      add(r.l, r.t, r.r, r.b);
    }
  }

 private:
  static constexpr size_t kMaxRects = 10;

  static int rectArea(const DirtyRect& r) {
    return (r.r - r.l + 1) * (r.b - r.t + 1);
  }

  static bool touches(const DirtyRect& a, const DirtyRect& b) {
    return a.l <= b.r + 1 && b.l <= a.r + 1 && a.t <= b.b + 1 && b.t <= a.b + 1;
  }

  static void mergeInto(DirtyRect* dst, const DirtyRect& src) {
    dst->l = std::min(dst->l, src.l);
    dst->t = std::min(dst->t, src.t);
    dst->r = std::max(dst->r, src.r);
    dst->b = std::max(dst->b, src.b);
  }

  static int unionArea(const DirtyRect& a, const DirtyRect& b) {
    DirtyRect u = a;
    mergeInto(&u, b);
    return rectArea(u);
  }

  void removeAt(size_t i) { rects_[i] = rects_[--count_]; }

  void absorbOverlaps(DirtyRect* box) {
    for (size_t i = 0; i < count_;) {
      if (touches(rects_[i], *box)) {
        mergeInto(box, rects_[i]);
        removeAt(i);
        i = 0;  // the grown box may now touch a rect checked earlier
        continue;
      }
      ++i;
    }
  }

  size_t cheapestMergeIndex(const DirtyRect& box) const {
    size_t best = 0;
    int best_cost = unionArea(rects_[0], box) - rectArea(rects_[0]);
    for (size_t i = 1; i < count_; ++i) {
      const int cost = unionArea(rects_[i], box) - rectArea(rects_[i]);
      if (cost < best_cost) {
        best_cost = cost;
        best = i;
      }
    }
    return best;
  }

  DirtyRect rects_[kMaxRects];
  size_t count_ = 0;
};

DirtyRegion s_dirty_current;
DirtyRegion s_dirty_previous;
DirtyRegion s_sweep_current;
DirtyRegion s_sweep_previous;
DirtyRegion* s_dirty_sink = nullptr;
bool s_panel_matches_frame = false;
unsigned long s_last_sweep_ms = 0;

void markDirty(int l, int t, int r, int b) {
  if (s_dirty_sink != nullptr) {
    s_dirty_sink->add(l, t, r, b);
  }
}

/** Screen box used both for text layout and for the shapes text should dodge. */
struct TagBox {
  int16_t l;
  int16_t t;
  int16_t r;
  int16_t b;
};

constexpr uint8_t kNoSymbolOwner = 0xFF;

/** Aircraft symbols and track vectors, dodged by tag text whenever room allows. */
class SymbolObstacles {
 public:
  void clear() {
    box_count_ = 0;
    segment_count_ = 0;
  }

  void addSymbol(uint8_t owner, int l, int t, int r, int b) {
    if (box_count_ >= kMaxBoxes) {
      return;
    }
    boxes_[box_count_].owner = owner;
    boxes_[box_count_].box.l = static_cast<int16_t>(l);
    boxes_[box_count_].box.t = static_cast<int16_t>(t);
    boxes_[box_count_].box.r = static_cast<int16_t>(r);
    boxes_[box_count_].box.b = static_cast<int16_t>(b);
    ++box_count_;
  }

  void addVector(int x0, int y0, int x1, int y1) {
    if (segment_count_ >= kMaxSegments) {
      return;
    }
    segments_[segment_count_].x0 = static_cast<int16_t>(x0);
    segments_[segment_count_].y0 = static_cast<int16_t>(y0);
    segments_[segment_count_].x1 = static_cast<int16_t>(x1);
    segments_[segment_count_].y1 = static_cast<int16_t>(y1);
    ++segment_count_;
  }

  /** True when the box hits a track vector or any symbol other than owner's. */
  bool blocks(const TagBox& box, uint8_t owner) const {
    const int l = box.l - kPadPx;
    const int t = box.t - kPadPx;
    const int r = box.r + kPadPx;
    const int b = box.b + kPadPx;

    for (size_t i = 0; i < box_count_; ++i) {
      if (boxes_[i].owner == owner) {
        continue;  // a tag may touch the symbol it belongs to
      }
      const TagBox& o = boxes_[i].box;
      if (l <= o.r && o.l <= r && t <= o.b && o.t <= b) {
        return true;
      }
    }
    for (size_t i = 0; i < segment_count_; ++i) {
      if (segmentHitsBox(segments_[i], l, t, r, b)) {
        return true;
      }
    }
    return false;
  }

 private:
  static constexpr size_t kMaxBoxes = services::adsb::kMaxAircraft;
  static constexpr size_t kMaxSegments = services::adsb::kMaxAircraft;
  /** Covers the stroke half-width of the drawn shapes. */
  static constexpr int kPadPx = 2;

  struct OwnedBox {
    TagBox box;
    uint8_t owner;
  };

  struct Segment {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
  };

  static bool segmentHitsBox(const Segment& s, int l, int t, int r, int b) {
    if (std::max(s.x0, s.x1) < l || std::min(s.x0, s.x1) > r ||
        std::max(s.y0, s.y1) < t || std::min(s.y0, s.y1) > b) {
      return false;
    }

    // Liang-Barsky: clip the segment against the box slabs.
    const float dx = static_cast<float>(s.x1 - s.x0);
    const float dy = static_cast<float>(s.y1 - s.y0);
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {static_cast<float>(s.x0 - l),
                        static_cast<float>(r - s.x0),
                        static_cast<float>(s.y0 - t),
                        static_cast<float>(b - s.y0)};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int i = 0; i < 4; ++i) {
      if (p[i] == 0.0f) {
        if (q[i] < 0.0f) {
          return false;
        }
        continue;
      }
      const float u = q[i] / p[i];
      if (p[i] < 0.0f) {
        if (u > leave) {
          return false;
        }
        enter = std::max(enter, u);
      } else {
        if (u < enter) {
          return false;
        }
        leave = std::min(leave, u);
      }
    }
    return enter <= leave;
  }

  OwnedBox boxes_[kMaxBoxes];
  Segment segments_[kMaxSegments];
  size_t box_count_ = 0;
  size_t segment_count_ = 0;
};

SymbolObstacles s_symbols;

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {&fonts::FreeSansBold12pt7b,
                                                  &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {&fonts::FreeSansBold9pt7b,
                                               &fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {&fonts::FreeSansBold12pt7b,
                                               &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

struct NightRgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

/** Red matrix ramp: grid darkest, symbol/tags mid, speed vector palest. */
constexpr NightRgb kNightBackground{0, 0, 0};
constexpr NightRgb kNightGrid{72, 0, 0};
constexpr NightRgb kNightLabel{120, 10, 10};
constexpr NightRgb kNightCenter{150, 22, 22};
constexpr NightRgb kNightAircraft{195, 40, 40};
constexpr NightRgb kNightTrack{255, 112, 112};
constexpr NightRgb kNightRunway{60, 0, 0};
constexpr NightRgb kNightRunwayLabel{104, 8, 8};
constexpr NightRgb kNightSweep{150, 16, 16};
constexpr uint8_t kNightDimPercent = 40;

radar::NightStyle s_night_style = radar::NightStyle::kNone;
bool s_palette_changed = false;

/** Channel order as the panel wants it, so the sweep trail can be blended. */
struct PanelRgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

PanelRgb s_sweep_panel_rgb{40, 220, 90};
PanelRgb s_background_panel_rgb{0, 0, 0};

uint8_t mixChannel(uint8_t from, uint8_t to, float f) {
  const int delta = static_cast<int>(to) - static_cast<int>(from);
  return static_cast<uint8_t>(static_cast<int>(from) + static_cast<int>(delta * f));
}

uint16_t blendPanelRgb(const PanelRgb& from, const PanelRgb& to, float f) {
  return tft.color565(mixChannel(from.r, to.r, f), mixChannel(from.g, to.g, f),
                      mixChannel(from.b, to.b, f));
}

uint8_t scaleChannel(uint8_t value, uint8_t percent) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) * percent) / 100);
}

PanelRgb scaledPanelRgb(uint8_t r, uint8_t g, uint8_t b, uint8_t percent, bool swap_rb) {
  const uint8_t sr = scaleChannel(r, percent);
  const uint8_t sg = scaleChannel(g, percent);
  const uint8_t sb = scaleChannel(b, percent);
  return swap_rb ? PanelRgb{sb, sg, sr} : PanelRgb{sr, sg, sb};
}

uint16_t dayColor(uint8_t r, uint8_t g, uint8_t b, uint8_t percent) {
  return tft.color565(scaleChannel(r, percent), scaleChannel(g, percent),
                      scaleChannel(b, percent));
}

/** Night tones are authored in true RGB, so undo the panel's BGR order here. */
uint16_t nightColor(const NightRgb& c, uint8_t percent) {
  const uint8_t r = scaleChannel(c.r, percent);
  const uint8_t g = scaleChannel(c.g, percent);
  const uint8_t b = scaleChannel(c.b, percent);
  return config::kDisplayRgbOrder ? tft.color565(b, g, r) : tft.color565(r, g, b);
}

void initPalette() {
  const radar::NightStyle style = radar::nightStyle();
  if (style != s_night_style) {
    s_night_style = style;
    s_palette_changed = true;
  }
  const uint8_t pct =
      (style == radar::NightStyle::kDim) ? kNightDimPercent : 100;

  if (style == radar::NightStyle::kRed) {
    radar::kColorBackground = nightColor(kNightBackground, pct);
    radar::kColorGrid = nightColor(kNightGrid, pct);
    radar::kColorSweep = nightColor(kNightSweep, pct);
    s_sweep_panel_rgb =
        scaledPanelRgb(kNightSweep.r, kNightSweep.g, kNightSweep.b, pct, true);
    s_background_panel_rgb = scaledPanelRgb(kNightBackground.r, kNightBackground.g,
                                            kNightBackground.b, pct, true);
    radar::kColorLabel = nightColor(kNightLabel, pct);
    radar::kColorCenter = nightColor(kNightCenter, pct);
    // Symbol and its tag text share one tone; the speed vector is the palest.
    radar::kColorAircraft = nightColor(kNightAircraft, pct);
    radar::kColorTagCallsign = nightColor(kNightAircraft, pct);
    radar::kColorTagType = nightColor(kNightAircraft, pct);
    radar::kColorTagAltitude = nightColor(kNightAircraft, pct);
    radar::kColorTrackVector = nightColor(kNightTrack, pct);
    radar::kColorRunway = nightColor(kNightRunway, pct);
    radar::kColorRunwayLabel = nightColor(kNightRunwayLabel, pct);
    return;
  }

  radar::kColorBackground = dayColor(radar::kBgR, radar::kBgG, radar::kBgB, pct);
  radar::kColorGrid = dayColor(radar::kGridR, radar::kGridG, radar::kGridB, pct);
  radar::kColorSweep = dayColor(radar::kSweepR, radar::kSweepG, radar::kSweepB, pct);
  s_sweep_panel_rgb =
      scaledPanelRgb(radar::kSweepR, radar::kSweepG, radar::kSweepB, pct, false);
  s_background_panel_rgb =
      scaledPanelRgb(radar::kBgR, radar::kBgG, radar::kBgB, pct, false);
  radar::kColorLabel = dayColor(255, 255, 255, pct);
  radar::kColorCenter = dayColor(255, 255, 255, pct);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        dayColor(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR, pct);
  } else {
    radar::kColorAircraft =
        dayColor(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB, pct);
  }
  radar::kColorTrackVector =
      dayColor(radar::kTrackR, radar::kTrackG, radar::kTrackB, pct);
  radar::kColorTagCallsign = dayColor(255, 255, 255, pct);
  radar::kColorTagType =
      dayColor(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB, pct);
  radar::kColorTagAltitude =
      dayColor(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB, pct);
  radar::kColorRunway =
      dayColor(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB, pct);
  radar::kColorRunwayLabel = dayColor(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                      radar::kRunwayLabelB, pct);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  const int r = radar::kBeyondRingDotRadiusPx;
  markDirty(x - r, y - r, x + r, y + r);
  s_symbols.addSymbol(kNoSymbolOwner, x - r, y - r, x + r, y + r);
  s_draw->fillSmoothCircle(x, y, r, radar::kColorAircraft);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color,
                        uint8_t owner) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  const int xs[] = {tip_x, base_x + wing_x, base_x - wing_x};
  const int ys[] = {tip_y, base_y + wing_y, base_y - wing_y};
  const int min_x = *std::min_element(xs, xs + 3);
  const int min_y = *std::min_element(ys, ys + 3);
  const int max_x = *std::max_element(xs, xs + 3);
  const int max_y = *std::max_element(ys, ys + 3);
  markDirty(min_x, min_y, max_x, max_y);
  s_symbols.addSymbol(owner, min_x, min_y, max_x, max_y);

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }

  const int half =
      static_cast<int>(radar::kAircraftTrackLineHalfWidth) + 1;
  markDirty(std::min(tip_x, ex) - half, std::min(tip_y, ey) - half,
            std::max(tip_x, ex) + half, std::max(tip_y, ey) + half);
  s_symbols.addVector(tip_x, tip_y, ex, ey);

  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_tag_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  if (plane.callsign[0] != '\0') {
    const int w = s_draw->textWidth(plane.callsign);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

/** Screen box reserved by one text block. */
struct TagPlacement {
  TagBox box;
  bool text_right_aligned = false;
};

/** Empty space kept between neighbouring text blocks (px). */
constexpr int kTagSeparationPx = 2;
/** Anchors are probed all around the blip: headings tried per ring. */
constexpr int kTagAngleSteps = 16;
/** Rings pushed progressively further out when every heading is taken. */
constexpr int kTagRingCount = 4;
constexpr int kTagRingStepPx = 12;
constexpr uint16_t kTagCandidateCount = kTagAngleSteps * kTagRingCount;
constexpr uint16_t kNoTagCandidate = 0xFFFF;

/** Keeps each aircraft's chosen anchor across polls so tags stop flipping. */
class TagPlacementMemory {
 public:
  /** Marks a new layout pass; used to age out entries when the table is full. */
  void beginPass() { ++pass_; }

  uint16_t recall(const char* hex) const {
    const size_t i = find(hex);
    return i < count_ ? entries_[i].candidate : kNoTagCandidate;
  }

  void remember(const char* hex, uint16_t candidate) {
    if (hex == nullptr || hex[0] == '\0') {
      return;
    }
    size_t i = find(hex);
    if (i >= count_) {
      i = (count_ < kMaxEntries) ? count_++ : stalestIndex();
      strncpy(entries_[i].hex, hex, sizeof(entries_[i].hex) - 1);
      entries_[i].hex[sizeof(entries_[i].hex) - 1] = '\0';
    }
    entries_[i].candidate = candidate;
    entries_[i].pass = pass_;
  }

 private:
  static constexpr size_t kMaxEntries = services::adsb::kMaxAircraft;

  struct Entry {
    char hex[8];
    uint16_t candidate;
    uint32_t pass;
  };

  size_t find(const char* hex) const {
    if (hex == nullptr || hex[0] == '\0') {
      return kMaxEntries;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (strcmp(entries_[i].hex, hex) == 0) {
        return i;
      }
    }
    return kMaxEntries;
  }

  size_t stalestIndex() const {
    size_t best = 0;
    for (size_t i = 1; i < count_; ++i) {
      if (entries_[i].pass < entries_[best].pass) {
        best = i;
      }
    }
    return best;
  }

  Entry entries_[kMaxEntries];
  size_t count_ = 0;
  uint32_t pass_ = 0;
};

/** Hands out text boxes that clear every box already placed this pass. */
class TagLayout {
 public:
  void beginPass() {
    count_ = 0;
    memory_.beginPass();
  }

  /** Picks the first free anchor around the blip, scanning 360 degrees. */
  TagPlacement place(const char* hex, uint8_t owner, int x, int y,
                     int block_w, int block_h) {
    // First pass also dodges symbols and track vectors; second keeps text only.
    for (int pass = 0; pass < 2; ++pass) {
      const bool dodge_symbols = pass == 0;
      const uint16_t remembered = memory_.recall(hex);
      if (remembered != kNoTagCandidate) {
        const TagPlacement kept =
            toPlacement(remembered, x, y, block_w, block_h);
        if (fits(kept.box, owner, dodge_symbols)) {
          return reserve(kept);
        }
      }

      for (uint16_t c = 0; c < kTagCandidateCount; ++c) {
        const TagPlacement p = toPlacement(c, x, y, block_w, block_h);
        if (!fits(p.box, owner, dodge_symbols)) {
          continue;
        }
        memory_.remember(hex, c);
        return reserve(p);
      }
    }

    // Nothing free anywhere: stay on screen and accept the overlap.
    TagPlacement p = toPlacement(0, x, y, block_w, block_h);
    clampOnScreen(&p.box);
    memory_.remember(hex, 0);
    return reserve(p);
  }

 private:
  static constexpr size_t kMaxBoxes = services::adsb::kMaxAircraft;
  static constexpr int kScreenMarginPx = 1;

  /** Heading of the blip-to-center direction; tags start out pointing inward. */
  static float baseAngle(int x, int y) {
    const float dx = static_cast<float>(radar::kCenterX - x);
    const float dy = static_cast<float>(radar::kCenterY - y);
    if (dx == 0.0f && dy == 0.0f) {
      return 0.0f;
    }
    return atan2f(dy, dx);
  }

  /** Rank 0 is the base heading, then alternating steps either side of it. */
  static float angleForRank(int rank, float base) {
    constexpr float kStep = 6.283185307f / static_cast<float>(kTagAngleSteps);
    const int step = (rank + 1) / 2;
    const float sign = (rank % 2) != 0 ? 1.0f : -1.0f;
    return base + sign * static_cast<float>(step) * kStep;
  }

  static TagPlacement toPlacement(uint16_t candidate, int x, int y,
                                  int block_w, int block_h) {
    const int ring = candidate / kTagAngleSteps;
    const int rank = candidate % kTagAngleSteps;
    const float angle = angleForRank(rank, baseAngle(x, y));
    const float ca = cosf(angle);
    const float sa = sinf(angle);

    // Push the block out far enough that its edge clears the aircraft symbol.
    const float clearance =
        static_cast<float>(radar::kAircraftNoseLenPx +
                           radar::kAircraftTailHalfPx +
                           radar::kAircraftLabelGapPx + ring * kTagRingStepPx);
    const int mid_x =
        x + static_cast<int>(lroundf(ca * (clearance + block_w * 0.5f)));
    const int mid_y =
        y + static_cast<int>(lroundf(sa * (clearance + block_h * 0.5f)));

    TagPlacement p;
    p.box.l = static_cast<int16_t>(mid_x - block_w / 2);
    p.box.t = static_cast<int16_t>(mid_y - block_h / 2);
    p.box.r = static_cast<int16_t>(p.box.l + block_w);
    p.box.b = static_cast<int16_t>(p.box.t + block_h);
    p.text_right_aligned = ca < 0.0f;
    return p;
  }

  static bool isOnScreen(const TagBox& box) {
    return box.l >= kScreenMarginPx && box.t >= kScreenMarginPx &&
           box.r <= radar::kSize - 1 - kScreenMarginPx &&
           box.b <= radar::kSize - 1 - kScreenMarginPx;
  }

  static void clampOnScreen(TagBox* box) {
    const int w = box->r - box->l;
    const int h = box->b - box->t;
    const int max_l = radar::kSize - 1 - kScreenMarginPx - w;
    const int max_t = radar::kSize - 1 - kScreenMarginPx - h;
    box->l = static_cast<int16_t>(
        std::max(kScreenMarginPx, std::min<int>(box->l, max_l)));
    box->t = static_cast<int16_t>(
        std::max(kScreenMarginPx, std::min<int>(box->t, max_t)));
    box->r = static_cast<int16_t>(box->l + w);
    box->b = static_cast<int16_t>(box->t + h);
  }

  bool fits(const TagBox& box, uint8_t owner, bool dodge_symbols) const {
    if (!isOnScreen(box) || !isFree(box)) {
      return false;
    }
    return !dodge_symbols || !s_symbols.blocks(box, owner);
  }

  bool isFree(const TagBox& box) const {
    for (size_t i = 0; i < count_; ++i) {
      const TagBox& o = boxes_[i];
      if (box.l <= o.r + kTagSeparationPx && o.l <= box.r + kTagSeparationPx &&
          box.t <= o.b + kTagSeparationPx && o.t <= box.b + kTagSeparationPx) {
        return false;
      }
    }
    return true;
  }

  void reserveBox(const TagBox& box) {
    if (count_ < kMaxBoxes) {
      boxes_[count_++] = box;
    }
  }

  TagPlacement reserve(const TagPlacement& p) {
    reserveBox(p.box);
    return p;
  }

  TagPlacementMemory memory_;
  TagBox boxes_[kMaxBoxes];
  size_t count_ = 0;
};

TagLayout s_tag_layout;

void drawAircraftTag(const TagPlacement& placement,
                     const services::adsb::Aircraft& plane) {
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  markDirty(placement.box.l, placement.box.t, placement.box.r,
            placement.box.b);

  const int anchor_x =
      placement.text_right_aligned ? placement.box.r : placement.box.l;
  s_draw->setTextDatum(placement.text_right_aligned ? textdatum_t::top_right
                                                    : textdatum_t::top_left);
  int ly = placement.box.t;

  if (plane.callsign[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagCallsign, radar::kColorBackground);
    s_draw->drawString(plane.callsign, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0') {
    s_draw->setTextColor(radar::kColorTagAltitude, radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  TagPlacement tag;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();
  s_symbols.clear();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    drawBeyondRingDot(dots[d].x, dots[d].y);
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                    planes[i].gs_knots, radar::kColorTrackVector);
    drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft,
                        static_cast<uint8_t>(i));
  }

  initTagLabelMetrics();
  applyTagStyle();
  const int tag_block_h = s_draw->fontHeight() * 3;
  s_tag_layout.beginPass();
  // Lay out nearest first so close traffic keeps the preferred anchor.
  for (size_t d = draw_count; d > 0; --d) {
    AircraftDrawItem& item = items[d - 1];
    const services::adsb::Aircraft& plane = planes[item.index];
    item.tag = s_tag_layout.place(plane.hex, static_cast<uint8_t>(item.index),
                                  item.x, item.y, measureTagBlockWidth(plane),
                                  tag_block_h);
  }

  for (size_t d = 0; d < draw_count; ++d) {
    drawAircraftTag(items[d].tag, planes[items[d].index]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_cardinal_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw, s_scale_vlw_size);
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  for (int i = 0; i < thickness && r - i > 0; ++i) {
    s_draw->drawCircle(cx, cy, r - i, color);
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

/** Bounding box of a wedge: the centre plus samples along its outer arc. */
void markSweepDirty(int cx, int cy, int radius, float start_deg, float end_deg) {
  constexpr int kSamples = 12;
  int l = cx;
  int t = cy;
  int r = cx;
  int b = cy;
  for (int i = 0; i <= kSamples; ++i) {
    const float deg =
        start_deg + (end_deg - start_deg) * (static_cast<float>(i) / kSamples);
    const float rad = (deg - 90.0f) * kDegToRad;
    const int x = cx + static_cast<int>(cosf(rad) * radius);
    const int y = cy + static_cast<int>(sinf(rad) * radius);
    l = std::min(l, x);
    t = std::min(t, y);
    r = std::max(r, x);
    b = std::max(b, y);
  }
  markDirty(l, t, r, b);
}

// Drawn before the grid so rings, runways and labels stay readable on top of it.
void drawSweep(int cx, int cy, int radius) {
  if (!radar::sweepEnabled()) {
    return;
  }

  const unsigned long phase = millis() % radar::kSweepPeriodMs;
  const float head_deg = static_cast<float>(phase) * 360.0f /
                         static_cast<float>(radar::kSweepPeriodMs);
  const float slice_deg =
      radar::kSweepTrailDeg / static_cast<float>(radar::kSweepTrailSlices);

  for (int i = 0; i < radar::kSweepTrailSlices; ++i) {
    const float fade = 1.0f - static_cast<float>(i) / radar::kSweepTrailSlices;
    const float level = radar::kSweepTrailPeak * fade * fade;
    const float trailing = head_deg - static_cast<float>(i) * slice_deg;
    s_draw->fillArc(cx, cy, 0, radius, trailing - slice_deg - 90.0f, trailing - 90.0f,
                    blendPanelRgb(s_background_panel_rgb, s_sweep_panel_rgb, level));
  }

  const float head_rad = (head_deg - 90.0f) * kDegToRad;
  s_draw->drawWideLine(cx, cy, cx + static_cast<int>(cosf(head_rad) * radius),
                       cy + static_cast<int>(sinf(head_rad) * radius),
                       radar::kSweepLineHalfWidth, radar::kColorSweep);

  markSweepDirty(cx, cy, radius, head_deg - radar::kSweepTrailDeg, head_deg);
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawSweep(cx, cy, grid_r);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  initPalette();
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  // A full-screen buffer is required: LovyanGFX resets the clip rect inside
  // some primitives, so a partial buffer would be written out of bounds.
  // 16bpp is preferred; 8bpp halves the block when DRAM is too fragmented.
  for (const uint8_t bpp : {uint8_t{16}, uint8_t{8}}) {
    s_frame.setColorDepth(bpp);
    if (s_frame.createSprite(radar::kSize, radar::kSize) != nullptr) {
      s_frame_ready = true;
      return true;
    }
  }
  return false;
}

void pushRegion(const DirtyRegion& region) {
  for (size_t i = 0; i < region.count(); ++i) {
    const DirtyRect& r = region.at(i);
    tft.setClipRect(r.l, r.t, r.r - r.l + 1, r.b - r.t + 1);
    s_frame.pushSprite(0, 0);
  }
  tft.clearClipRect();
}

// The whole frame is composed off-screen, but only the boxes touched this frame
// or the previous one are blitted, so the static grid is never re-sent over SPI.
// Sweep-only frames keep the aircraft boxes out of the push: the planes have not
// moved, and the pixels the wedge passed under are inside the sweep region anyway.
void renderFrame(bool force_full, bool sweep_only = false) {
  s_sweep_current.clear();
  s_dirty_sink = &s_sweep_current;
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)

  if (!sweep_only) {
    s_dirty_current.clear();
    s_dirty_sink = &s_dirty_current;
  } else {
    s_dirty_sink = nullptr;
  }
  {
    const DrawScope scope(s_frame);
    drawAircraft();
  }
  s_dirty_sink = nullptr;

  if (force_full || !s_panel_matches_frame || s_palette_changed) {
    s_palette_changed = false;
    s_frame.pushSprite(0, 0);
  } else {
    DirtyRegion update = s_sweep_previous;
    update.addAll(s_sweep_current);
    if (!sweep_only) {
      update.addAll(s_dirty_previous);
      update.addAll(s_dirty_current);
    }
    pushRegion(update);
  }

  s_sweep_previous = s_sweep_current;
  if (!sweep_only) {
    s_dirty_previous = s_dirty_current;
  }
  s_panel_matches_frame = true;
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void radarDisplayInit() {
  initPalette();
  initLabelMetrics();
  // Reserve the framebuffer while the internal heap is still unfragmented.
  if (!ensureFrameSprite()) {
    Serial.printf("radar: no frame buffer (largest free block %u bytes)\n",
                  ESP.getMaxAllocHeap());
  }
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame(true);
    return;
  }

  // Fallback when the framebuffer can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft();
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() {
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame(false);
    return;
  }

  radarDisplayDraw();
}

bool radarDisplayNightStyleChanged() {
  return radar::nightStyle() != s_night_style;
}

void radarDisplayAnimate() {
  // Without the off-screen buffer every frame would repaint the panel directly.
  if (!radar::sweepEnabled() || !s_frame_ready) {
    return;
  }
  const unsigned long now = millis();
  if (now - s_last_sweep_ms < radar::kSweepFrameIntervalMs) {
    return;
  }
  s_last_sweep_ms = now;
  initPalette();
  renderFrame(false, true);
}

}  // namespace ui
