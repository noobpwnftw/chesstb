CXX      ?= g++
CXX_IS_GCC := $(shell $(CXX) -dM -E -x c++ /dev/null | awk '/__GNUC__/ { gcc = 1 } /__clang__/ { clang = 1 } END { print gcc && !clang }')

COMMON_FLAGS ?= -O3 -DNDEBUG -flto=auto -march=native -Wall -Wno-strict-aliasing -Isrc -Ilib
CXXFLAGS ?= -std=c++17 $(COMMON_FLAGS)
CFLAGS   ?= $(COMMON_FLAGS)
LDFLAGS  ?= -pthread

# GEN_FORCE_INLINE (src/util/defines.h): ~1% of generation throughput for the
# movegen-driving targets, ~50s of serial LTO inlining for anything linking
# src/probe, so the tools opt out. One binary must not mix tagged and untagged
# TUs: always_inline is part of the definition, so a mismatch violates ODR.
GEN_DEFS := -DCHESSTB_GEN

# Command echo: quiet by default, `make V=1` prints the full command lines.
V ?= 0
ifeq ($(V),0)
Q := @
E := @echo
else
Q :=
E := @:
endif
# Suppress the sub-make's Entering/Leaving lines too, but only when quiet.
NPD := $(if $(Q),--no-print-directory)

ifeq ($(CXX_IS_GCC),1)
CXXFLAGS += -Wno-interference-size -Wno-class-memaccess
endif

ifeq ($(shell uname -m),aarch64)
CXXFLAGS += -mno-outline-atomics
CFLAGS   += -mno-outline-atomics
endif

OBJDIR := obj

COMMON_C := \
  $(wildcard lib/lz4/*.c) \
  $(wildcard lib/LZMA/*.c) \
  $(wildcard lib/zstd/common/*.c) \
  $(wildcard lib/zstd/compress/*.c) \
  $(wildcard lib/zstd/dictBuilder/*.c)

COMMON_CXX := \
  $(wildcard src/chess/*.cpp) \
  $(wildcard src/egtb/*.cpp) \
  $(wildcard src/util/*.cpp)

TRANSCRIBE_CXX := \
  $(wildcard src/transcribe/*.cpp)

SHRINK_C := lib/zstd/common/xxhash.c
SHRINK_CXX := \
  src/util/filesystem.cpp \
  src/util/intrin.cpp \
  src/shrink/shrink.cpp

HDRS := $(shell find src lib -name '*.h' -o -name '*.hpp' 2>/dev/null)
LIB_HDRS := $(shell find lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

COMMON_C_OBJ := $(COMMON_C:%.c=$(OBJDIR)/%.o)
SHRINK_C_OBJ := $(SHRINK_C:%.c=$(OBJDIR)/%.o)

$(OBJDIR)/%.o: %.c $(LIB_HDRS) Makefile
	@mkdir -p $(dir $@)
	$(E) "  CC      $<"
	$(Q)$(CXX) $(CFLAGS) -x c -c $< -o $@

ifeq ($(CXX_IS_GCC),1)
$(OBJDIR)/lib/LZMA/LzmaEnc.o: CFLAGS += -Wno-dangling-pointer
endif

.PHONY: all tools clean
all: chesstb shrink transcribe

chesstb: src/main.cpp $(COMMON_CXX) $(COMMON_C_OBJ) $(HDRS) Makefile
	$(E) "  CXXLD   chesstb"
	$(Q)$(CXX) $(CXXFLAGS) $(GEN_DEFS) \
	    -x c++ src/main.cpp $(COMMON_CXX) \
	    -x none $(COMMON_C_OBJ) \
	    $(LDFLAGS) -o $@

transcribe: $(TRANSCRIBE_CXX) $(COMMON_CXX) $(COMMON_C_OBJ) $(HDRS) Makefile
	$(E) "  CXXLD   transcribe"
	$(Q)$(CXX) $(CXXFLAGS) $(GEN_DEFS) \
	    -x c++ $(TRANSCRIBE_CXX) $(COMMON_CXX) \
	    -x none $(COMMON_C_OBJ) \
	    $(LDFLAGS) -o $@

shrink: $(SHRINK_CXX) $(SHRINK_C_OBJ) $(HDRS) Makefile
	$(E) "  CXXLD   shrink"
	$(Q)$(CXX) $(CXXFLAGS) \
	    -x c++ $(SHRINK_CXX) \
	    -x none $(SHRINK_C_OBJ) \
	    $(LDFLAGS) -o $@

tools:
	$(Q)$(MAKE) $(NPD) -C tools

clean:
	$(Q)rm -rf $(OBJDIR)
	$(Q)rm -f chesstb shrink transcribe
	$(Q)$(MAKE) $(NPD) -C tools clean
