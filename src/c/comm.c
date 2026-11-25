#include "comm.h"
#include "data.h"
#include "i18n.h"

#define TIMEOUT_MS 45000
#define RETRY_MS   600
#define MAX_RETRIES 4

static CommChangedHandler s_on_change;
static char s_status[64];
static bool s_js_ready;
static bool s_pending;  // a sync was asked for before the phone side was ready
static bool s_busy;
static AppTimer *s_timeout;
static AppTimer *s_retry;
static int s_retries;

static uint8_t *s_plan_buf;
static uint32_t s_plan_len;
static uint32_t s_plan_got;

static void changed(void) {
  if (s_on_change) s_on_change();
}

static void set_status(const char *text) {
  strncpy(s_status, text, sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = 0;
  changed();
}

static void plan_buf_free(void) {
  if (s_plan_buf) free(s_plan_buf);
  s_plan_buf = NULL;
  s_plan_len = s_plan_got = 0;
}

static void finish(const char *status) {
  s_busy = false;
  if (s_timeout) { app_timer_cancel(s_timeout); s_timeout = NULL; }
  if (s_retry) { app_timer_cancel(s_retry); s_retry = NULL; }
  plan_buf_free();
  if (status) set_status(status);
  else changed();
}

static void timeout_fired(void *ctx) {
  s_timeout = NULL;
  finish(L("Sync timed out", "Zeitüberschreitung"));
}

static void touch_timeout(void) {
  if (s_timeout) app_timer_reschedule(s_timeout, TIMEOUT_MS);
  else s_timeout = app_timer_register(TIMEOUT_MS, timeout_fired, NULL);
}

static void send_next(void);

static void retry_fired(void *ctx) {
  s_retry = NULL;
  send_next();
}

static void retry_later(void) {
  if (++s_retries > MAX_RETRIES) {
    finish(L("Phone unreachable", "Handy nicht erreichbar"));
    return;
  }
  if (!s_retry) s_retry = app_timer_register(RETRY_MS * s_retries, retry_fired, NULL);
}

// Sends the oldest finished workout, or, when none is left, asks for the routines.
static void send_next(void) {
  if (!s_busy) return;
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) {
    retry_later();
    return;
  }
  const uint8_t *data;
  uint16_t len;
  uint32_t id;
  if (log_peek(&data, &len, &id)) {
    dict_write_uint8(it, MESSAGE_KEY_Cmd, CMD_LOG_DATA);
    dict_write_int32(it, MESSAGE_KEY_Seq, (int32_t)id);
    dict_write_data(it, MESSAGE_KEY_Data, data, len);
    set_status(L("Sending workouts...", "Sende Trainings..."));
  } else {
    dict_write_uint8(it, MESSAGE_KEY_Cmd, CMD_SYNC_REQ);
    // While a workout runs its routines must stay as they are: upload only.
    dict_write_uint8(it, MESSAGE_KEY_Count, g_sess.active ? 0 : 1);
    dict_write_int32(it, MESSAGE_KEY_Total, plan_budget());
    // The phone answers in the watch's language.
    dict_write_cstring(it, MESSAGE_KEY_Text, i18n_lang());
    set_status(L("Syncing...", "Sync läuft..."));
  }
  if (app_message_outbox_send() != APP_MSG_OK) retry_later();
  touch_timeout();
}

void comm_sync(void) {
  if (s_busy) return;
  if (!connection_service_peek_pebble_app_connection()) {
    set_status(L("Phone not connected", "Handy nicht verbunden"));
    return;
  }
  if (!s_js_ready) {
    s_pending = true;
    set_status(L("Waiting for phone...", "Warte auf Handy..."));
    return;
  }
  s_busy = true;
  s_retries = 0;
  send_next();
}

bool comm_busy(void) {
  return s_busy;
}

const char *comm_status(void) {
  return s_status;
}

