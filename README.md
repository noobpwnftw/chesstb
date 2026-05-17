# chesstb

Chess endgame tablebase generator. Ships three tables per material:

- **WDL** -- 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTC** -- distance-to-conversion (plies to the next zeroing move:
  capture, promotion, or pawn push), 50-move-rule-aware.
- **DTM50** -- distance-to-mate table. A single file packs 101 layers along
  the halfmove-clock axis: layer 0 is the unbounded DTM (distance-to-mate
  ignoring the 50-move rule), and layers 1..100 give the optimal mate
  distance at each halfmove clock, with routes that would bust the 50MR
  window collapsing to DRAW. One probe answers both the no-50MR DTM and
  the 50MR DTM50 at any halfmove clock.

Paging on king-adjacent slices keeps even the largest 8-man tables
buildable well under 4 TB of resident RAM -- `KQRBKQRN`, for instance,
estimates a 2.25 TiB iterate-pass minimum against a 115.5 TiB full
resident footprint.

## Features

**Scale.** Materials far larger than RAM build by paging, and a run can
be parallelized, sized, and stopped at will:

- *Paging.* Slices bundle into groups of at least 64 MiB that load and
  LRU-evict against the `--mem` budget (one cap shared across both STM
  colors), spilling dirty groups to `--tmp`.
- *Throughput.* Generation is multi-threaded (`-t`), sizable up front
  with `--estimate`, and skips already-finished files on re-run.
- *Fleet.* A non-blocking per-material `flock` lets many processes
  share one `--tmp` tree and each claim distinct materials.
