#include "workout.h"
#include "comm.h"
#include "data.h"
#include "home.h"
#include "i18n.h"
#include "ui.h"

typedef enum { ST_SET, ST_HOLD, ST_REST, ST_DONE } State;
enum { FOCUS_WEIGHT = 0, FOCUS_AMOUNT = 1 };

static Window *s_win;
static Layer *s_canvas;
static State s_state;
static uint8_t s_focus = FOCUS_AMOUNT;
static int s_hr;          // bpm, 0 when there is no fresh reading
static time_t s_hr_at;    // when s_hr was read
static uint8_t s_hr_live;  // the period asked of the sensor, 0 when not sampling

static GRect s_boxes;  // where the value boxes are, for touch

static void menu_open(void);
static void finish_workout(void);

// ---------------------------------------------------------------------------------------------
// Helpers

static SessEntry *cur_entry(void) {
  return g_sess.cur < g_sess.n ? &g_sess.e[g_sess.cur] : NULL;
}

static bool has_weight(const SessEntry *e) {
  return e && e->mode != MODE_CARDIO;
}

static bool per_side_reps(const SessEntry *e) {
  return e && (e->flags & PEF_SIDE) && e->mode == MODE_REPS;
}

// "Links"/"Rechts" for a row of a timed per-side hold, else NULL.
static const char *side_name(const SessEntry *e, int si) {
  if (!SESS_PAIRS(e)) return NULL;
  return si % 2 ? L("Right", "Rechts") : L("Left", "Links");
}

// Set number and count as openGym shows them: a left/right pair is one set.
// Warm-ups and work sets are numbered apart, like openGym does.
static int rows_per_set(const SessEntry *e) { return SESS_PAIRS(e) ? 2 : 1; }
static bool is_warm(const SessEntry *e, int si) { return e->s[si].flags & SET_WARM; }
static int set_no(const SessEntry *e, int si) {
  int base = is_warm(e, si) ? 0 : sess_warm_rows(e);
  return (si - base) / rows_per_set(e) + 1;
}
static int set_count(const SessEntry *e) {
  return (e->n - sess_warm_rows(e) + rows_per_set(e) - 1) / rows_per_set(e);
}

// "Satz 2 von 4", "Aufwärmen 1 von 2", with " - Links" on a per-side hold. `hold` for the
// running hold, which says so in front.
static void set_label(char *buf, size_t size, const SessEntry *e, int si, bool hold) {
  int warm_sets = sess_warm_rows(e) / rows_per_set(e);
  int no = set_no(e, si), of = is_warm(e, si) ? warm_sets : set_count(e);
  const char *side = side_name(e, si);
  const char *what = is_warm(e, si) ? L("Warm-up", "Aufwärmen") : (hold ? L("Hold - set", "Halten - Satz") : L("Set", "Satz"));
  if (side) snprintf(buf, size, L("%s %d of %d - %s", "%s %d/%d - %s"), what, no, of, side);
  else if (hold || is_warm(e, si)) snprintf(buf, size, L("%s %d of %d", "%s %d von %d"), what, no, of);
  else if (e->sg) snprintf(buf, size, L("%s %d of %d - superset", "%s %d von %d - Supersatz"), what, no, of);
  else snprintf(buf, size, L("%s %d of %d  -  %d/%d", "%s %d von %d  -  %d/%d"), what, no, of, g_sess.cur + 1, g_sess.n);
}

// Reps of a per-side exercise are the total of both sides; the watch shows one side's ("8"),
// and half of an odd total as it falls ("7.5"), like openGym.
static void fmt_amount(char *buf, size_t size, const SessEntry *e, uint16_t amt) {
  if (per_side_reps(e)) {
    if (amt % 2) snprintf(buf, size, "%d.5", amt / 2);
    else snprintf(buf, size, "%d", amt / 2);
  } else {
    snprintf(buf, size, "%d", amt);
  }
}

static const char *amount_label(const SessEntry *e) {
  if (per_side_reps(e)) return L("reps/side", "Wdh./Seite");
  switch (e->mode) {
    case MODE_TIME: return L("sec", "Sek.");
    case MODE_CARDIO: return "min";
    case MODE_DIST: return "m";
    default: return L("reps", "Wdh.");
  }
}

static uint16_t amount_step(const SessEntry *e) {
  if (per_side_reps(e)) return 2;  // one more on each side
  switch (e->mode) {
    case MODE_TIME: return 5;
    case MODE_DIST: return 5;
    default: return 1;
  }
}

static void default_focus(void) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  // Weight first only when there is no weight yet for a loaded exercise.
  if (e && si >= 0 && has_weight(e) && e->s[si].w20 == 0 && !(g_plan.ex[e->ex].flags & EXF_BODYWEIGHT)) {
    s_focus = FOCUS_WEIGHT;
  } else {
    s_focus = FOCUS_AMOUNT;
  }
}

// Brings the state in line with the session after anything changed it.
static void settle(void) {
  time_t now = time(NULL);
  // A timed hold keeps running while the app is closed: its start is in the session.
  if (g_sess.hold_start) {
    if (sess_cur_set() >= 0) {
      s_state = ST_HOLD;
      return;
    }
    g_sess.hold_start = 0;
    sess_save();
  }
  if (g_sess.rest_end && (time_t)g_sess.rest_end > now) {
    s_state = ST_REST;
    return;
  }
  if (g_sess.rest_end) {
    g_sess.rest_end = 0;
    sess_save();
  }
  if (sess_cur_set() < 0 && !sess_advance()) {
    s_state = ST_DONE;
    return;
  }
  if (s_state != ST_SET) default_focus();
  s_state = ST_SET;
}

