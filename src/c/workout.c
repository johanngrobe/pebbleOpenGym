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
static time_t s_hold_start;
static int s_hr;

static GFont s_f_name, s_f_small, s_f_small_bold, s_f_value, s_f_value_md, s_f_value_sm, s_f_big, s_f_title;

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
  if (s_state == ST_HOLD && sess_cur_set() >= 0) return;
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

static void update_hr(void) {
#if defined(PBL_HEALTH)
  HealthValue v = health_service_peek_current_value(HealthMetricHeartRateBPM);
  s_hr = v > 0 ? (int)v : 0;
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
  bool numeric = value[0] >= '0' && value[0] <= '9';
  GFont f = numeric ? s_f_value : s_f_title;
  int vy = numeric ? r.origin.y + 8 : r.origin.y + 14;
  if (numeric) {
    // "62.5" or "102.5" does not fit the big digits in a half-width box: step down.
    GFont steps[] = { s_f_value, s_f_value_md, s_f_value_sm };
    int offs[] = { 8, 12, 16 };
    for (int i = 0; i < 3; i++) {
      GSize sz = graphics_text_layout_get_content_size(value, steps[i], GRect(0, 0, 400, 60),
                                                       GTextOverflowModeWordWrap, GTextAlignmentLeft);
      f = steps[i];
      vy = r.origin.y + offs[i];
      if (sz.w <= r.size.w - 8) break;
    }
  }
  draw_text(ctx, value, f, GRect(r.origin.x, vy, r.size.w, 44), GTextAlignmentCenter, fg);
  draw_text(ctx, label, s_f_small, GRect(r.origin.x, r.origin.y + r.size.h - 26, r.size.w, 22),
            GTextAlignmentCenter, focused ? C_ON_ACCENT : C_MUTED);
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

static void draw_top(GContext *ctx, GRect b) {
  char buf[24];
  fmt_duration(buf, sizeof(buf), time(NULL) - g_sess.start);
  draw_text(ctx, buf, s_f_small_bold, GRect(8, 0, 90, 22), GTextAlignmentLeft, C_MUTED);
  // The time of day; in 12-hour style without AM/PM, which would run into the heart rate.
  time_t now = time(NULL);
  strftime(buf, sizeof(buf), clock_is_24h_style() ? "%H:%M" : "%I:%M", localtime(&now));
  const char *clock = (!clock_is_24h_style() && buf[0] == '0') ? buf + 1 : buf;
  draw_text(ctx, clock, s_f_small_bold, GRect(0, 0, b.size.w, 22), GTextAlignmentCenter, C_FG);
  if (s_hr > 0) {
    snprintf(buf, sizeof(buf), "%d bpm", s_hr);
    draw_text(ctx, buf, s_f_small_bold, GRect(b.size.w - 100, 0, 76, 22), GTextAlignmentRight, C_MUTED);
  }
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

  draw_text(ctx, g_plan.ex[e->ex].name, s_f_name, GRect(8, 20, content_w - 4, 54), GTextAlignmentLeft, C_FG);
  set_label(buf, sizeof(buf), e, si, false);
  draw_text(ctx, buf, s_f_small, GRect(8, 74, content_w, 22), GTextAlignmentLeft, C_MUTED);

  SetRec *s = &e->s[si];
  char wv[12], av[8];
  bool bw = g_plan.ex[e->ex].flags & EXF_BODYWEIGHT;
  if (s->w20 == 0 && bw) strcpy(wv, L("BW", "Eigen"));
  else fmt_weight(wv, sizeof(wv), s->w20);
  fmt_amount(av, sizeof(av), e, s->amt);

  int top = 102, h = 86;
  if (has_weight(e)) {
    int bw_ = (content_w - 8 - 6) / 2;
    draw_value_box(ctx, GRect(8, top, bw_, h), wv, unit_label(), s_focus == FOCUS_WEIGHT);
    draw_value_box(ctx, GRect(8 + bw_ + 6, top, bw_, h), av, amount_label(e), s_focus == FOCUS_AMOUNT);
    snprintf(buf, sizeof(buf), L("Hold select: edit %s", "Mitte halten: %s ändern"), s_focus == FOCUS_WEIGHT ? amount_label(e) : unit_label());
    draw_text(ctx, buf, s_f_small, GRect(0, b.size.h - 34, content_w + 8, 22), GTextAlignmentCenter, C_MUTED);
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
  int elapsed = time(NULL) - s_hold_start;
  draw_text(ctx, g_plan.ex[e->ex].name, s_f_name, GRect(8, 20, content_w, 54), GTextAlignmentLeft, C_FG);
  set_label(buf, sizeof(buf), e, si, true);
  draw_text(ctx, buf, s_f_small, GRect(8, 74, content_w, 22), GTextAlignmentLeft, C_ACCENT);
  fmt_duration(buf, sizeof(buf), target > 0 ? target - elapsed : elapsed);
  draw_text(ctx, buf, s_f_big, GRect(0, 104, content_w + 8, 56), GTextAlignmentCenter, C_FG);
  draw_progress(ctx, GRect(16, 172, content_w - 16, 8), elapsed, target);
  draw_text(ctx, L("Select: stop", "Mitte: Stopp"), s_f_small, GRect(0, b.size.h - 34, content_w + 8, 22), GTextAlignmentCenter, C_MUTED);
  ui_draw_hints(ctx, b, HINT_NONE, HINT_STOP, HINT_NONE);
}

static void draw_rest(GContext *ctx, GRect b) {
  int content_w = b.size.w - 26;
  char buf[48];
  int left = (int)g_sess.rest_end - (int)time(NULL);
  draw_text(ctx, L("Rest", "Pause"), s_f_name, GRect(8, 20, content_w, 30), GTextAlignmentLeft, C_ACCENT);
  fmt_duration(buf, sizeof(buf), left);
  draw_text(ctx, buf, s_f_big, GRect(0, 50, content_w + 8, 56), GTextAlignmentCenter, C_FG);
  draw_progress(ctx, GRect(16, 116, content_w - 16, 8), left, g_sess.rest_total);

  SessEntry *e = cur_entry();
  int si = sess_cur_set();
  if (e && si >= 0) {
    draw_text(ctx, L("Next", "Als Nächstes"), s_f_small, GRect(8, 134, content_w, 20), GTextAlignmentLeft, C_MUTED);
    draw_text(ctx, g_plan.ex[e->ex].name, s_f_small_bold, GRect(8, 152, content_w, 44), GTextAlignmentLeft, C_FG);
    set_summary(buf, sizeof(buf), e, &e->s[si]);
    draw_text(ctx, buf, s_f_small, GRect(8, 196, content_w, 22), GTextAlignmentLeft, C_MUTED);
  }
  graphics_context_set_text_color(ctx, C_MUTED);
  int x = b.size.w - 34;
  draw_text(ctx, "+15", s_f_small, GRect(x, b.size.h * 22 / 100 - 12, 32, 20), GTextAlignmentRight, C_MUTED);
  draw_text(ctx, "-15", s_f_small, GRect(x, b.size.h * 78 / 100 - 12, 32, 20), GTextAlignmentRight, C_MUTED);
  ui_draw_hints(ctx, b, HINT_NONE, HINT_SKIP, HINT_NONE);
}

static void draw_done(GContext *ctx, GRect b) {
  int content_w = b.size.w - 26;
  char buf[48], dur[16];
  draw_text(ctx, L("All sets done", "Alle Sätze fertig"), s_f_title, GRect(8, 40, content_w, 64), GTextAlignmentCenter, C_FG);
  fmt_duration(dur, sizeof(dur), time(NULL) - g_sess.start);
  snprintf(buf, sizeof(buf), L("%d sets - %s", "%d Sätze - %s"), sess_count(SET_DONE), dur);
  draw_text(ctx, buf, s_f_small, GRect(8, 108, content_w, 22), GTextAlignmentCenter, C_MUTED);
  draw_text(ctx, L("Select: finish and save", "Mitte: speichern"), s_f_small_bold, GRect(8, 146, content_w, 22), GTextAlignmentCenter, C_ACCENT);
  draw_text(ctx, L("Back: menu", "Zurück: Menü"), s_f_small, GRect(8, 170, content_w, 22), GTextAlignmentCenter, C_MUTED);
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
    if (e && si >= 0 && e->s[si].amt > 0 && now - s_hold_start >= e->s[si].amt) {
      vibe_timer_done();
      light_enable_interaction();
      complete_set(SET_DONE);
    }
  }
  if (t->tm_sec % 5 == 0) update_hr();
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
        s_hold_start = time(NULL);
        s_state = ST_HOLD;
        vibes_short_pulse();
        redraw();
      } else {
        complete_set(SET_DONE);
      }
      break;
    case ST_HOLD: {
      int si = sess_cur_set();
      if (e && si >= 0) {
        int elapsed = time(NULL) - s_hold_start;
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
  if (ev->y < 100 || ev->y > 190) return;
  GRect b = layer_get_bounds(s_canvas);
  uint8_t f = ev->x < (b.size.w - 26) / 2 + 4 ? FOCUS_WEIGHT : FOCUS_AMOUNT;
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
  update_hr();
  tick_timer_service_subscribe(SECOND_UNIT, tick);
#ifdef _PBL_API_EXISTS_touch_service_subscribe
  touch_service_subscribe(touch, NULL);
#endif
  redraw();
}

static void win_disappear(Window *w) {
  tick_timer_service_unsubscribe();
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
  if (!s_f_name) {
    s_f_name = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    s_f_title = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
    s_f_small = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    s_f_small_bold = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_f_value = fonts_get_system_font(FONT_KEY_LECO_38_BOLD_NUMBERS);
    s_f_value_md = fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS);
    s_f_value_sm = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);
    s_f_big = fonts_get_system_font(FONT_KEY_ROBOTO_BOLD_SUBSET_49);
  }
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
  menu_cell_basic_draw(ctx, cell, g_plan.ex[e->ex].name, buf, NULL);
}

