# Compiles probe_fen, via `make tools` from the root or `make -C tools`.
# The verifiers have no target: uncomment their rules at the bottom of this
# file to build them.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O3 -flto=auto -Wall -Wno-strict-aliasing -I$(ROOT)/src -I$(ROOT)/lib -Wno-interference-size -Wno-format-security -Wno-class-memaccess
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
  $(wildcard $(ROOT)/src/util/*.cpp) \
  $(wildcard $(ROOT)/src/probe/*.cpp)

HDRS := $(shell find $(ROOT)/src $(ROOT)/lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

MAKEFILE := $(lastword $(MAKEFILE_LIST))

# Every rule here is explicit; without this, make's built-in .cpp rule would
# answer for a commented-out verifier and fail at link instead of saying that
# there is no rule for it.
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

.PHONY: all probe clean
.DEFAULT_GOAL := all
all:   probe_fen
probe: probe_fen

probe_fen: probe_fen.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
	$(CXX) $(CXXFLAGS) \
	    -x c++ probe_fen.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) \
	    $(LDFLAGS) -o $@

clean:
	rm -f probe_fen
# rm -f compare_syzygy
# rm -f check_tables
# rm -f check_fixedpoint

# Verifiers. No target builds these; uncomment the rule you want and run
# `make -C tools <binary>`. compare_syzygy also needs a Fathom checkout:
#   git clone https://github.com/jdart1/Fathom.git lib/Fathom
#
# FATHOM_C := $(ROOT)/lib/Fathom/src/tbprobe.c
#
# compare_syzygy: compare_syzygy.cpp $(COMMON_CXX) $(COMMON_C) $(FATHOM_C) $(HDRS) $(MAKEFILE)
#	$(CXX) $(CXXFLAGS) -I$(ROOT)/lib/Fathom/src \
#	    -x c++ compare_syzygy.cpp $(COMMON_CXX) \
#	    -x c $(COMMON_C) $(FATHOM_C) \
#	    $(LDFLAGS) -o $@
#
# check_tables: check_tables.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
#	$(CXX) $(CXXFLAGS) \
#	    -x c++ check_tables.cpp $(COMMON_CXX) \
#	    -x c $(COMMON_C) \
#	    $(LDFLAGS) -o $@
#
# check_fixedpoint: check_fixedpoint.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) $(MAKEFILE)
#	$(CXX) $(CXXFLAGS) \
#	    -x c++ check_fixedpoint.cpp $(COMMON_CXX) \
#	    -x c $(COMMON_C) \
#	    $(LDFLAGS) -o $@
