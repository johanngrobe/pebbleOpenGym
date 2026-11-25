// The openGym scoped-token API (docs/API_TOKENS.md in the openGym repository):
//   GET /api/token/data  -> { state, rev, wid }
//   PUT /api/token/data  <- { state, baseRev, baseWid }   (409 with the current document on a race)
// The token needs Workouts at "Change directly" and Plans at "See".

function baseUrl(url) {
  var u = String(url || '').trim().replace(/\/+$/, '');
  if (u && !/^https?:\/\//i.test(u)) u = 'https://' + u;
  return u.replace(/\/api$/, '');
}

// Errors carry the English text as message, plus key/vars so the caller can translate it.
function fail(key, vars) {
  var e = new Error(key.replace(/\{(\w+)\}/g, function (m, k) { return vars[k]; }));
  e.key = key;
  e.vars = vars;
  return e;
}

function explain(status, body) {
  var code = body && body.code;
  if (status === 0) return 'Server unreachable';
  if (status === 401) return 'Token refused: check settings';
  if (status === 403 && code === 'scope') return 'Token needs Workouts: Change';
  if (status === 403) return 'Server refused the request';
  if (status === 404) return 'No openGym API at that address';
  if (status === 409 && code === 'not-synced') return 'Open openGym once, then sync';
  if (status === 429) return 'Too many requests, try later';
  return fail('Server error {n}', { n: status });
}

function asError(e) {
  return typeof e === 'string' ? fail(e) : e;
}

function request(cfg, method, path, body, cb) {
  var xhr = new XMLHttpRequest();
  var done = false;
  var finish = function (err, status, data) {
    if (done) return;
    done = true;
    cb(err, status, data);
  };
  try {
    xhr.open(method, baseUrl(cfg.url) + path, true);
  } catch (e) {
    finish(fail('Bad server address'), 0, null);
    return;
  }
  xhr.setRequestHeader('Authorization', 'Bearer ' + cfg.token);
  xhr.setRequestHeader('Accept', 'application/json');
  if (body) xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.timeout = 20000;
  xhr.onload = function () {
    var data = null;
    try { data = JSON.parse(xhr.responseText); } catch (e) { data = null; }
    if (xhr.status >= 200 && xhr.status < 300) finish(null, xhr.status, data);
    else finish(asError(explain(xhr.status, data)), xhr.status, data);
  };
  xhr.onerror = function () { finish(asError(explain(0)), 0, null); };
  xhr.ontimeout = function () { finish(fail('Server timed out'), 0, null); };
  xhr.send(body ? JSON.stringify(body) : null);
}

function getData(cfg, cb) {
  request(cfg, 'GET', '/api/token/data', null, function (err, status, data) {
    if (err) return cb(err);
    if (!data || !data.state) return cb(fail('Open openGym once, then sync'));
    cb(null, data);
  });
}

// Reads the document, lets `change(state)` return the keys to write (or null for nothing), and
// writes them over the revision it read, again on a 409 with the newer document.
function update(cfg, change, cb, attempt) {
  attempt = attempt || 0;
  getData(cfg, function (err, doc) {
    if (err) return cb(err);
    var apply = function (doc) {
      var patch = change(doc.state);
      if (!patch) return cb(null, { unchanged: true });
      request(cfg, 'PUT', '/api/token/data', { state: patch.state, baseRev: doc.rev, baseWid: doc.wid },
        function (err, status, data) {
          if (status === 409 && data && data.state && attempt < 3) {
            attempt++;
            return apply({ state: data.state, rev: data.rev, wid: data.wid });
          }
          if (err) return cb(err);
          cb(null, { patch: patch, rev: data && data.rev });
        });
    };
    apply(doc);
  });
}

module.exports = { baseUrl: baseUrl, getData: getData, update: update };
