#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_CPUS 256
#define MAX_THREADS 1024
#define MAX_PATH_LEN 512
#define MAX_LINE_LEN 1024

/* Core Classification */
typedef enum {
    CORE_TYPE_UNKNOWN = 0,
    CORE_TYPE_P       = 1,   /* Performance Core */
    CORE_TYPE_E       = 2    /* Efficient Core */
} core_type_t;

static inline const char *core_type_to_str(core_type_t type) {
    switch (type) {
        case CORE_TYPE_P: return "P-Core";
        case CORE_TYPE_E: return "E-Core";
        default:          return "Unknown";
    }
}

/* Timestamp helpers */
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t get_time_us(void) {
    return get_time_ns() / 1000ULL;
}

static inline double get_time_ms(void) {
    return (double)get_time_ns() / 1000000.0;
}

static inline double get_time_sec(void) {
    return (double)get_time_ns() / 1000000000.0;
}

/* Logging Macros */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_CYAN    "\033[1;36m"

#define LOG_INFO(fmt, ...) \
    fprintf(stdout, COLOR_GREEN "[INFO] [%.3f s] " COLOR_RESET fmt "\n", get_time_sec(), ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    fprintf(stderr, COLOR_YELLOW "[WARN] [%.3f s] " COLOR_RESET fmt "\n", get_time_sec(), ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    fprintf(stderr, COLOR_RED "[ERROR] [%.3f s] (%s:%d) " COLOR_RESET fmt "\n", get_time_sec(), __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (getenv("DEBUG") != NULL) { \
            fprintf(stdout, COLOR_CYAN "[DEBUG] [%.3f s] " COLOR_RESET fmt "\n", get_time_sec(), ##__VA_ARGS__); \
        } \
    } while (0)

#endif /* COMMON_H */
