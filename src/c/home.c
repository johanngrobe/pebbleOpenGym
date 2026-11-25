#include "home.h"
#include "i18n.h"
#include "comm.h"
#include "data.h"
#include "ui.h"
#include "workout.h"

typedef enum { ROW_RESUME, ROW_TODAY, ROW_ROUTINE, ROW_SYNC, ROW_INFO } RowType;
typedef struct { RowType type; uint8_t idx; } Row;

#define SECTIONS 3
#define MAX_ROWS (MAX_ROUTINES + 2)

static Window *s_win;
static MenuLayer *s_menu;
static StatusBarLayer *s_status_bar;
static Row s_rows[SECTIONS][MAX_ROWS];
static uint8_t s_nrows[SECTIONS];
static const char *s_titles[SECTIONS];
static uint8_t s_nsec;
static char s_buf[64];

// ---------------------------------------------------------------------------------------------
// Routine preview: what a routine holds, and Start

static Window *s_prev_win;
static MenuLayer *s_prev_menu;
static StatusBarLayer *s_prev_bar;
static uint8_t s_prev_routine;

static void entry_work_summary(char *buf, size_t size, const PlanEntry *e);

// "3 x 8 @ 60 kg", with the warm-ups in front: "2 Aufw. + 3 x 8 @ 60 kg".
static void entry_summary(char *buf, size_t size, const PlanEntry *e) {
  if (!e->warm) { entry_work_summary(buf, size, e); return; }
  char work[48];
  entry_work_summary(work, sizeof(work), e);
  snprintf(buf, size, L("%d warm-up + %s", "%d Aufw. + %s"), e->warm, work);
}

static void entry_work_summary(char *buf, size_t size, const PlanEntry *e) {
  char w[12];
  fmt_weight(w, sizeof(w), e->w20);
  bool bw = g_plan.ex[e->ex].flags & EXF_BODYWEIGHT;
  bool side = e->flags & PEF_SIDE;
  if (side && e->mode == MODE_REPS) {
    // openGym's reps are both sides together: "3 x 8 per side" for 16.
    char a[8];
    if (e->amount % 2) snprintf(a, sizeof(a), "%d.5", e->amount / 2);
    else snprintf(a, sizeof(a), "%d", e->amount / 2);
    if (e->w20) snprintf(buf, size, L("%d x %s/side @ %s %s", "%d x %s/Seite @ %s %s"), e->sets, a, w, unit_label());
    else snprintf(buf, size, L("%d x %s per side", "%d x %s pro Seite"), e->sets, a);
    return;
  }
  switch (e->mode) {
    case MODE_TIME:
      if (side) snprintf(buf, size, L("%d x %d s per side", "%d x %d s pro Seite"), e->sets, e->amount);
      else snprintf(buf, size, "%d x %d s", e->sets, e->amount);
      break;
    case MODE_CARDIO:
      snprintf(buf, size, "%d min", e->amount);
      break;
    case MODE_DIST:
      snprintf(buf, size, "%d x %d m", e->sets, e->amount);
      break;
    default:
      if (e->w20) snprintf(buf, size, "%d x %d @ %s %s", e->sets, e->amount, w, unit_label());
      else snprintf(buf, size, "%d x %d%s", e->sets, e->amount, bw ? L(" (BW)", " (Eigengew.)") : "");
  }
}

// The preview's rows: Start, then the exercises, with a label row above each superset.
typedef enum { PR_START, PR_GROUP, PR_ENTRY } PrevRowType;
#define PREV_MAX_ROWS (1 + MAX_ENTRIES + MAX_ENTRIES / 2)
static struct { uint8_t type, idx; } s_prev_rows[PREV_MAX_ROWS];
static uint8_t s_prev_n;

#define PREV_ROW_H   56
#define PREV_LABEL_H 26
#define PREV_INDENT  14

static void prev_add(uint8_t type, uint8_t idx) {
  if (s_prev_n < PREV_MAX_ROWS) {
    s_prev_rows[s_prev_n].type = type;
    s_prev_rows[s_prev_n].idx = idx;
    s_prev_n++;
  }
}

static void prev_build(void) {
  Routine *rt = &g_plan.r[s_prev_routine];
  s_prev_n = 0;
  prev_add(PR_START, 0);
  uint8_t last = 0;
  for (int k = 0; k < rt->n; k++) {
    uint8_t sg = rt->e[k].sg;
    if (sg && sg != last) prev_add(PR_GROUP, sg - 1);
    last = sg;
    prev_add(PR_ENTRY, k);
  }
}

static uint16_t prev_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return s_prev_n;
}

static int16_t prev_row_h(MenuLayer *m, MenuIndex *idx, void *ctx) {
  return s_prev_rows[idx->row].type == PR_GROUP ? PREV_LABEL_H : PREV_ROW_H;
}

