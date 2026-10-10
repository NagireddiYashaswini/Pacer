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
#include <immintrin.h>
#include <sys/syscall.h>

#define DEFAULT_PHASE_DURATION 2.0  /* Seconds per phase */
#define DEFAULT_NUM_PHASES 4        /* 2 Compute, 2 Memory */
#define DEFAULT_MATRIX_DIM 128
#define DEFAULT_BUFFER_MB 64

typedef enum {
    PHASE_COMPUTE = 0,
    PHASE_MEMORY  = 1
} workload_phase_t;

typedef struct {
    int thread_idx;
    int num_phases;
    double phase_duration_sec;
    double max_total_duration_sec;
    int matrix_dim;
    size_t buffer_mb;
    pid_t tid;
    double elapsed_sec;
    long compute_iters;
    long memory_steps;
} phase_worker_args_t;

static inline double get_wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void update_shm_telemetry(pid_t pid, pid_t tid, workload_phase_t phase,
                                 uint64_t inst, uint64_t cycles) {
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, tid);
    int fd = open(shm_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) {
        double ipc = (phase == PHASE_COMPUTE) ? 2.6 : 0.35;
        double mpki = (phase == PHASE_COMPUTE) ? 0.2 : 28.0;
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

/* Pointer chain generator */
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

    for (size_t i = 0; i < num_elements; i++) indices[i] = i;

    uint64_t rng = 0xabcdef1234567890ULL;
    for (size_t i = num_elements - 1; i > 0; i--) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        size_t j = (size_t)(rng % (i + 1));
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    for (size_t i = 0; i < num_elements - 1; i++) {
        pool[indices[i]] = (void *)&pool[indices[i + 1]];
    }
    pool[indices[num_elements - 1]] = (void *)&pool[indices[0]];

    free(indices);
    return pool;
}

/* AVX2 GEMM kernel */
static void avx2_gemm_block(const float *__restrict__ A, const float *__restrict__ B,
                            float *__restrict__ C, int N) {
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < N; k++) {
            __m256 a_val = _mm256_set1_ps(A[i * N + k]);
            for (int j = 0; j < N; j += 8) {
                __m256 b_vec = _mm256_loadu_ps(&B[k * N + j]);
                __m256 c_vec = _mm256_loadu_ps(&C[i * N + j]);
                c_vec = _mm256_fmadd_ps(a_val, b_vec, c_vec);
                _mm256_storeu_ps(&C[i * N + j], c_vec);
            }
        }
    }
}

static void *phase_worker(void *arg) {
    phase_worker_args_t *w = (phase_worker_args_t *)arg;
    w->tid = (pid_t)syscall(__NR_gettid);
    pid_t pid = getpid();

    int N = w->matrix_dim;
    size_t mat_size = (size_t)N * N * sizeof(float);
    float *A = (float *)aligned_alloc(64, mat_size);
    float *B = (float *)aligned_alloc(64, mat_size);
    float *C = (float *)aligned_alloc(64, mat_size);
    for (int i = 0; i < N * N; i++) {
        A[i] = 1.0f; B[i] = 2.0f; C[i] = 0.0f;
    }

    size_t mem_bytes = w->buffer_mb * 1024 * 1024;
    void **pool = create_pointer_chain(mem_bytes);
    void **p = pool;

    double start_time = get_wall_time_sec();
    double last_shm_update = start_time;

    long compute_iters = 0;
    long memory_steps = 0;

    for (int phase_idx = 0; phase_idx < w->num_phases; phase_idx++) {
        workload_phase_t current_phase = (phase_idx % 2 == 0) ? PHASE_COMPUTE : PHASE_MEMORY;
        double phase_start = get_wall_time_sec();

        if (w->thread_idx == 0) {
            printf("[PHASE_EVENT] time=%.3f phase=%s index=%d/%d\n",
                   phase_start - start_time,
                   (current_phase == PHASE_COMPUTE ? "COMPUTE" : "MEMORY"),
                   phase_idx + 1, w->num_phases);
            fflush(stdout);
        }

        while (1) {
            double now = get_wall_time_sec();
            if ((now - phase_start) >= w->phase_duration_sec) {
                break;
            }
            if (w->max_total_duration_sec > 0.0 && (now - start_time) >= w->max_total_duration_sec) {
                break;
            }

            if (current_phase == PHASE_COMPUTE) {
                avx2_gemm_block(A, B, C, N);
                compute_iters++;
            } else {
                /* Pointer dereferences */
                p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;
                p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;
                memory_steps += 8;
            }

            if (now - last_shm_update >= 0.2) {
                uint64_t inst = (uint64_t)((compute_iters * 500000) + (memory_steps * 2));
                uint64_t cyc = (current_phase == PHASE_COMPUTE) ? (uint64_t)(inst / 2.6) : (uint64_t)(inst / 0.35);
                update_shm_telemetry(pid, w->tid, current_phase, inst, cyc);
                last_shm_update = now;
            }
        }
    }

    double end_time = get_wall_time_sec();
    w->elapsed_sec = end_time - start_time;
    w->compute_iters = compute_iters;
    w->memory_steps = memory_steps;

    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, w->tid);
    unlink(shm_path);

    if (p == NULL) printf("p is NULL\n");

    free(A); free(B); free(C);
    free(pool);
    return NULL;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --phases <N>           Number of alternating phases (default: %d)\n", DEFAULT_NUM_PHASES);
    printf("  --phase-duration <sec> Duration per phase in seconds (default: %.1f)\n", DEFAULT_PHASE_DURATION);
    printf("  --duration <sec>       Total maximum execution time in seconds (0 = disabled)\n");
    printf("  --threads <T>          Number of worker threads (default: 1)\n");
    printf("  --help                 Display this help message\n");
}

int main(int argc, char **argv) {
    int num_phases = DEFAULT_NUM_PHASES;
    double phase_duration = DEFAULT_PHASE_DURATION;
    double max_total_duration = 0.0;
    int num_threads = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--phases") == 0 && i + 1 < argc) {
            num_phases = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--phase-duration") == 0 && i + 1 < argc) {
            phase_duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            max_total_duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("================ BENCH_PHASE (DYNAMIC PHASE SHIFTER) ================\n");
    printf("PID:               %d\n", getpid());
    printf("Threads:           %d\n", num_threads);
    printf("Number of Phases:  %d (alternating COMPUTE <-> MEMORY)\n", num_phases);
    printf("Phase Duration:    %.2f s (Total projected: %.2f s)\n", phase_duration, num_phases * phase_duration);
    printf("=====================================================================\n");

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    phase_worker_args_t *args = malloc(sizeof(phase_worker_args_t) * num_threads);

    double t0 = get_wall_time_sec();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_idx = i;
        args[i].num_phases = num_phases;
        args[i].phase_duration_sec = phase_duration;
        args[i].max_total_duration_sec = max_total_duration;
        args[i].matrix_dim = DEFAULT_MATRIX_DIM;
        args[i].buffer_mb = DEFAULT_BUFFER_MB;
        pthread_create(&threads[i], NULL, phase_worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    double t1 = get_wall_time_sec();
    double wall_time = t1 - t0;

    printf("\n>>> RESULT: bench_phase completed in %.4f seconds (%d phases executed)\n",
           wall_time, num_phases);

    free(threads);
    free(args);
    return 0;
}
