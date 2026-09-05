# chesstb

Chess endgame tablebase generator. Produces five metrics, shipping in full or in
reduced forms -- an STM drop, loss-only distances, and relaxed WDL or DTZ (see
[Shrink](#shrink), [Transcribe](#transcribe)):

- **WDL** -- 50-move-rule-aware win/draw/loss with cursed/blessed classes.
- **DTZ** -- distance-to-zeroing (plies to the next capture, promotion or pawn
  push), 50-move-rule-aware without class offsets (see [Format](#format)).
- **DTC** -- distance-to-conversion at any halfmove clock, priced in pawn pushes:
  how many the winning side still owes before a capture or promotion, and how
  long until the next zeroing move on the line that owes them. Embeds DTZ as its
  unbounded row (see [DTC](#dtc-lzdtc)).
- **DTM** -- distance-to-mate, ignoring the 50-move rule.
- **DTM50** -- distance-to-mate at any halfmove clock, packed with DTM into
  one file (see [Universal DTM](#universal-dtm-lzdtm50)).

## Download

Available mirrors:

```text
ftp://chessdb:chessdb@ftp.chessdb.cn/pub/chesstb/
rsync://ftp.chessdb.cn/ftp/pub/chesstb/
hf://buckets/noobpwnftw/chesstb/
```

Below, `...` is build in progress, `---` is not planned, and Op1 is one
[opposing pawn pair](#opposing-pawn-pairs).

`dtm50/` supersedes `dtm/`; for pawnful materials `dtc/` supersedes `dtz/`.

Default shipping format:

| Material     | `wdl/`   | `dtz/`   | `dtc/`   | `dtm/`   | `dtm50/` |
|--------------|---------:|---------:|---------:|---------:|---------:|
| 3 (Pawnless) |    288 B | 8.03 KiB |      N/A | 8.03 KiB | 9.59 KiB |
| 3 (Pawnful)  |  4.2 KiB | 2.51 KiB | 6.82 KiB | 7.07 KiB | 14.8 KiB |
| 4 (Pawnless) |  500 KiB | 2.02 MiB |      N/A | 2.19 MiB |  7.5 MiB |
| 4 (Pawnful)  |  646 KiB |  782 KiB | 3.28 MiB | 4.02 MiB | 12.3 MiB |
| 5 (Pawnless) | 55.7 MiB |  273 MiB |      N/A |  389 MiB | 1.93 GiB |
| 5 (Pawnful)  |  209 MiB |  346 MiB | 1023 MiB |  1.3 GiB |  4.4 GiB |
| 6 (Pawnless) | 7.15 GiB |   26 GiB |      N/A | 48.7 GiB |  236 GiB |
| 6 (Pawnful)  | 36.4 GiB | 63.2 GiB |  175 GiB |  246 GiB |  847 GiB |
| 7 (Pawnless) |      ... |      ... |      N/A |      --- |      --- |
| 7 (Pawnful)  |      ... |      ... |      --- |      --- |      --- |
| 8 (Op1)      |      ... |      ... |      --- |      --- |      --- |
| Total        | 43.8 GiB | 89.8 GiB |  176 GiB |  297 GiB | 1.06 TiB |

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

Tables with [castling rights](#castling-rights) live under `castling/`:

| Material | `wdl/`   | `dtz/`   | `full/wdl/` | `full/dtz/` |
|----------|---------:|---------:|------------:|------------:|
| 3        |     72 B |    904 B |       200 B |    1.88 KiB |
| 4        | 18.7 KiB |  132 KiB |    92.4 KiB |     310 KiB |
| 5        | 7.21 MiB | 33.9 MiB |    26.3 MiB |    83.7 MiB |
| 6        | 1.52 GiB | 5.29 GiB |    5.11 GiB |    14.1 GiB |
| Total    | 1.52 GiB | 5.32 GiB |    5.13 GiB |    14.2 GiB |

A complete 6-man DTZ shipping set is therefore 140.5 GiB, castling included.

## Build

```sh
make
make tools
```

The build produces:

- `chesstb`: generator
- `shrink`: postprocessor for shipping-format files
- `transcribe`: re-encoder for shipping-format files

`make tools` adds the diagnostic tool `probe_fen` under `tools/` (see
[Probe](#probe)). Nothing beyond the vendored sources is needed.

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
| `--mem MiB` | resident memory soft cap (`0` = unbounded, see [Memory](#memory)) |
| `--cache MiB` | decoded-block cache budget shared by the sub-table probe readers (default 64, `0` = one block) |
| `--wdl/--dtz/--dtc/--dtm/--dtm50 DIR` | additional directory searched for existing tables and sub-tables, repeatable; output always goes to `./wdl/`, `./dtz/`, `./dtc/`, `./dtm/`, `./dtm50/` |
| `--tmp DIR` | scratch directory for spilled groups and checkpoints (default `./tmp/`) |
| `--builddtc` | also build DTC, pawnful materials only (see [DTC](#dtc-lzdtc)) |
| `--builddtm` | also build the standalone DTM table |
| `--builddtm50` | also build DTM50 (implies the DTM pass) |
| `--probe` | read DTM/DTM50 sub-tables through the direct probe reader instead of flat-decompressed temp files -- less scratch space, slower reads |
| `--fleet` | cooperative multi-process mode: take a per-material `flock`, skip materials another worker owns, and wait rather than abort on not-yet-ready sub-tables |
| `--enumerate N` | print canonical material names with ≤ N total pieces |
| `--estimate` | print the working-set estimate and exit |
| `--info PATHS` | dump `.info` files and exit; accepts globs (`--info dtz/*.info`) |

Requested materials are expanded through their capture/promotion
dependency closure and generated in dependency order. The DTZ pass always
runs, and WDL is a projection of its output. `--builddtm50` implies the DTM
pass (DTM built first, then folded in), then DTM50; `--builddtm` builds the
standalone DTM table on its own. Existing final files are skipped, with each
pass gated on a single output -- the DTZ pass on `wdl/<material>.lzw`, so
deleting that file is what forces a rebuild.

## Memory

`--mem MiB` soft caps resident memory usage; `--mem 0` is unbounded.
Positive values page groups through `--tmp`, and anything below 64 is
raised to 64, since a budget under one group cannot page. The same budget
applies to both generation and compression phases.

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
  [opposing pawn pair](#opposing-pawn-pairs).)
- **per-batch** -- a whole topological batch of pawn slices fused. This is
  the `--mem 0` ceiling.

The two passes reach differently, so their peaks are computed separately.
Iterate holds the dispatched group plus its king-neighbour reach. Init holds
the group plus the groups its pawn pushes target, a one-step closure; iterate
never needs those, staying inside the king-neighbour reach.

Both keep both STM colors resident, but not the same way. Iterate splits them,
the mover's group against the opponent's reach. Init writes both colors over
one group set, so its peak is twice that group's own-plus-push union.

`--builddtc` adds a `dtc per-dispatch` line for the pawnful materials it builds:
DTC keeps two budget layers live, since init reads push targets at the budget
below as well as its own, so its init peak is twice the figure above while
iterate stays inside one budget and pages like DTZ.

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

Generation handles `SIGINT` and `SIGTERM` at the next safe iteration
boundary. It flushes dirty scratch groups, writes a pass-specific checkpoint
under `--tmp`, and exits with status 130. DTZ, DTC, DTM, and DTM50 each record
enough state to continue the current fusion (and layer or iteration phase)
instead of restarting the material.

`SIGQUIT` instead schedules a clean exit at the next material boundary, where
no checkpoint is needed. `SIGHUP` cancels any pending exit request, including
one from `SIGINT` or `SIGTERM`, provided it has not yet been handled.

To resume from a checkpoint, re-run the same command with its saved state.
Delete the checkpoint to start over or before changing `--mem`.

## Probe

```sh
./tools/probe_fen "8/8/8/5k2/8/8/1Q6/K7 w"
./tools/probe_fen --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
./tools/probe_fen --wdl ./wdl --dtz ./dtz --dtm50 ./dtm50 "8/8/8/8/4k3/8/Q7/K7 w"
./tools/probe_fen --dtm50 ./dtm50 "8/8/8/4k3/8/8/Q7/K7 w - - 50 30"
./tools/probe_fen --dtc ./dtc "8/8/8/k7/8/8/K4P2/8 w - - 0 1"
```

`probe_fen` reports WDL, DTZ, DTC, DTM and DTM50 for a FEN, each field gated on
the tables that answer it being present. DTC prints as `class/order/value`,
its class being the one the clock leaves rather than the clock-free WDL one (see
[DTC](#dtc-lzdtc)). Every table-directory flag may be repeated to add search
locations.

The FEN halfmove clock selects the DTM50 layer (0 when the field is absent
or unparsable, DRAW at 100 and above) and the DTC budget (a tighter clock buys
fewer pushes, and once no budget fits it reports the draw that clock leaves);
`--children` threads each child's hmc through (zeroing → 0, quiet → parent+1) so
the per-child DTM50 and DTC values match what the engine would see post-move,
and `--limit N` caps how many children are printed.

A FEN carrying castling rights is answered only by a table that holds those
rights (see [Castling rights](#castling-rights)); with none on disk the probe
reports no table.

`--cache MiB` bounds the decoded blocks held resident across every open
table (default 64, `0` = a single block). Blocks are reclaimed
least-recently-used from one shared budget, so probing many materials
costs a fixed amount instead of growing per material.

## Shrink

```sh
./shrink wdl dtz dtc dtm dtm50
./shrink wdl/KQK.lzw dtz/KQK.lzdtz dtm/KQK.lzdtm dtm50/KQK.lzdtm50
./shrink dtc/KBPK.lzdtc
./shrink --dry-run dtz/*
./shrink --out output wdl/* dtz/* dtc/* dtm/* dtm50/*
```

`shrink` rewrites files in place by default, dropping the larger compressed
STM color when it can be reconstructed at probe time (see [Format](#format)).

Arguments may be individual table files or generated table directories.
Files are detected by magic, so mixed shell globs are safe: `.info`
metadata, already-shrunk files, loss-only files (see
[Transcribe](#transcribe)), and non-derivable files are skipped.
`-n`/`--dry-run` parses each file and reports the size it would shrink to
without writing anything, then prints a grand total.

`--out DIR` leaves the inputs untouched and writes results under `DIR`
instead. Since the argument list is usually a glob spanning several table
directories, `DIR` is one flat directory keyed by filename alone (the
extension already distinguishes the table kinds). Tables that cannot
shrink are copied there unchanged, non-table files are not copied.

## Transcribe

```sh
./transcribe --out output --dtz dtz --dtm dtm --dtm50 dtm50 -r KQK,KRK
./transcribe --out output --do-wdl --list five.txt --block 32
./transcribe --out output --do-wdl --relaxed --list five.txt
./transcribe --out output --dtc dtc --loss-only --list five.txt
./transcribe --out output --dtm50 dtm50 --extract-dtm --list five.txt
./transcribe --out output --dtz dtz --relaxed --fleet --list seven.txt
```

`transcribe` re-encodes a finished table into alternative shipping forms.
Everything the save path consumes -- classified cells, value histogram,
index permutation -- is recoverable from the file, so it re-blocks,
re-permutes, re-ranks and recompresses without a solve. Output goes under
`--out`, the source stays mapped for the run.

| Flag | Meaning |
|------|---------|
| `-r LIST` / `--list FILE` | materials, as for the generator |
| `--out DIR` | output directory, must differ from the input one |
| `--wdl DIR` | WDL companions, and the source for `--do-wdl` (default `./wdl/`) |
| `--do-wdl` | transcribe the WDL table itself |
| `--dtz/--dtc/--dtm/--dtm50 DIR` | transcribe that metric from DIR |
| `--extract-dtz` | transcribe DTC's embedded DTZ as a standalone table; automatic with `--loss-only` |
| `--extract-dtm` | transcribe DTM50's embedded DTM as a standalone table; automatic with `--loss-only` |
| `--loss-only` | emit loss-only frames (see below) |
| `--relaxed` | emit relaxed WDL or DTZ; symmetric materials use loss-only for requested distance tables, while other asymmetric distance types are rejected (see below) |
| `--block KiB` | output block size (default: the metric's own; WDL caps at 64) |
| `--samples N` | blocks compressed per permutation candidate (default: the metric's own) |
| `-t N` | worker threads |
| `--cache MiB` | decoded-block budget shared by the sources (default 64) |
| `--fleet` | cooperate through per-material locks and skip finished output |
| `--tmp DIR` | scratch directory for block spill |

A dropped frame carries no payload, so it passes through untouched and a
shipped set re-blocks with its drops intact. A distance table needs its WDL
companion to store every color it re-encodes, since that is where the class
comes from; transcribe from the full tables when a source lacks the frames
it needs.

### Loss-only

`--loss-only` writes distance tables that price only their loss-class cells.
Wins become don't-cares, and the prober rebuilds one with the same one-ply
minimax it runs for a dropped color. WDL is never loss-only: it carries the
class the derive reads.

Loss-only and an STM drop are alternatives, not a stack. Dropping a color
from a loss-only table would leave that color's losses deriving from wins
that are not stored either, so `shrink` skips loss-only files and
`transcribe` refuses to convert a dropped source to loss-only. Symmetric
materials are exempt from the conflict: their second frame is the first one
mirrored rather than a derive.

An STM drop keeps whichever frame compresses smaller, whereas loss-only
must keep every loss wherever it sits -- the higher-entropy half -- so it
generally loses on materials where a drop applies at all. It wins where a
drop is a no-op, and there its derive is also the cheaper one: it only ever
derives a win, while a dropped color derives its losses too, which cannot
prune by class.

Both packs carry an unbounded row -- DTM50's is the DTM, DTC's the DTZ -- so a
loss-only one writes the matching loss-only companion as a byproduct and takes
its layout.

### Relaxed

Relaxation makes selected cells into don't-cares instead of dropping an entire
frame. WDL and DTZ use different rules to decide which cells may be omitted.
Relaxed tables may also have an STM frame dropped; `shrink` preserves the
relaxed flag on the remaining frame. A relaxed WDL table is not a transcription
source. A relaxed DTZ one is, for `--relaxed` or `--loss-only`, neither of
which reads a freed value.

#### WDL

**The bound.** On a relaxed frame the probe computes a *bound* for every cell
-- the best class any capture or promotion reaches, read from the child's own
table -- and answers the max of that bound and the stored code. Those two move
kinds are exactly the ones that leave the material, so every child sits in a
sub-table and the derive cannot re-enter the frame it is resolving.

**The slack.** Where the bound already reaches the cell's own class, the max
recovers that class whatever sits underneath, so the cell may store any code no
better than its own. Where the bound falls short, the cell stores itself and
the max is a no-op. Nothing marks which is which -- the probe takes the max
unconditionally, so there is no per-cell flag and no predicate.

Classes rank `LOSE` < `BLESSED_LOSS` < `DRAW` < `CURSED_WIN` < `WIN`, and the
slack is the room below a cell's own class, so it lands on wins and draws:

| Cell holds | Bound reaches | Cell may store |
|------------|---------------|----------------|
| `WIN` | `WIN`, by a winning capture | any of the five codes |
| `DRAW` | `DRAW`, by a drawing capture | `LOSE`, `BLESSED_LOSS`, `DRAW` |
| `LOSE` | -- | nothing: it is already the bottom |

A `KQKR` cell that wins by taking the rook is the easy case: the capture lands
in `KQK`, which answers WIN, so the cell itself stores whatever compresses
best and the probe recovers WIN from the capture regardless. Boundary markers
never relax, the dropped-frame derive reading them rather than folding them.

**Spending it.** Two fills compete for the freed cells and neither dominates,
so each block is written both ways and the smaller kept. **Run-stitch** extends
the adjacent run into the slack, winning where the value function is locally
flat. **relz** rewrites free bytes to continue an older copy of the last four
committed bytes -- LZ4's minimum match -- winning where the pattern is strided
instead.

Transcription needs the one-move sub-tables in full form; the probe reaches
them through the ordinary derive. The permutation search runs against the
capped cells. A cap is measured against the cell's own stored code, so a second
pass would cap against the first one's fill and lose slack for good.

#### DTZ

A winning position has DTZ 1 when it has a zeroing move that preserves its WDL
class. The transcriber omits that value. When the prober reads a winning cell
from a relaxed frame, it checks the zeroing moves first and returns 1 if one
preserves the class. Otherwise it returns the decoded value. There is no
per-cell marker; omitted cells are ordinary compression don't-cares.

A zeroing move is a capture, a promotion, or any pawn move. The check needs only
the resulting WDL class. Captures and promotions read a sub-table, while an
ordinary pawn move reads the current material's WDL table. For a double push,
the effective class also accounts for any legal en-passant reply. The check
never reads a child DTZ value, so pawn pushes do not cause recursive DTZ probes.

Losses cannot use this rule because the losing side maximizes DTZ; one zeroing
move does not determine the final value. A quiet mate in one also has DTZ 1,
but recognizing it requires examining the child position. These values remain
stored.

Transcription requires the full WDL closure for its zeroing moves, and both of
an asymmetric pawn material's own WDL frames. The DTZ source may already be
relaxed or carry an STM drop; its WDL companion may be neither, being where
every cell reads its class.

Relaxed DTZ is a limited form of loss-only: it omits only the winning values
that can be recovered from a local WDL test. That restriction lets it coexist
with an STM drop. Full loss-only cannot do so on an asymmetric material because
the missing frame's losses could depend on wins that were also omitted. A
symmetric material already has only one physical frame, and every derived win
reads a stored loss from that frame. Full loss-only is therefore strictly
better there at essentially the same probe cost, so `--relaxed` emits
loss-only distance tables for symmetric materials. For asymmetric materials,
the option applies only to WDL and DTZ; requesting another distance metric is
an error rather than silently writing it in full form.

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
those rank streams with LZMA. DTZ ranks raw ply distances exactly, halving
only cursed/blessed plies (1-byte tier) -- lossy by up to a ply but
harmless, as those distances are already past the 50-move horizon. DTM
halves every value losslessly via its parity invariant (WIN odd, LOSE even).

Blocks compress independently -- 64 KiB for WDL, 1 MiB for DTZ/DTC/DTM/DTM50 --
and are located through a per-color offset section.

Each `.info` carries symmetry-expanded orbit-weighted W/D/L/illegal counts
per stored color, plus that color's longest win and its FEN. The orbit
weight is each canonical position's true multiplicity (2 under file-mirror
symmetry; 4 or 8 under the pawnless dihedral group, depending on king-slice
stabilizers), so for each color W + D + L + illegal equals the table's
weighted domain total. Since WDL is projected from the DTZ pass, it has no
`.info` of its own; its counts sit in the DTZ one.

Decoding stays within one material: DTZ/DTC/DTM/DTM50 read class from the
material's own WDL companion, and WDL needs no companion, so a full table
decodes from its own files alone, answering in O(1) -- one block read and
an index, plus one extra probe when the FEN carries ep rights. Sub-tables
enter only when generating a material, or on a dropped (see
[Shrink](#shrink)), loss-only or relaxed frame.

Each color's frame opens with a flag byte: `0x80` one value for the whole
domain, `0x40` dropped, `0x20` losses only, `0x10` relaxed (see
[Transcribe](#transcribe)). All three of the latter leave cells to the
prober, so the generator declines them as sub-tables and regenerates
instead.

A table with one STM color dropped rebuilds it by one-ply minimax against
the kept color. A quiet move keeps the child in the kept opposite-STM
frame of the same material, read directly; only captures and promotions,
which change material, reach a sub-table. DTM50 threads each child's hmc
through the recursion so per-layer semantics are preserved; DTC takes each
child's pair for a win and folds every push budget for a loss, which settles
on whatever budget the most stubborn defence forces.

A loss-only frame runs the same minimax for a win, bottoming out in one ply:
every child that can win a win's minimax is a loss for the child's side to
move, and those are stored. Its losses are read directly, so it never derives
the costlier case: both walk every move for the argmin, but a win needs a
distance only from the children that can win it, where a loss needs one from
every child.

For WDL the only WIN-vs-CURSED_WIN ambiguity at one ply is a child sitting
at exactly the last in-rule ply (`dtz == 100`), since the parent then tips
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
monotone non-decreasing -- `DTM ≤ DTM50(hmc=0) ≤ … ≤ DTM50(hmc=99)` --
ending in a single flip to DRAW once no line fits. The unbounded DTM
(shortest mate, ignoring the clock) is just one more layer at the front
(pack layer 0), at most one extra change-point per position. So the pack
alone (`dtm50/`, 6.35 GiB) replaces pack-plus-standalone-`dtm/` (8.04
GiB), dropping the redundant 1.69 GiB DTM table -- 21% off the 3–5 man
set. A no-50MR probe reads layer 0; an `hmc = k` probe reads layer
`k + 1`. `transcribe --extract-dtm` transcribes it as a standalone table
(see [Transcribe](#transcribe)).

Class comes from the WDL companion per layer: **layer 0** keeps
cursed/blessed as decisive; **hmc = 0** folds them to DRAW; **hmc > 0**
also collapses budget-busting routes to DRAW. Where DTM50 and WDL disagree
(a WDL=WIN cell drawn for lack of window) the draw-end hint says so.

**Pack layout.** All 101 layers live in one `dtm50/<material>.lzdtm50`
(layer 0 the unbounded DTM, 1..100 the hmc layers). Since the value is
constant or nearly so for most positions, each is classified per block into
one of four states, packed 2 bits per position:

- **CONST** -- identical at every layer.
- **SINGLE** -- one transition. `[h, r0, r1]`, h ∈ [1, 100].
- **DOUBLE** -- two transitions. `[h1, h2, r0, r1, r2]`.
- **MULTI** -- three or more. `[k, 128-bit changepoint bitmap, k ranks]`.

Each non-CONST state carries an "ends in DRAW" hint bit (piggybacked on the
last h byte's MSB, MULTI's on its `k` byte); when set, the trailing rank is
dropped and the decoder synthesizes DRAW -- the dominant terminal pattern
for W/L positions, and DRAW's only spelling. The rank table holds only
frequency-sorted W/L storage values; DRAW and ILLEGAL are WDL-companion
don't-cares that never take a slot, and all-DRAW/ILLEGAL blocks emit zero
bytes.

**DTZ rides along.** The draw-end changepoint encodes DTZ. For a W/L position
with `2 ≤ dtz ≤ 100` the 50MR result flips when zeroing no longer fits, at
`h = 102 - dtz`. Both ends are blind: `h = 1` is a position already drawn at a
fresh clock, so it names the cursed/blessed band without pinning a distance,
and `dtz ≤ 1` never flips -- there `dtz = 0` is exactly checkmate, which
move-gen splits from `dtz = 1`. The prober decodes the flip with the value and
minimaxes it with the layer, so the pack answers DTZ everywhere but that band.

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
./tools/probe_fen --children "8/8/8/4k3/8/8/Q7/K7 w - - 50 1"
```

## DTC (`.lzdtc`)

Distance-to-conversion, with pawn pushes priced separately from waiting. A DTC
probe answers two numbers against the clock the caller holds:

- **order** -- pawn pushes the winning side still owes before a *conversion*
  (a capture or a promotion). A push is irreversible progress that is not yet a
  conversion; the count resets at every conversion.
- **value** -- plies to the next zeroing move on the line that owes them, which
  is DTZ's quantity. At order 0 the next zeroing *is* the conversion; above it the
  next zeroing is the first of the pushes still owed.

The key is **order first, then value**: a line converting without touching a
pawn outranks one that spends a push, even when the push is quicker. That is
what DTZ cannot express -- it prices every push at 1, so it reads 1 almost
everywhere a pawn can move and cannot tell a useful push from a wasted one.
Two pawns against a lone king make the difference plain: DTZ shuffles both of
them up a square at a time, ten pushes to promote, while DTC double-pushes one
and runs it in, four pushes, leaving the other pawn home.

**The clock decides how many pushes you can afford.** Fewer pushes costs a
longer wait, so a tight clock forces the trade the other way. The table is a
stack of layers indexed by push budget: layer *k* answers "the winning side may
spend at most *k* of its own pushes", and inside a layer the value is DTZ's, so
a layer is a DTZ retro whose pawn edges resolve one layer down for the winner's
push and at the same layer for the loser's. A probe takes the fewest pushes
whose wait still fits `rule50`; where no budget fits, it answers DRAW, because
that clock has taken the win. So a DTC probe reports a class of its own --
`WIN`/`LOSE` with the pair, or `DRAW` -- the same shape DTM50 reports per layer,
and distinct from the clock-independent WDL class.

Layers hold only what 50MR settles, so the ply ceiling *is* the band: a win the
budget reaches but the clock does not is a larger budget's business, and where no
budget manages it the answer is the unbounded row's. Classes come from the WDL
companion throughout.

Storage rides the same changepoint pack as [Universal DTM](#universal-dtm-lzdtm50):
budgets count down from the widest, so the unbounded row sits at pack index 0 and
each cell's DRAW run lands at the tail, where the draw-end hint pays for it. Most
cells are CONST -- their optimal line never pushed, so no budget changes anything.

Why exactly 30 rows suffice is a subtle invariant. An 8-man position can give
the winning side at most six pawns, each with at most five non-converting pushes
before promotion, so the budget curve has at most 30 relevant changepoints. For
a clean W/L cell its terminal changepoint is exactly DTZ; the embedded DTZ row
therefore supplies that endpoint, and only the preceding 29 points are solved
separately. A cursed/blessed cell has no valid finite-budget endpoint, so its
probe reads row 0 only as DTZ and reports DTC as DRAW.

**That unbounded row is the DTZ table**, embedded the way DTM50 embeds the DTM:
generation reads it from `dtz/` instead of solving it again, writes its plies raw
and takes its storage layout. One `.lzdtc` then answers both metrics, cursed band
included, so a pawnful material needs no `dtz/`, and
`transcribe --extract-dtz` raises the table back out byte for byte.

**Pawnful only.** With no push to budget the stack is one layer whose every
zeroing move is already a conversion, so a pawnless pack would only repeat the
DTZ table.

## Opposing pawn pairs

Materials with pawns on both sides blow up because each free pawn is an
independent ~48-square dimension. But a great many practically interesting
positions have an *opposing* pawn pair -- one white and one black pawn on the
same file, white below black, neither able to pass the other. Such a pair
has only 120 joint placements (white on ranks 2..6, black strictly above on
3..7 → 15 rank pairs × 8 files) against the ~2304 a free white/black pawn
duo spans. An **opposing-pair table** indexes the pair as that single
120-entry dimension.

**Naming.** Lowercase `'p'` denotes the pair, written once on each side so
both colors visibly hold a pawn: `KQpKp` is white K+Q plus the pair's white
pawn, black K plus the pair's black pawn -- physically the 5-man material
`KQPKP`. At most one pair per material.

**What it buys.** The pair pawns are ordinary pawns on the board; only the
indexing changes, so a position has the same value in either table.

| Pair table | positions      | resident | full material | positions       | resident |
|------------|---------------:|---------:|---------------|----------------:|---------:|
| `KpKp`     | 216,720        |  847 KiB | `KPKP`        | 4,074,336       | 15.5 MiB |
| `KQpKp`    | 13,870,080     | 52.9 MiB | `KQPKP`       | 260,757,504     |  995 MiB |
| `KRPpKRp`  | 39,649,935,360 |  148 GiB | `KRPPKRP`     | 383,835,045,888 | 1.40 TiB |

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
`--builddtm`/`--builddtm50` and shrink treat a `'p'` material like any other.

```sh
./chesstb --estimate -r KQpKp
./chesstb -r KQpKp --builddtm50
./tools/probe_fen --wdl ./wdl "8/8/8/8/1p6/1P6/3k4/K6Q w"
```

**Probing.** The probe prefers a pair table whenever the position has an
opposing pair and that `'p'` table is on disk, falling back to the physical
material otherwise. The preference is re-evaluated per node, so children of
a root also route into a `'p'` table -- a board-derived material lookup
alone would miss that, since it sees the pair pawns as ordinary free pawns.
The probe is purely value-driven here, so unlike generation it will also
route a post-capture child into a smaller `'p'` table when the position
still holds an opposing pair and that table exists.

## Castling rights

A table built from the men alone has no cell for a position that can still
castle: the same men without rights are a different position, one move
poorer, so every prober declined those rather than answer them. A **castling
table** indexes the rights alongside the men. Lowercase `'r'` is a rook that
still holds one, together with the king it castles with -- `KrK` is
physically `KRK`, `KrrK` a king that has not moved with both rooks home,
`KrrKrr` that on both sides, two per side at most. Which files the men
started on, and which side of the king a rook stands on, are index
dimensions rather than part of the name, so one table covers every
arrangement and 960/DFRC is the general case.

Pinning those men pays for itself: a rights-holding side's king and rooks
become one 56-entry dimension in place of two free 64-square ones -- 56
whether it holds one right or two, since two exclude the corner king files
on their own, and the sides multiply independently, so `KrKr` is 56 x 56 =
3,136 cells with neither king free.

Rights only decrease, which is what lets a castling table sit *above* its
twin rather than beside it. A king move, a castle, or that rook moving turns
`r` into an ordinary `R` -- `KrrK` drops to `KRrK` or `KRRK` -- while
capturing the rook takes its right with it, `KrrK` to `KrK`. Both can happen
at once: a rook taking the enemy rook down its own file leaves `KRK`.

`--enumerate N` never emits castling materials; they exist only when you
name one. Everything else -- generation, `--estimate`, paging, the
`--build*` passes, shrink, transcribe -- treats an `'r'` material like any
other.

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
