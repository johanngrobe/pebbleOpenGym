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

// "Satz 2/4", "Aufwärmen 1/2", with " - Links" on a per-side hold. `hold` for the
// running hold, which says so in front.
static void set_label(char *buf, size_t size, const SessEntry *e, int si, bool hold) {
  int warm_sets = sess_warm_rows(e) / rows_per_set(e);
  int no = set_no(e, si), of = is_warm(e, si) ? warm_sets : set_count(e);
  const char *side = side_name(e, si);
  const char *what = is_warm(e, si) ? L("Warm-up", "Aufwärmen") : (hold ? L("Hold - set", "Halten - Satz") : L("Set", "Satz"));
  if (side) snprintf(buf, size, "%s %d/%d - %s", what, no, of, side);
  else snprintf(buf, size, "%s %d/%d", what, no, of);  // a superset's gets a link symbol after it
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
static int64_t now_ms(void) {
  time_t secs;
  uint16_t ms;
  time_ms(&secs, &ms);
  return (int64_t)secs * 1000 + ms;
}

// The rest's end and the hold's start to the millisecond, like Timer+ does it, so the first
// second of a timer is a whole one. The session keeps them in seconds (the rest's end rounded
// up, the hold's start down); after a restart those count, a fraction of a second off.
static int64_t s_rest_end_ms, s_hold_start_ms;

static int64_t rest_end_ms(void) {
  int64_t e = (int64_t)g_sess.rest_end * 1000;
  return s_rest_end_ms > e - 1000 && s_rest_end_ms <= e ? s_rest_end_ms : e;
}

static int64_t hold_start_ms(void) {
  int64_t st = (int64_t)g_sess.hold_start * 1000;
  return s_hold_start_ms >= st && s_hold_start_ms < st + 1000 ? s_hold_start_ms : st;
}

static void set_rest_end_ms(int64_t at) {
  s_rest_end_ms = at;
  g_sess.rest_end = (at + 999) / 1000;
}

static void start_hold(void) {
  s_hold_start_ms = now_ms();
  g_sess.hold_start = s_hold_start_ms / 1000;
}

// Whole seconds as the digits show them: a rest's left rounded up (it ends at 00:00), a hold's
// gone rounded down.
static int rest_left_ms(void) {
  int64_t l = rest_end_ms() - now_ms();
  return l < 0 ? 0 : (int)l;
}
static int rest_left_s(void) { return (rest_left_ms() + 999) / 1000; }
static int hold_gone_ms(void) {
  int64_t g = now_ms() - hold_start_ms();
  return g < 0 ? 0 : (int)g;
}
static int hold_gone_s(void) { return hold_gone_ms() / 1000; }

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
  set_rest_end_ms(now_ms() + (int64_t)secs * 1000);
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

// The digit size of the weight and reps boxes, the same all through the workout: the biggest
// that fits the widest value of the session in a half-width box of `area` height ("62.5" or
// "102.5" does not fit the biggest digits). It only gets smaller when a bigger value comes up.
static int value_step(int box_w, int area) {
  char sample[12] = "0", v[12];
  int sw = 0;
  for (int i = 0; i < g_sess.n; i++) {
    const SessEntry *e = &g_sess.e[i];
    for (int k = 0; k < e->n; k++) {
      fmt_weight(v, sizeof(v), e->s[k].w20);
      if ((int)strlen(v) > sw) { sw = strlen(v); strcpy(sample, v); }
      fmt_amount(v, sizeof(v), e, e->s[k].amt);
      if ((int)strlen(v) > sw) { sw = strlen(v); strcpy(sample, v); }
    }
  }
  // Every digit as wide as the widest one.
  for (char *c = sample; *c; c++) if (*c >= '0' && *c <= '9') *c = '8';
  int i = 0;
  for (; i + 1 < g_ui.n_value; i++) {
    if (g_ui.value_h[i] > area) continue;
    GSize sz = graphics_text_layout_get_content_size(sample, g_ui.value[i], GRect(0, 0, 400, 80),
                                                     GTextOverflowModeWordWrap, GTextAlignmentLeft);
    if (sz.w <= box_w - 8) break;
  }
  return i;
}

static void draw_value_box(GContext *ctx, GRect r, const char *value, const char *label, bool focused,
                           int step) {
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
    f = g_ui.value[step];
    fh = g_ui.value_h[step];
    vy = r.origin.y + (area - fh) / 2 - 3;
  }
  draw_text(ctx, value, f, GRect(r.origin.x, vy, r.size.w, fh + 8), GTextAlignmentCenter, fg);
  draw_text(ctx, label, g_ui.small, GRect(r.origin.x + 2, label_y, r.size.w - 4, g_ui.small_line),
            GTextAlignmentCenter, focused ? C_ON_ACCENT : C_MUTED);
}

static int text_w(const char *text, GFont f);

// A timer as "00:00" (minutes past 99 just grow), never below zero.
static void fmt_timer(char *buf, size_t size, int secs) {
  if (secs < 0) secs = 0;
  snprintf(buf, size, "%02d:%02d", secs / 60, secs % 60);
}

// A timer in the biggest digits that fit across `w`, centred; returns the height it took. Each
// digit sits centred in a slot as wide as the widest one, so the time keeps still as it counts.
static int draw_big(GContext *ctx, const char *text, int y, int w, GColor color) {
  GFont f = g_ui.big[0];
  int h = g_ui.big_h[0], dw = 0, cw = 0;
  for (int i = 0; i < g_ui.n_big; i++) {
    f = g_ui.big[i];
    h = g_ui.big_h[i];
    dw = text_w("8", f) + 2;  // the widest digit, and a little air
    cw = text_w(":", f) + 2;
    int total = 0;
    for (const char *c = text; *c; c++) total += *c == ':' ? cw : dw;
    if (total <= w - 8) break;
  }
  int total = 0;
  for (const char *c = text; *c; c++) total += *c == ':' ? cw : dw;
  int x = (w - total) / 2;
  char ch[2] = {0, 0};
  for (const char *c = text; *c; c++) {
    int slot = *c == ':' ? cw : dw;
    ch[0] = *c;
    draw_text(ctx, ch, f, GRect(x - 4, y, slot + 8, h + 8), GTextAlignmentCenter, color);
    x += slot;
  }
  return h + 6;
}