static void redraw(void) {
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void start_rest(uint16_t secs) {
  g_sess.rest_end = time(NULL) + secs;
  g_sess.rest_total = secs;
  s_state = ST_REST;
}

static void end_rest(bool alert) {
  g_sess.rest_end = 0;
  sess_save();
  if (alert) {
    vibe_timer_done();
    light_enable_interaction();
  }
  s_state = ST_SET;
  settle();
  default_focus();
  redraw();
}

// Marks the current set done (or skipped) and moves on, with a rest after a done set.
static uint16_t rest_of(const SessEntry *e) {
  return e->rest ? e->rest : g_plan.rest_default;
}

// A superset rests once per round: its own rest when openGym gives it one, else the longest of
// its exercises' (openGym lib/supersetFlow.js).
static uint16_t superset_rest(uint8_t sg) {
  Routine *rt = &g_plan.r[g_sess.routine];
  if (g_sess.routine < g_plan.n_routines && sg <= rt->n_groups && rt->g[sg - 1].rest) return rt->g[sg - 1].rest;
  uint16_t rest = 0;
  for (int i = 0; i < g_sess.n; i++) {
    if (g_sess.e[i].sg == sg && rest_of(&g_sess.e[i]) > rest) rest = rest_of(&g_sess.e[i]);
  }
  return rest;
}

// The superset exercise after `from` whose turn it is: the next one in the group with sets open,
// round and round, skipping the finished ones. -1 when the whole group is done.
static int superset_next(int from) {
  uint8_t sg = g_sess.e[from].sg;
  for (int step = 1; step <= g_sess.n; step++) {
    int i = (from + step) % g_sess.n;
    if (g_sess.e[i].sg == sg && sess_open_in(i) > 0) return i;
  }
  return -1;
}

// Marks the current set done (or skipped) and moves on, with a rest after a done set. In a
// superset the exercises take turns, and the rest comes after each round.
static void complete_set(uint8_t flag) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (!e || si < 0) return;
  g_sess.hold_start = 0;
  e->s[si].flags |= flag;
  bool warm = e->s[si].flags & SET_WARM;
  uint16_t rest = rest_of(e);
  if (warm && rest > 60) rest = 60;  // a warm-up needs no full rest
  bool more;
  // Its turn goes on through its warm-ups, and through the right side after the left.
  bool turn_over = !warm && !(SESS_PAIRS(e) && si % 2 == 0 && sess_cur_set() == si + 1);
  if (e->sg && turn_over) {
    int from = g_sess.cur;
    int next = superset_next(from);
    if (next >= 0) {
      g_sess.cur = next;
      more = true;
      // The round ends where the turn goes back to an exercise above (or the same one).
      rest = next <= from ? superset_rest(e->sg) : 0;
    } else {
      more = sess_advance();
      rest = superset_rest(e->sg);
    }
  } else {
    more = sess_cur_set() >= 0 || sess_advance();
  }
  if (!more) {
    s_state = ST_DONE;
    g_sess.rest_end = 0;
    vibes_double_pulse();
  } else if (flag == SET_DONE && rest > 0) {
    start_rest(rest);
  } else {
    if (e->sg && turn_over) vibes_short_pulse();  // the next exercise of the superset, now
    s_state = ST_SET;
    default_focus();
  }
  sess_save();
  redraw();
}

// Changes one value of the current set, and of the open sets after it, so a weight change
// carries over to the rest of the exercise.
static void adjust(int dir) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (!e || si < 0) return;
  bool weight = s_focus == FOCUS_WEIGHT && has_weight(e);
  int v = weight ? e->s[si].w20 : e->s[si].amt;
  int step = weight ? g_plan.step20 : amount_step(e);
  v += dir * step;
  if (v < 0) v = 0;
  if (v > 60000) v = 60000;
  // A warm-up changes alone (with its other side on a per-side hold); a work set carries the
  // change to the open work sets after it.
  int end = e->n;
  if (is_warm(e, si)) end = (si / rows_per_set(e) + 1) * rows_per_set(e);
  for (int k = si; k < end; k++) {
    if (k > si && (e->s[k].flags & (SET_DONE | SET_SKIP))) continue;
    if (weight) e->s[k].w20 = v;
    else e->s[k].amt = v;
  }
  redraw();
}

// Heart rate, when on in the settings: while the workout is on screen the sensor is asked for a
// reading as often as set (instead of the system's idle rate), and each new one is shown as it
// comes. The request is dropped again when the workout leaves the screen, to save the battery.
// A reading older than three periods, and at least 30 s, is not shown (watch off the wrist, say).
static int hr_stale(void) {
  return s_hr_live * 3 > 30 ? s_hr_live * 3 : 30;
}

static void update_hr(void) {
#if defined(PBL_HEALTH)
  if (!s_hr_live) {
    s_hr = 0;
    return;
  }
  time_t now = time(NULL);
  HealthServiceAccessibilityMask m =
      health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
  HealthValue v = (m & HealthServiceAccessibilityMaskAvailable)
                      ? health_service_peek_current_value(HealthMetricHeartRateBPM) : 0;
  if (v > 0) {
    s_hr = (int)v;
    s_hr_at = now;
  } else if (now - s_hr_at > hr_stale()) {
    s_hr = 0;
  }
#endif
}

#if defined(PBL_HEALTH)
static void health_event(HealthEventType type, void *ctx) {
  if (type != HealthEventHeartRateUpdate) return;
  s_hr_at = 0;  // a fresh reading: take it whatever came before
  update_hr();
  redraw();
}
#endif

