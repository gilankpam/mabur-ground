#include "mtest.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "gs_draw.h"
#include "gs_font.h"
#include "gs_layer.h"
#include "menu_layer.h"
#include "menu_model.h"

using namespace maburplay;

// The settings menu painted into the OSD surface, against the synthetic
// scaled .gfont fixture (the same one test_gs_compact uses).

namespace {

struct Canvas {
  std::vector<uint32_t> px;
  Surface s;
  Canvas(int w, int h) : px((size_t)w * h, 0u) {
    s.pixels = px.data(); s.width = w; s.height = h; s.stride_px = w;
  }
  uint32_t at(int x, int y) const { return px[(size_t)y * s.stride_px + x]; }
  bool all_zero() const {
    for (uint32_t p : px)
      if (p) return false;
    return true;
  }
  bool any_in(const DirtyRect& r) const {
    for (int y = r.y; y < r.y + r.h; ++y)
      for (int x = r.x; x < r.x + r.w; ++x)
        if (at(x, y)) return true;
    return false;
  }
};

GsFont& font() {
  static GsFont f;
  static bool loaded = false;
  if (!loaded) {
    std::string err;
    REQUIRE(f.load(GSFONT_SCALED, &err));
    loaded = true;
  }
  return f;
}

MenuView sample() {
  MenuView v;
  v.open = true;
  v.crumb = "Video";
  v.linked = true;
  ViewRow a;
  a.id = "video.mode"; a.label = "Camera mode"; a.value = "60 fps"; a.vtx = true; a.selected = true;
  ViewRow b;
  b.id = "video.slices"; b.label = "Slices"; b.value = "On"; b.adv = true;
  ViewRow c;
  c.id = "video.colour"; c.label = "Colour"; c.value = "Vivid"; c.warn = "judder";
  v.rows = {a, b, c};
  v.help = "Pick the camera mode.";
  v.keys = "▶ change · ◀ back";
  return v;
}

bool intersects(const DirtyRect& a, const DirtyRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool inside(const DirtyRect& in, const DirtyRect& out) {
  return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
         in.y + in.h <= out.y + out.h;
}

}  // namespace

TEST(a_closed_view_paints_nothing_and_has_empty_bounds) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  m.set_view(MenuView{});
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) == 0);
  CHECK(out.empty());
  CHECK(c.all_zero());
  const DirtyRect b = m.bounds();
  CHECK(b.w == 0 || b.h == 0);
  CHECK(m.debug_texts().empty());
}

TEST(an_open_view_draws_crumb_rows_help_and_keys_in_order) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  m.set_view(sample());
  CHECK(m.dirty());
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) > 0);
  CHECK(!m.dirty());
  const std::vector<std::string> want = {
      "Video", "● drone linked",
      "Camera mode", "VTX", "60 fps",
      "Slices", "ADV", "On",
      "Colour", "⚠ judder", "Vivid",
      "Pick the camera mode.",
      "▶ change · ◀ back"};
  CHECK(m.debug_texts() == want);

  MenuView off = sample();
  off.linked = false;
  m.set_view(off);
  m.paint(c.s, &out);
  CHECK(m.debug_texts()[1] == "○ no drone");
}

TEST(the_panel_is_45_percent_wide_and_inset) {
  for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{3840, 2160}}) {
    Canvas c(w, h);
    MenuLayer m(font());
    std::string err;
    REQUIRE(m.layout(w, h, &err));
    m.set_view(sample());
    std::vector<DirtyRect> out;
    m.paint(c.s, &out);
    const DirtyRect p = m.debug_panel_box();
    const int inset = (int)std::lround(24.0 * h / 1080);
    CHECK(p.x == inset);
    CHECK(p.y == inset);
    CHECK(p.w == (int)std::lround(0.45 * w));
    CHECK(p.h > 0 && p.y + p.h < h);
    // The fill is premultiplied like every other OSD pixel: the bottom-right
    // corner of the panel sits right of the keys text, on bare panel.
    CHECK(c.at(p.x + p.w - 1, p.y + p.h - 1) == premul(tok::kMenuPanel, 232));
    CHECK(out.size() == 1);
    CHECK(inside(p, m.bounds()));
  }
}

