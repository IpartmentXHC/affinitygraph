CXX ?= g++
CC ?= gcc
CLANG ?= clang
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS ?=
BUILD := build
VMLINUX_H ?= $(BUILD)/bpf/vmlinux.h
LATENCY_REPO ?= ../core-to-core-latency
CALIBRATION_DIR ?= /etc/affinitygraph/calibration
CALIBRATION_OUTPUT ?= $(CALIBRATION_DIR)/hardware-node-edges.csv
CALIBRATION_SCRIPT := scripts/generate_calibration.sh

CORE_SOURCES := src/config.cpp src/topology.cpp src/collector.cpp src/graph.cpp src/solver.cpp src/domain_solver.cpp src/actuator.cpp
CORE_OBJECTS := $(CORE_SOURCES:%.cpp=$(BUILD)/%.o)
RUNTIME_OBJECTS := $(BUILD)/src/runtime.o $(BUILD)/src/bpf_reader.o

.PHONY: all test runtime-test clean install bpf calibrate \
	ops-env-check ops-local-test ops-cloud-build ops-cloud-preflight \
	ops-archive-dry ops-archive-apply ops-cloud-clean-dry ops-cloud-clean-apply

all: $(BUILD)/affinity-run $(BUILD)/affinityctl $(BUILD)/affinity-replay $(BUILD)/affinity-domain-replay $(BUILD)/affinitygraph-tests $(BUILD)/supervisor-test $(BUILD)/bpf-lifecycle-test

