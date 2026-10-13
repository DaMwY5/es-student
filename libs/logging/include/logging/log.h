#pragma once

#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_INF 2
#define LOG_LEVEL_DBG 3

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INF
#endif

void log_write(const char *level, const char *function, int line,
               const char *format, ...);

#define LOG_ERR(...) do { if (LOG_LEVEL >= LOG_LEVEL_ERR) \
    log_write("err", __func__, __LINE__, __VA_ARGS__); } while (0)
#define LOG_INF(...) do { if (LOG_LEVEL >= LOG_LEVEL_INF) \
    log_write("inf", __func__, __LINE__, __VA_ARGS__); } while (0)
#define LOG_DBG(...) do { if (LOG_LEVEL >= LOG_LEVEL_DBG) \
    log_write("dbg", __func__, __LINE__, __VA_ARGS__); } while (0)