TEST(a_picker_draws_to_the_right_of_the_panel_with_no_title) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = sample();
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> panel_texts = m.debug_texts();
  CHECK(m.debug_picker_box().w == 0);

  v.picker = MenuView::Picker::kChoices;
  v.choices = {{"60", "60 fps", "", true}, {"90", "90 fps", "judder", true}};
  v.picker_cursor = 1;
  v.picker_current = 0;
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> all = m.debug_texts();
  REQUIRE(all.size() > panel_texts.size());
  const std::vector<std::string> picker(all.begin() + (long)panel_texts.size(), all.end());
  CHECK(picker.front() == "60 fps ·");
  CHECK((picker == std::vector<std::string>{"60 fps ·", "90 fps", "judder"}));

  const DirtyRect p = m.debug_panel_box();
  const DirtyRect k = m.debug_picker_box();
  CHECK(k.x == p.x + p.w + 12);
  CHECK(k.y == p.y);
  CHECK(k.w > 0 && k.h > 0);
  CHECK(c.any_in(k));
  CHECK(inside(k, m.bounds()));

  // A number picker: arrows, the value, the range.
  v.picker = MenuView::Picker::kNumber;
  v.choices.clear();
  v.number_text = "24000";
  v.number_range = "4000 – 40000";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> num = m.debug_texts();
  CHECK((std::vector<std::string>(num.begin() + (long)panel_texts.size(), num.end()) ==
         std::vector<std::string>{"▲", "24000", "▼", "4000 – 40000"}));

  // A confirm: the sentence and the two answers, no title.
  v.picker = MenuView::Picker::kConfirm;
  v.confirm_text = "Restart the encoder?";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> conf = m.debug_texts();
  CHECK((std::vector<std::string>(conf.begin() + (long)panel_texts.size(), conf.end()) ==
         std::vector<std::string>{"Restart the encoder?", "▶ Go ahead", "◀ Cancel"}));
}

// The operator's menu size multiplies every design length on top of the
// screen-height factor: inset, text, rows, and the panel's share of the
// width.
TEST(a_larger_menu_size_scales_the_panel_and_its_text) {
  for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}}) {
    Canvas c(w, h);
    MenuLayer normal(font()), larger(font());
    std::string err;
    REQUIRE(normal.layout(w, h, &err));
    larger.set_scale(1.5);
    REQUIRE(larger.layout(w, h, &err));
    normal.set_view(sample());
    larger.set_view(sample());
    normal.paint(c.s, nullptr);
    larger.paint(c.s, nullptr);
    const DirtyRect n = normal.debug_panel_box(), l = larger.debug_panel_box();
    CHECK(l.x == (int)std::lround(24.0 * 1.5 * h / 1080));
    CHECK(l.w == (int)std::lround(0.45 * 1.5 * w));
    // Text snaps to a baked size, so the height grows by about the scale, not
    // exactly: at 720p the normal help text is already clamped up to the
    // smallest baked size, so the panel grows less there.
    CHECK(l.h > n.h * 1.2);
    CHECK(larger.debug_row_px() == font().nearest((int)std::lround(22.0 * 1.5 * h / 1080))->px);
    CHECK(larger.debug_row_px() > normal.debug_row_px());
  }
}

// A live size change re-lays the menu out on its own: the caller never
// calls layout() again, and the next paint moves the boxes.
TEST(set_scale_after_layout_relays_out_and_dirties) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  m.set_view(sample());
  m.paint(c.s, nullptr);
  const DirtyRect before = m.debug_panel_box();
  const int px_before = m.debug_row_px();
  CHECK(!m.dirty());
  m.set_scale(1.25);
  CHECK(m.dirty());
  std::vector<DirtyRect> out;
  m.paint(c.s, &out);
  CHECK(!out.empty());
  CHECK(m.debug_panel_box().w > before.w);
  CHECK(m.debug_row_px() > px_before);
  // The same size again is a no-op.
  m.set_scale(1.25);
  CHECK(!m.dirty());
}

