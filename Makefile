CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG -flto=auto -march=native -Wall -Isrc -Ilib -Wno-interference-size -Wno-format-security -Wno-class-memaccess
CFLAGS   ?= -O3 -DNDEBUG -flto=auto -march=native -Wall -Ilib
LDFLAGS  ?= -pthread

ifeq ($(shell uname -m),aarch64)
CXXFLAGS += -mno-outline-atomics
endif

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

SHRINK_C := lib/zstd/common/xxhash.c
SHRINK_CXX := \
  src/util/filesystem.cpp \
  src/util/intrin.cpp \
  src/shrink/shrink.cpp

HDRS := $(shell find src lib -name '*.h' -o -name '*.hpp' 2>/dev/null)

.PHONY: all tests clean
all: chesstb shrink

chesstb: src/main.cpp $(COMMON_CXX) $(COMMON_C) $(HDRS) Makefile
	$(CXX) $(CXXFLAGS) \
	    -x c++ src/main.cpp $(COMMON_CXX) \
	    -x c $(COMMON_C) \
	    $(LDFLAGS) -o $@

shrink: $(SHRINK_CXX) $(SHRINK_C) $(HDRS) Makefile
	$(CXX) $(CXXFLAGS) \
	    -x c++ $(SHRINK_CXX) \
	    -x c $(SHRINK_C) \
	    $(LDFLAGS) -o $@

tests:
	$(MAKE) -C tests

clean:
	rm -f chesstb shrink
	$(MAKE) -C tests clean