static int isqrt(int v) {
  int r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

// The timer dial of the rest and hold screens: the whole area under the top bar, left of the
// button hints, is a pie with a plain disc in the middle for the text. The time left is the
// accent colour, clockwise from 12 o'clock; the time gone is dim and grows anti-clockwise from
// 12. Each second, with the digits, the pie glides to its new place in a quarter second at full
// frame rate and then rests until the next one: smooth, and idle in between. It sweeps in when
// the timer starts and glides further after +15 / -15. Without a target (a hold counting up) it
// is all dim and still.
typedef struct { GPoint c; int r; } Disc;

enum { DIAL_FRAME_MS = 25, DIAL_STEP_MS = 250, DIAL_JUMP_MS = 400 };
static AppTimer *s_frame;
static int32_t s_dial = -1;     // the angle of time left on screen, -1 when no dial is up
static int32_t s_glide_from;
static int64_t s_glide_start;
static int s_glide_ms;
static bool s_gliding;

// The angle of `left_ms` out of `total_ms`.
static int32_t dial_target(int64_t left_ms, int64_t total_ms) {
  if (total_ms <= 0 || left_ms <= 0) return 0;
  if (left_ms > total_ms) left_ms = total_ms;
  return (int32_t)(TRIG_MAX_ANGLE * left_ms / total_ms);
}

// The angle to show now for `target`: on the way there when the dial is gliding.
static int32_t dial_angle(int32_t target) {
  int64_t now = now_ms();
  if (s_dial < 0 || (!s_gliding && target != s_dial)) {
    // A jump (the dial coming up from empty, or +15 / -15) takes a little longer than a second's step.
    bool jump = s_dial < 0 || abs(target - s_dial) > TRIG_MAX_ANGLE / 30;
    s_glide_from = s_dial < 0 ? 0 : s_dial;
    s_glide_start = now;
    s_glide_ms = jump ? DIAL_JUMP_MS : DIAL_STEP_MS;
    s_gliding = true;
  }
  int32_t a = target;
  if (s_gliding) {
    int t = now - s_glide_start;
    if (t >= s_glide_ms) {
      s_gliding = false;
    } else {
      int p = t * 1024 / s_glide_ms;
      // Ease in and out.
      p = p < 512 ? 2 * p * p / 1024 : 1024 - 2 * (1024 - p) * (1024 - p) / 1024;
      a = s_glide_from + (int32_t)((int64_t)(target - s_glide_from) * p / 1024);
    }
  }
  s_dial = a;
  return a;
}

static void check_timers(void);

static void dial_frame(void *ctx) {
  s_frame = NULL;
  if (!s_gliding) check_timers();
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void dial_stop(void) {
  if (s_frame) app_timer_cancel(s_frame);
  s_frame = NULL;
  s_dial = -1;
  s_gliding = false;
}

// `target` is the angle of time left, or -1 for a hold without a target; `next_ms` when the
// time shown changes next.
static Disc draw_dial(GContext *ctx, GRect b, int32_t target, int next_ms) {
  int w = b.size.w - 18, y0 = g_ui.top_h, h = b.size.h - y0;
  GPoint c = GPoint(w / 2, y0 + h / 2);
  int r = (w < h ? w : h) / 2 - 6;
  graphics_context_set_fill_color(ctx, C_DIM);
  graphics_fill_rect(ctx, GRect(0, y0, w, h), 0, GCornerNone);
  if (target >= 0) {
    int32_t a = dial_angle(target);
    if (a > 0) {
      int R = isqrt(w * w / 4 + h * h / 4) + 2;  // out to the corners
      graphics_context_set_fill_color(ctx, C_ACCENT);
      graphics_fill_radial(ctx, GRect(c.x - R, c.y - R, 2 * R, 2 * R), GOvalScaleModeFitCircle, R, 0, a);
      // The pie reaches past the area; the top bar and the hints keep the plain background.
      graphics_context_set_fill_color(ctx, C_BG);
      graphics_fill_rect(ctx, GRect(0, 0, b.size.w, y0), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(w, y0, b.size.w - w, h), 0, GCornerNone);
    }
    // Frames while it glides; then a wake-up when the timer's next second is due, which starts the
    // next glide with the digits (the clock's own ticks are off by a fraction of a second).
    if (!s_frame) s_frame = app_timer_register(s_gliding ? DIAL_FRAME_MS : next_ms + 5, dial_frame, NULL);
  }
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_circle(ctx, c, r);
  return (Disc){c, r};
}

// One line of text centred in the disc at y, cut to the disc's width at that height. Returns
// the line height, 0 when the line falls outside the disc.
static int disc_text(GContext *ctx, Disc d, const char *t, GFont f, int y, int lh, GColor color) {
  int top = y + lh / 4 - d.c.y, bottom = y + lh - d.c.y;  // Gothic's glyphs sit low in the box
  int dy = abs(top) > abs(bottom) ? abs(top) : abs(bottom);
  if (dy >= d.r) return 0;
  int w = 2 * isqrt(d.r * d.r - dy * dy) - 8;
  draw_text(ctx, t, f, GRect(d.c.x - w / 2, y, w, lh), GTextAlignmentCenter, color);
  return lh;
}

static int text_w(const char *text, GFont f) {
  return graphics_text_layout_get_content_size(text, f, GRect(0, 0, 400, 40), GTextOverflowModeWordWrap,
                                               GTextAlignmentLeft).w;
}

// A line with its digits in slots as wide as the widest digit, so "Set 1/3" and "Set 3/3" keep
// the same shape. Draws it from x when `draw`; returns its width either way.
static int mono_text(GContext *ctx, const char *t, GFont f, int x, int y, int lh, GColor color, bool draw) {
  int dw = 0;
  char d[2] = {0, 0};
  for (char c = '0'; c <= '9'; c++) {
    d[0] = c;
    int cw = text_w(d, f);
    if (cw > dw) dw = cw;
  }
  int sw = text_w("8 8", f) - text_w("88", f);  // a space, which is not measured at a run's end
  int start = x, n = 0;
  char run[48];
  for (const char *c = t;; c++) {
    bool digit = *c >= '0' && *c <= '9';
    if (digit || *c == ' ' || !*c || n == (int)sizeof(run) - 1) {
      if (n) {
        run[n] = 0;
        int rw = text_w(run, f);
        if (draw) draw_text(ctx, run, f, GRect(x, y, rw + 4, lh), GTextAlignmentLeft, color);
        x += rw;
        n = 0;
      }
      if (!*c) break;
      if (digit) {
        d[0] = *c;
        if (draw) draw_text(ctx, d, f, GRect(x - 2, y, dw + 4, lh), GTextAlignmentCenter, color);
        x += dw;
        continue;
      }
      if (*c == ' ') {
        x += sw;
        continue;
      }
    }
    run[n++] = *c;
  }
  return x - start;
}

// A filled heart s pixels wide and tall, its top left at (x, y): two circles and a point below.
static void draw_heart(GContext *ctx, int x, int y, int s, GColor color) {
  int r = (s + 2) / 4, cx = x + s / 2;
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_circle(ctx, GPoint(cx - r + 1, y + r), r);
  graphics_fill_circle(ctx, GPoint(cx + r - 1, y + r), r);
  GPoint pts[] = {{x, y + r + 1}, {x + s, y + r + 1}, {cx, y + s}};
  GPath path = {.num_points = 3, .points = pts};
  gpath_draw_filled(ctx, &path);
}

// A small pie of the sets done or skipped out of all the workout's sets, filling clockwise
// from 12 o'clock in the accent colour on a dim disc.
static void count_progress(int *done, int *all) {
  *done = *all = 0;
  for (int i = 0; i < g_sess.n; i++) {
    const SessEntry *e = &g_sess.e[i];
    for (int k = 0; k < e->n; k++) {
      (*all)++;
      if (e->s[k].flags & (SET_DONE | SET_SKIP)) (*done)++;
    }
  }
}

static void draw_progress_pie(GContext *ctx, GRect r, int done, int all) {
  graphics_context_set_fill_color(ctx, C_DIM);
  graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, r.size.w / 2, 0, TRIG_MAX_ANGLE);
  if (all > 0 && done > 0) {
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, r.size.w / 2, 0, TRIG_MAX_ANGLE * done / all);
  }
}

static void draw_top(GContext *ctx, GRect b) {
  char dur[12], clk[8], hr[12] = "";
  GFont f = g_ui.top;
  int h = g_ui.top_h + 2;
  fmt_duration(dur, sizeof(dur), time(NULL) - g_sess.start);
  draw_text(ctx, dur, f, GRect(8, 0, 100, h), GTextAlignmentLeft, C_MUTED);
  int right_start = b.size.w - 24;
  // Icons sized and placed to sit level with Gothic's digits.
  int big = g_ui.top_h > 20, s = big ? 13 : 10, hy = big ? 9 : 6;
  if (s_hr > 0) {
    // The number with a heart before it.
    snprintf(hr, sizeof(hr), "%d", s_hr);
    right_start -= text_w(hr, f);
    draw_text(ctx, hr, f, GRect(b.size.w - 124, 0, 100, h), GTextAlignmentRight, C_MUTED);
    right_start -= 3 + s + 1;
    draw_heart(ctx, right_start, hy, s, PBL_IF_COLOR_ELSE(GColorRed, C_MUTED));
    right_start -= 6;
  }
  // The time of day, always in the middle; in 12-hour style without AM/PM.
  time_t now = time(NULL);
  strftime(clk, sizeof(clk), clock_is_24h_style() ? "%H:%M" : "%I:%M", localtime(&now));
  const char *clock = (!clock_is_24h_style() && clk[0] == '0') ? clk + 1 : clk;
  int clock_end = (b.size.w + text_w(clock, f)) / 2;
  draw_text(ctx, clock, f, GRect(0, 0, b.size.w, h), GTextAlignmentCenter, C_FG);
  // The workout's progress left of the heart rate, the sets done or skipped out of all: a little
  // pie or "12/30", as set on the phone. Left out when big text leaves no room by the clock.
  int done, all;
  count_progress(&done, &all);
  if (g_progress_style == PROGRESS_NUMBERS) {
    char p[24];
    snprintf(p, sizeof(p), "%d/%d", done, all);
    int pw = text_w(p, f);
    if (right_start - pw >= clock_end + 6) {
      draw_text(ctx, p, f, GRect(right_start - pw - 4, 0, pw + 8, h), GTextAlignmentCenter, C_MUTED);
    }
  } else {
    int d = s + 2;
    if (right_start - d >= clock_end + 6) draw_progress_pie(ctx, GRect(right_start - d, hy - 1, d, d), done, all);
  }
}

