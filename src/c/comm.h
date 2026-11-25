#pragma once
#include <pebble.h>

// Message commands (mirrored in src/pkjs/index.js).
enum {
  CMD_HELLO = 1,       // phone -> watch: the phone side is ready, with the settings the watch
                       // keeps (Vibe, Size, Focus, colours)
  CMD_PLAN_BEGIN = 2,  // phone -> watch: Total = bytes
  CMD_PLAN_CHUNK = 3,  // phone -> watch: Seq = offset, Data = bytes
  CMD_PLAN_END = 4,    // phone -> watch: Total = bytes
  CMD_LOG_ACK = 5,     // phone -> watch: Seq = record id, stored on the phone
  CMD_STATUS = 6,      // phone -> watch: Text, Count = 0 working / 1 done / 2 error
  CMD_LOOK = 7,        // phone -> watch: Theme, Accent, OnAccent (also sent with HELLO)
  CMD_SYNC_REQ = 10,   // watch -> phone: Count = 1 when it wants the routines, Total = byte budget
  CMD_LOG_DATA = 11,   // watch -> phone: Seq = record id, Data = record
};

typedef void (*CommChangedHandler)(void);

void comm_init(CommChangedHandler on_change);
void comm_deinit(void);
void comm_sync(void);
bool comm_busy(void);
const char *comm_status(void);
