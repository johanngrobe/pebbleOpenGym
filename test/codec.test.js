// node test/codec.test.js — checks the phone-side formats against the watch's layout.
var assert = require('assert');
var codec = require('../src/pkjs/codec');
var names = require('../src/pkjs/exercise_names');

// --- A reader mirroring plan_parse() in src/c/data.c ---------------------------------------
function parsePlan(b) {
  var p = 0;
  var u8 = function () { return b[p++]; };
  var u16 = function () { var v = b[p] | (b[p + 1] << 8); p += 2; return v; };
  var u32 = function () { var v = (u16() + u16() * 65536); return v; };
  var str = function () { var n = u8(); var s = Buffer.from(b.slice(p, p + n)).toString('utf8'); p += n; return s; };
  var plan = { ver: u8(), unit: u8(), rest: u16(), step20: u16(), synced: u32(), week: [] };
  for (var i = 0; i < 7; i++) plan.week.push(u8());
  plan.over = [];
  for (var n = u8(), i = 0; i < n; i++) plan.over.push([u32(), u8()]);
  plan.ex = [];
  for (n = u8(), i = 0; i < n; i++) plan.ex.push({ id: str(), name: str(), flags: u8() });
  plan.routines = [];
  for (n = u8(), i = 0; i < n; i++) {
    var r = { id: str(), name: str(), e: [] };
    for (var m = u8(), k = 0; k < m; k++) r.e.push({ ex: u8(), sets: u8(), mode: u8(), amount: u16(), w20: u16(), rest: u16(), sg: u8(), flags: u8(), warm: u8() });
    r.g = [];
    for (m = u8(), k = 0; k < m; k++) r.g.push({ name: str(), rest: u16(), circuit: u8() });
    plan.routines.push(r);
  }
  assert.strictEqual(p, b.length, 'plan has trailing bytes');
  return plan;
}

var now = new Date(2026, 10, 25, 9, 0, 0); // Wednesday
var state = {
  unit: 'kg', lang: 'de', restSec: 120,
  routines: [
    { id: 'a', name: 'Oberkörper schwer', ex: [
      { id: '0025', sets: 4, reps: 6, weight: 0 },
      { id: '0251', sets: 3, reps: 10, weight: 0, bodyweight: true },
      { id: 'cx1', sets: 2, sec: 30, mode: 'time', weight: 0 },
    ] },
    { id: 'b', name: 'Cardio', ex: [{ id: '0685', sets: 1, min: 25, speed: 10 }] },
    { id: 'c', name: 'Empty', ex: [] },
  ],
  customEx: [{ id: 'cx1', n: 'Meine Übung mit sehr langem Namen über dreißig Bytes', custom: true }],
  week: { 3: 'a', 5: 'b' },
  dayPlan: { '2026-11-26': 'rest', '2026-11-27': 'a' },
  exWeights: { '0025': { w: 50, d: '2026-11-01' } },
  workouts: [{ id: 'w1', start: 1, entries: [{ id: '0025', sets: [{ w: 62.5, r: 6, done: true }, { w: 70, r: 1, done: false }] }] }],
};

var res = codec.encodePlan(state, { names: names, budget: 4000, now: now });
var plan = parsePlan(res.bytes);
assert.strictEqual(plan.ver, 3);
assert.strictEqual(plan.rest, 120);
assert.strictEqual(plan.step20, 50);
assert.strictEqual(plan.routines.length, 2, 'empty routine left out');
assert.deepStrictEqual(plan.week, [255, 255, 255, 0, 255, 1, 255]);
assert.deepStrictEqual(plan.over, [[20261126, 0xFE], [20261127, 0]]);
assert.strictEqual(plan.ex[0].name, 'Bankdrücken mit Langhantel', 'German name');
assert.strictEqual(plan.ex[1].flags, 1, 'bodyweight flag');
assert.ok(Buffer.byteLength(plan.ex[2].name) <= 39 && plan.ex[2].name.indexOf('�') < 0, 'cut on a character boundary');
assert.strictEqual(plan.routines[0].e[0].w20, 62.5 * 20, 'last top weight, done sets only');
assert.strictEqual(plan.routines[0].e[2].mode, 1);
assert.strictEqual(plan.routines[0].e[2].amount, 30);
assert.strictEqual(plan.routines[1].e[0].mode, 2);
assert.strictEqual(plan.routines[1].e[0].amount, 25);