$(BUILD)/%.o: %.cpp include/affinitygraph/core.hpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD)/affinitygraph-tests: tests/core_test.cpp $(CORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -ldl -o $@

$(BUILD)/affinity-run: src/affinity_run.cpp $(CORE_OBJECTS) $(RUNTIME_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -ldl -o $@

$(BUILD)/affinityctl: src/affinityctl.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

$(BUILD)/affinity-replay: src/affinity_replay.cpp $(BUILD)/src/topology.o $(BUILD)/src/solver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/affinity-domain-replay: src/affinity_domain_replay.cpp $(BUILD)/src/config.o $(BUILD)/src/topology.o $(BUILD)/src/domain_solver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/supervisor-test: tests/supervisor_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -pthread -o $@

$(BUILD)/bpf-lifecycle-test: tests/bpf_lifecycle_test.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -pthread -o $@

bpf: $(VMLINUX_H)
	@mkdir -p $(BUILD)
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_arm64 -I$(dir $(VMLINUX_H)) -Iinclude -c bpf/affinitygraph.bpf.c -o $(BUILD)/affinitygraph.bpf.o

$(BUILD)/bpf/vmlinux.h:
	@mkdir -p $(@D)
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

calibrate:
	@test -d "$(LATENCY_REPO)" || { echo "missing LATENCY_REPO=$(LATENCY_REPO)" >&2; exit 2; }
	@test -x "$(CALIBRATION_SCRIPT)" || { echo "missing executable $(CALIBRATION_SCRIPT)" >&2; exit 2; }
	sudo "$(CALIBRATION_SCRIPT)" --latency-repo "$(LATENCY_REPO)" --output "$(CALIBRATION_OUTPUT)"

test: all
	$(BUILD)/affinitygraph-tests
	! nm -D $(BUILD)/affinity-replay | grep -E 'sched_setaffinity|bpf|socket|connect'
	! strings $(BUILD)/affinity-replay | grep -E '/proc|affinitygraph.sock|sched_setaffinity|BPF'
	$(BUILD)/affinity-replay --sequence tests/fixtures/sequence-small.json --strategy strategies/incremental-hotspot-v1.toml --output $(BUILD)/sequence-result.json
	grep -q '"deterministic": true' $(BUILD)/sequence-result.json
	grep -q '"effective": true' $(BUILD)/sequence-result.json
	$(BUILD)/affinity-domain-replay --runtime-log tests/fixtures/domain-runtime-small.jsonl --config config/affinitygraph.toml --output $(BUILD)/domain-replay-result.json
	grep -q '"deterministic": true' $(BUILD)/domain-replay-result.json
	grep -q '"ready": true' $(BUILD)/domain-replay-result.json
	grep -q '"families":\["family-a","family-b"\]' $(BUILD)/domain-replay-result.json
	grep -q '"family_pairs": \[' $(BUILD)/domain-replay-result.json
	grep -q '"node_decision":"initial"' $(BUILD)/domain-replay-result.json
	$(BUILD)/affinity-run run --config tests/runtime.toml -- $(BUILD)/supervisor-test
	sh tests/supervisor_test.sh

runtime-test: $(BUILD)/affinity-run $(BUILD)/affinityctl $(BUILD)/affinitygraph-tests $(BUILD)/supervisor-test $(BUILD)/bpf-lifecycle-test
	$(BUILD)/affinitygraph-tests
	$(BUILD)/affinity-run run --config tests/runtime.toml -- $(BUILD)/supervisor-test
	sh tests/supervisor_test.sh

clean:
	rm -rf $(BUILD)

install: all
	install -D -m 0755 $(BUILD)/affinity-run $(DESTDIR)/usr/sbin/affinity-run
	install -D -m 0755 $(BUILD)/affinityctl $(DESTDIR)/usr/bin/affinityctl
	install -D -m 0755 $(BUILD)/affinity-replay $(DESTDIR)/usr/bin/affinity-replay
	install -D -m 0755 $(BUILD)/affinity-domain-replay $(DESTDIR)/usr/bin/affinity-domain-replay
	install -D -m 0644 config/affinitygraph.toml $(DESTDIR)/etc/affinitygraph/affinitygraph.toml
	install -D -m 0644 calibration/kunpeng920/hardware-node-edges.csv $(DESTDIR)/etc/affinitygraph/calibration/hardware-node-edges.csv
	install -D -m 0644 calibration/kunpeng920/README.md $(DESTDIR)/etc/affinitygraph/calibration/README.md
	install -D -m 0644 calibration/kunpeng920/checksums.sha256 $(DESTDIR)/etc/affinitygraph/calibration/checksums.sha256
	install -D -m 0644 deploy/affinitygraph-clickhouse.service $(DESTDIR)/usr/lib/systemd/system/affinitygraph-clickhouse.service
	install -D -m 0644 deploy/affinitygraph@.service $(DESTDIR)/usr/lib/systemd/system/affinitygraph@.service
	install -D -m 0755 deploy/target-wrapper $(DESTDIR)/usr/libexec/affinitygraph/target-wrapper
	install -D -m 0644 LICENSE $(DESTDIR)/usr/share/doc/affinitygraph/LICENSE
	install -D -m 0644 THIRD_PARTY_NOTICES.md $(DESTDIR)/usr/share/doc/affinitygraph/THIRD_PARTY_NOTICES.md
	install -D -m 0644 docs/operations.md $(DESTDIR)/usr/share/doc/affinitygraph/operations.md
	@if test -f $(BUILD)/affinitygraph.bpf.o; then install -D -m 0644 $(BUILD)/affinitygraph.bpf.o $(DESTDIR)/usr/lib/affinitygraph/affinitygraph.bpf.o; fi

ops-env-check:
	./scripts/ops/env-check.sh

ops-local-test:
	./scripts/ops/local-test.sh

ops-cloud-build:
	./scripts/ops/cloud-build.sh

ops-cloud-preflight:
	@test -n "$(CONFIG)" || { echo "CONFIG=<config.toml> is required" >&2; exit 2; }
	./scripts/ops/cloud-preflight.sh --config "$(CONFIG)" $(if $(RELEASE),--release "$(RELEASE)",)

ops-archive-dry:
	./scripts/ops/experiment-archive.sh --dry-run

ops-archive-apply:
	@test -n "$(CONFIRM_PLAN_SHA256)" || { echo "CONFIRM_PLAN_SHA256 is required" >&2; exit 2; }
	./scripts/ops/experiment-archive.sh --apply --confirm-plan-sha256 "$(CONFIRM_PLAN_SHA256)"

ops-cloud-clean-dry:
	./scripts/ops/cloud-clean.sh --dry-run

ops-cloud-clean-apply:
	@test -n "$(CONFIRM_PLAN_SHA256)" || { echo "CONFIRM_PLAN_SHA256 is required" >&2; exit 2; }
	./scripts/ops/cloud-clean.sh --apply --confirm-plan-sha256 "$(CONFIRM_PLAN_SHA256)"
