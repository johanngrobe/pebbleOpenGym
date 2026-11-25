// Binary formats shared with the watch (src/c/data.c), and the openGym document shapes.
// Plain ES5: the phone's PebbleKit JS runtime is not guaranteed to know anything newer.

var LIMITS = {
  ex: 64,          // MAX_EX
  routines: 16,    // MAX_ROUTINES
  entries: 16,     // MAX_ENTRIES
  sets: 12,        // MAX_SETS
  overrides: 14,   // MAX_OVERRIDES
  idBytes: 23,     // ID_LEN - 1
  exNameBytes: 39, // EX_NAME_LEN - 1
  routineNameBytes: 24,
  groups: 8,         // MAX_GROUPS: supersets per routine
  groupNameBytes: 23 // GROUP_NAME_LEN - 1
};

// 2: supersets (a group per entry, and each routine's groups after its entries); 3: entry flags
// and warm-up sets.
var PLAN_VERSION = 3;
var PEF_SIDE = 1;
// 2: per set a byte, 1 left / 2 right and 4 a warm-up, a per-side bit in the entry flags, and the
// workout's note at the end. 1 is still read.
var LOG_VERSION = 2;
var MODES = ['reps', 'time', 'cardio', 'distance'];
var ROUTINE_NONE = 0xFF;
var ROUTINE_REST = 0xFE;

// ---------------------------------------------------------------------------------------------
// Bytes

function utf8Bytes(str, maxBytes) {
  var bin = unescape(encodeURIComponent(String(str == null ? '' : str)));
  var out = [];
  for (var i = 0; i < bin.length; i++) out.push(bin.charCodeAt(i));
  if (maxBytes != null && out.length > maxBytes) {
    var end = maxBytes;
    // Never cut a character in half: back off over continuation bytes.
    while (end > 0 && (out[end] & 0xC0) === 0x80) end--;
    out = out.slice(0, end);
  }
  return out;
}