static void hr_start(void) {
#if defined(PBL_HEALTH)
  if (s_hr_live || !g_heart_period) return;
  s_hr_live = g_heart_period;
  s_hr_at = time(NULL);  // the last stored reading counts as fresh once
  health_service_set_heart_rate_sample_period(s_hr_live);
  health_service_events_subscribe(health_event, NULL);
  update_hr();
#endif
}

static void hr_stop(void) {
#if defined(PBL_HEALTH)
  if (!s_hr_live) return;
  s_hr_live = 0;
  health_service_events_unsubscribe();
  health_service_set_heart_rate_sample_period(0);
  s_hr = 0;
#endif
}

// ---------------------------------------------------------------------------------------------
// Drawing

static void draw_text(GContext *ctx, const char *text, GFont font, GRect r, GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, align, NULL);
}

static void draw_value_box(GContext *ctx, GRect r, const char *value, const char *label, bool focused) {
  if (focused) {
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_rect(ctx, r, 8, GCornersAll);
  } else {
    graphics_context_set_stroke_color(ctx, C_DIM);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_round_rect(ctx, r, 8);
    graphics_context_set_stroke_width(ctx, 1);
  }
  GColor fg = focused ? C_ON_ACCENT : C_FG;
  // The label along the bottom, the value centred in what is left above it.
  int label_y = r.origin.y + r.size.h - g_ui.small_line - 4;
  int area = label_y - r.origin.y;
  bool numeric = value[0] >= '0' && value[0] <= '9';
  GFont f = g_ui.title;
  int fh = g_ui.title_line, vy = r.origin.y + (area - fh) / 2 - 2;
  if (numeric) {
    // "62.5" or "102.5" does not fit the big digits in a half-width box: step down.
    for (int i = 0; i < g_ui.n_value; i++) {
      f = g_ui.value[i];
      fh = g_ui.value_h[i];
      if (fh > area && i + 1 < g_ui.n_value) continue;
      GSize sz = graphics_text_layout_get_content_size(value, f, GRect(0, 0, 400, 80),
                                                       GTextOverflowModeWordWrap, GTextAlignmentLeft);
      if (sz.w <= r.size.w - 8) break;
    }
    vy = r.origin.y + (area - fh) / 2 - 3;
  }
  draw_text(ctx, value, f, GRect(r.origin.x, vy, r.size.w, fh + 8), GTextAlignmentCenter, fg);
  draw_text(ctx, label, g_ui.small, GRect(r.origin.x + 2, label_y, r.size.w - 4, g_ui.small_line),
            GTextAlignmentCenter, focused ? C_ON_ACCENT : C_MUTED);
}

// A timer in the biggest digits that fit across `w`; returns the height it took.
static int draw_big(GContext *ctx, const char *text, int y, int w, GColor color) {
  GFont f = g_ui.big[0];
  int h = g_ui.big_h[0];
  for (int i = 0; i < g_ui.n_big; i++) {
    f = g_ui.big[i];
    h = g_ui.big_h[i];
    GSize sz = graphics_text_layout_get_content_size(text, f, GRect(0, 0, 400, 80),
                                                     GTextOverflowModeWordWrap, GTextAlignmentLeft);
    if (sz.w <= w - 8) break;
  }
  draw_text(ctx, text, f, GRect(0, y, w, h + 8), GTextAlignmentCenter, color);
  return h + 6;
}

static void draw_progress(GContext *ctx, GRect r, int num, int den) {
  graphics_context_set_fill_color(ctx, C_DIM);
  graphics_fill_rect(ctx, r, 3, GCornersAll);
  if (den <= 0) return;
  if (num < 0) num = 0;
  if (num > den) num = den;
  int w = r.size.w * num / den;
  if (w > 0) {
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_rect(ctx, GRect(r.origin.x, r.origin.y, w, r.size.h), 3, GCornersAll);
  }
}

static int text_w(const char *text, GFont f) {
  return graphics_text_layout_get_content_size(text, f, GRect(0, 0, 400, 40), GTextOverflowModeWordWrap,
                                               GTextAlignmentLeft).w;
}

static void draw_top(GContext *ctx, GRect b) {
  char dur[12], clk[8], hr[12] = "";
  GFont f = g_ui.top;
  int h = g_ui.top_h + 2;
  fmt_duration(dur, sizeof(dur), time(NULL) - g_sess.start);
  draw_text(ctx, dur, f, GRect(8, 0, 100, h), GTextAlignmentLeft, C_MUTED);
  int left_end = 8 + text_w(dur, f), right_start = b.size.w - 24;
  if (s_hr > 0) {
    // Large text has room for the number only.
    snprintf(hr, sizeof(hr), g_ui.level ? "%d" : "%d bpm", s_hr);
    right_start -= text_w(hr, f);
    draw_text(ctx, hr, f, GRect(b.size.w - 124, 0, 100, h), GTextAlignmentRight, C_MUTED);
  }
  // The time of day; in 12-hour style without AM/PM, which would run into the heart rate. Left
  // out when big text leaves no room between the two.
  time_t now = time(NULL);
  strftime(clk, sizeof(clk), clock_is_24h_style() ? "%H:%M" : "%I:%M", localtime(&now));
  const char *clock = (!clock_is_24h_style() && clk[0] == '0') ? clk + 1 : clk;
  int cw = text_w(clock, f);
  if ((b.size.w - cw) / 2 >= left_end + 6 && (b.size.w + cw) / 2 <= right_start - 6) {
    draw_text(ctx, clock, f, GRect(0, 0, b.size.w, h), GTextAlignmentCenter, C_FG);
  }
}

