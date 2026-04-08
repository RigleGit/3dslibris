#include "debug_log.h"

#ifdef DSLIBRIS_DEBUG

#include <cstdarg>
#include <cstdio>

namespace debug_log {

namespace {

static int g_level = DSLIBRIS_DEBUG_LOG_LEVEL;
static uint32_t g_categories = (uint32_t)DSLIBRIS_DEBUG_LOG_CATEGORIES;

static const char *LevelTag(int level) {
  switch (level) {
  case DBG_LEVEL_ERROR:
    return "ERROR";
  case DBG_LEVEL_WARN:
    return "WARN";
  case DBG_LEVEL_INFO:
    return "INFO";
  case DBG_LEVEL_DEBUG:
    return "DEBUG";
  case DBG_LEVEL_TRACE:
    return "TRACE";
  default:
    return "LOG";
  }
}

static const char *CategoryTag(uint32_t category) {
  switch (category) {
  case DBG_CAT_GENERAL:
    return "GEN";
  case DBG_CAT_TEXT:
    return "TEXT";
  case DBG_CAT_BIDI:
    return "BIDI";
  case DBG_CAT_SHAPE:
    return "SHAPE";
  case DBG_CAT_WRAP:
    return "WRAP";
  case DBG_CAT_LAYOUT:
    return "LAYOUT";
  case DBG_CAT_CLIP:
    return "CLIP";
  case DBG_CAT_RENDER:
    return "RENDER";
  case DBG_CAT_EPUB:
    return "EPUB";
  case DBG_CAT_PERF:
    return "PERF";
  default:
    return "MISC";
  }
}

} // namespace

void SetLevel(int level) {
  if (level < DBG_LEVEL_ERROR)
    level = DBG_LEVEL_ERROR;
  if (level > DBG_LEVEL_TRACE)
    level = DBG_LEVEL_TRACE;
  g_level = level;
}

int GetLevel() { return g_level; }

void SetCategories(uint32_t categories_mask) { g_categories = categories_mask; }

uint32_t GetCategories() { return g_categories; }

bool ShouldLog(int level, uint32_t category) {
  if (level > g_level)
    return false;
  return (category & g_categories) != 0u;
}

void Log(IStatusReporter *reporter, int level, uint32_t category,
         const char *msg) {
  if (!reporter || !msg || !ShouldLog(level, category))
    return;
  char buf[320];
  std::snprintf(buf, sizeof(buf), "[%s][%s] %s", LevelTag(level),
                CategoryTag(category), msg);
  reporter->PrintStatus(buf);
}

void Logf(IStatusReporter *reporter, int level, uint32_t category,
          const char *fmt, ...) {
  if (!reporter || !fmt || !ShouldLog(level, category))
    return;

  char payload[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(payload, sizeof(payload), fmt, args);
  va_end(args);

  char msg[320];
  std::snprintf(msg, sizeof(msg), "[%s][%s] %s", LevelTag(level),
                CategoryTag(category), payload);
  reporter->PrintStatus(msg);
}

} // namespace debug_log

#endif // DSLIBRIS_DEBUG