assert.deepStrictEqual(plan.routines[0].g, [], 'no supersets');
assert.strictEqual(plan.routines[0].e[0].sg, 0);

// Supersets: members share `sg`; name and rest come from the first member that has them.
var ssState = Object.assign({}, state, { routines: [{ id: 's', name: 'Arme', ex: [
  { id: '0025', sets: 3, reps: 8 },
  { id: '0251', sets: 3, reps: 10, sg: 'g1' },
  { id: '0426', sets: 3, reps: 12, sg: 'g1', sgName: 'Bizeps & Trizeps', sgRest: 75 },
  { id: '0334', sets: 2, reps: 15, sg: 'g2', sgCircuit: { rounds: 3 } },
  { id: '0027', sets: 2, reps: 15, sg: 'g2' },
  { id: '0031', sets: 3, reps: 10, sg: 'lonely' },
] }], week: {}, dayPlan: {} });
var ss = parsePlan(codec.encodePlan(ssState, { names: names, budget: 4000, now: now }).bytes).routines[0];
assert.deepStrictEqual(ss.e.map(function (e) { return e.sg; }), [0, 1, 1, 2, 2, 0], 'group per entry; one member is no superset');
assert.deepStrictEqual(ss.g, [{ name: 'Bizeps & Trizeps', rest: 75, circuit: 0 }, { name: '', rest: 0, circuit: 1 }]);

// Per side: a flag on the entry; reps stay openGym's total of both sides.
var sideState = Object.assign({}, state, { routines: [{ id: 'u', name: 'Einseitig', ex: [
  { id: '0025', sets: 3, reps: 16, side: true },
  { id: 'cx1', sets: 2, sec: 30, mode: 'time', side: true },
  { id: '0251', sets: 3, reps: 10, warmupSets: 2 },
] }], week: {}, dayPlan: {} });
var su = parsePlan(codec.encodePlan(sideState, { names: names, budget: 4000, now: now }).bytes).routines[0];
assert.deepStrictEqual(su.e.map(function (e) { return e.flags; }), [1, 1, 0]);
assert.strictEqual(su.e[0].amount, 16, 'total reps, the watch halves them for display');
assert.deepStrictEqual(su.e.map(function (e) { return e.warm; }), [0, 0, 2], 'warm-up sets');

// Trimming to a budget keeps the routine planned soonest.
var oneOnly = Object.assign({}, state, { routines: [state.routines[0]] });
var budget = codec.encodePlan(oneOnly, { names: names, budget: 99999, now: now }).bytes.length;
var small = codec.encodePlan(state, { names: names, budget: budget, now: now });
var sp = parsePlan(small.bytes);
assert.ok(small.bytes.length <= budget, 'within budget');
assert.strictEqual(sp.routines.length, 1);
assert.strictEqual(sp.routines[0].id, 'a');
assert.strictEqual(small.dropped, 1);

// --- A record laid out like sess_record() in src/c/data.c -------------------------------------
function record() {
  var b = [];
  var u8 = function (v) { b.push(v & 255); };
  var u16 = function (v) { u8(v); u8(v >> 8); };
  var u32 = function (v) { u16(v & 0xFFFF); u16(v >>> 16); };
  var str = function (s) { var x = Buffer.from(s, 'utf8'); u8(x.length); for (var i = 0; i < x.length; i++) b.push(x[i]); };
  u8(1); u32(1795000000); u32(1795003000); str('a'); str('Oberkörper schwer'); u8(2);
  str('0025'); u8(0); u8(0); u8(2); u16(62.5 * 20); u16(6); u16(65 * 20); u16(5);
  str('cx1'); u8(1); u8(0); u8(1); u16(0); u16(28);
  return b;
}
var rec = codec.decodeLog(record());
assert.strictEqual(rec.entries[0].sets[0].side, undefined, 'a version 1 record has no sides');
var w = codec.toWorkout(rec);
assert.strictEqual(w.id, 'pbl' + (1795000000).toString(36));
assert.strictEqual(w.name, 'Oberkörper schwer');
assert.strictEqual(w.start, 1795000000000);
assert.deepStrictEqual(w.entries[0].sets, [{ w: 62.5, r: 6, done: true }, { w: 65, r: 5, done: true }]);
assert.strictEqual(w.entries[0].topW, 65);
assert.deepStrictEqual(w.entries[1].sets, [{ w: 0, sec: 28, done: true }]);
assert.strictEqual(w.vol, 62.5 * 6 + 65 * 5);

