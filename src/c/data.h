#pragma once
#include <pebble.h>

// Limits. The phone trims what it sends to these (src/pkjs/codec.js mirrors them).
#define MAX_EX        64
#define MAX_ROUTINES  16
#define MAX_ENTRIES   16
#define MAX_SETS      12
#define MAX_OVERRIDES 14
#define ID_LEN        24
#define EX_NAME_LEN   40
#define R_NAME_LEN    32
#define MAX_GROUPS    8   // supersets per routine
#define GROUP_NAME_LEN 24

// Persistent storage budget for finished workouts waiting to be uploaded, and the running one.
#define LOG_MAX     1024
#define NOTE_LEN    160  // the workout's dictated note, bytes with the 0
#define ACTIVE_MAX  640

enum { MODE_REPS = 0, MODE_TIME = 1, MODE_CARDIO = 2, MODE_DIST = 3 };

#define EXF_BODYWEIGHT 0x01

#define SET_DONE 0x01
#define SET_SKIP 0x02
#define SET_WARM 0x04  // a warm-up row; they come first in an entry

#define ROUTINE_NONE 0xFF
#define ROUTINE_REST 0xFE

typedef struct {
  char id[ID_LEN];
  char name[EX_NAME_LEN];
  uint8_t flags;
} Exercise;

typedef struct {
  uint8_t ex;       // index into Plan.ex
  uint8_t sets;
  uint8_t mode;
  uint16_t amount;  // reps, seconds, minutes or metres, by mode
  uint16_t w20;     // weight in 1/20 of the profile's unit
  uint16_t rest;    // seconds, 0 = the profile's default
  uint8_t sg;       // superset: 1-based index into Routine.g, 0 = a plain exercise
  uint8_t flags;    // PEF_*
  uint8_t warm;     // warm-up sets before the work sets (openGym's warmupSets)
} PlanEntry;

// Single-sided (openGym's `side`). Reps are the total of both sides, shown per side; a timed
// hold is done once per side, so each set is a left and a right row (even index = left).
#define PEF_SIDE 0x01

#define GRPF_CIRCUIT 0x01

typedef struct {
  char name[GROUP_NAME_LEN];  // "" = no name of its own
  uint16_t rest;              // seconds between rounds, 0 = not set
  uint8_t flags;
} Group;

typedef struct {
  char id[ID_LEN];
  char name[R_NAME_LEN];
  uint8_t n;
  PlanEntry e[MAX_ENTRIES];
  uint8_t n_groups;
  Group g[MAX_GROUPS];
} Routine;

typedef struct {
  bool loaded;
  uint8_t unit;           // 0 kg, 1 lb
  uint16_t rest_default;  // seconds
  uint16_t step20;        // weight step, 1/20 unit
  uint32_t synced_at;     // unix seconds
  uint8_t week[7];        // weekday (0 = Sunday) -> routine index
  uint8_t n_over;
  struct { uint32_t ymd; uint8_t idx; } over[MAX_OVERRIDES];
  uint8_t n_ex;
  uint8_t n_routines;
  Exercise ex[MAX_EX];
  Routine r[MAX_ROUTINES];
} Plan;

typedef struct {
  uint16_t w20;
  uint16_t amt;
  uint8_t flags;
} SetRec;

typedef struct {
  uint8_t ex;
  uint8_t mode;
  uint8_t flags;    // PEF_*, from the plan entry
  uint8_t sg;       // superset, as PlanEntry.sg: its exercises take turns
  uint8_t n;
  uint16_t rest;
  SetRec s[MAX_SETS];
} SessEntry;

// Per side and timed: its rows come in left/right pairs.
#define SESS_PAIRS(e) (((e)->flags & PEF_SIDE) && (e)->mode == MODE_TIME)

typedef struct {
  bool active;
  uint32_t start;
  uint32_t rest_end;   // unix seconds, 0 = no rest running
  uint16_t rest_total; // seconds the running rest started with
  uint8_t routine;
  uint8_t cur;
  uint8_t n;
  SessEntry e[MAX_ENTRIES];
  char note[NOTE_LEN];  // openGym's workout note; "" = none
} Session;

extern Plan g_plan;
extern Session g_sess;

void data_init(void);

// Plan (routines from the last sync)
bool plan_apply_blob(const uint8_t *data, size_t len);
int plan_budget(void);
int plan_today(void);  // routine index, ROUTINE_REST or ROUTINE_NONE
const char *unit_label(void);

// Running workout
void sess_start(uint8_t routine);
void sess_save(void);
void sess_discard(void);
bool sess_finish(void);  // false when there is no room to keep it
int sess_cur_set(void);  // first open set of the current entry, -1 when none
bool sess_advance(void); // move to the next entry with open sets, false when all are done
int sess_count(uint8_t flag);
int sess_total(void);
int sess_open_in(uint8_t entry);
bool sess_add_set(uint8_t entry);
int sess_warm_rows(const SessEntry *e);
void sess_add_note(const char *text);  // adds a sentence to the note (cut to fit)  // the warm-up rows at the start of an entry  // one more set (a left/right pair for a timed per-side hold)

// Finished workouts waiting for the phone
int log_count(void);
bool log_peek(const uint8_t **data, uint16_t *len, uint32_t *id);
void log_remove(uint32_t id);

// How the watch vibrates when a rest or a hold runs out (Settings on the phone).
enum { VIBE_OFF = 0, VIBE_SHORT = 1, VIBE_NORMAL = 2, VIBE_STRONG = 3 };
extern uint8_t g_vibe;
void vibe_set(uint8_t v);
void vibe_timer_done(void);  // the alarm itself, as set
void vibe_warning(void);     // the nudge 10 s before a rest ends (none when off)

void fmt_weight(char *buf, size_t size, uint16_t w20);
void fmt_duration(char *buf, size_t size, int secs);
