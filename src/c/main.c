#include <pebble.h>
#include "comm.h"
#include "data.h"
#include "home.h"
#include "i18n.h"
#include "ui.h"
#include "workout.h"

static void comm_changed(void) {
  home_refresh();
}

// Pebble stops the app when it is left, so a rest or timed hold running out then can't buzz.
// A wakeup set on the way out starts the app again at that moment.
static void alarm_schedule(void) {
  if (g_vibe == VIBE_OFF) return;
  time_t at = sess_alarm_at();
  if (at <= time(NULL)) return;
  StatusCode r = wakeup_schedule(at, 0, false);
  if (r < 0) APP_LOG(APP_LOG_LEVEL_WARNING, "wakeup_schedule: %d", (int)r);
}

static void init(void) {
  bool woke = launch_reason() == APP_LAUNCH_WAKEUP;
  wakeup_cancel_all();  // running now, so the app buzzes on its own
  i18n_init();
  data_init();
  ui_init();
  comm_init(comm_changed);
#ifdef _PBL_API_EXISTS_app_touch_navigation_enable
  app_touch_navigation_enable(true);
#endif
  home_push();
  // A workout left running (app closed, battery swap, crash) opens straight away.
  if (g_sess.active) {
    // A rest that ran out while closed is just cleared when the workout opens, so the wakeup
    // that brought the app back buzzes here. A timed hold that ran out completes with its buzz.
    bool rest_over = g_sess.rest_end && (time_t)g_sess.rest_end <= time(NULL);
    workout_open();
    if (woke && rest_over) {
      vibe_timer_done();
      light_enable_interaction();
    }
  }
}

static void deinit(void) {
  sess_save();
  alarm_schedule();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