// Two chain links side by side, the superset's sign, left edge at x and level with the small
// font's digits on the text line starting at y.
static const int8_t s_link_size[] = {10, 13, 16}, s_link_top[] = {6, 8, 10};

static int link_level(void) {
  return g_ui.level < 2 ? g_ui.level : 2;
}

static void draw_link(GContext *ctx, int x, int y, GColor color) {
  int lv = link_level();
  int h = s_link_size[lv] * 7 / 10, w = s_link_size[lv], cy = y + s_link_top[lv] + s_link_size[lv] / 2;
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, GRect(x, cy - h / 2, w, h), h / 2);
  graphics_draw_round_rect(ctx, GRect(x + w * 3 / 5, cy - h / 2, w, h), h / 2);
  graphics_context_set_stroke_width(ctx, 1);
}

// One side at a time: a circle with its left half filled, the size of the link symbol.
static void draw_side(GContext *ctx, int x, int y, GColor color) {
  int lv = link_level(), size = s_link_size[lv];
  GRect r = GRect(x, y + s_link_top[lv], size, size);
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_radial(ctx, r, GOvalScaleModeFitCircle, size / 2, TRIG_MAX_ANGLE / 2, TRIG_MAX_ANGLE);
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(x + size / 2, y + s_link_top[lv] + size / 2), size / 2 - 1);
  graphics_context_set_stroke_width(ctx, 1);
}

enum { HEAD_LINK = 1, HEAD_SIDE = 2 };

// The exercise's name under the top bar, and the set line under it, with symbols after it: the
// link for a superset (whose line then stands out on an accent pill) and the half circle for a
// one-sided exercise. Returns where they end.
static int draw_heading(GContext *ctx, const char *name, const char *line, int w, GColor line_color,
                        uint8_t marks) {
  int y = g_ui.top_h;
  int nh = g_ui.name_line * 3;  // long names get three lines; the boxes below are smaller for it
  // Always room for all three, so the boxes below keep their place and size.
  draw_text(ctx, name, g_ui.name, GRect(8, y, w, nh + 4), GTextAlignmentLeft, C_FG);
  y += nh;
  int lv = link_level(), size = s_link_size[lv];
  int lw = mono_text(ctx, line, g_ui.small, 0, 0, g_ui.small_line, line_color, false);
  int end = 8 + lw;  // where the text and its symbols end
  if (marks & HEAD_LINK) end += 6 + size * 8 / 5;
  if (marks & HEAD_SIDE) end += 6 + size;
  GColor fg = line_color;
  if (marks & HEAD_LINK) {
    int cy = y + s_link_top[lv] + size / 2;  // level with the digits
    graphics_context_set_fill_color(ctx, C_ACCENT);
    graphics_fill_rect(ctx, GRect(2, cy - size / 2 - 5, end - 2 + 8, size + 10), (size + 10) / 2, GCornersAll);
    fg = C_ON_ACCENT;
  }
  mono_text(ctx, line, g_ui.small, 8, y, g_ui.small_line, fg, true);
  int x = 8 + lw + 6;
  if (marks & HEAD_LINK) {
    draw_link(ctx, x, y, fg);
    x += size * 8 / 5 + 6;
  }
  if (marks & HEAD_SIDE) draw_side(ctx, x, y, fg);
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
  uint8_t marks = (e->sg && !side_name(e, si) && !is_warm(e, si) ? HEAD_LINK : 0) |
                  (e->flags & PEF_SIDE ? HEAD_SIDE : 0);
  int top = draw_heading(ctx, g_plan.ex[e->ex].name, buf, content_w - 4, C_MUTED, marks) + 6;

  SetRec *s = &e->s[si];
  char wv[12], av[8];
  bool bw = g_plan.ex[e->ex].flags & EXF_BODYWEIGHT;
  if (s->w20 == 0 && bw) strcpy(wv, L("BW", "Eigen"));
  else fmt_weight(wv, sizeof(wv), s->w20);
  fmt_amount(av, sizeof(av), e, s->amt);

  // The boxes take the room down to the bottom (holding Select to switch is in the README).
  int h = b.size.h - top - 6;
  s_boxes = GRect(8, top, content_w - 8, h);
  int bw_ = (content_w - 8 - 6) / 2;
  int step = value_step(bw_, h - g_ui.small_line - 4);
  if (has_weight(e)) {
    draw_value_box(ctx, GRect(8, top, bw_, h), wv, unit_label(), s_focus == FOCUS_WEIGHT, step);
    draw_value_box(ctx, GRect(8 + bw_ + 6, top, bw_, h), av, amount_label(e), s_focus == FOCUS_AMOUNT, step);
  } else {
    draw_value_box(ctx, GRect(8, top, content_w - 8, h), av, amount_label(e), true, step);
  }
  ui_draw_hints(ctx, b, HINT_PLUS, e->mode == MODE_TIME ? HINT_SKIP : HINT_CHECK, HINT_MINUS);
}

// The disc's lines: a label, the big time, and up to two lines under it, centred as a block.
static void draw_dial_text(GContext *ctx, GRect b, Disc d, const char *label, const char *time_text,
                           const char *line1, const char *line2) {
  int sl = g_ui.small_line, big = g_ui.big_h[0] + 6;
  if (g_ui.level) line2 = NULL;  // large text: no room for a fourth line
  int total = sl + big + (line1 ? sl : 0) + (line2 ? sl : 0);
  int y = d.c.y - total / 2 - 4;
  // A little apart from the time; its numbers ("Hold - set 2/3") in even slots.
  int lw = mono_text(ctx, label, g_ui.small_bold, 0, 0, sl, C_ACCENT, false);
  mono_text(ctx, label, g_ui.small_bold, d.c.x - lw / 2, y - 8, sl, C_ACCENT, true);
  y += sl;
  y += draw_big(ctx, time_text, y - 6, b.size.w - 18, C_FG);
  if (line1) y += disc_text(ctx, d, line1, g_ui.small_bold, y, sl, C_FG);
  if (line2) disc_text(ctx, d, line2, g_ui.small, y, sl, C_MUTED);
}

static void draw_hold(GContext *ctx, GRect b) {
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (!e || si < 0) return;
  char label[40], t[12];
  int target = e->s[si].amt;
  int elapsed = hold_gone_s();
  set_label(label, sizeof(label), e, si, true);
  fmt_timer(t, sizeof(t), target > 0 ? target - elapsed : elapsed);
  // The pie steps with the digits; the next step is due when the next second is gone.
  Disc d = draw_dial(ctx, b, target > 0 ? dial_target((int64_t)(target - elapsed) * 1000, (int64_t)target * 1000) : -1,
                     1000 - hold_gone_ms() % 1000);
  draw_dial_text(ctx, b, d, label, t, g_plan.ex[e->ex].name, NULL);
  ui_draw_hints(ctx, b, HINT_NONE, HINT_STOP, HINT_NONE);
}