static void list_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (sess_open_in(idx->row) == 0) {
    if (!sess_add_set(idx->row)) { vibes_double_pulse(); return; }
  }
  g_sess.cur = idx->row;
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
    case M_RESUME: menu_cell_basic_draw(ctx, cell, L("Resume", "Fortsetzen"), NULL, NULL); break;
    case M_EXERCISES:
      snprintf(buf, sizeof(buf), L("%d/%d sets done", "%d/%d Sätze fertig"), sess_count(SET_DONE), sess_total());
      menu_cell_basic_draw(ctx, cell, L("Exercises", "Übungen"), buf, NULL);
      break;
    case M_SKIP: menu_cell_basic_draw(ctx, cell, L("Skip this set", "Satz überspringen"), NULL, NULL); break;
    case M_ADD: menu_cell_basic_draw(ctx, cell, L("Add a set", "Satz hinzufügen"), L("To this exercise", "Zu dieser Übung"), NULL); break;
    case M_NOTE: {
      const char *sub = s_note_failed ? L("Did not work: phone near?", "Ging nicht: Handy in der Nähe?")
                      : g_sess.note[0] ? g_sess.note
                      : L("Speak it, saved with the workout", "Sprechen, wird mitgespeichert");
      menu_cell_basic_draw(ctx, cell, g_sess.note[0] ? L("Add to the note", "Notiz ergänzen") : L("Dictate a note", "Notiz diktieren"), sub, NULL);
      break;
    }
    case M_FINISH: menu_cell_basic_draw(ctx, cell, L("Finish and save", "Beenden und speichern"), NULL, NULL); break;
    case M_DISCARD:
      menu_cell_basic_draw(ctx, cell, L("Discard workout", "Training verwerfen"),
                           s_discard_armed ? L("Select again to discard", "Nochmal drücken")
                                           : L("Nothing is saved", "Nichts wird gespeichert"), NULL);
      break;
    case M_LEAVE: menu_cell_basic_draw(ctx, cell, L("Leave for now", "Pausieren"), L("Resume it later", "Später fortsetzen"), NULL); break;
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
  s_sum_title = text_layer_create(GRect(10, 30, b.size.w - 20, 40));
  text_layer_set_background_color(s_sum_title, GColorClear);
  text_layer_set_text_color(s_sum_title, s_sum_title_color);
  text_layer_set_font(s_sum_title, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text(s_sum_title, s_sum_title_text);
  layer_add_child(root, text_layer_get_layer(s_sum_title));

  s_sum_body = text_layer_create(GRect(10, 76, b.size.w - 20, b.size.h - 86));
  text_layer_set_background_color(s_sum_body, GColorClear);
  text_layer_set_text_color(s_sum_body, C_FG);
  text_layer_set_font(s_sum_body, fonts_get_system_font(FONT_KEY_GOTHIC_24));
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
