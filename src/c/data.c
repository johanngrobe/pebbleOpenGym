#include "data.h"

// Persistent storage layout. Values are capped at PERSIST_DATA_MAX_LENGTH (256 bytes), so every
// blob is split over consecutive keys starting at its base, with its length under its own key.
#define K_SCHEMA      1
#define K_PLAN_LEN    2
#define K_LOG_LEN     3
#define K_ACTIVE_LEN  4
#define K_VIBE        5
#define K_NOTE        6  // the running workout's note, a string of its own
// 7, 8: the text size and the colours, kept by ui.c
#define K_FOCUS       9
#define K_HEART       10
#define K_PROGRESS    11
#define K_PLAN_BASE   100
#define K_LOG_BASE    200
#define K_ACTIVE_BASE 300
#define SCHEMA        1
#define CHUNK         PERSIST_DATA_MAX_LENGTH
#define PLAN_HARD_MAX 12288

// Older versions are still read: what was stored before an update keeps working.
#define PLAN_VERSION  3   // 2 added supersets, 3 per-side exercises
#define LOG_VERSION   2   // 2 added the side of each set, per-side entries and the note
#define ACTIVE_VERSION 4  // 2 added the entry flags, 3 the superset, 4 the hold start

Plan g_plan;
Session g_sess;
uint8_t g_vibe = VIBE_NORMAL;
bool g_focus_remind = true;
uint8_t g_progress_style = PROGRESS_PIE;
uint8_t g_heart_period = HEART_DEFAULT;

static uint8_t s_log[LOG_MAX];
static uint16_t s_log_len;

// ---------------------------------------------------------------------------------------------
// Blob storage

static int blob_len(uint32_t len_key) {
  return persist_exists(len_key) ? persist_read_int(len_key) : 0;
}

static bool blob_write(uint32_t len_key, uint32_t base, const uint8_t *data, size_t len) {
  int old = blob_len(len_key);
  int chunks = (len + CHUNK - 1) / CHUNK;
  int old_chunks = (old + CHUNK - 1) / CHUNK;
  bool ok = true;
  for (int i = 0; i < chunks; i++) {
    size_t n = len - i * CHUNK;
    if (n > CHUNK) n = CHUNK;
    if (persist_write_data(base + i, data + i * CHUNK, n) < 0) ok = false;
  }
  for (int i = chunks; i < old_chunks; i++) persist_delete(base + i);
  if (!ok) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "blob_write failed at key %d (%d bytes)", (int)base, (int)len);
    persist_write_int(len_key, 0);
    return false;
  }
  persist_write_int(len_key, len);
  return true;
}

static int blob_read(uint32_t len_key, uint32_t base, uint8_t *buf, size_t max) {
  int len = blob_len(len_key);
  if (len <= 0 || (size_t)len > max) return 0;
  int chunks = (len + CHUNK - 1) / CHUNK;
  for (int i = 0; i < chunks; i++) {
    size_t n = len - i * CHUNK;
    if (n > CHUNK) n = CHUNK;
    if (persist_read_data(base + i, buf + i * CHUNK, n) != (int)n) return 0;
  }
  return len;
}

static void blob_clear(uint32_t len_key, uint32_t base) {
  int chunks = (blob_len(len_key) + CHUNK - 1) / CHUNK;
  for (int i = 0; i < chunks; i++) persist_delete(base + i);
  persist_delete(len_key);
}

// ---------------------------------------------------------------------------------------------
// Little-endian reader / writer

typedef struct { const uint8_t *p; size_t len, pos; bool err; } Rd;

