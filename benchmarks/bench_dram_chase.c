#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>

/*
 * Pure DRAM Pointer Chase Microbenchmark.
 * Default working set: 256 MiB (>= 4x 37.5 MiB L3 cache on Xeon w5-2565X).
 * Single dependent pointer dereference per iteration (no unrolling) so
 * out-of-order execution cannot hide DRAM latency via MLP.
 * Allocated and first-touched AFTER inheriting CPU affinity.
 */

#define DEFAULT_BUFFER_MB 256
#define DEFAULT_DURATION_SEC 3.0

static inline double get_wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void **create_pure_dram_chain(size_t buffer_bytes) {
    size_t cache_line_size = 64;
    size_t num_elements = buffer_bytes / cache_line_size;
    if (num_elements < 1024) num_elements = 1024;

    void **pool = (void **)aligned_alloc(64, num_elements * cache_line_size);
    size_t *indices = (size_t *)malloc(num_elements * sizeof(size_t));
    if (!pool || !indices) {
        free(pool);
        free(indices);
        return NULL;
    }

    for (size_t i = 0; i < num_elements; i++) {
        indices[i] = i;
    }

    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    for (size_t i = num_elements - 1; i > 0; i--) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        size_t j = (size_t)(rng % (i + 1));
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    /* Stride of 8 pointers (64 bytes) per cache line */
    size_t stride = cache_line_size / sizeof(void *);
    for (size_t i = 0; i < num_elements - 1; i++) {
        pool[indices[i] * stride] = (void *)&pool[indices[i + 1] * stride];
    }
    pool[indices[num_elements - 1] * stride] = (void *)&pool[indices[0] * stride];

    free(indices);
    return pool;
}

int main(int argc, char **argv) {
    size_t buffer_mb = DEFAULT_BUFFER_MB;
    double duration_sec = DEFAULT_DURATION_SEC;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--buffer-mb") == 0 && i + 1 < argc) {
            buffer_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_sec = atof(argv[++i]);
        }
    }

    int start_cpu = sched_getcpu();
    size_t bytes = buffer_mb * 1024ULL * 1024ULL;
    void **pool = create_pure_dram_chain(bytes);
    if (!pool) {
        fprintf(stderr, "Failed to allocate %zu MiB DRAM chain\n", buffer_mb);
        return 1;
    }

    printf("BENCH_DRAM_CHASE pid=%d cpu=%d buffer_mb=%zu duration=%.2f\n",
           getpid(), start_cpu, buffer_mb, duration_sec);
    fflush(stdout);

    double t0 = get_wall_time_sec();
    void **p = pool;
    uint64_t steps = 0;

    while (1) {
        for (int k = 0; k < 1024; k++) {
            p = (void **)*p;
        }
        steps += 1024;
        if ((get_wall_time_sec() - t0) >= duration_sec) {
            break;
        }
    }

    if (p == NULL) fprintf(stderr, "impossible\n");
    double elapsed = get_wall_time_sec() - t0;
    printf("DONE steps=%llu elapsed=%.4f\n", (unsigned long long)steps, elapsed);
    free(pool);
    return 0;
}