static void draw_two_lines(GContext *ctx, const Layer *cell, const char *title, const char *sub, int x) {
  GRect b = layer_get_bounds(cell);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), GRect(x, 0, b.size.w - x - 6, 28),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_draw_text(ctx, sub, fonts_get_system_font(FONT_KEY_GOTHIC_24), GRect(x, 25, b.size.w - x - 6, 28),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// The superset bracket down the left edge: from the label row's middle to the last member.
static void draw_bracket(GContext *ctx, const Layer *cell, bool top, bool bottom) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, menu_cell_layer_is_highlighted(cell) ? C_ON_ACCENT : C_ACCENT);
  int y0 = top ? b.size.h / 2 : 0;
  int y1 = bottom ? b.size.h - 8 : b.size.h;
  graphics_fill_rect(ctx, GRect(4, y0, 3, y1 - y0), 0, GCornerNone);
  if (top) graphics_fill_rect(ctx, GRect(4, y0, 7, 3), 0, GCornerNone);
  if (bottom) graphics_fill_rect(ctx, GRect(4, y1 - 3, 7, 3), 0, GCornerNone);
}

static void prev_draw(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  Routine *rt = &g_plan.r[s_prev_routine];
  uint8_t i = s_prev_rows[idx->row].idx;
  switch (s_prev_rows[idx->row].type) {
    case PR_START:
      snprintf(s_buf, sizeof(s_buf), L("%d exercises", "%d Übungen"), rt->n);
      draw_two_lines(ctx, cell, L("Start workout", "Training starten"), s_buf, 6);
      break;
    case PR_GROUP: {
      const Group *g = &rt->g[i];
      const char *kind = g->flags & GRPF_CIRCUIT ? L("Circuit", "Zirkel") : L("Superset", "Supersatz");
      char rest[16] = "";
      if (g->rest) snprintf(rest, sizeof(rest), L(" - %d s rest", " - %d s Pause"), g->rest);
      if (g->name[0]) snprintf(s_buf, sizeof(s_buf), "%s: %s%s", kind, g->name, rest);
      else snprintf(s_buf, sizeof(s_buf), "%s%s", kind, rest);
      GRect b = layer_get_bounds(cell);
      if (!menu_cell_layer_is_highlighted(cell)) graphics_context_set_text_color(ctx, C_ACCENT);
      graphics_draw_text(ctx, s_buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                         GRect(PREV_INDENT, 1, b.size.w - PREV_INDENT - 6, 22),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      draw_bracket(ctx, cell, true, false);
      break;
    }
    case PR_ENTRY: {
      const PlanEntry *e = &rt->e[i];
      entry_summary(s_buf, sizeof(s_buf), e);
      if (e->sg) {
        bool last = i + 1 >= rt->n || rt->e[i + 1].sg != e->sg;
        draw_two_lines(ctx, cell, g_plan.ex[e->ex].name, s_buf, PREV_INDENT);
        draw_bracket(ctx, cell, false, last);
      } else {
        draw_two_lines(ctx, cell, g_plan.ex[e->ex].name, s_buf, 6);
      }
      break;
    }
  }
}

static int16_t prev_header_h(MenuLayer *m, uint16_t s, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void prev_header(GContext *ctx, const Layer *cell, uint16_t s, void *data) {
  ui_draw_header(ctx, cell, g_plan.r[s_prev_routine].name);
}

static void prev_select(MenuLayer *m, MenuIndex *idx, void *ctx) {
  if (s_prev_rows[idx->row].type != PR_START) return;
  sess_start(s_prev_routine);
  vibes_short_pulse();
  workout_open();
  window_stack_remove(s_prev_win, false);
}

static void prev_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect frame;
  s_prev_bar = ui_status_bar(root, &frame);
  s_prev_menu = menu_layer_create(frame);
  menu_layer_set_callbacks(s_prev_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = prev_rows,
    .get_cell_height = prev_row_h,
    .draw_row = prev_draw,
    .get_header_height = prev_header_h,
    .draw_header = prev_header,
    .select_click = prev_select,
  });
  ui_style_menu(s_prev_menu);
  menu_layer_set_click_config_onto_window(s_prev_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_prev_menu));
}

static void prev_unload(Window *w) {
  menu_layer_destroy(s_prev_menu);
  status_bar_layer_destroy(s_prev_bar);
  window_destroy(s_prev_win);
  s_prev_win = NULL;
}

static void preview_open(uint8_t routine) {
  if (routine >= g_plan.n_routines) return;
  s_prev_routine = routine;
  prev_build();
  s_prev_win = window_create();
  window_set_background_color(s_prev_win, C_BG);
  window_set_window_handlers(s_prev_win, (WindowHandlers) { .load = prev_load, .unload = prev_unload });
  window_stack_push(s_prev_win, true);
}

// ---------------------------------------------------------------------------------------------
// Home

static void add_row(int sec, RowType type, uint8_t idx) {
  if (s_nrows[sec] < MAX_ROWS) s_rows[sec][s_nrows[sec]++] = (Row) { type, idx };
}

