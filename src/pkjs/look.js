// The watch's colours, like openGym's own: a dark or light theme and an accent colour.
//
// openGym keeps both in the profile (`theme`: 'dark' | 'light' | 'system', `accent`: a preset key
// or 'custom' with `accentCustom` '#rrggbb'). The settings page can follow that or pick its own.
// The watch has 64 colours (2 bits per channel), so the accent is turned into the nearest one
// that still stands out from the theme's background, and the text on it is black or white,
// whichever reads better.

// openGym's presets (frontend/src/lib/format.js ACCENTS), each as a watch colour per theme.
// Picked by hand: the nearest match alone puts Pink and Red on one colour in the dark theme and
// Teal on Blue, Yellow on Orange in the light one. Light shades follow openGym's darker
// --acc-ink and stand out from white about 3:1 (KellyGreen and TiffanyBlue a hair under).
var PRESETS = {
  brand:  { dark: '#aaff55', light: '#55aa00' },  // Inchworm, KellyGreen
  lime:   { dark: '#00aa55', light: '#00aa55' },  // JaegerGreen
  sky:    { dark: '#0055ff', light: '#0055ff' },  // BlueMoon
  orange: { dark: '#ffaa00', light: '#ff5500' },  // ChromeYellow, Orange
  violet: { dark: '#aa55ff', light: '#aa55ff' },  // LavenderIndigo
  pink:   { dark: '#ff55aa', light: '#ff0055' },  // BrilliantRose, Folly
  red:    { dark: '#ff0000', light: '#aa0000' },  // Red, DarkCandyAppleRed
  teal:   { dark: '#00aaaa', light: '#00aaaa' },  // TiffanyBlue
  gold:   { dark: '#ffff00', light: '#aa5500' }   // Yellow, WindsorTan
};
var ORDER = ['brand', 'lime', 'sky', 'orange', 'violet', 'pink', 'red', 'teal', 'gold'];
// openGym's names for them (format.js ACCENT_NAMES).
var NAMES = {
  brand: 'openGym green', lime: 'Classic green', sky: 'Blue', orange: 'Orange', violet: 'Purple',
  pink: 'Pink', red: 'Red', teal: 'Teal', gold: 'Yellow'
};

// What a watch that never heard from openGym shows: the app's original look.
var FALLBACK = { theme: 'dark', accent: 'brand' };
// What openGym draws for a profile with no usable accent (accent.js DEFAULT_ACCENT).
var PROFILE_DEFAULT_ACCENT = 'lime';

var THEME = { dark: 0, light: 1 };
var HEX = /^#[0-9a-f]{6}$/i;
var MIN_CONTRAST = 3;  // WCAG for large text and controls, as openGym uses for its accent

function rgb(hex) {
  var n = parseInt(hex.slice(1), 16);
  return [n >> 16, (n >> 8) & 255, n & 255];
}

function lin(v) {
  v /= 255;
  return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
}

function luminance(c) {
  return 0.2126 * lin(c[0]) + 0.7152 * lin(c[1]) + 0.0722 * lin(c[2]);
}

function contrast(a, b) {
  var x = luminance(a), y = luminance(b);
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

// "Redmean" distance: close to how different two colours look, and cheap.
function distance(a, b) {
  var r = (a[0] + b[0]) / 2;
  var dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
  return (2 + r / 256) * dr * dr + 4 * dg * dg + (2 + (255 - r) / 256) * db * db;
}

// A Pebble GColor8 byte: alpha in the top two bits, then 2 bits each of red, green and blue.
function toByte(c) {
  return 0xC0 | (Math.round(c[0] / 85) << 4) | (Math.round(c[1] / 85) << 2) | Math.round(c[2] / 85);
}

var PALETTE = [];
for (var r = 0; r < 4; r++) {
  for (var g = 0; g < 4; g++) {
    for (var b = 0; b < 4; b++) PALETTE.push([r * 85, g * 85, b * 85]);
  }
}

// The watch colour nearest to `hex` that stands out from `bg` by MIN_CONTRAST. Greys are left out:
// a grey accent would look like the switched-off text around it (openGym makes the same call).
function nearest(hex, bg) {
  var want = rgb(hex);
  var best = null, bestD = Infinity;
  PALETTE.forEach(function (c) {
    var grey = c[0] === c[1] && c[1] === c[2];
    if (grey || contrast(c, bg) < MIN_CONTRAST) return;
    var d = distance(want, c);
    if (d < bestD) { bestD = d; best = c; }
  });
  return best;
}

// The profile's look as the watch draws it. state: openGym's document state (may be missing).
function fromProfile(state) {
  if (!state || typeof state !== 'object') return null;
  var theme = state.theme === 'light' ? 'light' : 'dark';  // 'system': the watch has no system theme
  var accent = state.accent;
  if (accent === 'custom' && typeof state.accentCustom === 'string' && HEX.test(state.accentCustom)) {
    return { theme: theme, accent: state.accentCustom.toLowerCase() };
  }
  return { theme: theme, accent: PRESETS[accent] ? accent : PROFILE_DEFAULT_ACCENT };
}

// cfg: the settings page's { theme, accent } ('' = like the profile); profile: fromProfile's.
function resolve(cfg, profile) {
  var base = profile || FALLBACK;
  var theme = THEME[cfg.theme] != null ? cfg.theme : base.theme;
  var accent = PRESETS[cfg.accent] ? cfg.accent : base.accent;
  return { theme: theme, accent: accent };
}

// { Theme, Accent, OnAccent } for the watch.
function encode(look) {
  var light = look.theme === 'light';
  var bg = light ? [255, 255, 255] : [0, 0, 0];
  var preset = PRESETS[look.accent];
  var custom = !preset && HEX.test(look.accent);
  var acc = preset ? rgb(preset[light ? 'light' : 'dark'])
    : custom ? nearest(look.accent, bg) : rgb(PRESETS[FALLBACK.accent].dark);
  var ink = contrast(acc, [0, 0, 0]) >= contrast(acc, [255, 255, 255]) ? [0, 0, 0] : [255, 255, 255];
  return { Theme: THEME[look.theme], Accent: toByte(acc), OnAccent: toByte(ink) };
}

module.exports = {
  PRESETS: PRESETS, ORDER: ORDER, NAMES: NAMES, THEME: THEME,
  fromProfile: fromProfile, resolve: resolve, encode: encode, nearest: nearest
};