// The exercise's name under the top bar, and the line under it; returns where they end.
static int draw_heading(GContext *ctx, const char *name, const char *line, int w, GColor line_color) {
  int y = g_ui.top_h;
  int nh = g_ui.name_line * 2;
  GSize sz = graphics_text_layout_get_content_size(name, g_ui.name, GRect(0, 0, w, nh),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  // A short name takes one line, which leaves more room below on large text.
  if (g_ui.level && sz.h <= g_ui.name_line + 6) nh = g_ui.name_line;
  draw_text(ctx, name, g_ui.name, GRect(8, y, w, nh + 4), GTextAlignmentLeft, C_FG);
  y += nh;
  draw_text(ctx, line, g_ui.small, GRect(8, y, w, g_ui.small_line), GTextAlignmentLeft, line_color);
  return y + g_ui.small_line;
}

static void set_summary(char *buf, size_t size, const SessEntry *e, const SetRec *s) {
  char w[12], a[8];
  fmt_weight(w, sizeof(w), s->w20);
  if (per_side_reps(e)) {
    fmt_amount(a, sizeof(a), e, s->amt);
    if (s->w20) snprintf(buf, size, L("%s %s x %s per side", "%s %s x %s pro Seite"), w, unit_label(), a);
    else snprintf(buf, size, L("%s reps per side", "%s Wdh. pro Seite"), a);
    return;
  }
  switch (e->mode) {
    case MODE_TIME:
      if (SESS_PAIRS(e)) snprintf(buf, size, "%d s - %s", s->amt, side_name(e, s - e->s));
      else snprintf(buf, size, "%d s", s->amt);
      break;
    case MODE_CARDIO: snprintf(buf, size, "%d min", s->amt); break;
    case MODE_DIST: snprintf(buf, size, "%d m", s->amt); break;
    default:
      if (s->w20) snprintf(buf, size, "%s %s x %d", w, unit_label(), s->amt);
      else snprintf(buf, size, L("%d reps", "%d Wdh."), s->amt);
  }
}

static void draw_set(GContext *ctx, GRect b) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (!e || si < 0) return;
  int content_w = b.size.w - 26;
  char buf[48];

  set_label(buf, sizeof(buf), e, si, false);
  int top = draw_heading(ctx, g_plan.ex[e->ex].name, buf, content_w - 4, C_MUTED) + 6;

  SetRec *s = &e->s[si];
  char wv[12], av[8];
  bool bw = g_plan.ex[e->ex].flags & EXF_BODYWEIGHT;
  if (s->w20 == 0 && bw) strcpy(wv, L("BW", "Eigen"));
  else fmt_weight(wv, sizeof(wv), s->w20);
  fmt_amount(av, sizeof(av), e, s->amt);

  // The boxes take the room down to the bottom (Normal keeps a line there for the hint).
  int h = b.size.h - top - (g_ui.footer ? 40 : 6);
  s_boxes = GRect(8, top, content_w - 8, h);
  if (has_weight(e)) {
    int bw_ = (content_w - 8 - 6) / 2;
    draw_value_box(ctx, GRect(8, top, bw_, h), wv, unit_label(), s_focus == FOCUS_WEIGHT);
    draw_value_box(ctx, GRect(8 + bw_ + 6, top, bw_, h), av, amount_label(e), s_focus == FOCUS_AMOUNT);
    if (g_ui.footer) {
      snprintf(buf, sizeof(buf), L("Hold select: edit %s", "Mitte halten: %s ändern"), s_focus == FOCUS_WEIGHT ? amount_label(e) : unit_label());
      draw_text(ctx, buf, g_ui.small, GRect(0, b.size.h - 34, content_w + 8, 22), GTextAlignmentCenter, C_MUTED);
    }
  } else {
    draw_value_box(ctx, GRect(8, top, content_w - 8, h), av, amount_label(e), true);
  }
  ui_draw_hints(ctx, b, HINT_PLUS, e->mode == MODE_TIME ? HINT_SKIP : HINT_CHECK, HINT_MINUS);
}

static void draw_hold(GContext *ctx, GRect b) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (!e || si < 0) return;
  int content_w = b.size.w - 26;
  char buf[40];
  int target = e->s[si].amt;
  int elapsed = time(NULL) - (time_t)g_sess.hold_start;
  set_label(buf, sizeof(buf), e, si, true);
  int y = draw_heading(ctx, g_plan.ex[e->ex].name, buf, content_w, C_ACCENT);
  fmt_duration(buf, sizeof(buf), target > 0 ? target - elapsed : elapsed);
  y += 8;
  y += draw_big(ctx, buf, y, content_w + 8, C_FG);
  draw_progress(ctx, GRect(16, y + 2, content_w - 16, 8), elapsed, target);
  if (y + 10 + g_ui.small_line + 6 <= b.size.h) {
    draw_text(ctx, L("Select: stop", "Mitte: Stopp"), g_ui.small, GRect(0, b.size.h - g_ui.small_line - 10, content_w + 8, g_ui.small_line),
              GTextAlignmentCenter, C_MUTED);
  }
  ui_draw_hints(ctx, b, HINT_NONE, HINT_STOP, HINT_NONE);
}