// The widest fixed-width picker (a confirm) keeps its full width beside the
// panel at the largest size, 4:3 screens included: the panel gives up width
// before the picker is squeezed against the screen edge.
TEST(the_picker_keeps_its_width_at_the_largest_size) {
  for (auto [w, h] : {std::pair{1920, 1080}, std::pair{1280, 720}, std::pair{1024, 768}}) {
    Canvas c(w, h);
    MenuLayer m(font());
    std::string err;
    m.set_scale(1.5);
    REQUIRE(m.layout(w, h, &err));
    MenuView v = sample();
    v.picker = MenuView::Picker::kConfirm;
    v.confirm_text = "Restart the encoder?";
    m.set_view(v);
    m.paint(c.s, nullptr);
    const DirtyRect k = m.debug_picker_box();
    const int inset = (int)std::lround(24.0 * 1.5 * h / 1080);
    CHECK(k.w >= (int)std::lround(320.0 * 1.5 * h / 1080));
    CHECK(k.x + k.w <= w - inset);
  }
}

TEST(a_changed_row_dirties_only_its_rect) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = sample();
  m.set_view(v);
  m.paint(c.s, nullptr);
  const DirtyRect p = m.debug_panel_box();

  v.rows[1].value = "Off";
  m.set_view(v);
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) == 1);
  REQUIRE(out.size() == 1);
  CHECK(inside(out[0], p));
  CHECK(out[0].w == p.w);
  CHECK(out[0].h * 5 < p.h);

  // Nothing changed: nothing drawn.
  m.set_view(v);
  out.clear();
  CHECK(!m.dirty());
  CHECK(m.paint(c.s, &out) == 0);
  CHECK(out.empty());

  // invalidate() forces the whole panel.
  m.invalidate();
  CHECK(m.dirty());
  CHECK(m.paint(c.s, &out) >= 1);
}

TEST(closing_erases_every_box_it_drew) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = sample();
  v.picker = MenuView::Picker::kChoices;
  v.choices = {{"60", "60 fps", "", true}, {"90", "90 fps", "", false}};
  v.picker_cursor = 0;
  v.picker_current = 0;
  v.toast = "Saved";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const DirtyRect b = m.bounds();
  CHECK(c.any_in(b));

  m.set_view(MenuView{});
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) > 0);
  CHECK(c.all_zero());
  for (const DirtyRect& r : out) CHECK(inside(r, b));
  CHECK(m.debug_texts().empty());
}

TEST(a_busy_row_animates_on_tick) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = sample();
  v.rows[1].busy = true;
  v.rows[1].status = "applying";
  m.set_tick(1000);
  m.set_view(v);
  m.paint(c.s, nullptr);
  // 1000 ms is the fourth 300 ms step: three dots.
  const auto t0 = m.debug_texts();
  CHECK(t0[8] == "applying...");
  const std::vector<uint32_t> before = c.px;

  m.set_tick(1400);
  CHECK(m.dirty());
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) > 0);
  CHECK(m.debug_texts()[8] == "applying");
  // The sweep bar: a 2 px strip along the busy row's bottom edge whose
  // pixels moved.
  bool strip_moved = false;
  for (const DirtyRect& r : out) {
    if (r.h != 2) continue;
    for (int y = r.y; y < r.y + r.h; ++y)
      for (int x = r.x; x < r.x + r.w; ++x)
        if (c.px[(size_t)y * 1920 + x] != before[(size_t)y * 1920 + x]) strip_moved = true;
  }
  CHECK(strip_moved);

  // A tick inside the same sweep step and dot step changes nothing.
  m.set_tick(1401);
  CHECK(!m.dirty());

  // An idle view ignores ticks.
  MenuView idle = sample();
  m.set_view(idle);
  m.paint(c.s, nullptr);
  m.set_tick(5000);
  CHECK(!m.dirty());
}

TEST(a_menu_view_with_no_surface_is_a_no_op) {
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  m.set_view(sample());
  std::vector<DirtyRect> out;
  CHECK(m.paint(Surface{}, &out) == 0);
  CHECK(out.empty());
  DirtyRect r{0, 0, 1920, 1080};
  CHECK(m.repaint_intersecting(&r, 1, Surface{}, &out) == 0);
  CHECK(out.empty());
  // Still owed: the next real surface gets the whole view.
  CHECK(m.dirty());
  Canvas c(1920, 1080);
  CHECK(m.paint(c.s, &out) > 0);
  CHECK(!c.all_zero());

  // Before layout() there is nothing to draw either.
  MenuLayer fresh(font());
  fresh.set_view(sample());
  CHECK(fresh.paint(c.s, &out) == 0);
}