function utf8Decode(bytes) {
  var bin = '';
  for (var i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
  try {
    return decodeURIComponent(escape(bin));
  } catch (e) {
    return bin;
  }
}

function Writer() { this.b = []; }
Writer.prototype.u8 = function (v) { this.b.push(v & 0xFF); };
Writer.prototype.u16 = function (v) { this.u8(v); this.u8(v >>> 8); };
Writer.prototype.u32 = function (v) { this.u16(v & 0xFFFF); this.u16((v >>> 16) & 0xFFFF); };
Writer.prototype.str = function (s, max) {
  var bytes = utf8Bytes(s, Math.min(max, 255));
  this.u8(bytes.length);
  for (var i = 0; i < bytes.length; i++) this.b.push(bytes[i]);
};

function Reader(bytes) { this.b = bytes; this.p = 0; }
Reader.prototype.u8 = function () {
  if (this.p >= this.b.length) throw new Error('record too short');
  return this.b[this.p++] & 0xFF;
};
Reader.prototype.u16 = function () { var lo = this.u8(); return lo | (this.u8() << 8); };
Reader.prototype.u32 = function () { var lo = this.u16(); return (lo + this.u16() * 65536) >>> 0; };
Reader.prototype.str = function () {
  var n = this.u8();
  if (this.p + n > this.b.length) throw new Error('record too short');
  var s = utf8Decode(this.b.slice(this.p, this.p + n));
  this.p += n;
  return s;
};

// ---------------------------------------------------------------------------------------------
// Dates and numbers

function pad(n) { return (n < 10 ? '0' : '') + n; }

function isoLocal(date) {
  return date.getFullYear() + '-' + pad(date.getMonth() + 1) + '-' + pad(date.getDate());
}

function ymdLocal(date) {
  return date.getFullYear() * 10000 + (date.getMonth() + 1) * 100 + date.getDate();
}

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function num(v) {
  var n = typeof v === 'number' ? v : parseFloat(v);
  return isFinite(n) ? n : 0;
}

function toW20(w) { return clamp(Math.round(num(w) * 20), 0, 65535); }

// ---------------------------------------------------------------------------------------------
// Plan: routines -> watch

function modeOf(cfg) {
  var m = cfg && cfg.mode;
  if (m === 'reps' || m === 'time' || m === 'cardio' || m === 'distance') return m;
  if (cfg && (cfg.min != null || cfg.speed != null)) return 'cardio';
  return 'reps';
}

function amountOf(cfg, mode) {
  var v;
  if (mode === 'time') v = num(cfg.sec) || 45;
  else if (mode === 'cardio') v = num(cfg.min) || 20;
  else if (mode === 'distance') v = num(cfg.m) || 20;
  else v = parseInt(cfg.reps, 10) || 10;
  return clamp(Math.round(v), 0, 65535);
}

function capitalize(s) {
  return s ? s.charAt(0).toUpperCase() + s.slice(1) : s;
}

// What each exercise is called, in the profile's language when we have it.
function nameResolver(state, names, lang) {
  var custom = {};
  (state.customEx || []).forEach(function (c) {
    if (c && c.id != null && c.n) custom[String(c.id)] = c.n;
  });
  var l = String(lang || 'en');
  var tables = [names[l], names[l.split('-')[0]], names.en].filter(Boolean);
  var alias = names.alias || {};
  return function (id) {
    id = String(id);
    if (custom[id]) return custom[id];
    if (alias[id]) id = alias[id];
    for (var i = 0; i < tables.length; i++) {
      if (tables[i][id]) return capitalize(tables[i][id]);
    }
    return 'Exercise ' + id;
  };
}

// The heaviest done set of the newest workout that has this exercise.
function lastTopWeights(workouts) {
  var top = {};
  for (var i = 0; i < workouts.length; i++) {
    var w = workouts[i];
    (w && w.entries || []).forEach(function (e) {
      if (!e || e.id == null) return;
      var best = 0;
      (e.sets || []).forEach(function (s) {
        if (s && s.done !== false && !s.warmup && num(s.w) > best) best = num(s.w);
      });
      if (best > 0) top[String(e.id)] = best;  // later workouts overwrite earlier ones
    });
  }
  return top;
}

function usableRoutines(state) {
  return (state.routines || []).filter(function (r) {
    return r && r.id != null && Array.isArray(r.ex) && r.ex.length && !r.archived && !r.hidden;
  });
}

function scheduledIds(state, now) {
  var ids = [];
  var week = state.week || {};
  var dayPlan = state.dayPlan || {};
  var today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  for (var i = 0; i < LIMITS.overrides; i++) {
    var d = new Date(today.getFullYear(), today.getMonth(), today.getDate() + i);
    var id = dayPlan[isoLocal(d)];
    if (id == null) id = week[d.getDay()] != null ? week[d.getDay()] : week[String(d.getDay())];
    if (id != null && id !== 'rest' && ids.indexOf(String(id)) < 0) ids.push(String(id));
  }
  return ids;
}

// Supersets: openGym gives the members of a group the same `sg` on the routine exercise, with the
// group's name, rest and circuit settings on its members (the first that has one speaks for the
// group). Sets e.sg on each entry, 1-based in the order the groups come, 0 for a plain exercise,
// and returns the groups. A group with one member left (the others trimmed) is no superset.
function groupEntries(entries) {
  var count = {};
  entries.forEach(function (e) {
    var sg = e.cfg.sg;
    if (sg != null && sg !== '' && sg !== false) count[String(sg)] = (count[String(sg)] || 0) + 1;
  });
  var groups = [];
  var number = {};
  entries.forEach(function (e) {
    var key = e.cfg.sg != null && e.cfg.sg !== '' && e.cfg.sg !== false ? String(e.cfg.sg) : null;
    e.sg = 0;
    if (key == null || count[key] < 2) return;
    if (number[key] == null) {
      if (groups.length >= LIMITS.groups) return;
      groups.push({ name: '', rest: 0, circuit: false });
      number[key] = groups.length;
    }
    var g = groups[number[key] - 1];
    var name = typeof e.cfg.sgName === 'string' ? e.cfg.sgName.trim() : '';
    if (!g.name && name) g.name = name;
    if (!g.rest && num(e.cfg.sgRest) > 0) g.rest = clamp(Math.round(num(e.cfg.sgRest)), 0, 3600);
    if (e.cfg.sgCircuit && typeof e.cfg.sgCircuit === 'object') g.circuit = true;
    e.sg = number[key];
  });
  return groups;
}

function buildPlan(state, chosen, opts) {
  var nameOf = opts.nameOf;
  var lastTop = opts.lastTop;
  var exWeights = state.exWeights || {};
  var w = new Writer();
  var unit = state.unit === 'lb' ? 1 : 0;
  w.u8(PLAN_VERSION);
  w.u8(unit);
  w.u16(clamp(Math.round(num(state.restSec) || 90), 0, 3600));
  w.u16(opts.step ? clamp(Math.round(num(opts.step) * 20), 1, 2000) : (unit ? 100 : 50));
  w.u32(Math.floor(opts.now.getTime() / 1000));

  var routineIdx = {};
  chosen.forEach(function (r, i) { routineIdx[String(r.id)] = i; });
  var idxOf = function (id) {
    if (id === 'rest') return ROUTINE_REST;
    var i = routineIdx[String(id)];
    return i == null ? ROUTINE_NONE : i;
  };

  var week = state.week || {};
  for (var d = 0; d < 7; d++) {
    var wid = week[d] != null ? week[d] : week[String(d)];
    w.u8(wid == null ? ROUTINE_NONE : idxOf(wid));
  }

  var overrides = [];
  var dayPlan = state.dayPlan || {};
  var now = opts.now;
  for (var i = 0; i < LIMITS.overrides; i++) {
    var day = new Date(now.getFullYear(), now.getMonth(), now.getDate() + i);
    var v = dayPlan[isoLocal(day)];
    if (v == null) continue;
    var idx = idxOf(v);
    if (idx !== ROUTINE_NONE) overrides.push([ymdLocal(day), idx]);
  }
  w.u8(overrides.length);
  overrides.forEach(function (o) { w.u32(o[0]); w.u8(o[1]); });

  // Exercises, each once.
  var exList = [];
  var exIdx = {};
  var routines = chosen.map(function (r) {
    var entries = [];
    r.ex.forEach(function (cfg) {
      if (!cfg || cfg.id == null || entries.length >= LIMITS.entries) return;
      var id = String(cfg.id);
      if (exIdx[id] == null) {
        if (exList.length >= LIMITS.ex) return;
        exIdx[id] = exList.length;
        exList.push({ id: id, bodyweight: cfg.bodyweight === true });
      }
      var mode = modeOf(cfg);
      var weight = num(cfg.weight);
      if (!(weight > 0) && cfg.bodyweight !== true) {
        weight = lastTop[id] || num(exWeights[id] && exWeights[id].w) || 0;
      }
      entries.push({
        cfg: cfg,
        ex: exIdx[id],
        sets: mode === 'cardio' ? 1 : clamp(parseInt(cfg.sets, 10) || 3, 1, LIMITS.sets),
        mode: MODES.indexOf(mode),
        amount: amountOf(cfg, mode),
        w20: mode === 'cardio' ? 0 : toW20(weight),
        rest: clamp(Math.round(num(cfg.restSec)), 0, 3600),
        flags: cfg.side ? PEF_SIDE : 0,
        warm: clamp(Math.round(num(cfg.warmupSets)), 0, 5)  // openGym's MAX_PLANNED_WARMUPS
      });
    });
    return { id: String(r.id), name: r.name || 'Routine', entries: entries, groups: groupEntries(entries) };
  });

  w.u8(exList.length);
  exList.forEach(function (e) {
    w.str(e.id, LIMITS.idBytes);
    w.str(nameOf(e.id), LIMITS.exNameBytes);
    w.u8(e.bodyweight ? 1 : 0);
  });

  w.u8(routines.length);
  routines.forEach(function (r) {
    w.str(r.id, LIMITS.idBytes);
    w.str(r.name, LIMITS.routineNameBytes);
    w.u8(r.entries.length);
    r.entries.forEach(function (e) {
      w.u8(e.ex);
      w.u8(e.sets);
      w.u8(e.mode);
      w.u16(e.amount);
      w.u16(e.w20);
      w.u16(e.rest);
      w.u8(e.sg);
      w.u8(e.flags);
      w.u8(e.warm);
    });
    w.u8(r.groups.length);
    r.groups.forEach(function (g) {
      w.str(g.name, LIMITS.groupNameBytes);
      w.u16(g.rest);
      w.u8(g.circuit ? 1 : 0);
    });
  });
  return w.b;
}

// The routines as the watch stores them, trimmed to fit `budget` bytes: the routines on the
// plan for the next two weeks go first, then the rest in the app's order.
// opts: { names, lang, step, budget, now }
function encodePlan(state, opts) {
  var now = opts.now || new Date();
  var all = usableRoutines(state).slice(0, 64);
  var scheduled = scheduledIds(state, now);
  var priority = all.slice().sort(function (a, b) {
    var pa = scheduled.indexOf(String(a.id)), pb = scheduled.indexOf(String(b.id));
    pa = pa < 0 ? 1e6 + all.indexOf(a) : pa;
    pb = pb < 0 ? 1e6 + all.indexOf(b) : pb;
    return pa - pb;
  });
  var build = {
    nameOf: nameResolver(state, opts.names || {}, opts.lang || state.lang),
    lastTop: lastTopWeights(state.workouts || []),
    step: opts.step,
    now: now
  };
  var budget = opts.budget || 2048;
  for (var k = Math.min(priority.length, LIMITS.routines); k >= 0; k--) {
    var keep = priority.slice(0, k);
    var chosen = all.filter(function (r) { return keep.indexOf(r) >= 0; });
    var bytes = buildPlan(state, chosen, build);
    if (bytes.length <= budget) {
      return { bytes: bytes, routines: chosen.length, dropped: all.length - chosen.length };
    }
  }
  return { bytes: buildPlan(state, [], build), routines: 0, dropped: all.length };
}

// ---------------------------------------------------------------------------------------------
// Logs: watch -> openGym

function decodeLog(bytes) {
  var r = new Reader(bytes);
  var ver = r.u8();
  if (ver < 1 || ver > LOG_VERSION) throw new Error('unknown record version ' + ver);
  var rec = { start: r.u32(), end: r.u32(), routineId: r.str(), routineName: r.str(), entries: [] };
  var n = r.u8();
  for (var i = 0; i < n; i++) {
    var e = { id: r.str(), mode: MODES[r.u8()] || 'reps', flags: r.u8(), sets: [] };
    e.side = (e.flags & 2) !== 0;
    var ns = r.u8();
    for (var k = 0; k < ns; k++) {
      var set = { w: r.u16() / 20, amt: r.u16() };
      if (ver >= 2) {
        var b = r.u8();
        set.side = [null, 'L', 'R'][b & 3] || null;
        set.warm = (b & 4) !== 0;
      }
      e.sets.push(set);
    }
    rec.entries.push(e);
  }
  if (ver >= 2) rec.note = r.str().trim();
  return rec;
}

function workoutId(start) {
  return 'pbl' + start.toString(36);
}

// One logged row as openGym keeps it. A per-side hold is a left and a right row (openGym tags
// them `side`); per-side reps are logged as the total of both sides, as openGym does. A warm-up
// carries openGym's marks (lib/history.js insertWarmupRow).
function setRow(mode, s) {
  var row;
  if (mode === 'time') row = { w: s.w, sec: s.amt };
  else if (mode === 'cardio') row = { min: s.amt };
  else if (mode === 'distance') row = { m: s.amt, w: s.w };
  else row = { w: s.w, r: s.amt };
  if (mode === 'time' && s.side) row.side = s.side;
  if (s.warm) { row.phase = 'warmup'; row.warmup = true; }
  row.done = true;
  return row;
}

// A decoded record as openGym saves a finished workout (frontend lib/finish-workout.js).
// Warm-ups count for neither the top weight nor the volume.
function toWorkout(rec) {
  var entries = rec.entries.map(function (e) {
    var sets = e.sets.map(function (s) { return setRow(e.mode, s); });
    var top = 0;
    sets.forEach(function (s) { if (!s.warmup && num(s.w) > top) top = num(s.w); });
    var out = { id: e.id, sets: sets, topW: top || null };
    if (e.side) out.side = true;  // until mergeWorkouts sets the routine's target
    return out;
  });
  var vol = 0;
  entries.forEach(function (e) {
    e.sets.forEach(function (s) { if (s.r && !s.warmup) vol += num(s.w) * s.r; });
  });
  var start = rec.start * 1000;
  var end = Math.max(rec.end * 1000, start);
  var workout = {
    id: workoutId(rec.start),
    d: isoLocal(new Date(start)),
    start: start,
    end: end,
    routineIds: rec.routineId ? [rec.routineId] : [],
    routineId: rec.routineId || null,
    name: rec.routineName || 'Workout',
    entries: entries,
    prs: [],
    vol: Math.round(vol * 100) / 100,
    source: 'pebble'
  };
  if (rec.note) workout.note = rec.note;  // openGym's workout note (lib/finish-workout.js)
  return workout;
}

function bestWeight(entry) {
  var best = 0;
  (entry && entry.sets || []).forEach(function (s) {
    if (s && s.done !== false && !s.warmup && num(s.w) > best) best = num(s.w);
  });
  return best;
}

// What each exercise was meant to be, as openGym keeps it on a logged entry (`target`): the
// routine's settings for it, so openGym reads the sets the way it planned them (per side among
// them). An exercise the routine no longer has gets the little the watch knows.
function addTargets(state, workout) {
  var routine = null;
  (state.routines || []).forEach(function (r) {
    if (r && String(r.id) === String(workout.routineId)) routine = r;
  });
  var used = {};
  workout.entries.forEach(function (e) {
    var side = e.side === true;
    delete e.side;
    var cfg = null;
    (routine && routine.ex || []).forEach(function (c, i) {
      if (!cfg && !used[i] && c && String(c.id) === String(e.id)) { cfg = c; used[i] = true; }
    });
    if (cfg) e.target = JSON.parse(JSON.stringify(cfg));
    else if (side) e.target = { id: e.id, side: true, mode: e.sets[0] && e.sets[0].sec != null ? 'time' : 'reps' };
  });
}

// Adds the waiting workouts to the profile's history. Returns the keys to write, or null when
// every one of them is there already.
function mergeWorkouts(state, waiting, now) {
  var workouts = (state.workouts || []).slice();
  var have = {};
  workouts.forEach(function (w) { if (w && w.id != null) have[w.id] = true; });
  var exWeights = {};
  var src = state.exWeights || {};
  for (var k in src) if (Object.prototype.hasOwnProperty.call(src, k)) exWeights[k] = src[k];

  var added = [];
  waiting.slice().sort(function (a, b) { return a.start - b.start; }).forEach(function (w) {
    if (have[w.id]) return;
    var before = workouts.filter(function (x) { return x && num(x.start) < w.start; });
    var prs = [];
    w.entries.forEach(function (e) {
      var mine = bestWeight(e);
      if (!(mine > 0)) return;
      var prev = 0;
      before.forEach(function (x) {
        (x.entries || []).forEach(function (pe) {
          if (pe && String(pe.id) === String(e.id)) prev = Math.max(prev, bestWeight(pe));
        });
      });
      if (prev > 0 && mine > prev) prs.push(e.id);
      var known = exWeights[e.id];
      if (!known || mine > num(known.w)) exWeights[e.id] = { w: mine, d: w.d };
    });
    var copy = JSON.parse(JSON.stringify(w));
    copy.prs = prs;
    addTargets(state, copy);
    copy._ts = now || Date.now();
    workouts.push(copy);
    have[w.id] = true;
    added.push(w.id);
  });
  if (!added.length) return null;
  workouts.sort(function (a, b) { return num(a && a.start) - num(b && b.start); });
  return { state: { workouts: workouts, exWeights: exWeights }, added: added };
}

module.exports = {
  LIMITS: LIMITS,
  utf8Bytes: utf8Bytes,
  encodePlan: encodePlan,
  decodeLog: decodeLog,
  toWorkout: toWorkout,
  mergeWorkouts: mergeWorkouts,
  workoutId: workoutId,
  isoLocal: isoLocal
};