static uint8_t rd_u8(Rd *r) {
  if (r->pos + 1 > r->len) { r->err = true; return 0; }
  return r->p[r->pos++];
}
static uint16_t rd_u16(Rd *r) {
  uint16_t lo = rd_u8(r);
  return lo | ((uint16_t)rd_u8(r) << 8);
}
static uint32_t rd_u32(Rd *r) {
  uint32_t lo = rd_u16(r);
  return lo | ((uint32_t)rd_u16(r) << 16);
}
static void rd_str(Rd *r, char *out, size_t cap) {
  uint8_t n = rd_u8(r);
  if (r->pos + n > r->len) { r->err = true; out[0] = 0; return; }
  size_t k = n < cap - 1 ? n : cap - 1;
  memcpy(out, r->p + r->pos, k);
  out[k] = 0;
  r->pos += n;
}

typedef struct { uint8_t *p; size_t cap, pos; bool err; } Wr;

static void wr_u8(Wr *w, uint8_t v) {
  if (w->pos + 1 > w->cap) { w->err = true; return; }
  w->p[w->pos++] = v;
}
static void wr_u16(Wr *w, uint16_t v) { wr_u8(w, v & 0xFF); wr_u8(w, v >> 8); }
static void wr_u32(Wr *w, uint32_t v) { wr_u16(w, v & 0xFFFF); wr_u16(w, v >> 16); }
static void wr_str(Wr *w, const char *s) {
  size_t n = strlen(s);
  if (n > 255) n = 255;
  wr_u8(w, n);
  if (w->pos + n > w->cap) { w->err = true; return; }
  memcpy(w->p + w->pos, s, n);
  w->pos += n;
}

// ---------------------------------------------------------------------------------------------
// Plan

static bool plan_parse(const uint8_t *data, size_t len, Plan *out) {
  Rd r = { data, len, 0, false };
  memset(out, 0, sizeof(*out));
  uint8_t ver = rd_u8(&r);
  if (ver < 1 || ver > PLAN_VERSION) return false;
  out->unit = rd_u8(&r);
  out->rest_default = rd_u16(&r);
  out->step20 = rd_u16(&r);
  out->synced_at = rd_u32(&r);
  for (int i = 0; i < 7; i++) out->week[i] = rd_u8(&r);
  uint8_t n_over = rd_u8(&r);
  for (int i = 0; i < n_over; i++) {
    uint32_t ymd = rd_u32(&r);
    uint8_t idx = rd_u8(&r);
    if (out->n_over < MAX_OVERRIDES) {
      out->over[out->n_over].ymd = ymd;
      out->over[out->n_over].idx = idx;
      out->n_over++;
    }
  }
  uint8_t n_ex = rd_u8(&r);
  if (n_ex > MAX_EX) return false;
  for (int i = 0; i < n_ex; i++) {
    rd_str(&r, out->ex[i].id, ID_LEN);
    rd_str(&r, out->ex[i].name, EX_NAME_LEN);
    out->ex[i].flags = rd_u8(&r);
  }
  out->n_ex = n_ex;
  uint8_t n_r = rd_u8(&r);
  if (n_r > MAX_ROUTINES) return false;
  for (int i = 0; i < n_r; i++) {
    Routine *rt = &out->r[i];
    rd_str(&r, rt->id, ID_LEN);
    rd_str(&r, rt->name, R_NAME_LEN);
    uint8_t n = rd_u8(&r);
    if (n > MAX_ENTRIES) return false;
    for (int k = 0; k < n; k++) {
      PlanEntry *e = &rt->e[k];
      e->ex = rd_u8(&r);
      e->sets = rd_u8(&r);
      e->mode = rd_u8(&r);
      e->amount = rd_u16(&r);
      e->w20 = rd_u16(&r);
      e->rest = rd_u16(&r);
      e->sg = ver >= 2 ? rd_u8(&r) : 0;
      e->flags = ver >= 3 ? rd_u8(&r) : 0;
      e->warm = ver >= 3 ? rd_u8(&r) : 0;
      if (e->ex >= n_ex || e->mode > MODE_DIST) return false;
      if (e->sets > MAX_SETS) e->sets = MAX_SETS;
      if (e->sets == 0) e->sets = 1;
    }
    rt->n = n;
    if (ver >= 2) {
      uint8_t n_g = rd_u8(&r);
      for (int g = 0; g < n_g; g++) {
        Group tmp;
        rd_str(&r, tmp.name, GROUP_NAME_LEN);
        tmp.rest = rd_u16(&r);
        tmp.flags = rd_u8(&r);
        if (g < MAX_GROUPS) rt->g[g] = tmp;
      }
      rt->n_groups = n_g < MAX_GROUPS ? n_g : MAX_GROUPS;
    }
    // A group the routine does not have is no group.
    for (int k = 0; k < n; k++) if (rt->e[k].sg > rt->n_groups) rt->e[k].sg = 0;
  }
  out->n_routines = n_r;
  if (out->step20 == 0) out->step20 = out->unit ? 100 : 50;
  out->loaded = !r.err;
  return out->loaded;
}

