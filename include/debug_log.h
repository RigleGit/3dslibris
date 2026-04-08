/*
 * debug_log.h — Structured debug logging (category + level).
 *
 * Define DSLIBRIS_DEBUG (e.g. via -DDSLIBRIS_DEBUG in CFLAGS) to enable logs.
 * In debug builds, logs can be filtered by level/category at compile time
 * and at runtime.
 *
 * Compile-time filters:
 *   -DDSLIBRIS_DEBUG_LOG_LEVEL=DBG_LEVEL_INFO
 *   -DDSLIBRIS_DEBUG_LOG_CATEGORIES=(DBG_CAT_TEXT|DBG_CAT_LAYOUT)
 *
 * Runtime filters (optional):
 *   debug_log::SetLevel(...)
 *   debug_log::SetCategories(...)
 *
 * The first argument of log macros must be a pointer to any object that has
 * PrintStatus(const char*) (e.g. IStatusReporter*, App*).
 */

#pragma once

#include <cstdint>
#include <cstdio>

#include "shared/status_reporter.h"

#ifdef DSLIBRIS_DEBUG

enum DebugLogLevel {
  DBG_LEVEL_ERROR = 1,
  DBG_LEVEL_WARN = 2,
  DBG_LEVEL_INFO = 3,
  DBG_LEVEL_DEBUG = 4,
  DBG_LEVEL_TRACE = 5,
};

enum DebugLogCategory {
  DBG_CAT_GENERAL = 1u << 0,
  DBG_CAT_TEXT = 1u << 1,
  DBG_CAT_BIDI = 1u << 2,
  DBG_CAT_SHAPE = 1u << 3,
  DBG_CAT_WRAP = 1u << 4,
  DBG_CAT_LAYOUT = 1u << 5,
  DBG_CAT_CLIP = 1u << 6,
  DBG_CAT_RENDER = 1u << 7,
  DBG_CAT_EPUB = 1u << 8,
  DBG_CAT_PERF = 1u << 9,
  DBG_CAT_ALL = 0xFFFFFFFFu,
};

#ifndef DSLIBRIS_DEBUG_LOG_LEVEL
#define DSLIBRIS_DEBUG_LOG_LEVEL DBG_LEVEL_DEBUG
#endif

#ifndef DSLIBRIS_DEBUG_LOG_CATEGORIES
#define DSLIBRIS_DEBUG_LOG_CATEGORIES DBG_CAT_ALL
#endif

namespace debug_log {

void SetLevel(int level);
int GetLevel();

void SetCategories(uint32_t categories_mask);
uint32_t GetCategories();

bool ShouldLog(int level, uint32_t category);
void Log(IStatusReporter *reporter, int level, uint32_t category,
         const char *msg);
void Logf(IStatusReporter *reporter, int level, uint32_t category,
          const char *fmt, ...);

} // namespace debug_log

#define DBG_SHOULD_LOG(level, category)                                         \
  ((level) <= DSLIBRIS_DEBUG_LOG_LEVEL &&                                       \
   (((uint32_t)(category) & (uint32_t)DSLIBRIS_DEBUG_LOG_CATEGORIES) != 0u) && \
   debug_log::ShouldLog((level), (uint32_t)(category)))

#define DBG_LOG_CAT(app_ptr, category, msg)                                     \
  do {                                                                           \
    if ((app_ptr) != nullptr && DBG_SHOULD_LOG(DBG_LEVEL_INFO, (category)))     \
      debug_log::Log((app_ptr), DBG_LEVEL_INFO, (uint32_t)(category), (msg));   \
  } while (0)

#define DBG_LOGF_CAT(app_ptr, level, category, fmt, ...)                         \
  do {                                                                            \
    if ((app_ptr) != nullptr && DBG_SHOULD_LOG((level), (category)))             \
      debug_log::Logf((app_ptr), (level), (uint32_t)(category), (fmt),           \
                      ##__VA_ARGS__);                                             \
  } while (0)

// Backward-compatible defaults.
#define DBG_LOG(app_ptr, msg) DBG_LOG_CAT((app_ptr), DBG_CAT_GENERAL, (msg))
#define DBG_LOGF(app_ptr, fmt, ...)                                              \
  DBG_LOGF_CAT((app_ptr), DBG_LEVEL_INFO, DBG_CAT_GENERAL, (fmt),               \
               ##__VA_ARGS__)

#else /* !DSLIBRIS_DEBUG */

#define DBG_LEVEL_ERROR 1
#define DBG_LEVEL_WARN 2
#define DBG_LEVEL_INFO 3
#define DBG_LEVEL_DEBUG 4
#define DBG_LEVEL_TRACE 5

#define DBG_CAT_GENERAL 0u
#define DBG_CAT_TEXT 0u
#define DBG_CAT_BIDI 0u
#define DBG_CAT_SHAPE 0u
#define DBG_CAT_WRAP 0u
#define DBG_CAT_LAYOUT 0u
#define DBG_CAT_CLIP 0u
#define DBG_CAT_RENDER 0u
#define DBG_CAT_EPUB 0u
#define DBG_CAT_PERF 0u
#define DBG_CAT_ALL 0u

#define DBG_LOG(app_ptr, msg)        ((void)0)
#define DBG_LOGF(app_ptr, fmt, ...)  ((void)0)
#define DBG_LOG_CAT(app_ptr, category, msg) ((void)0)
#define DBG_LOGF_CAT(app_ptr, level, category, fmt, ...) ((void)0)
#define DBG_SHOULD_LOG(level, category) (0)

#endif /* DSLIBRIS_DEBUG */
