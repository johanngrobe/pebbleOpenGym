// The phone half of openGym for Pebble. The watch runs workouts on its own; this side only
// carries data when the phone is near:
//   watch --LOG_DATA--> here: a finished workout, kept in localStorage until openGym has it
//   here  --PLAN_*-->  watch: the routines, trimmed to what the watch can store
var api = require('./api');
var codec = require('./codec');
var config = require('./config');
var names = require('./exercise_names');
var i18n = require('./i18n');
var look = require('./look');

var CMD = {
  HELLO: 1, PLAN_BEGIN: 2, PLAN_CHUNK: 3, PLAN_END: 4, LOG_ACK: 5, STATUS: 6, LOOK: 7,
  SYNC_REQ: 10, LOG_DATA: 11
};
var STATUS = { WORKING: 0, DONE: 1, ERROR: 2 };
var CHUNK = 1000;

// ---------------------------------------------------------------------------------------------
// Storage

function load(key, fallback) {
  try {
    var v = localStorage.getItem(key);
    return v ? JSON.parse(v) : fallback;
  } catch (e) {
    return fallback;
  }
}

function save(key, value) {
  localStorage.setItem(key, JSON.stringify(value));
}

function settings() {
  return load('settings', {});
}

function outbox() {
  return load('outbox', []);
}

// ---------------------------------------------------------------------------------------------
// Messages to the watch, one at a time

var queue = [];
var sending = false;
var tries = 0;

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  Pebble.sendAppMessage(queue[0], function () {
    queue.shift();
    sending = false;
    tries = 0;
    pump();
  }, function () {
    sending = false;
    if (++tries > 4) {
      console.log('watch unreachable, dropping ' + queue.length + ' messages');
      queue = [];
      tries = 0;
      return;
    }
    setTimeout(pump, 400 * tries);
  });
}

function send(msg) {
  queue.push(msg);
  pump();
}

// The watch's language, as it last told us (sent with every sync request).
function watchLang() {
  return load('watchLang', '');
}

function tr(text, vars) {
  return i18n.t(watchLang(), text, vars);
}

function errText(err) {
  return err.key ? tr(err.key, err.vars) : err.message;
}

function utf8Len(s) {
  return unescape(encodeURIComponent(s)).length;
}

// At most 60 bytes for the watch's 64-byte status, cut between characters (umlauts are 2 bytes).
function status(state, text) {
  var s = String(text).slice(0, 60);
  while (utf8Len(s) > 60) s = s.slice(0, -1);
  send({ Cmd: CMD.STATUS, Count: state, Text: s });
}

function sendPlan(bytes) {
  send({ Cmd: CMD.PLAN_BEGIN, Total: bytes.length });
  for (var off = 0; off < bytes.length; off += CHUNK) {
    send({ Cmd: CMD.PLAN_CHUNK, Seq: off, Data: bytes.slice(off, off + CHUNK) });
  }
  send({ Cmd: CMD.PLAN_END, Total: bytes.length });
}

// ---------------------------------------------------------------------------------------------
// Sync

function configured(cfg) {
  return cfg && cfg.url && cfg.token;
}

// Uploads every workout waiting here. cb(err, count)
function flushOutbox(cfg, cb) {
  var waiting = outbox();
  if (!waiting.length) return cb(null, 0);
  api.update(cfg, function (state) {
    return codec.mergeWorkouts(state, waiting, Date.now());
  }, function (err) {
    if (err) return cb(err);
    // Whatever was added, or was there already: openGym has all of them now.
    var sent = {};
    waiting.forEach(function (w) { sent[w.id] = true; });
    save('outbox', outbox().filter(function (w) { return !sent[w.id]; }));
    cb(null, waiting.length);
  });
}

function sendRoutines(cfg, budget, uploaded) {
  status(STATUS.WORKING, tr('Loading routines...'));
  api.getData(cfg, function (err, doc) {
    if (err) return status(STATUS.ERROR, errText(err));
    profileLook(doc.state);
    var plan = codec.encodePlan(doc.state, {
      names: names,
      lang: cfg.lang || doc.state.lang,
      step: cfg.step ? parseFloat(cfg.step) : null,
      budget: budget,
      now: new Date()
    });
    console.log('plan: ' + plan.bytes.length + ' bytes, ' + plan.routines + ' routines, ' +
      plan.dropped + ' left out, ' + uploaded + ' uploaded');
    sendPlan(plan.bytes);
  });
}

var syncing = false;
var again = null;  // a request that came in while one was running

