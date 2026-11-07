#include "energieflow.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

namespace esphome::energieflow {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// Icons of Tessera's set (screen_manager/app/tile_icons.py).
constexpr uint32_t ICON_SOLAR = 0xF0A72;     // solar-power
constexpr uint32_t ICON_GRID = 0xF0D3E;      // transmission-tower
constexpr uint32_t ICON_HOME = 0xF02DC;      // home
constexpr uint32_t ICON_BATTERY = 0xF1901;   // home-battery
constexpr uint32_t ICON_CAR = 0xF0B6C;       // car-electric
constexpr uint32_t ICON_HEATPUMP = 0xF1A43;  // heat-pump

// Below this many watts a branch counts as idle: its line goes grey.
constexpr float IDLE_W = 20.f;

enum Slot : uint8_t { SOLAR, GRID, HOME, BATTERY, WALLBOX, HEATPUMP, NODES };
// The lines: one from each circle to the house (in the slot of that circle, none for the house), and the three that do
// not touch the house: the sun to the grid, the sun to the battery, and the grid to the battery (either way).
enum Link : uint8_t { SOLAR_GRID = NODES, SOLAR_BATTERY, GRID_BATTERY, LINES };

// One value of Home Assistant, read from the tile's `entities`.
struct Reading {
  bool chosen = false;  // the person chose a sensor for it
  bool valid = false;   // it has a number now
  float value = 0.f;    // in W for a power, % for a charge, ct/kWh for a price
};

// The words of a number in the screen's language: "850 W", "1,2 kW".
static std::string power_text(float w, const char *decimal) {
  char buf[24];
  const float a = std::fabs(w);
  if (a < 1000.f) {
    snprintf(buf, sizeof(buf), "%.0f W", a);
  } else {
    snprintf(buf, sizeof(buf), "%.1f kW", a / 1000.f);
  }
  std::string s(buf);
  if (decimal && *decimal && *decimal != '.') {
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s.replace(dot, 1, decimal);
  }
  return s;
}

static std::string number_text(float v, int decimals, const char *decimal, const char *unit) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f%s", decimals, v, unit);
  std::string s(buf);
  if (decimal && *decimal && *decimal != '.') {
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s.replace(dot, 1, decimal);
  }
  return s;
}