bool plan_apply_blob(const uint8_t *data, size_t len) {
  Plan *tmp = malloc(sizeof(Plan));
  if (!tmp) return false;
  bool ok = plan_parse(data, len, tmp);
  if (ok) ok = blob_write(K_PLAN_LEN, K_PLAN_BASE, data, len);
  if (ok) memcpy(&g_plan, tmp, sizeof(Plan));
  free(tmp);
  return ok;
}

static void plan_load(void) {
  int len = blob_len(K_PLAN_LEN);
  if (len <= 0 || len > PLAN_HARD_MAX) return;
  uint8_t *buf = malloc(len);
  if (!buf) return;
  int got = blob_read(K_PLAN_LEN, K_PLAN_BASE, buf, len);
  if (got > 0) plan_parse(buf, got, &g_plan);
  free(buf);
}

int plan_budget(void) {
  int max = (int)persist_get_max_size();
  // Every stored value costs a little bookkeeping too; keep a margin.
  int budget = max - LOG_MAX - ACTIVE_MAX - 400;
  if (budget > PLAN_HARD_MAX) budget = PLAN_HARD_MAX;
  if (budget < 512) budget = 512;
  return budget;
}

int plan_today(void) {
  if (!g_plan.loaded) return ROUTINE_NONE;
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  uint32_t ymd = (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
  for (int i = 0; i < g_plan.n_over; i++) {
    if (g_plan.over[i].ymd == ymd) return g_plan.over[i].idx;
  }
  uint8_t idx = g_plan.week[t->tm_wday];
  if (idx != ROUTINE_REST && idx >= g_plan.n_routines) return ROUTINE_NONE;
  return idx;
}

const char *unit_label(void) {
  return g_plan.unit ? "lb" : "kg";
}

// ---------------------------------------------------------------------------------------------
// Running workout

static int sess_serialize(uint8_t *buf, size_t cap) {
  Wr w = { buf, cap, 0, false };
  wr_u8(&w, ACTIVE_VERSION);
  wr_u32(&w, g_sess.start);
  wr_u32(&w, g_sess.rest_end);
  wr_u16(&w, g_sess.rest_total);
  wr_u32(&w, g_sess.hold_start);
  wr_u8(&w, g_sess.routine);
  wr_u8(&w, g_sess.cur);
  wr_u8(&w, g_sess.n);
  for (int i = 0; i < g_sess.n; i++) {
    SessEntry *e = &g_sess.e[i];
    wr_u8(&w, e->ex);
    wr_u8(&w, e->mode);
    wr_u8(&w, e->flags);
    wr_u8(&w, e->sg);
    wr_u16(&w, e->rest);
    wr_u8(&w, e->n);
    for (int k = 0; k < e->n; k++) {
      wr_u16(&w, e->s[k].w20);
      wr_u16(&w, e->s[k].amt);
      wr_u8(&w, e->s[k].flags);
    }
  }
  return w.err ? -1 : (int)w.pos;
}

static void sess_load(void) {
  uint8_t buf[ACTIVE_MAX];
  int len = blob_read(K_ACTIVE_LEN, K_ACTIVE_BASE, buf, sizeof(buf));
  memset(&g_sess, 0, sizeof(g_sess));
  if (len <= 0 || !g_plan.loaded) return;
  Rd r = { buf, len, 0, false };
  uint8_t ver = rd_u8(&r);
  if (ver < 1 || ver > ACTIVE_VERSION) return;
  g_sess.start = rd_u32(&r);
  g_sess.rest_end = rd_u32(&r);
  g_sess.rest_total = rd_u16(&r);
  g_sess.hold_start = ver >= 4 ? rd_u32(&r) : 0;
  g_sess.routine = rd_u8(&r);
  g_sess.cur = rd_u8(&r);
  g_sess.n = rd_u8(&r);
  if (g_sess.n > MAX_ENTRIES || g_sess.routine >= g_plan.n_routines) { g_sess.n = 0; return; }
  for (int i = 0; i < g_sess.n; i++) {
    SessEntry *e = &g_sess.e[i];
    e->ex = rd_u8(&r);
    e->mode = rd_u8(&r);
    e->flags = ver >= 2 ? rd_u8(&r) : 0;
    // Before version 3 the routine (kept as it is while a workout runs) knows the superset.
    Routine *rt = &g_plan.r[g_sess.routine];
    e->sg = ver >= 3 ? rd_u8(&r) : (g_sess.routine < g_plan.n_routines && i < rt->n ? rt->e[i].sg : 0);
    e->rest = rd_u16(&r);
    e->n = rd_u8(&r);
    if (e->n > MAX_SETS || e->ex >= g_plan.n_ex) { r.err = true; break; }
    for (int k = 0; k < e->n; k++) {
      e->s[k].w20 = rd_u16(&r);
      e->s[k].amt = rd_u16(&r);
      e->s[k].flags = rd_u8(&r);
    }
  }
  if (r.err) { memset(&g_sess, 0, sizeof(g_sess)); return; }
  if (persist_exists(K_NOTE)) persist_read_string(K_NOTE, g_sess.note, NOTE_LEN);
  if (g_sess.cur >= g_sess.n) g_sess.cur = 0;
  g_sess.active = true;
}

void sess_save(void) {
  if (!g_sess.active) return;
  uint8_t buf[ACTIVE_MAX];
  int len = sess_serialize(buf, sizeof(buf));
  if (len > 0) blob_write(K_ACTIVE_LEN, K_ACTIVE_BASE, buf, len);
  if (g_sess.note[0]) persist_write_string(K_NOTE, g_sess.note);
  else persist_delete(K_NOTE);
}

void sess_start(uint8_t routine) {
  memset(&g_sess, 0, sizeof(g_sess));
  if (routine >= g_plan.n_routines) return;
  Routine *rt = &g_plan.r[routine];
  g_sess.active = true;
  g_sess.start = time(NULL);
  g_sess.routine = routine;
  g_sess.n = rt->n;
  for (int i = 0; i < rt->n; i++) {
    PlanEntry *p = &rt->e[i];
    SessEntry *e = &g_sess.e[i];
    e->ex = p->ex;
    e->mode = p->mode;
    e->flags = p->flags;
    e->sg = p->sg;
    e->rest = p->rest;
    int per = SESS_PAIRS(e) ? 2 : 1;  // rows per set
    int work = p->sets, warm = p->warm;
    if (work * per > MAX_SETS) work = MAX_SETS / per;
    if ((warm + work) * per > MAX_SETS) warm = MAX_SETS / per - work;
    e->n = (warm + work) * per;
    // Warm-ups ramp like openGym's (history.js insertWarmupRow): each one halfway from the last
    // up to the work weight, rounded down to the weight step, at the work set's reps.
    uint16_t from = 0;
    for (int k = 0; k < warm; k++) {
      uint16_t w = 0;
      if (p->w20 > from) {
        uint32_t mid = from + (p->w20 - from) / 2;
        uint16_t step = g_plan.step20 ? g_plan.step20 : 1;
        w = (mid / step) * step;
        if (w > p->w20) w = p->w20;
      } else {
        w = p->w20;
      }
      for (int j = 0; j < per; j++) {
        SetRec *s = &e->s[k * per + j];
        s->w20 = w;
        s->amt = p->amount;
        s->flags = SET_WARM;
      }
      from = w;
    }
    for (int k = warm * per; k < e->n; k++) {
      e->s[k].w20 = p->w20;
      e->s[k].amt = p->amount;
      e->s[k].flags = 0;
    }
  }
  sess_save();
}

void sess_discard(void) {
  memset(&g_sess, 0, sizeof(g_sess));
  blob_clear(K_ACTIVE_LEN, K_ACTIVE_BASE);
  persist_delete(K_NOTE);
}

void sess_add_note(const char *text) {
  if (!g_sess.active || !text || !text[0]) return;
  size_t have = strlen(g_sess.note);
  const char *sep = have ? " " : "";
  if (have + strlen(sep) + 1 >= NOTE_LEN) return;
  strcat(g_sess.note, sep);
  have = strlen(g_sess.note);
  size_t room = NOTE_LEN - 1 - have;
  size_t n = strlen(text);
  if (n > room) {
    n = room;
    while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--;  // never half a character
  }
  memcpy(g_sess.note + have, text, n);
  g_sess.note[have + n] = 0;
  sess_save();
}

int sess_cur_set(void) {
  if (!g_sess.active || g_sess.cur >= g_sess.n) return -1;
  SessEntry *e = &g_sess.e[g_sess.cur];
  for (int k = 0; k < e->n; k++) {
    if (!(e->s[k].flags & (SET_DONE | SET_SKIP))) return k;
  }
  return -1;
}

uint32_t sess_alarm_at(void) {
  if (!g_sess.active) return 0;
  if (g_sess.rest_end) return g_sess.rest_end;
  int si = sess_cur_set();
  if (!g_sess.hold_start || si < 0) return 0;
  uint16_t target = g_sess.e[g_sess.cur].s[si].amt;
  return target ? g_sess.hold_start + target : 0;  // a hold without a target just counts up
}

int sess_open_in(uint8_t entry) {
  if (entry >= g_sess.n) return 0;
  SessEntry *e = &g_sess.e[entry];
  int open = 0;
  for (int k = 0; k < e->n; k++) {
    if (!(e->s[k].flags & (SET_DONE | SET_SKIP))) open++;
  }
  return open;
}

int sess_warm_rows(const SessEntry *e) {
  int n = 0;
  while (n < e->n && (e->s[n].flags & SET_WARM)) n++;
  return n;
}

bool sess_add_set(uint8_t entry) {
  if (entry >= g_sess.n) return false;
  SessEntry *e = &g_sess.e[entry];
  int add = SESS_PAIRS(e) ? 2 : 1;
  if (e->n < add || e->n + add > MAX_SETS) return false;
  for (int k = 0; k < add; k++) {
    // A pair copies the last pair, so left stays on even rows.
    e->s[e->n] = e->s[e->n - add];
    e->s[e->n].flags = 0;  // a work set, also when the last row was a warm-up
    e->n++;
  }
  return true;
}

bool sess_advance(void) {
  for (int step = 1; step <= g_sess.n; step++) {
    int i = (g_sess.cur + step) % g_sess.n;
    if (sess_open_in(i) > 0) {
      g_sess.cur = i;
      return true;
    }
  }
  return sess_open_in(g_sess.cur) > 0;
}

int sess_count(uint8_t flag) {
  int c = 0;
  for (int i = 0; i < g_sess.n; i++) {
    for (int k = 0; k < g_sess.e[i].n; k++) {
      if (g_sess.e[i].s[k].flags & flag) c++;
    }
  }
  return c;
}

int sess_total(void) {
  int c = 0;
  for (int i = 0; i < g_sess.n; i++) c += g_sess.e[i].n;
  return c;
}

static int done_in(int entry) {
  int c = 0;
  for (int k = 0; k < g_sess.e[entry].n; k++) {
    if (g_sess.e[entry].s[k].flags & SET_DONE) c++;
  }
  return c;
}

// A finished workout as a self-contained record (exercise ids and names inside), so it can be
// uploaded correctly even after the routines changed.
static int sess_record(uint8_t *buf, size_t cap) {
  Wr w = { buf, cap, 0, false };
  Routine *rt = &g_plan.r[g_sess.routine];
  wr_u8(&w, LOG_VERSION);
  wr_u32(&w, g_sess.start);
  wr_u32(&w, (uint32_t)time(NULL));
  wr_str(&w, rt->id);
  wr_str(&w, rt->name);
  int n = 0;
  for (int i = 0; i < g_sess.n; i++) {
    if (done_in(i) > 0) n++;
  }
  wr_u8(&w, n);
  for (int i = 0; i < g_sess.n; i++) {
    SessEntry *e = &g_sess.e[i];
    int done = done_in(i);
    if (done == 0) continue;
    Exercise *ex = &g_plan.ex[e->ex];
    wr_str(&w, ex->id);
    wr_u8(&w, e->mode);
    // Bit 0 bodyweight (the exercise), bit 1 per side (this routine's entry).
    wr_u8(&w, (ex->flags & EXF_BODYWEIGHT) | ((e->flags & PEF_SIDE) ? 0x02 : 0));
    wr_u8(&w, done);
    for (int k = 0; k < e->n; k++) {
      if (!(e->s[k].flags & SET_DONE)) continue;
      wr_u16(&w, e->s[k].w20);
      wr_u16(&w, e->s[k].amt);
      // 1 left, 2 right; 4 a warm-up.
      wr_u8(&w, (SESS_PAIRS(e) ? (k % 2 ? 2 : 1) : 0) | (e->s[k].flags & SET_WARM ? 4 : 0));
    }
  }
  wr_str(&w, g_sess.note);
  return w.err ? -1 : (int)w.pos;
}

bool sess_finish(void) {
  if (!g_sess.active) return true;
  if (sess_count(SET_DONE) == 0) {
    sess_discard();
    return true;
  }
  uint8_t buf[LOG_MAX];
  int len = sess_record(buf, sizeof(buf));
  if (len <= 0 || s_log_len + 2 + len > LOG_MAX) return false;
  s_log[s_log_len] = len & 0xFF;
  s_log[s_log_len + 1] = len >> 8;
  memcpy(s_log + s_log_len + 2, buf, len);
  s_log_len += 2 + len;
  if (!blob_write(K_LOG_LEN, K_LOG_BASE, s_log, s_log_len)) {
    s_log_len -= 2 + len;
    return false;
  }
  sess_discard();
  return true;
}

// ---------------------------------------------------------------------------------------------
// Finished workouts

int log_count(void) {
  int c = 0;
  uint16_t pos = 0;
  while (pos + 2 <= s_log_len) {
    uint16_t len = s_log[pos] | (s_log[pos + 1] << 8);
    pos += 2 + len;
    c++;
  }
  return c;
}

static uint32_t record_id(const uint8_t *rec) {
  return rec[1] | (rec[2] << 8) | (rec[3] << 16) | ((uint32_t)rec[4] << 24);
}

bool log_peek(const uint8_t **data, uint16_t *len, uint32_t *id) {
  if (s_log_len < 2) return false;
  *len = s_log[0] | (s_log[1] << 8);
  if (*len < 5 || 2 + *len > s_log_len) return false;
  *data = s_log + 2;
  *id = record_id(*data);
  return true;
}

void log_remove(uint32_t id) {
  uint16_t pos = 0;
  while (pos + 2 <= s_log_len) {
    uint16_t len = s_log[pos] | (s_log[pos + 1] << 8);
    if (len >= 5 && record_id(s_log + pos + 2) == id) {
      uint16_t end = pos + 2 + len;
      memmove(s_log + pos, s_log + end, s_log_len - end);
      s_log_len -= 2 + len;
      if (s_log_len) blob_write(K_LOG_LEN, K_LOG_BASE, s_log, s_log_len);
      else blob_clear(K_LOG_LEN, K_LOG_BASE);
      return;
    }
    pos += 2 + len;
  }
}

// ---------------------------------------------------------------------------------------------

void fmt_weight(char *buf, size_t size, uint16_t w20) {
  int whole = w20 / 20;
  int hundredths = (w20 % 20) * 5;
  if (hundredths == 0) snprintf(buf, size, "%d", whole);
  else if (hundredths % 10 == 0) snprintf(buf, size, "%d.%d", whole, hundredths / 10);
  else snprintf(buf, size, "%d.%02d", whole, hundredths);
}

void fmt_duration(char *buf, size_t size, int secs) {
  if (secs < 0) secs = 0;
  // Hours and minutes ("0:45", "1:23"); the seconds are not needed for a workout.
  snprintf(buf, size, "%d:%02d", secs / 3600, (secs / 60) % 60);
}

void vibe_set(uint8_t v) {
  if (v > VIBE_STRONG) return;
  g_vibe = v;
  persist_write_int(K_VIBE, v);
}

void focus_remind_set(bool on) {
  if (on == g_focus_remind && persist_exists(K_FOCUS)) return;
  g_focus_remind = on;
  persist_write_bool(K_FOCUS, on);
}

void progress_style_set(uint8_t v) {
  if (v > PROGRESS_NUMBERS) return;
  if (v == g_progress_style && persist_exists(K_PROGRESS)) return;
  g_progress_style = v;
  persist_write_int(K_PROGRESS, v);
}

void heart_set(uint8_t secs) {
  if (secs > HEART_MAX) secs = HEART_MAX;
  if (secs == g_heart_period && persist_exists(K_HEART)) return;
  g_heart_period = secs;
  persist_write_int(K_HEART, secs);
}

void vibe_timer_done(void) {
  static const uint32_t normal[] = { 300, 150, 300 };
  static const uint32_t strong[] = { 600, 200, 600, 200, 600 };
  switch (g_vibe) {
    case VIBE_SHORT: vibes_short_pulse(); break;
    case VIBE_NORMAL: vibes_enqueue_custom_pattern((VibePattern) { .durations = normal, .num_segments = 3 }); break;
    case VIBE_STRONG: vibes_enqueue_custom_pattern((VibePattern) { .durations = strong, .num_segments = 5 }); break;
    default: break;
  }
}

void vibe_warning(void) {
  if (g_vibe != VIBE_OFF) vibes_short_pulse();
}

void data_init(void) {
  if (!persist_exists(K_SCHEMA) || persist_read_int(K_SCHEMA) != SCHEMA) {
    blob_clear(K_PLAN_LEN, K_PLAN_BASE);
    blob_clear(K_LOG_LEN, K_LOG_BASE);
    blob_clear(K_ACTIVE_LEN, K_ACTIVE_BASE);
    persist_write_int(K_SCHEMA, SCHEMA);
  }
  if (persist_exists(K_VIBE)) g_vibe = persist_read_int(K_VIBE);
  if (persist_exists(K_FOCUS)) g_focus_remind = persist_read_bool(K_FOCUS);
  if (persist_exists(K_HEART)) heart_set(persist_read_int(K_HEART));
  if (persist_exists(K_PROGRESS)) progress_style_set(persist_read_int(K_PROGRESS));
  memset(&g_plan, 0, sizeof(g_plan));
  plan_load();
  s_log_len = blob_read(K_LOG_LEN, K_LOG_BASE, s_log, sizeof(s_log));
  sess_load();
}
