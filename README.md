# chesstb

Chess endgame tablebase generator. Produces four tables per material:

- **WDL** -- 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTC** -- distance-to-conversion (plies to the next zeroing move:
  capture, promotion, or pawn push), 50-move-rule-aware.
- **DTM** -- distance-to-mate, no 50-move-rule. Flat ply count to mate
  across all moves, including captures and promotions into sub-tablebases.
- **DTM50** -- distance-to-mate under the 50-move rule. 100 hmc layers per
  material, one for each value of the halfmove clock, packed into a single
  file via a layer-axis change-point encoding. Each layer answers "given
  this hmc, what's the optimal mate distance?" exactly. Routes that would
  bust the 50MR window collapse to DRAW.

WDL is the projection induced by the DTC table and is written alongside it.
DTM and DTM50 are opt-in (additive after DTC); both depend on the WDL
companion at probe time for class reconstruction.

## Features

- WDL/DTC verified against Syzygy on the full 5-man set and the longest
  pawnless and pawnful 6-man records.
- Comparable 5-man file sizes to Syzygy.
- Pages large materials through disk-backed scratch groups.
- Clean SIGINT interrupt + resume during generation (DTC and DTM each
  checkpoint independently).
- 1-byte ranked tier when ranks fit in 256, 2-byte ranked tier otherwise.

The 2-byte tier stores raw values. For DTC, ordinary win/loss plies are
exact and the 1-byte tier halves only cursed/blessed plies. For DTM the
parity invariant (WIN values odd, LOSS values even) means the 1-byte
tier halves every classified value losslessly -- class is recovered from
the WDL companion at decode time.