static void draw_rest(GContext *ctx, GRect b) {
  char t[12], next[48];
  int left = rest_left_s();
  fmt_timer(t, sizeof(t), left);
  // The pie steps with the digits; the next step is due when the rest left drops a second.
  int lms = rest_left_ms();
  Disc d = draw_dial(ctx, b, dial_target((int64_t)left * 1000, (int64_t)g_sess.rest_total * 1000),
                     lms > 0 ? (lms - 1) % 1000 + 1 : 1000);
  // Under the time: what comes next, and its set.
  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  const char *name = NULL;
  next[0] = 0;
  if (e && si >= 0) {
    name = g_plan.ex[e->ex].name;
    set_summary(next, sizeof(next), e, &e->s[si]);
  }
  draw_dial_text(ctx, b, d, L("Rest", "Pause"), t, name, next[0] ? next : NULL);
  ui_draw_hints(ctx, b, HINT_PLUS, HINT_SKIP, HINT_MINUS);
  // "15" under the + and over the -: the seconds they add and take, in the hints' own column.
  GFont lf = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  GRect col = GRect(b.size.w - 18, 0, 18, 16);
  col.origin.y = b.size.h * 22 / 100 + 6;
  draw_text(ctx, "15", lf, col, GTextAlignmentCenter, C_MUTED);
  col.origin.y = b.size.h * 78 / 100 - 22;
  draw_text(ctx, "15", lf, col, GTextAlignmentCenter, C_MUTED);
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
  switch (s_state) {
    case ST_SET: draw_set(ctx, b); break;
    case ST_HOLD: draw_hold(ctx, b); break;
    case ST_REST: draw_rest(ctx, b); break;
    case ST_DONE: draw_done(ctx, b); break;
  }
  if (s_state != ST_REST && s_state != ST_HOLD) dial_stop();
  draw_top(ctx, b);
}

// ---------------------------------------------------------------------------------------------
// Time

// A rest or hold running out, and the nudge 10 s before a rest ends; from the clock's tick and
// from the dial's wake-up on the timer's own seconds, whichever comes first.
static uint32_t s_warned;  // the rest end the nudge was for

static void check_timers(void) {
  if (!g_sess.active) return;
  if (s_state == ST_REST) {
    int left = rest_left_s();
    if (left <= 10 && left > 8 && s_warned != g_sess.rest_end) {
      s_warned = g_sess.rest_end;
      vibe_warning();
    }
    if (rest_left_ms() <= 0) end_rest(true);
  } else if (s_state == ST_HOLD) {
    SessEntry *e = cur_entry();
    int si = sess_cur_set();
    if (e && si >= 0 && e->s[si].amt > 0 && hold_gone_s() >= e->s[si].amt) {
      vibe_timer_done();
      light_enable_interaction();
      complete_set(SET_DONE);
    }
  }
}

static void tick(struct tm *t, TimeUnits changed) {
  if (!g_sess.active) return;
  check_timers();
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
    set_rest_end_ms(rest_end_ms() + 15000);
    int left = rest_left_s();
    if (left > g_sess.rest_total) g_sess.rest_total = left;
    sess_save();
    redraw();
  }
}

static void down_click(ClickRecognizerRef r, void *ctx) {
  if (s_state == ST_SET) adjust(-1);
  else if (s_state == ST_REST) {
    set_rest_end_ms(rest_end_ms() - 15000);
    if (rest_left_ms() <= 0) end_rest(false);
    else { sess_save(); redraw(); }
  }
}

static void select_click(ClickRecognizerRef r, void *ctx) {
  SessEntry *e = cur_entry();
  switch (s_state) {
    case ST_SET:
      if (e && e->mode == MODE_TIME) {
        start_hold();
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
        int elapsed = hold_gone_s();
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
  dial_stop();
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
static StatusBarLayer *s_sum_bar;
static ScrollLayer *s_sum_scroll;
static Layer *s_sum_content;

// What the summary shows, taken from the session before sess_finish() clears it (and copied,
// as a sync may replace the plan while the summary is up).
typedef struct {
  char name[EX_NAME_LEN];
  char detail[40];
} SumRow;

static struct {
  bool stats;              // a saved workout with numbers; else just a title and a text
  const char *title;
  GColor title_color;
  char text[160];          // the text when there are no stats
  char routine[R_NAME_LEN];
  int secs, sets, reps, n_ex;
  uint32_t vol20;          // work sets only, as openGym counts it
  char top_name[EX_NAME_LEN];
  uint16_t top_w20, top_amt;  // the heaviest work set, 0 = none
  int hr_avg, hr_max, kcal;   // from Pebble Health, 0 = not known
  uint8_t n_rows;
  SumRow rows[MAX_ENTRIES];
} s_sum;

// The celebration on a saved workout, a little cartoon in Pebble's own manner: a jelly blob
// with a dumbbell drops in surprised and lands with a wobble, strains and sweats, snaps the
// weight overhead, grins under popping stars, hops, then rockets off the top and the summary
// slides up in its place. Buzzes on the beats; any button cuts it short.
#define CELEB_MS 2350
#define T_LAND 350      // hits the ground
#define T_STRAIN 750    // starts to lift
#define T_PUSH 950      // pushes it up
#define T_TOP 1150      // weight overhead, stars
#define T_HOP 1300      // a happy hop...
#define T_HOP_LAND 1560
#define T_CROUCH 1760   // ...and away
#define T_LAUNCH 1880
#define T_TEXT 2060     // the summary slides in

static bool s_sum_celebrate;
static Layer *s_celeb;
static Animation *s_celeb_anim;
static int s_celeb_ms;   // how far the celebration is
static int s_celeb_prev; // for the buzzes on the beats
static bool s_sum_closing;

// 0..1000 through [a, b] at time t.
static int phase(int t, int a, int b) {
  if (t <= a) return 0;
  if (t >= b) return 1000;
  return (t - a) * 1000 / (b - a);
}
static int ease_out(int p) { return 1000 - (1000 - p) * (1000 - p) / 1000; }
static int ease_in(int p) { return p * p / 1000; }
// Overshoots a little before settling.
static int ease_out_back(int p) {
  int64_t q = p - 1000;
  return (int)(1000 + 27 * q * q * q / 10000000 + 17 * q * q / 10000);
}
// A jelly wobble set off at t0: `amp` at once, swinging back and forth and dying away.
static int wobble(int t, int t0, int amp) {
  int d = t - t0;
  if (d < 0 || d > 600) return 0;
  int decay = (600 - d) * (600 - d) / 360;  // 1000 .. 0
  return amp * decay / 1000 * cos_lookup(TRIG_MAX_ANGLE * d / 280) / TRIG_MAX_RATIO;
}

#define OUTLINE GColorBlack

static void ellipse(GContext *ctx, GPoint c, int rx, int ry, GColor fill, int stroke) {
  GPoint pts[24];
  for (int i = 0; i < 24; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / 24;
    pts[i] = GPoint(c.x + cos_lookup(a) * rx / TRIG_MAX_RATIO, c.y + sin_lookup(a) * ry / TRIG_MAX_RATIO);
  }
  GPath path = { .num_points = 24, .points = pts };
  graphics_context_set_fill_color(ctx, fill);
  gpath_draw_filled(ctx, &path);
  if (stroke) {
    graphics_context_set_stroke_color(ctx, OUTLINE);
    graphics_context_set_stroke_width(ctx, stroke);
    gpath_draw_outline(ctx, &path);
  }
}

// A thick line with a dark outline, like a limb.
static void limb(GContext *ctx, GPoint a, GPoint b, int w, GColor color) {
  graphics_context_set_stroke_color(ctx, OUTLINE);
  graphics_context_set_stroke_width(ctx, w + 4);
  graphics_draw_line(ctx, a, b);
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_stroke_width(ctx, w);
  graphics_draw_line(ctx, a, b);
}

static void outlined_rect(GContext *ctx, GRect r, GColor fill) {
  graphics_context_set_fill_color(ctx, OUTLINE);
  graphics_fill_rect(ctx, grect_inset(r, GEdgeInsets(-2)), 3, GCornersAll);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, r, 2, GCornersAll);
}

// The dumbbell, its middle at c, tilted by `tilt` px end to end.
static void dumbbell(GContext *ctx, GPoint c, int tilt) {
  GPoint l = GPoint(c.x - 46, c.y + tilt / 2), r = GPoint(c.x + 46, c.y - tilt / 2);
  limb(ctx, l, r, 4, GColorLightGray);
  GColor plate = COLOR_FALLBACK(GColorFolly, C_FG);
  for (int s = -1; s <= 1; s += 2) {
    GPoint e = s < 0 ? l : r;
    outlined_rect(ctx, GRect(e.x - 5 + s * 1, e.y - 12, 10, 24), plate);
    outlined_rect(ctx, GRect(e.x + s * 8 - 3, e.y - 8, 6, 16), plate);
  }
}

// A four-pointed sparkle star.
static void star(GContext *ctx, GPoint c, int size) {
  if (size <= 1) return;
  GPoint pts[8];
  for (int i = 0; i < 8; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / 8;
    int d = i % 2 ? size * 2 / 5 : size;
    pts[i] = GPoint(c.x + sin_lookup(a) * d / TRIG_MAX_RATIO, c.y - cos_lookup(a) * d / TRIG_MAX_RATIO);
  }
  GPath path = { .num_points = 8, .points = pts };
  graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorYellow, C_FG));
  gpath_draw_filled(ctx, &path);
  graphics_context_set_stroke_color(ctx, OUTLINE);
  graphics_context_set_stroke_width(ctx, 2);
  gpath_draw_outline(ctx, &path);
}

