CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread -mavx2 -mfma -Iinclude
LDFLAGS ?= -pthread -lm

BIN_DIR := bin
SRC_DIR := src
BENCH_DIR := benchmarks

BENCHMARKS := $(BIN_DIR)/bench_compute $(BIN_DIR)/bench_memory $(BIN_DIR)/bench_phase $(BIN_DIR)/bench_dram_chase
TESTS := $(BIN_DIR)/test_phase1 $(BIN_DIR)/test_phase_detector $(BIN_DIR)/test_rapl_wraparound $(BIN_DIR)/test_speedup_estimator $(BIN_DIR)/diagnose_speedup
DAEMON := $(BIN_DIR)/hybridsched_daemon

DAEMON_SRCS := $(SRC_DIR)/daemon_main.c $(SRC_DIR)/scheduler_daemon.c \
               $(SRC_DIR)/topology.c $(SRC_DIR)/pmu_sampler.c \
               $(SRC_DIR)/rapl_energy.c $(SRC_DIR)/phase_detector.c

.PHONY: all clean benchmarks tests daemon test

all: $(BIN_DIR) $(BENCHMARKS) $(TESTS) $(DAEMON)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Daemon
$(DAEMON): $(DAEMON_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Benchmarks
$(BIN_DIR)/bench_compute: $(BENCH_DIR)/bench_compute.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/bench_memory: $(BENCH_DIR)/bench_memory.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/bench_phase: $(BENCH_DIR)/bench_phase.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/bench_dram_chase: $(BENCH_DIR)/bench_dram_chase.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Tests
$(BIN_DIR)/test_phase1: $(SRC_DIR)/test_phase1.c $(SRC_DIR)/topology.c $(SRC_DIR)/pmu_sampler.c $(SRC_DIR)/rapl_energy.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_phase_detector: $(SRC_DIR)/test_phase_detector.c $(SRC_DIR)/phase_detector.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_rapl_wraparound: $(SRC_DIR)/test_rapl_wraparound.c $(SRC_DIR)/rapl_energy.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/test_speedup_estimator: $(SRC_DIR)/test_speedup_estimator.c $(SRC_DIR)/scheduler_daemon.c $(SRC_DIR)/topology.c $(SRC_DIR)/pmu_sampler.c $(SRC_DIR)/rapl_energy.c $(SRC_DIR)/phase_detector.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/diagnose_speedup: $(SRC_DIR)/diagnose_speedup.c $(SRC_DIR)/scheduler_daemon.c $(SRC_DIR)/topology.c $(SRC_DIR)/pmu_sampler.c $(SRC_DIR)/rapl_energy.c $(SRC_DIR)/phase_detector.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: all
	./$(BIN_DIR)/test_phase_detector
	./$(BIN_DIR)/test_rapl_wraparound
	python3 harness/smoke_test_prompt3.py

clean:
	rm -rf $(BIN_DIR)/* /dev/shm/hybridsched_*