static void draw_rest(GContext *ctx, GRect b) {
  int content_w = b.size.w - 26;
  char buf[48];
  int left = (int)g_sess.rest_end - (int)time(NULL);
  int y = g_ui.top_h;
  draw_text(ctx, L("Rest", "Pause"), g_ui.name, GRect(8, y, content_w, g_ui.name_line + 4), GTextAlignmentLeft, C_ACCENT);
  y += g_ui.name_line + 3;
  fmt_duration(buf, sizeof(buf), left);
  y += draw_big(ctx, buf, y, content_w + 8, C_FG);
  draw_progress(ctx, GRect(16, y + 4, content_w - 16, 8), left, g_sess.rest_total);
  y += 18;

  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (e && si >= 0) {
    // Below the "-15" hint on the right, so the lines stop short of it.
    int w = content_w - (g_ui.level ? 24 : 0);
    int left_h = b.size.h - y;
    // "Next" only when the name and its sets still fit in two lines each after it.
    if (left_h >= g_ui.small_line * 3 + 20) {
      draw_text(ctx, L("Next", "Als Nächstes"), g_ui.small, GRect(8, y, w, g_ui.small_line), GTextAlignmentLeft, C_MUTED);
      y += g_ui.small_line - 4;
      left_h = b.size.h - y;
    }
    int name_h = left_h - g_ui.small_line - 4;
    if (name_h > g_ui.small_line * 2) name_h = g_ui.small_line * 2;
    if (name_h < g_ui.small_line) name_h = g_ui.small_line;
    GSize sz = graphics_text_layout_get_content_size(g_plan.ex[e->ex].name, g_ui.small_bold, GRect(0, 0, w, name_h),
                                                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
    draw_text(ctx, g_plan.ex[e->ex].name, g_ui.small_bold, GRect(8, y, w, name_h), GTextAlignmentLeft, C_FG);
    y += sz.h + 4 > name_h ? name_h : sz.h + 4;
    set_summary(buf, sizeof(buf), e, &e->s[si]);
    draw_text(ctx, buf, g_ui.small, GRect(8, y, w, g_ui.small_line), GTextAlignmentLeft, C_MUTED);
  }
  int x = b.size.w - 46;
  int hh = g_ui.small_line;
  draw_text(ctx, "+15", g_ui.hint, GRect(x, b.size.h * 22 / 100 - hh / 2 - 2, 44, hh), GTextAlignmentRight, C_MUTED);
  draw_text(ctx, "-15", g_ui.hint, GRect(x, b.size.h * 78 / 100 - hh / 2 - 2, 44, hh), GTextAlignmentRight, C_MUTED);
  ui_draw_hints(ctx, b, HINT_NONE, HINT_SKIP, HINT_NONE);
}

static void draw_done(GContext *ctx, GRect b) {
  int content_w = b.size.w - 26;
  char buf[48], dur[16];
  int sl = g_ui.small_line;
  int y = g_ui.level ? g_ui.top_h + 4 : 40;
  draw_text(ctx, L("All sets done", "Alle Sätze fertig"), g_ui.title, GRect(8, y, content_w, 64), GTextAlignmentCenter, C_FG);
  y += 68;
  fmt_duration(dur, sizeof(dur), time(NULL) - g_sess.start);
  snprintf(buf, sizeof(buf), L("%d sets - %s", "%d Sätze - %s"), sess_count(SET_DONE), dur);
  draw_text(ctx, buf, g_ui.small, GRect(8, y, content_w, sl), GTextAlignmentCenter, C_MUTED);
  y += g_ui.level ? sl + 8 : 38;
  draw_text(ctx, L("Select: finish and save", "Mitte: speichern"), g_ui.small_bold, GRect(8, y, content_w, sl * 2),
            GTextAlignmentCenter, C_ACCENT);
  GSize sz = graphics_text_layout_get_content_size(L("Select: finish and save", "Mitte: speichern"), g_ui.small_bold,
                                                   GRect(0, 0, content_w, sl * 2), GTextOverflowModeWordWrap, GTextAlignmentCenter);
  y += sz.h > sl ? sz.h + 2 : (g_ui.level ? sl : 24);
  draw_text(ctx, L("Back: menu", "Zurück: Menü"), g_ui.small, GRect(8, y, content_w, sl), GTextAlignmentCenter, C_MUTED);
  ui_draw_hints(ctx, b, HINT_NONE, HINT_CHECK, HINT_NONE);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  if (!g_sess.active) return;
  draw_top(ctx, b);
  switch (s_state) {
    case ST_SET: draw_set(ctx, b); break;
    case ST_HOLD: draw_hold(ctx, b); break;
    case ST_REST: draw_rest(ctx, b); break;
    case ST_DONE: draw_done(ctx, b); break;
  }
}

// ---------------------------------------------------------------------------------------------
// Time

static void tick(struct tm *t, TimeUnits changed) {
  if (!g_sess.active) return;
  time_t now = time(NULL);
  if (s_state == ST_REST) {
    int left = (int)g_sess.rest_end - (int)now;
    if (left == 10) vibe_warning();
    if (left <= 0) end_rest(true);
  } else if (s_state == ST_HOLD) {
    SessEntry *e = cur_entry();
    int si = sess_cur_set();
    if (e && si >= 0 && e->s[si].amt > 0 && now - (time_t)g_sess.hold_start >= e->s[si].amt) {
      vibe_timer_done();
      light_enable_interaction();
      complete_set(SET_DONE);
    }
  }
  // The setting can change from the phone while the workout is open.
  if (g_heart_period != s_hr_live) {
    hr_stop();
    hr_start();
  } else if (t->tm_sec % 5 == 0) update_hr();
  redraw();
}

// ---------------------------------------------------------------------------------------------
// Buttons

static void up_click(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_SET) adjust(+1);
  else if (s_state == ST_REST) {
    g_sess.rest_end += 15;
    int left = (int)g_sess.rest_end - (int)time(NULL);
    if (left > g_sess.rest_total) g_sess.rest_total = left;
    sess_save();
    redraw();
  }
}

