# Compiles the test/diagnostic tools used by ../run_probe and ../run_compare.
# Invoked from those wrappers; usable standalone with `make -C tests`.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -flto=auto -Wall -I$(ROOT)/src -I$(ROOT)/lib -Wno-interference-size -Wno-format-security -Wno-class-memaccess
LDFLAGS  ?= -pthread

ifeq ($(shell uname -m),aarch64)
CXXFLAGS += -mno-outline-atomics
endif

COMMON_C := \
  $(wildcard $(ROOT)/lib/lz4/*.c) \
  $(wildcard $(ROOT)/lib/LZMA/*.c) \
  $(wildcard $(ROOT)/lib/zstd/common/*.c) \
  $(wildcard $(ROOT)/lib/zstd/compress/*.c) \
  $(wildcard $(ROOT)/lib/zstd/dictBuilder/*.c)

COMMON_CXX := \
  $(wildcard $(ROOT)/src/chess/*.cpp) \
  $(wildcard $(ROOT)/src/egtb/*.cpp) \
  $(wildcard $(ROOT)/src/util/*.cpp) \
  $(wildcard $(ROOT)/src/probe/*.cpp)

FATHOM_C := $(ROOT)/lib/Fathom/src/tbprobe.c

HDRS := $(shell find $(ROOT)/src $(ROOT)/lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

MAKEFILE := $(lastword $(MAKEFILE_LIST))

.PHONY: all probe compare check-tables check-fixedpoint clean
all: probe_fen compare_chesstb check_tables check_fixedpoint
probe:            probe_fen
compare:          compare_chesstb
check-tables:     check_tables
check-fixedpoint: check_fixedpoint

probe_fen: probe_fen.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) \
	    -x c++ probe_fen.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) \
	    $(LDFLAGS) -o $@

compare_chesstb: compare_disk_vs_syzygy.cpp $(COMMON_CXX) $(COMMON_C) $(FATHOM_C) $(HDRS) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/lib/Fathom/src \
	    -x c++ compare_disk_vs_syzygy.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) $(FATHOM_C) \
	    $(LDFLAGS) -o $@

check_tables: check_tables.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) \
	    -x c++ check_tables.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) \
	    $(LDFLAGS) -o $@

check_fixedpoint: check_fixedpoint.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) \
	    -x c++ check_fixedpoint.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) \
	    $(LDFLAGS) -o $@

clean:
	rm -f probe_fen compare_chesstb check_tables check_fixedpoint