The `.info` counts are symmetry-expanded orbit-weighted counts; for each
stored color, W + D + L + illegal equals the table's weighted domain total.
This verifies that legal and illegal cells exhaust the indexed chess domain.

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
./chesstb -r KBPK
./chesstb --enumerate 5 > five.txt
./chesstb --list five.txt
./chesstb -r KQRBKQNP --mem 4096 -t 32
./chesstb -r KBNK --builddtm
./chesstb -r KBNK --builddtm50
```

Outputs:

- `wdl/<material>.lzw`
- `dtc/<material>.lzdtc`
- `dtc/<material>.info`
- `dtm/<material>.lzdtm`            (with `--builddtm`)
- `dtm/<material>.info`             (with `--builddtm`)
- `dtm50/<material>.lzdtm50`        (with `--builddtm50`)
- `dtm50/<material>.info`           (with `--builddtm50`)

Requested materials are expanded through their capture/promotion
dependency closure and generated in dependency order. The DTC pass always
runs; DTM/DTM50 passes run after DTC for each material when the matching
flag is set. Existing final files are skipped.

## DTM50

The 50-move rule turns "distance to mate" into a moving target: a position
that wins-in-200-plies-flat is a 50MR draw, but the same position with 30
plies left on the clock might still win if a zeroing capture lands soon.
Carrying the halfmove clock in the table itself solves that exactly --
each material stores 100 hmc layers, and each cell at layer k answers
"what's optimal mate distance from here, given hmc = k?". Cells whose
only winning route would bust the 50MR window collapse to DRAW at that
layer, so probing returns the actual playable outcome rather than a
pretend value.

The generator builds every layer as a single forward classification pass.
Pawn slices iterate in topo order, and within each slice's fusion the hmc
loop runs 99 down to 0. That order makes every read a finalized read: a
non-pawn quiet at hmc = k targets opp's k+1 in the same slice (just built
one step earlier), an in-material pawn push targets opp's hmc = 0 in a
push-destination slice (already finalized in a prior topo batch), and a
capture or promotion reads the sub-tablebase. hmc = 99 has no k+1, so
its non-pawn quiet reads inline an explicit 50MR-draw / mate check.
hmc = 99 of each fusion also runs the full chess-legality check per cell;
every lower-hmc layer piggybacks ILLEGAL from opp[k+1] at the same idx
and is orders of magnitude faster.

The 5-class WDL companion still does class duty for halved values. At
hmc = 0 there's no ambiguity. At hmc > 0 a WDL=WIN cell might be DRAW
in DTM50 because the only winning route is too long for the remaining
window; both collapse to storage = 0 and the prober recovers WIN(1)/
LOSS(0) by local move-gen when the WDL flags a decisive cell. Cursed
and blessed classes always project to DRAW under DTM50 -- by
construction those cells can't carry mate-in-≤1.

### Pack layout

All 100 hmc layers go into a single `dtm50/<material>.lzdtm50`. The pack
exploits two structural properties of DTM50 across the hmc axis: for
most positions the value sequence is constant or nearly so, and where it
does change it is monotone non-decreasing (tighter 50MR budget can only
make a mate longer) and ends in a single irreversible flip to DRAW once
no winning line fits. Per output block each position is classified into
one of four states, packed as a 2-bit-per-position vector:

- **CONST** -- value identical at every layer (draws, fast mates, mate
  cells). One rank id in a packed const stream.
- **SINGLE** -- exactly one transition. `[h, r0, r1]` where h ∈ [1, 99].
- **DOUBLE** -- two transitions. `[h1, h2, r0, r1, r2]`.
- **MULTI** -- three or more transitions. `[k, 100-bit changepoint
  bitmap, k ranks]`.

Each non-CONST state also carries a per-position "ends in DRAW" hint
bit; when set, the trailing rank is omitted and the decoder synthesizes
DRAW directly. This is the dominant terminal pattern for W/L positions
(run out of 50MR budget -> DRAW) and typically eliminates one rank from
the majority of non-CONST entries. The hint piggybacks on the MSB of
the last h byte (or the k byte for MULTI), free of bandwidth.

A probe at hmc = k locates the position's state via a stride-256 prefix
index over the 2-bit vector (built once at block-decompress time, cached
alongside the block bytes), then follows the state's payload to pick the
rank. CONST is a direct lookup; SINGLE/DOUBLE compare hmc to h/h1/h2;
MULTI does the original mask + popcount over the changepoint bitmap.
Random-access cost is O(STRIDE) state walking + O(1) per-stream offset
math, independent of block size.

The rank table is frequency-sorted W/L storage values only -- DRAW
(storage 0) and ILLEGAL are WDL-companion-authoritative don't-cares
that never occupy a rank slot. The draw-end hint synthesizes DRAW
without a table lookup, and the upstream WDL guard ensures ILLEGAL
never reaches the DTM50 read path. Blocks where every cell across every
layer is DRAW or ILLEGAL emit zero compressed bytes -- a `usz==0`
sentinel in the offset table tells the probe (and the generator-side
flat sub-loader) to fill DRAW directly without paging the block in.
Common for drawn-fortress materials and around large illegal-by-
occupancy regions of the index space.

Total storage is roughly an order of magnitude under the equivalent
100-file-per-material per-hmc layout.

```sh
./chesstb -r KBNK --builddtm50
./chesstb --estimate -r KBNK --builddtm50
./tests/probe_fen --children --rule50 50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 1"
```

## Memory

`--mem MiB` caps resident table bytes across both colors for the current
material. `--mem 0` is unbounded. Positive values page groups through
`--tmp`. The same budget applies to DTC and DTM passes.

Use `--estimate` before a large run:

```sh
./chesstb --estimate -r KQRBKQNP
./chesstb --estimate -r KQRBKQNP --builddtm
./chesstb --estimate -r KQRBKQNP --builddtm50
```

The estimate reports total resident-table size and peak group counts for
init and iterate passes. With `--builddtm`/`--builddtm50`, the iterate
peak unions opponent's pawn-push-target groups (forward reads). DTM50
pins opp[0] and opp[k+1] alongside the current write layer at every step
(3× the per-layer peak); the previous step's k+2 layer is evicted before
the next step starts, so resident layer count stays bounded across the
99 → 0 sweep. Generation can run beyond RAM since storage is split into
load/evict groups and spilled as needed.

## Resume

Generation is interruptible. `SIGINT` flushes dirty groups, writes a
checkpoint, then exits. DTC and DTM record (batch, fusion, ply); DTM50
records (batch, fusion, hmc). Re-run the same command to resume the
in-progress material instead of restarting. DTC, DTM, and DTM50 each
carry their own checkpoint files. Checkpoints and scratch group files
are removed after `save_to_disk` completes.

## Probe

```sh
./run_probe "8/8/8/5k2/8/8/1Q6/K7 w"
./run_probe --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./run_probe --wdl ./wdl --dtc ./dtc --dtm ./dtm "8/8/8/8/4k3/8/Q7/K7 w"
./run_probe --rule50 50 --dtm50 ./dtm50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 30"
```

`run_probe` reports WDL, DTC (as `dtz`), DTM, and DTM50 when their tables
are present. It derives the material from the FEN, mirrors to the
canonical table orientation when needed, and honors a legal FEN
en-passant target. DTC/DTM/DTM50 all require the WDL companion to decode
class; the probe gates each field on its companion being available.
`--rule50 N` selects the DTM50 layer; `--children` threads each child's
hmc through (zeroing → 0, quiet → parent+1) so the per-child DTM50 value
matches what the engine would see post-move.

## Verify

Two internal verifiers plus an external cross-check, layered from cheap
to exhaustive:

```sh
./tests/check_tables --enumerate 5
./tests/check_tables --list five.txt KRRK
./tests/check_fixedpoint KRRK
./tests/check_fixedpoint --enumerate 5
./run_compare --enumerate 5
./run_compare --list five.txt
```

- `check_tables` -- internal-consistency pass. Walks every legal canonical
  position and checks the table's own invariants (DTC: `DRAW=0`, wins
  `>0`, losses zero only at mate, cursed iff `dtc>100`; DTM: wins `>0`,
  losses zero only at mate, WIN odd, LOSE even). Works on **both** full
  and shrunk shipping-format files, so it can verify a shrink in place --
  considerably slower against shrunk files because dropped-STM lookups
  reconstruct by one-ply minimax.
- `check_fixedpoint` -- full Bellman verifier. Recomputes each table value
  from its legal children and compares to disk; this is the exhaustive
  correctness check. **Full tables only**; shipping-format files with a
  dropped STM color are rejected.
- `run_compare` -- disk WDL vs. Syzygy through Fathom for every legal
  canonical position, plus a DTZ cross-check on the longest-win FEN
  against Syzygy's root probe (`+/-1` ply tolerance, since Fathom root DTZ
  is approximate). Requires matching `.rtbw` and `.rtbz` files under
  `syzygy/` and a Fathom checkout at `lib/Fathom/` (`git clone
  https://github.com/jdart1/Fathom.git lib/Fathom`).

