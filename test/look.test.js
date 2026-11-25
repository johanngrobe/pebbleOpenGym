// node test/look.test.js — checks the watch colours picked from openGym's theme and accent.
var assert = require('assert');
var look = require('../src/pkjs/look');

var BLACK = 0xC0, WHITE = 0xFF;
var INCHWORM = 0xED;  // the app's original accent

// The settings page wins over the profile; '' follows it; nothing known is the original look.
assert.deepStrictEqual(look.resolve({}, null), { theme: 'dark', accent: 'brand' });
assert.deepStrictEqual(look.encode(look.resolve({}, null)), { Theme: 0, Accent: INCHWORM, OnAccent: BLACK });
var profile = look.fromProfile({ theme: 'light', accent: 'sky' });
assert.deepStrictEqual(look.resolve({ theme: '', accent: '' }, profile), { theme: 'light', accent: 'sky' });
assert.deepStrictEqual(look.resolve({ theme: 'dark', accent: 'red' }, profile), { theme: 'dark', accent: 'red' });

// openGym's 'system' is dark here; an unknown or missing accent is openGym's default.
assert.deepStrictEqual(look.fromProfile({ theme: 'system', accent: 'nope' }), { theme: 'dark', accent: 'lime' });
assert.deepStrictEqual(look.fromProfile({}), { theme: 'dark', accent: 'lime' });
assert.strictEqual(look.fromProfile(null), null);

// Own colour: kept from the profile, junk falls back.
assert.deepStrictEqual(look.fromProfile({ accent: 'custom', accentCustom: '#3366CC' }), { theme: 'dark', accent: '#3366cc' });
assert.deepStrictEqual(look.fromProfile({ accent: 'custom', accentCustom: 'red' }), { theme: 'dark', accent: 'lime' });

function rgbOf(byte) {
  return [(byte >> 4) & 3, (byte >> 2) & 3, byte & 3].map(function (v) { return v * 85; });
}
function lum(c) {
  var l = c.map(function (v) { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); });
  return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
}
function contrast(a, b) {
  var x = lum(a), y = lum(b);
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

['dark', 'light'].forEach(function (theme) {
  var bg = theme === 'light' ? [255, 255, 255] : [0, 0, 0];
  var seen = {};
  look.ORDER.forEach(function (k) {
    var e = look.encode({ theme: theme, accent: k });
    assert.strictEqual(e.Theme, look.THEME[theme]);
    assert.strictEqual(e.Accent & 0xC0, 0xC0, 'opaque');
    // Every preset its own colour, standing out from the page (the hand-picked light shades
    // KellyGreen and TiffanyBlue sit a hair under 3:1).
    assert(!seen[e.Accent], theme + ' ' + k + ' shares a colour with ' + seen[e.Accent]);
    seen[e.Accent] = k;
    assert(contrast(rgbOf(e.Accent), bg) >= 2.8, theme + ' ' + k + ' too faint');
    // Text on the accent reads.
    assert([BLACK, WHITE].indexOf(e.OnAccent) >= 0);
    assert(contrast(rgbOf(e.Accent), rgbOf(e.OnAccent)) >= 3, theme + ' ' + k + ' ink');
  });

  // An own colour that would vanish into the page is moved until it reads.
  var faint = look.encode({ theme: theme, accent: theme === 'dark' ? '#000033' : '#ffffcc' });
  assert(contrast(rgbOf(faint.Accent), bg) >= 3, theme + ' own colour too faint');
  var c = rgbOf(faint.Accent);
  assert(!(c[0] === c[1] && c[1] === c[2]), 'never a grey');
});

console.log('look tests passed');
