# chesstb

Chess endgame tablebase generator. Produces four metrics in three shipping
formats:

- **WDL** -- 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTC** -- distance-to-conversion (plies to the next zeroing move:
  capture, promotion, or pawn push), 50-move-rule-aware.
- **DTM** -- distance-to-mate, ignoring the 50-move rule.
- **DTM50** -- distance-to-mate at any halfmove clock, packed with DTM into
  one file (see [Universal DTM](#universal-dtm-lzdtm50)).

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
- *Interrupt/resume.* `SIGINT` checkpoints each pass, so a material resumes
  mid-pass (see [Resume](#resume)).
- *Frozen pawn pairs.* An opt-in `'p'` material indexes a blocked pawn pair
  as one 120-entry dimension (see [Frozen pawn pairs](#frozen-pawn-pairs)).

**Verifiers.** Three layered checks, cheap to exhaustive (see
[Verify](#verify)): a Syzygy WDL cross-check through Fathom, an internal
invariant pass, and a full Bellman fixed-point recomputation.

## Download

Prebuilt 3–6 man tables, 510 materials each. `dtm50/` answers both DTM and
DTM50; `dtm/` answers unbounded DTM alone at roughly a quarter the size, for
when the clock-aware layers are not needed.

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

`full/` holds the unshrunk tables (both STM colors, ~2.5×: shrink drops the
*larger* compressed color, so the survivor is the smaller half):

| Table   | 3-man | 4-man   | 5-man     | 6-man    | Total    |
|---------|-------|---------|-----------|----------|----------|
| `wdl/`  | 11 kB | 3.1 MB  | 814 MiB   | 134 GiB  | 135 GiB  |
| `dtc/`  | 22 kB | 6.7 MB  | 1.54 GiB  | 233 GiB  | 234 GiB  |
| `dtm/`  | 35 kB | 14.8 MB | 4.02 GiB  | 715 GiB  | 719 GiB  |
| `dtm50/`| 58 kB | 52.0 MB | 16.64 GiB | 2842 GiB | 2859 GiB |

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

Options:

| Flag | Meaning |
|------|---------|
| `-r LIST` | comma-separated materials (`-r KQK,KRK`) |
| `--list FILE` | newline-separated materials; `;` and `#` start comments |
| `-t N` | worker threads (default: hardware concurrency) |
| `--mem MiB` | resident table cap per material, `0` = unbounded (see [Memory](#memory)) |
| `--cache MiB` | decoded-block cache budget shared by the sub-table probe readers (default 64, `0` = one block) |
| `--wdl/--dtc/--dtm/--dtm50 DIR` | additional directory searched for existing tables and sub-tables, repeatable; output always goes to `./wdl/`, `./dtc/`, `./dtm/`, `./dtm50/` |
| `--tmp DIR` | scratch directory for spilled groups and checkpoints (default `./tmp/`) |
| `--builddtm` | also build the standalone DTM table |
| `--builddtm50` | also build DTM50 (implies the DTM pass) |
| `--probe` | read DTM/DTM50 sub-tables through the direct probe reader instead of flat-decompressed temp files -- less scratch space, slower reads |
| `--fleet` | cooperative multi-process mode: take a per-material `flock`, skip materials another worker owns, and wait rather than abort on not-yet-ready sub-tables |
| `--enumerate N` | print canonical material names with ≤ N total pieces |
| `--estimate` | print the working-set estimate and exit |
| `--info PATHS` | dump `.info` files and exit; accepts globs (`--info dtc/*.info`) |

Requested materials are expanded through their capture/promotion
dependency closure and generated in dependency order. The DTC pass always
runs, and WDL is a projection of its output. `--builddtm50` implies the DTM
pass (DTM built first, then folded in), then DTM50; `--builddtm` builds the
standalone DTM table on its own. Existing final files are skipped, with each
pass gated on a single output -- the DTC pass on `wdl/<material>.lzw`, so
deleting that file is what forces a rebuild.

## Memory

`--mem MiB` caps resident table bytes across both colors for the current
material. `--mem 0` is unbounded. Positive values page groups through
`--tmp`, and anything below 64 is raised to 64, since a budget under one
group cannot page. The same budget applies to both generation and
compression passes.

Use `--estimate` before a large run:

```sh
./chesstb --estimate -r KQRBKQRN
./chesstb --estimate -r KQRBKQNP --builddtm
./chesstb --estimate -r KQRBKQNP --builddtm50
```

The report opens with the index geometry -- position count, full resident
size across both colors, and the slice/group layout the pager works in
(bytes per slice, slices bundled per group, bytes per group, and the slice
and group totals). Then it gives peak residency for each pass at three
fusion scopes, in groups and in bytes:

- **per-dispatch** -- one group processed at a time. This is the floor:
  the smallest `--mem` a run can be squeezed into.
- **per-mirror-pair** -- one pawn slice plus its file mirror held resident
  for its whole trajectory. (Unrelated to a
  [frozen pawn pair](#frozen-pawn-pairs).)
- **per-batch** -- a whole topological batch of pawn slices fused. This is
  the `--mem 0` ceiling.

The two passes reach differently, so their peaks are computed separately.
Iterate holds the dispatched group plus everything its king-neighbour reach
touches; init holds the group plus the pawn-push-target groups it reads
forward -- a one-step closure iteration never leaves the king-neighbour
reach for. Both keep both STM colors resident, but not the same way: iterate
splits them, the mover's group against the opponent's reach, while init
writes both colors over one group set, so its peak is twice that group's
me-and-push union.

`--builddtm50` adds a `dtm50 per-dispatch` line. That pass pages one table
per hmc layer, so its floor is a fixed offset off the peaks above: init pages
both colors' write layer at the group plus their layer-0 push closure,
iterate the mover's layers 0 and `hmc` plus the opponent's `hmc+1` across the
king-neighbour reach. The line names whichever binds -- init on pawn
materials, iterate on pawnless ones. The layers never need to be
co-resident: the packer sweeps them per block into a scratch buffer, and the
save cache throttles worker count to the budget instead of widening.

The floor follows from the index geometry alone, so `--estimate` quantifies
it per material: pawn materials index larger, having only file-mirror
symmetry, but their pawns give the pager finer slices. For the pawnless
8-man `KQRBKQRN` the floor is 18 groups (2.25 TiB) against a 115.5 TiB full
resident footprint; `--builddtm50` makes it 19.

## Resume

Generation is interruptible. `SIGINT` flushes dirty groups, writes a
checkpoint, then exits. Each pass keeps its own checkpoint -- DTC and DTM
record (batch, fusion, ply), DTM50 records (batch, fusion, hmc). Re-run
the same command to resume the in-progress material instead of
restarting.

## Probe

```sh
./run_probe "8/8/8/5k2/8/8/1Q6/K7 w"
./run_probe --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./run_probe --wdl ./wdl --dtc ./dtc --dtm50 ./dtm50 "8/8/8/8/4k3/8/Q7/K7 w"
./run_probe --dtm50 ./dtm50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 30"
```

`run_probe` reports WDL, DTC (as `dtz`), DTM, and DTM50 when their tables
are present; a `--dtm50` directory answers both DTM and DTM50, and `--dtm`
is read when the pack is absent. Each table-directory flag may be repeated
to add search locations. It derives the material from the FEN, mirrors to the
canonical table orientation when needed, and honors a legal FEN
en-passant target. DTC/DTM/DTM50 all require the WDL companion to decode
class; the probe gates each field on its companion being available.
The FEN halfmove clock selects the DTM50 layer (0 when the field is absent
or unparsable, DRAW at 100 and above); `--children` threads each child's hmc
through (zeroing → 0, quiet → parent+1) so the per-child DTM50 value matches
what the engine would see post-move, and `--limit N` caps how many children
are printed.

`--cache MiB` bounds the decoded blocks held resident across every open
table (default 64, `0` = a single block). Blocks are reclaimed
least-recently-used from one shared budget, so probing many materials
costs a fixed amount instead of growing per material. The same flag is on
`check_tables`, `check_fixedpoint`, and `compare_syzygy`.

## Verify

Two internal verifiers plus an external cross-check, layered from cheap
to exhaustive:

```sh
./run_compare --enumerate 5
./run_compare --list five.txt
./tests/check_tables --enumerate 5
./tests/check_tables --list five.txt KRRK
./tests/check_tables --checksum-only --enumerate 5
./tests/check_fixedpoint KRRK
./tests/check_fixedpoint --enumerate 5
```

- `run_compare` -- disk WDL vs. Syzygy through Fathom for every legal
  canonical position. Requires matching `.rtbw` files under
  `syzygy/` and a Fathom checkout at `lib/Fathom/` (`git clone
  https://github.com/jdart1/Fathom.git lib/Fathom`).
- `check_tables` -- internal-consistency pass. Walks every legal canonical
  position and checks the table's own invariants, including one cross-metric
  identity: DTC pins the clock at which DTM50 flips to DRAW, so a W/L cell
  must still be decisive at `hmc = 100 - dtc` and drawn one tick later. Works
  on **both** full and shrunk shipping-format files, so it can verify a shrink
  in place -- considerably slower against shrunk files, since a full walk
  visits every dropped-STM position and so derives each one by one-ply minimax.
  `--checksum-only` just opens each table to run its file checksums and
  skips the scan.
- `check_fixedpoint` -- full Bellman verifier. Recomputes each table value
  from its legal children and compares to disk; this is the exhaustive
  correctness check. DTC and DTM use independent probe instances, so DTM
  validates against the `.lzdtm50` pack even with `dtm/` absent. `--dtc`
  and `--dtm` restrict which is checked. **Full tables only**;
  shipping-format files with a dropped STM color are rejected.

`make -C tests` builds all four test binaries directly: `probe_fen`,
`compare_syzygy`, `check_tables`, `check_fixedpoint`.

## Shrink

```sh
./shrink wdl dtc dtm dtm50
./shrink wdl/KQK.lzw dtc/KQK.lzdtc dtm/KQK.lzdtm dtm50/KQK.lzdtm50
./shrink --dry-run dtc/*
```

`shrink` rewrites files in place, dropping the larger compressed STM
color when it can be reconstructed at probe time (see [Format](#format)).

Arguments may be individual table files or generated table directories.
Files are detected by magic, so mixed shell globs are safe: `.info`
metadata, already-shrunk files, and non-derivable files are skipped.
`-n`/`--dry-run` parses each file and reports the size it would shrink to
without writing anything, then prints a grand total.

Shrink is a postprocessing step. The generator does not accept any
dependency on shrunken files -- shipping-format files are treated as
absent and regenerated as full files.

The choice is per file, and `check_tables` validates both forms. Holding
every 3–5 man table full costs about 12 GiB across `wdl/`, `dtc/`, and
`dtm50/`; shrinking the 6-man set saves close to 2 TiB.

A dropped color is recoverable by running the same one-ply derive
exhaustively over its domain -- a sweep, not a regeneration -- provided the
material's sub-table closure is already full, so a wholly shrunk set
rebuilds bottom-up. Byte-exact WDL boundary codes additionally need the DTC
companion to locate the positions at `dtc == 100`; folding them to WIN/LOSE
yields a correct table that differs in bytes.

## Format

Positions map to a storage index built for paging: king and pawn dimensions
carry the slice structure, so the compressor's permutation search is
restricted to the remaining piece classes (identical pieces within a class
are combinationally ranked), and gaps are skipped at block granularity
rather than compacted into a denser index. The chosen permutation is stored
per color in the file header.

WDL stores a class code per position, 4 bits each and two to a byte, packed
with LZ4-HC primed by a dictionary trained on the table itself (skipped
below 256 blocks). The distance tables instead map each value to a
frequency-sorted rank, indexed in 1 byte (≤256 ranks) or 2, and compress
those rank streams with LZMA. DTC ranks raw ply distances exactly, halving
only cursed/blessed plies (1-byte tier) -- lossy by up to a ply but
harmless, as those distances are already past the 50-move horizon. DTM
halves every value losslessly via its parity invariant (WIN odd, LOSE even).

Blocks compress independently -- 64 KiB for WDL, 1 MiB for DTC/DTM/DTM50 --
and are located through a per-color offset section.

Each `.info` carries symmetry-expanded orbit-weighted W/D/L/illegal counts
per stored color, plus that color's longest win and its FEN. The orbit
weight is each canonical position's true multiplicity (2 under file-mirror
symmetry; 4 or 8 under the pawnless dihedral group, depending on king-slice
stabilizers), so for each color W + D + L + illegal equals the table's
weighted domain total -- a check that legal and illegal cells exhaust the
indexed chess domain. Since WDL is projected from the DTC pass, it has no
`.info` of its own; its counts sit in the DTC one.

Decoding stays within one material: DTC/DTM/DTM50 read class from the
material's own WDL companion, and WDL needs no companion, so a full table
decodes from its own files alone, answering in O(1) -- one block read and
an index, plus one extra probe when the FEN carries ep rights. Sub-tables
enter only when generating a material or when a stored color is absent
(see [Shrink](#shrink)).

A table with one STM color dropped rebuilds it by one-ply minimax against
the kept color. A quiet move keeps the child in the kept opposite-STM
frame of the same material, read directly; only captures and promotions,
which change material, reach a sub-table. DTM50 threads each child's hmc
through the recursion so per-layer semantics are preserved.

For WDL the only WIN-vs-CURSED_WIN ambiguity at one ply is a child sitting
at exactly the last in-rule ply (`dtc == 100`), since the parent then tips
one ply past the 50-move edge. The format reserves two spare 4-bit codes
(`BOUNDARY_WIN`, `BOUNDARY_LOSS`) to mark those positions; every other
reader folds them to WIN/LOSE. Zeroing moves reset the clock, so they cross
no edge and need no marker.

## Universal DTM (`.lzdtm50`)

One file, two distance-to-mate answers: the unbounded DTM (no 50-move
rule) and the exact 50MR DTM50 at any halfmove clock.

The 50-move rule turns "distance to mate" into a moving target: a position
that wins-in-200-plies-flat is a 50MR draw, yet with 30 plies left on the
clock it might still win if a zeroing capture lands soon. The pack carries
the clock itself -- each material stores one layer per clock value, hmc 0
through 99, answering "optimal mate distance at this clock?" and collapsing
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

Incidentally, the draw-end changepoint encodes DTC. For W/L positions with
`2 ≤ dtc ≤ 100`, the 50MR result flips when zeroing no longer fits, at
`h = 102 - dtc`. The boundary cases are ambiguous: `h = 1` identifies a
cursed/blessed position only as `dtc > 100`, while `dtc ≤ 1` never flips.
The pack could therefore recover non-cursed DTC by itself, though the
current format does not expose it; what it is used for is a DTC/DTM50
cross-check in `check_tables` (see [Verify](#verify)).

A probe maps its query to a layer, locates the state via a stride-256
prefix index over the 2-bit vector (built at block-decompress time), and
reads the rank: direct for CONST, a compare for SINGLE/DOUBLE, mask +
popcount for MULTI -- O(STRIDE) walking + O(1) offset math, independent of
block size. All 101 layers together cost under 4× a single layer, so total
storage runs some 20× under an equivalent 100-file-per-material per-hmc
layout.

```sh
./chesstb -r KBNK --builddtm50
./chesstb --estimate -r KBNK --builddtm50
./tests/probe_fen --children "8/8/8/4k3/8/8/Q7/K7 w - - 50 1"
```

## Frozen pawn pairs

Materials with pawns on both sides blow up because each free pawn is an
independent ~48-square dimension. But a great many practically interesting
positions have a *blocked* pawn pair -- one white and one black pawn on the
same file, white below black, neither able to pass the other. Such a pair
has only 120 joint placements (white on ranks 2..6, black strictly above on
3..7 → 15 rank pairs × 8 files) against the ~2304 a free white/black pawn
duo spans. A **frozen-pair table** indexes the pair as that single
120-entry dimension.

**Naming.** Lowercase `'p'` denotes the pair, written once on each side so
both colors visibly hold a pawn: `KQpKp` is white K+Q plus the pair's white
pawn, black K plus the pair's black pawn -- physically the 5-man material
`KQPKP`. At most one pair per material.

**What it buys.** The pair pawns are ordinary pawns on the board; only the
indexing changes, so a position has the same value in either table.

| Pair table | positions      | resident | full material | positions       | resident |
|------------|----------------|----------|---------------|-----------------|----------|
| `KpKp`     | 216,720        | 847 KiB  | `KPKP`        | 4,074,336       | 15.5 MiB |
| `KQpKp`    | 13,870,080     | 52.9 MiB | `KQPKP`       | 260,757,504     | 995 MiB  |
| `KRPpKRp`  | 39,649,935,360 | 148 GiB  | `KRPPKRP`     | 383,835,045,888 | 1.40 TiB |

The gain is ~19× when the pair is the only pawn structure, and less when
free pawns remain alongside it (`KRPpKRp` keeps a free white pawn, so ~9.7×).

**Domain.** A pair table covers exactly the positions that contain an
opposing pair. Because the pair pawns are indistinguishable from free pawns
on a board, the pair slot is identified positionally: it is the opposing
pair minimal by (file, white rank, black rank). Cells where the designated
pair is not that minimum are pruned, so every stored cell round-trips
between board and index.

**Life cycle.** While the pair stands, neither member can pass the other and
neither can promote. Members may push toward each other until adjacent, and
every non-capturing move -- those pushes included -- stays inside the same
`'p'` material: the pair just moves to another of the 120 placements.
Captures leave it, always one man lighter:

- **p → PP** -- any capture of a free piece, whether the captor is a free
  piece or a pair member (diagonally or en passant). Both members join the
  child as free pawns, so the child is the full free-pawn material minus the
  captured man. From `KQpKp`, that is `KPKP`.
- **p → P** -- a pair member is itself captured (including en passant after
  a double push). The surviving member becomes a free pawn. From `KQpKp`,
  that is `KQPK` or `KQKP`.

By design a partial table resolves into **full** sub-tables: the pair is
unfolded on the way out rather than tracked through the capture, so no
`'p'` table roots a dependency tree of further `'p'` tables. Every such
transition also drops a man, so no child is the equally-sized full material
either. `KQpKp` builds on `KPKP`, `KQPK`, `KQKP` and their closures -- 26
configurations in the plan, itself the only pair-bearing one -- and never
needs `KQPKP`.

The one place a `'p'` material depends on another is promotion of a *free*
pawn, which is not a pair transition at all: the pair is untouched, so
`KPpKp` promotes into `KQpKp`/`KRpKp`/`KBpKp`/`KNpKp` and its 41-config
closure carries those four alongside itself. A pair table with no free pawn
to promote, like `KQpKp`, has no pair-bearing dependency whatsoever.

**Opt-in.** `--enumerate N` never emits pair materials; they exist only when
you ask for one by name. Generation, `--estimate`, `--mem` paging,
`--builddtm`/`--builddtm50`, shrink, and all three verifiers treat a `'p'`
material like any other.

```sh
./chesstb --estimate -r KQpKp
./chesstb -r KQpKp --builddtm50
./tests/check_fixedpoint KQpKp
```

**Probing.** The probe prefers a pair table whenever the position has an
opposing pair and that `'p'` table is on disk, falling back to the physical
material otherwise. The preference is re-evaluated per node, so children of
a root also route into a `'p'` table -- a board-derived material lookup
alone would miss that, since it sees the pair pawns as ordinary free pawns.
The probe is purely value-driven here, so unlike generation it will also
route a post-capture child into a smaller `'p'` table when the position
still holds an opposing pair and that table exists.

## Layout

```text
src/chess/   board, moves, FEN
src/egtb/    generator (DTC + DTM + DTM50), compression, slicing, paging
src/probe/   standalone probe library
src/shrink/  shipping-format shrinker
src/system/  platform shims
src/util/    allocation, threading, compression helpers
tests/       FEN probe, table/fixedpoint verifiers, Syzygy compare
lib/         vendored LZ4, LZMA, zstd (xxHash and dict trainer)
```