static void on_plan_end(void) {
  if (!s_plan_buf || s_plan_got != s_plan_len) {
    finish(L("Sync failed: incomplete data", "Fehler: Daten unvollständig"));
    return;
  }
  if (g_sess.active) {
    finish(L("Workout running: routines kept", "Training läuft: Routinen behalten"));
    return;
  }
  if (plan_apply_blob(s_plan_buf, s_plan_len)) {
    char text[32];
    time_t now = time(NULL);
    strftime(text, sizeof(text), clock_is_24h_style() ? L("Synced %H:%M", "Synchronisiert %H:%M") : L("Synced %I:%M %p", "Synchronisiert %I:%M %p"), localtime(&now));
    finish(text);
  } else {
    finish(L("Could not store routines", "Routinen nicht gespeichert"));
  }
}

static void inbox_received(DictionaryIterator *it, void *ctx) {
  Tuple *cmd = dict_find(it, MESSAGE_KEY_Cmd);
  if (!cmd) return;
  Tuple *seq = dict_find(it, MESSAGE_KEY_Seq);
  Tuple *total = dict_find(it, MESSAGE_KEY_Total);
  Tuple *data = dict_find(it, MESSAGE_KEY_Data);
  if (s_busy) touch_timeout();

  switch (cmd->value->uint8) {
    case CMD_HELLO: {
      Tuple *vibe = dict_find(it, MESSAGE_KEY_Vibe);
      if (vibe) vibe_set(vibe->value->uint8);
      s_js_ready = true;
      s_pending = false;
      comm_sync();  // every time the phone side starts: upload what is waiting, refresh routines
      break;
    }

    case CMD_LOG_ACK:
      if (seq) log_remove((uint32_t)seq->value->int32);
      s_retries = 0;
      changed();
      send_next();
      break;

    case CMD_PLAN_BEGIN:
      plan_buf_free();
      if (total && total->value->int32 > 0 && total->value->int32 <= 16384) {
        s_plan_len = total->value->int32;
        s_plan_buf = malloc(s_plan_len);
        if (!s_plan_buf) finish(L("Not enough memory", "Nicht genug Speicher"));
        else set_status(L("Loading routines...", "Lade Routinen..."));
      }
      break;

    case CMD_PLAN_CHUNK:
      if (s_plan_buf && seq && data) {
        uint32_t off = (uint32_t)seq->value->int32;
        if (off + data->length <= s_plan_len) {
          memcpy(s_plan_buf + off, data->value->data, data->length);
          if (off + data->length > s_plan_got) s_plan_got = off + data->length;
        }
      }
      break;

    case CMD_PLAN_END:
      on_plan_end();
      break;

    case CMD_STATUS: {
      Tuple *text = dict_find(it, MESSAGE_KEY_Text);
      Tuple *count = dict_find(it, MESSAGE_KEY_Count);
      int state = count ? count->value->uint8 : 0;
      if (state == 0) {
        if (text) set_status(text->value->cstring);
      } else {
        finish(text ? text->value->cstring : NULL);
      }
      break;
    }
  }
}

static void outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "outbox failed: %d", (int)reason);
  if (s_busy) retry_later();
}

static void connection_changed(bool connected) {
  if (!connected) {
    s_js_ready = false;
    if (s_busy) finish(L("Phone disconnected", "Handy getrennt"));
  }
  // When the phone comes back the phone side starts again and says hello.
}

void comm_init(CommChangedHandler on_change) {
  s_on_change = on_change;
  if (!g_plan.loaded) set_status(L("Not synced yet", "Noch nicht synchronisiert"));
  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_failed(outbox_failed);
  uint32_t in = app_message_inbox_size_maximum();
  if (in > 2048) in = 2048;
  uint32_t out = app_message_outbox_size_maximum();
  if (out > LOG_MAX + 64) out = LOG_MAX + 64;
  app_message_open(in, out);
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_changed,
  });
}

void comm_deinit(void) {
  connection_service_unsubscribe();
  plan_buf_free();
}
