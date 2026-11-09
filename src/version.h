// The one place the firmware version lives. Shown on the status page, in
// /api/data and /api/diag, and as the Apple Home accessory's firmware revision.
#pragma once

#ifndef HTRAM_FW_VERSION  // test builds pass their own with -D
#define HTRAM_FW_VERSION "1.4.2"
#endif
