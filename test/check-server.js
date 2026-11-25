// Read-only check of a real openGym server: is the token right, and what would the watch get?
// Writes nothing.
//
//   OPENGYM_URL=https://gym.example.com OPENGYM_TOKEN=ogt_... node test/check-server.js
var codec = require('../src/pkjs/codec');
var names = require('../src/pkjs/exercise_names');

var base = String(process.env.OPENGYM_URL || '').trim().replace(/\/+$/, '').replace(/\/api$/, '');
var token = String(process.env.OPENGYM_TOKEN || '').trim();
if (!base || !token) {
  console.error('Set OPENGYM_URL and OPENGYM_TOKEN.');
  process.exit(2);
}
if (!/^https?:\/\//i.test(base)) base = 'https://' + base;

function get(path) {
  return fetch(base + path, { headers: { Authorization: 'Bearer ' + token, Accept: 'application/json' } })
    .then(function (res) {
      return res.text().then(function (text) {
        var body = null;
        try { body = JSON.parse(text); } catch (e) { /* not JSON */ }
        return { status: res.status, body: body, text: text };
      });
    });
}

function fail(msg) {
  console.error('\n✗ ' + msg);
  process.exit(1);
}

(async function () {
  console.log('Server: ' + base);
  var me;
  try {
    me = await get('/api/token/me');
  } catch (e) {
    fail('Cannot reach the server: ' + (e.cause && e.cause.code || e.message));
  }
  if (me.status === 404) fail('No openGym token API at this address (404). Wrong URL, or API_TOKENS=off on the server.');
  if (me.status === 401) fail('Token refused (401): wrong, expired or revoked.');
  if (me.status !== 200 || !me.body) fail('Unexpected answer ' + me.status + ': ' + me.text.slice(0, 200));

  var scopes = me.body.token && me.body.token.scopes || {};
  console.log('Profile: ' + (me.body.user && me.body.user.name) + '   token: ' + (me.body.token && me.body.token.name));
  console.log('Scopes:  workouts=' + scopes.workouts + '  plans=' + scopes.plans);
  var problems = [];
  if (scopes.workouts !== 'apply') problems.push('Workouts must be "Change directly", or finished workouts cannot be saved.');
  if (scopes.plans !== 'view' && scopes.plans !== 'apply') problems.push('Plans & routines must be at least "See", or no routines reach the watch.');

  var data = await get('/api/token/data');
  if (data.status !== 200 || !data.body) fail('Reading the profile failed: ' + data.status + ' ' + data.text.slice(0, 200));
  var state = data.body.state;
  if (!state) fail('The profile has never synced: open openGym once while signed in.');

  var plan = codec.encodePlan(state, { names: names, lang: state.lang, budget: 2300, now: new Date() });
  console.log('Units:   ' + (state.unit || 'kg') + '   language: ' + (state.lang || 'en') +
    '   history: ' + (state.workouts || []).length + ' workouts   revision: ' + data.body.rev);
  console.log('\nThe watch would get ' + plan.routines + ' routine(s) in ' + plan.bytes.length +
    ' bytes (budget at 4 KB storage: 2300)' + (plan.dropped ? ', ' + plan.dropped + ' left out for space' : '') + ':');

  // Show it as the watch will, by reading the bytes back.
  var b = plan.bytes, p = 0;
  var u8 = function () { return b[p++]; };
  var u16 = function () { var v = b[p] | (b[p + 1] << 8); p += 2; return v; };
  var u32 = function () { return u16() + u16() * 65536; };
  var str = function () { var n = u8(); var s = Buffer.from(b.slice(p, p + n)).toString('utf8'); p += n; return s; };
  u8(); var unit = u8() ? 'lb' : 'kg'; var rest = u16(); u16(); u32();
  var week = []; for (var i = 0; i < 7; i++) week.push(u8());
  for (var n = u8(), i = 0; i < n; i++) { u32(); u8(); }
  var ex = []; for (n = u8(), i = 0; i < n; i++) ex.push({ id: str(), name: str(), bw: u8() & 1 });
  var days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  for (n = u8(), i = 0; i < n; i++) {
    var rid = str(), rname = str();
    var planned = days.filter(function (d, k) { return week[k] === i; });
    console.log('\n  ' + rname + (planned.length ? '   (' + planned.join(', ') + ')' : ''));
    for (var m = u8(), k = 0; k < m; k++) {
      var e = { ex: u8(), sets: u8(), mode: u8(), amount: u16(), w20: u16(), rest: u16() };
      var x = ex[e.ex];
      var what = ['reps', 's hold', 'min', 'm'][e.mode];
      var w = e.mode === 2 ? '' : e.w20 ? '  @ ' + e.w20 / 20 + ' ' + unit : x.bw ? '  (BW)' : '';
      var unknown = /^Exercise /.test(x.name) ? '   ⚠ no name known for id ' + x.id : '';
      console.log('    ' + e.sets + ' x ' + e.amount + ' ' + what + w + '   ' + x.name + unknown);
    }
  }
  console.log('\nDefault rest: ' + rest + ' s');
  if (problems.length) {
    problems.forEach(function (m) { console.error('\n✗ ' + m); });
    process.exit(1);
  }
  console.log('\n✓ Token and data look good. Nothing was written.');
})();