class FlowTile : public tessera::Tile {
 public:
  explicit FlowTile(const tessera::Plugin *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    parent_ = c.parent;

    // Smaller than 3 x 3 the diagram has no room. The editor puts a new plugin tile on in one cell and widens it one
    // edge at a time, so the tile takes every size and shows the house and the grid until it is large enough.
    compact_ = c.columns < 3 || c.rows < 3;
    if (compact_) {
      const int hi = ui::line_height(Font::ICON_SMALL), hv = ui::line_height(Font::BODY_LARGE), ht = ui::line_height(Font::BODY);
      const int top = (height_ - hi - hv - ht) / 2 < 0 ? 0 : (height_ - hi - hv - ht) / 2;
      icon_[HOME] = centred(parent_, Font::ICON_SMALL, theme::INK, width_);
      ui::set_text(icon_[HOME], ui::icon(ICON_HOME));
      value_[HOME] = centred(parent_, Font::BODY_LARGE, theme::INK, width_);
      extra_[HOME] = centred(parent_, Font::BODY, theme::MUTED, width_);
      lv_obj_set_pos(icon_[HOME], 0, top);
      lv_obj_set_pos(value_[HOME], 0, top + hi);
      lv_obj_set_pos(extra_[HOME], 0, top + hi + hv);
      on_theme();
      return;
    }

    // The circles: as large as the tile allows, the same for all six.
    const int by_width = width_ * 11 / 100, by_height = height_ * 14 / 100;
    radius_ = by_width < by_height ? by_width : by_height;
    const int least = ui::px(26);
    if (radius_ < least) radius_ = least;
    const int pad = ui::px(4);
    const int border = ui::px(3);

    // Centres in the way power-flow-card-plus lays them out: the grid left, the sun on top, the battery below, the
    // house in the middle right, and the wallbox and the heat pump as the house's own branches beside it.
    const int mid = height_ / 2;
    cx_[GRID] = radius_ + pad;               cy_[GRID] = mid;
    cx_[SOLAR] = width_ * 40 / 100;          cy_[SOLAR] = radius_ + pad;
    cx_[BATTERY] = width_ * 40 / 100;        cy_[BATTERY] = height_ - radius_ - pad;
    cx_[HOME] = width_ * 67 / 100;           cy_[HOME] = mid;
    cx_[WALLBOX] = width_ - radius_ - pad;   cy_[WALLBOX] = radius_ + pad;
    cx_[HEATPUMP] = width_ - radius_ - pad;  cy_[HEATPUMP] = height_ - radius_ - pad;

    // The lines first, so the circles lie over their ends.
    const int k = radius_ * 45 / 100;  // how far above and below the middle the sun's and the battery's lines meet the house
    const float ek = std::sqrt(float(radius_ * radius_ - k * k));
    set_points(GRID, {pt(cx_[GRID] + radius_, mid), pt(cx_[HOME] - radius_, mid)});
    // From the sun and the battery: right to the house, left to the grid, and straight between them.
    elbow(SOLAR, cx_[SOLAR] + k, cy_[SOLAR] + ek, mid - k, cx_[HOME] - ek);
    elbow(SOLAR_GRID, cx_[SOLAR] - k, cy_[SOLAR] + ek, mid - k, cx_[GRID] + ek);
    elbow(BATTERY, cx_[BATTERY] + k, cy_[BATTERY] - ek, mid + k, cx_[HOME] - ek);
    elbow(GRID_BATTERY, cx_[BATTERY] - k, cy_[BATTERY] - ek, mid + k, cx_[GRID] + ek);
    set_points(SOLAR_BATTERY, {pt(cx_[SOLAR], cy_[SOLAR] + radius_), pt(cx_[BATTERY], cy_[BATTERY] - radius_)});
    branch_points(WALLBOX);
    branch_points(HEATPUMP);
    for (int i = 0; i < LINES; ++i) {
      if (i == HOME) continue;
      line_[i] = lv_line_create(parent_);
      lv_line_set_points(line_[i], points_[i], count_[i]);
      lv_obj_set_style_line_width(line_[i], border, 0);
      lv_obj_set_style_line_rounded(line_[i], true, 0);
      lv_obj_remove_flag(line_[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // The circles, each with its icon, its value and a third line where it has one.
    const uint32_t icons[NODES] = {ICON_SOLAR, ICON_GRID, ICON_HOME, ICON_BATTERY, ICON_CAR, ICON_HEATPUMP};
    const int d = 2 * radius_;
    value_font_ = value_font(d);
    for (int i = 0; i < NODES; ++i) {
      circle_[i] = ui::block(parent_, theme::CARD);
      lv_obj_set_size(circle_[i], d, d);
      lv_obj_set_pos(circle_[i], cx_[i] - radius_, cy_[i] - radius_);
      lv_obj_set_style_radius(circle_[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(circle_[i], border, 0);
      lv_obj_set_style_pad_all(circle_[i], 0, 0);
      icon_[i] = centred(circle_[i], Font::ICON_SMALL, theme::INK, d);
      ui::set_text(icon_[i], ui::icon(icons[i]));
      value_[i] = centred(circle_[i], value_font_, theme::INK, d - 2 * border - ui::px(4));
      extra_[i] = centred(circle_[i], Font::BODY, theme::MUTED, d - 2 * border - ui::px(4));
    }

    // Over the grid: module 3 and its step; under it: the price of the hour.
    const int words = (cx_[SOLAR] - ui::px(4)) * 2 > d ? d + ui::px(40) : d;
    m3_title_ = ui::label(parent_, Font::BODY, theme::MUTED);
    m3_value_ = ui::label(parent_, Font::BODY_LARGE, theme::INK);
    price_ = ui::label(parent_, Font::BODY, theme::MUTED);
    for (lv_obj_t *l : {m3_title_, m3_value_, price_}) {
      lv_obj_set_width(l, words);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    const int left = cx_[GRID] - words / 2 < 0 ? 0 : cx_[GRID] - words / 2;
    const int hv = ui::line_height(Font::BODY_LARGE), ht = ui::line_height(Font::BODY);
    lv_obj_set_pos(m3_value_, left, cy_[GRID] - radius_ - ui::px(4) - hv);
    lv_obj_set_pos(m3_title_, left, cy_[GRID] - radius_ - ui::px(4) - hv - ht);
    lv_obj_set_pos(price_, left, cy_[GRID] + radius_ + ui::px(4));

    // Beside the heat pump: what it makes now, heating or hot water.
    heat_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(heat_, d + ui::px(40));
    lv_obj_set_style_text_align(heat_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(heat_, width_ - d - ui::px(40), cy_[HEATPUMP] - radius_ - ui::px(4) - ht);

    // Until sensors are chosen.
    choose_ = ui::label(parent_, Font::BODY, theme::MUTED);
    lv_obj_set_width(choose_, width_);
    lv_obj_set_style_text_align(choose_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(choose_, 0, mid - ht / 2);
    ui::set_text(choose_, plugin_->text("choose"));

    on_theme();
  }

  void on_state(JsonObjectConst data) override {
    JsonObjectConst e = data["entities"];
    read(e["solar"], power_[SOLAR], 1);
    read(e["grid"], power_[GRID], 1);
    read(e["home"], power_[HOME], 1);
    read(e["battery"], power_[BATTERY], 1);
    read(e["wallbox"], power_[WALLBOX], 1);
    read(e["heatpump"], power_[HEATPUMP], 1);
    read(e["soc"], soc_, 0);
    read(e["car_soc"], car_soc_, 0);
    read(e["heating"], heating_, 1);
    read(e["hot_water"], hot_water_, 1);
    read(e["price"], price_ct_, 2);
    read(e["price_m3"], price_m3_ct_, 2);
  }

  void on_tick(uint32_t epoch) override {
    const char *dec = plugin_->text("decimal");
    if (compact_) {
      // The house's use, and under it the grid: "Netz 1,2 kW" while it draws, the hint to make the tile larger otherwise.
      const Reading &h = power_[HOME], &g = power_[GRID];
      ui::set_text(value_[HOME], h.valid ? power_text(h.value, dec) : std::string("–"));
      std::string below = plugin_->text("larger");
      if (g.valid && std::fabs(g.value) >= IDLE_W)
        below = std::string(plugin_->text(g.value < 0 ? "export_short" : "grid_short")) + " " + power_text(g.value, dec);
      ui::set_text(extra_[HOME], below);
      return;
    }
    bool any = false;
    for (int i = 0; i < NODES; ++i) any = any || power_[i].chosen;
    set_hidden(choose_, any);

    for (int i = 0; i < NODES; ++i) {
      const Reading &r = power_[i];
      const bool shown = any && (r.chosen || i == HOME);
      set_hidden(circle_[i], !shown);
      if (i == WALLBOX || i == HEATPUMP) set_hidden(line_[i], !shown || !r.chosen);
      if (!shown) continue;
      ui::set_text(value_[i], r.valid ? power_text(r.value, dec) : std::string("–"));
      // Grid: export in the grid's blue, import in ink. Everything else in ink.
      ui::set_color(value_[i], i == GRID && r.valid && r.value < -IDLE_W ? theme::MARK_BLUE : theme::INK);
      std::string third;
      if (i == BATTERY && soc_.valid) third = number_text(soc_.value, 0, dec, " %");
      if (i == WALLBOX && car_soc_.valid) third = number_text(car_soc_.value, 0, dec, " %");
      ui::set_text(extra_[i], third);
      stack(i, !third.empty());
      // A branch that carries power is drawn in its colour, an idle one grey.
      const bool active = r.valid && std::fabs(r.value) >= IDLE_W;
      if (active != active_[i] || !painted_) {
        active_[i] = active;
        if (i == WALLBOX || i == HEATPUMP) lv_obj_set_style_line_color(line_[i], active ? colour(i) : ui::color(theme::TRACK), 0);
        lv_obj_set_style_border_color(circle_[i], active || i == HOME ? colour(i) : ui::color(theme::TRACK), 0);
      }
    }

    flows(any);

    // The heat pump's heating or hot water, while it makes some.
    std::string heat;
    if (power_[HEATPUMP].chosen) {
      if (hot_water_.valid && hot_water_.value >= IDLE_W)
        heat = std::string(plugin_->text("hot_water_short")) + " " + power_text(hot_water_.value, dec);
      else if (heating_.valid && heating_.value >= IDLE_W)
        heat = std::string(plugin_->text("heating_short")) + " " + power_text(heating_.value, dec);
    }
    ui::set_text(heat_, heat);

    // The price of the hour under the grid, and module 3 with its step over it.
    ui::set_text(price_, power_[GRID].chosen && price_ct_.valid ? number_text(price_ct_.value, 1, dec, " ct") : "");
    const bool m3 = power_[GRID].chosen && price_m3_ct_.chosen;
    set_hidden(m3_title_, !m3);
    set_hidden(m3_value_, !m3);
    if (m3) {
      ui::set_text(m3_title_, plugin_->text("module3"));
      int step = step_of(epoch);  // 0 NT, 1 ST, 2 HT, -1 while the clock is not set
      std::string words = step < 0 ? std::string() : std::string(plugin_->text(step == 0 ? "nt" : step == 1 ? "st" : "ht"));
      if (price_m3_ct_.valid) {
        if (!words.empty()) words += " · ";
        words += number_text(price_m3_ct_.value, 1, dec, " ct");
      }
      ui::set_text(m3_value_, words);
      ui::set_tone(m3_value_, step == 0 ? Tone::GOOD : step == 2 ? Tone::ALERT : Tone::ACCENT, theme::INK);
    }
    painted_ = true;
  }

  void on_theme() override {
    painted_ = false;
    for (int i = 0; i < NODES; ++i) {
      if (!circle_[i]) continue;
      lv_obj_set_style_bg_color(circle_[i], ui::color(theme::CARD), 0);
      ui::set_color(extra_[i], theme::MUTED);
      lv_obj_set_style_text_color(icon_[i], colour(i), 0);
    }
    if (compact_) {
      lv_obj_set_style_text_color(icon_[HOME], colour(HOME), 0);
      ui::set_color(extra_[HOME], theme::MUTED);
      on_tick(tessera::epoch());
      return;
    }
    ui::set_color(m3_title_, theme::MUTED);
    ui::set_color(price_, theme::MUTED);
    ui::set_color(heat_, theme::MUTED);
    ui::set_color(choose_, theme::MUTED);
    on_tick(tessera::epoch());
  }

 private:
  // The colour of each branch, as in the dashboard: the sun amber, the grid blue, the battery green, the house ink,
  // the wallbox purple and the heat pump red.
  static lv_color_t colour(int i) {
    switch (i) {
      case SOLAR: return ui::color(theme::MARK_AMBER);
      case GRID: return ui::color(theme::MARK_BLUE);
      case BATTERY: return ui::color(theme::MARK_GREEN);
      case WALLBOX: return ui::color(theme::MARK_PURPLE);
      case HEATPUMP: return ui::tone_color(Tone::ALERT, theme::INK);
      default: return ui::color(theme::INK);
    }
  }

  // Where the power of the sun, the grid and the battery goes, as power-flow-card-plus splits it: the sun feeds the grid
  // first and then the battery, the grid or the battery covers the rest. Each line in its colour while it carries power.
  void flows(bool any) {
    const Reading &s = power_[SOLAR], &g = power_[GRID], &b = power_[BATTERY];
    const float sun = s.valid && s.value > 0 ? s.value : 0.f;
    const float import = g.valid && g.value > 0 ? g.value : 0.f, out = g.valid && g.value < 0 ? -g.value : 0.f;
    const float discharge = b.valid && b.value > 0 ? b.value : 0.f, charge = b.valid && b.value < 0 ? -b.value : 0.f;
    const float sun_grid = std::fmin(out, sun);
    const float sun_battery = std::fmin(charge, sun - sun_grid);
    const float grid_battery = std::fmin(import, charge - sun_battery);
    const float battery_grid = std::fmax(0.f, out - sun_grid);
    const float flow[LINES] = {sun - sun_grid - sun_battery, import - grid_battery, 0.f, discharge - battery_grid, 0.f, 0.f,
                               sun_grid, sun_battery, grid_battery + battery_grid};
    const bool shown[LINES] = {s.chosen, g.chosen, false, b.chosen, false, false,
                               s.chosen && g.chosen, s.chosen && b.chosen, g.chosen && b.chosen};
    for (int i : {(int) SOLAR, (int) GRID, (int) BATTERY, (int) SOLAR_GRID, (int) SOLAR_BATTERY, (int) GRID_BATTERY}) {
      set_hidden(line_[i], !any || !shown[i]);
      const bool active = flow[i] >= IDLE_W;
      int hue = i == SOLAR_GRID || i == SOLAR_BATTERY ? SOLAR : i == GRID_BATTERY ? (battery_grid > grid_battery ? BATTERY : GRID) : i;
      const int look = active ? hue : -1;
      if (look != look_[i] || !painted_) {
        look_[i] = look;
        lv_obj_set_style_line_color(line_[i], active ? colour(hue) : ui::color(theme::TRACK), 0);
      }
    }
  }

  // Module 3 of §14a: NT 23:00-06:45, HT 08:30-15:15 and 17:15-21:15, ST in between; only from October to March
  // (Q1 and Q4), ST all day from April to September.
  static int step_of(uint32_t epoch) {
    if (!epoch) return -1;
    const tessera::LocalTime t = tessera::local_time(epoch);
    const bool seasons = t.month <= 3 || t.month >= 10;
    if (!seasons) return 1;
    const int m = t.hour * 60 + t.minute;
    if (m >= 23 * 60 || m < 6 * 60 + 45) return 0;
    if ((m >= 8 * 60 + 30 && m < 15 * 60 + 15) || (m >= 17 * 60 + 15 && m < 21 * 60 + 15)) return 2;
    return 1;
  }

  // A number of an entity: a power in W (kW and MW converted), a price in ct (EUR converted), a charge as it is.
  // kind: 0 as it is, 1 a power, 2 a price.
  static void read(JsonVariantConst e, Reading &r, int kind) {
    r.chosen = !e.isNull();
    r.valid = false;
    if (!r.chosen) return;
    const char *state = e["state"] | "";
    char *end = nullptr;
    const float v = strtof(state, &end);
    if (end == state || std::isnan(v)) return;
    const char *unit = e["attributes"]["unit_of_measurement"] | "";
    float f = 1.f;
    if (kind == 1) {
      if (!strcmp(unit, "kW")) f = 1000.f;
      else if (!strcmp(unit, "MW")) f = 1000000.f;
    } else if (kind == 2) {
      if (strstr(unit, "ct") == nullptr && strstr(unit, "Cent") == nullptr) f = 100.f;  // EUR/kWh, €/kWh
    }
    r.value = v * f;
    r.valid = true;
  }

  static lv_point_precise_t pt(float x, float y) {
    lv_point_precise_t p;
    p.x = static_cast<lv_value_precise_t>(x);
    p.y = static_cast<lv_value_precise_t>(y);
    return p;
  }

  void set_points(int i, std::initializer_list<lv_point_precise_t> pts) {
    int j = 0;
    for (const auto &p : pts) if (j < POINTS) points_[i][j++] = p;
    count_[i] = j;
  }

  // Down (or up) from a circle at x0, a rounded corner at the height yc, and on to x1: the curve of power-flow-card-plus.
  // The corner is a quadratic curve of CURVE points, as round as the room between the circles allows.
  void elbow(int i, float x0, float y0, float yc, float x1) {
    const float sy = yc >= y0 ? 1.f : -1.f, sx = x1 >= x0 ? 1.f : -1.f;
    float r = radius_ * 0.8f;
    if (r > std::fabs(yc - y0)) r = std::fabs(yc - y0);
    if (r > std::fabs(x1 - x0) / 2) r = std::fabs(x1 - x0) / 2;
    int j = 0;
    points_[i][j++] = pt(x0, y0);
    const float ax = x0, ay = yc - sy * r, bx = x0 + sx * r, by = yc;  // the corner from (ax, ay) to (bx, by) around (x0, yc)
    for (int n = 0; n <= CURVE; ++n) {
      const float t = float(n) / CURVE, u = 1.f - t;
      points_[i][j++] = pt(u * u * ax + 2 * u * t * x0 + t * t * bx, u * u * ay + 2 * u * t * yc + t * t * by);
    }
    points_[i][j++] = pt(x1, yc);
    count_[i] = j;
  }

  // A straight line from the house to a branch beside it, from edge to edge.
  void branch_points(int i) {
    const float dx = float(cx_[i] - cx_[HOME]), dy = float(cy_[i] - cy_[HOME]);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.f) { count_[i] = 0; return; }
    const float ux = dx / len, uy = dy / len;
    points_[i][0] = pt(cx_[HOME] + ux * radius_, cy_[HOME] + uy * radius_);
    points_[i][1] = pt(cx_[i] - ux * radius_, cy_[i] - uy * radius_);
    count_[i] = 2;
  }

  Font value_font(int d) const {
    for (Font f : {Font::BODY_LARGE, Font::BODY}) {
      if (ui::text_width("8,8 kW", f) <= d - ui::px(12)) return f;
    }
    return Font::BODY;
  }

  static lv_obj_t *centred(lv_obj_t *parent, Font font, theme::Role role, int width) {
    lv_obj_t *l = ui::label(parent, font, role);
    lv_obj_set_width(l, width);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
  }

  // The icon, the value and the third line, together in the middle of the circle.
  void stack(int i, bool third) {
    const int d = 2 * radius_;
    const int hi = ui::line_height(Font::ICON_SMALL);
    const int hv = ui::line_height(value_font_);
    const int he = third ? ui::line_height(Font::BODY) : 0;
    const int top = (d - hi - hv - he) / 2;
    const int bw = lv_obj_get_width(value_[i]);
    lv_obj_set_pos(icon_[i], 0, top);
    lv_obj_set_pos(value_[i], (d - bw) / 2, top + hi);
    lv_obj_set_pos(extra_[i], (d - bw) / 2, top + hi + hv);
    set_hidden(extra_[i], !third);
  }

  static void set_hidden(lv_obj_t *o, bool hidden) {
    if (!o) return;
    if (hidden != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
      if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }

  const tessera::Plugin *plugin_;
  lv_obj_t *parent_{};
  int width_ = 0, height_ = 0, radius_ = 0;
  int cx_[NODES]{}, cy_[NODES]{};
  static constexpr int CURVE = 8, POINTS = CURVE + 3;
  lv_point_precise_t points_[LINES][POINTS]{};
  int count_[LINES]{};
  int look_[LINES]{};
  lv_obj_t *line_[LINES]{}, *circle_[NODES]{}, *icon_[NODES]{}, *value_[NODES]{}, *extra_[NODES]{};
  lv_obj_t *m3_title_{}, *m3_value_{}, *price_{}, *heat_{}, *choose_{};
  Reading power_[NODES], soc_, car_soc_, heating_, hot_water_, price_ct_, price_m3_ct_;
  bool active_[NODES]{};
  bool painted_ = false;
  bool compact_ = false;
  Font value_font_ = Font::BODY;
};

void Energieflow::setup() {
  add_tile("flow", [this]() { return new FlowTile(this); });
}

}  // namespace esphome::energieflow