static void down_click(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_SET) adjust(-1);
  else if (s_state == ST_REST) {
    g_sess.rest_end -= 15;
    if ((time_t)g_sess.rest_end <= time(NULL)) end_rest(false);
    else { sess_save(); redraw(); }
  }
}

static void select_click(ClickRecognizerRef r, void *ctx) {
  SessEntry *e = cur_entry();
  switch (s_state) {
    case ST_SET:
      if (e && e->mode == MODE_TIME) {
        g_sess.hold_start = time(NULL);
        s_state = ST_HOLD;
        sess_save();
        vibes_short_pulse();
        redraw();
      } else {
        complete_set(SET_DONE);
      }
      break;
    case ST_HOLD: {
      int si = sess_cur_set();
      if (e && si >= 0) {
        int elapsed = time(NULL) - (time_t)g_sess.hold_start;
        e->s[si].amt = elapsed > 0 ? elapsed : 1;
      }
      complete_set(SET_DONE);
      break;
    }
    case ST_REST:
      end_rest(false);
      break;
    case ST_DONE:
      finish_workout();
      break;
  }
}

static void select_long(ClickRecognizerRef r, void *ctx) {
  if (s_state != ST_SET || !has_weight(cur_entry())) return;
  s_focus = s_focus == FOCUS_WEIGHT ? FOCUS_AMOUNT : FOCUS_WEIGHT;
  vibes_short_pulse();
  redraw();
}

static void back_click(ClickRecognizerRef r, void *ctx) {
  menu_open();
}

static void click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 450, select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

#ifdef _PBL_API_EXISTS_touch_service_subscribe
// Touch on the Pebble Time 2: tap a value box to edit it.
static void touch(const TouchEvent *ev, void *ctx) {
  if (ev->type != TouchEvent_Touchdown || s_state != ST_SET || !has_weight(cur_entry())) return;
  if (ev->y < s_boxes.origin.y - 4 || ev->y > s_boxes.origin.y + s_boxes.size.h + 4) return;
  uint8_t f = ev->x < s_boxes.origin.x + s_boxes.size.w / 2 ? FOCUS_WEIGHT : FOCUS_AMOUNT;
  if (f != s_focus) {
    s_focus = f;
    redraw();
  }
}
#endif

// ---------------------------------------------------------------------------------------------
// Window

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void win_appear(Window *w) {
  settle();
  hr_start();
  tick_timer_service_subscribe(SECOND_UNIT, tick);
#ifdef _PBL_API_EXISTS_touch_service_subscribe
  touch_service_subscribe(touch, NULL);
#endif
  redraw();
}

static void win_disappear(Window *w) {
  tick_timer_service_unsubscribe();
  hr_stop();
#ifdef _PBL_API_EXISTS_touch_service_subscribe
  touch_service_unsubscribe();
#endif
  sess_save();
}

static void win_unload(Window *w) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
  window_destroy(s_win);
  s_win = NULL;
}

void workout_open(void) {
  if (!g_sess.active) return;
  if (s_win) return;
  s_state = ST_SET;
  default_focus();
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_click_config_provider(s_win, click_config);
  window_set_window_handlers(s_win, (WindowHandlers) {
    .load = win_load,
    .appear = win_appear,
    .disappear = win_disappear,
    .unload = win_unload,
  });
  window_stack_push(s_win, true);
}

static void close_workout(void) {
  if (s_win) window_stack_remove(s_win, false);
  home_refresh();
}

// ---------------------------------------------------------------------------------------------
// Exercise list: jump to any exercise

static Window *s_list_win;
static MenuLayer *s_list_menu;
static StatusBarLayer *s_list_bar;

static uint16_t list_rows(MenuLayer *m, uint16_t s, void *ctx) {
  return g_sess.n;
}

static void list_draw(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  SessEntry *e = &g_sess.e[idx->row];
  char buf[32];
  int open = sess_open_in(idx->row);
  int done = 0;
  for (int k = 0; k < e->n; k++) if ((e->s[k].flags & (SET_DONE | SET_WARM)) == SET_DONE) done++;
  done /= rows_per_set(e);  // a per-side hold's set is its left and its right row; warm-ups apart
  if (open) snprintf(buf, sizeof(buf), L("%d/%d sets done", "%d/%d Sätze fertig"), done, set_count(e));
  else snprintf(buf, sizeof(buf), "%s", L("Done - select adds a set", "Fertig - Mitte: Satz dazu"));
  ui_draw_cell(ctx, cell, g_plan.ex[e->ex].name, buf);
}

static int16_t list_row_h(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return ui_cell_h(true);
}

static void list_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (sess_open_in(idx->row) == 0) {
    if (!sess_add_set(idx->row)) { vibes_double_pulse(); return; }
  }
  g_sess.cur = idx->row;
  g_sess.hold_start = 0;  // a hold that was running is dropped with the switch
  if (s_state != ST_REST) s_state = ST_SET;
  default_focus();
  sess_save();
  window_stack_remove(s_list_win, false);
}

static void list_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect frame;
  s_list_bar = ui_status_bar(root, &frame);
  s_list_menu = menu_layer_create(frame);
  menu_layer_set_callbacks(s_list_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = list_rows,
    .get_cell_height = g_ui.level ? list_row_h : NULL,
    .draw_row = list_draw,
    .select_click = list_select,
  });
  ui_style_menu(s_list_menu);
  menu_layer_set_selected_index(s_list_menu, MenuIndex(0, g_sess.cur), MenuRowAlignCenter, false);
  menu_layer_set_click_config_onto_window(s_list_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_list_menu));
}

static void list_unload(Window *w) {
  menu_layer_destroy(s_list_menu);
  status_bar_layer_destroy(s_list_bar);
  window_destroy(s_list_win);
  s_list_win = NULL;
}

