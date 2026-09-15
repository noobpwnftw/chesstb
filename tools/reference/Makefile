# Reference tools.
#
#   make test_retro        board and reverse move generation tests
#   make refgen            standalone reference tablebase generator
#   make check_reference   compare chesstb tables with the reference
#   make test              build test_retro and run it
#
# board, retro and refgen include nothing from src/: keeping them apart is what
# lets them check chesstb rather than repeat it. Only check_reference links
# chesstb's prober, to read the tables under test.

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)

CXX       ?= g++
REF_FLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread

REF_SRC := board.cpp retro.cpp
REF_HDR := board.h retro.h
GEN_SRC := $(REF_SRC) refgen.cpp
GEN_HDR := $(REF_HDR) refgen.h

# chesstb's prober and its compression libraries, built here without LTO so this
# does not depend on how the main build was configured.
TB_FLAGS := -std=c++17 -O2 -DNDEBUG -march=native -Wall -Wno-strict-aliasing \
            -Wno-interference-size -Wno-class-memaccess -I$(ROOT)/src -I$(ROOT)/lib -pthread
TB_CXX := $(wildcard $(ROOT)/src/chess/*.cpp) $(wildcard $(ROOT)/src/util/*.cpp) \
          $(wildcard $(ROOT)/src/probe/*.cpp)
TB_C := $(wildcard $(ROOT)/lib/lz4/*.c) $(wildcard $(ROOT)/lib/LZMA/*.c) \
        $(wildcard $(ROOT)/lib/zstd/common/*.c) $(wildcard $(ROOT)/lib/zstd/compress/*.c) \
        $(wildcard $(ROOT)/lib/zstd/dictBuilder/*.c)
TB_OBJ := $(patsubst $(ROOT)/%.c,obj/%.o,$(TB_C)) $(patsubst $(ROOT)/%.cpp,obj/%.o,$(TB_CXX))
TB_HDR := $(shell find $(ROOT)/src $(ROOT)/lib -name '*.h' 2>/dev/null)

.PHONY: all test clean
all: test_retro refgen check_reference

test_retro: test_retro.cpp $(REF_SRC) $(REF_HDR) Makefile
	$(CXX) $(REF_FLAGS) -o $@ test_retro.cpp $(REF_SRC)

refgen: refgen_main.cpp $(GEN_SRC) $(GEN_HDR) Makefile
	$(CXX) $(REF_FLAGS) -o $@ refgen_main.cpp $(GEN_SRC)

obj/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CXX) -O2 -DNDEBUG -march=native -I$(ROOT)/src -I$(ROOT)/lib -x c -c $< -o $@

obj/%.o: $(ROOT)/%.cpp $(TB_HDR)
	@mkdir -p $(dir $@)
	$(CXX) $(TB_FLAGS) -c $< -o $@

check_reference: check_reference.cpp $(GEN_SRC) $(GEN_HDR) $(TB_OBJ) Makefile
	$(CXX) $(TB_FLAGS) -o $@ check_reference.cpp $(GEN_SRC) $(TB_OBJ)

test: test_retro
	./test_retro

clean:
	rm -rf test_retro refgen check_reference obj
