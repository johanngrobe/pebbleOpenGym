#include "ui.h"

// Persistent keys of the text size and the colours (data.c holds the others, 1-6 and 100+).
#define K_TEXT 7
#define K_LOOK 8

UiFonts g_ui;
UiColors g_colors;
static uint8_t s_text = TEXT_AUTO;

// What the phone sent, as stored: theme, accent, text on the accent.
typedef struct { uint8_t theme, accent, on_accent; } Look;
// Until the phone says otherwise: the app's original black and lime.
static Look s_look = { THEME_DARK, 0xED /* Inchworm */, 0xC0 /* Black */ };

static void pick_colors(void) {
  UiColors *c = &g_colors;
  bool light = s_look.theme == THEME_LIGHT;
  c->theme = light ? THEME_LIGHT : THEME_DARK;
  c->bg = light ? GColorWhite : GColorBlack;
  c->fg = light ? GColorBlack : GColorWhite;
  c->muted = light ? GColorDarkGray : GColorLightGray;
  c->dim = light ? GColorLightGray : GColorDarkGray;
  c->accent = (GColor) { .argb = s_look.accent | 0xC0 };
  c->on_accent = (GColor) { .argb = s_look.on_accent | 0xC0 };
  // An accent the same as the page would hide everything drawn in it.
  if (gcolor_equal(c->accent, c->bg) || gcolor_equal(c->accent, c->on_accent)) {
    c->accent = GColorInchworm;
    c->on_accent = GColorBlack;
  }
}

bool ui_colors_set(uint8_t theme, uint8_t accent, uint8_t on_accent) {
  Look l = { theme == THEME_LIGHT ? THEME_LIGHT : THEME_DARK, accent, on_accent };
  if (memcmp(&l, &s_look, sizeof(l)) == 0) return false;
  s_look = l;
  persist_write_data(K_LOOK, &s_look, sizeof(s_look));
  pick_colors();
  return true;
}

static GFont font(const char *key) {
  return fonts_get_system_font(key);
}

// The watch's own text size: Emery starts at Large, so only Extra large asks for more.
static uint8_t level_of(uint8_t text) {
  switch (text) {
    case TEXT_NORMAL: return UI_NORMAL;
    case TEXT_LARGE: return UI_LARGE;
    case TEXT_XL: return UI_XL;
    default:
#ifdef _PBL_API_EXISTS_preferred_content_size
      return preferred_content_size() >= PreferredContentSizeExtraLarge ? UI_XL : UI_NORMAL;
#else
      return UI_NORMAL;
#endif
  }
}