static void list_open(void) {
  s_list_win = window_create();
  window_set_background_color(s_list_win, C_BG);
  window_set_window_handlers(s_list_win, (WindowHandlers) { .load = list_load, .unload = list_unload });
  window_stack_push(s_list_win, true);
}

// ---------------------------------------------------------------------------------------------
// Workout menu (Back)

enum { M_RESUME, M_EXERCISES, M_SKIP, M_ADD, M_NOTE, M_FINISH, M_DISCARD, M_LEAVE, M_COUNT };

static Window *s_menu_win;
static MenuLayer *s_menu;
static StatusBarLayer *s_menu_bar;
static bool s_discard_armed;

// ---------------------------------------------------------------------------------------------
// A note by voice: the watch's dictation (through the phone), confirmed on the watch, added to
// the workout's note, which goes to openGym with the workout.

#if defined(PBL_MICROPHONE)
static DictationSession *s_dict;
#endif
static bool s_note_failed;

static void note_done(DictationSession *session, DictationSessionStatus status, char *text, void *ctx) {
#if defined(PBL_MICROPHONE)
  if (status == DictationSessionStatusSuccess) {
    sess_add_note(text);
    s_note_failed = false;
    vibes_short_pulse();
  } else if (status != DictationSessionStatusFailureTranscriptionRejected) {
    s_note_failed = true;  // "Nochmal sprechen" and Back are no failure
  }
  if (s_menu) menu_layer_reload_data(s_menu);
#endif
}

static void note_dictate(void) {
#if defined(PBL_MICROPHONE)
  if (!s_dict) s_dict = dictation_session_create(0, note_done, NULL);
  if (s_dict && dictation_session_start(s_dict) == DictationSessionStatusSuccess) return;
#endif
  s_note_failed = true;
  vibes_double_pulse();
  if (s_menu) menu_layer_reload_data(s_menu);
}

static uint16_t menu_rows(MenuLayer *m, uint16_t s, void *ctx) {
  return M_COUNT;
}

static void menu_draw(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  char buf[32];
  switch (idx->row) {
    case M_RESUME: ui_draw_cell(ctx, cell, L("Resume", "Fortsetzen"), NULL); break;
    case M_EXERCISES:
      snprintf(buf, sizeof(buf), L("%d/%d sets done", "%d/%d Sätze fertig"), sess_count(SET_DONE), sess_total());
      ui_draw_cell(ctx, cell, L("Exercises", "Übungen"), buf);
      break;
    case M_SKIP: ui_draw_cell(ctx, cell, L("Skip this set", "Satz überspringen"), NULL); break;
    case M_ADD: ui_draw_cell(ctx, cell, L("Add a set", "Satz hinzufügen"), L("To this exercise", "Zu dieser Übung")); break;
    case M_NOTE: {
      const char *sub = s_note_failed ? L("Did not work: phone near?", "Ging nicht: Handy in der Nähe?")
                      : g_sess.note[0] ? g_sess.note
                      : L("Speak it, saved with the workout", "Sprechen, wird mitgespeichert");
      ui_draw_cell(ctx, cell, g_sess.note[0] ? L("Add to the note", "Notiz ergänzen") : L("Dictate a note", "Notiz diktieren"), sub);
      break;
    }
    case M_FINISH: ui_draw_cell(ctx, cell, L("Finish and save", "Beenden und speichern"), NULL); break;
    case M_DISCARD:
      ui_draw_cell(ctx, cell, L("Discard workout", "Training verwerfen"),
                           s_discard_armed ? L("Select again to discard", "Nochmal drücken")
                                           : L("Nothing is saved", "Nichts wird gespeichert"));
      break;
    case M_LEAVE: ui_draw_cell(ctx, cell, L("Leave for now", "Pausieren"), L("Resume it later", "Später fortsetzen")); break;
  }
}

static int16_t menu_row_h(MenuLayer *m, MenuIndex *idx, void *ctx) {
  switch (idx->row) {
    case M_RESUME: case M_SKIP: case M_FINISH: return ui_cell_h(false);
    default: return ui_cell_h(true);
  }
}

static void menu_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (idx->row != M_DISCARD) s_discard_armed = false;
  switch (idx->row) {
    case M_RESUME:
      window_stack_remove(s_menu_win, true);
      break;
    case M_EXERCISES:
      list_open();
      window_stack_remove(s_menu_win, false);
      break;
    case M_SKIP:
      if (s_state == ST_SET || s_state == ST_HOLD) {
        s_state = ST_SET;
        complete_set(SET_SKIP);
      }
      window_stack_remove(s_menu_win, true);
      break;
    case M_ADD: {
      if (cur_entry() && sess_add_set(g_sess.cur)) {
        if (s_state == ST_DONE) s_state = ST_SET;
        sess_save();
      } else {
        vibes_double_pulse();
      }
      window_stack_remove(s_menu_win, true);
      break;
    }
    case M_NOTE:
      s_note_failed = false;
      note_dictate();
      break;
    case M_FINISH:
      finish_workout();
      break;
    case M_DISCARD:
      if (!s_discard_armed) {
        s_discard_armed = true;
        menu_layer_reload_data(s_menu);
        return;
      }
      s_discard_armed = false;
      sess_discard();
      window_stack_remove(s_menu_win, false);
      close_workout();
      break;
    case M_LEAVE:
      sess_save();
      window_stack_remove(s_menu_win, false);
      close_workout();
      break;
  }
}

