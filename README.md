# chesstb

Chess endgame tablebase generator. Produces three tables per material:

- **WDL** — 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTC** — distance-to-conversion (plies to the next zeroing move:
  capture, promotion, or pawn push), 50-move-rule-aware.
- **DTM** — distance-to-mate, no 50-move-rule. Flat ply count to mate
  across all moves, including captures and promotions into sub-tablebases.

WDL is the projection induced by the DTC table and is written alongside it.
DTM is opt-in (additive after DTC) and depends on the WDL companion at
probe time for class reconstruction.

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
tier halves every classified value losslessly — class is recovered from
the WDL companion at decode time.

The `.info` counts are symmetry-expanded orbit-weighted counts; for each
stored color, W + D + L + illegal equals the table’s weighted domain total.
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
```

Outputs:

- `wdl/<material>.lzw`
- `dtc/<material>.lzdtc`
- `dtc/<material>.info`
- `dtm/<material>.lzdtm`    (with `--builddtm`)
- `dtm/<material>.info`     (with `--builddtm`)

Requested materials are expanded through their capture/promotion
dependency closure and generated in dependency order. The DTC pass always
runs; the DTM pass runs after DTC for each material when `--builddtm` is
set. Existing final files are skipped. Shrunk shipping-format files are
treated as absent and regenerated as full files.

## Memory

`--mem MiB` caps resident table bytes across both colors for the current
material. `--mem 0` is unbounded. Positive values page groups through
`--tmp`. The same budget applies to DTC and DTM passes.

Use `--estimate` before a large run:

```sh
./chesstb --estimate -r KQRBKQNP
./chesstb --estimate -r KQRBKQNP --builddtm
```

The estimate reports total resident-table size and peak group counts for
init and iterate passes. With `--builddtm`, the iterate peak unions
opp's pawn-push-target groups (DTM's `PAWN_EVAL` reads them forward).
Generation can run beyond RAM since storage is split into load/evict
groups and spilled as needed.

## Resume

Generation is interruptible. `SIGINT` flushes dirty groups, writes a
checkpoint with the current batch, fusion, phase, and ply, then exits.
Re-run the same command to resume the in-progress material instead of
restarting it. DTC and DTM each carry their own checkpoint files; the
DTC pass restarts independently of DTM. Checkpoints and scratch group
files are removed after `save_to_disk` completes.

## Compare And Probe

```sh
./run_compare --enumerate 5
./run_compare --list five.txt
./run_probe "8/8/8/5k2/8/8/1Q6/K7 w"
./run_probe --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./run_probe --wdl ./wdl --dtc ./dtc --dtm ./dtm "8/8/8/8/4k3/8/Q7/K7 w"
```

`run_compare` compares generated WDL against Syzygy through Fathom. It
expects matching Syzygy `.rtbw` files under `syzygy/`.

`run_probe` reports WDL, DTC (as `dtz`), and DTM (when present). It derives
the material from the FEN, mirrors to the canonical table orientation when
needed, and honors a legal FEN en-passant target. DTC and DTM both require
the WDL companion to decode class; the probe gates each field on its
companion being available.

The test binaries can also be built directly:

```sh
make -C tests
```

## Shrink

```sh
./shrink wdl/KQK.lzw dtc/KQK.lzdtc dtm/KQK.lzdtm
```

`shrink` rewrites files in place, dropping the larger compressed STM
color when it can be derived at probe time. The probe code detects
dropped colors and reconstructs them by one-ply minimax against the kept
color and sub-TBs. The DTC and DTM wire layouts are identical, so a
single rank-encoded shrinker handles both.

## Layout

```text
src/chess/   board, moves, FEN
src/egtb/    generator (DTC + DTM), compression, slicing, paging
src/probe/   standalone probe library
src/shrink/  shipping-format shrinker
src/util/    allocation, threading, compression helpers
tests/       Syzygy compare and FEN probe tools
lib/         vendored LZ4, LZMA, zstd
```

## Notes

- WDL files use `.lzw`.
- DTC files use `.lzdtc`; DTM files use `.lzdtm`.
- DTC scratch groups use `.dtcs`; DTM scratch groups use `.dtms` (both
  under `--tmp`).
- Building DTM standalone is not supported (requires DTC produced WDL).
