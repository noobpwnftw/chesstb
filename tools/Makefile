# Compiles probe_fen, via `make tools` from the root or `make -C tools`.
# The verifiers have no target: uncomment their rules at the bottom of this
# file to build them.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

CXX      ?= g++
CXX_IS_GCC := $(shell $(CXX) -dM -E -x c++ /dev/null | awk '/__GNUC__/ { gcc = 1 } /__clang__/ { clang = 1 } END { print gcc && !clang }')

COMMON_FLAGS ?= -O3 -DNDEBUG -flto=auto -march=native -Wall -Wno-strict-aliasing -I$(ROOT)/src -I$(ROOT)/lib
CXXFLAGS ?= -std=c++17 $(COMMON_FLAGS)
CFLAGS   ?= $(COMMON_FLAGS)
LDFLAGS  ?= -pthread

# Command echo: quiet by default, `make V=1` prints the full command lines.
V ?= 0
ifeq ($(V),0)
Q := @
E := @echo
else
Q :=
E := @:
endif

ifeq ($(CXX_IS_GCC),1)
CXXFLAGS += -Wno-interference-size -Wno-class-memaccess
endif

ifeq ($(shell uname -m),aarch64)
CXXFLAGS += -mno-outline-atomics
CFLAGS   += -mno-outline-atomics
endif

OBJDIR := $(ROOT)/obj

# What the prober and its API define.
COMMON_CXX := \
  $(wildcard $(ROOT)/src/chess/*.cpp) \
  $(ROOT)/src/util/filesystem.cpp \
  $(wildcard $(ROOT)/src/probe/*.cpp)

HDRS := $(shell find $(ROOT)/src $(ROOT)/lib -name '*.h' -o -name '*.hpp' 2>/dev/null)
LIB_HDRS := $(shell find $(ROOT)/lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

# The prober as drop-in sources in namespace chesstb (`make lib`):
# obj/libchesstb/chesstb.h and chesstb.cpp. A host copies them and compiles
# chesstb.cpp with its own flags. probe_fen builds the same way, without the
# tree's include paths.
LIBDIR := $(OBJDIR)/libchesstb

MAKEFILE := $(lastword $(MAKEFILE_LIST))

# Every rule here is explicit; without this, make's built-in .cpp rule would
# answer for a commented-out verifier and fail at link instead of saying that
# there is no rule for it.
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

$(OBJDIR)/%.o: $(ROOT)/%.c $(LIB_HDRS) $(MAKEFILE)
	@mkdir -p $(dir $@)
	$(E) "  CC      $(patsubst $(ROOT)/%,%,$<)"
	$(Q)$(CXX) $(CFLAGS) -x c -c $< -o $@

.PHONY: all probe lib clean
.DEFAULT_GOAL := all
all:   probe_fen
probe: probe_fen

probe_fen: probe_fen.cpp $(LIBDIR)/chesstb.cpp $(MAKEFILE)
	$(E) "  CXXLD   probe_fen"
	$(Q)$(CXX) $(filter-out -I%,$(CXXFLAGS)) -DCHESSTB_NOTATION -I$(LIBDIR) probe_fen.cpp $(LIBDIR)/chesstb.cpp $(LDFLAGS) -o $@

lib: $(LIBDIR)/chesstb.cpp

$(LIBDIR)/chesstb.cpp: lib.py $(COMMON_CXX) $(HDRS) $(MAKEFILE)
	$(E) "  GEN     libchesstb"
	$(Q)python3 lib.py $(LIBDIR) $(COMMON_CXX)

# A verifier: its source in the library's form (lib.py -t) built with
# chesstb.cpp and the index -> position inverse, whose divisions need intrin.cpp,
# plus any objects among its prerequisites.
define VERIFIER
	$(E) "  CXXLD   $@"
	$(Q)python3 lib.py -t $(VERIFIER_I) $(OBJDIR)/tools $< $(ROOT)/src/util/intrin.cpp
	$(Q)$(CXX) $(CXXFLAGS) $(VERIFIER_I) -DCHESSTB_INDEX_INVERSE -DCHESSTB_NOTATION \
	    $(OBJDIR)/tools/$< $(LIBDIR)/chesstb.cpp $(filter %.o,$^) $(LDFLAGS) -o $@
endef

clean:
	$(Q)rm -f probe_fen compare_syzygy check_tables check_fixedpoint
	$(Q)rm -rf $(LIBDIR) $(OBJDIR)/tools

# Verifiers. No target builds these; uncomment the rule you want and run
# `make -C tools <binary>`. They walk tables index by index, so they build the
# prober's index -> position (CHESSTB_INDEX_INVERSE). compare_syzygy also needs
# a Fathom checkout:
#   git clone https://github.com/jdart1/Fathom.git lib/Fathom
#
# FATHOM_C     := $(ROOT)/lib/Fathom/src/tbprobe.c
# FATHOM_C_OBJ := $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.o,$(FATHOM_C))
# $(FATHOM_C_OBJ): CFLAGS += -I$(ROOT)/lib/Fathom/src
# $(FATHOM_C_OBJ): $(wildcard $(ROOT)/lib/Fathom/src/*.c $(ROOT)/lib/Fathom/src/*.h)
#
# XXHASH_OBJ := $(OBJDIR)/lib/zstd/common/xxhash.o
#
# compare_syzygy: VERIFIER_I := -I$(ROOT)/lib/Fathom/src
# compare_syzygy: compare_syzygy.cpp $(FATHOM_C_OBJ) $(LIBDIR)/chesstb.cpp $(MAKEFILE)
#	$(VERIFIER)
#
# check_tables: check_tables.cpp $(XXHASH_OBJ) $(LIBDIR)/chesstb.cpp $(MAKEFILE)
#	$(VERIFIER)
#
# check_fixedpoint: check_fixedpoint.cpp $(LIBDIR)/chesstb.cpp $(MAKEFILE)
#	$(VERIFIER)