static void menu_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect frame;
  s_menu_bar = ui_status_bar(root, &frame);
  s_menu = menu_layer_create(frame);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_rows,
    .get_cell_height = g_ui.level ? menu_row_h : NULL,
    .draw_row = menu_draw,
    .select_click = menu_select,
  });
  ui_style_menu(s_menu);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
}

static void menu_unload(Window *w) {
  menu_layer_destroy(s_menu);
  status_bar_layer_destroy(s_menu_bar);
  window_destroy(s_menu_win);
  s_menu_win = NULL;
}

static void menu_open(void) {
  if (s_menu_win) return;
  s_discard_armed = false;
  s_menu_win = window_create();
  window_set_background_color(s_menu_win, C_BG);
  window_set_window_handlers(s_menu_win, (WindowHandlers) { .load = menu_load, .unload = menu_unload });
  window_stack_push(s_menu_win, true);
}

// ---------------------------------------------------------------------------------------------
// Summary after Finish

static Window *s_sum_win;
static TextLayer *s_sum_title, *s_sum_body;
static StatusBarLayer *s_sum_bar;
static char s_sum_text[160];
static const char *s_sum_title_text;
static GColor s_sum_title_color;

static void sum_click(ClickRecognizerRef r, void *ctx) {
  window_stack_remove(s_sum_win, true);
}

static void sum_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, sum_click);
  window_single_click_subscribe(BUTTON_ID_BACK, sum_click);
}

static void sum_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  GRect unused;
  s_sum_bar = ui_status_bar(root, &unused);
  int th = g_ui.level ? 68 : 40;
  s_sum_title = text_layer_create(GRect(10, 30, b.size.w - 20, th));
  text_layer_set_background_color(s_sum_title, GColorClear);
  text_layer_set_text_color(s_sum_title, s_sum_title_color);
  text_layer_set_font(s_sum_title, g_ui.title);
  text_layer_set_text(s_sum_title, s_sum_title_text);
  layer_add_child(root, text_layer_get_layer(s_sum_title));

  // A title that wraps (big text, German) pushes the body down.
  GSize tsz = graphics_text_layout_get_content_size(s_sum_title_text, g_ui.title, GRect(0, 0, b.size.w - 20, th),
                                                    GTextOverflowModeWordWrap, GTextAlignmentLeft);
  int by = 30 + (tsz.h > 40 ? tsz.h + 6 : 46);
  s_sum_body = text_layer_create(GRect(10, by, b.size.w - 20, b.size.h - by - 6));
  text_layer_set_background_color(s_sum_body, GColorClear);
  text_layer_set_text_color(s_sum_body, C_FG);
  text_layer_set_font(s_sum_body, g_ui.level == UI_XL ? g_ui.small : fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text(s_sum_body, s_sum_text);
  layer_add_child(root, text_layer_get_layer(s_sum_body));
}

static void sum_unload(Window *w) {
  text_layer_destroy(s_sum_title);
  text_layer_destroy(s_sum_body);
  status_bar_layer_destroy(s_sum_bar);
  window_destroy(s_sum_win);
  s_sum_win = NULL;
}

static void summary_push(const char *title, GColor title_color) {
  s_sum_win = window_create();
  window_set_background_color(s_sum_win, C_BG);
  window_set_click_config_provider(s_sum_win, sum_click_config);
  window_set_window_handlers(s_sum_win, (WindowHandlers) { .load = sum_load, .unload = sum_unload });
  s_sum_title_text = title;
  s_sum_title_color = title_color;
  window_stack_push(s_sum_win, true);
}

// Saves the workout into the upload queue and shows what was done. The numbers are read
// before sess_finish() clears the session.
static void finish_workout(void) {
  if (s_sum_win) return;
  int sets = sess_count(SET_DONE);
  int secs = time(NULL) - g_sess.start;
  uint32_t vol20 = 0;
  for (int i = 0; i < g_sess.n; i++) {
    SessEntry *e = &g_sess.e[i];
    if (e->mode != MODE_REPS) continue;
    for (int k = 0; k < e->n; k++) {
      if ((e->s[k].flags & (SET_DONE | SET_WARM)) == SET_DONE) vol20 += (uint32_t)e->s[k].w20 * e->s[k].amt;
    }
  }
  bool saved = sess_finish();
  char dur[16];
  fmt_duration(dur, sizeof(dur), secs);
  if (!saved) {
    snprintf(s_sum_text, sizeof(s_sum_text), "%s",
             L("No room left on the watch. Sync with your phone, then finish again.",
               "Kein Platz mehr auf der Uhr. Mit dem Handy synchronisieren, dann erneut beenden."));
    summary_push(L("Not saved", "Nicht gespeichert"), GColorRed);
    return;
  }
  if (sets == 0) {
    snprintf(s_sum_text, sizeof(s_sum_text), "%s",
             L("No sets were done, so nothing was kept.", "Keine Sätze gemacht, nichts gespeichert."));
    summary_push(L("Nothing saved", "Nichts gespeichert"), C_MUTED);
  } else {
    snprintf(s_sum_text, sizeof(s_sum_text), L("%d %s in %s\n%d %s lifted\n\nUploads when your phone is near.",
               "%d %s in %s\n%d %s bewegt\n\nWird hochgeladen, sobald das Handy in der Nähe ist."),
             sets, sets == 1 ? L("set", "Satz") : L("sets", "Sätze"), dur, (int)(vol20 / 20), unit_label());
    vibes_double_pulse();
    summary_push(L("Workout saved", "Training gespeichert"), C_ACCENT);
  }
  if (s_menu_win) window_stack_remove(s_menu_win, false);
  if (s_win) window_stack_remove(s_win, false);
  home_refresh();
  comm_sync();
}
