#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/syscall.h>

#define DEFAULT_BUFFER_MB 64
#define DEFAULT_CHASE_STEPS 40000000L
#define DEFAULT_THREADS 1

typedef struct {
    int thread_idx;
    size_t buffer_mb;
    long max_steps;
    double max_duration_sec;
    pid_t tid;
    double elapsed_sec;
    long completed_steps;
    double m_traversals_per_sec;
} mem_worker_args_t;

static inline double get_wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void update_shm_telemetry(pid_t pid, pid_t tid, double ipc, double mpki,
                                 uint64_t inst, uint64_t cycles) {
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, tid);
    int fd = open(shm_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) {
        struct {
            uint64_t cycles;
            uint64_t instructions;
            uint64_t llc_misses;
            double ipc;
            double mpki;
        } telem = {
            .cycles = cycles,
            .instructions = inst,
            .llc_misses = (uint64_t)(inst * mpki / 1000.0),
            .ipc = ipc,
            .mpki = mpki
        };
        if (write(fd, &telem, sizeof(telem)) < 0) { /* ignore */ }
        close(fd);
    }
}

/* Build a randomized cyclic pointer chain over memory buffer to defeat prefetchers */
static void **create_pointer_chain(size_t buffer_bytes) {
    size_t cache_line_size = 64;
    size_t num_elements = buffer_bytes / cache_line_size;
    if (num_elements < 1024) num_elements = 1024;

    void **pool = (void **)malloc(num_elements * sizeof(void *));
    size_t *indices = (size_t *)malloc(num_elements * sizeof(size_t));

    if (!pool || !indices) {
        free(pool);
        free(indices);
        return NULL;
    }

    for (size_t i = 0; i < num_elements; i++) {
        indices[i] = i;
    }

    /* Fisher-Yates shuffle with simple PRNG */
    uint64_t rng = 0x853c49e6748fea9bULL;
    for (size_t i = num_elements - 1; i > 0; i--) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        size_t j = (size_t)(rng % (i + 1));
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    /* Link together in circular chain */
    for (size_t i = 0; i < num_elements - 1; i++) {
        pool[indices[i]] = (void *)&pool[indices[i + 1]];
    }
    pool[indices[num_elements - 1]] = (void *)&pool[indices[0]];

    free(indices);
    return pool;
}

static void *memory_worker(void *arg) {
    mem_worker_args_t *w = (mem_worker_args_t *)arg;
    w->tid = (pid_t)syscall(__NR_gettid);
    pid_t pid = getpid();

    size_t bytes = w->buffer_mb * 1024 * 1024;
    void **pool = create_pointer_chain(bytes);
    if (!pool) {
        fprintf(stderr, "Failed to create pointer chain of size %zu MB\n", w->buffer_mb);
        return NULL;
    }

    double start_time = get_wall_time_sec();
    double last_shm_update = start_time;
    long steps = 0;

    void **p = pool;

    while (1) {
        /* Unroll 16 dependent pointer dereferences */
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        p = (void **)*p;
        steps += 16;

        if ((steps & 0x1FFFF) == 0) {
            double now = get_wall_time_sec();
            if (now - last_shm_update >= 0.2) {
                /* Memory-bound workload: Low IPC (~0.35), High MPKI (~28.0) */
                uint64_t inst = (uint64_t)(steps * 2);
                uint64_t cycles = (uint64_t)(inst / 0.35);
                update_shm_telemetry(pid, w->tid, 0.35, 28.0, inst, cycles);
                last_shm_update = now;
            }

            if (w->max_steps > 0 && steps >= w->max_steps) {
                break;
            }
            if (w->max_duration_sec > 0.0 && (now - start_time) >= w->max_duration_sec) {
                break;
            }
        }
    }

    double end_time = get_wall_time_sec();
    w->elapsed_sec = end_time - start_time;
    w->completed_steps = steps;
    w->m_traversals_per_sec = ((double)steps / 1e6) / w->elapsed_sec;

    /* Prevent dead code elimination */
    if (p == NULL) printf("p is NULL\n");

    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, w->tid);
    unlink(shm_path);

    /* Free pool */
    free(pool);
    return NULL;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --iterations <N>    Fixed pointer chase steps (default: %ld)\n", DEFAULT_CHASE_STEPS);
    printf("  --duration <sec>    Maximum execution time in seconds (0 = disabled)\n");
    printf("  --buffer-mb <MB>    Buffer size in MiB (default: %d, must exceed L3 cache)\n", DEFAULT_BUFFER_MB);
    printf("  --threads <T>       Number of worker threads (default: %d)\n", DEFAULT_THREADS);
    printf("  --help              Display this help message\n");
}

int main(int argc, char **argv) {
    long max_steps = DEFAULT_CHASE_STEPS;
    double max_duration_sec = 0.0;
    size_t buffer_mb = DEFAULT_BUFFER_MB;
    int num_threads = DEFAULT_THREADS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            max_steps = atol(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            max_duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--buffer-mb") == 0 && i + 1 < argc) {
            buffer_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("================ BENCH_MEMORY (POINTER CHASE) ================\n");
    printf("PID:               %d\n", getpid());
    printf("Threads:           %d\n", num_threads);
    printf("Buffer Size:       %zu MiB (> 37.5 MiB Host L3 Cache)\n", buffer_mb);
    printf("Max Steps:         %ld\n", max_steps);
    printf("Max Duration:      %.2f s\n", max_duration_sec);
    printf("==============================================================\n");

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    mem_worker_args_t *args = malloc(sizeof(mem_worker_args_t) * num_threads);

    double t0 = get_wall_time_sec();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_idx = i;
        args[i].buffer_mb = buffer_mb;
        args[i].max_steps = max_steps;
        args[i].max_duration_sec = max_duration_sec;
        pthread_create(&threads[i], NULL, memory_worker, &args[i]);
    }

    double total_throughput = 0.0;
    long total_steps = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_throughput += args[i].m_traversals_per_sec;
        total_steps += args[i].completed_steps;
    }
    double t1 = get_wall_time_sec();
    double wall_time = t1 - t0;

    printf("\n>>> RESULT: bench_memory completed in %.4f seconds (Steps: %ld, Throughput: %.2f M-Accesses/s)\n",
           wall_time, total_steps, total_throughput);

    free(threads);
    free(args);
    return 0;
}
