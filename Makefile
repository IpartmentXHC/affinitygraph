CXX ?= g++
CC ?= gcc
CLANG ?= clang
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
LDFLAGS ?=
BUILD := build
VMLINUX_H ?= $(BUILD)/bpf/vmlinux.h

CORE_SOURCES := src/config.cpp src/topology.cpp src/collector.cpp src/graph.cpp src/solver.cpp src/actuator.cpp
CORE_OBJECTS := $(CORE_SOURCES:%.cpp=$(BUILD)/%.o)
RUNTIME_OBJECTS := $(BUILD)/src/runtime.o $(BUILD)/src/bpf_reader.o

.PHONY: all test clean install bpf

all: $(BUILD)/affinity-run $(BUILD)/affinityctl $(BUILD)/affinitygraph-tests $(BUILD)/supervisor-test $(BUILD)/bpf-lifecycle-test

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -c $< -o $@

$(BUILD)/affinitygraph-tests: tests/core_test.cpp $(CORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -ldl -o $@

$(BUILD)/affinity-run: src/affinity_run.cpp $(CORE_OBJECTS) $(RUNTIME_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -pthread -ldl -o $@

$(BUILD)/affinityctl: src/affinityctl.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

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
	$(BUILD)/affinity-run run --config tests/runtime.toml -- $(BUILD)/supervisor-test
	sh tests/supervisor_test.sh

clean:
	rm -rf $(BUILD)

install: all
	install -D -m 0755 $(BUILD)/affinity-run $(DESTDIR)/usr/sbin/affinity-run
	install -D -m 0755 $(BUILD)/affinityctl $(DESTDIR)/usr/bin/affinityctl
	install -D -m 0644 config/affinitygraph.toml $(DESTDIR)/etc/affinitygraph/affinitygraph.toml
	install -D -m 0644 calibration/kunpeng920/hardware-node-edges.csv $(DESTDIR)/etc/affinitygraph/calibration/hardware-node-edges.csv
	install -D -m 0644 calibration/kunpeng920/README.md $(DESTDIR)/etc/affinitygraph/calibration/README.md
	install -D -m 0644 calibration/kunpeng920/checksums.sha256 $(DESTDIR)/etc/affinitygraph/calibration/checksums.sha256
	install -D -m 0644 deploy/affinitygraph-clickhouse.service $(DESTDIR)/usr/lib/systemd/system/affinitygraph-clickhouse.service
	install -D -m 0644 LICENSE $(DESTDIR)/usr/share/doc/affinitygraph/LICENSE
	install -D -m 0644 THIRD_PARTY_NOTICES.md $(DESTDIR)/usr/share/doc/affinitygraph/THIRD_PARTY_NOTICES.md
	install -D -m 0644 docs/operations.md $(DESTDIR)/usr/share/doc/affinitygraph/operations.md
	@if test -f $(BUILD)/affinitygraph.bpf.o; then install -D -m 0644 $(BUILD)/affinitygraph.bpf.o $(DESTDIR)/usr/lib/affinitygraph/affinitygraph.bpf.o; fi
