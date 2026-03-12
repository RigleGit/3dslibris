/*
 * debug_log.h — Compile-time debug logging control.
 *
 * Define DSLIBRIS_DEBUG (e.g. via -DDSLIBRIS_DEBUG in CFLAGS) to enable
 * verbose diagnostic logging (timing, format parse flow, state changes).
 * When disabled, DBG_LOG calls compile to nothing with zero runtime cost.
 *
 * Usage:
 *   DBG_LOG(app, "EPUB: parse begin");
 *   DBG_LOGF(app, "TIMING: scan=%llums count=%u", ms, n);
 *
 * The `app` argument must be a pointer (or reference) to App that has
 * a PrintStatus(const char*) method.
 */

#pragma once

#ifdef DSLIBRIS_DEBUG

// Log a literal string.
#define DBG_LOG(app_ptr, msg) \
  do { (app_ptr)->PrintStatus(msg); } while (0)

// Log a formatted string (printf-style). Uses a stack buffer to avoid
// heap allocation on the 3DS's limited memory.
#define DBG_LOGF(app_ptr, fmt, ...) \
  do { \
    char _dbg_buf[256]; \
    snprintf(_dbg_buf, sizeof(_dbg_buf), fmt, __VA_ARGS__); \
    (app_ptr)->PrintStatus(_dbg_buf); \
  } while (0)

#else /* !DSLIBRIS_DEBUG */

#define DBG_LOG(app_ptr, msg)        ((void)0)
#define DBG_LOGF(app_ptr, fmt, ...)  ((void)0)

#endif /* DSLIBRIS_DEBUG */
