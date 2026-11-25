#include "focus.h"
#include "data.h"
#include "i18n.h"
#include "ui.h"

#define AUTO_CLOSE_MS 20000  // the workout's clock runs on; don't keep it covered for long

static Window *s_win;
static Layer *s_canvas;
static AppTimer *s_timer;

static void focus_close(void) {
  if (s_win) window_stack_remove(s_win, true);
}

static void timer_fired(void *ctx) {
  s_timer = NULL;
  focus_close();
}

static void any_click(ClickRecognizerRef r, void *ctx) {
  focus_close();
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, any_click);
  window_single_click_subscribe(BUTTON_ID_BACK, any_click);
  window_single_click_subscribe(BUTTON_ID_UP, any_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, any_click);
}

// A crescent moon, like the watch's own Do Not Disturb icon.
static void draw_moon(GContext *ctx, GPoint c, int r) {
  graphics_context_set_fill_color(ctx, C_ACCENT);
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_circle(ctx, GPoint(c.x + r * 2 / 3, c.y - r / 2), r);
}

static int16_t draw_wrapped(GContext *ctx, const char *text, GFont font, GRect r, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  return graphics_text_layout_get_content_size(text, font, r, GTextOverflowModeTrailingEllipsis,
                                               GTextAlignmentLeft).h;
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  int x = 10, w = b.size.w - x - 24;  // room for the hint on the right
  int y = g_ui.level ? 6 : 16;
  int moon = g_ui.title_line / 3;
  draw_moon(ctx, GPoint(x + moon, y + g_ui.title_line / 2 + 2), moon);
  graphics_context_set_text_color(ctx, C_FG);
  graphics_draw_text(ctx, L("Focus", "Fokus"), g_ui.title, GRect(x + moon * 2 + 8, y - 4, w, g_ui.title_line + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  y += g_ui.title_line + 6;

  y += draw_wrapped(ctx, L("Turn on Do Not Disturb to silence notifications while you train.",
                           "Schalte \"Nicht stören\" ein, damit dich beim Training keine Mitteilungen stören."),
                    g_ui.small, GRect(x, y, w, b.size.h - y), C_FG) + 8;
  draw_wrapped(ctx, L("Settings > Do Not Disturb, or a Quick Launch button.",
                      "Einstellungen > Nicht stören, oder per Schnellstart."),
               g_ui.small, GRect(x, y, w, b.size.h - y), C_MUTED);

  ui_draw_hints(ctx, b, HINT_NONE, HINT_CHECK, HINT_NONE);
}

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
}

static void win_unload(Window *w) {
  if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
  layer_destroy(s_canvas);
  s_canvas = NULL;
  window_destroy(s_win);
  s_win = NULL;
}

void focus_remind_open(void) {
  if (!g_focus_remind || s_win) return;
#ifdef _PBL_API_EXISTS_quiet_time_is_active
  if (quiet_time_is_active()) return;  // already quiet: nothing to remind of
#endif
  s_win = window_create();
  window_set_background_color(s_win, C_BG);
  window_set_click_config_provider(s_win, click_config);
  window_set_window_handlers(s_win, (WindowHandlers) { .load = win_load, .unload = win_unload });
  window_stack_push(s_win, true);
  s_timer = app_timer_register(AUTO_CLOSE_MS, timer_fired, NULL);
}