TEST(banner_and_toast_draw_with_the_menu_closed) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v;
  v.banner = "⚠ Camera 90 fps: this display has no 90 Hz mode";
  v.toast = "Hold ▶ to open the menu";
  m.set_view(v);
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) == 2);
  CHECK((m.debug_texts() == std::vector<std::string>{v.toast, v.banner}));
  REQUIRE(out.size() == 2);
  const DirtyRect toast = out[0], banner = out[1];
  CHECK(c.any_in(toast));
  CHECK(c.any_in(banner));
  // Both centred; the banner at 12 % down, the toast above the GS bar.
  CHECK(std::abs((toast.x + toast.w / 2) - 960) <= 1);
  CHECK(std::abs((banner.x + banner.w / 2) - 960) <= 1);
  CHECK(banner.y == (int)std::lround(0.12 * 1080));
  CHECK(toast.y + toast.h == 1080 - 90);
  CHECK(m.debug_panel_box().w == 0);

  // The toast expires: only its box is erased, the banner stays.
  v.toast.clear();
  m.set_view(v);
  out.clear();
  CHECK(m.paint(c.s, &out) == 1);
  CHECK(!c.any_in(toast));
  CHECK(c.any_in(banner));
}

TEST(an_open_menu_moves_the_banner_off_the_panel_and_picker) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  const std::string banner =
      "⚠ Camera 90 fps — this display has no 90 Hz mode. Showing at 60 Hz (judder).";
  auto banner_box = [](const std::vector<DirtyRect>& out) { return out.back(); };

  // Closed: at 12 % down.
  MenuView closed;
  closed.banner = banner;
  m.set_view(closed);
  std::vector<DirtyRect> out;
  REQUIRE(m.paint(c.s, &out) == 1);
  const DirtyRect at_top = banner_box(out);
  CHECK(at_top.y == (int)std::lround(0.12 * 1080));

  // Open with a picker up: below both, clear of each.
  MenuView open = sample();
  open.banner = banner;
  open.picker = MenuView::Picker::kChoices;
  open.choices = {{"60", "60 fps", "", true}, {"90", "90 fps", "judder", true}};
  open.picker_cursor = 0;
  open.picker_current = 1;
  m.set_view(open);
  out.clear();
  m.paint(c.s, &out);
  const DirtyRect p = m.debug_panel_box(), k = m.debug_picker_box();
  const DirtyRect b = banner_box(out);
  CHECK(b.w > 0 && b.h > 0);
  CHECK(!intersects(b, p));
  CHECK(!intersects(b, k));
  CHECK(std::abs((b.x + b.w / 2) - 960) <= 1);
  CHECK(b.y + b.h <= 1080 - 90);
  // The old banner box was erased where nothing else now draws: its top
  // rows sit inside neither the panel, the picker nor the new banner.
  bool erased = false;
  for (const DirtyRect& r : out) erased |= r.x == at_top.x && r.y == at_top.y &&
                                           r.w == at_top.w && r.h == at_top.h;
  CHECK(erased);
  const int probe_x = std::max(k.x + k.w, at_top.x) + 1;
  if (probe_x < at_top.x + at_top.w) CHECK(c.at(probe_x, at_top.y) == 0u);

  // Closed again: back at 12 %, the open-menu position erased.
  m.set_view(closed);
  out.clear();
  m.paint(c.s, &out);
  CHECK(banner_box(out).y == at_top.y);
  CHECK(!intersects(b, at_top));
  CHECK(!c.any_in(b));
  const int below = at_top.y + at_top.h;
  CHECK(!c.any_in(DirtyRect{p.x, below, p.w, p.y + p.h - below}));
}

TEST(a_header_draws_under_the_crumb_and_only_when_set) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));

  MenuView v = sample();
  v.header = "Reading the drone's settings…";
  m.set_view(v);
  m.paint(c.s, nullptr);
  CHECK(m.debug_texts()[2] == v.header);

  MenuView without = sample();
  m.set_view(without);
  m.paint(c.s, nullptr);
  const std::vector<std::string> want = {
      "Video", "● drone linked",
      "Camera mode", "VTX", "60 fps",
      "Slices", "ADV", "On",
      "Colour", "⚠ judder", "Vivid",
      "Pick the camera mode.",
      "▶ change · ◀ back"};
  CHECK(m.debug_texts() == want);
}

