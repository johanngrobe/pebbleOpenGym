#pragma once
#include <pebble.h>

// ---------------------------------------------------------------------------------------------
// Colours, like openGym's: a dark (true black) or light theme and one accent colour for the
// thing to press. Set on the phone, or taken from the openGym profile; the phone picks the
// watch colours (src/pkjs/look.js).

enum { THEME_DARK = 0, THEME_LIGHT = 1 };

typedef struct {
  uint8_t theme;
  GColor bg, fg;
  GColor muted;      // secondary text, button hints
  GColor dim;        // outlines, empty progress
  GColor accent, on_accent;
} UiColors;

extern UiColors g_colors;

#define C_BG        (g_colors.bg)
#define C_FG        (g_colors.fg)
#define C_MUTED     (g_colors.muted)
#define C_DIM       (g_colors.dim)
#define C_ACCENT    (g_colors.accent)
#define C_ON_ACCENT (g_colors.on_accent)

// From the phone: THEME_*, and the accent and the text on it as GColor8 bytes. Stored; true when
// anything changed (the open screens then restyle).
bool ui_colors_set(uint8_t theme, uint8_t accent, uint8_t on_accent);

// ---------------------------------------------------------------------------------------------
// Text size. Normal is the app's own look; Large and Extra large make every text bigger for
// reading at arm's length. "Like the watch" follows the watch's system text size.

enum { TEXT_AUTO = 0, TEXT_NORMAL = 1, TEXT_LARGE = 2, TEXT_XL = 3 };  // as set on the phone
enum { UI_NORMAL = 0, UI_LARGE = 1, UI_XL = 2 };                         // what is shown

#define UI_MAX_STEPS 4

typedef struct {
  uint8_t level;  // UI_NORMAL, UI_LARGE, UI_XL
  // Workout screens
  GFont top; int16_t top_h;              // duration, clock, heart rate along the top
  GFont name; int16_t name_line;         // the exercise
  GFont title; int16_t title_line;       // "All sets done", "Rest"
  GFont small, small_bold; int16_t small_line;
  GFont hint;                            // "+15", "-15" next to the buttons
  GFont big[UI_MAX_STEPS]; int16_t big_h[UI_MAX_STEPS]; uint8_t n_big;          // timers
  GFont value[UI_MAX_STEPS]; int16_t value_h[UI_MAX_STEPS]; uint8_t n_value;    // weight, reps
  bool footer;                           // room for the "Hold select: ..." line at the bottom
  // Lists
  GFont cell_title, cell_sub; int16_t cell_title_line, cell_sub_line;
  GFont header; int16_t header_h;
} UiFonts;

extern UiFonts g_ui;

void ui_init(void);                // reads the stored settings, picks the fonts and colours
void ui_text_size_set(uint8_t v);  // TEXT_*, from the phone; stored
uint8_t ui_text_size(void);

// A list row: a title and an optional second line, in the chosen size.
void ui_draw_cell(GContext *ctx, const Layer *cell, const char *title, const char *sub);
int16_t ui_cell_h(bool has_sub);

static inline void ui_style_menu(MenuLayer *menu) {
  menu_layer_set_normal_colors(menu, C_BG, C_FG);
  menu_layer_set_highlight_colors(menu, C_ACCENT, C_ON_ACCENT);
}

// A list screen in the current colours: again after they change on the phone.
static inline void ui_style_list(Window *w, StatusBarLayer *sb, MenuLayer *menu) {
  window_set_background_color(w, C_BG);
  status_bar_layer_set_colors(sb, C_BG, C_FG);
  ui_style_menu(menu);
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
  graphics_draw_text(ctx, title, g_ui.header,
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
