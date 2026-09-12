# chesstb

chesstb is a chess endgame tablebase generator that supports five metrics.
Tables can be stored in full or reduced form. Reduced forms include a dropped
side-to-move (STM) frame, loss-only distances, and relaxed WDL or DTZ. See
[Shrink](#shrink) and [Transcribe](#transcribe).

- **WDL:** 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTZ:** distance-to-zeroing (plies to the next capture, promotion, or pawn
  push), accounting for the 50-move rule without class offsets (see
  [Format](#format)).
- **DTC:** distance-to-conversion at any halfmove clock, measured first in pawn
  pushes: how many pushes the winning side still needs before a capture or
  promotion, and how many plies remain until the next zeroing move on that line.
  DTZ is embedded as the unbounded row (see [DTC](#dtc-lzdtc)).
- **DTM:** distance-to-mate, ignoring the 50-move rule.
- **DTM50:** distance-to-mate at any halfmove clock, packed with DTM into one
  file (see [Universal DTM](#universal-dtm-lzdtm50)).

## Download

Available mirrors:

```text
ftp://chessdb:chessdb@ftp.chessdb.cn/pub/chesstb/
rsync://ftp.chessdb.cn/ftp/pub/chesstb/
hf://buckets/noobpwnftw/chesstb/
```

In the tables below, `...` means a build is in progress, `---` means a build is
not planned, and Op1 denotes one [opposing pawn pair](#opposing-pawn-pairs).

`dtm50/` supersedes `dtm/`; for pawnful materials, `dtc/` supersedes `dtz/`.

Default shipping format:

| Material     | `wdl/`   | `dtz/`   | `dtc/`   | `dtm/`   | `dtm50/` |
|--------------|---------:|---------:|---------:|---------:|---------:|
| 3 (Pawnless) |    288 B | 8.03 KiB |      N/A | 8.03 KiB | 9.59 KiB |
| 3 (Pawnful)  | 4.07 KiB | 1.95 KiB | 6.82 KiB | 7.07 KiB | 14.8 KiB |
| 4 (Pawnless) |  212 KiB | 1.87 MiB |      N/A | 2.08 MiB | 7.38 MiB |
| 4 (Pawnful)  |  556 KiB |  522 KiB | 3.09 MiB | 3.69 MiB | 11.8 MiB |
| 5 (Pawnless) |   42 MiB |  245 MiB |      N/A |  389 MiB | 1.93 GiB |
| 5 (Pawnful)  |  160 MiB |  260 MiB | 1023 MiB |  1.3 GiB |  4.4 GiB |
| 6 (Pawnless) | 6.07 GiB | 23.4 GiB |      N/A | 46.6 GiB |  232 GiB |
| 6 (Pawnful)  | 28.4 GiB | 46.9 GiB |  173 GiB |  241 GiB |  839 GiB |
| 7 (Pawnless) |      ... |      ... |      N/A |      --- |      --- |
| 7 (Pawnful)  |      ... |      ... |      --- |      --- |      --- |
| 8 (Op1)      |      ... |      ... |      --- |      --- |      --- |
| Total        | 34.7 GiB | 70.8 GiB |  174 GiB |  289 GiB | 1.05 TiB |

Raw generator output (`full/`):

| Material     | `wdl/`   | `dtz/`   | `dtc/`   | `dtm/`   | `dtm50/` |
|--------------|---------:|---------:|---------:|---------:|---------:|
| 3 (Pawnless) | 1.34 KiB | 16.8 KiB |      N/A | 16.8 KiB | 20.5 KiB |
| 3 (Pawnful)  | 9.51 KiB | 5.01 KiB | 13.8 KiB |   17 KiB | 31.8 KiB |
| 4 (Pawnless) | 1.09 MiB | 4.69 MiB |      N/A | 4.98 MiB | 20.3 MiB |
| 4 (Pawnful)  | 1.83 MiB | 1.68 MiB | 6.79 MiB | 9.11 MiB | 27.2 MiB |
| 5 (Pawnless) |  198 MiB |  731 MiB |      N/A |  992 MiB | 5.53 GiB |
| 5 (Pawnful)  |  616 MiB |  847 MiB | 2.38 GiB | 3.05 GiB | 10.6 GiB |
| 6 (Pawnless) | 21.2 GiB |   71 GiB |      N/A |  122 GiB |  658 GiB |
| 6 (Pawnful)  |  113 GiB |  162 GiB |  438 GiB |  593 GiB | 2.07 TiB |
| 7 (Pawnless) |      --- |      --- |      N/A |      --- |      --- |
| 7 (Pawnful)  |      --- |      --- |      --- |      --- |      --- |
| 8 (Op1)      |      --- |      --- |      --- |      --- |      --- |
| Total        |  135 GiB |  234 GiB |  440 GiB |  719 GiB | 2.73 TiB |

Tables with [castling rights](#castling-rights) are stored under `castling/`:

| Material | `wdl/`   | `dtz/`   | `full/wdl/` | `full/dtz/` |
|----------|---------:|---------:|------------:|------------:|
| 3        |     72 B |    904 B |       200 B |    1.88 KiB |
| 4        | 16.6 KiB |  127 KiB |    92.4 KiB |     310 KiB |
| 5        | 5.77 MiB | 31.1 MiB |    26.3 MiB |    83.7 MiB |
| 6        | 1.27 GiB | 4.71 GiB |    5.11 GiB |    14.1 GiB |
| Total    | 1.28 GiB | 4.74 GiB |    5.13 GiB |    14.2 GiB |

A complete 6-man DTZ shipping set therefore occupies 111.5 GiB, including
castling tables.

## Build

```sh
make
make tools
```

The build produces:

- `chesstb`: generator
- `shrink`: postprocessor for shipping-format files
- `transcribe`: re-encoder for shipping-format files

`make tools` builds the `tools/probe_fen` diagnostic utility. See
[Probe](#probe). The build has no dependencies beyond the vendored sources.

## Generate

```sh
./chesstb -r KQK
./chesstb -r KBPK --builddtc
./chesstb -r KBNK --builddtm
./chesstb -r KBNK --builddtm50
./chesstb -r KBPK --mem 64 -t 8
./chesstb --enumerate 5 > five.txt
./chesstb --list five.txt
```

Outputs:

- `wdl/<material>.lzw`
- `dtz/<material>.lzdtz`
- `dtz/<material>.info`
- `dtc/<material>.lzdtc`            (with `--builddtc`; pawnful materials only)
- `dtc/<material>.info`             (with `--builddtc`; pawnful materials only)
- `dtm/<material>.lzdtm`            (with `--builddtm` or `--builddtm50`)
- `dtm/<material>.info`             (with `--builddtm` or `--builddtm50`)
- `dtm50/<material>.lzdtm50`        (with `--builddtm50`)
- `dtm50/<material>.info`           (with `--builddtm50`)

Options:

| Flag | Meaning |
|------|---------|
| `-r LIST` | comma-separated materials (`-r KQK,KRK`) |
| `--list FILE` | newline-separated materials; `;` and `#` start comments |
| `-t N` | worker threads (default: hardware concurrency) |
| `--mem MiB` | soft cap on resident memory (`0` = unbounded; see [Memory](#memory)) |
| `--cache MiB` | decoded-block cache budget shared by the sub-table probe readers (default: 64; `0` = one block) |
| `--wdl/--dtz/--dtc/--dtm/--dtm50 DIR` | additional search directory for existing tables and sub-tables; repeatable; output always goes to `./wdl/`, `./dtz/`, `./dtc/`, `./dtm/`, or `./dtm50/` |
| `--tmp DIR` | scratch directory for spilled groups and checkpoints (default: `./tmp/`) |
| `--builddtc` | also build DTC for pawnful materials only (see [DTC](#dtc-lzdtc)) |
| `--builddtm` | also build the standalone DTM table |
| `--builddtm50` | also build DTM50 (implies the DTM pass) |
| `--probe` | read DTM/DTM50 sub-tables directly instead of using flat-decompressed temporary files; uses less scratch space but reads more slowly |
| `--fleet` | cooperative multi-process mode: acquire a per-material `flock`, skip materials owned by another worker, and wait rather than abort when sub-tables are not yet ready |
| `--enumerate N` | print canonical material names with ≤ N total pieces |
| `--estimate` | print the working-set estimate and exit |
| `--info PATHS` | dump `.info` files and exit; accepts globs (`--info dtz/*.info`) |

The generator expands each requested material to include its capture and
promotion dependencies, then processes them in dependency order. The DTZ pass
always runs; WDL is projected from its output. `--builddtm50` runs DTM first and
folds it into DTM50. `--builddtm` adds the standalone DTM table without
requesting DTM50.

Existing output is skipped. Each pass checks one output file for completion. For
the DTZ pass, that file is `wdl/<material>.lzw`; delete it to force a rebuild.

## Memory

`--mem MiB` sets a soft cap on resident memory usage; `--mem 0` removes the cap.
Positive values enable paging of groups through `--tmp`. Values below 64 are
raised to 64 because paging requires enough memory for at least one group. The
same budget applies to both generation and compression phases.

Use `--estimate` before a large run:

```sh
./chesstb --estimate -r KQRBKQRN
./chesstb --estimate -r KQRBKQNP --builddtm
./chesstb --estimate -r KQRBKQNP --builddtm50
```

The report first shows the index geometry: position count, full resident size
across both colors, and the pager's slice and group layout. It then gives peak
residency for each pass at three fusion scopes, in groups and bytes:

- **per-dispatch:** one group at a time. This is the minimum usable `--mem`.
- **per-mirror-pair:** one pawn slice and its file mirror for their full
  trajectory.
- **per-batch:** one complete topological batch of pawn slices. This is the
  `--mem 0` peak.

Initialization and building have separate peaks. The build phase holds the
mover's group and the opponent's king-neighbor reach. Initialization writes both
STM colors across the current group and its pawn-push targets, doubling the
memory required for that union.

For pawnful materials, `--builddtc` adds a `dtc per-dispatch` line. DTC
initialization reads pawn-push targets from the next lower budget, so it keeps
two budget layers resident. The build phase stays within one budget layer and
pages like DTZ.

`--builddtm50` adds a `dtm50 per-dispatch` line. This pass pages one table per
halfmove-clock layer. Initialization pages both colors' write layer for the
current group and their layer-0 pawn-push closure. The build phase pages the
mover's layers 0 and `hmc`, plus the opponent's `hmc+1` layer, across the
king-neighbor reach. The reported limit comes from initialization for pawnful
materials and building for pawnless ones.

The packer processes layers one block at a time, and the save cache limits the
worker count to fit the budget; layers need not be resident together.

The minimum memory requirement follows from the index geometry alone, so
`--estimate` quantifies it for each material. Pawnful materials have larger
indexes because they have only file-mirror symmetry, but their pawns give the
pager finer slices. For the pawnless 8-man `KQRBKQRN`, the minimum is 18 groups
(2.25 TiB), compared with a full resident footprint of 115.5 TiB; `--builddtm50`
raises it to 19 groups.

## Resume

`SIGINT` and `SIGTERM` cause the generator to stop at the next safe iteration
boundary, flush its state, write a pass-specific checkpoint under `--tmp`, and
exit with status 130.

`SIGQUIT` instead schedules a clean exit at the next material boundary, where no
checkpoint is needed. `SIGHUP` cancels any pending exit request, including one
from `SIGINT` or `SIGTERM`, provided it has not yet been handled.

To resume from a checkpoint, rerun the same command with the saved state in
place. Delete the checkpoint to start over or before changing `--mem`.

## Probe

```sh
./tools/probe_fen "8/8/8/5k2/8/8/1Q6/K7 w"
./tools/probe_fen --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./tools/probe_fen --wdl ./wdl --dtz ./dtz --dtm50 ./dtm50 "8/8/8/8/4k3/8/Q7/K7 w"
./tools/probe_fen --dtm50 ./dtm50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 30"
./tools/probe_fen --dtc ./dtc "8/8/8/k7/8/8/K4P2/8 w - - 0 1"
```

`probe_fen` reports WDL, DTZ, DTC, DTM, and DTM50 for a FEN. It includes a field
only when the required table is available. DTC is displayed as
`class/order/value`; its class reflects the supplied clock, not the
clock-independent WDL class. See [DTC](#dtc-lzdtc). Every table-directory flag
may be repeated.

The FEN halfmove clock selects the DTM50 layer and DTC budget. A missing or
invalid clock is treated as 0; a clock of 100 or more yields DRAW. `--children`
passes the resulting clock to each child (zeroing → 0, quiet → parent+1).

A FEN with castling rights can be probed only with a table that includes those
rights (see [Castling rights](#castling-rights)). If no such table is available
on disk, the probe reports that no table was found.

`--cache MiB` limits the memory used by decoded blocks across all open tables
(default: 64; `0` = a single block). Blocks share one cache budget and are
evicted in least-recently-used order, so cache memory stays within a fixed
budget as more materials are probed.

## Shrink

```sh
./shrink wdl dtz dtc dtm dtm50
./shrink wdl/KQK.lzw dtz/KQK.lzdtz dtm/KQK.lzdtm dtm50/KQK.lzdtm50
./shrink dtc/KBPK.lzdtc
./shrink --dry-run dtz/*
./shrink --out output wdl/* dtz/* dtc/* dtm/* dtm50/*
```

`shrink` rewrites files in place by default, dropping the larger compressed STM
frame when it can be reconstructed at probe time (see [Format](#format)).

Arguments may be individual table files or generated table directories. Files
are identified by their magic bytes, so mixed shell globs are safe: `.info`
metadata, already-shrunk files, loss-only files (see [Transcribe](#transcribe)),
and non-derivable files are skipped. `-n`/`--dry-run` parses each file and
reports the size it would shrink to without writing anything, then prints a
grand total.

`--out DIR` leaves the inputs untouched and writes results under `DIR` instead.
Since the argument list is usually a glob spanning several table directories,
`DIR` is one flat directory keyed by filename alone (the extension already
distinguishes the table types). Tables that cannot be shrunk are copied there
unchanged; non-table files are not copied.

## Transcribe

```sh
./transcribe --out output --dtz dtz --dtm dtm --dtm50 dtm50 -r KQK,KRK
./transcribe --out output --do-wdl --list five.txt --block 32
./transcribe --out output --do-wdl --relaxed --list five.txt
./transcribe --out output --dtc dtc --loss-only --list five.txt
./transcribe --out output --dtm50 dtm50 --extract-dtm --list five.txt
./transcribe --out output --dtz dtz --relaxed --shrink --fleet --list seven.txt
```

`transcribe` re-encodes a finished table in another shipping format. The source
file contains the classified cells, value histogram, and index permutation, so
the tool can change the block layout, permutation, ranks, and compression
without solving the table again. It writes to `--out` and leaves the source
mapped during the run.

| Flag | Meaning |
|------|---------|
| `-r LIST` / `--list FILE` | materials, as for the generator |
| `--out DIR` | output directory; must differ from the input directory |
| `--wdl DIR` | additional WDL search directory; repeatable (default `./wdl/`) |
| `--do-wdl` | transcribe the WDL table itself |
| `--dtz/--dtc/--dtm/--dtm50 DIR` | transcribe the specified metric from `DIR` |
| `--extract-dtz` | transcribe DTC's embedded DTZ as a standalone table; automatic with `--loss-only` |
| `--extract-dtm` | transcribe DTM50's embedded DTM as a standalone table; automatic with `--loss-only` |
| `--loss-only` | emit loss-only frames (see below) |
| `--shrink` | transcribe both STM frames, then drop one payload when the other frame can reconstruct it |
| `--relaxed` | emit relaxed WDL or DTZ; symmetric materials use loss-only for requested distance tables, while other asymmetric distance types are rejected (see below) |
| `--block KiB` | output block size (default: the metric's default block size; capped at 64 for WDL) |
| `--samples N` | blocks compressed per permutation candidate (default: the metric's default sample count) |
| `-t N` | worker threads |
| `--cache MiB` | decoded-block budget shared by the sources (default: 64) |
| `--fleet` | cooperate through per-material locks and skip finished output |
| `--tmp DIR` | scratch directory for spilled blocks |

A dropped frame has no payload and passes through unchanged. A distance table
needs its WDL companion for the class of every color being re-encoded. Use the
full tables as input when a reduced source lacks a required frame.

### Loss-only

`--loss-only` writes distance tables that store distances only for loss-class
cells. Win values become don't-cares. The prober reconstructs them with the same
one-ply minimax used for a dropped color. WDL is never loss-only because it
provides the classes used during reconstruction.

Loss-only storage and an STM drop cannot be combined: the dropped color's losses
could depend on omitted wins. `shrink` therefore skips loss-only files, and
`transcribe` rejects converting a dropped source to loss-only. Symmetric
materials are exempt because their second frame mirrors the first.

Dropping an STM frame keeps the smaller frame; loss-only storage must keep every
loss and is usually larger when a frame can be dropped. Loss-only storage is
useful when dropping a frame saves no space. Its probe reconstructs only wins;
reconstructing losses would require examining every child.

Both formats contain an unbounded row: DTM for DTM50 and DTZ for DTC. A
loss-only conversion writes the matching loss-only companion and reuses its
layout.

### Relaxed

Relaxation treats selected cells as don't-cares. WDL and DTZ use different rules
to select them. Relaxed tables may also drop an STM frame; `shrink` preserves
the relaxed flag. Relaxed WDL cannot be transcribed again. Relaxed DTZ can be
transcribed with `--relaxed` or `--loss-only` because neither reads the freed
values.

#### WDL

**Bound.** For every cell in a relaxed frame, the probe calculates the best
class reachable by a capture or promotion. It reads that class from the child
table and returns the maximum of the bound and the stored code. These moves
always change material, so the child belongs to a sub-table and reconstruction
cannot re-enter the frame being read.

**Slack.** If the bound reaches the cell's class, the cell may store any lower
code; otherwise it stores its class. Taking the maximum recovers both cases
without a per-cell marker.

Classes rank `LOSE` < `BLESSED_LOSS` < `DRAW` < `CURSED_WIN` < `WIN`, and the
slack is the range of codes below a cell's own class. This provides flexibility
when encoding wins and draws:

| Cell holds | Bound reaches | Cell may store |
|------------|---------------|----------------|
| `WIN` | `WIN`, by a winning capture | any of the five codes |
| `DRAW` | `DRAW`, by a drawing capture | `LOSE`, `BLESSED_LOSS`, `DRAW` |
| `LOSE` | -- | nothing: it is already the bottom |

For example, a `KQKR` win by rook capture reaches a `KQK` WIN, so its stored
cell can use any class code to improve compression. Boundary markers are not
relaxed because dropped-frame reconstruction reads them directly.

**Encoding.** The encoder tries two fills for freed cells and keeps the smaller
compressed block. **Run-stitch** extends an adjacent run, which works well for
locally constant values. **relz** continues an earlier occurrence of the last
four committed bytes, the minimum LZ4 match, which works well for strided data.

Transcription needs full one-move sub-tables. The permutation search uses the
capped cells. Relaxed WDL cannot be processed twice because the second pass
would measure caps against the first pass's fill.

#### DTZ

A winning DTZ cell is 1 if a zeroing move preserves its WDL class. The
transcriber omits such values; the probe checks zeroing moves first, returning 1
on a class-preserving move and the decoded value otherwise. No per-cell marker
is needed.

A zeroing move is a capture, a promotion, or any pawn move. The check needs only
the resulting WDL class. Captures and promotions read a sub-table, while an
ordinary pawn move reads the current material's WDL table. For a double push,
the effective class also accounts for any legal en-passant reply. The check
never reads a child DTZ value, so pawn pushes do not cause recursive DTZ probes.

Losses cannot use this rule because the losing side maximizes DTZ; one zeroing
move does not determine the final value. A quiet mate in one also has DTZ 1, but
recognizing it requires examining the child position. These values remain
stored.

Transcription requires the full WDL closure for its zeroing moves and both of an
asymmetric pawn material's own WDL frames. The DTZ source may already be relaxed
or have an STM frame dropped. Its WDL companion must have neither reduction
because it supplies the class of every cell.

Relaxed DTZ omits only wins recoverable by a local WDL test, so it can coexist
with an STM drop. Full loss-only storage cannot be combined with an STM drop for
asymmetric materials. Symmetric materials have one physical frame whose stored
losses allow every win to be derived, so `--relaxed` emits loss-only distance
tables for them. On asymmetric materials, `--relaxed` supports only WDL and DTZ;
other distance metrics are rejected.

## Format

Positions map to a storage index built for paging: king and pawn dimensions
carry the slice structure, so the compressor's permutation search is restricted
to the remaining piece classes (identical pieces within a class are
combinationally ranked), and gaps are skipped at block granularity rather than
compacted into a denser index. The chosen permutation is stored per color in the
file header.

WDL stores a 4-bit class code per position, with two codes per byte, compressed
using LZ4-HC and a dictionary trained on the table itself (the dictionary is
omitted for tables with fewer than 256 blocks). Distance tables map each value
to a frequency-sorted rank, encoded in 1 byte (≤256 ranks) or 2 bytes, and
compress those rank streams with LZMA. DTZ ranks raw ply distances exactly,
halving only cursed/blessed plies in the 1-byte tier. This can lose up to one
ply but does not affect play because those distances are past the 50-move
horizon. DTM halves every value losslessly via its parity invariant (WIN odd,
LOSE even).

Blocks compress independently. WDL uses 64 KiB blocks; DTZ, DTC, DTM, and DTM50
use 1 MiB blocks. A per-color offset section locates each block.

Each `.info` file contains symmetry-expanded, orbit-weighted W/D/L/illegal
counts per stored color, plus that color's longest win and its FEN. The orbit
weight is each canonical position's true multiplicity (2 under file-mirror
symmetry; 4 or 8 under the pawnless dihedral group, depending on king-slice
stabilizers), so for each color W + D + L + illegal equals the table's weighted
domain total. Since WDL is projected from the DTZ pass, it has no `.info` file
of its own; its counts are included in the DTZ `.info` file.

Decoding stays within one material: DTZ/DTC/DTM/DTM50 read classes from the
material's own WDL companion, and WDL needs no companion, so a full table
decodes from its own files in O(1): one block read and one index lookup, plus
one extra probe when the FEN carries en-passant rights. Sub-tables are needed
only when generating a material or probing a dropped (see [Shrink](#shrink)),
loss-only, or relaxed frame.

Each color's frame begins with a flag byte: `0x80` indicates one value for the
whole domain, `0x40` a dropped frame, `0x20` losses only, and `0x10` a relaxed
frame (see [Transcribe](#transcribe)). The latter three require the prober to
reconstruct cells, so the generator does not accept them as sub-tables and
regenerates the tables instead.

A table with one STM frame dropped reconstructs it using one-ply minimax against
the retained frame. A quiet move leaves the child in the retained opposite-STM
frame of the same material, which is read directly. Only captures and
promotions, which change material, require a sub-table. DTM50 passes each
child's halfmove clock through the recursion to preserve the semantics of each
layer. For a win, DTC uses each child's order/value pair. For a loss, it
combines results across all push budgets and settles on the budget forced by the
most persistent defense.

A loss-only frame uses the same minimax to reconstruct a win, completing the
reconstruction after one ply. Every useful child is a stored loss for that
child's side to move. Losses are read directly. Reconstructing a win still
examines every move for the argmin, but it needs distances only from children
that preserve the win; reconstructing a loss would need a distance from every
child.

For WDL, the only ambiguity between WIN and CURSED_WIN arises when a child is at
the last ply allowed by the rule (`dtz == 100`) and its parent crosses the
50-move boundary. Two spare 4-bit codes (`BOUNDARY_WIN`, `BOUNDARY_LOSS`) mark
those positions; every other reader maps them to WIN/LOSE. Zeroing moves reset
the clock, so they do not cross this boundary and need no marker.

## Universal DTM (`.lzdtm50`)

An `.lzdtm50` file provides both unbounded DTM and exact 50-move-rule DTM50 for
any halfmove clock.

The 50-move rule makes DTM depend on the halfmove clock. Each material stores a
layer for clocks 0 through 99, containing the optimal mate distance or DRAW if
no winning line reaches a zeroing move in time.

**Unbounded DTM.** Along the halfmove-clock axis, each position's value is
monotonically non-decreasing:

`DTM ≤ DTM50(hmc=0) ≤ … ≤ DTM50(hmc=99)`

The sequence ends in DRAW once no winning line fits within the remaining clock.
Packed layer 0 adds unbounded DTM, introducing at most one extra change point
per position; clock `k` uses layer `k + 1`. `transcribe --extract-dtm` writes
layer 0 as a standalone table.

Each layer derives its class from the WDL companion: **layer 0** treats
cursed/blessed results as decisive; **hmc = 0** maps them to DRAW; **hmc > 0**
also maps routes that exceed the remaining clock to DRAW. Where DTM50 and WDL
disagree (a WDL=WIN cell is drawn because too few plies remain), the draw-end
hint records the difference.

**Pack layout.** All 101 layers are stored in one `dtm50/<material>.lzdtm50`
file (layer 0 contains unbounded DTM; layers 1..100 contain the halfmove-clock
layers). Since the value is constant or nearly so for most positions, each
position is classified within its block into one of four states, using 2 bits
per position:

- **CONST:** identical at every layer.
- **SINGLE:** one transition, encoded as `[h, r0, r1]`, where h ∈ [1, 100].
- **DOUBLE:** two transitions, encoded as `[h1, h2, r0, r1, r2]`.
- **MULTI:** three or more transitions, encoded as
  `[k, 128-bit changepoint bitmap, k ranks]`.

Each non-CONST state has an "ends in DRAW" bit in the most significant bit of
the last `h` byte (or MULTI's `k` byte). When set, this bit allows the trailing
rank to be omitted and tells the decoder to return DRAW. Rank tables contain
only frequency-sorted W/L values; DRAW and ILLEGAL use no rank, so
all-DRAW/ILLEGAL blocks have no payload.

**Embedded DTZ.** The final change point to DRAW encodes DTZ. For a W/L position
with `2 ≤ dtz ≤ 100`, the 50-move-rule result changes when zeroing no longer
fits within the remaining clock, at `h = 102 - dtz`. The endpoints require
special handling: `h = 1` indicates a position already drawn at a fresh clock,
identifying the cursed/blessed band without specifying a distance. Positions
with `dtz ≤ 1` never change. In that range, `dtz = 0` means checkmate, which
move generation distinguishes from `dtz = 1`. The prober decodes the transition
alongside the value and combines it with the layer using minimax, so the packed
table provides DTZ everywhere except in the cursed/blessed band.

A stride-256 prefix index over the 2-bit vector locates each state. Rank lookup
is direct for CONST, uses comparisons for SINGLE/DOUBLE, and uses a mask plus
popcount for MULTI: O(STRIDE) scanning and O(1) offset calculation. All 101
layers require less than 4× the storage of one layer, or about one-twentieth the
storage of 100 separate halfmove-clock tables.

```sh
./chesstb -r KBNK --builddtm50
./chesstb --estimate -r KBNK --builddtm50
./tools/probe_fen --children "8/8/8/4k3/8/8/Q7/K7 w - - 50 1"
```

## DTC (`.lzdtc`)

DTC measures distance-to-conversion, counting pawn pushes separately from
waiting moves. A DTC probe returns two numbers for the supplied halfmove clock:

- **order:** the number of pawn pushes the winning side still needs before a
  *conversion* (a capture or a promotion). A push is irreversible progress that
  is not yet a conversion; the count resets at every conversion.
- **value:** the number of plies to the next zeroing move on that line, the same
  quantity measured by DTZ. At order 0, the next zeroing move *is* the
  conversion; at higher orders, it is the first of the remaining pawn pushes.

Order is optimized before value: a conversion without a pawn push beats a faster
line that uses one. DTZ cannot express this because every pawn move has value 1.
With two pawns against a king, DTZ may advance both in ten pushes; DTC can use a
double push and promote one in four pushes.

**Push budgets.** Using fewer pushes can require more waiting moves. The table
therefore stores layers indexed by push budget. Layer *k* allows the winning
side to use at most *k* pawn pushes. Within a layer, values use DTZ semantics. A
winning-side pawn push moves to the next lower layer; a losing-side push stays
in the same layer.

The probe selects the smallest push budget whose waiting distance fits `rule50`.
If none fits, it returns DRAW. DTC therefore reports its own class: `WIN` or
`LOSE` with an order/value pair, or `DRAW`. This is separate from
clock-independent WDL.

Layers store only results settled by the 50-move rule. A win that exceeds the
current clock belongs to a larger push budget; unresolved results use the
unbounded row. Classes always come from the WDL companion.

Storage uses the same change-point format as
[Universal DTM](#universal-dtm-lzdtm50): budgets count down from the largest, so
the unbounded row sits at packed index 0 and each cell's DRAW run appears at the
end, where the draw-end hint encodes it. Most cells are CONST because their
optimal line has no pawn push.

Thirty rows suffice: an 8-man position gives the winner at most six pawns, each
with at most five non-converting pushes. For a W/L cell, embedded DTZ supplies
the final change point, leaving at most 29 to solve. Cursed/blessed cells have
no finite-budget endpoint; row 0 supplies DTZ and DTC reports DRAW.

**The unbounded row is the DTZ table.** As with DTM inside DTM50, generation
reads DTZ from `dtz/` instead of solving it again, stores its raw ply values,
and reuses its layout. One `.lzdtc` file therefore answers both metrics,
including the cursed band. A pawnful material does not need a separate `dtz/`
file, and `transcribe --extract-dtz` can reproduce it byte for byte.

**Pawnful only.** Without pawn pushes to budget, the stack has just one layer,
and every zeroing move is already a conversion. A pawnless pack would therefore
duplicate the DTZ table.

## Opposing pawn pairs

Tables for materials with pawns on both sides grow quickly because each free
pawn adds an independent dimension of about 48 squares. Many positions contain
an *opposing* pawn pair: one white and one black pawn on the same file, with
white below black and neither able to pass the other. Such a pair has only 120
joint placements (white on ranks 2..6, black strictly above on ranks 3..7 → 15
rank pairs × 8 files), compared with approximately 2,304 placements for a free
white/black pawn pair. An **opposing-pair table** indexes the pair as a single
120-entry dimension.

**Naming.** Lowercase `'p'` denotes the pair and appears once on each side to
show that both colors have a pawn. `KQpKp` represents white K+Q plus the pair's
white pawn, and black K plus the pair's black pawn. Its physical material is
`KQPKP`. At most one pair is allowed per material.

**Size reduction.** The pair pawns remain ordinary board pieces. Only their
indexing changes, so a position has the same value in either table.

| Pair table | positions      | resident | full material | positions       | resident |
|------------|---------------:|---------:|---------------|----------------:|---------:|
| `KpKp`     | 216,720        |  847 KiB | `KPKP`        | 4,074,336       | 15.5 MiB |
| `KQpKp`    | 13,870,080     | 52.9 MiB | `KQPKP`       | 260,757,504     |  995 MiB |
| `KRPpKRp`  | 39,649,935,360 |  148 GiB | `KRPPKRP`     | 383,835,045,888 | 1.40 TiB |

The size reduction is approximately 19× when the pair is the only pawn
structure, and smaller when free pawns remain alongside it. For example,
`KRPpKRp` retains a free white pawn, giving a reduction of approximately 9.7×.

**Domain.** Pair pawns are indistinguishable from free pawns on the board, so
the indexed pair is the first in lexicographic order by file and pawn ranks.
Other cells are pruned, preserving a unique board-to-index mapping.

**Transitions.** Non-capturing moves by a pair pawn stay in the same `'p'`
material. Captures leave it with one fewer piece:

- **p → PP:** any capture of a free piece, whether the captor is a free piece or
  a pair member (diagonally or en passant). Both members join the child as free
  pawns, so the child is the full free-pawn material minus the captured piece.
  From `KQpKp`, that is `KPKP`.
- **p → P:** a pair member is itself captured (including en passant after a
  double push). The surviving member becomes a free pawn. From `KQpKp`, that is
  `KQPK` or `KQKP`.

A capture resolves into a **full** sub-table, converting any surviving pair
pawns to free pawns. `KQpKp` therefore depends on `KPKP`, `KQPK`, `KQKP`, and
their closures, but not the equally sized `KQPKP`. Only `KQpKp` itself is a pair
table in its 26-configuration plan.

A free-pawn promotion preserves the pair, so `KPpKp` depends on
`KQpKp`/`KRpKp`/`KBpKp`/`KNpKp`. A table without free pawns, such as `KQpKp`,
has no pair-table dependency.

**Opt-in.** `--enumerate N` never emits pair materials; they exist only when you
request one by name. Generation, `--estimate`, `--mem` paging,
`--builddtm`/`--builddtm50`, and `shrink` treat a `'p'` material like any other.

```sh
./chesstb --estimate -r KQpKp
./chesstb -r KQpKp --builddtm50
./tools/probe_fen --wdl ./wdl "8/8/8/8/1p6/1P6/3k4/K6Q w"
```

## Castling rights

A piece-only table cannot distinguish positions with castling rights. A
**castling table** indexes the rights with the pieces. Lowercase `'r'` denotes a
rook that retains castling rights, together with its king. `KrK` has the same
physical material as `KRK`. `KrrK` represents an unmoved king with both rooks on
their starting squares, and `KrrKrr` represents that configuration for both
sides. Each side may have at most two rights. Starting files and each rook's
side of the king are index dimensions, not part of the material name. One table
covers every arrangement, including the general Chess960/DFRC case.

The constrained king and rooks form one 56-entry dimension per side instead of
separate 64-square dimensions. Thus `KrKr` has 56 × 56 = 3,136 placements.

Castling rights can only decrease, so a castling table depends on its
counterpart without castling rights. A king move, castling move, or rook move
turns `r` into an ordinary `R`: `KrrK` becomes `KRrK` or `KRRK`. Capturing the
rook removes its right, so `KrrK` becomes `KrK`. A move can do both; a rook
capturing the enemy rook along its file leaves `KRK`.

`--enumerate N` never emits castling materials; they must be requested by name.
Generation, `--estimate`, paging, the `--build*` passes, `shrink`, and
`transcribe` treat an `'r'` material like any other.

```sh
./chesstb --estimate -r KrrKrr
./chesstb -r KrK --builddtm --builddtm50 --builddtc
./tools/probe_fen --wdl ./wdl --dtz ./dtz "8/8/8/8/8/7k/8/KR6 w K -"
```

## Layout

```text
src/chess/      board, moves, FEN, indexing primitives
src/egtb/       generators, compression, slicing, paging
src/probe/      standalone probe library
src/shrink/     shipping-format shrinker
src/transcribe/ shipping-format re-encoder
src/system/     platform shims
src/util/       allocation, threading, compression helpers
tools/          diagnostic tools
lib/            vendored LZ4, LZMA, zstd (xxHash and dict trainer)
```
