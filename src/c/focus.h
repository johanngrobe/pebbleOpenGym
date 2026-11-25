#pragma once
#include <pebble.h>

// The Focus reminder: shown over a workout that has just started, when it is switched on in the
// settings. Pebble apps can't turn on Do Not Disturb themselves, so this asks the wearer to.
void focus_remind_open(void);
