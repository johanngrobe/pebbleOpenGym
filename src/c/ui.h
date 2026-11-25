#pragma once
#include <pebble.h>

// openGym's look: true black, one lime accent for the thing to press.
#define C_BG      GColorBlack
#define C_FG      GColorWhite
#define C_MUTED   GColorLightGray
#define C_DIM     GColorDarkGray
#define C_ACCENT  GColorInchworm
#define C_ON_ACCENT GColorBlack

static inline void ui_style_menu(MenuLayer *menu) {
  menu_layer_set_normal_colors(menu, C_BG, C_FG);
  menu_layer_set_highlight_colors(menu, C_ACCENT, C_ON_ACCENT);
}

// The time along the top of every list screen. Puts the frame left below it in *content.
static inline StatusBarLayer *ui_status_bar(Layer *root, GRect *content) {
  StatusBarLayer *sb = status_bar_layer_create();
  status_bar_layer_set_colors(sb, C_BG, C_FG);
  status_bar_layer_set_separator_mode(sb, StatusBarLayerSeparatorModeNone);
  layer_add_child(root, status_bar_layer_get_layer(sb));
  GRect b = layer_get_bounds(root);
  *content = GRect(0, STATUS_BAR_LAYER_HEIGHT, b.size.w, b.size.h - STATUS_BAR_LAYER_HEIGHT);
  return sb;
}

// Section headers: quiet lime text on black instead of the default light bar.
static inline void ui_draw_header(GContext *ctx, const Layer *cell, const char *title) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_ACCENT);
  graphics_draw_text(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(8, -2, b.size.w - 16, b.size.h + 2), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

// Button hints drawn along the right edge, next to the buttons.
typedef enum { HINT_NONE, HINT_PLUS, HINT_MINUS, HINT_CHECK, HINT_STOP, HINT_SKIP } Hint;

static inline void ui_draw_hint(GContext *ctx, GPoint c, Hint h, GColor color) {
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, 3);
  switch (h) {
    case HINT_PLUS:
      graphics_draw_line(ctx, GPoint(c.x - 5, c.y), GPoint(c.x + 5, c.y));
      graphics_draw_line(ctx, GPoint(c.x, c.y - 5), GPoint(c.x, c.y + 5));
      break;
    case HINT_MINUS:
      graphics_draw_line(ctx, GPoint(c.x - 5, c.y), GPoint(c.x + 5, c.y));
      break;
    case HINT_CHECK:
      graphics_draw_line(ctx, GPoint(c.x - 6, c.y), GPoint(c.x - 2, c.y + 4));
      graphics_draw_line(ctx, GPoint(c.x - 2, c.y + 4), GPoint(c.x + 6, c.y - 5));
      break;
    case HINT_STOP:
      graphics_fill_rect(ctx, GRect(c.x - 5, c.y - 5, 10, 10), 1, GCornersAll);
      break;
    case HINT_SKIP:
      graphics_draw_line(ctx, GPoint(c.x - 6, c.y - 5), GPoint(c.x - 1, c.y));
      graphics_draw_line(ctx, GPoint(c.x - 1, c.y), GPoint(c.x - 6, c.y + 5));
      graphics_draw_line(ctx, GPoint(c.x + 1, c.y - 5), GPoint(c.x + 6, c.y));
      graphics_draw_line(ctx, GPoint(c.x + 6, c.y), GPoint(c.x + 1, c.y + 5));
      break;
    default:
      break;
  }
  graphics_context_set_stroke_width(ctx, 1);
}

static inline void ui_draw_hints(GContext *ctx, GRect b, Hint up, Hint select, Hint down) {
  int x = b.size.w - 9;
  ui_draw_hint(ctx, GPoint(x, b.size.h * 22 / 100), up, C_MUTED);
  ui_draw_hint(ctx, GPoint(x, b.size.h / 2), select, C_ACCENT);
  ui_draw_hint(ctx, GPoint(x, b.size.h * 78 / 100), down, C_MUTED);
}
