// Host-test stub of Logging.h — printf-backed LOG_* macros.
#pragma once

#include <cstdio>

#define LOG_INF(mod, fmt, ...) ::printf("[%s] " fmt "\n", mod, ##__VA_ARGS__)
#define LOG_DBG(mod, fmt, ...) ::printf("[%s] " fmt "\n", mod, ##__VA_ARGS__)
#define LOG_ERR(mod, fmt, ...) ::printf("[%s] " fmt "\n", mod, ##__VA_ARGS__)