# Compiles probe_fen, via `make tools` from the root or `make -C tools`.
# The verifiers have no target: uncomment their rules at the bottom of this
# file to build them.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

CXX      ?= g++

COMMON_FLAGS ?= -O3 -DNDEBUG -flto=auto -march=native -Wall -Wno-strict-aliasing -I$(ROOT)/src -I$(ROOT)/lib
CXXFLAGS ?= -std=c++17 $(COMMON_FLAGS) -Wno-interference-size -Wno-class-memaccess
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

ifeq ($(shell uname -m),aarch64)
CXXFLAGS += -mno-outline-atomics
CFLAGS   += -mno-outline-atomics
endif

OBJDIR := $(ROOT)/obj

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
LIB_HDRS := $(shell find $(ROOT)/lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

COMMON_C_OBJ := $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.o,$(COMMON_C))

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

$(OBJDIR)/lib/LZMA/LzmaEnc.o: CFLAGS += -Wno-dangling-pointer

.PHONY: all probe clean
.DEFAULT_GOAL := all
all:   probe_fen
probe: probe_fen

probe_fen: probe_fen.cpp $(COMMON_CXX) $(COMMON_C_OBJ) $(HDRS) $(MAKEFILE)
	$(E) "  CXXLD   probe_fen"
	$(Q)$(CXX) $(CXXFLAGS) \
	    -x c++ probe_fen.cpp $(COMMON_CXX) \
	    -x none $(COMMON_C_OBJ) \
	    $(LDFLAGS) -o $@

clean:
	$(Q)rm -f probe_fen compare_syzygy check_tables check_fixedpoint

# Verifiers. No target builds these; uncomment the rule you want and run
# `make -C tools <binary>`. compare_syzygy also needs a Fathom checkout:
#   git clone https://github.com/jdart1/Fathom.git lib/Fathom
#
# FATHOM_C     := $(ROOT)/lib/Fathom/src/tbprobe.c
# FATHOM_C_OBJ := $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.o,$(FATHOM_C))
# $(FATHOM_C_OBJ): CFLAGS += -I$(ROOT)/lib/Fathom/src
# tbprobe.c #includes tbchess.c, which LIB_HDRS does not glob.
# $(FATHOM_C_OBJ): $(wildcard $(ROOT)/lib/Fathom/src/*.c $(ROOT)/lib/Fathom/src/*.h)
#
# compare_syzygy: compare_syzygy.cpp $(COMMON_CXX) $(COMMON_C_OBJ) $(FATHOM_C_OBJ) $(HDRS) $(MAKEFILE)
#	$(E) "  CXXLD   compare_syzygy"
#	$(Q)$(CXX) $(CXXFLAGS) -I$(ROOT)/lib/Fathom/src \
#	    -x c++ compare_syzygy.cpp $(COMMON_CXX) \
#	    -x none $(COMMON_C_OBJ) $(FATHOM_C_OBJ) \
#	    $(LDFLAGS) -o $@
#
# check_tables: check_tables.cpp $(COMMON_CXX) $(COMMON_C_OBJ) $(HDRS) $(MAKEFILE)
#	$(E) "  CXXLD   check_tables"
#	$(Q)$(CXX) $(CXXFLAGS) \
#	    -x c++ check_tables.cpp $(COMMON_CXX) \
#	    -x none $(COMMON_C_OBJ) \
#	    $(LDFLAGS) -o $@
#
# check_fixedpoint: check_fixedpoint.cpp $(COMMON_CXX) $(COMMON_C_OBJ) $(HDRS) $(MAKEFILE)
#	$(E) "  CXXLD   check_fixedpoint"
#	$(Q)$(CXX) $(CXXFLAGS) \
#	    -x c++ check_fixedpoint.cpp $(COMMON_CXX) \
#	    -x none $(COMMON_C_OBJ) \
#	    $(LDFLAGS) -o $@