// A non-empty pill mark sits beside the link pill, in amber, and shrinks
// the crumb's fit() width rather than overlapping it.
TEST(a_pill_mark_draws_beside_the_pill_and_only_when_set) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));

  MenuView v = sample();
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> without = m.debug_texts();
  REQUIRE(without.size() >= 2);
  CHECK(without[0] == "Video");
  CHECK(without[1] == "● drone linked");

  v.pill_mark = "no heartbeat";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::vector<std::string> with = m.debug_texts();
  REQUIRE(with.size() == without.size() + 1);
  CHECK(with[0] == "Video");
  CHECK(with[1] == "⚠ no heartbeat");
  CHECK(with[2] == "● drone linked");

  // Clearing it goes back to the two-run header.
  v.pill_mark.clear();
  m.set_view(v);
  m.paint(c.s, nullptr);
  CHECK(m.debug_texts() == without);
}

TEST(a_pill_mark_takes_room_from_the_crumb_rather_than_overlapping) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));

  MenuView v = sample();
  v.crumb = std::string(80, 'x');   // long enough to be clipped either way
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::string crumb_no_mark = m.debug_texts()[0];

  v.pill_mark = "no heartbeat";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const std::string crumb_with_mark = m.debug_texts()[0];
  CHECK(crumb_with_mark.size() < crumb_no_mark.size());
}

// A choice's note is drawn in the colour its tone maps to, by the same
// token a row's own status would use.
TEST(choice_note_drawn_in_its_tone) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));

  MenuView v = sample();
  m.set_view(v);
  m.paint(c.s, nullptr);
  const size_t panel_n = m.debug_texts().size();

  v.picker = MenuView::Picker::kChoices;
  v.choices = {{"60", "60 fps", "", true},
               {"90", "90 fps", "bad note", true, ChoiceTone::kBad}};
  v.picker_cursor = 0;
  v.picker_current = 0;
  m.set_view(v);
  m.paint(c.s, nullptr);

  const auto texts = m.debug_texts();
  const auto colors = m.debug_colors();
  REQUIRE(texts.size() == colors.size());
  auto it = std::find(texts.begin() + (long)panel_n, texts.end(), std::string("bad note"));
  REQUIRE(it != texts.end());
  const size_t i = (size_t)(it - texts.begin());
  CHECK(colors[i] == tok::kStatusCritical);
}

namespace {

// An update page over a menu that still has rows: the page replaces them.
MenuView page_view(int n_lines) {
  MenuView v = sample();
  v.upgrade.shown = true;
  v.upgrade.title = "Update from file";
  v.upgrade.header = "v0.4.0 (this GS v0.3.2)";
  v.upgrade.header_warn = true;
  v.upgrade.pair = "pairs with drone v0.4.x";
  for (int i = 1; i <= n_lines; ++i) v.upgrade.lines.push_back("line " + std::to_string(i));
  v.upgrade.keys = "▲▼ scroll  ▶ install  ◀ back";
  return v;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST(an_upgrade_page_draws_header_pair_and_eight_body_lines_from_first) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = page_view(12);
  m.set_view(v);
  m.paint(c.s, nullptr);
  std::vector<std::string> want = {"Update from file", "● drone linked",
                                   "v0.4.0 (this GS v0.3.2)", "pairs with drone v0.4.x"};
  for (int i = 1; i <= 8; ++i) want.push_back("line " + std::to_string(i));
  want.push_back("▲▼ scroll  ▶ install  ◀ back");
  CHECK(m.debug_texts() == want);
  const auto colors = m.debug_colors();
  REQUIRE(colors.size() == want.size());
  CHECK(colors[2] == tok::kStatusCaution);   // header_warn
  CHECK(colors[3] == tok::kTextPrimary);     // pair, no warn

  v.upgrade.first = 4;
  m.set_view(v);
  m.paint(c.s, nullptr);
  std::vector<std::string> scrolled(want.begin(), want.begin() + 4);
  for (int i = 5; i <= 12; ++i) scrolled.push_back("line " + std::to_string(i));
  scrolled.push_back(want.back());
  CHECK(m.debug_texts() == scrolled);
}

TEST(an_upgrade_page_cursor_line_is_selected) {
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  // The y of the first panel-edge pixel that differs between two cursors.
  auto paint = [&](int cursor) {
    Canvas c(1920, 1080);
    MenuView v = page_view(3);
    v.upgrade.cursor = cursor;
    m.set_view(v);
    m.invalidate();
    m.paint(c.s, nullptr);
    return c.px;
  };
  auto first_diff_y = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    const DirtyRect p = m.debug_panel_box();
    for (int y = p.y; y < p.y + p.h; ++y)
      if (a[(size_t)y * 1920 + p.x + 2] != b[(size_t)y * 1920 + p.x + 2]) return y;
    return -1;
  };
  const auto none = paint(-1), one = paint(1), zero = paint(0);
  const int y1 = first_diff_y(none, one), y0 = first_diff_y(none, zero);
  CHECK(y0 >= 0);
  CHECK(y1 > y0);   // the second line's fill sits below the first's
}

