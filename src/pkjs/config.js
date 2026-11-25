// The settings page, opened from the Pebble phone app. Served as a data: URL so nothing has to
// be hosted anywhere: it talks to no server, it only hands the values back to the watch app.

var i18n = require('./i18n');

function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

// lang: the watch's language (or the phone's until the watch has told us).
function page(settings, lang) {
  var s = settings || {};
  var t = function (text, vars) { return i18n.t(lang, text, vars); };
  var html = '<!doctype html><html lang="' + i18n.norm(lang) + '"><head><meta charset="utf-8">' +
    '<meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>' + t('openGym for Pebble') + '</title><style>' +
    'body{font-family:-apple-system,system-ui,sans-serif;background:#000;color:#fff;margin:0;padding:20px 16px 40px}' +
    'h1{font-size:22px;margin:0 0 4px}p{color:#a1a1a6;font-size:14px;line-height:1.45;margin:0 0 18px}' +
    'label{display:block;font-size:14px;color:#a1a1a6;margin:16px 0 6px}' +
    'input,select{box-sizing:border-box;width:100%;font-size:16px;padding:12px;border-radius:10px;' +
    'border:1px solid #333;background:#111;color:#fff}' +
    'button{width:100%;margin-top:26px;padding:14px;font-size:17px;font-weight:600;border:0;border-radius:12px;' +
    'background:#a3e635;color:#000}code{color:#a3e635}.small{font-size:13px;margin-top:6px}' +
    '</style></head><body>' +
    '<h1>openGym</h1>' +
    '<p>' + t('Your own openGym server. The watch keeps your routines and logs workouts without the phone; ' +
    'they upload the next time the phone is near.') + '</p>' +
    '<label for="url">' + t('Server address') + '</label>' +
    '<input id="url" type="url" autocapitalize="off" autocorrect="off" placeholder="https://gym.example.com" value="' + escapeHtml(s.url) + '">' +
    '<label for="token">' + t('API token') + '</label>' +
    '<input id="token" type="password" autocapitalize="off" autocorrect="off" placeholder="ogt_..." value="' + escapeHtml(s.token) + '">' +
    '<p class="small">' + t('Needs openGym 1.4.5 or newer. Sign in on the website, then: Settings &rarr; Integrations &rarr; API tokens.') + ' ' +
    t('Workouts: {a}, Plans &amp; routines: {b}.', { a: '<code>Change directly</code>', b: '<code>See</code>' }) + '</p>' +
    '<label for="lang">' + t('Exercise names') + '</label>' +
    '<select id="lang">' +
    '<option value="">' + t('Like the openGym profile') + '</option>' +
    '<option value="en"' + (s.lang === 'en' ? ' selected' : '') + '>English</option>' +
    '<option value="de"' + (s.lang === 'de' ? ' selected' : '') + '>Deutsch</option>' +
    '</select>' +
    '<label for="step">' + t('Weight step on the watch') + '</label>' +
    '<select id="step">' +
    '<option value="">' + t('Default (2.5 kg / 5 lb)') + '</option>' +
    ['0.5', '1', '1.25', '2', '2.5', '5', '10'].map(function (v) {
      return '<option value="' + v + '"' + (String(s.step) === v ? ' selected' : '') + '>' + v + '</option>';
    }).join('') +
    '</select>' +
    '<label for="vibe">' + t('Vibration when a timer ends') + '</label>' +
    '<select id="vibe">' +
    [['normal', 'Normal (two pulses)'], ['strong', 'Strong (three long pulses)'], ['short', 'Short (one pulse)'], ['off', 'Off']]
      .map(function (o) {
        var sel = (s.vibe || 'normal') === o[0] ? ' selected' : '';
        return '<option value="' + o[0] + '"' + sel + '>' + t(o[1]) + '</option>';
      }).join('') +
    '</select>' +
    '<p class="small">' + t('For the rest timer and timed holds. Off also drops the nudge 10 seconds before a rest ends.') + '</p>' +
    '<button id="save">' + t('Save') + '</button>' +
    '<script>' +
    'function q(n){var m=(location.search+"&"+location.hash).match(new RegExp("[?&#]"+n+"=([^&]*)"));return m?decodeURIComponent(m[1]):null}' +
    'document.getElementById("save").onclick=function(){' +
    'var r={url:document.getElementById("url").value.trim(),token:document.getElementById("token").value.trim(),' +
    'lang:document.getElementById("lang").value,step:document.getElementById("step").value,vibe:document.getElementById("vibe").value};' +
    'location.href=(q("return_to")||"pebblejs://close#")+encodeURIComponent(JSON.stringify(r));};' +
    '</script></body></html>';
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(html);
}

module.exports = { page: page };
