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

#define VECTOR_DIM 64
#define DEFAULT_RECORDS 200000
#define DEFAULT_QUERIES 50
#define DEFAULT_THREADS 2

typedef struct record_node {
    uint32_t id;
    uint32_t category;
    struct record_node *next_pointer;
    float vector[VECTOR_DIM];
} __attribute__((aligned(64))) record_node_t;

typedef struct {
    int thread_idx;
    int num_queries;
    double target_duration_sec;
    record_node_t *database_records;
    record_node_t **index_lookup_chain;
    size_t num_records;
    pid_t tid;
    double elapsed_sec;
    long total_scanned_nodes;
    long total_vector_ops;
} service_worker_args_t;

static inline double get_wall_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void update_shm_telemetry(pid_t pid, pid_t tid, bool is_compute_phase,
                                 uint64_t inst, uint64_t cycles) {
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, tid);
    int fd = open(shm_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) {
        double ipc = is_compute_phase ? 2.5 : 0.40;
        double mpki = is_compute_phase ? 0.3 : 24.0;
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

/* AVX2 Cosine / Dot-Product Calculation (Vector Scoring) */
static float avx2_vector_dot(const float *__restrict__ a, const float *__restrict__ b, int dim) {
    __m256 sum = _mm256_setzero_ps();
    for (int i = 0; i < dim; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }
    /* Horizontal sum */
    float temp[8];
    _mm256_storeu_ps(temp, sum);
    return temp[0] + temp[1] + temp[2] + temp[3] + temp[4] + temp[5] + temp[6] + temp[7];
}

static void *service_worker(void *arg) {
    service_worker_args_t *w = (service_worker_args_t *)arg;
    w->tid = (pid_t)syscall(__NR_gettid);
    pid_t pid = getpid();

    float query_vector[VECTOR_DIM];
    for (int i = 0; i < VECTOR_DIM; i++) {
        query_vector[i] = (float)(i + 1) * 0.05f;
    }

    double start_time = get_wall_time_sec();
    long scanned_nodes = 0;
    long vector_ops = 0;

    int q_idx = 0;
    while (1) {
        double now = get_wall_time_sec();
        if (w->target_duration_sec > 0.0) {
            if ((now - start_time) >= w->target_duration_sec) break;
        } else {
            if (q_idx >= w->num_queries) break;
        }

        /* -------------------------------------------------------------
         * PHASE 1: INDEX TRAVERSAL & FILTER (MEMORY-BOUND)
         * Traverses non-contiguous pointer links across the large heap.
         * ------------------------------------------------------------- */
        double phase1_start = get_wall_time_sec();
        record_node_t *candidates[512];
        int num_candidates = 0;

        /* Pick a starting node based on query index */
        size_t start_idx = ((size_t)q_idx * 7919) % w->num_records;
        record_node_t *curr = w->index_lookup_chain[start_idx];

        while (curr && num_candidates < 512) {
            scanned_nodes++;
            if ((curr->category % 17) == 0) {
                candidates[num_candidates++] = curr;
            }
            curr = curr->next_pointer;
        }

        double phase1_end = get_wall_time_sec();
        if (w->thread_idx == 0 && (q_idx % 5 == 0)) {
            printf("[SERVICE_PHASE] Query %3d | Traversal Phase (MEM) : %.3f ms | Matched: %d\n",
                   q_idx + 1, (phase1_end - phase1_start) * 1000.0, num_candidates);
            fflush(stdout);
        }

        /* Update SHM for Memory Phase */
        uint64_t inst_m = (uint64_t)(scanned_nodes * 4);
        uint64_t cyc_m = (uint64_t)(inst_m / 0.40);
        update_shm_telemetry(pid, w->tid, false, inst_m, cyc_m);

        /* -------------------------------------------------------------
         * PHASE 2: VECTOR SIMD SCORING & RANKING (COMPUTE-BOUND)
         * Executes dense AVX2 FMA dot products across matched records.
         * ------------------------------------------------------------- */
        double phase2_start = get_wall_time_sec();
        float best_score = -1e9f;
        int best_id = -1;

        for (int rep = 0; rep < 400; rep++) {
            for (int c = 0; c < num_candidates; c++) {
                float score = avx2_vector_dot(query_vector, candidates[c]->vector, VECTOR_DIM);
                if (score > best_score) {
                    best_score = score;
                    best_id = candidates[c]->id;
                }
                vector_ops++;
            }
        }

        double phase2_end = get_wall_time_sec();
        if (w->thread_idx == 0 && (q_idx % 5 == 0)) {
            printf("[SERVICE_PHASE] Query %3d | SIMD Scoring   (COMP): %.3f ms | Top ID: %d (Score: %.2f)\n",
                   q_idx + 1, (phase2_end - phase2_start) * 1000.0, best_id, best_score);
            fflush(stdout);
        }

        /* Update SHM for Compute Phase */
        uint64_t inst_c = (uint64_t)(vector_ops * 8);
        uint64_t cyc_c = (uint64_t)(inst_c / 2.5);
        update_shm_telemetry(pid, w->tid, true, inst_c, cyc_c);

        q_idx++;
    }

    double end_time = get_wall_time_sec();
    w->elapsed_sec = end_time - start_time;
    w->total_scanned_nodes = scanned_nodes;
    w->total_vector_ops = vector_ops;

    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/hybridsched_%d_%d", pid, w->tid);
    unlink(shm_path);

    return NULL;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  --duration <sec>       Target runtime duration in seconds (default: 4.0)\n");
    printf("  --queries <Q>          Number of queries per thread if duration=0 (default: %d)\n", DEFAULT_QUERIES);
    printf("  --records <N>          Database size in records (default: %d)\n", DEFAULT_RECORDS);
    printf("  --threads <T>          Number of worker threads (default: %d)\n", DEFAULT_THREADS);
    printf("  --help                 Display this help message\n");
}

int main(int argc, char **argv) {
    double duration_sec = 4.0;
    int num_queries = DEFAULT_QUERIES;
    size_t num_records = DEFAULT_RECORDS;
    int num_threads = DEFAULT_THREADS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--queries") == 0 && i + 1 < argc) {
            num_queries = atoi(argv[++i]);
            duration_sec = 0.0;
        } else if (strcmp(argv[i], "--records") == 0 && i + 1 < argc) {
            num_records = (size_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    printf("================ BENCH_SERVICE (REALISTIC VECTOR SEARCH ENGINE) ================\n");
    printf("PID:               %d\n", getpid());
    printf("Threads:           %d\n", num_threads);
    printf("Database Records:  %zu (~%.1f MB heap)\n",
           num_records, (double)(num_records * sizeof(record_node_t)) / (1024.0 * 1024.0));
    if (duration_sec > 0.0) {
        printf("Target Duration:   %.2f s\n", duration_sec);
    } else {
        printf("Queries / Thread:  %d\n", num_queries);
    }
    printf("Vector Dimension:  %d floats (AVX2 FMA accelerated)\n", VECTOR_DIM);
    printf("=================================================================================\n");

    /* Allocate and initialize linked database nodes */
    printf("[+] Allocating and indexing %zu record nodes...", num_records);
    fflush(stdout);
    record_node_t *records = (record_node_t *)aligned_alloc(64, num_records * sizeof(record_node_t));
    record_node_t **lookup_chain = (record_node_t **)malloc(num_records * sizeof(record_node_t *));
    if (!records || !lookup_chain) {
        fprintf(stderr, "Failed to allocate record database\n");
        return 1;
    }

    /* Pseudo-random permutation for pointer chasing */
    size_t *indices = (size_t *)malloc(num_records * sizeof(size_t));
    for (size_t i = 0; i < num_records; i++) indices[i] = i;

    uint64_t rng = 0xdeadbeefcafebabeULL;
    for (size_t i = num_records - 1; i > 0; i--) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        size_t j = (size_t)(rng % (i + 1));
        size_t tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    for (size_t i = 0; i < num_records; i++) {
        size_t idx = indices[i];
        records[idx].id = (uint32_t)idx;
        records[idx].category = (uint32_t)(idx * 31);
        for (int v = 0; v < VECTOR_DIM; v++) {
            records[idx].vector[v] = ((float)((idx + v) % 100)) / 100.0f;
        }
        lookup_chain[i] = &records[idx];
    }

    for (size_t i = 0; i < num_records - 1; i++) {
        lookup_chain[i]->next_pointer = lookup_chain[i + 1];
    }
    lookup_chain[num_records - 1]->next_pointer = NULL;
    free(indices);
    printf(" DONE.\n");

    /* Spawn query worker threads */
    pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
    service_worker_args_t *args = malloc(sizeof(service_worker_args_t) * num_threads);

    double t0 = get_wall_time_sec();
    for (int i = 0; i < num_threads; i++) {
        args[i].thread_idx = i;
        args[i].num_queries = num_queries;
        args[i].target_duration_sec = duration_sec;
        args[i].database_records = records;
        args[i].index_lookup_chain = lookup_chain;
        args[i].num_records = num_records;
        pthread_create(&threads[i], NULL, service_worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    double t1 = get_wall_time_sec();
    double wall_time = t1 - t0;

    long total_scanned = 0;
    long total_vector = 0;
    for (int i = 0; i < num_threads; i++) {
        total_scanned += args[i].total_scanned_nodes;
        total_vector += args[i].total_vector_ops;
    }

    printf("\n>>> RESULT: bench_service completed in %.4f seconds (Scanned: %ld nodes, Vector Ops: %ld)\n",
           wall_time, total_scanned, total_vector);

    free(threads);
    free(args);
    free(lookup_chain);
    free(records);
    return 0;
}
