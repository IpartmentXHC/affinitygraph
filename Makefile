CXX ?= g++
CC ?= gcc
CLANG ?= clang
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS ?=
BUILD := build
VMLINUX_H ?= $(BUILD)/bpf/vmlinux.h

CORE_SOURCES := src/config.cpp src/topology.cpp src/collector.cpp src/graph.cpp src/solver.cpp src/domain_solver.cpp src/actuator.cpp
CORE_OBJECTS := $(CORE_SOURCES:%.cpp=$(BUILD)/%.o)
RUNTIME_OBJECTS := $(BUILD)/src/runtime.o $(BUILD)/src/bpf_reader.o

.PHONY: all test runtime-test clean install bpf

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
	install -D -m 0644 LICENSE $(DESTDIR)/usr/share/doc/affinitygraph/LICENSE
	install -D -m 0644 THIRD_PARTY_NOTICES.md $(DESTDIR)/usr/share/doc/affinitygraph/THIRD_PARTY_NOTICES.md
	install -D -m 0644 docs/operations.md $(DESTDIR)/usr/share/doc/affinitygraph/operations.md
	@if test -f $(BUILD)/affinitygraph.bpf.o; then install -D -m 0644 $(BUILD)/affinitygraph.bpf.o $(DESTDIR)/usr/lib/affinitygraph/affinitygraph.bpf.o; fi