- *Interrupt/resume.* `SIGINT` checkpoints each pass independently, so an
  interrupted material resumes mid-pass rather than restarting (see
  [Resume](#resume)).

**Encoding.** Each value maps to a frequency-sorted rank, indexed in 1
byte (≤256 ranks) or 2. WDL packs with LZ4-HC; DTC/DTM/DTM50 rank streams
with LZMA. DTC ranks raw ply distances exactly, halving only cursed/
blessed plies (1-byte tier) -- lossy by up to a ply but harmless, as those
distances are already past the 50-move horizon. DTM halves every value
losslessly via its parity invariant (WIN odd, LOSE even). Decoding stays
within one material: DTC/DTM/DTM50 read class from the material's own WDL
companion, and WDL is self-contained, so a full table decodes from its own
files alone. Sub-tables enter only when generating a material or
reconstructing a dropped STM color (see [Shrink](#shrink)).

**Metadata.** Each `.info` carries symmetry-expanded orbit-weighted
W/D/L/illegal counts per stored color, plus that color's longest win and
its FEN. The orbit weight is each canonical position's true multiplicity
(2 under file-mirror symmetry; 4 or 8 under the pawnless dihedral group,
depending on king-slice stabilizers), so for each color
W + D + L + illegal equals the table's weighted domain total -- a check
that legal and illegal cells exhaust the indexed chess domain.

**Verifiers.** Three layered checks, cheap to exhaustive (see
[Verify](#verify)): a Syzygy WDL cross-check through Fathom, an internal
invariant pass, and a full Bellman fixed-point recomputation.

## Download

Prebuilt 3–6 man tables (510 files each for `wdl/`, `dtc/`, `dtm/`, `dtm50/`):

```
ftp://chessdb:chessdb@ftp.chessdb.cn/pub/chesstb/
rsync://ftp.chessdb.cn/ftp/pub/chesstb/
```

Shipping format (shrunk to one STM color):

| Table   | 3-man | 4-man   | 5-man    | 6-man    | Total    |
|---------|-------|---------|----------|----------|----------|
| `wdl/`  | 5 kB  | 1.2 MB  | 265 MiB  | 43.6 GiB | 43.8 GiB |
| `dtc/`  | 11 kB | 2.9 MB  | 619 MiB  | 89.2 GiB | 89.8 GiB |
| `dtm/`  | 15 kB | 6.5 MB  | 1.68 GiB | 295 GiB  | 297 GiB  |
| `dtm50/`| 28 kB | 21.6 MB | 6.45 GiB | 1095 GiB | 1101 GiB |

`full/` holds the unshrunk tables (both STM colors, ~2×):

| Table   | 3-man | 4-man   | 5-man     | 6-man    | Total    |
|---------|-------|---------|-----------|----------|----------|
| `wdl/`  | 11 kB | 3.1 MB  | 814 MiB   | 134 GiB  | 135 GiB  |
| `dtc/`  | 22 kB | 6.7 MB  | 1.54 GiB  | 233 GiB  | 234 GiB  |
| `dtm/`  | 35 kB | 14.8 MB | 4.02 GiB  | 715 GiB  | 719 GiB  |
| `dtm50/`| 58 kB | 52.0 MB | 16.64 GiB | 2842 GiB | 2859 GiB |

The `.lzdtm50` pack answers both DTM and DTM50, so `dtm50/` alone suffices —
standalone `dtm/` is only a build input, listed for reference.

## Build

```sh
make
make tests
```

The build produces:

- `chesstb`: generator
- `shrink`: postprocessor for shipping-format files

## Generate

```sh
./chesstb -r KQK
./chesstb -r KBNK --builddtm
./chesstb -r KBNK --builddtm50
./chesstb -r KBPK --mem 64 -t 8
./chesstb --enumerate 5 > five.txt
./chesstb --list five.txt
```

Outputs:

- `wdl/<material>.lzw`
- `dtc/<material>.lzdtc`
- `dtc/<material>.info`
- `dtm/<material>.lzdtm`            (with `--builddtm`; also a layer-0 source for `--builddtm50`)
- `dtm/<material>.info`             (with `--builddtm`)
- `dtm50/<material>.lzdtm50`        (with `--builddtm50`)
- `dtm50/<material>.info`           (with `--builddtm50`)

Requested materials are expanded through their capture/promotion
dependency closure and generated in dependency order. The DTC pass always
runs. `--builddtm50` implies the DTM pass (DTM built first, then folded
in), then DTM50; `--builddtm` builds the standalone DTM table on its own.
Existing final files are skipped. The shipped `.lzdtm50` supersedes `.lzdtm`.

## Universal DTM (`.lzdtm50`)

One file, two distance-to-mate answers: the unbounded DTM (no 50-move
rule) and the exact 50MR DTM50 at any halfmove clock.

The 50-move rule turns "distance to mate" into a moving target: a position
that wins-in-200-plies-flat is a 50MR draw, yet with 30 plies left on the
clock it might still win if a zeroing capture lands soon. The pack carries
the clock itself -- each material stores 100 hmc layers (1..100), and a
cell at layer k answers "optimal mate distance given hmc = k?", collapsing
to DRAW where the only winning route would bust the window.

**Unbounded DTM for free.** Along the hmc axis each position's value is
monotone non-decreasing -- `DTM` `≤` `DTM50(hmc=0)` `≤` `…` `≤`
`DTM50(hmc=99)` -- ending in a single flip to DRAW once no line fits. The
unbounded DTM (shortest mate, ignoring the clock) is just one more layer at
the front (pack layer 0), at most one extra change-point per position. So
the pack alone (`dtm50/`, 6.47 GiB) replaces pack-plus-standalone-`dtm/`
(8.15 GiB), dropping the redundant 1.69 GiB DTM table -- 21% off the 3–5
man set. A no-50MR probe reads layer 0; an `hmc = k` probe reads layer
`k + 1`.

Class comes from the WDL companion per layer: **layer 0** keeps
cursed/blessed as decisive; **hmc = 0** folds them to DRAW; **hmc > 0**
also collapses budget-busting routes to DRAW. Where DTM50 and WDL disagree
(a WDL=WIN cell drawn for lack of window) both store 0, and the prober
recovers WIN/LOSS by local move-gen.

**Pack layout.** All 101 layers live in one `dtm50/<material>.lzdtm50`
(layer 0 the unbounded DTM, 1..100 the hmc layers). Since the value is
constant or nearly so for most positions, each is classified per block into
one of four states, packed 2 bits per position:

- **CONST** -- identical at every layer.
- **SINGLE** -- one transition. `[h, r0, r1]`, h ∈ [1, 100].
- **DOUBLE** -- two transitions. `[h1, h2, r0, r1, r2]`.
- **MULTI** -- three or more. `[k, 128-bit changepoint bitmap, k ranks]`.

Each non-CONST state carries an "ends in DRAW" hint bit (piggybacked on the
last h byte's MSB); when set, the trailing rank is dropped and the decoder
synthesizes DRAW -- the dominant terminal pattern for W/L positions. The
rank table holds only frequency-sorted W/L storage values; DRAW and ILLEGAL
are WDL-companion don't-cares that never take a slot, and all-DRAW/ILLEGAL
blocks emit zero bytes.

A probe maps its query to a layer, locates the state via a stride-256
prefix index over the 2-bit vector (built at block-decompress time), and
reads the rank: direct for CONST, a compare for SINGLE/DOUBLE, mask +
popcount for MULTI -- O(STRIDE) walking + O(1) offset math, independent of
block size. Total storage runs roughly an order of magnitude under an
equivalent 100-file-per-material per-hmc layout.

```sh
./chesstb -r KBNK --builddtm50
./chesstb --estimate -r KBNK --builddtm50
./tests/probe_fen --children --rule50 50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 1"
```

## Memory

`--mem MiB` caps resident table bytes across both colors for the current
material. `--mem 0` is unbounded. Positive values page groups through
`--tmp`. The same budget applies to both generation and compression passes.

Use `--estimate` before a large run:

```sh
./chesstb --estimate -r KQRBKQRN
./chesstb --estimate -r KQRBKQNP --builddtm
./chesstb --estimate -r KQRBKQNP --builddtm50
```

The estimate reports total resident-table size and peak group counts for
the init and iterate passes; with `--builddtm`/`--builddtm50` the iterate
peak also unions opponent's pawn-push-target groups (forward reads).

## Resume

Generation is interruptible. `SIGINT` flushes dirty groups, writes a
checkpoint, then exits. Each pass keeps its own checkpoint -- DTC and DTM
record (batch, fusion, ply), DTM50 records (batch, fusion, hmc). Re-run
the same command to resume the in-progress material instead of
restarting. Checkpoints and scratch group files are removed after
`save_to_disk` completes.

## Probe

```sh
./run_probe "8/8/8/5k2/8/8/1Q6/K7 w"
./run_probe --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./run_probe --wdl ./wdl --dtc ./dtc --dtm50 ./dtm50 "8/8/8/8/4k3/8/Q7/K7 w"
./run_probe --rule50 50 --dtm50 ./dtm50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 30"
```

`run_probe` reports WDL, DTC (as `dtz`), DTM, and DTM50 when their tables
are present. DTM and DTM50 both come from the one `.lzdtm50` pack, so a
`--dtm50` directory alone answers both; the standalone `--dtm` table is
only a fallback. It derives the material from the FEN, mirrors to the
canonical table orientation when needed, and honors a legal FEN
en-passant target. DTC/DTM/DTM50 all require the WDL companion to decode
class; the probe gates each field on its companion being available.
`--rule50 N` selects the DTM50 layer; `--children`
threads each child's hmc through (zeroing → 0, quiet → parent+1) so the
per-child DTM50 value matches what the engine would see post-move.

## Verify

Two internal verifiers plus an external cross-check, layered from cheap
to exhaustive:

```sh
./run_compare --enumerate 5
./run_compare --list five.txt
./tests/check_tables --enumerate 5
./tests/check_tables --list five.txt KRRK
./tests/check_fixedpoint KRRK
./tests/check_fixedpoint --enumerate 5
```

- `run_compare` -- disk WDL vs. Syzygy through Fathom for every legal
  canonical position. Requires matching `.rtbw` files under
  `syzygy/` and a Fathom checkout at `lib/Fathom/` (`git clone
  https://github.com/jdart1/Fathom.git lib/Fathom`).
- `check_tables` -- internal-consistency pass. Walks every legal canonical
  position and checks the table's own invariants. Works on **both** full
  and shrunk shipping-format files, so it can verify a shrink in place --
  considerably slower against shrunk files because dropped-STM lookups
  reconstruct by one-ply minimax.
- `check_fixedpoint` -- full Bellman verifier. Recomputes each table value
  from its legal children and compares to disk; this is the exhaustive
  correctness check. DTC and DTM use independent probe instances, so DTM
  validates against the `.lzdtm50` pack even with `dtm/` absent. **Full
  tables only**; shipping-format files with a dropped STM color are rejected.

`make -C tests` builds all four test binaries directly: `probe_fen`,
`compare_syzygy`, `check_tables`, `check_fixedpoint`.

## Shrink

```sh
./shrink wdl dtc dtm dtm50
./shrink wdl/KQK.lzw dtc/KQK.lzdtc dtm/KQK.lzdtm dtm50/KQK.lzdtm50
./shrink --dry-run dtc/*
```

`shrink` rewrites files in place, dropping the larger compressed STM
color when it can be reconstructed at probe time. The probe code detects
dropped colors and rebuilds them by one-ply minimax against the kept
color and sub-TBs (DTM50 threads each child's hmc through the recursion
so per-layer semantics are preserved).

WDL stays self-contained -- it never reads another table. Rebuilding a
dropped WDL frame is a strict one-ply minimax over children: a quiet move
keeps the child in the kept opposite-STM frame of the same material, read
directly. The only WIN-vs-CURSED_WIN ambiguity at one ply is a child
sitting at exactly the last in-rule ply (`dtz == 100`), since the parent
then tips one ply past the 50-move edge. The WDL format reserves two
spare 4-bit codes (`BOUNDARY_WIN`, `BOUNDARY_LOSS`) to mark those edge
positions; every other reader folds them to WIN/LOSE via `wdl_from_storage()`.
Zeroing moves reset the clock, so they cross no edge and need no marker.

Arguments may be individual table files or generated table directories.
Mixed shell globs are safe: non-table files such as `.info` metadata are
skipped.

Shrink is a postprocessing step. The generator does not accept any
dependency on shrunken files -- shipping-format files are treated as
absent and regenerated as full files.

## Layout

```text
src/chess/   board, moves, FEN
src/egtb/    generator (DTC + DTM + DTM50), compression, slicing, paging
src/probe/   standalone probe library
src/shrink/  shipping-format shrinker
src/util/    allocation, threading, compression helpers
tests/       FEN probe, table/fixedpoint verifiers, Syzygy compare
lib/         vendored LZ4, LZMA, zstd
```
