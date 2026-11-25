#include <pebble.h>
#include "comm.h"
#include "data.h"
#include "home.h"
#include "i18n.h"
#include "workout.h"

static void comm_changed(void) {
  home_refresh();
}

static void init(void) {
  i18n_init();
  data_init();
  comm_init(comm_changed);
#ifdef _PBL_API_EXISTS_app_touch_navigation_enable
  app_touch_navigation_enable(true);
#endif
  home_push();
  // A workout left running (app closed, battery swap, crash) opens straight away.
  if (g_sess.active) workout_open();
}

static void deinit(void) {
  sess_save();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