// Version 2: a per-side hold as left/right rows, per-side reps as the total.
function recordV2() {
  var b = [];
  var u8 = function (v) { b.push(v & 255); };
  var u16 = function (v) { u8(v); u8(v >> 8); };
  var u32 = function (v) { u16(v & 0xFFFF); u16(v >>> 16); };
  var str = function (s) { var x = Buffer.from(s, 'utf8'); u8(x.length); for (var i = 0; i < x.length; i++) b.push(x[i]); };
  u8(2); u32(1795100000); u32(1795103000); str('u'); str('Einseitig'); u8(2);
  str('0025'); u8(0); u8(2); u8(1); u16(20 * 20); u16(16); u8(0);
  str('cx1'); u8(1); u8(2); u8(2); u16(0); u16(30); u8(1); u16(0); u16(28); u8(2);
  str('');
  return b;
}
function recordWarm() {
  var b = [];
  var u8 = function (v) { b.push(v & 255); };
  var u16 = function (v) { u8(v); u8(v >> 8); };
  var u32 = function (v) { u16(v & 0xFFFF); u16(v >>> 16); };
  var str = function (s) { var x = Buffer.from(s, 'utf8'); u8(x.length); for (var i = 0; i < x.length; i++) b.push(x[i]); };
  u8(2); u32(1795200000); u32(1795203000); str('a'); str('Oberkörper schwer'); u8(1);
  str('0025'); u8(0); u8(0); u8(3); u16(40 * 20); u16(6); u8(4); u16(80 * 20); u16(6); u8(0); u16(80 * 20); u16(5); u8(0);
  str('Schulter zwickt, nächstes Mal leichter');
  return b;
}
var w2 = codec.toWorkout(codec.decodeLog(recordV2()));
assert.deepStrictEqual(w2.entries[0].sets, [{ w: 20, r: 16, done: true }]);
assert.deepStrictEqual(w2.entries[1].sets, [{ w: 0, sec: 30, side: 'L', done: true }, { w: 0, sec: 28, side: 'R', done: true }]);
var m2 = codec.mergeWorkouts(sideState, [w2], 1).state.workouts.filter(function (x) { return x.id === w2.id; })[0];
assert.deepStrictEqual(m2.entries[0].target, sideState.routines[0].ex[0], "the routine's settings as target");
assert.strictEqual(m2.entries[1].target.side, true);
assert.strictEqual(m2.entries[0].side, undefined, 'no stray flag on the entry');

// Warm-ups: openGym's marks, and left out of top weight and volume.
var ww = codec.toWorkout(codec.decodeLog(recordWarm()));
assert.deepStrictEqual(ww.entries[0].sets[0], { w: 40, r: 6, phase: 'warmup', warmup: true, done: true });
assert.strictEqual(ww.entries[0].sets[1].warmup, undefined);
assert.strictEqual(ww.entries[0].topW, 80);
assert.strictEqual(ww.vol, 80 * 6 + 80 * 5);
assert.strictEqual(ww.note, 'Schulter zwickt, nächstes Mal leichter', 'dictated note');
assert.strictEqual(w2.note, undefined, 'no note, no field');

// --- Merging into the profile ----------------------------------------------------------------
var merged = codec.mergeWorkouts(state, [w], 42);
assert.deepStrictEqual(Object.keys(merged.state).sort(), ['exWeights', 'workouts'], 'writes only Workouts keys');
assert.strictEqual(merged.state.workouts.length, 2);
assert.deepStrictEqual(merged.state.workouts[1].prs, ['0025'], 'heavier than before: a PR');
assert.deepStrictEqual(merged.state.exWeights['0025'], { w: 65, d: w.d });
assert.strictEqual(codec.mergeWorkouts({ workouts: merged.state.workouts }, [w]), null, 'uploaded twice: nothing to do');

console.log('codec tests passed (' + res.bytes.length + ' byte plan)');