function onSyncRequest(wantRoutines, budget) {
  var cfg = settings();
  if (!configured(cfg)) {
    status(STATUS.ERROR, tr('Set up in the Pebble app'));
    return;
  }
  if (syncing) {
    again = [wantRoutines, budget];
    return;
  }
  syncing = true;
  if (outbox().length) status(STATUS.WORKING, tr('Uploading to openGym...'));
  flushOutbox(cfg, function (err, count) {
    syncing = false;
    if (again) {
      var next = again;
      again = null;
      return onSyncRequest(next[0], next[1]);
    }
    if (err) {
      status(STATUS.ERROR, errText(err));
      return;
    }
    if (wantRoutines) sendRoutines(cfg, budget, count);
    else status(STATUS.DONE, count ? tr(count > 1 ? 'Uploaded {n} workouts' : 'Uploaded {n} workout', { n: count }) : tr('Up to date'));
  });
}

function onLog(seq, data) {
  try {
    var workout = codec.toWorkout(codec.decodeLog(data));
    var box = outbox();
    if (!box.some(function (w) { return w.id === workout.id; })) box.push(workout);
    save('outbox', box);
    console.log('stored workout ' + workout.id + ' (' + workout.entries.length + ' exercises)');
  } catch (e) {
    // A record that cannot be read would block the queue on the watch forever: keep its bytes
    // for a look later and let the watch move on.
    console.log('unreadable workout record: ' + e.message);
    var bad = load('unreadable', []);
    bad.push({ seq: seq, data: data, at: Date.now() });
    save('unreadable', bad.slice(-10));
  }
  send({ Cmd: CMD.LOG_ACK, Seq: seq });
}

// ---------------------------------------------------------------------------------------------
// Events

// Hello starts a sync on the watch, and carries the settings the watch keeps itself.
var VIBE = { off: 0, short: 1, normal: 2, strong: 3 };
var SIZE = { auto: 0, normal: 1, large: 2, xl: 3 };
var FOCUS = { off: 0, remind: 1 };
// The workout's progress in the top bar: a little pie or numbers ("12/30").
var PROGRESS = { pie: 0, numbers: 1 };
// Seconds between heart rate readings during a workout; 'on' is an older setting.
var HEART = { off: 0, '1': 1, '5': 5, '10': 10, '30': 30 };
function heartSetting(v) {
  return HEART[v] != null && v !== 'on' ? String(v) : '5';
}

// The colours as the watch should draw them: the settings page's choice, or the openGym profile's
// as last seen. { Theme, Accent, OnAccent }
function colors() {
  var s = settings();
  return look.encode(look.resolve({ theme: s.theme, accent: s.accent }, load('profileLook', null)));
}

function hello() {
  var s = settings();
  var v = VIBE[s.vibe];
  var c = colors();
  save('sentLook', c);
  send({
    Cmd: CMD.HELLO, Vibe: v == null ? VIBE.normal : v, Size: SIZE[s.size] || SIZE.auto,
    Focus: s.focus === 'off' ? FOCUS.off : FOCUS.remind,
    Heart: HEART[heartSetting(s.heart)],
    Progress: PROGRESS[s.progress] || PROGRESS.pie,
    Theme: c.Theme, Accent: c.Accent, OnAccent: c.OnAccent
  });
}

// The profile's theme and accent, from a sync. Changed in openGym: the watch gets them right away.
function profileLook(state) {
  var p = look.fromProfile(state);
  if (!p) return;
  save('profileLook', p);
  var c = colors();
  if (JSON.stringify(c) === JSON.stringify(load('sentLook', null))) return;
  save('sentLook', c);
  send({ Cmd: CMD.LOOK, Theme: c.Theme, Accent: c.Accent, OnAccent: c.OnAccent });
}

Pebble.addEventListener('ready', hello);

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (p.Cmd === CMD.LOG_DATA) onLog(p.Seq, p.Data || []);
  else if (p.Cmd === CMD.SYNC_REQ) {
    if (p.Text) save('watchLang', i18n.norm(p.Text));
    onSyncRequest(p.Count === 1, p.Total || 2048);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  var lang = watchLang() || (typeof navigator !== 'undefined' && navigator.language) || '';
  Pebble.openURL(config.page(settings(), lang));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var value;
  try {
    value = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    try { value = JSON.parse(e.response); } catch (err2) { return; }
  }
  save('settings', {
    url: api.baseUrl(value.url),
    token: String(value.token || '').trim(),
    lang: value.lang || '',
    step: value.step || '',
    vibe: VIBE[value.vibe] != null ? value.vibe : 'normal',
    focus: FOCUS[value.focus] != null ? value.focus : 'remind',
    heart: heartSetting(value.heart),
    progress: PROGRESS[value.progress] != null ? value.progress : 'pie',
    size: SIZE[value.size] != null ? value.size : 'auto',
    theme: look.THEME[value.theme] != null ? value.theme : '',
    accent: look.PRESETS[value.accent] ? value.accent : ''
  });
  // Tell the watch, which syncs on hello.
  hello();
});