static void rebuild(void) {
  memset(s_nrows, 0, sizeof(s_nrows));
  s_nsec = 0;

  int sec = s_nsec++;
  int today = plan_today();
  if (g_sess.active) {
    s_titles[sec] = L("Workout", "Training");
    add_row(sec, ROW_RESUME, 0);
  } else if (!g_plan.loaded || g_plan.n_routines == 0) {
    s_titles[sec] = "openGym";
    add_row(sec, ROW_INFO, 0);
  } else {
    s_titles[sec] = L("Today", "Heute");
    if (today == ROUTINE_REST) add_row(sec, ROW_INFO, 1);
    else if (today == ROUTINE_NONE) add_row(sec, ROW_INFO, 2);
    else add_row(sec, ROW_TODAY, today);
  }

  if (g_plan.loaded && g_plan.n_routines) {
    sec = s_nsec++;
    s_titles[sec] = L("Routines", "Routinen");
    for (int i = 0; i < g_plan.n_routines; i++) add_row(sec, ROW_ROUTINE, i);
  }

  sec = s_nsec++;
  s_titles[sec] = L("Phone", "Handy");
  add_row(sec, ROW_SYNC, 0);
}

static uint16_t num_sections(MenuLayer *m, void *ctx) {
  return s_nsec;
}

static uint16_t num_rows(MenuLayer *m, uint16_t section, void *ctx) {
  return section < s_nsec ? s_nrows[section] : 0;
}

static int16_t header_h(MenuLayer *m, uint16_t s, void *ctx) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t s, void *data) {
  ui_draw_header(ctx, cell, s_titles[s]);
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  Row *r = &s_rows[idx->section][idx->row];
  switch (r->type) {
    case ROW_RESUME:
      snprintf(s_buf, sizeof(s_buf), L("%s - %d/%d sets", "%s - %d/%d Sätze"), g_plan.r[g_sess.routine].name,
               sess_count(SET_DONE), sess_total());
      menu_cell_basic_draw(ctx, cell, L("Resume workout", "Training fortsetzen"), s_buf, NULL);
      break;
    case ROW_TODAY:
      snprintf(s_buf, sizeof(s_buf), L("Planned - %d exercises", "Geplant - %d Übungen"), g_plan.r[r->idx].n);
      menu_cell_basic_draw(ctx, cell, g_plan.r[r->idx].name, s_buf, NULL);
      break;
    case ROW_ROUTINE:
      snprintf(s_buf, sizeof(s_buf), L("%d exercises", "%d Übungen"), g_plan.r[r->idx].n);
      menu_cell_basic_draw(ctx, cell, g_plan.r[r->idx].name, s_buf, NULL);
      break;
    case ROW_SYNC: {
      int pending = log_count();
      const char *status = comm_status();
      if (pending) snprintf(s_buf, sizeof(s_buf), L("%d to upload - %s", "%d zum Hochladen - %s"), pending, status);
      else snprintf(s_buf, sizeof(s_buf), "%s", status);
      menu_cell_basic_draw(ctx, cell, comm_busy() ? L("Syncing...", "Sync läuft...") : L("Sync now", "Synchronisieren"), s_buf, NULL);
      break;
    }
    case ROW_INFO:
      if (r->idx == 1) menu_cell_basic_draw(ctx, cell, L("Rest day", "Ruhetag"), L("Pick a routine below", "Routine unten wählen"), NULL);
      else if (r->idx == 2) menu_cell_basic_draw(ctx, cell, L("Nothing planned", "Nichts geplant"), L("Pick a routine below", "Routine unten wählen"), NULL);
      else menu_cell_basic_draw(ctx, cell, L("No routines yet", "Noch keine Routinen"), L("Set up in the Pebble app", "In der Pebble-App einrichten"), NULL);
      break;
  }
}

static void select_row(MenuLayer *m, MenuIndex *idx, void *ctx) {
  Row *r = &s_rows[idx->section][idx->row];
  switch (r->type) {
    case ROW_RESUME:
      workout_open();
      break;
    case ROW_TODAY:
    case ROW_ROUTINE:
      if (g_sess.active) {
        // One workout at a time: the running one comes first.
        vibes_double_pulse();
        menu_layer_set_selected_index(s_menu, MenuIndex(0, 0), MenuRowAlignCenter, true);
      } else {
        preview_open(r->idx);
      }
      break;
    case ROW_SYNC:
      comm_sync();
      break;
    case ROW_INFO:
      break;
  }
}

void home_refresh(void) {
  if (!s_menu) return;
  rebuild();
  menu_layer_reload_data(s_menu);
}

static void win_appear(Window *w) {
  home_refresh();
}

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect menu_frame;
  s_status_bar = ui_status_bar(root, &menu_frame);
  s_menu = menu_layer_create(menu_frame);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = num_sections,
    .get_num_rows = num_rows,
    .get_header_height = header_h,
    .draw_header = draw_header,
    .draw_row = draw_row,
    .select_click = select_row,
  });
  ui_style_menu(s_menu);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
  rebuild();
}

static void win_unload(Window *w) {
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  status_bar_layer_destroy(s_status_bar);
}

void home_push(void) {
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_window_handlers(s_win, (WindowHandlers) {
    .load = win_load,
    .appear = win_appear,
    .unload = win_unload,
  });
  window_stack_push(s_win, true);
}