TEST(an_upgrade_running_page_draws_a_bar_only_at_a_percent) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = page_view(0);
  v.upgrade.running = true;
  v.upgrade.phase_text = "Writing";
  v.upgrade.pct = 25;
  v.upgrade.keys = "";
  m.set_view(v);
  m.paint(c.s, nullptr);
  auto t = m.debug_texts();
  CHECK(has(t, "25 %"));
  CHECK(has(t, "Writing"));
  CHECK(std::count(t.begin(), t.end(), std::string("Don't power off")) == 1);
  const int with_bar = m.debug_panel_box().h;

  v.upgrade.pct = -1;
  m.set_view(v);
  m.paint(c.s, nullptr);
  t = m.debug_texts();
  CHECK(!has(t, "25 %"));
  CHECK(has(t, "Writing"));
  CHECK(has(t, "Don't power off"));
  CHECK(m.debug_panel_box().h < with_bar);

  // An ended page keeps its phase text but is not running: no warning line.
  v.upgrade.running = false;
  v.upgrade.phase_text = "Cancelled";
  m.set_view(v);
  m.paint(c.s, nullptr);
  t = m.debug_texts();
  CHECK(has(t, "Cancelled"));
  CHECK(!has(t, "Don't power off"));
}

TEST(an_upgrade_page_error_is_drawn_in_the_error_token) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = page_view(0);
  v.upgrade.error = "Write failed — reflash from SD card";
  m.set_view(v);
  m.paint(c.s, nullptr);
  const auto t = m.debug_texts();
  const auto colors = m.debug_colors();
  auto it = std::find(t.begin(), t.end(), v.upgrade.error);
  REQUIRE(it != t.end());
  CHECK(colors[(size_t)(it - t.begin())] == tok::kStatusCritical);
}

TEST(an_upgrade_confirm_reuses_the_confirm_prompt) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v = page_view(2);
  v.upgrade.confirm = "Install v0.4.0?";
  m.set_view(v);
  m.paint(c.s, nullptr);
  CHECK(m.debug_picker_box().w > 0);
  const auto t = m.debug_texts();
  CHECK(has(t, "Install v0.4.0?"));
  CHECK(has(t, "▶ Go ahead"));
  CHECK(has(t, "◀ Cancel"));

  // A menu picker left open under the page is not drawn beside it.
  v.upgrade.confirm = "";
  v.picker = MenuView::Picker::kChoices;
  v.choices = {{"60", "60 fps", "", true}};
  m.set_view(v);
  m.paint(c.s, nullptr);
  CHECK(m.debug_picker_box().w == 0);
}

TEST(an_upgrade_page_with_no_rows_paints) {
  Canvas c(1920, 1080);
  MenuLayer m(font());
  std::string err;
  REQUIRE(m.layout(1920, 1080, &err));
  MenuView v;
  v.open = true;
  v.upgrade.shown = true;
  m.set_view(v);
  std::vector<DirtyRect> out;
  CHECK(m.paint(c.s, &out) > 0);
  CHECK(!m.dirty());
}

MTEST_MAIN