static void celeb_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int t = s_celeb_ms;
  int cx = b.size.w / 2;
  int ground = b.size.h * 66 / 100;
  graphics_context_set_antialiased(ctx, true);

  // Where the blob is: dropping in, hopping, rocketing off.
  int yoff = 0;
  if (t < T_LAND) yoff = -(ground + 40) * (1000 - ease_in(phase(t, 0, T_LAND))) / 1000;
  else if (t >= T_HOP && t < T_HOP_LAND) { int p = phase(t, T_HOP, T_HOP_LAND); yoff = -18 * 4 * p / 1000 * (1000 - p) / 1000; }
  else if (t >= T_LAUNCH) yoff = -(ground + 120) * ease_in(phase(t, T_LAUNCH, T_LAUNCH + 260)) / 1000;

  // Its squash and stretch, in thousandths of its height.
  int sy = 1000;
  if (t < T_LAND) sy = 1000 + 200 * phase(t, 0, T_LAND) / 1000;
  sy -= wobble(t, T_LAND, 320);
  if (t >= T_STRAIN && t < T_PUSH) sy -= 150 * ease_out(phase(t, T_STRAIN, T_PUSH)) / 1000;
  if (t >= T_PUSH) sy -= wobble(t, T_PUSH, 150);
  sy -= wobble(t, T_HOP, 120) + wobble(t, T_HOP_LAND, 180);
  if (t >= T_CROUCH && t < T_LAUNCH) sy -= 280 * ease_out(phase(t, T_CROUCH, T_LAUNCH)) / 1000;
  if (t >= T_LAUNCH) sy = 1000 + 450 * ease_out(phase(t, T_LAUNCH, T_LAUNCH + 120)) / 1000;
  int sx = 1000000 / sy;  // the jelly keeps its volume
  int w = 64 * sx / 1000, h = 56 * sy / 1000;
  int jit = (t >= T_STRAIN && t < T_PUSH) ? ((t / 40) % 2 ? 1 : -1) : 0;  // trembling with effort
  int bottom = ground + yoff;
  int top = bottom - h;
  GPoint bc = GPoint(cx + jit, bottom - h / 2);

  // Its shadow, small while it is high up.
  int near = 1000 - (yoff < 0 ? (-yoff * 1000 / (ground + 40)) : 0);
  if (near > 0 && t < T_TEXT) {
    int sw = (20 + 16 * near / 1000) * sx / 1000;
    graphics_context_set_fill_color(ctx, C_DIM);
    graphics_fill_rect(ctx, GRect(cx - sw, ground + 2, sw * 2, 5), 2, GCornersAll);
  }

  // Dust puffing out where it lands.
  int dp = phase(t, T_LAND, T_LAND + 300);
  if (dp > 0 && dp < 1000) {
    int r = 6 * (1000 - dp) / 1000 + 1, d = 30 + 26 * ease_out(dp) / 1000;
    graphics_context_set_fill_color(ctx, C_MUTED);
    graphics_fill_circle(ctx, GPoint(cx - d, ground - 2 - 6 * dp / 1000), r);
    graphics_fill_circle(ctx, GPoint(cx + d, ground - 2 - 6 * dp / 1000), r);
    graphics_fill_circle(ctx, GPoint(cx - d + 12, ground - 4 * dp / 1000), r * 2 / 3);
    graphics_fill_circle(ctx, GPoint(cx + d - 12, ground - 4 * dp / 1000), r * 2 / 3);
  }

  // Speed lines as it rockets away.
  if (t >= T_LAUNCH) {
    int p = phase(t, T_LAUNCH, CELEB_MS);
    graphics_context_set_stroke_color(ctx, C_MUTED);
    graphics_context_set_stroke_width(ctx, 3);
    for (int i = -1; i <= 1; i++) {
      int y0 = bottom + 8 + (i ? 10 : 0), len = (40 - (i ? 12 : 0)) * (1000 - p) / 1000;
      if (len > 2) graphics_draw_line(ctx, GPoint(cx + i * 18, y0), GPoint(cx + i * 18, y0 + len));
    }
  }

  // The weight: hugged at the chest, then hauled overhead (and wiggled about in triumph).
  int lift = 0;
  if (t >= T_STRAIN && t < T_PUSH) lift = 90 * phase(t, T_STRAIN, T_PUSH) / 1000;
  else if (t >= T_PUSH) lift = 90 + 910 * ease_out_back(phase(t, T_PUSH, T_TOP)) / 1000;
  int chest_y = top + h * 55 / 100, over_y = top - 26;
  GPoint bar = GPoint(bc.x, chest_y + (over_y - chest_y) * lift / 1000);
  int tilt = (t >= T_TOP && t < T_CROUCH) ? 8 * sin_lookup(TRIG_MAX_ANGLE * (t - T_TOP) / 300) / TRIG_MAX_RATIO : 0;
  if (t >= T_STRAIN && t < T_PUSH) tilt = jit * 4;
  dumbbell(ctx, bar, tilt);

  // Arms up to the bar.
  for (int s = -1; s <= 1; s += 2) {
    GPoint shoulder = GPoint(bc.x + s * w * 40 / 100, top + h * 42 / 100);
    GPoint hand = GPoint(bar.x + s * 34, bar.y - s * tilt * 34 / 92);
    limb(ctx, shoulder, hand, 6, C_ACCENT);
    ellipse(ctx, hand, 5, 5, C_ACCENT, 2);
  }

  // The blob.
  ellipse(ctx, bc, w / 2, h / 2, C_ACCENT, 3);

  // Its face.
  int ex = w * 19 / 100, ey = top + h * 38 / 100, my = top + h * 66 / 100;
  GColor f = C_ON_ACCENT;
  graphics_context_set_stroke_color(ctx, f);
  graphics_context_set_fill_color(ctx, f);
  graphics_context_set_stroke_width(ctx, 3);
  bool triumph = t >= T_TOP;
  if (triumph) {
    // ^ ^ and a big open grin, with rosy cheeks.
    for (int s = -1; s <= 1; s += 2) {
      GPoint e = GPoint(bc.x + s * ex, ey);
      graphics_draw_line(ctx, GPoint(e.x - 5, e.y + 3), GPoint(e.x, e.y - 3));
      graphics_draw_line(ctx, GPoint(e.x, e.y - 3), GPoint(e.x + 5, e.y + 3));
      graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorMelon, f));
      graphics_fill_circle(ctx, GPoint(bc.x + s * (ex + 9), my - 3), 4);
    }
    graphics_context_set_fill_color(ctx, OUTLINE);
    graphics_fill_radial(ctx, GRect(bc.x - 10, my - 10, 20, 20), GOvalScaleModeFitCircle, 10,
                         TRIG_MAX_ANGLE / 4, TRIG_MAX_ANGLE * 3 / 4);
    graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorFolly, f));
    graphics_fill_radial(ctx, GRect(bc.x - 5, my + 2, 10, 10), GOvalScaleModeFitCircle, 5,
                         TRIG_MAX_ANGLE / 4, TRIG_MAX_ANGLE * 3 / 4);
  } else if (t >= T_STRAIN) {
    // > <, a clenched wavy mouth, and a drop of sweat flying off.
    for (int s = -1; s <= 1; s += 2) {
      GPoint e = GPoint(bc.x + s * ex, ey);
      graphics_draw_line(ctx, GPoint(e.x - s * 4, e.y - 4), GPoint(e.x + s * 3, e.y));
      graphics_draw_line(ctx, GPoint(e.x + s * 3, e.y), GPoint(e.x - s * 4, e.y + 4));
    }
    for (int i = 0; i < 4; i++)
      graphics_draw_line(ctx, GPoint(bc.x - 8 + i * 4, my + (i % 2 ? 2 : -1)),
                         GPoint(bc.x - 4 + i * 4, my + (i % 2 ? -1 : 2)));
    int sp = phase(t, T_STRAIN + 60, T_TOP);
    if (sp > 0 && sp < 1000) {
      GPoint d = GPoint(bc.x + w / 2 + 18 * sp / 1000, top + 4 - 14 * sp / 1000 + 30 * sp / 1000 * sp / 1000);
      graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorPictonBlue, f));
      graphics_fill_circle(ctx, d, 3);
      graphics_fill_circle(ctx, GPoint(d.x - 1, d.y - 3), 2);
    }
  } else if (t < T_LAND) {
    // Wide-eyed, mouth an "o" on the way down.
    for (int s = -1; s <= 1; s += 2) graphics_fill_circle(ctx, GPoint(bc.x + s * ex, ey), 5);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, GPoint(bc.x, my + 1), 4);
  } else {
    // Content, with one blink.
    bool blink = t > 560 && t < 620;
    for (int s = -1; s <= 1; s += 2) {
      if (blink) graphics_draw_line(ctx, GPoint(bc.x + s * ex - 4, ey), GPoint(bc.x + s * ex + 4, ey));
      else graphics_fill_circle(ctx, GPoint(bc.x + s * ex, ey), 4);
    }
    graphics_draw_arc(ctx, GRect(bc.x - 8, my - 10, 16, 16), GOvalScaleModeFitCircle,
                      TRIG_MAX_ANGLE * 3 / 8, TRIG_MAX_ANGLE * 5 / 8);
  }

  // Stars popping around the lifted weight, twinkling, gone as it leaves.
  if (t >= T_TOP) {
    static const int8_t pos[][3] = { { -70, -70, 12 }, { 68, -58, 10 }, { -52, -110, 8 }, { 54, -104, 11 }, { -78, -20, 7 }, { 80, -14, 8 } };
    int fade = 1000 - ease_in(phase(t, T_CROUCH, T_LAUNCH + 80));
    for (int i = 0; i < (int)ARRAY_LENGTH(pos); i++) {
      int pop = ease_out_back(phase(t, T_TOP + i * 50, T_TOP + i * 50 + 220));
      int tw = 1000 + 180 * sin_lookup(TRIG_MAX_ANGLE * (t + i * 90) / 240) / TRIG_MAX_RATIO;
      star(ctx, GPoint(cx + pos[i][0], ground - 30 + pos[i][1]), pos[i][2] * pop / 1000 * tw / 1000 * fade / 1000);
    }
  }
}

