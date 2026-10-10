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

#define DEFAULT_MATRIX_DIM 128
#define DEFAULT_ITERATIONS 500
#define DEFAULT_THREADS 1

typedef struct {
    int thread_idx;
    int matrix_dim;
    long max_iterations;
    double max_duration_sec;
    pid_t tid;
    double elapsed_sec;
    long completed_iterations;
    double gflops;
} worker_args_t;

static inline double get_wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* AVX2 GEMM kernel: C += A * B for N x N matrices */
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

static void *compute_worker(void *arg) {
    worker_args_t *w = (worker_args_t *)arg;
    w->tid = (pid_t)syscall(__NR_gettid);
    pid_t pid = getpid();

    int N = w->matrix_dim;
    size_t mat_size = (size_t)N * N * sizeof(float);

    float *A = (float *)aligned_alloc(64, mat_size);
    float *B = (float *)aligned_alloc(64, mat_size);
    float *C = (float *)aligned_alloc(64, mat_size);

    if (!A || !B || !C) {
        fprintf(stderr, "Failed to allocate aligned matrices\n");
        return NULL;
    }

    for (int i = 0; i < N * N; i++) {
        A[i] = (float)(i % 17) * 0.1f;
        B[i] = (float)(i % 19) * 0.1f;
        C[i] = 0.0f;
    }

    double start_time = get_wall_time_sec();
    double last_shm_update = start_time;
    long iters = 0;

    double ops_per_iter = 2.0 * (double)N * (double)N * (double)N;

    while (1) {
        avx2_gemm_block(A, B, C, N);
        iters++;

        double now = get_wall_time_sec();
        if (now - last_shm_update >= 0.2) {
            /* Compute workload: High IPC (~2.6), minimal MPKI (~0.2) */
            uint64_t inst = (uint64_t)(iters * (ops_per_iter / 4.0));
            uint64_t cycles = (uint64_t)(inst / 2.6);
            update_shm_telemetry(pid, w->tid, 2.6, 0.2, inst, cycles);
            last_shm_update = now;
        }

        if (w->max_iterations > 0 && iters >= w->max_iterations) {
            break;
        }
        if (w->max_duration_sec > 0.0 && (now - start_time) >= w->max_duration_sec) {
            break;
        }
    }

    double end_time = get_wall_time_sec();
    w->elapsed_sec = end_time - start_time;
    w->completed_iterations = iters;
    w->gflops = (iters * ops_per_iter * 1e-9) / w->elapsed_sec;

    /* Final telemetry update */
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, w->tid);
    unlink(shm_path);

    free(A);
    free(B);
    free(C);
    return NULL;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --iterations <N>    Fixed iterations to execute (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  --duration <sec>    Maximum execution time in seconds (0 = disabled)\n");
    printf("  --threads <T>       Number of worker threads (default: %d)\n", DEFAULT_THREADS);
    printf("  --matrix-size <N>   Matrix dimension N (default: %d, must be multiple of 8)\n", DEFAULT_MATRIX_DIM);
    printf("  --help              Display this help message\n");
}

int main(int argc, char **argv) {
    long max_iterations = DEFAULT_ITERATIONS;
    double max_duration_sec = 0.0;
    int num_threads = DEFAULT_THREADS;
    int matrix_dim = DEFAULT_MATRIX_DIM;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            max_iterations = atol(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            max_duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--matrix-size") == 0 && i + 1 < argc) {
            matrix_dim = atoi(argv[++i]);
            if (matrix_dim % 8 != 0) matrix_dim = ((matrix_dim / 8) + 1) * 8;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("================ BENCH_COMPUTE (AVX2 GEMM) ================\n");
    printf("PID:               %d\n", getpid());
    printf("Threads:           %d\n", num_threads);
    printf("Matrix Dimension:  %d x %d\n", matrix_dim, matrix_dim);
    printf("Max Iterations:    %ld\n", max_iterations);
    printf("Max Duration:      %.2f s\n", max_duration_sec);
    printf("===========================================================\n");

    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    worker_args_t *args = malloc(sizeof(worker_args_t) * num_threads);

    double t0 = get_wall_time_sec();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_idx = i;
        args[i].matrix_dim = matrix_dim;
        args[i].max_iterations = max_iterations;
        args[i].max_duration_sec = max_duration_sec;
        pthread_create(&threads[i], NULL, compute_worker, &args[i]);
    }

    double total_gflops = 0.0;
    long total_iters = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_gflops += args[i].gflops;
        total_iters += args[i].completed_iterations;
    }
    double t1 = get_wall_time_sec();
    double wall_time = t1 - t0;

    printf("\n>>> RESULT: bench_compute completed in %.4f seconds (Iterations: %ld, Throughput: %.2f GFLOPS)\n",
           wall_time, total_iters, total_gflops);

    free(threads);
    free(args);
    return 0;
}
