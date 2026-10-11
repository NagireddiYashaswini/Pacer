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

#define DEFAULT_NUM_PHASES 8
#define DEFAULT_MIN_DUR 0.25
#define DEFAULT_MAX_DUR 1.00
#define DEFAULT_MATRIX_DIM 128
#define DEFAULT_BUFFER_MB 64

typedef enum {
    PHASE_COMPUTE = 0,
    PHASE_MEMORY  = 1
} workload_phase_t;

typedef struct {
    int thread_idx;
    int num_phases;
    double min_dur_sec;
    double max_dur_sec;
    unsigned int seed;
    double p_stay_compute;  /* Markov P(Compute -> Compute) */
    double p_stay_memory;   /* Markov P(Memory -> Memory) */
    int matrix_dim;
    size_t buffer_mb;
    pid_t tid;
    double elapsed_sec;
    long compute_iters;
    long memory_steps;
} irregular_worker_args_t;

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

/* Pointer chain generator for pseudo-random memory chasing */
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

    uint64_t rng = 0x123456789abcdef0ULL;
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

static void *irregular_worker(void *arg) {
    irregular_worker_args_t *w = (irregular_worker_args_t *)arg;
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

    unsigned int rand_state = w->seed + w->thread_idx * 1000;
    workload_phase_t current_phase = PHASE_COMPUTE;

    for (int phase_idx = 0; phase_idx < w->num_phases; phase_idx++) {
        /* Determine next phase using Markov transition matrix */
        double r_trans = (double)rand_r(&rand_state) / (double)RAND_MAX;
        if (current_phase == PHASE_COMPUTE) {
            current_phase = (r_trans < w->p_stay_compute) ? PHASE_COMPUTE : PHASE_MEMORY;
        } else {
            current_phase = (r_trans < w->p_stay_memory) ? PHASE_MEMORY : PHASE_COMPUTE;
        }

        /* Sample duration uniformly between min_dur and max_dur */
        double r_dur = (double)rand_r(&rand_state) / (double)RAND_MAX;
        double phase_duration = w->min_dur_sec + r_dur * (w->max_dur_sec - w->min_dur_sec);

        double phase_start = get_wall_time_sec();

        if (w->thread_idx == 0) {
            printf("[PHASE_EVENT] time=%.3f phase=%s dur=%.3fs index=%d/%d\n",
                   phase_start - start_time,
                   (current_phase == PHASE_COMPUTE ? "COMPUTE" : "MEMORY"),
                   phase_duration, phase_idx + 1, w->num_phases);
            fflush(stdout);
        }

        while (1) {
            double now = get_wall_time_sec();
            if ((now - phase_start) >= phase_duration) {
                break;
            }

            if (current_phase == PHASE_COMPUTE) {
                avx2_gemm_block(A, B, C, N);
                compute_iters++;
            } else {
                p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;
                p = (void **)*p; p = (void **)*p; p = (void **)*p; p = (void **)*p;
                memory_steps += 8;
            }

            double update_interval = (phase_duration < 0.2) ? (phase_duration / 2.0) : 0.05;
            if (update_interval < 0.005) update_interval = 0.005;

            if (now - last_shm_update >= update_interval) {
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
    printf("  --phases <N>           Number of stochastic phases (default: %d)\n", DEFAULT_NUM_PHASES);
    printf("  --min-dur <sec>        Minimum phase duration in seconds (default: %.2f)\n", DEFAULT_MIN_DUR);
    printf("  --max-dur <sec>        Maximum phase duration in seconds (default: %.2f)\n", DEFAULT_MAX_DUR);
    printf("  --p-stay-comp <p>      Markov P(Compute->Compute) (default: 0.30)\n");
    printf("  --p-stay-mem <p>       Markov P(Memory->Memory) (default: 0.30)\n");
    printf("  --seed <int>           Random seed for reproducibility (default: 42)\n");
    printf("  --threads <T>          Number of worker threads (default: 1)\n");
    printf("  --help                 Display this help message\n");
}

int main(int argc, char **argv) {
    int num_phases = DEFAULT_NUM_PHASES;
    double min_dur = DEFAULT_MIN_DUR;
    double max_dur = DEFAULT_MAX_DUR;
    double p_stay_comp = 0.30;
    double p_stay_mem = 0.30;
    unsigned int seed = 42;
    int num_threads = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--phases") == 0 && i + 1 < argc) {
            num_phases = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--min-dur") == 0 && i + 1 < argc) {
            min_dur = atof(argv[++i]);
        } else if (strcmp(argv[i], "--max-dur") == 0 && i + 1 < argc) {
            max_dur = atof(argv[++i]);
        } else if (strcmp(argv[i], "--p-stay-comp") == 0 && i + 1 < argc) {
            p_stay_comp = atof(argv[++i]);
        } else if (strcmp(argv[i], "--p-stay-mem") == 0 && i + 1 < argc) {
            p_stay_mem = atof(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("================ BENCH_IRREGULAR (STOCHASTIC MARKOV PHASE SHIFTER) ================\n");
    printf("PID:                %d\n", getpid());
    printf("Threads:            %d\n", num_threads);
    printf("Number of Phases:   %d\n", num_phases);
    printf("Duration Range:     [%.2f s, %.2f s]\n", min_dur, max_dur);
    printf("Transition Matrix:  P(C->C)=%.2f, P(C->M)=%.2f | P(M->M)=%.2f, P(M->C)=%.2f\n",
           p_stay_comp, 1.0 - p_stay_comp, p_stay_mem, 1.0 - p_stay_mem);
    printf("Random Seed:        %u\n", seed);
    printf("===================================================================================\n");

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    irregular_worker_args_t *args = malloc(sizeof(irregular_worker_args_t) * num_threads);

    double t0 = get_wall_time_sec();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_idx = i;
        args[i].num_phases = num_phases;
        args[i].min_dur_sec = min_dur;
        args[i].max_dur_sec = max_dur;
        args[i].p_stay_compute = p_stay_comp;
        args[i].p_stay_memory = p_stay_mem;
        args[i].seed = seed;
        args[i].matrix_dim = DEFAULT_MATRIX_DIM;
        args[i].buffer_mb = DEFAULT_BUFFER_MB;
        pthread_create(&threads[i], NULL, irregular_worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    double t1 = get_wall_time_sec();
    double wall_time = t1 - t0;

    printf("\n>>> RESULT: bench_irregular completed in %.4f seconds (%d phases executed)\n",
           wall_time, num_phases);

    free(threads);
    free(args);
    return 0;
}