// The summary slides up into place; 1000 is home.
static void sum_text_at(int p) {
  int off = 60 * (1000 - ease_out_back(p)) / 1000;
  GRect f = layer_get_frame(s_sum_content);
  f.origin.y = off;
  layer_set_frame(s_sum_content, f);
  layer_set_hidden(s_sum_content, false);
}

static void buzz(uint32_t ms) {
  static uint32_t seg[1];
  seg[0] = ms;
  vibes_enqueue_custom_pattern((VibePattern) { .durations = seg, .num_segments = 1 });
}

static void celeb_anim_update(Animation *a, const AnimationProgress progress) {
  int t = (int)((int64_t)progress * CELEB_MS / ANIMATION_NORMALIZED_MAX);
  // Buzzes on the beats: the landing, the weight going up, the hop, the take-off.
  int prev = s_celeb_prev;
  if (prev < T_LAND && t >= T_LAND) buzz(40);
  if (prev < T_TOP && t >= T_TOP) buzz(90);
  if (prev < T_HOP_LAND && t >= T_HOP_LAND) buzz(30);
  if (prev < T_LAUNCH && t >= T_LAUNCH) buzz(60);
  s_celeb_prev = s_celeb_ms = t;
  if (t >= T_TEXT) sum_text_at(phase(t, T_TEXT, CELEB_MS));
  layer_mark_dirty(s_celeb);
}

static void celeb_anim_stopped(Animation *a, bool finished, void *ctx) {
  s_celeb_anim = NULL;
  if (s_sum_closing) return;
  layer_set_hidden(s_celeb, true);
  sum_text_at(1000);
}

static const AnimationImplementation s_celeb_impl = { .update = celeb_anim_update };

static void celeb_start(void) {
  layer_set_hidden(s_sum_content, true);
  s_celeb_ms = s_celeb_prev = 0;
  s_celeb_anim = animation_create();
  animation_set_duration(s_celeb_anim, CELEB_MS);
  animation_set_curve(s_celeb_anim, AnimationCurveLinear);
  animation_set_implementation(s_celeb_anim, &s_celeb_impl);
  animation_set_handlers(s_celeb_anim, (AnimationHandlers) { .stopped = celeb_anim_stopped }, NULL);
  animation_schedule(s_celeb_anim);
}

static void sum_click(ClickRecognizerRef r, void *ctx) {
  if (s_celeb_anim) {  // the first press only skips the celebration
    animation_unschedule(s_celeb_anim);
    return;
  }
  window_stack_remove(s_sum_win, true);
}

static void sum_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, sum_click);
  window_single_click_subscribe(BUTTON_ID_BACK, sum_click);
}

// Text at (x, y), wrapped to w; draws it when there is a ctx, returns its height either way.
// Gothic leaves room above its letters, so the box starts a little higher than the text.
static int sum_text(GContext *ctx, const char *t, GFont f, int x, int y, int w, GTextAlignment al, GColor c, bool wrap) {
  int pad = f == g_ui.title ? 6 : 4;
  GRect r = GRect(x, y - pad, w, wrap ? 400 : 40);
  GSize sz = graphics_text_layout_get_content_size(t, f, r, wrap ? GTextOverflowModeWordWrap : GTextOverflowModeTrailingEllipsis, al);
  if (!wrap) {
    int line = f == g_ui.title ? 32 : f == g_ui.small || f == g_ui.small_bold ? g_ui.small_line : sz.h + 4;
    sz.h = line;
  }
  if (ctx) {
    r.size.h = sz.h + pad + 4;
    graphics_context_set_text_color(ctx, c);
    graphics_draw_text(ctx, t, f, r, wrap ? GTextOverflowModeWordWrap : GTextOverflowModeTrailingEllipsis, al, NULL);
  }
  return sz.h - (wrap ? pad : 0) + (wrap ? 4 : 0);
}