static void pick_fonts(void) {
  UiFonts *u = &g_ui;
  memset(u, 0, sizeof(*u));
  u->level = level_of(s_text);
  u->title = font(FONT_KEY_GOTHIC_28_BOLD);
  u->title_line = 32;

  switch (u->level) {
    case UI_NORMAL:
      u->top = font(FONT_KEY_GOTHIC_18_BOLD); u->top_h = 20;
      u->name = font(FONT_KEY_GOTHIC_24_BOLD); u->name_line = 27;
      u->small = font(FONT_KEY_GOTHIC_18); u->small_bold = font(FONT_KEY_GOTHIC_18_BOLD); u->small_line = 22;
      u->hint = font(FONT_KEY_GOTHIC_18);
      u->value[0] = font(FONT_KEY_LECO_38_BOLD_NUMBERS); u->value_h[0] = 38;
      u->value[1] = font(FONT_KEY_LECO_32_BOLD_NUMBERS); u->value_h[1] = 32;
      u->value[2] = font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM); u->value_h[2] = 26;
      u->n_value = 3;
      u->cell_title = font(FONT_KEY_GOTHIC_24_BOLD); u->cell_title_line = 28;
      u->cell_sub = font(FONT_KEY_GOTHIC_18); u->cell_sub_line = 22;
      u->header = font(FONT_KEY_GOTHIC_14_BOLD); u->header_h = MENU_CELL_BASIC_HEADER_HEIGHT;
      break;

    case UI_LARGE:
      u->top = font(FONT_KEY_GOTHIC_24_BOLD); u->top_h = 26;
      u->name = font(FONT_KEY_GOTHIC_28_BOLD); u->name_line = 30;
      u->small = font(FONT_KEY_GOTHIC_24); u->small_bold = font(FONT_KEY_GOTHIC_24_BOLD); u->small_line = 27;
      u->hint = font(FONT_KEY_GOTHIC_24_BOLD);
      u->value[0] = font(FONT_KEY_LECO_42_NUMBERS); u->value_h[0] = 42;
      u->value[1] = font(FONT_KEY_LECO_38_BOLD_NUMBERS); u->value_h[1] = 38;
      u->value[2] = font(FONT_KEY_LECO_32_BOLD_NUMBERS); u->value_h[2] = 32;
      u->value[3] = font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM); u->value_h[3] = 26;
      u->n_value = 4;
      u->cell_title = font(FONT_KEY_GOTHIC_28_BOLD); u->cell_title_line = 32;
      u->cell_sub = font(FONT_KEY_GOTHIC_24); u->cell_sub_line = 28;
      u->header = font(FONT_KEY_GOTHIC_18_BOLD); u->header_h = 22;
      break;

    default:  // UI_XL
      u->top = font(FONT_KEY_GOTHIC_24_BOLD); u->top_h = 26;
      u->name = font(FONT_KEY_GOTHIC_28_BOLD); u->name_line = 30;
      u->small = font(FONT_KEY_GOTHIC_28); u->small_bold = font(FONT_KEY_GOTHIC_28_BOLD); u->small_line = 31;
      u->hint = font(FONT_KEY_GOTHIC_24_BOLD);
      // The biggest digits there are (LECO 60 has no glyphs on Emery).
      u->value[0] = font(FONT_KEY_LECO_42_NUMBERS); u->value_h[0] = 42;
      u->value[1] = font(FONT_KEY_LECO_38_BOLD_NUMBERS); u->value_h[1] = 38;
      u->value[2] = font(FONT_KEY_LECO_32_BOLD_NUMBERS); u->value_h[2] = 32;
      u->n_value = 3;
      u->cell_title = font(FONT_KEY_GOTHIC_28_BOLD); u->cell_title_line = 32;
      u->cell_sub = font(FONT_KEY_GOTHIC_28); u->cell_sub_line = 32;
      u->header = font(FONT_KEY_GOTHIC_24_BOLD); u->header_h = 28;
      break;
  }
  // The timers use the digits of the weight and reps boxes, the biggest that fit.
  memcpy(u->big, u->value, sizeof(u->big));
  memcpy(u->big_h, u->value_h, sizeof(u->big_h));
  u->n_big = u->n_value;
}

void ui_init(void) {
  if (persist_exists(K_TEXT)) s_text = persist_read_int(K_TEXT);
  if (s_text > TEXT_XL) s_text = TEXT_AUTO;
  pick_fonts();
  if (persist_get_size(K_LOOK) == sizeof(s_look)) persist_read_data(K_LOOK, &s_look, sizeof(s_look));
  pick_colors();
}

void ui_text_size_set(uint8_t v) {
  if (v > TEXT_XL) return;
  if (v != s_text) persist_write_int(K_TEXT, v);
  s_text = v;
  pick_fonts();
}

uint8_t ui_text_size(void) {
  return s_text;
}

void ui_draw_cell(GContext *ctx, const Layer *cell, const char *title, const char *sub) {
  if (g_ui.level == UI_NORMAL) {
    menu_cell_basic_draw(ctx, cell, title, sub, NULL);
    return;
  }
  GRect b = layer_get_bounds(cell);
  int w = b.size.w - 12;
  // Gothic draws below its box top; pull up so the lines sit in the middle of the row.
  int y = sub ? 0 : (b.size.h - g_ui.cell_title_line) / 2 - 6;
  graphics_draw_text(ctx, title, g_ui.cell_title, GRect(6, y, w, g_ui.cell_title_line + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  if (sub) {
    graphics_draw_text(ctx, sub, g_ui.cell_sub, GRect(6, g_ui.cell_title_line - 2, w, g_ui.cell_sub_line + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

int16_t ui_cell_h(bool has_sub) {
  if (has_sub) return g_ui.cell_title_line + g_ui.cell_sub_line + 6;
  return g_ui.cell_title_line + 16;
}