`make -C tests` builds all four test binaries directly: `probe_fen`,
`compare_chesstb`, `check_tables`, `check_fixedpoint`.

## Shrink

```sh
./shrink wdl dtc dtm dtm50
./shrink wdl/KQK.lzw dtc/KQK.lzdtc dtm/KQK.lzdtm
./shrink --dry-run dtc/*
```

`shrink` rewrites files in place, dropping the larger compressed STM
color when it can be derived at probe time. The probe code detects
dropped colors and reconstructs them by one-ply minimax against the kept
color and sub-TBs (DTM50 derive threads each child's hmc through the
recursion so per-layer semantics are preserved). WDL, DTC, DTM, and
DTM50 all dispatch through `shrink` by magic; DTC/DTM share the
rank-encoded wire layout while DTM50 uses its own rs-pack header (16-byte
offset entries: `dso` + uncompressed payload size).

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

## Notes

- WDL files use `.lzw`.
- DTC files use `.lzdtc`; DTM files use `.lzdtm`; DTM50 files use `.lzdtm50`.
- DTC scratch groups use `.dtcs`; DTM scratch groups use `.dtms`; DTM50
  scratch groups use `.dtm50s` (all under `--tmp`).
- Building DTM or DTM50 standalone is not supported (both require the
  DTC-produced WDL companion). DTM50 reuses DTC's 5-class WDL --
  cursed/blessed project to DRAW via a 3-line fold at decode.