// A section's name with a hairline after it.
static int sum_section(GContext *ctx, const char *t, int x, int y, int w) {
  GFont f = g_ui.small_bold;
  int h = sum_text(ctx, t, f, x, y, w, GTextAlignmentLeft, C_ACCENT, false);
  if (ctx) {
    int tw = text_w(t, f) + 6;
    graphics_context_set_stroke_color(ctx, C_DIM);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(x + tw, y + h / 2), GPoint(x + w, y + h / 2));
  }
  return h + 2;
}

// The whole summary, top to bottom; drawn when there is a ctx. Returns its height.
static int sum_layout(GContext *ctx, int width) {
  const int x = 8, w = width - 16;
  int y = 6;
  bool big = g_ui.level != UI_NORMAL;
  GTextAlignment mid = GTextAlignmentCenter;
  if (!s_sum.stats) {
    y += sum_text(ctx, s_sum.title, g_ui.title, x, y, w, GTextAlignmentLeft, s_sum.title_color, true) + 4;
    y += sum_text(ctx, s_sum.text, big ? g_ui.small : fonts_get_system_font(FONT_KEY_GOTHIC_24), x, y, w,
                  GTextAlignmentLeft, C_FG, true);
    return y + 8;
  }

  y += sum_text(ctx, s_sum.title, g_ui.title, x, y, w, mid, C_ACCENT, true);
  if (s_sum.routine[0]) y += sum_text(ctx, s_sum.routine, g_ui.small, x, y, w, mid, C_MUTED, false);
  y += 8;

  // The headline numbers in tiles: two across, or one across in big text.
  char val[5][12];
  const char *lab[5];
  int n = 0;
  if (s_sum.secs < 3600) {
    snprintf(val[n], 12, "%d", s_sum.secs / 60);
    lab[n++] = L("minutes", "Minuten");
  } else {
    fmt_duration(val[n], 12, s_sum.secs);
    lab[n++] = L("hours", "Stunden");
  }
  snprintf(val[n], 12, "%d", s_sum.sets);
  lab[n++] = s_sum.sets == 1 ? L("set", "Satz") : L("sets", "Sätze");
  static char vol_label[24];
  if (s_sum.vol20 > 0) {
    snprintf(val[n], 12, "%d", (int)(s_sum.vol20 / 20));
    snprintf(vol_label, sizeof(vol_label), L("%s lifted", "%s bewegt"), unit_label());
    lab[n++] = vol_label;
  }
  if (s_sum.reps > 0) {
    snprintf(val[n], 12, "%d", s_sum.reps);
    lab[n++] = L("reps", "Wdh.");
  }
  if (n < 4) {
    snprintf(val[n], 12, "%d", s_sum.n_ex);
    lab[n++] = s_sum.n_ex == 1 ? L("exercise", "Übung") : L("exercises", "Übungen");
  }
  if (n > 4) n = 4;
  int cols = big ? 1 : 2, gap = 6;
  GFont vf = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  // Big text: one stat a row, the number with its label beside it.
  int th = big ? 38 : 26 + g_ui.small_line + 6;
  for (int i = 0; i < n; i++) {
    int c = i % cols;
    bool lone = cols == 2 && c == 0 && i == n - 1;  // an odd last tile takes the whole row
    int tw = lone ? w : (w - gap * (cols - 1)) / cols;
    int tx = x + c * (tw + gap);
    if (ctx) {
      graphics_context_set_stroke_color(ctx, i == 0 ? C_ACCENT : C_DIM);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_round_rect(ctx, GRect(tx + 1, y + 1, tw - 2, th - 2), 6);
      if (big) {
        int vw = text_w(val[i], vf);
        sum_text(ctx, val[i], vf, tx + 10, y + 4, vw + 4, GTextAlignmentLeft, C_FG, false);
        sum_text(ctx, lab[i], g_ui.small, tx + 18 + vw, y + 4 + (g_ui.level == UI_XL ? 0 : 3), tw - vw - 24,
                 GTextAlignmentLeft, C_MUTED, false);
      } else {
        sum_text(ctx, val[i], vf, tx, y + 2, tw, mid, C_FG, false);
        sum_text(ctx, lab[i], g_ui.small, tx + 2, y + 28, tw - 4, mid, C_MUTED, false);
      }
    }
    if (c == cols - 1 || lone || i == n - 1) y += th + gap;
  }

  // Heart and energy, from Pebble Health.
  if (s_sum.hr_avg > 0 || s_sum.kcal > 0) {
    y += 4;
    char buf[40];
    int lh = g_ui.small_line, s = big ? 13 : 10;
    if (s_sum.hr_avg > 0) {
      if (s_sum.hr_max > 0) snprintf(buf, sizeof(buf), L("%d avg · %d max", "Ø %d · max %d"), s_sum.hr_avg, s_sum.hr_max);
      else snprintf(buf, sizeof(buf), L("%d avg bpm", "Ø %d bpm"), s_sum.hr_avg);
      if (ctx) draw_heart(ctx, x + 1, y + (lh - s) / 2 + 1, s, COLOR_FALLBACK(GColorRed, C_FG));
      y += sum_text(ctx, buf, g_ui.small_bold, x + s + 7, y, w - s - 7, GTextAlignmentLeft, C_FG, false);
    }
    if (s_sum.kcal > 0) {
      snprintf(buf, sizeof(buf), L("%d kcal burned", "%d kcal verbrannt"), s_sum.kcal);
      if (ctx) {
        // A little flame, with a yellow heart.
        int fy = y + (lh - s) / 2;
        static const int8_t outer[][2] = { {5,10}, {1,8}, {0,5}, {2,1}, {3,4}, {5,0}, {8,3}, {10,6}, {9,9} };
        static const int8_t inner[][2] = { {5,10}, {3,8}, {4,5}, {5,6}, {6,4}, {7,8} };
        GPoint po[9], pi[6];
        for (int k = 0; k < 9; k++) po[k] = GPoint(x + 1 + outer[k][0] * s / 10, fy + 1 + outer[k][1] * s / 10);
        for (int k = 0; k < 6; k++) pi[k] = GPoint(x + 1 + inner[k][0] * s / 10, fy + 1 + inner[k][1] * s / 10);
        GPath path = { .num_points = 9, .points = po };
        graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorOrange, C_FG));
        gpath_draw_filled(ctx, &path);
        path = (GPath) { .num_points = 6, .points = pi };
        graphics_context_set_fill_color(ctx, COLOR_FALLBACK(GColorChromeYellow, C_BG));
        gpath_draw_filled(ctx, &path);
      }
      y += sum_text(ctx, buf, g_ui.small_bold, x + s + 7, y, w - s - 7, GTextAlignmentLeft, C_FG, false);
    }
  }

  // The heaviest work set.
  if (s_sum.top_w20 > 0) {
    y += 8;
    y += sum_section(ctx, L("Top set", "Bester Satz"), x, y, w);
    y += sum_text(ctx, s_sum.top_name, g_ui.small_bold, x, y, w, GTextAlignmentLeft, C_FG, false);
    char wt[12], buf[32];
    fmt_weight(wt, sizeof(wt), s_sum.top_w20);
    snprintf(buf, sizeof(buf), "%s %s x %d", wt, unit_label(), s_sum.top_amt);
    y += sum_text(ctx, buf, g_ui.name, x, y, w, GTextAlignmentLeft, C_ACCENT, false);
  }

  // Exercise by exercise.
  if (s_sum.n_rows > 0) {
    y += 8;
    y += sum_section(ctx, L("Exercises", "Übungen"), x, y, w);
    for (int i = 0; i < s_sum.n_rows; i++) {
      y += sum_text(ctx, s_sum.rows[i].name, g_ui.small_bold, x, y, w, GTextAlignmentLeft, C_FG, false);
      y += sum_text(ctx, s_sum.rows[i].detail, g_ui.small, x, y, w, GTextAlignmentLeft, C_MUTED, false) + 4;
    }
  }

  y += 6;
  y += sum_text(ctx, L("Uploads to openGym when your phone is near.", "Wird zu openGym hochgeladen, sobald das Handy in der Nähe ist."),
                g_ui.small, x, y, w, mid, C_MUTED, true);
  return y + 10;
}

