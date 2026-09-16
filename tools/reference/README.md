# Reference tools

An independent check on chesstb's tables: a second generator, written from the
rules of chess rather than from chesstb's code, and a comparison that reads the
tables chesstb writes.

| File | What it is |
| --- | --- |
| `board.h`, `board.cpp` | Board, legal move generation (standard and Chess960 castling), FEN, position validity |
| `retro.h`, `retro.cpp` | Reverse move generation: every legal predecessor of a position |
| `test_retro.cpp` | Perft and bijection tests for `board` and `retro` |
| `retro_expected.txt` | Predecessor counts and sets from python-chess, for the parity test |
| `refgen.h`, `refgen.cpp` | Reference generator for WDL, DTZ, DTM, DTM50 and DTC |
| `refgen_main.cpp` | `refgen`, a command line front end to the generator |
| `check_reference.cpp` | Probes chesstb's tables for every position the reference generates |

`board`, `retro` and `refgen` include nothing from `src/` and need only a C++17
compiler. Only `check_reference` links chesstb's prober. They can be lifted out
and used on their own.

## Build

```sh
make -C tools/reference            # test_retro, refgen, check_reference
make -C tools/reference test       # build and run test_retro
```

## Reverse move generation

`ref::predecessors(board, options)` returns each distinct position from which one
legal move reaches `board`, with that move. `ref::unmake_perft` counts
predecessors along every path to a depth.

Candidates are over-generated and each is kept only if forward generation finds
the move, so every result is a real predecessor. Completeness is the part that
needs testing. It covers uncaptures, unpromotions, double pushes, en passant,
uncastling (standard or Chess960), castling rights the predecessor held and lost,
and en passant captures the predecessor declined.

Two validity modes decide which predecessors count:

- `TABLE`: any position a tablebase indexes (side not to move is not in check).
- `STRICT`: also the checks python-chess applies (piece and pawn counts, castling
  rights, en passant plausibility, impossible checks).

`test_retro` checks:

- perft against published counts, standard and Chess960;
- the bijection: across whole move trees, every forward move is recovered from
  the position it reaches and every predecessor found is reached;
- exact parity with a python-chess implementation on `retro_expected.txt`.

`./test_retro --quick` runs a shorter set.

## Reference generator

```sh
./refgen [-t N] [--verify] [--no-dtm] [--no-dtm50] [--no-dtc] MATERIAL...
```

Materials use chesstb's names, including `r` for a rook holding a castling right
and `p` for an opposing pair. The generator builds each material's closure first
and keeps it in memory.

Each metric follows the definition chesstb documents:

- **WDL, DTZ:** five classes; a quiet move past 100 plies turns a win cursed and a
  loss blessed; a double push accounts for the en passant replies it allows.
- **DTM:** plies to mate with no move counter.
- **DTM50:** plies to mate at each halfmove clock from 99 to 0; a quiet move at a
  clock of 99 is a draw unless it mates.
- **DTC:** the value at each pawn push budget from 0 to 28; captures and
  promotions convert, pushes spend one budget.
- **Castling:** Chess960 rules, with rights lost to king moves, rook moves and
  captures on the rook's square; a castling material is solved together with the
  materials it can reach by losing rights.

`--verify` rechecks every stored DTZ and DTM against its children.

DTM50 is solved from clock 99 down. Clocks 99 and 98 are evaluated in full; below
that, a layer can differ from the one above only where a quiet move reaches a
position that changed between the two layers above, so only those positions are
evaluated again. Castling families, whose quiet moves can leave the table, are
evaluated in full at every clock.

DTC is solved from budget 0 up. A budget's layer is a function of the layer below
it and of values that do not depend on the budget, so once a layer repeats the one
below it, every layer above repeats too and is copied rather than solved. Both
shortcuts are exact: they reproduce the values a full solve gives, which is what
the comparison below checks.

Memory is held to one byte per position for WDL, two for each distance metric
asked for, and two for DTC on pawn materials. A sub-table's DTZ is dropped once it
cannot be read again.

## Comparing with chesstb

```sh
./check_reference --tables DIR -r KQK,KRPKR [-t N] [--closure]
                  [--hmc all|0,1,50,99] [--rule50 0,50,99] [--limit N]
                  [--no-dtm] [--no-dtm50] [--no-dtc]
```

`DIR` holds chesstb's `wdl/`, `dtz/`, `dtm/`, `dtm50/` and `dtc/`. For every
legal position of each material:

| Metric | Compared |
| --- | --- |
| WDL | class, through the prober |
| DTZ | plies; cursed and blessed values may differ by one, the rounding chesstb documents |
| DTM | plies, from `dtm/` and separately from `dtm50/` |
| DTM50 | class and plies at each clock in `--hmc` |
| DTC | the stored curve at every budget, and the budget and value the prober reports at each clock in `--rule50` |

No table stores a position just after a double push that left an en passant
capture; the prober works it out from the stored value and the capture. For each
stored position that a legal double push could have reached, with a legal capture
available, every metric above is also checked on the en passant form, against the
reference value with the capture added.

`--closure` also compares the sub-tables. The report lists positions checked,
mismatched and missing per material and metric, with up to `--limit` sample FENs
for each.

## Results

`test_retro` passes in full: perft against published counts for standard chess
and against python-chess for Chess960; the bijection across 22 suites (standard
chess under both validity modes, and Chess960), with 15,698 of 15,698 forward
moves recovered and 57,007 of 57,007 predecessors confirmed; and predecessor
sets and unmake counts identical to the python-chess implementation.

`check_reference` against tables chesstb generated locally with `--builddtm
--builddtm50 --builddtc`:

| Materials | Clocks | Positions (WDL) | DTM50 values | Differences |
| --- | --- | ---: | ---: | --- |
| All 24 pawnless 3-4-man | all 100 | 50,289,569 | 5,028,956,900 | none |
| All 11 3-4-man with pawns | 0, 1, 2, 3, 10, 25, 50, 75, 90, 96, 97, 98, 99 | 84,420,308 | 1,097,464,004 | none |
| KrK, KrrK, KrKr, KrKR, KrKN, KrPK, KrKP and the KpKp pair table | all 100 | 1,641,532 | 164,153,200 | none |
| KRBKR (5-man) | 0, 1, 2, 50, 97, 98, 99 | 152,620,510 | 1,068,343,570 | none |
| KNPKN (5-man with a pawn) | 0, 1, 2, 50, 97, 98, 99 | 556,739,460 | 3,897,176,220 | none |
| KrPKR (5-man, a castling right and a pawn) | WDL and DTZ only | 15,084,888 | | none |

"Positions" counts each legal position once per side to move; DTM is also
compared at every one of them from both `dtm/` and `dtm50/`, and DTZ at every
position that is not a draw.

For the materials with pawns, the DTC curve is compared at all 29 budgets:
1,797,031,197 values over the 3-4-man materials and 5,780,369,929 over KNPKN.
The prober's DTC answer, which picks the smallest budget that fits the clock, is
compared at clocks 0, 50 and 99 for the 3-4-man materials (185,899,779 answers).
That comparison needs every budget's layer in memory at once, which is 23 GB for
a 5-man material with a pawn, so at 5 men the stored curve is checked but the
prober's choice among budgets is not.

KPKP, the one 3-4-man material with en passant positions, adds 42,366 of them,
each checked for WDL, DTZ, DTM from both files, DTM50 at every clock above and
the DTC answer.