static void sum_content_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_antialiased(ctx, true);
  sum_layout(ctx, layer_get_bounds(layer).size.w);
}

static void sum_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect b = layer_get_bounds(root);
  GRect frame;
  s_sum_bar = ui_status_bar(root, &frame);
  s_sum_scroll = scroll_layer_create(frame);
  scroll_layer_set_shadow_hidden(s_sum_scroll, true);
  int h = sum_layout(NULL, frame.size.w);
  s_sum_content = layer_create(GRect(0, 0, frame.size.w, h));
  layer_set_update_proc(s_sum_content, sum_content_proc);
  scroll_layer_add_child(s_sum_scroll, s_sum_content);
  scroll_layer_set_content_size(s_sum_scroll, GSize(frame.size.w, h));
  scroll_layer_set_click_config_onto_window(s_sum_scroll, w);
  scroll_layer_set_callbacks(s_sum_scroll, (ScrollLayerCallbacks) { .click_config_provider = sum_click_config });
  layer_add_child(root, scroll_layer_get_layer(s_sum_scroll));

  s_sum_closing = false;
  s_celeb = layer_create(b);
  layer_set_update_proc(s_celeb, celeb_update_proc);
  layer_set_hidden(s_celeb, !s_sum_celebrate);
  layer_add_child(root, s_celeb);
  if (s_sum_celebrate) celeb_start();
}

static void sum_unload(Window *w) {
  s_sum_closing = true;
  if (s_celeb_anim) animation_unschedule(s_celeb_anim);
  layer_destroy(s_celeb);
  s_celeb = NULL;
  layer_destroy(s_sum_content);
  scroll_layer_destroy(s_sum_scroll);
  status_bar_layer_destroy(s_sum_bar);
  window_destroy(s_sum_win);
  s_sum_win = NULL;
}

// Shows s_sum; `celebrate` plays the celebration (and its buzzes) before it.
static void summary_push(bool celebrate) {
  s_sum_win = window_create();
  window_set_background_color(s_sum_win, C_BG);
  window_set_window_handlers(s_sum_win, (WindowHandlers) { .load = sum_load, .unload = sum_unload });
  s_sum_celebrate = celebrate;
  window_stack_push(s_sum_win, true);
}

static void summary_message(const char *title, GColor color, const char *text) {
  s_sum.stats = false;
  s_sum.title = title;
  s_sum.title_color = color;
  snprintf(s_sum.text, sizeof(s_sum.text), "%s", text);
  summary_push(false);
}

static int plural(int n, const char *one, const char *more, char *buf, size_t size) {
  return snprintf(buf, size, "%d %s", n, n == 1 ? one : more);
}

// The numbers for the summary, read from the session.
static void summary_collect(void) {
  memset(&s_sum, 0, sizeof(s_sum));
  s_sum.stats = true;
  s_sum.title = L("Workout saved", "Training gespeichert");
  time_t now = time(NULL);
  s_sum.secs = now - g_sess.start;
  s_sum.sets = sess_count(SET_DONE);
  if (g_sess.routine < g_plan.n_routines) snprintf(s_sum.routine, sizeof(s_sum.routine), "%s", g_plan.r[g_sess.routine].name);
  for (int i = 0; i < g_sess.n; i++) {
    SessEntry *e = &g_sess.e[i];
    int done = 0, total = 0;
    uint16_t top_w = 0, top_a = 0;
    for (int k = 0; k < e->n; k++) {
      SetRec *r = &e->s[k];
      if (!(r->flags & SET_DONE)) continue;
      done++;
      total += r->amt;
      if (e->mode != MODE_REPS) continue;
      s_sum.reps += r->amt;
      if (r->flags & SET_WARM) continue;
      s_sum.vol20 += (uint32_t)r->w20 * r->amt;
      if (r->w20 > top_w || (r->w20 == top_w && r->amt > top_a)) { top_w = r->w20; top_a = r->amt; }
    }
    if (!done) continue;
    s_sum.n_ex++;
    const char *name = g_plan.ex[e->ex].name;
    if (top_w > s_sum.top_w20 || (top_w == s_sum.top_w20 && top_a > s_sum.top_amt)) {
      s_sum.top_w20 = top_w;
      s_sum.top_amt = top_a;
      snprintf(s_sum.top_name, sizeof(s_sum.top_name), "%s", name);
    }
    if (s_sum.n_rows >= MAX_ENTRIES) continue;
    SumRow *row = &s_sum.rows[s_sum.n_rows++];
    snprintf(row->name, sizeof(row->name), "%s", name);
    char *d = row->detail;
    size_t ds = sizeof(row->detail);
    int sets = done / rows_per_set(e);
    int len = plural(sets, L("set", "Satz"), L("sets", "Sätze"), d, ds);
    switch (e->mode) {
      case MODE_REPS:
        if (top_w > 0) {
          char wt[12];
          fmt_weight(wt, sizeof(wt), top_w);
          snprintf(d + len, ds - len, " · %s %s x %d", wt, unit_label(), top_a);
        } else {
          snprintf(d + len, ds - len, L(" · %d reps", " · %d Wdh."), total);
        }
        break;
      case MODE_TIME:
        snprintf(d + len, ds - len, " · %d:%02d min", total / 60, total % 60);
        break;
      case MODE_CARDIO:
        snprintf(d + len, ds - len, " · %d min", total);
        break;
      case MODE_DIST:
        if (total >= 1000) snprintf(d + len, ds - len, " · %d.%d km", total / 1000, (total % 1000) / 100);
        else snprintf(d + len, ds - len, " · %d m", total);
        break;
    }
  }
#if defined(PBL_HEALTH)
  time_t from = g_sess.start;
  if (health_service_metric_aggregate_averaged_accessible(HealthMetricHeartRateBPM, from, now, HealthAggregationAvg,
                                                          HealthServiceTimeScopeOnce) & HealthServiceAccessibilityMaskAvailable) {
    s_sum.hr_avg = health_service_aggregate_averaged(HealthMetricHeartRateBPM, from, now, HealthAggregationAvg,
                                                     HealthServiceTimeScopeOnce);
    s_sum.hr_max = health_service_aggregate_averaged(HealthMetricHeartRateBPM, from, now, HealthAggregationMax,
                                                     HealthServiceTimeScopeOnce);
  }
  if (health_service_metric_accessible(HealthMetricActiveKCalories, from, now) & HealthServiceAccessibilityMaskAvailable) {
    s_sum.kcal = health_service_sum(HealthMetricActiveKCalories, from, now);
  }
  if (s_sum.hr_avg < 0) s_sum.hr_avg = 0;
  if (s_sum.hr_max < s_sum.hr_avg) s_sum.hr_max = 0;
  if (s_sum.kcal < 0) s_sum.kcal = 0;
#endif
}

// Saves the workout into the upload queue and shows what was done. The numbers are read
// before sess_finish() clears the session.
static void finish_workout(void) {
  if (s_sum_win) return;
  summary_collect();
  int sets = s_sum.sets;
  bool saved = sess_finish();
  if (!saved) {
    summary_message(L("Not saved", "Nicht gespeichert"), GColorRed,
                    L("No room left on the watch. Sync with your phone, then finish again.",
                      "Kein Platz mehr auf der Uhr. Mit dem Handy synchronisieren, dann erneut beenden."));
    return;
  }
  if (sets == 0) {
    summary_message(L("Nothing saved", "Nichts gespeichert"), C_MUTED,
                    L("No sets were done, so nothing was kept.", "Keine Sätze gemacht, nichts gespeichert."));
  } else {
    summary_push(true);
  }
  if (s_menu_win) window_stack_remove(s_menu_win, false);
  if (s_win) window_stack_remove(s_win, false);
  home_refresh();
  comm_sync();
}
