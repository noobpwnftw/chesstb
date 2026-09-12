# Correctness of ChessTB: Proofs for a Five-Metric Chess Endgame Tablebase System

## Abstract

We prove that the ChessTB endgame tablebase system computes and serves the exact game-theoretic class of every position under the 50-move rule. It computes exact clean DTZ, exact DTM, DTM50 values equal to the value of the clocked game at every clock, and DTC layers equal to a push-budget game. Cursed DTZ is computed exactly under the exit composition of Definition 5.19, and the one-byte storage tier decodes an even cursed value one ply short.

The central tool is a Clock Lemma: the clocked game is acyclic, and a position is decisive at clock c exactly when it lies in the zeroing attractor of depth 100 − c. The lemma yields the correctness of the 50-move classification and of the boundary codes. It also shows that the last change point of a DTM50 column encodes DTZ as 102 − dtz.

We further prove that the index is a bijection with exact orbit weights, that the scheduling is well-founded, that the parallel passes are independent of thread interleaving, and that the change-point codec is lossless. Paging and resume are proved correct as well. Every reduced shipping form (a dropped side-to-move frame, loss-only frames, relaxed WDL and relaxed DTZ) is reconstructed exactly by one ply of minimax, except that a derived cursed DTZ lies within the tolerance of Theorem 12.15.

## 1. Introduction

ChessTB is a C++17 system of about 34,500 lines that generates, compresses, reduces and probes chess endgame tablebases for five metrics: WDL, DTZ, DTC, DTM and DTM50. This paper proves that system correct against a formal model of chess under the 50-move rule.

Correctness means two things. First, every value the generator writes equals the game-theoretic value of Definition 2.3. Second, every value the prober returns, whether read directly or reconstructed from a reduced file, equals that value too.

### 1.1 Results

| Result | Statement | Status |
| --- | --- | --- |
| Clock Lemma (Lemma 2.4) | val(x, c) is decisive iff x lies in the zeroing attractor of depth 100 − c | proved |
| Index (Theorems 3.2–3.15) | Every legal position has exactly one canonical cell; orbit weights are exact orbit sizes; reader and writer agree | proved |
| Scheduling (Theorems 4.1–4.7) | Every value read is final before it is read; retrograde search stays within one pawn slice and its mirror | proved |
| WDL (Theorem 5.17) | Every stored class, boundary codes included, is exact | proved |
| DTZ (Theorem 5.20) | DTZ is exact, clean and cursed | proved |
| DTM (Theorems 6.12–6.13) | Exact, and odd/even by class | proved |
| DTM50 (Theorems 7.2–7.9) | Each layer equals the clocked game; layers are monotone; the first DRAW packed layer is 102 − dtz | proved |
| DTC (Theorems 8.3–8.19) | Layers equal the push-budget game; 30 budgets suffice; saturation is sound; the reader returns the minimal budget | proved |
| Concurrency (Theorem 9.8) | Results are independent of thread interleaving | proved |
| Paging and resume (Theorems 10.3–10.20) | Paging is invisible; resume is exact; output bytes are deterministic | proved |
| Encoding (Theorems 11.3–11.29) | Every consulted cell round-trips; cursed 1-byte values lose at most one ply | proved |
| Reconstruction (Theorems 12.12–12.39) | Dropped, loss-only and relaxed frames rebuild exactly, except that a derived cursed DTZ lies within the tolerance of Theorem 12.15; the prober never returns a wrong value | proved |
| Probe depth (Theorem 12.10) | Nested probes from tables of at most 8 men enter depth-capped functions only at depth at most 25, below the cap of 64, and the bound is attained in the model | proved |

### 1.2 Method

Sections 3 and 4 prove the index and scheduling results. The remaining argument proceeds in three steps.

1. **Semantics.** Section 2 defines the targets. The clocked game Γ is acyclic (Lemma 2.2), so its values are unique. The Clock Lemma then reduces the outcome (W, D or L) at every clock to one integer per position, z(x), together with whether x lies in the A or the B sets.
2. **Solvers.** Sections 5–8 show that each solver computes the unique solution of a Bellman system equivalent to its target. Each such proof separates soundness (every write is a true bound) from completeness (every value is found at the right ply), and adds a frontier lemma showing that pruning never skips work.
3. **Storage and reading.** Sections 9–12 show that concurrency, paging, encoding and reconstruction preserve those values.

## 2. Formal model

This section fixes the semantics every later theorem refers to. Its main result, the Clock Lemma (Lemma 2.4), reduces the outcome under the 50-move rule at every clock to one integer per position, z(x), together with whether x lies in the A or the B sets. The WDL, DTZ and DTM50 results and the embedded-DTZ theorem of Section 7 rest on it.

### 2.1 Positions, moves and the potential μ

A position x is a board, a side to move and a set of castling rights. L(x) is the set of legal moves, and x·m is the position after move m. En-passant rights are not part of x. A position with en-passant rights is entered only by a double push. A double push is a zeroing move that lowers μ, so such a position lies on no cycle, and Lemma 2.2 and the Clock Lemma extend to it unchanged. Its value is the stored value combined with its en-passant captures (Theorem 12.36). Moves split into three disjoint kinds:

- **Zeroing moves** Z(x): captures, promotions and pawn moves. They reset the halfmove clock.
- **Exits** E(x): non-zeroing moves that drop a castling right. These are the moves of a king whose side holds a right, castling included, and the moves of a rook standing on a square that carries a right.
- **Quiet moves** Q(x): all other moves.

Define the potential

μ(x) = (men(x), Σ\_p life(p), rights(x)),

ordered lexicographically. The life of a pawn is its number of remaining steps to promotion: 8 − rank for White and rank − 1 for Black.

**Lemma 2.1 (potential).** Every zeroing move and every exit strictly decreases μ. Every quiet move leaves μ unchanged.

*Proof.* A capture removes a man. A promotion keeps the men and removes a pawn life of at least 1, and a non-promoting pawn move keeps the men and lowers one life by 1 or 2. An exit keeps men and pawns and removes a right. A quiet move keeps all three coordinates. ∎

Consequently every cycle of the move graph consists of quiet moves and stays inside one material, one pawn configuration and one rights set. It follows that retrograde search stays inside one pawn slice and its file mirror (Theorem 4.7).

### 2.2 The clocked game Γ

The states of Γ are pairs (x, c) with clock c ∈ {0, …, 100}.

- If L(x) = ∅, the state is terminal: a loss at distance 0 if the side to move is in check (checkmate), otherwise a draw (stalemate).
- If c = 100 and L(x) ≠ ∅, the state is a draw (the 50-move rule).
- Otherwise move m leads to (x·m, 0) if m is zeroing, and to (x·m, c + 1) if not.

A mate delivered by the move that brings the clock to 100 is still a mate: the first rule applies to the child (y, 100) with L(y) = ∅ before the second.

**Lemma 2.2.** Γ is acyclic.

*Proof.* Every move strictly decreases (μ(x), 100 − c) lexicographically: a non-zeroing quiet move keeps μ and increases c, and exits and zeroing moves decrease μ (Lemma 2.1). This order is well-founded. ∎

By Lemma 2.2, backward induction assigns every state a unique minimax value val(x, c) ∈ {W\_d, D, L\_d}. The winner minimizes the mate distance d and the loser maximizes it.

**Definition 2.3 (the five targets).**

1. **DTM50.** DTM50\_h(x) = val(x, h) for h = 0, …, 99.
2. **DTM.** DTM(x) is the value in the unclocked game Γ∞, which has the same moves and no 50-move rule. It is defined as the least fixed point of retrograde analysis: W\_d and L\_d are the positions from which a forced mate in d plies exists, and every other position is D.
3. **WDL class.** cls(x) is WIN if val(x, 0) = W and LOSE if val(x, 0) = L. If val(x, 0) = D, it is CURSED\_WIN when x is won in Γ∞, BLESSED\_LOSS when x is lost in Γ∞, and DRAW otherwise. The class order is LOSE < BLESSED\_LOSS < DRAW < CURSED\_WIN < WIN.
4. **DTZ.** For clean classes, DTZ is the number z(x) defined next. For cursed classes it is the distance val′ of Definition 5.19: the same construction applied inside Γ∞, with cursed distances across castling exits composed as that definition specifies.
5. **DTC** is defined in Section 8.

Definition 2.3.3 is consistent: a strategy that wins (x, 0) reaches mate before the rule can apply, and the same strategy wins in Γ∞, where the loser has exactly the same moves.

### 2.3 The zeroing attractor and the Clock Lemma

Define sets A\_d, where the side to move can force, within d plies, mate or a zeroing move into a position of class LOSE, and B\_d, where the opponent can force the same against the side to move, for d ≥ 0:

- B\_0 is the set of checkmated positions, and A\_0 = ∅.
- x ∈ A\_d (d ≥ 1) iff some move m satisfies one of:
  - m is zeroing and cls(x·m) = LOSE;
  - m is not zeroing and x·m ∈ B\_{d−1}.
- x ∈ B\_d (d ≥ 1) iff x ∈ B\_0, or L(x) ≠ ∅ and every move m satisfies one of:
  - m is zeroing and cls(x·m) = WIN;
  - m is not zeroing and x·m ∈ A\_{d−1}.

The classes cls(x·m) of zeroing children are well defined before x, because μ(x·m) < μ(x). By induction on d, A\_d ⊆ A\_{d+1} and B\_d ⊆ B\_{d+1}. Define z(x) = min{d : x ∈ A\_d ∪ B\_d}, or ∞ if x is in no such set. For clean decisive positions, z(x) is DTZ: the number of plies to the next zeroing move, or to mate, when the winner minimizes and the loser maximizes that number.

**Lemma 2.4 (Clock Lemma).** For every position x and every clock c ∈ {0, …, 100}:

val(x, c) = W iff x ∈ A\_{100−c}, and val(x, c) = L iff x ∈ B\_{100−c}.

*Proof.* Induction on k = 100 − c, with μ as the outer induction for zeroing children. For k = 0, a state with L(x) ≠ ∅ is drawn, so val(x, 100) is L exactly on checkmates (B\_0) and never W (A\_0 = ∅). For k ≥ 1, a zeroing child (y, 0) is L iff cls(y) = LOSE and W iff cls(y) = WIN (Definition 2.3.3), and a non-zeroing child (y, c + 1) is L iff y ∈ B\_{k−1} and W iff y ∈ A\_{k−1} by induction. So x is W iff some child is L iff x ∈ A\_k, and x is L iff it is checkmate, or L(x) ≠ ∅ and every child is W, iff x ∈ B\_k. ∎

**Corollary 2.5.**

1. cls(x) = WIN iff x ∈ A\_{100}, and cls(x) = LOSE iff x ∈ B\_{100}. So a clean decisive x has z(x) ≤ 100, and z(x) > 100 means the class is cursed, blessed or DRAW.
2. *Monotonicity of class in the clock.* val(x, c) = W implies val(x, c′) = W for all c′ ≤ c, and likewise for L. No position lies in both A\_d and B\_{d′} for d, d′ ≤ 100, so val(x, c) is never L for a position that is W at another clock.
3. *Draw threshold.* For clean decisive x with 1 ≤ z(x) ≤ 100, val(x, c) is decisive exactly when c ≤ 100 − z(x). The first drawn clock is therefore h\* = 101 − z(x). This lies in {1, …, 99} when z(x) ≥ 2, and equals 100 (no clock in 0, …, 99 is drawn) when z(x) = 1.

*Proof.* Part 1 is Lemma 2.4 at c = 0. Part 2: monotonicity follows from A\_k ⊆ A\_{k+1} and B\_k ⊆ B\_{k+1}; if x ∈ A\_d ∩ B\_{d′}, then val(x, 100 − max(d, d′)) would be both W and L. Part 3 is Lemma 2.4 with the definition of z. ∎

Section 7 proves that the solver's stored layers change to DRAW exactly at clock h\*, that is, at packed layer h = h\* + 1 = 102 − z(x), and that the reader's formula dtz = 102 − h inverts it.

## 3. Index correctness

The index is correct in the following sense: every legal position, and every one of its symmetric images, maps to exactly one stored cell, and that cell decodes back to one of those images. Every overlap or phantom cell is marked illegal and carries weight 0. A legal placement of a material M puts its men on distinct squares, with the kings non-adjacent, the pawns on ranks 2–7, the men that hold castling rights on their squares and, in a pair material, at least one opposing pair: a white pawn and a black pawn on the same file, the white one on the lower rank. It carries no side to move, so whether a side is in check plays no part in it. D(M) is the set of legal placements of M. We use the following notation. A populated non-pawn class c has N\_c legal squares and k\_c identical pieces, and radix r\_c = C(N\_c, k\_c). The within-slice weights are W\_i = r\_0·…·r\_{i−1}, the within size is S = W\_n, the number of king slices is K and the number of pawn slices is Pn. The number of cells is T = Pn·K·S.

### 3.1 Combinadic ranking

**Lemma 3.1 (combinatorial number system).** For 0 ≤ k ≤ N, the map ρ(c\_0 < … < c\_{k−1}) = Σᵢ C(c\_i, i + 1) is a bijection from the k-subsets of \[0, N) onto \[0, C(N, k)).

*Proof.* Induction on k, trivial for k = 0. With c = c\_{k−1}, the other terms are ρ of a (k − 1)-subset of \[0, c), so lie in \[0, C(c, k − 1)) by induction, and ρ ∈ \[C(c, k), C(c, k) + C(c, k − 1)) = \[C(c, k), C(c + 1, k)). For c = k − 1, …, N − 1 these intervals tile \[0, C(N, k)), and on each ρ is a translate of a bijection. ∎

**Theorem 3.2 (ranking and unranking).** For k ≤ min(N, 7), the combinadic rank ρ is a bijection from sorted k-subsets of the legal squares onto \[0, C(N, k)), and the unrank is its inverse.

*Proof.* The binomial table, built by Pascal's rule, equals C(a, b) for a ≤ 64, b ≤ 7. The rank is ρ ∘ pos with pos strictly increasing on the sorted legal squares, a bijection by Lemma 3.1. The unrank table stores each k-subset, enumerated once by lexicographic successor, at slot ρ. ∎

**Theorem 3.3 (constant-time quiet-move update).** Let L = ρ⁻¹(r), from ∈ L, and let to be a legal square with to ∉ L ∖ {from}. Then the quiet-move rank update of (L, from, to) equals ρ(sort(L − from + to)).

*Proof.* Without the lookup table, the update writes to into the slot of from and moves it by one insertion step toward the side on which to lies, shifting each neighbor it passes one slot back. Since L is sorted and to ∉ L ∖ {from}, the result is sort(L − from + to), which is then ranked. The table exists iff Nᵏ ≤ 2²⁸. Its key ν = Σ Nⁱ·a\_i is a base-N numeral, hence injective, and the table maps all k! orderings of each subset to that subset's rank. Replacing from by to yields such an ordering of the new subset. ∎

### 3.2 Mixed-radix layout and exact division

**Lemma 3.4 (invariant-divisor division).** For 1 < d < 2⁶³ and 0 ≤ n < 2⁶³, invariant-divisor division of n by d yields ⌊n/d⌋.

*Proof.* Let l = ⌊log₂ d⌋. If d = 2ˡ, then magic = 0 and the result is n >> l.

Otherwise the parameters come from a 128-bit division of 2⁶⁴⁺ˡ by d. Its quotient lies in \[2⁶³, 2⁶⁴) and fits in one word, since the high word 2ˡ of the dividend is below d. Its remainder satisfies ρ < d < 2⁶³, so ρ can be doubled without overflow. Doubling gives ⌊2⁶⁵⁺ˡ/d⌋ = 2⌊2⁶⁴⁺ˡ/d⌋ + \[2ρ ≥ d\]. The multiplier is magic = M′ − 2⁶⁴, where M′ = ⌊2⁶⁵⁺ˡ/d⌋ + 1 ∈ \[2⁶⁴ + 1, 2⁶⁵), the doubling wrapping modulo 2⁶⁴. The shift is l + 1.

It remains to evaluate the result. Let q = ⌊magic·n/2⁶⁴⌋. Since q ≤ n, the sum n + q does not overflow, and the result is ⌊M′n/2⁶⁵⁺ˡ⌋. Write n = Qd + r and let e = M′d − 2⁶⁵⁺ˡ, so that e ∈ \[1, d\]. Then M′n/2⁶⁵⁺ˡ = Q + (r + en/2⁶⁵⁺ˡ)/d. Since en < 2ˡ⁺¹·2⁶⁴ = 2⁶⁵⁺ˡ, the floor is Q. ∎

Every divisor in use (within weights, class radices of the storage permutation, king strides, 120, C(48, k), paging group sizes) is below 2⁶³, every invariant divisor built from one exceeds 1 (Lemma 3.7), and every numerator is below T ≤ 2⁴⁸, so Lemma 3.4 applies throughout.

**Lemma 3.5 (ranges at the 8-man limit).** Let a material have at most 8 men and at most 6 pawns, where pair members count as pawns and castling rooks as men. Then (a) T < 2⁴⁷; (b) every per-color, per-type count is at most 6, so every base-9 digit of the material key is at most 6 and the key is below 2³²; (c) every key weight Nⁱ and every key of the lookup table of Theorem 3.3 is at most 2²⁸, and every class radix is at most C(64, 6); (d) the product of the pawn-group radices is below 2³¹.

*Proof.* (a) Class radices are at most 64ᵏ, pawn-group radices at most 48ᵏ, and the pair's 120 ≤ 48². K = 462 without pawns or rights, 1806 with pawns and no rights, and at most 56·64 = 3584 with rights, where a castling rook is in neither Pn nor S. The maximum, one pawn without rights, is T ≤ 1806·48·64⁵ = 93,080,531,238,912 < 2⁴⁷, so generator and reader size tests pass with the same T, and all numerators are below 2⁶³. (b) The 32-bit key is a base-9 numeral over the per-color counts of queens, rooks, bishops, knights and pawns, castling rooks counted as rooks and pair members not counted. Kings have weight 0, and the pair flag and the castling code are separate fields outside it. At most 6 men are not kings, so the digits sum to at most 6 and the key is at most 6·9⁹ = 2,324,522,934 < 2³². Keys of at least 2³⁰ are truncated identically by writer and reader (Theorem 11.6). (c) Nⁱ is stored only if the previous weight is at most ⌊2²⁸/N⌋, the table exists only if Nᵏ ≤ 2²⁸, and a class holds at most 6 men. (d) Among the products C(48, a)·C(48, b) with a + b ≤ 6, and 120·C(48, a)·C(48, b) with a + b ≤ 4 for a pair, the largest are C(48, 3)² = 299,151,616 and, with a pair, 120·C(48, 2)² = 152,686,080, both below 2³¹; for instance C(48, 4)·C(48, 2) = 219,486,240. ∎

**Theorem 3.6 (layout).** On the box B of digit tuples (p, k, d\_0, …, d\_{n−1}):

- Composition is a bijection B → \[0, T), and decomposition is its inverse.
- The odometer is the successor function.
- The single-digit updates compute the composition with one digit replaced.
- The storage id of a surviving pawn-slice cell is the number of surviving cells below it, so the surviving pawn slices are numbered by \[0, Pn).

*Proof.* Σ W\_i(r\_i − 1) = S − 1 by telescoping, giving the mixed-radix bijection. Decomposition extracts the digits by exact division (Lemma 3.4), the final remainder being d\_0 as W\_0 = 1. An odometer carry at digit i adds W\_i − (W\_i − 1) = 1. Composition is linear, so a one-digit update is one addition modulo 2⁶⁴, exact when the new tuple lies in B. The storage id of a surviving pawn-slice cell is the number of surviving cells below it: the cell's 64-cell block records the survivors before the block, and a popcount of the block's bits below the cell counts the rest. ∎

**Lemma 3.7 (degenerate strides).** Suppose S = 1, which holds exactly when every man other than the kings is a pawn or a castling rook, or Pn = 1, which holds exactly for pawnless materials. Then every split of an index into digits, and every computation of a cell's paging group and offset, returns the digits of Theorem 3.6, and no invariant divisor is built from a divisor d ≤ 1.

*Proof.* S ≥ 1, and K·S > 1 since every king-slice count is at least 56, so only S, the slices per paging group and an absent pawn group's radix can equal 1. Invariant divisors are built and used only for divisors above 1, and the reader applies the same guards with ordinary division. In the guarded case of a divisor 1, the quotient is the numerator and the remainder is 0. When Pn = 1, the pawn-slice split is skipped, giving pawn digit 0 with the numerator as remainder; this is exact since the numerator is below K·S. The lowest within digit, of weight W\_0 = 1, is the final remainder, and a pawn group of radix 1 gets digit 0, as in Theorem 3.6. ∎

### 3.3 Canonical representatives

**Lemma 3.8 (fundamental domains).** Let TRI = {(f, r) : r ≤ f ≤ 3}, the ten squares of the a1–d1–d4 triangle. TRI meets every orbit of the dihedral group G₈ on squares exactly once, and files a–d meet every orbit of G₂ = {id, FILE} exactly once. Squares of TRI off the diagonal have trivial stabilizer. Diagonal squares have stabilizer {id, DIAG}.

*Proof.* Folding each coordinate to min(v, 7 − v) and ordering the pair so that f ≥ r gives a G₈ invariant realized by a group element, hence existence and uniqueness; the count 6·8 + 4·4 = 64 confirms the stabilizers. FILE fixes no square. ∎

**Lemma 3.9 (king slices).** Every non-adjacent king pair has exactly one canonical pair in its orbit. The king-slice lookup returns that pair's slice together with a transform that reaches it. The stabilizer of a canonical pair is {id, DIAG} exactly when the stabilizer flag is set, and trivial otherwise.

*Proof.* Let K₀ be the following set of king pairs. For pawnless materials, K₀ contains the pairs with wk ∈ TRI and bk distinct and non-adjacent, with bk ≤ DIAG(bk) when wk is on the diagonal. For materials with pawns, K₀ contains every such pair with wk on files a–d. The canonical pairs are those of K₀, and by Lemma 3.8 each orbit meets K₀ once. For materials with pawns the stabilizer flag is never set; for pawnless materials it is set iff both kings are on the diagonal.

The entry of a pair in K₀ is its own slice with the identity transform. The entry of every other pair is the slice of the K₀ pair in its orbit, with a transform, among the group's eight or two, that reaches that K₀ pair. The table is filled in two passes, first the pairs of K₀ and then the other pairs. The second pass writes into the table it reads and recognizes K₀ entries by their identity transform. No other entry has the identity transform, since a pair canonical under the identity lies in K₀. Adjacent or coincident pairs get no slice. ∎

**Lemma 3.10 (canonical opposing pair).** The key (file, white rank, black rank) determines an opposing pair, so the key-minimal pair is unique. The surviving pawn cells are exactly the triples (pair, W, B) whose pawns stand on distinct squares and whose designated pair is the canonical pair of all the pawns. The map from pawn sets with at least one opposing pair to pawn slices is therefore a bijection. Non-canonical designations are not cells at all: they are removed from storage. ∎

**Lemma 3.11 (mirrored pawn slices).** For every pawn slice s, mirror(s) is the slice of the file-mirrored pawn set, with its designated pair recomputed. It is an involution, and it agrees with the pawn digit that the canonical index I produces after a FILE transform.

*Proof.* Mirroring keeps ranks and colors, so it maps opposing pairs to opposing pairs. Without a pair, each group is mirrored and re-ranked, and the cell survives since disjointness is preserved. With a pair, mirroring reverses the file order in the key (file, white rank, black rank), so the mirrored designated pair need not be canonical; the mirror is obtained by mirroring every pawn, recomputing the canonical pair and ranking the rest as free pawns, giving by Lemma 3.10 the unique surviving cell of the mirrored set. Mirroring twice restores the set, whose canonical pair is unique, so mirror is an involution. I applies FILE to all pawns before the canonical-pair rule, the same computation. ∎

**Theorem 3.12 (canonical index).** Let P be a legal placement of material M with symmetry group G, and let I(P) be the canonical index of P. Then:

- (a) I(P) is defined, and I(hP) = I(P) for every h ∈ G.
- (b) Decoding I(P) and filling the board reproduces gP for some g ∈ G.
- (c) Every cell outside the image of I is an overlap cell, where two men share a square, or a phantom cell, the non-canonical half of a stabilized pair. The generator writes both kinds as ILLEGAL.

*Proof.* Canonicalization applies the transform t of Lemma 3.9 to all men, and in a stabilized slice (pawnless materials only) also DIAG if that lowers the within index. For hP, every g′ canonicalizing the kings has g′h ∈ t·Stab(K), where K is P's king pair. So the candidates {tP, DIAG·tP}, and their minimum, are the same for every image, which proves (a).

The digits come from Lemma 3.10 and Theorems 3.2 and 3.6, each inverted by decoding, which proves (b).

For (c), full decoding fails at every overlap cell, where a man would be placed on an already occupied square. Such a cell is written ILLEGAL by the generator and likewise by the DTM, DTM50 and DTC passes. In a stabilized slice, initialization writes ILLEGAL at every cell whose recomputed canonical index differs from the cell's own. A non-overlap cell of an unstabilized slice decodes to kings canonical under the identity, so I(decode C) = C. Castling materials use the trivial group, and their slice lookup covers exactly the free-king squares neither occupied by a pinned man nor adjacent to the pinned king. Every non-overlap cell C of a castling material satisfies I(decode C) = C by Lemma 3.13, which takes only this coverage from the present theorem. ∎

**Lemma 3.13 (castling cells round-trip).** In a castling material, every cell C whose full decoding succeeds satisfies I(decode C) = C. This covers every Chess960 placement: one right with the king on any file and its rook on either side of it, and two rights with rook files lo < k < hi, where k is the king's file.

*Proof.* Full decoding clears the rights and, for each castling rook r of color c in C's king slice, sets c's h-side right if file(r) > file(k\_c) and its a-side right otherwise, recording r's square. With two rights the grouping enumerates only lo < k < hi, so the sides are distinct. Reading the recorded squares a-side first lists the rooks in the grouping's increasing file order. So the rights counts equal the material's, and the canonical index recovers the placement through its injective base-9 key over the king file and rook files (an absent second rook keyed as 8). The free-king lookup returns C's king slice (Theorem 3.12(c)). A free rook on a castling-rook square would make an overlap cell, whose decoding fails, so removing those squares from the rook class leaves exactly the free rooks and C's within digits. The group is trivial, and the pawn digit is recomputed by Lemma 3.10. ∎

**Theorem 3.14 (orbit weights).** For each cell, the orbit weight equals the size of the orbit the cell represents, or 0 for overlap and phantom cells. Hence Σ weights = \|D(M)\|, the number of legal placements.

*Proof.* Overlap cells get 0 and castling cells 1 (trivial group). File-mirror cells get 2: FILE moves the white king off files a–d, so each orbit has two members, exactly one with the king on files a–d. Unstabilized pawnless cells get 8, since Stab(P) ⊆ Stab(kings) = {id}. In a stabilized slice the table gives 8, 4 or 0 as w\_alt, the within index of the DIAG image, is greater than, equal to or less than w. By injectivity w = w\_alt iff DIAG fixes the position, whose orbit then has 4 elements; otherwise the orbit has 8, meets the slice in exactly w and w\_alt, and 8 goes to the minimum, the canonical cell. ∎

### 3.4 The reader agrees with the generator

**Theorem 3.15.** For every legal placement P, the standalone reader's index equals Φ\_perm(I(P)). The map Φ\_perm keeps the pawn-slice and king-slice digits and re-encodes the within digits in the stored class order. For perm = 0, Φ is the identity.

*Proof.* The reader shares both groupings with the generator and builds its binomial table, legal-square lists, king and pawn slices and configuration by the same rules. Its four differences leave the index unchanged:

- **Unranking.** Greedy: the largest p with C(p, k) ≤ rank is c\_{k−1}, and rank − C(p, k) ranks a (k − 1)-subset of \[0, p). This inverts ρ by the tiling of Lemma 3.1 and stops since C(k − 1, k) = 0.
- **Division.** The reader divides ordinarily, or by invariant divisor for pawn slices, and the generator divides by invariant divisor. All these divisions are exact (Lemma 3.4).
- **Castling rooks.** The reader removes them after canonicalization and the generator before. Canonicalization is skipped when rights exist, so the order is immaterial. The reader refuses a rights count differing from the material's.
- **Layout order.** Composition uses Φ\_perm, and the writer stores cells in that order (Section 11.7). ∎

**Lemma 3.16 (the reader's index is defined).** For every legal position that routing sends to a table, the standalone reader's index is a cell, never the empty sentinel.

*Proof.* Routing picks the table by the position's own rights, so the counts agree and no side holds more than two. The grouping enumerates every king file with every rook-file set that can hold rights, and a legal position has no adjacent kings and no free king on a pinned man's square, so the king-slice lookups are defined (Lemma 3.9, Theorem 3.12(c)). The canonical pair is computed over all pawns, so the pawn digit is defined (Lemma 3.10). ∎

### 3.5 Incremental decoding

**Lemma 3.17 (incremental decoding).** Suppose the board holds a complete, overlap-free cell C with placements P, and C′ lies in the same slice, meaning the same combined king and pawn slice. Then checked incremental decoding of C′ returns true exactly when C′ has no overlap, and in that case it leaves the board and placements equal to those of the full decoding of C′. Unchecked incremental decoding produces the same board whenever C′ has no overlap.

*Proof.* The slice fixes every man outside the within classes. Changed classes are cleared before any new square is placed, and each new square is tested against the unchanged men and the classes already placed; squares of one class are distinct (Theorem 3.2), so overlap is reported exactly when full decoding reports it. ∎

**Lemma 3.18 (the board cache).** The board cache patches only when the cached cell decoded as legal and lies in the same slice, and otherwise decodes in full. Every unchecked decode happens at a cell that is overlap-free. So the precondition of Lemma 3.17 always holds, and every decoded board equals the full decoding of its cell.

*Proof.* Unchecked decodes occur in three places:

- **Initialization.** The first pass of each solver, DTM, DTM50 and DTC included, decodes checked and uses the cached board only at cells that pass the check. The later passes, namely the DTM50 layers and the DTC budgets k > 0, filter on the stored ILLEGAL marker, which is sound: a cell not marked ILLEGAL is overlap-free by Theorem 3.12(c).
- **Solver passes.** Every decode in a mark, cell or propagation loop comes after a filter that admits only a win, loss or intermediate entry, never ILLEGAL. So the cell is overlap-free by Theorem 3.12(c).
- **transcribe.** Its legal-cell iterator decodes checked before the board is used, and the relaxation predicates take their board from that iterator.

An unchecked decode of an overlap-free cell returns true, so the cache holds only verified boards. A failed checked decode clears the legality flag, forcing a full decode next. The flag starts clear, so the empty sentinel never reaches the same-slice test. Moves made on the cached board (the sub-table read) are unmade, restoring every changed field. Positions carry no en-passant state. ∎

**Lemma 3.19 (legal cells in transcribe).** transcribe's legal-cell test holds at a cell i exactly when the generator's final entry at i is not ILLEGAL. The reader never queries a cell at which the test fails.

*Proof.* By Theorem 5.4, initialization writes ILLEGAL exactly where the checked decode fails, where a stabilized slice's decoded placement has canonical index other than i, or where the decoded placement with that side to move is illegal. No later pass writes ILLEGAL. transcribe's test, for the same side to move, is the conjunction of the same three tests. The reader's index of a legal P is the image of I(P) (Theorems 3.12(a), 3.15), whose entry is not ILLEGAL, so failing cells are never queried. transcribe's other don't-care cells, the omitted cells of relaxed distance frames, are treated in Theorem 12.32. ∎

**Lemma 3.20 (castling with coincident squares).** Making and then unmaking a castling move is exact for every castling placement. This includes placements where the king's destination is the rook's origin, where the king already stands on its destination, and where the rook already stands on its destination.

*Proof.* Let A = {k₀, r₀} be the origins and B = {k₁, r₁} the destinations, with k₀ ≠ r₀ and k₁ ≠ r₁. Making the move empties both origins before placing both men, so every square is right whichever coincide. The king's and rook's sets change by {k₀} Δ {k₁} and {r₀} Δ {r₁}, and the occupancy and mover's color set by A Δ B: squares in A ∩ B stay occupied, those in A ∖ B are vacated, and those in B ∖ A were empty, since the castling path must be empty apart from k₀ and r₀. So the occupancy becomes (occ ∖ A) ∪ B. No move writes the rights' rook squares. Unmaking restores the rights first, so it reads the same r₀, and reverses the same differences and assignments. ∎

## 4. Well-founded scheduling

Every value the solver reads is final before it is read. This rests on three orders:

- materials are built in dependency order;
- pawn slices are processed in batches of strictly increasing pawn life;
- retrograde search stays inside a pawn slice and its file mirror.

### 4.1 The material dependency order

For a material X, let M be its number of men (pair pawns and castling rooks included), C its number of castling rights, and Pw its free pawns plus 2 for an opposing pair. Define R(X) = M + C + Pw.

**Theorem 4.1 (well-foundedness).** R strictly decreases along every edge that the dependency closure follows.

| Edge | ΔM | ΔC | ΔPw | ΔR |
| --- | --: | --: | --: | --: |
| Capture, no pair | −1 | 0 | ≤ 0 | ≤ −1 |
| Capture in a pair material (p → PP) | −1 | 0 | 0 or −1 | ≤ −1 |
| Pair member captured (p → P) | −1 | 0 | −1 | −2 |
| k rights dropped | 0 | −k | 0 | −k |
| Castling rook captured | −1 | −1 | 0 | −2 |
| Promotion | 0 | 0 | −1 | −1 |

So the relation is well-founded, with recursion depth at most R. R is invariant under canonical renaming, which permutes pieces and may swap colors with their rights.

**Theorem 4.2 (post-order).** Let I(list) be the invariant "every entry's direct dependencies appear earlier in the list". If I holds when the dependency closure is run on X, it holds afterwards, and X and all its transitive dependencies are present.

*Proof.* Induction on R. A call either returns early because its argument is present (sound under I), or appends its argument after all its children. No recursive call appends X, since R strictly decreases along every edge (Theorem 4.1), so X is no transitive dependency of itself. That is therefore its only append of a new element, so I is preserved. Membership is tested by material key, compared together with the separately stored pair flag and castling code. The key is a base-9 numeral of the per-color, per-type counts of the non-king men, castling rooks included and pair members excluded. It is injective while every count is at most 8. ∎

The material list is built only by the dependency closure. Bare kings are then removed without reordering, so every sub-table is generated before any material that reads it.

**Lemma 4.3 (compound children).** Let X be a material in the list. Its children are the targets of its captures, promotions, captures with promotion and castling exits, and in a castling material also the targets of its captures combined with the rights the move drops. (a) Every table that the generator of X reads or opens is present and precedes X. (b) For every capture, promotion and castling exit from a legal cell of X, the sub-table read selects one of these tables.

*Proof.* (a) The dependency closure follows five single-move edges, each changing only what its move changes:

E1, a free man is captured (in a pair material the pair becomes two free pawns);

E2, a pair member is captured and its partner becomes a free pawn;

E3(c, d), color c drops d ≥ 1 rights and their rooks become free rooks;

E4(c), a castling rook of c is captured and c loses one right;

E5, a pawn is promoted.

Let F be the free men of X. Each child is reached by a chain of these edges:

a capture, a promotion, a captured pair member and an exit are E1, E5, E2 and E3 respectively;

a capture of the free man j with a promotion, without rights, is E1(j) then E5: E1 keeps the pawn free, and promoting any pawn of that color to that piece gives the same material;

in a castling material, a capture of the free man j dropping d ≥ 0 of the mover's rights, with a promotion only when d = 0, is E1(j) followed by E3(mover, d) when d ≥ 1, or by E5;

a capture of a castling rook of color v dropping d of the mover's rights, possibly with a promotion, is E4(v) then E3(mover, d) or E5 when there is no pair. With a pair, the child is F plus two free pawns, with no pair and one right of v fewer: E3(v, 1), then E1 on the rook it freed, then E3(mover, d) or E5;

a capture of a pair member of color v in a castling material dropping d of the mover's rights is E2 with the survivor of color opp(v), then E3(mover, d);

the rights-free children with a capture and a promotion opened for a castling material are E4 once per right, then E1(j), then E5.

By Theorem 4.2 each edge's target precedes its source, so every child precedes X, and bare-king removal keeps the order. A table is determined by its material (multiset of men, pair flag, rights) up to color exchange, and each child built has the material of its chain's end, so they are the same table.

(b) We treat exits, captures and promotions in a castling material, and moves in a material without rights, in turn.

- *Exits.* An exit drops all of c's rights when c's king moves, and one right when a rook leaves a castling-rook square of c. So 1 ≤ d ≤ rights(c), and all these tables are prepared.
- *Captures in a castling material.* A capture selects its table by d, the victim, whether the victim holds a right, and the promotion; a capture of a pair member selects by d and the member's color. If d > 0, the mover drops the rights, since only c's king and castling rooks stand on c's king and castling-rook squares. So d ≤ rights(mover). The victims are prepared as follows.
  - A rook captured on a castling-rook square of its color v has rights(v) ≥ 1, and it is prepared as a rook holding a right.
  - A capture on a pair square is prepared for every d.
  - Any other victim is a free non-king man, the only kind that full decoding places there.
- *Captures with promotion.* A capture with promotion is made by a seventh-rank pawn on a non-pawn on the last rank. The pawn stands on no king or rook square, so d = 0, and in no pair, so it is free. Its table is prepared.
- *Promotions in a castling material.* A non-capturing promotion selects its pawn's table with rights kept. The pawn is on no king or castling-rook square, and its destination is empty while all those squares are occupied, so no right drops. This table is prepared for every free pawn.
- *Materials without rights.* A capture on a pair square selects E2, and any other capture selects its free victim's table. A promotion selects a free pawn's table, and a capture with promotion selects that of a free pawn and an enemy free non-king man. All of these are prepared. ∎

### 4.2 The pawn-slice order

For a pawn slice, life is the sum of the lives of its pawns, pair members included.

**Lemma 4.4.** A non-capturing, non-promoting pawn push lowers slice life by exactly 1 (single push) or 2 (double push). File mirroring preserves life.

**Lemma 4.5 (push targets are valid cells).** Every target produced by the push-target map is a surviving cell.

*Proof.* Targets, and the intermediate square of a double push, are checked against every pawn square. No push jumps a pawn on its file, so the opposing pairs are unchanged as sets of pawn identities. Let the designated pair D have key (f₀, a, b). Four cases:

1. **A white push of another pawn** only raises the keys of that pawn's own pairs.
2. **A white push of D's white pawn from a to a′.** A smaller new key needs a white pawn on file f₀ with rank in (a, a′\], but those squares were checked empty, or a black pawn with a lower rank above D's white pawn, which already contradicts D's minimality.
3. **A black push of another pawn z.** A pair (y, z) whose key drops below D's has y = D's white pawn; then z is above D's black pawn and cannot pass it, so its rank stays above b.
4. **A black push of D's black pawn** lowers only the keys of pairs containing it; every such pair other than D has its white pawn above rank a, since a white pawn below rank a on file f₀ would pair with D's black pawn at a smaller key, so its key stays above D's.

So D stays canonical and the target survives. ∎

**Theorem 4.6 (topological batches).** Batching buckets slices by life in increasing order and keeps one representative per mirror pair. For every slice s and every push target t of s or of mirror(s), the target lies in a strictly earlier batch. No member of a batch's mirror expansion pushes into another member of the same batch.

*Proof.* life(t) < life(s) by Lemma 4.4, mirroring preserves life, and buckets are strictly ordered by life. The bound life ≤ 36 holds as there are at most 6 pawns. ∎

The push-target map ignores blocking by pieces, so it emits a superset of the real pushes. The ordering argument needs only this superset property.

### 4.3 Retrograde moves stay inside a mirror pair

The retrograde generator emits only quiet moves by knights, bishops, rooks, queens and the king of the side that just moved, all onto empty squares. It emits no pawn moves, so pawn squares are unchanged.

**Theorem 4.7.** Let m be a non-capturing, non-promoting move from a legal canonical cell in pawn slice s, and let Q be the resulting board.

- (i) The quiet-move index map returns the empty sentinel exactly when m is a castling-rights exit, or m is a king move that leaves the two kings adjacent (so Q is not legal). Applied to a retrograde move, the empty sentinel means that the material has no such predecessor. Rights are only ever lost, so no position of the material has a rights holder arriving on its square.
- (ii) Otherwise it returns I(Q), the canonical index of Theorem 3.12.
- (iii) For a non-pawn move, the pawn slice of I(Q) is s, except in file-mirror materials, where a king move that forces the FILE transform gives mirror(s). A pawn push gives a push target of s (Lemma 4.5).

*Proof.* The map tries its branches in order:

- **A castling-rights holder moves.** A move of the king or a castling rook of a side with rights is an exit, and the map returns the empty sentinel.
- **The free king moves in a castling material.** The free-king lookup gives the slice with the king on its new square, or none exactly when that square is adjacent to the pinned king (Theorem 3.12); with the trivial group, only the king digit changes.
- **Any other king move.** The king-slice lookup fails exactly for adjacent kings. With identity transform and an unstabilized new slice the placement is canonical (Lemma 3.9, Theorem 3.12(a)) and only the king digit changes; otherwise the fallback recomputes I(Q) with the transform and, in a stabilized slice, the DIAG minimum.
- **A non-king move in a stabilized slice.** Such a slice is pawnless. The move goes to the fallback, since any within digit can change the DIAG minimum.
- **A pawn push.** The kings stay, so the transform stays the identity; the single-push slice update replaces the pair digit or a free-pawn digit, giving the target that the push-target map emits for this push, which survives (Lemma 4.5). A double push takes the same slice update: its middle square is empty, so no pawn is jumped and the target survives (Lemma 4.5). Separately, the opponent's en-passant replies are read from the board decoded from the full cell I(Q) of the pushed position (Lemma 4.11). Its kings are those of the canonical parent, so the transform is the identity and the decoded board is the pushed position.
- **Any other piece.** Theorem 3.3 gives the new within digit, and Theorem 3.6 the new index.

Only the fallback's transform can change the pawn digit; in file-mirror materials it is the identity or FILE, giving s or mirror(s) (Lemma 3.11). ∎

A fusion is a union of mirror pairs of one batch, and the solvers operate on fusions. No push leads from one pair to another (Theorem 4.6), and quiet moves stay inside a pair (Theorem 4.7), so the pairs of a fusion do not interact.

### 4.4 Consequence for the solvers

**Corollary 4.8 (finality of inputs).** During the solve of a mirror pair {s, mirror(s)} of material M, every value read through a zeroing move or an exit is already final. There are two cases:

- **Captures, promotions and exits** read a sub-table or an exit twin, which is complete by Theorem 4.2 and Lemma 4.3.
- **Pawn pushes** read a slice of an earlier batch, by Theorem 4.6.

Every quiet move stays inside {s, mirror(s)} (Theorem 4.7), so the only cyclic dependencies, those among quiet moves (Lemma 2.1), are resolved inside a single solve, the subject of Sections 5–8.

**Lemma 4.9 (sub-table reads).** For every capture, promotion and castling exit, the sub-table read takes the child's value from the right cell of the right table.

*Proof.*

- **Material.** In a castling material captures go to the castling tables, since a captured rook may hold a right and a capturing king or rook gives its rights up. A pair-pawn capture goes to the pair-broken survivor table, and any other capture or promotion to the table keyed by (mover, victim, promotion), which fixes the child's material whichever man captured. En passant takes the victim on (rank of from, file of to), so a double-pushed pair pawn is recognized.
- **Pairs.** Every child that stays in a pair material has the opposing pair the canonical-pair rule requires: quiet moves and exits keep the pawns, pushes keep the pairs (Lemma 4.5), and a promoting pawn is on its seventh rank, in no pair. Captures go to pair-free materials, which index every placement, including those with an opposing pair.
- **Rights.** A captured rook holds a right exactly when its square is one of the king slice's castling rook squares, from which full decoding set the board's rights.
- **Color.** A symmetric child table stores only White to move. A read for Black is made on the color-mirrored position with White to move, which lies in the same material and has the same value (Lemma 4.12).
- **Index.** The cell is Theorem 3.12 applied to the board after making the move. The sub-table read then undoes the move (Lemma 3.18). ∎

**Lemma 4.10 (rights after a move).** Let x be a legal cell of a castling material and m a move from x. After m is made, every remaining right of a color c has c's king on its home square and its rook on the square recorded for that right. Hence the child's index removes exactly the rooks of those rights, their number equals the child's rights, and the remaining rooks form a class of the size that the child's rank ρ expects.

*Proof.* Full decoding sets each right with c's king and rook on their squares (Lemma 4.9, Rights). Before the board changes, a move of c's king clears all of c's rights, and a right is cleared when its rook square is m's origin or destination (the rook moves, is captured, or is captured with promotion). Kings are never captured, so neither square of a surviving right is m's origin or destination; the other squares m can change are the castling rook's, whose rights the mover's king move clears, and an en-passant victim's, on rank 4 or 5. So each surviving right's men stay put, and the remaining rook squares, in file order, hold c's rooks. The read selects the child by d (all the mover's rights if the origin is its king square, 1 if one of its castling-rook squares, else 0) and by whether the victim holds a right (the destination is a castling-rook square of its color): the tests that clear the board's rights. So the child's rights equal the board's, likewise for exits (same d) and for material-preserving moves (no right changes). The rights-count test passes, and removing those squares leaves the child's free-rook count, the subset size of its ρ (Theorem 3.2). ∎

**Lemma 4.11 (pawn captures in pair materials).** In a pair material with free men F, a capture of a pawn v of color c, en passant included, selects the table of the material F + P\_{opp(c)}, with the rights the move leaves, whether or not v is a member of the designated pair and whichever opposing pair the cell designates. Here P\_c denotes a pawn of color c.

*Proof.* If v is on a designated pair square, the read selects the pair-broken survivor table, of material F + P\_{opp(c)}. Otherwise v is a free pawn of color c, and the read selects that capture's table, of material (F − P\_c) + P\_W + P\_B, the pair becoming two free pawns (edge E1 of Lemma 4.3). These materials are equal and pair-free. In a castling material both tables carry the rights the move leaves (Lemma 4.10), so both branches name one table. If F has no pawn of color c, v is the pair member and the first branch applies.

Neither branch promotes: pawns lie on ranks 2 to 7, and a capture with promotion takes a man on rank 1 or 8. The selected tables are prepared (Lemma 4.3), and in either branch the child's cell is I of the board after the move (Lemma 4.9, Index). The color decision depends only on the child's material.

For en-passant replies to a double push, the pushed cell is I of the pushed position. Its kings are those of the canonical parent, so the transform is the identity (castling materials have the trivial group). Hence the decoded board is the pushed position, and each reply reads the right child whatever pair the cell designates. So the en-passant clause of Lemma 4.9 does not depend on Lemma 4.5. ∎

**Lemma 4.12 (the color mirror).** The color mirror maps a legal position of a material with pair flag π and rights (w, b) to a legal position of the color-swapped material with pair flag π and rights (b, w). Hence, for every symmetric material, in particular one with a pair or with rights, the value of a position with Black to move equals the stored White value of its mirror, both in the sub-table read of a symmetric child and in the prober's read of a symmetric table.

*Proof.* The mirror reflects squares in the midline between the fourth and fifth ranks and swaps colors. With ranks from 0, a white pawn on (f, a) and a black pawn on (f, b), a < b, become a black pawn on (f, 7 − a) and a white pawn on (f, 7 − b), with 7 − b < 7 − a and ranks in \[1, 6\], so opposing pairs map bijectively to opposing pairs. A right of c becomes one of opp(c) on its home rank, on the same side since files are kept, so the rights counts are exchanged and the image lies in the color-swapped material, which for a symmetric material is itself (so w = b). By color symmetry the mirror, with the other side to move, has the same value. A symmetric table is solved for both colors and saved without its Black frame, so its White frame is exact. Both the sub-table read and the prober replace a Black-frame read by a White-frame read of the mirror, which lies in the same material and has the same value. ∎

### 4.5 The build pipeline

The build pipeline processes the materials of the list in order. For each material it runs the enabled stages in the order DTZ, DTC, DTM, DTM50. The DTZ stage also writes WDL. The DTC stage runs only for materials with pawns. The DTM stage runs also when only DTM50 is enabled. A stage is skipped when the file it tests is already found (Lemma 4.15).

**Lemma 4.13 (file names).** Canonical renaming maps a material and its color exchange to the same configuration, and applying it to a canonical configuration changes nothing. The file name of a canonical configuration determines that configuration. So distinct materials have distinct file names, and the generator and every reader form the same name for a material.

*Proof.* With s the strength sum, k the per-type counts (a right counted as a rook) and r the rights count of a color, renaming exchanges colors iff s\_B > s\_W, or s\_B = s\_W and k\_B > k\_W, or both tie and r\_W > r\_B. Exchange swaps each compared pair, so the first unequal one leaves exactly one orientation unexchanged, reached from both inputs; if all are equal the orientations coincide. The pair flag is carried, men are sorted injectively on kinds, and canonical input is not exchanged, so renaming is idempotent. The name lists each color's men from its king, a right as a lowercase r and a pair as a lowercase p. White's r comes directly after White's rooks, before any later white man and hence before Black's king; Black's r comes after Black's king. Each color's p ends that color's men, so White's p stands just before Black's king and Black's p ends the name. Parsing switches color at the second king and assigns each r by its side of it, so it inverts naming on canonical configurations, the only ones generator and readers name. ∎

Two such names may differ only in letter case, as KrK and KRK do. The key stored in the file header does not separate them (Theorem 11.6). Every table lookup matches the exact name, so a lookup finds the file of its own material or none.

**Lemma 4.14 (open child readers).** When the generator of X reads a child through a capture, a promotion, a capture with promotion or a castling exit, a reader of the child's table is open exactly when the child is not K v K. A read that finds no reader returns DRAW, which is K v K's exact value in every metric. Two children registered under equal keys are the same table.

*Proof.* The generator registers every child the read can select (those of Lemma 4.3, with both colors' pair-broken survivor tables and c's exit twins for 1 ≤ d ≤ rights(c)) and opens a reader for each except bare kings. It refuses when a file is missing. Opening lists every registered child (for a rights-free capture with promotion, removing the victim and replacing the pawn give the same men), and in a castling material also the rights-free capture-with-promotion children of Lemma 4.3, which no read selects. Children are keyed by the smaller of the keys of their two color orientations, each key being the material key together with the pair flag and the castling code of that orientation, and equal keys mean the same canonical configuration (Theorem 4.2, Lemma 4.13), hence the same table. Exit twins keep every man, so are never bare kings. By Lemma 4.3(b) a read from a legal cell selects a registered child, so a read with no reader is of K v K, reached when a capture removes the last other man, castling rook included. There no king can give check, so every position is drawn at every clock and in Γ∞: DRAW is exact for DTZ, DTM and DTC, and for DTM50 by Lemma 7.5. ∎

**Lemma 4.15 (the skip test).** A stage skips X exactly when the first search directory that holds the file it tests (X's WDL file for the DTZ stage, X's file of the stage's metric otherwise) holds one with that metric's magic and no frame flagged dropped, loss-only or relaxed. Every generator output passes it, and every file that passes is a complete generator output or a flag-free re-encoding of one by transcribe or shrink, which decodes every cell to the same entry.

*Proof.* A file gets its final name only by renaming a temporary file after its body and checksum are written, except shrink's unchanged copies, which copy such a file directly, and temporary names are never searched, so found files are complete. Generators write only singular and payload frames, never flagged loss-only or relaxed, so their output passes. The DTZ stage tests only the WDL file. It renames the WDL file and then the DTZ file, with no return between, so X's WDL file passes without a passing DTZ file only if the run stopped between the renames or a tool later replaced the DTZ file by a flagged one. Then nothing reads the missing file: X's DTC stage refuses, a parent's exit read fails to open it, and DTM and DTM50 do not read DTZ. ∎

**Lemma 4.16 (one file per material and metric).** For each material and metric, the skip decision and every later read refer to the same file, and once a stage has written X's file, that file is the one found.

*Proof.* Every write goes to the metric's output directory, which heads its search list, and the skip test and every reader locate files through the same first-match lookup. ∎

**Theorem 4.17 (complete and current inputs).** When a stage of X starts, every file it reads either exists under its final name and is a complete generator output, or fails to open and the stage refuses. The files read are:

- DTZ: the WDL files of the sub-tables, and the DTZ and WDL files of the exit twins;
- DTC, for X with pawns: the WDL files of the sub-tables, the DTC and WDL files of the exit twins, and the DTZ and WDL files of X;
- DTM: the DTM and WDL files of the sub-tables and of the exit twins;
- DTM50: the DTM50 and WDL files of the sub-tables, read at clock 0, the DTM50 and WDL files of the exit twins, and, when X is saved, the DTM and WDL files of X.

Each such file holds the exact table of its material, up to the one-byte tier's cursed rounding (Theorem 11.9). Exit reads absorb that rounding (Definition 5.19), and DTC compares only clean DTZ values and copies the decoded DTZ values into its unbounded layer. A WDL file and a metric file of one material agree.

*Proof.* Induction on X's place in the list, then on stage order. Sub-tables and exit twins precede X (Lemma 4.3(a)). Stages run sequentially, and each earlier stage has found its output (Lemma 4.15) or renamed it into place. A failed stage leaves no file under its final name, so reading it fails to open instead of returning a value. The same stages run for every material except DTC, for pawn materials only; exit twins keep every pawn, so those of a pawn material have DTC files, and DTC reads sub-tables only through WDL. X's own inputs are guarded: DTC refuses without X's DTZ file, DTM50 without X's DTM file, and X's WDL file comes from its DTZ stage, which runs first. A stage writes only its own metric's file of X, never reads it, and replaces files only by rename, so bytes behind an open mapping never change. The file read is the file found or written (Lemma 4.16). By induction each file is its material's exact table (Theorems 5.20, 6.12, 7.2, 8.3) with run-independent bytes (Theorem 10.20), so a file left by an earlier run of the same binary equals this run's, and a material's WDL and metric files, decoded together, agree. ∎

## 5. WDL/DTZ solver correctness

The solver computes the 50-move-rule class and the DTZ of every cell exactly, composing cursed distances across castling exits as Definition 5.19 specifies. The WDL projection, and hence every WDL file, is exact.

### 5.1 Target semantics

Fix a material with a set of castling rights. For a legal canonical cell x, moves split into zeroing moves Z(x), exits E(x) and quiet moves Q(x). The value domain is W\_d (1 ≤ d ≤ 100), CW\_d (d ≥ 1), D, BL\_d (d ≥ 1) and L\_d (0 ≤ d ≤ 100). The mover's preference ≻ compares class first. Within W and CW a smaller d is better; within BL and L a larger d is better.

Each move offers the mover an option.

- **A zeroing move** into a child of class k offers τ(k): τ(LOSE) = W\_1, τ(BL) = CW\_1, τ(D) = D, τ(CW) = BL\_1, τ(WIN) = L\_1.
- **A quiet move or exit** into a child of value v offers s(v):
  - s(L\_d) = W\_{d+1}, demoted to CW\_{d+1} if d + 1 > 100;
  - s(BL\_d) = CW\_{d+1};
  - s(D) = D;
  - s(CW\_d) = BL\_{d+1};
  - s(W\_d) = L\_{d+1}, demoted to BL\_{d+1} if d + 1 > 100.

**Definition 5.1 (Bellman system B).**

- val(x) = L\_0 if x is checkmated.
- val(x) = D if x is stalemated.
- Otherwise val(x) is the ≻-maximum over x's options.

**Lemma 5.2.** B has exactly one solution. Its clean band coincides with the zeroing attractor of Section 2.3 (val(x) = W\_d iff x ∈ A\_d ∖ A\_{d−1}, and dually for losses). So the class of val(x) is cls(x), and on the clean band its index is DTZ. The proof uses of an exit option only that a cursed twin value yields a cursed option, so it applies unchanged to the solution val′ of B under the exit composition of Definition 5.19, whose cursed index is DTZ by Definition 2.3.4.

*Proof.* A solution exists: assign the clean band from A\_d and B\_d, then CW\_d and BL\_d by increasing d from the options already assigned, and D to every remaining cell. A remaining cell has no W or CW option, or it would have been assigned at that option's stratum, and not only L and BL options, or it would have been assigned at the stratum of its largest such distance, so its maximum is D. Uniqueness goes by strata. By induction on d, W\_d and L\_d agree between any two solutions: a W\_d's witness option exists in both, and an L\_d has the same options in both. With the clean band fixed, CW\_d and BL\_d agree by the same induction, and every remaining cell is D in both. The clean stratum is the recurrence of A\_d and B\_d, and its demotion at 100 matches Corollary 2.5.1. A cell is W or CW iff it is won in Γ∞, and L or BL iff it is lost in Γ∞. By induction on d: the witness option of a W or CW cell comes from a zeroing child of class LOSE or BLESSED\_LOSS, which is lost in Γ∞, from an exit twin lost in Γ∞, or from a quiet child L or BL of smaller distance; every option of an L or BL cell comes from a child won in Γ∞ or from a quiet child W or CW of smaller distance. Conversely, by induction on the rank of the Γ∞ attractor, a won cell has a move to a lost cell of smaller rank, which is L or BL, so the cell is W or CW; every move of a lost cell leads to a won cell of smaller rank, which is W or CW, so the cell is L or BL. ∎

The solver's cell word decodes to a class as follows: a win is CW if tagged or its value exceeds 100, a loss is BL if tagged or its value exceeds 100, and a word with neither result bit is D.

**Lemma 5.3 (cell words).** In every solver the classes of a final word are pairwise disjoint. In DTZ, DTM and DTC the final decoding reads every intermediate word as D and not as ILLEGAL. In DTM50 a converted builder word is never ILLEGAL (see the remark before Lemma 7.3). The change flag, the hint bits and the provenance bits change neither the value nor the class.

*Proof.* A final word holds its value in bits 0–10 and its win and loss flags in bits 11–12. ILLEGAL is 0x7FF with no flag, and decisive values are at most 0x7FE under RANGE (Section 13.1), and every write sets at most one of the two result bits, the win flag for a win and the loss flag for a loss, so the classes are disjoint. In DTZ, DTM and DTC an intermediate has a bound ≤ 0x7FE in bits 0–10 (in DTM, 1 plus a child DTM ≤ 2045) and flags only in bits 13–15, so it reads as D, not ILLEGAL. The value test reads bits 0–10 and the result test bits 11–12. DTZ's cursed test reads bits 13–14, which on a final word are its tags and on an intermediate are hints that no class test reaches, since without a result bit the class is D. No test reads the change bit, so the change, hint and provenance bits affect neither value nor class. ∎

### 5.2 Initialization

For a cell x, let Σ be the best option over Z(x) ∪ E(x), let β\_c be the largest clean exit-loss distance, and let β\_k be the largest composed cursed exit-loss distance.

**Theorem 5.4.** After initialization, every in-pair cell satisfies the case that applies to it:

| Case | Entry written | Status |
| --- | --- | --- |
| Non-position, non-canonical, or side not to move in check | ILLEGAL | exact |
| Checkmate | L\_0 | exact |
| Stalemate | bare intermediate (reads D) | exact |
| A zeroing move reaches LOSE | W\_1 | exact (minimum possible) |
| Best is a clean exit win, distance e | W\_e | upper bound; repaired by Theorem 5.15 |
| Best is a cursed exit win, and no zeroing move reaches BL | tagged CW\_e | upper bound |
| No quiet move | final value from Σ, β\_c, β\_k | exact (cursed exits up to +1, Section 5.6) |
| Quiet moves remain | intermediate with hint *cwin*, *closs*(β\_k), *draw* or *bound*(β\_c) | sound summary of Z ∪ E |

In addition, every legal cell is flagged on the frontier. We write M for the largest initial value or bound, aggregated over the fusion (Lemma 5.21).

*Proof.* Zeroing children map through τ. An exit composes a clean twin's DTZ as that value plus one, and a cursed twin value u as (u + 1) \| 1 (Definition 5.19). As in s, the cursed-band test sends a twin value of 100 into the cursed band. A double push is valued together with the opponent's en-passant replies, which the legal-move generator omits. These replies are generated on the post-push board and read through the decoded board of the pushed position, which equals the post-push board since no king moves. Only the class of the child is needed, since the push zeroes. Each hint records what the loss check relies on:

- *bound*(b): every option of Z ∪ E is a clean loss, each zeroing option is L\_1, and the largest exit loss is L\_b, where b = 0 when x has no exit.
- *closs*(b): no option of Z ∪ E is D, CW or W, and the largest BL distance over Z ∪ E is max(b, \[Z ≠ ∅\]). Here b ≥ 1 whenever an exit supplies a BL option, since a composed cursed value (u + 1) \| 1 is at least 1. ∎

### 5.3 The schedule

Passes run in the order P(W, 0), P(B, 0), P(W, 1), P(B, 1), …, with timestamps t(W, p) = 2p and t(B, p) = 2p + 1. Each pass of positive ply has a mark sub-phase and then a verify sub-phase; the passes at ply 0 have only the verify sub-phase. The mark value is mcv(c, p) = p − \[c = WHITE\].

**Lemma 5.5 (the stagger).** Let x of color c have val(x) = L\_{v+1}, attained through a quiet child y with val(y) = W\_v. The pass that marks y's predecessors falls strictly between x's checks at ply v and ply v + 1:

- For c = W, y is written by time 2v − 2, marked at 2v + 1, and x is checked at 2v and 2v + 2.
- For c = B, y is written by time 2v − 1, marked at 2v + 2, and x is checked at 2v + 1 and 2v + 3.

So x is flagged after its check at ply v and before its check at ply v + 1. ∎

**Lemma 5.6 (frontier).** A cell that is due at a pass has its group and chunk bits set when the pass reaches it.

*Proof.* Every write that can create future work sets both bits, and so does the rewrite that clears a change flag after a failed check. A chunk is cleared at ply p only if it holds no *pending intermediate* and its largest classified value m satisfies m + 1 < p. An intermediate is pending at ply p if it carries a *cwin* or *closs* hint or the change flag, or if its stored bound b satisfies b ≥ p; the test reads the entries as they stand before re-verification. So a cell with hint *bound*(b) keeps its chunk flagged through ply b, at which it is due, and a *cwin* or *closs* cell keeps it flagged while it stays intermediate. In the CLEAN phase a tagged cell of value v contributes v + 100 to m. The writes that set no bit:

- The ply-0 win-in-1 lies in a chunk that cannot be cleared before ply 3.
- Losses are propagated immediately, and a clean L\_100 keeps m ≥ 100 until cursed ply 100.
- A promoted CW\_1 keeps m ≥ 1.

Further:

- A chunk only partly inside the dispatched range is never cleared (Lemma 5.7).
- Frontier bits are never reset between fusions. This is safe, since a set bit only causes a scan and initialization sets the bit of every legal cell.
- A tagged cell of value v, counted as v + 100, keeps its chunk flagged through the CLEAN phase and up to cursed ply v + 1. ∎

**Lemma 5.7 (straddling chunks).** In DTZ, DTM, DTM50 and DTC, during the solve of a fusion, a chunk's frontier byte is cleared only by a scan whose range is the whole aligned block of C cells. Hence a chunk that meets two groups is never cleared during that solve, and its byte flags every chunk that holds a trigger in either group.

*Proof.* DTZ, DTM and DTC set up frontier bytes once per table. DTM50 zeroes them at the start of each fusion, and its initialization flags the chunk of every legal in-pair cell in both colors; these are the cells its passes act on. Thereafter a byte is set by writes or reseeding and cleared only by scans. Group boundaries (Lemma 10.2: \[gσS, min((g + 1)σ, Pn·K)·S)) need not be multiples of C, and groups are scanned in separate dispatches, each over its own cells. By Theorem 9.1 each dispatched range lies in one aligned block and has length C only if it is the whole block, so no range over a block meeting two groups has length C. Every clear tests for length C, so such a byte is only ever set. A set byte only causes a scan, so it flags every chunk holding a trigger in either group, as Lemmas 5.6, 6.8 and 10.12 require. ∎

### 5.4 The CLEAN phase

**Lemma 5.8 (clean check).** Suppose every opposite-color win of value below p is exact. Then the clean loss check confirms x at ply p iff val(x) = L\_p.

*Proof.* A stalemate is stored as the bare intermediate, the word with no hint, no flag and bound 0. The same word is stored for a drawn cell without quiet moves and for a *bound*(0) cell. A stalemate and a drawn cell without quiet moves are no predecessor of any cell (Lemma 5.10), so marking never flags them, and bound 0 is never due at a positive ply, so neither is ever checked. This matters for the drawn cell: its word claims every option of Z ∪ E is a clean loss, which is false for it. Were a stalemate checked, it would be rejected, since the check confirms no cell without a legal move; val(x) = D in both cases. A *bound*(0) cell is checked like any other. A cwin, closs or draw hint means some option is D, CW or BL, so x is not a clean loss, and an exit bound above p excludes L\_p. Among quiet children:

- A child not stored as a win disqualifies x. By the hypothesis it is either no win, giving x an option at D or better, or a win of value ≥ p, giving x a distance above p.
- A tagged or ≥ 100 child disqualifies x, giving BL or a distance above p.
- A child with value ≥ p disqualifies x.
- Each remaining child is exact W\_v with v < p and contributes v + 1.

Zeroing moves contribute 1, and the bound covers castling moves and exits. By the *bound*(b) guarantee, these terms are exactly the distances of x's options. The loser maximizes, so x is L\_p iff the maximum is p. ∎

**Lemma 5.9 (one exit partition).** Initialization and both loss checks split the legal moves of a cell into the same sets Z(x), E(x) and Q(x).

*Proof.* Both routines set zeroing moves aside first. Initialization classifies a remaining move as an exit exactly when the sub-table read reports a dropped castling right, that is, when the move starts on the king or a castling-rook square of a side with rights. Castling is such a move. The quiet-move index map tests the same condition first and returns the empty sentinel. Its only other sentinel, adjacent kings (Theorem 4.7(i)), arises from no legal move. The loss checks skip exactly castling and sentinel moves, so they examine exactly the moves initialization treats as quiet. ∎

**Lemma 5.10 (predecessors are genuine).** Suppose win propagation, mate-in-one marking or change marking writes or flags a cell y as a predecessor of x. Then y is a legal canonical cell with a legal quiet move into x that is not an exit. Consequently y is not a cell without quiet moves, and the option y receives through that move is s(val(x)).

*Proof.* The retrograde generator moves a non-pawn piece of the side that just moved in x from a to an empty b across empty squares (Section 4.3), giving y. Moving it back crosses the same empty squares, captures nothing and moves no pawn, so y → x is quiet and pseudo-legal. It leaves the mover's king as in x, where it is unattacked because x is legal with the other side to move, so y → x is legal. The index map returned a cell, not the sentinel, so it is no exit (Theorem 4.7(i)). The routines skip ILLEGAL entries, which are exactly the non-positions, the non-canonical cells and the positions whose side not to move is in check (Theorem 5.4), so y is legal and canonical. Initialization treats y → x as quiet (Lemma 5.9), so y is not in the case “No quiet move”, and the move offers s(val(x)). ∎

**Lemma 5.11 (clean propagation).** From an exact L\_q with q ≤ 99, clean win propagation with target q + 1 makes every predecessor whose true value is W\_{q+1} hold exactly that. Every other predecessor keeps or gains a sound upper bound. From L\_100 it writes a tagged CW\_101, but never over a pending *cwin* cell.

*Proof.* A predecessor has a quiet move into x, hence the option s(L\_q), so every write is a real option and a sound bound. If val(y) = W\_{q+1}, y is no loss and a stored untagged win bounds it from above (Theorem 5.12(S)), so y holds an intermediate, W\_{q+1}, or a larger untagged or tagged win. An intermediate is overwritten, since the *cwin* skip applies only to target 101. Larger wins, untagged or tagged, exist only as castling exit seeds, and the call overwrites them. Without castling, initial wins are W\_1 or tagged CW\_1 on cells with no quiet move, which are never predecessors (Lemma 5.10), and clean targets increase. For q = 100 the call writes CW\_101 only on intermediates without the *cwin* hint: a classified clean win dominates it, a tagged seed above 101 is repaired at cursed ply 100 (Theorem 5.15), and a *cwin* cell has CW\_1 (Lemma 5.18). ∎

**Theorem 5.12 (CLEAN phase).** At the end of P(c, p) in the CLEAN phase:

- (S) Every untagged final loss is exact. Every untagged final win bounds its true win from above. Every tagged entry is a sound cursed bound.
- (C) Every color-c loss of value ≤ p, and every opposite-color win of value ≤ p + 1, is final and exact. Exact wins are never overwritten.

*Proof.* Induction over the passes.

- A loss x ∈ L\_p with quiet moves has p ≥ 2. Its maximum comes from its bound, which makes it due, or from a child W\_{p−1}, which flags x at the mark pass of Lemma 5.5; x is reached by Lemma 5.6 and confirmed by Lemma 5.8.
- A win W\_t comes from a zeroing move, from an exit, or from a child L\_{t−1}. For t = 1 that child is checkmated, and mate-in-one marking at ply 0 writes W\_1, the least win, on every predecessor (Lemma 5.10); for t ≥ 2, Lemma 5.11 handles it.
- Soundness holds because every write carries a real option.
- Concurrency does not affect verdicts (Theorem 9.8). ∎

### 5.5 The CURSED phase and the WDL projection

**Lemma 5.13 (cursed check).** Under the cursed analog of the threshold hypothesis, the cursed loss check confirms x at ply p iff val′(x) = BL\_p. Here val′ is the perturbed model of Section 5.6.

*Proof.* A stalemate or a drawn cell without quiet moves is never checked, as in Lemma 5.8. A child not stored as a cursed win or a clean win disqualifies x as in Lemma 5.8. Clean wins below 100 are skipped, since any BL option dominates their L option. Every other counted term is a real BL distance, every BL option is counted, and the cursed-band test counts an untagged W\_100 child as 101. A *closs* cell whose only BL option is a zeroing move into CW has bound 0 and no quiet cursed child, so nothing wakes it. The re-check of *closs* cells at cursed ply 1 confirms it exactly when val′ = BL\_1. ∎

**Lemma 5.14 (unflagged hints).** Change marking never flags a cell with hint *draw* or *cwin*, and this loses no confirmation.

*Proof.* A *draw* or *cwin* cell has a D or CW option (Theorem 5.4), so it is neither a loss nor a blessed loss, and both loss checks reject it at once (Lemmas 5.8, 5.13). A flag could only trigger a failing check. The only writes to it, from win propagation, mate-in-one marking and the promotion of Lemma 5.18, ignore the flag. ∎

The restart logic has four cases. Let pc₀ be the pending-cursed flag set during initialization, and let f indicate whether the CLEAN loop settled. Below 101, every cursed distance descends from an initialization seed recorded in pc₀: a *cwin* hint, a *closs* hint or a tagged initial entry. At 101 and above it may also descend from a boundary seed: a clean L\_100 child gives CW\_101, a clean W\_100 child BL\_101.

| pc₀ | f | Behavior | Why it is correct |
| --- | --- | --- | --- |
| false | true | return | No seed exists, and a settled clean phase has no value at 100. |
| false | false | cursed loop from ply 101 | Only boundary seeds exist. Their CW\_101 were written at clean ply 100. A White W\_100 is marked at cursed ply 101; a Black W\_100 was marked at clean ply 100, and its flagged White predecessors are checked at cursed ply 101. |
| true | true | cursed loop from ply 1; return if it settles, otherwise continue past 100 | No boundary seed exists, so Theorem 5.16 applies to the whole loop. |
| true | false | cursed loop from ply 1 until it settles or reaches 100, then from ply max(p, 99) + 1, where p is the ply it ended at | The first loop settles every seed-derived value below 101 (Theorem 5.16). Plies it skips below 100 can hold no value. The continuation from ply 100 processes the clean L\_100 and W\_100 cells at cursed plies 100 and 101. |

**Theorem 5.15 (non-minimal seeds).** Two kinds of win are written before their final value is known: exit-seeded wins in castling materials, and the CW\_101 boundary seeds written at clean ply 100. The final value of each is still the minimum over its options.

*Proof.* In the CURSED phase, win propagation overwrites any tagged win above its target in every material. A stored tagged win of value e > t is not yet marked, since marking happens at ply e or e + 1, after ply t − 1. In the CLEAN phase only castling materials overwrite, and only they need to. Without exits, the initial wins are W\_1 or a tagged CW\_1 of a cell with no quiet move, which propagation never reaches. Clean targets never decrease, and the other tagged wins appear only at ply 100, where every target is 101. Before an overwrite, a checker at ply p ≤ t reads a value ≥ p and rejects the cell, as it would the true value. Clean exits compose exactly, and cursed exits as Definition 5.19 specifies. ∎

**Theorem 5.16 (termination and stopping rule).** Each loop is bounded: 101 plies in the CLEAN phase and at most 2,046 in the CURSED phase. A pass reports activity when it writes a value (a cwin promotion, or a confirmed loss together with its propagation), when it propagates a stored loss of value p, or when its mark sub-phase finds a win of value p or p − 1 in a scanned chunk: in the CLEAN phase an untagged win, in the CURSED phase a tagged win or one of value ≥ 100. Suppose that a ply p > M passes with neither color reporting activity. Then:

- In the CLEAN phase, no clean value of distance ≥ p is still unfound.
- In the first cursed loop, no seed-derived cursed value of distance ≥ p is still unfound.
- In the continuation from ply 100, no cursed value of distance ≥ p is still unfound.

*Proof.* A value of distance q > M does not come from initialization, so its witness is a quiet child of distance q − 1. Hence every distance from M + 1 to q is realized, p included, and finding a value of distance p is activity. A loss of distance p is confirmed or propagated at ply p. A win of distance p lies in a flagged chunk (Lemma 5.6) and is found at ply p by the mark sub-phase of its color, which reports every win of value p or p − 1 in a scanned chunk; it marks value p for Black and p − 1 for White. In the first cursed loop a chain may start at a boundary seed of distance 100 or 101, which is why that claim is restricted to seed-derived values. The continuation starts at ply 100 and covers boundary seeds. ∎

**Theorem 5.17 (WDL is exact).** For every cell, the class of the decoded entry equals cls(x). Consequently the projected WDL table is exact, boundary codes included.

*Proof.* Untagged entries of value ≤ 100 are written only in the clean band. At the end of the solve each is exact: an exit seed is replaced whenever a shorter line exists (Theorem 5.15), and every other such entry is exact when written (Lemma 5.11, Theorem 5.12). Every write above 100 or in the cursed phase is tagged. So decoding yields the classes of val′, which are those of val (Definition 5.19), and intermediates decode to D. Every decisive cell is classified by the end of the solve (Theorems 5.12, 5.20), so the remaining intermediates are exactly the drawn cells. Boundary codes go exactly on untagged value-100 entries, the cells with val = W\_100 or L\_100, which by Lemma 12.11 are the only ones a parent cannot classify from classes alone. ∎

### 5.6 Cursed distances

**Lemma 5.18 (pending captures).** Every *cwin* cell without a clean win ends as the tagged value CW\_1, which is its true value.

*Proof.* A *cwin* cell has a zeroing move into a blessed loss, which offers CW\_1, and it has no clean win, or the CLEAN phase would have overwritten it. So val = CW\_1, the best cursed value. Its pending flag starts the CURSED phase at ply 1, and its chunk, holding a pending intermediate, is never pruned (Lemma 5.6), so the mark sub-phase of its color at ply 1 promotes it to tagged W\_1. Before that no write reaches it:

- **CLEAN phase.** Win propagation skips *cwin* cells for cursed targets, and a clean target would reflect a clean win, which the cell lacks.
- **CURSED phase.** Win propagation skips *cwin* cells, and every target is loss + 1 ≥ 2, no improvement on CW\_1.

After promotion the cell holds value 1, and no target lowers it. ∎

**Definition 5.19 (cursed exit composition).** Across a castling exit, a cursed twin value u composes as f(u) = (u + 1) \| 1. The twin is read from its shipped DTZ file, whose one-byte tier returns u − 1 for an even cursed u (Theorem 11.9). Since f(u − 1) = f(u) for every even u, the composed value does not depend on the tier the twin was stored in. It is also odd, so it is itself stored exactly, and no halving error can compound along a line of exits. Let val′ be the solution of B under this composition. Clean exits compose as u + 1, and the change alters no class, so val′ has the class of val everywhere and the same clean band. In a material without castling rights there are no exits, and val′ = val.

**Theorem 5.20 (final statement for the solver).** After the solve:

- every clean cell holds its exact value;
- every cursed or blessed cell holds a tagged entry equal to its value under Definition 5.19;
- every other cell decodes to D.

The 50-move class is exact in every case.

*Proof.* Theorem 5.12 covers the clean band. The cursed band follows by the same induction over the CURSED passes. It uses Lemma 5.13 in place of Lemma 5.8, with the induction hypothesis supplying its threshold hypothesis, together with the stagger of Lemma 5.5, the frontier of Lemma 5.6, cursed propagation, and Lemma 5.18 for cwin cells. By Theorem 5.16 and the restart analysis before Theorem 5.15, the plies that the loops skip or stop before hold no unfound value, and cells without quiet moves hold their final values from initialization (Theorem 5.4). The only wins written above their final value are exit-seeded wins and CW\_101 boundary seeds, which Theorem 5.15 replaces whenever a shorter line exists. ∎

**Lemma 5.21 (fusions).** The solver runs Sections 5.3–5.5 on a whole fusion at once, with M, pc₀ and f aggregated over its pairs. This yields each pair's own values:

- Pairs of a fusion do not interact (§4.3).
- A pass over a pair that has already settled writes nothing (Theorem 5.16).
- For a pair whose own pc₀ or f differs from the fusion's, the restart path only adds plies at which that pair writes nothing. ∎

## 6. DTM solver correctness

The DTM generator computes exact unbounded mate distances. It is a breadth-first retrograde analysis. Its soundness holds at every write and its completeness holds ply by ply, and the parity invariant it maintains makes halved storage lossless.

**Definition 6.1.** Let L\_0 be the set of checkmated positions. For n ≥ 1:

- W\_n is the set of positions not in W\_{\<n} that have a move into L\_{n−1};
- L\_n is the set of positions not in L\_{\<n} that have a legal move, all of whose moves lead into W\_{≤ n−1}, and at least one into W\_{n−1}.

DTM(x) is win(n) for x ∈ W\_n, loss(n) for x ∈ L\_n, and DRAW otherwise. This is the least fixed point of retrograde analysis, and it equals the value of Γ∞ with a minimizing winner and a maximizing loser.

### 6.1 Initialization

**Lemma 6.2.** For each legal cell, initialization returns one of the following:

- (a) loss(0) iff the position is checkmate;
- (b) win(v), where v is 1 + the minimum l over zeroing and exit children that are loss(l), so that v is an upper bound on the true win distance;
- (c) an exact loss(d), where d is 1 + the maximum child win, when x has a legal move, every move is a capture, a promotion or an exit, and every child is an opponent win (a pawn push leaves the cell an intermediate under (d));
- (d) an intermediate cell whose bound B is 1 + the maximum child win over zeroing and exit children when at least one such child exists and all of them are opponent wins, and 0 otherwise, in particular when x has no zeroing or exit move. A stalemate is such an intermediate with B = 0: it reads as D, and the loss check, which requires a legal move, never makes it final.

Every non-illegal cell is flagged on the frontier.

*Proof.* Children come from sub-tables, exit tables and push targets, the last final by Corollary 4.8. For a double push, the opponent's value with the en-passant right is the best of the stored child and the en-passant captures, or of the captures alone if they are its only moves. This is exactly minimax over the true move set, since the legal-move generator excludes en passant. ∎

**Lemma 6.3 (no illegal child).** Every child that initialization reads through a capture, promotion, exit, en-passant capture or pawn push is a win, a loss or a draw, never ILLEGAL. Consequently, an intermediate that has a zeroing or exit move, all of whose zeroing and exit children are opponent wins, has B ≥ 2.

*Proof.* A legal move's child is a legal position with the opponent to move. The read reaches its canonical cell in the right table and color: by Lemma 4.9 for captures, promotions, exits and en-passant captures, and for pushes by Lemma 4.5 and Theorem 4.7(ii), in a surviving slice of an earlier batch. Generators write ILLEGAL only to non-positions, non-canonical cells of stabilized slices, and positions whose side not to move is in check (Theorem 3.12(c), Theorem 5.4), so by induction over the build order (Theorem 4.2) and the batches the child is a win, loss or draw. For the consequence, B = 1 + max w with each w ≥ 1. ∎

**Lemma 6.4 (en-passant replies).** For a double push from a legal cell, the opponent's en-passant replies are generated on the board after the push and made on the full decoding of the push target's canonical cell. The two boards place the same man on every square and have the same castling rights, so each reply reads the child of the true reply.

*Proof.* The parent board is its cell's full decoding. A push moves no king, so the king pair, slice and transform are unchanged, and the transform is the identity. Pawnful materials have no stabilized slice (Theorem 4.7), and by Lemma 4.5 the pushed placement is a surviving cell whose designated pair is still canonical. So the pushed board indexes to the cell whose full decoding places the same man on every square and has the same castling rights, which derive from the same king slice. Each reply therefore has the same from, to and victim squares on both boards. The sub-table read (Lemma 4.9) uses that decoded cell. The index omits the side to move, so reading the cell with the opponent to move is exact. ∎

**Lemma 6.5 (exit tests agree).** Let m be a legal move from a legal cell. The loss check assigns m the empty sentinel exactly when initialization reads m's child from a sub-table, an exit table or a push target, that is, exactly when m is a capture, a promotion, a pawn move or a castling-rights exit, castling included.

*Proof.* The loss check gives the sentinel to captures, pawn moves and castling, and otherwise exactly when the quiet-move index map does, which for a legal move (kings never adjacent) is exactly for castling-rights exits (Theorem 4.7(i)). The map's exit test, that some side c has rights and m starts on c's king square or a castling rook square of c, is the predicate under which initialization's sub-table read sees m drop a right. Castling satisfies it. No pawn move does, since the king squares hold kings and full decoding grants a right only with the rook on its rook square. Without rights, no move satisfies it. So both sides pick out exactly captures, promotions, pawn moves and right-dropping moves, and on every other move the loss check reads a quiet in-pair child while initialization ignores m. ∎

**Lemma 6.6 (the bound is fixed).** Whenever a pass reads an intermediate cell, its value field is the bound B that initialization wrote.

*Proof.* After initialization an intermediate is changed only in its change flag, by the flag OR or a failed re-check, until it is replaced by a final entry, which never again reads as an intermediate (Lemma 5.3). ∎

### 6.2 Soundness and completeness

**Lemma 6.7 (soundness).** At all times:

1. every final loss(d) is exact;
2. every final win(v) belongs to W\_d for some d ≤ v;
3. no cell with DTM = DRAW is ever final.

*Proof.* Check each write site.

- Initialization writes loss(0) only at checkmates, win(v) with v an upper bound on the true win distance (Lemma 6.2(b)), and loss(d) only when every move is a capture, promotion or exit into an opponent win (Lemma 6.2(c)). Its child values, the en-passant valuation of double pushes included (Lemma 6.4), come from sub-tables, exit tables and push targets, exact by induction over the build order (Theorem 4.2) and the batches, so loss(d) is exact and win(v) bounds from above a distance d ≤ v with x ∈ W\_d.

- Mate-in-one marking writes win(1), the smallest win distance, to the quiet predecessors of a mate.
- Win propagation with target l + 1 runs from a final loss(l), so each predecessor has a move into L\_l and lies in W\_{≤ l+1}. It skips losses and wins of value at most the target, and overwrites a larger win with the target, which is a real option.
- The loss check writes loss(p) only if all three conditions hold:
  - every in-material child is a final win below p;
  - the empty-sentinel moves are exactly the zeroing and exit moves, whose children initialization folded into B (Theorem 4.7, Lemma 6.5); if at least one exists, they are all covered by 1 ≤ B ≤ p, with B as initialization wrote it (Lemma 6.6);
  - the maximum contribution equals p.

  By Lemma 6.10, proved jointly with this lemma by induction in schedule order, those child wins are exact by then. So every move leads to an opponent win, and the maximum is exactly p − 1. ∎

The schedule is ply 0 for White then Black, and then for p = 1, 2, …, the pass P(W, p) followed by the pass P(B, p). Each pass has two phases:

- **Mark.** The pass sets the change flag on the opponent predecessors of this color's wins of value p − (c XOR 1), that is, of value p − 1 for White and p for Black.
- **Cell.** The pass re-verifies every flagged cell and every cell with bound B = p, and propagates the losses of value p.

**Invariant J(q, c).** After the pass P(c, q):

1. every color-c cell with a true loss ≤ q is final and exact;
2. every color-(c XOR 1) cell with a true win ≤ q + 1 is final and exact.

**Lemma 6.8 (frontier).** Chunk and group pruning never skips a cell that needs work.

*Proof.* In the DTM solver an intermediate is *pending* at ply q if its change flag is set or its bound B satisfies B ≥ q. A due cell has the change flag or B = q, the cells the cell sub-phase re-verifies, so it is pending; and as B is fixed (Lemma 6.6), a cell with B < q is due again only through a change flag, whose marking re-flags its chunk. A chunk is cleared at ply q only if it holds no pending intermediate and its largest classified value is below q − 1; any later need (a new win or a change flag) re-flags it. A loss written during a scan lies in the scanned chunk, which that pass cannot clear because it held the re-verified intermediate, and its wins are propagated at once. Mate-in-one marking at ply 0 leaves a classified value ≥ 1, so its chunk cannot be cleared before ply 3, and the win is marked by ply 2. ∎

**Lemma 6.9 (group pruning).** When a pass clears the group bit of g, every chunk of g satisfies the chunk-clearing condition of Lemma 6.8 at that ply. Hence group pruning never skips a cell that needs work.

*Proof.* Clearing g at ply p requires every scanned chunk to meet the condition, hence p ≥ 2. An unscanned chunk of g has a clear bit, either (i) cleared at an earlier ply p′ < p of this fusion, or (ii) clear since initialization. In (i), every write since then would have re-flagged it: a loss written during a scan lies in a scanned chunk, mate-in-one marking precedes any clearing, and every other write sets both bits. So m + 1 < p′ < p still holds. In (ii), initialization flags every non-ILLEGAL in-pair cell and writes touch no other, so the chunk holds no pending intermediate or classified value. A resumed fusion starts with all bits set, so only (i) arises there. A straddling chunk is never cleared (Lemma 5.7), so if its bit is clear it is in case (ii). So every chunk of g meets the condition and, by Lemma 6.8, needs no work until a write re-flags it, which also sets the group bit. A pass that writes a loss does not clear g, since that loss's chunk held the re-verified intermediate. ∎

**Lemma 6.10 (completeness).** J(q, c) holds for all q and c.

*Proof.* Induction in schedule order. Let x of color c lie in L\_p.

- If B = p (so p ≥ 2 by Lemma 6.3), x is due at ply p, and every in-material child is an opponent win of value ≤ p − 1, final and exact by J(p − 2, c).
- Otherwise B < p, so the maximum p − 1 is attained by an in-material child y ∈ W\_{p−1}. The stagger places the mark pass that flags y's predecessors at value p − 1 before color c's cell pass at ply p. No re-check clears the flag in between, so x, whose chunk and group are scanned (Lemmas 6.8, 6.9), is verified at ply p.

Wins at p + 1 are written by win propagation from their loss-p child at ply p, when the cell sub-phase confirms that loss or finds it stored from initialization, or were already exact from initialization. A predecessor in W\_{p+1} lies in no W\_{≤ p}, so by soundness (Lemma 6.7) the target p + 1 it receives is exact. ∎

**Lemma 6.11 (termination).** Let M be the largest of: the value of every final entry written by initialization; for every intermediate written by initialization, one more than its largest opponent win among zeroing and exit children, which is at least its bound; and l + 1 for every loss of value l that a pass at a positive ply confirms, or finds stored and propagates, whether or not the propagation writes. Losses at ply 0 do not count, so the win(1) targets of mate-in-one marking at ply 0 are excluded. A resume restores M from the checkpoint. A pass P(c, p) *reports activity* when its mark sub-phase finds a win of value p or p − 1, or its cell sub-phase confirms a loss or propagates a stored loss of value p. The loop stops at the first ply q ≥ 1 with q > M at which neither P(W, q) nor P(B, q) reports activity, and in any case after ply 2046; under RANGE (Section 13.1), every true value is final by then. Then no true value of distance ≥ q exists, so every cell is final and exact by Lemma 6.10.

*Proof.* Let v > M. A true loss of distance v was initialized as an intermediate whose zeroing and exit children contribute at most M, so it has a quiet child in W\_{v−1}. A true win of distance v has a quiet child in L\_{v−1}, or initialization would have written win(v) (Lemma 6.2(b)) and v ≤ M. So descending through quiet children from a true value of distance ≥ q reaches a true value y of distance exactly q. If y ∈ L\_q, the loss check writes it at ply q (Lemma 6.10), and that pass reports activity. If y ∈ W\_q with q ≥ 2, its quiet child in L\_{q−1} is confirmed or propagated at ply q − 1 ≥ 1, so q ≤ M. If q = 1, y was written at ply 0, and its chunk and group cannot be cleared before ply 3 (Lemmas 6.8, 6.9), so the mark sub-phase at ply 1 finds it and reports activity. Each case contradicts the stopping condition. ∎

**Theorem 6.12.** By induction over the build order (Theorem 4.2), every legal cell holds exactly DTM when the solve finishes. ∎

### 6.3 Parity

**Theorem 6.13 (parity).** Every DTM or DTM50 win distance is odd and every loss distance is even.

*Proof.* L\_0 = 0, W\_n = 1 + (a loss n − 1) and L\_n = 1 + (a win n − 1), so induction on n gives the claim. In the solvers, every write site (initial wins and losses, the loss check, win propagation, the en-passant conversions and the DTM50 builder) adds exactly 1 to a child of opposite parity, the mate boundary writes loss(0), and sub-table and exit values satisfy parity by induction over the build order. ∎

By Theorem 11.10, storing v >> 1 is therefore lossless.

## 7. DTM50 correctness and the embedded-DTZ theorem

Each stored DTM50 layer h equals val(x, h) of the clocked game Γ. The layers are monotone in h. For clean decisive positions with 2 ≤ dtz ≤ 100, the first drawn clock is exactly 101 − dtz (packed layer 102 − dtz), whatever line the winner chooses, and the reader's formula dtz = 102 − h inverts this.

### 7.1 The recurrence equals the definition

Write C for the minimax combine: a win at 1 + the minimum child loss if any child is a loss; otherwise DRAW if any child is a draw; otherwise a loss at 1 + the maximum child win. C is the maximum under the mover's preference order, so it is associative and splits over any partition of the moves.

**Lemma 7.1.** By Lemma 2.2, val(·, h) is the unique solution of:

V\_h(x) = C({V\_0(x·m) : m zeroing} ∪ {V\_{h+1}(x·m) : m not zeroing}),

with checkmate as loss(0), stalemate as DRAW, and V\_100(y) = loss(0) if y is checkmated and DRAW otherwise. Exits read the exit table at h + 1, captures and promotions the sub-table at clock 0; an exit reaching clock 100 is decided by the mate test first, as Γ makes (y, 100) terminal. ∎

**Theorem 7.2 (implementation equals the recurrence).** After the DTM50 pass, layer h holds V\_h for every legal cell and every h ∈ \[0, 99\].

*Proof.* **Moves are split correctly.** The zeroing test (a pawn move or a capture) is exactly Z(x), promotions and en passant included. Each child is read as Lemma 7.1 prescribes: never as ILLEGAL (Lemma 7.5), and after a double push together with the en-passant replies (Lemma 7.6). Exactly the legal cells are built at every layer (Lemmas 7.3, 7.4).

1. **The cache join is exact.** Initialization at h = 99 evaluates every move and stores in layer 0 the fold INV over the zeroing moves, non-empty when one exists (Lemma 7.5). For any builder A, converting A + enc(INV) equals converting A + INV in each of three cases: INV contains a win, a draw but no win, or only losses with worst loss ≥ 2. These cases cover every non-empty INV: when no zeroing child is a loss or a draw, every zeroing child is an opponent win w ≥ 1, and each converted loss 1 + w is at least 2. When no zeroing move exists, the layer builder recomputes that fact and ignores the stored word. So the builder at h ≤ 98, adding INV to the non-zeroing moves at h + 1, computes V\_h. No layer 0 of the fusion is read as V\_0 before h = 0, since pushes target only earlier batches (Theorem 4.6), so layer 0 can hold the cache.
2. **Every input is final when read.** Layers run from 99 down to 0 per fusion, fusions in ascending pawn life, and layer h reads only the opponent's complete layer h + 1, exits at h + 1 and sub-tables at 0 (finished earlier), the final layer 0 of earlier slices, and its own cache.
3. **The h = 0 overwrite is safe.** A cell's cache is read immediately before its layer 0 is written, and the h = 0 pass reads no other cell's layer 0. ∎

Call an in-pair cell x of the active fusion *legal for c* if it is a legal canonical placement and, with c to move, the side not to move is not in check. A converted builder is a win at 1 + the minimum child loss, DRAW, or a loss at 1 + the maximum child win; its value field is at most 1 + 2045 = 0x7FE (RANGE, Section 13.1), never the ILLEGAL code 0x7FF. An entry is ILLEGAL exactly when its value field is 0x7FF.

**Lemma 7.3 (the legality test).** Whenever the pass for (c, h) reads the layer-0 entry of x for c, that entry is ILLEGAL iff x is not legal for c. At h = 0 the entry is read before the pass overwrites it.

*Proof.* Initialization writes ILLEGAL for both colors at overlap and phantom cells (Theorem 3.12(c)) and for c at decoded cells not legal for c. A cell legal for c with no zeroing move keeps its creation value 0, which is DRAW (Lemma 10.8), as do cells without legal moves. A cell legal for c with a zeroing move gets enc(INV), which is not ILLEGAL: it encodes the fold of a non-empty set of child values (Lemma 7.5) as a win, a draw or a loss of distance at most 1 + 2045 = 0x7FE (RANGE, Section 13.1), as a converted builder does. Later only the pass (c, 0) writes this entry, as each write targets the writer's own in-pair index and the fusions partition the batch (Theorem 4.6). A resume restores layer 0 exactly (Theorem 10.17). That pass reads the entry of x just before writing it, and the layer builder, evaluating only non-zeroing moves, reads no layer 0 of the fusion. ∎

**Lemma 7.4 (the DTM50 frontier).** For each color c there is a frontier byte per group and per aligned chunk. During every pass (c, h) of a fusion, every group and every chunk that holds a cell legal for c has its byte set to 1. Hence the pass (c, h) writes every cell legal for c, for every h ∈ \[0, 98\], and initialization writes it at h = 99.

*Proof.* Bytes start at 0. Initialization visits every index of each group meeting an active slice (the ranges tile the group, Theorem 9.1) and sets the group and chunk bytes of every in-pair x whose entry for c is not ILLEGAL, that is, every x legal for c (proof of Lemma 7.3); all layers share one grouping. A run resumed from a clock below 99 sets every byte to 1 and does not rerun initialization. A run resumed at clock 99 starts from zeroed bytes and reruns initialization.

A pass clears a chunk byte only after a range covering the whole aligned chunk (Theorem 9.1) in which no in-pair cell passed the legality test, so the chunk holds no cell legal for c (Lemma 7.3). It clears a group byte only when no range found a legal cell: skipped ranges lie in chunks with byte 0, legal-free by the claim, scanned ones are legal-free by Lemma 7.3, and the ranges tile the group. Legality does not depend on h, so the claim persists, and each pass scans and writes every legal cell after the legality test. ∎

**Lemma 7.5 (child reads).** Let x be legal for its side to move, at clock h, and let m be a legal move. The DTM50 child read of m, or the double-push valuation when m is a double push, returns a win, a loss or DRAW, never ILLEGAL. So the builder receives a child for every legal move, and INV is non-empty whenever a zeroing move exists.

*Proof.* Let y = x·m, a legal position with the opponent to move. Converted entries are never ILLEGAL (the remark before Lemma 7.3).

**Capture or promotion.** The read takes the right cell of the right table at clock 0 (Lemma 4.9). All child materials but K v K are open, and reading K v K as DRAW is exact, as a lone king never gives check. Otherwise the exact, non-ILLEGAL WDL class (Theorem 5.17) gives DRAW for DRAW, CURSED\_WIN, BLESSED\_LOSS and singular frames, and a WIN or LOSE cell, never in a skipped block (Theorem 11.24), decodes exactly (Theorem 11.10), as in the flat layer-1 reader.

**Single push.** I(y) (Theorem 4.7(ii)) is legal for the opponent and lies in a strictly earlier batch (Theorem 4.6), whose pass at h = 0 wrote a converted builder, loss(0) or DRAW there (Lemma 7.4).

**Double push.** By Lemma 7.6 the valuation returns single-push values, negated capture reads, or the better of such, none ILLEGAL by the cases above.

**Non-zeroing move (quiet or exit) to clock 100.** The mate test returns loss(0) or DRAW.

**Exit at clock h + 1 ≤ 99.** Every exit material, with any rights dropped, is open, so this is a table read as for a capture.

**Other quiet move at clock h + 1 ≤ 99.** The quiet-move index map returns I(y) (Theorem 4.7(i)–(iii)), a cell of s or mirror(s), hence, as the fusion is a union of mirror pairs, an in-pair cell legal for the opponent, written at layer h + 1 by initialization if h + 1 = 99 and by the opponent's pass otherwise (Lemma 7.4), with a converted builder, loss(0) or DRAW. ∎

**Lemma 7.6 (DTM50 double-push valuation).** Let m be a double push from a legal cell x at any clock, let y = x·m, and let yᵉ be y with the opponent's en-passant right. The double-push valuation returns val(yᵉ, 0).

*Proof.* The clock is 0 after a pawn move. Let N and E be the legal non-en-passant moves and en-passant captures of yᵉ; the children of N ignore the right. The stored V\_0(y) is final (Theorem 4.6, and Theorem 7.2 for the earlier batch) and is C over N if N ≠ ∅. Captures are generated on the decoded board, which is y since the kings did not move (as in the proof of Theorem 5.4); each child is read in the sub-table at clock 0 at the right cell (Lemma 4.9, en-passant clause), and its one-ply negation (loss(v) ↦ win(v + 1), win(v) ↦ loss(v + 1), DRAW ↦ DRAW) is C over that child. The valuation returns V\_0(y) if E = ∅ (so yᵉ = y), the best capture if N = ∅ ≠ E (the stored terminal value of y does not apply to yᵉ), and the better of both otherwise. As C is the preference maximum and splits over partitions (Section 7.1), each case is C over N ∪ E at clock 0, which is val(yᵉ, 0) by Lemma 7.1 with en-passant rights (Section 2.1). ∎

### 7.2 Monotonicity

Order values by distance within a class, shorter below longer, with any decisive value below DRAW, and wins incomparable with losses. Write ⊑ for this order; it is the reverse of the order ⪯ of Section 8.1.

**Theorem 7.7 (monotonicity).** DTM(x) ⊑ V\_0(x) ⊑ V\_1(x) ⊑ … ⊑ V\_99(x). In particular, every decisive layer has the class of DTM(x).

*Proof.* V\_h ⊑ V\_{h+1}, by induction on μ and then on h downward from V\_99 ⊑ V\_100 (a V\_100 loss(0) is a mate, so V\_99 = loss(0)): a winning move's child loss(b − 1) at h + 1 is the same zeroing child at h or, by induction, a loss no longer at h + 1 than at h + 2; a losing cell's children at h are opponent wins no longer than at h + 1. DTM ⊑ V\_0, as a clocked winning strategy of length b also wins in Γ∞. ∎

*Consequence.* The packed column DTM, V\_0, …, V\_99 satisfies hypothesis (M) of Theorem 11.15, and a single WDL class decodes every rank in it. Encoding stops at the first DRAW, so the column is stored losslessly.

**Lemma 7.8 (the DTM50 head).** Let x be a legal cell whose class w is not DRAW, and let the DTM file store s at x. Then the head row of x's DTM50 column holds s, and a DTM read from the flat DTM50 column returns the same value as a read from the DTM file, cursed and blessed classes included.

*Proof.* The head re-encodes the DTM entry at x (Lemma 11.32): under w, s decodes to win(2s + 1) for WIN and CURSED\_WIN and to loss(2s) for LOSE and BLESSED\_LOSS, also in a singular frame, whose single value is s, and the storage value is (2s + 1) >> 1 = s or 2s >> 1 = s (Theorem 11.10). The rank table is gathered from these values, so s has a rank (Theorem 11.6), and a flat read at layer 0 returns s (Theorem 11.15; the head's rank is not D) and decodes it under w as the DTM reader does. Legal cells in skipped DTM blocks are DRAW (Lemma 11.17), and a DRAW cell's head 0x7FF has no rank, so its column is a don't-care constant record never decoded (Lemmas 11.16, 12.16). ∎

### 7.3 The embedded-DTZ theorem

**Theorem 7.9.** For every position x, the first drawn clock is f(x) = 101 − z(x) whenever 1 ≤ z(x) ≤ 100, where z is the zeroing attractor of Section 2.3. Therefore:

| dtz | First DRAW clock | First DRAW packed layer | Column shape |
| --- | --- | --- | --- |
| 0 (mate) or 1 | none in 0..99 | none | never flips |
| 2 ≤ dtz ≤ 100 | 101 − dtz | 102 − dtz | ends in DRAW at that layer |
| cursed or blessed (z > 100) | 0 | 1 | DTM, then DRAW |

*Proof.* The layers are val(·, h) (Theorem 7.2), decisive iff x ∈ A\_{100−h} ∪ B\_{100−h} (Lemma 2.4). These sets grow with d, so this holds iff h ≤ 100 − z(x): the first drawn clock is 101 − z(x), in 0..99 iff z ≥ 2, and packed layer = clock + 1. Checkmates are loss(0) at every clock. Cursed and blessed cells have val(x, 0) = D, so flip at packed layer 1. ∎

The flip depends only on *which* clocks are decisive, not on distances, so the theorem holds whichever winning line is optimal, and the flip agrees with the standalone DTZ file, which measures the same z (Section 5).

**Corollary 7.10 (the reader).** The draw-flip reader returns z(x) for every clean WIN or LOSE.

*Proof.* The decoder reports the last change point, the first DRAW layer, since encoding stops there. Flip h = 0 ("never flips") means z ≤ 1, and the reader returns 0 exactly for a checkmated loser, 1 otherwise. Flip h = 1 cannot occur for a clean class, and the reader returns "unknown". For 2 ≤ h ≤ 100 it returns 102 − h = z (Theorem 7.9). ∎

### 7.4 Probe shortcuts

**Theorem 7.11.** For a clean WIN or LOSE at clock h ∈ \[0, 99\]:

- (a) if h + DTM(x) ≤ 100, then DTM50\_h(x) = DTM(x);
- (b) if h + dtz(x) > 100, then DTM50\_h(x) = DRAW.

*Proof.* (a) We prove, by induction on d, the stronger claim that val(x, h) = DTM(x) for every position x with finite DTM(x) = d and every clock h ∈ \[0, 100\] with h + d ≤ 100; no class condition is assumed. For d = 0, x is checkmated and val(x, h) = loss(0) at every clock, clock 100 included (Section 2.2). For d ≥ 1, h ≤ 99. If x wins, a DTM-optimal move leads to a child of DTM loss(d − 1) at clock h + 1, or 0 after a zeroing move, and (h + 1) + (d − 1) ≤ 100, so by induction the child is loss(d − 1) and val(x, h) is a win of distance at most d; monotonicity (Theorem 7.7) turns "at most d" into equality. If x loses, every child has DTM win(e) with e ≤ d − 1, one with e = d − 1, and is reached at clock h + 1 or 0 with (h + 1) + e ≤ 100, so every child is win(e) and val(x, h) = loss(d). (b) h > 100 − z lies past the flip (Theorem 7.9). Without a DTZ the DTZ field is 0, and the test of (b) does not fire. ∎

**Theorem 7.12 (remaining probe shortcuts).** For a table cell x of class w, probed at clock r, each value the full probe returns without reading that metric's own table is exact:

- (a) If w = DRAW, DTM and DTZ are reported as draws, and so are DTC and DTM50 at a finite clock. A DRAW cell is drawn in Γ∞ and at every clock (Definition 2.3, Corollary 2.5), and every DTC layer is D (Corollary 8.10).
- (b) If a DTM50 file exists and r ≥ 100, DTM50 is a loss at distance 0 when w = LOSE and DTM = 0, and a draw otherwise; a LOSE cell without DTM gets no answer. Clock 100 is terminal, lost at distance 0 if checkmate and drawn otherwise (Section 2.2), and a LOSE cell has DTM 0 exactly when checkmated (Definition 6.1). With en-passant rights, whether or not a DTM50 file exists, DTC and DTM50 are reported drawn at r ≥ 100: the side to move has a capture and so is not mated, and P(r) = 0 admits no DTC price.
- (c) If w is cursed or blessed, DTM50 is a draw at every clock: val(x, 0) = D, and a position not decisive at clock 0 is decisive at no clock (Corollary 2.5.2).
- (d) DTM is read from the flat DTM50 column, whose first entry is DTM (Lemma 7.8), and DTZ from its draw flip (Corollary 7.10).
- (e) For a pawnful cell, DTZ is read from the DTC cell, whose row 0 holds the DTZ as decoded from the DTZ file (Theorem 8.15), and the DTZ file is then skipped. ∎

**Lemma 7.13 (row 0 is the prober's DTZ).** At every cell of decisive class, row 0 of the DTC cell equals the value that the prober's DTZ read returns from the DTZ file the DTC file was packed from.

*Proof.* Both readers decode with the exact stored class w (the packer's reader opens only full-format files, Theorem 5.17 and Lemma 11.1; the prober's class is exact, Theorems 12.12, 12.31 and 12.36), index by the plan built from the header code (Theorem 11.29) or a singular frame's one-byte tier (Corollary 11.8), and apply Theorem 11.9: 2s − 1 when eb = 1 and w is CURSED\_WIN or BLESSED\_LOSS, s otherwise. Row 0 stores this value without its class, and no decisive cell lies in a skipped DTZ block (Theorem 11.24, Lemma 11.2). ∎

**Lemma 7.14 (DTZ sources in the full probe).** For a clean class, every source from which the full probe takes DTZ returns z(x). For a cursed or blessed class, each returns a value within the tolerance of Theorem 12.15. The test of Theorem 7.11(b) is evaluated only on an exact clean DTZ or on no DTZ.

*Proof.* The full probe takes DTZ from the following sources, in this order.

1. *The flat DTM50 result.* A read gives DTZ only through the draw flip, exact for clean WIN and LOSE and absent otherwise (Corollary 7.10). A derivation is exact (Theorem 12.18). The probe applies the test of Theorem 7.11(b) at this point, before any DTC or DTZ read and only for WIN and LOSE. The test therefore sees an exact value, or no value, in which case it does not fire. At a finite clock a pawnless cell's DTC is then priced from this DTZ (Theorem 8.18), or, if none was supplied, after source 3.
2. *The DTC cell, for a pawnful cell.* It is read whether or not source 1 supplied DTZ, and row 0, when read, replaces that value. Row 0 is the decoded DTZ (Theorem 8.15, Lemma 7.13). It is exact for clean classes and within the tolerance of Theorem 12.15 otherwise (Theorem 11.9). It is decisive at a decisive cell, since a DRAW row 0 occurs only in singular frames and skipped blocks (Corollary 11.8, Theorem 11.24). Win and loss derivation return the clean DTZ (Lemma 12.19), and cursed derivation is within tolerance (Theorem 12.20).
3. *The DTZ file.* It is read only if neither source above supplied DTZ, and a pawnless cell's DTC not priced after source 1 is priced from this clean DTZ (Theorem 8.18). ∎

## 8. DTC correctness

The stored DTC layers equal a precisely defined push-budget game. That game is monotone in the budget, and it collapses to clean DTZ once the budget reaches the slice's push capacity. Together these facts prove that 30 budgets suffice, that the saturation shortcut is sound, and that the reader returns the exact minimal budget.

### 8.1 The push-budget game

Moves from a cell x fall into four kinds:

- **C (conversion):** a capture or promotion.
- **P (push):** a non-converting pawn move.
- **E (exit):** a castling-rights exit.
- **Q (quiet):** every other move.

Values are W\_n (1 ≤ n ≤ 100), L\_n (0 ≤ n ≤ 100) or D. The order ⪯ places D below everything and ranks W\_n ⪯ W\_m and L\_n ⪯ L\_m when m ≤ n; wins and losses are incomparable.

**Definition 8.1 (layer k).** Fix k ∈ {0, …, 29} ∪ {∞}, with ∞ − 1 = ∞. A move m to child c *wins with contribution t* or *loses with contribution t* as follows:

| Kind | Wins with t | Loses with t |
| --- | --- | --- |
| C | t = 1 iff WDL(c) = LOSE | t = 1 iff WDL(c) = WIN |
| P | t = 1 iff k ≥ 1 and V\_{k−1}(c) is a loss | t = 1 iff V\_k(c) is a win |
| Q | t = n + 1 iff V\_k(c) = L\_n and n + 1 ≤ 100 | t = n + 1 iff V\_k(c) = W\_n and n + 1 ≤ 100 |
| E | as Q, reading the exit table at budget k | as Q, reading the exit table at budget k |

Cursed and blessed sub-table classes fold to D. A double push is valued by row P on the opponent's combined value: the no-en-passant child, taken at budget k − 1 when the push would win and at budget k when it would lose, together with each legal en-passant capture at its conversion value. When the captures are the opponent's only legal moves, the combined value is theirs alone.

The layer value V\_k(x) is:

- W\_t, where t is the minimum contribution over winning moves, if some move wins;
- otherwise L\_t, where t is the maximum contribution, if x has a legal move and every move loses;
- L\_0 if x is checkmated;
- D in every other case.

The asymmetry in row P is the budget: a winning push spends one unit. A losing push and every reply, forced or not, cost nothing.

*Uniqueness.* Within a slice, induct on the value: a W\_n needs a child at L\_{n−1}, and an L\_n needs every quiet child at W\_{≤ n−1}. Across slices, induct on (life, k): P children have strictly smaller life (Lemma 4.4). Definition 8.1 therefore has a unique solution.

**Definition 8.2 (the DTC answer).** Let P(r) = 100 − r for a clock r < 100, and P(r) = 0 for r ≥ 100. Let V\_30 = V\_∞, which is clean DTZ on clean cells and D on cursed, blessed and drawn cells (Lemma 8.9). Then

b\*(x, r) = min{b ∈ \[0, 30\] : V\_b(x) decisive and \|V\_b(x)\| ≤ P(r)},

and DTC(x, r) = (order b\*, value \|V\_{b\*}(x)\|). If the set is empty, DTC reports DRAW.

### 8.2 Layer correctness

**Theorem 8.3.** Suppose every lower-life batch stores V\_b at every budget and this batch stores V\_{k−1}. Sub-tables and exit tables are exact by induction over the build order (Theorem 4.2). Then after layer k is solved, the stored layer equals V\_k for every cell of the batch.

*Proof.* **Child oracle.** Every child test equals Definition 8.1. The push read gives "mover wins" iff k > 0 and the child is a loss at layer k − 1, and "mover loses" iff it is a win at layer k, through the complete layer maps of strictly earlier batches (Theorem 4.6); by Lemma 8.8 at the lower-life child, never both. The batch's own budget-(k − 1) entry, read for the copy or seed and the commit-flag comparison, was set when layer k − 1 finished, and a resume restores it from a checkpoint written after that. The loss check and retrograde read layer k directly, exits read the exit table at budget k, and captures read WDL sub-tables with cursed classes folded to draw.

1. **Initialization.** Every cell holds the layer-k initial classification. For k > 0, a cell is copied from layer k − 1 only when that entry is budget-independent.
   - **Fact A.** Initialization reads the layer only in push and exit reads, each of which first sets the budget-dependence flag. If initialization stops early at a winning capture while the flag is still clear, it has visited only quiet moves and captures, and the result is win(1) at every budget. So a cell with a clear flag classifies identically at every budget.
   - **Fact B.** The initialization provenance bit is set iff the budget-dependence flag is clear, and seeding carries the provenance bits through every retrograde write. A budget-independent intermediate read no exit, so it has bound 0 and at most the hint *draw*, which the one *draw* provenance bit lets seeding rebuild exactly. A budget-independent final, a capture win(1) or a loss with no quiet move, is never overwritten by retrograde (losses are skipped, win(1) is minimal), so copying it is exact. A stale change flag only triggers an exact re-check.
2. **Retrograde.** With the boundary fixed, the iteration is the retrograde of Section 5 applied to quiet children at layer k, with four differences.
   - **Zeroing moves in the loss check.** Counting each zeroing move as contribution 1 without reading it is exact: a zeroing win is already final, and a zeroing draw, a push included, sets the hint *draw*, which the check rejects, unless the cell has no quiet legal move; then it is no quiet predecessor, so it is never flagged, and its bound 0 is never due.
   - **Overwrites.** A win overwrites a larger one only in castling materials: otherwise every initial win is win(1), and retrograde targets arrive in increasing order.
   - **The cap at 100.** Definition 8.1 has no value above 100, and the pass loop stops after the pass pair at ply 100 (Proposition 8.7).
   - **Several fusions.** All fusions are initialized before any is iterated, and with several fusions the frontier is reseeded before each, restoring every flag an earlier iteration cleared (safe by Lemma 10.12).

   By uniqueness, the fixed point computed is V\_k. ∎

**Lemma 8.4 (seeding is exact).** Write ι\_k(x) for the layer-k initial classification of a cell x. Let k ≥ 1 and let x be a legal cell of the batch whose budget-dependence flag is clear at budget k − 1. Then ι\_{k−1}(x) = ι\_k(x), and this is L\_0, a W\_1 reached by a capture or promotion, an L\_1 with no quiet move, or an intermediate of bound 0 whose only possible hint is *draw*. Initialization of layer k writes ι\_k(x) at x, apart from a change flag carried over from layer k − 1. In particular, the bound 0 of a seeded intermediate is the bound of ι\_k(x).

*Proof.* We first show that ι\_{k−1}(x) = ι\_k(x) and that this classification has the stated form, and then that initialization of layer k writes it. The flag starts clear, every push and exit read sets it, and nothing clears it. The largest exit-loss distance (initially 0) and the exit-win distance (initially undefined) change only on exit reads. So under a clear flag every decisive option is a capture or promotion, and ι is as follows:

- with no legal move, L\_0 in check and a bare intermediate otherwise;
- W\_1 with a winning option;
- with no quiet move, L\_{max(1, 0)} = L\_1 if the best option loses and a bare intermediate if it draws;
- otherwise, the intermediate with hint *draw* or bound 0.

Fact A gives ι\_{k−1}(x) = ι\_k(x).

It remains to show that initialization of layer k writes ι\_k(x) at x, apart from the change flag. Call an intermediate or final *marked* when its initialization provenance bit is set, and call a final *seeded* when seeding wrote it over a marked intermediate; a seeded final records that intermediate's *draw* hint in its provenance bits. We show, by induction on k and within layer k over the writes in order, that the layer-k word S of every legal batch cell x satisfies at every moment:

(a) if S is a marked intermediate, the flag of x is clear at budget k and S is ι\_k(x) with the mark, apart from the change flag;

(b) if S is a seeded final, the flag is clear, ι\_k(x) is an intermediate, and S records its *draw* hint;

(c) if S is a marked final, the flag is clear and S is ι\_k(x) with the mark.

Quiet predecessors stay in the pair (Theorem 4.7), so only layer k of x's batch writes x's layer-k word. We take the kinds of write in turn.

- *Fresh classification.* A fresh classification is marked iff the flag is clear (Fact B), so it satisfies (a) or (c) or makes no claim. At k = 0 only fresh classification writes.
- *Copies.* Copies go from (a) to (a) by Fact A and from (c) to (c).
- *Seeding.* A seeded cell is in (b) at k − 1, so by the first part ι\_{k−1}(x) is the bound-0 intermediate with the recorded *draw* hint, which seeding writes marked: (a) at k by Fact A.
- *Iteration.* A final written over (a) records the *draw* hint of ι\_k(x), giving (b), and one written over (b) carries the record. Marked finals are never overwritten (Fact B), other finals claim nothing, and intermediate writes touch only the change flag.

Initialization copies or seeds exactly the cells in (a), (b) or (c) at k − 1 and classifies the rest afresh, so it writes ι\_k(x) apart from the change flag. ∎

**Lemma 8.5 (carried change flags).** A change flag that initialization of layer k carries over from layer k − 1 is cleared by the loss check at ply 0 of layer k, before the first mark sub-phase of that layer, and the check writes nothing else.

*Proof.* Change marking flags only quiet predecessors, which have a legal quiet move. Only White cells carry flags out of a layer: a Black flag set in the mark sub-phase of P(W, p) is consumed in the cell sub-phase of P(B, p), and every layer and checkpoint ends with a complete pass pair. At ply 0 of layer k, P(W, 0) runs first and writes White cells only in its own re-checks; as initialization flags every legal cell's chunk (Lemma 5.6), it loss-checks each flagged White cell while it is still intermediate. The check fails, since at ply 0 every quiet child disqualifies, a non-win outright and a win because its value is at least 0 = ply, and a failed check clears the flag. Ply 0 has no mark sub-phase, so the layer's own flags come later, and a fusion resumed past ply 0 had completed it. ∎

**Lemma 8.6 (DTC double-push valuation).** Let m be a double push from a legal cell x at layer k, let y = x·m, let E be the opponent's legal en-passant captures from y and N its other legal moves. The valuation returns a class w with: w = LOSE iff m wins by row P of Definition 8.1 on the combined value; w = WIN iff m loses by that row; and otherwise w = DRAW. The double push sets the budget-dependence flag.

*Proof.* The captures E are generated on the board after the push and made on the full decoding of y's cell. By the argument of Lemma 6.4 the two boards agree, with the same rights. So each capture reads the child of the true reply at its conversion value (Lemma 4.9), never ILLEGAL, and its opponent-side class is the inverse of the read, with cursed and blessed classes folded to D. A capture value does not depend on the budget.

The no-en-passant read returns LOSE iff k ≥ 1 and V\_{k−1}(y) is a loss, WIN iff V\_k(y) is a win and the first test failed, and DRAW otherwise. By Lemma 8.8 a loss at k − 1 is a loss at k, so the read is WIN iff V\_k(y) is a win.

At a budget j, the combined value is:

- a loss iff every capture loses for the opponent and either N is empty or V\_j(y) is a loss;
- a win iff some capture wins for the opponent, or N is non-empty and V\_j(y) is a win.

We distinguish three cases.

- *E = ∅.* The valuation returns the read alone, which is row P on V(y).
- *N = ∅.* The valuation returns the maximum over E. It is LOSE when every capture loses, a case in which m wins by row P iff k ≥ 1; at k = 0 DRAW is returned instead, so the class is neither a win nor a loss. It is WIN when some capture wins, a loss at every budget.
- *E and N both non-empty.* The valuation returns the maximum of the read and the captures. This maximum is LOSE iff k ≥ 1, V\_{k−1}(y) is a loss and every capture loses, that is, iff the combined value at k − 1 is a loss. It is WIN iff some capture wins or V\_k(y) is a win, that is, iff the combined value at k is a win.

The push is a non-converting zeroing move (row P), which sets the flag. ∎

**Proposition 8.7 (stopping at ply 100).** Stopping the pass loop of layer k after the pass pair at ply 100 leaves V\_k in every cell of the fusion, whether or not that pair wrote.

*Proof.* By invariant (C) of Theorem 5.12, applied to the DTC retrograde in Theorem 8.3, after P(c, p) every color-c loss of value ≤ p and opposite-color win of value ≤ p + 1 is final and exact; so after P(W, 100) and P(B, 100) every value ≤ 100 is, and none exceeds 100 (Definition 8.1; win propagation writes no target above 100). The staggered edge cases fall inside (Lemma 5.5): a White L\_100 through a Black W\_99 child is flagged at P(B, 99), mark value 99, and checked at P(W, 100); a Black L\_100 through a White W\_99 child is flagged at P(W, 100), mark value 99, and checked at P(B, 100); a cell whose maximum is its bound 100 is due at ply 100. So every remaining intermediate has V\_k(x) = D, as it decodes; soundness is that of Theorem 8.3. ∎

### 8.3 Monotonicity and the DTZ limit

**Lemma 8.8 (monotonicity in the budget).** For k < k′, V\_k(x) ⪯ V\_{k′}(x). Exit tables are monotone by induction over castling rights.

*Proof.* Induct on life, then on n = \|V\_k(x)\|. *Win case.* A C witness is budget-independent; a P witness has V\_{k′−1}(c) a loss (life induction); a Q witness has V\_{k′}(c) = L\_{≤ n−1} (value induction). So x wins at k′ with contribution ≤ n. *Loss case.* Every move still loses at k′ with contribution ≤ n, and no P move wins, as V\_k(c) is a win and k ≤ k′ − 1, so V\_{k′−1}(c) is a win (life induction). ∎

**Lemma 8.9 (the unbounded layer is DTZ).** On clean cells, V\_∞ equals DTZ, and V\_∞ = D on cursed, blessed and drawn cells.

*Proof.* With ∞ − 1 = ∞, Definition 8.1 becomes the clean attractor of Section 2.3: a zeroing move contributes 1, a quiet move the child's value + 1, the cap is 100, and cursed classes fold to D. The clean DTZ solves it, and the solution is unique. ∎

**Corollary 8.10.** A decisive V\_k(x) implies that DTZ(x) is decisive in the same class with DTZ(x) ≤ \|V\_k(x)\|. In particular, every cursed or blessed cell is D in every layer.

### 8.4 The budget bound

**Lemma 8.11.** Let x lie in a slice of life λ with N pawns. If k ≥ λ − N, then V\_k(x) = V\_∞(x).

*Proof.* A pawn of life ℓ makes at most ℓ − 1 non-converting pushes, as its last step promotes, so λ − N bounds the remaining ones. V\_k ⪯ V\_∞ by Lemma 8.8; for the converse, induct on (λ, value). A winning push lands in life ≤ λ − 1, where k − 1 ≥ life(c) − N. Exits keep the pawns, hence λ and N, so row E at budget k is exact by the lemma for the exit material (induction over castling rights). If k = 0, every pawn has life 1 and no push exists. Antisymmetry gives equality. ∎

**Theorem 8.12 (budget bound).** Pawn life is at most 6 and N ≤ 6, so λ − N ≤ 5N ≤ 30. A slice with N pawns has life in \[N, 6N\], so a material has at most 31 batches (batches have distinct lives, Theorem 4.6), and the layer map, one row per batch, fits the checkpoint's 31-row layer-alias table. So V\_b = V\_∞ = DTZ for all b ≥ 30, budgets 0 to 29 with the DTZ row determine every layer, and storing min(λ − N + 1, 30) layers per batch, with higher budgets aliased to the last real layer, loses nothing (Lemma 8.11). ∎

**Lemma 8.13 (layer-map reads).** Every lookup of a slice's layer map reads a slice that its batch has already claimed, so it returns that batch's map; the value a slice holds before it is claimed is never read.

*Proof.* Batch i claims its slices, both members of each mirror pair, before any work of the batch begins, and every slice lies in some batch, since batching keeps one representative of every mirror pair (Theorem 4.6). A resumed run claims batches 0 to i in order, including those it skips, so the claims, and the maps restored from the checkpoint, are those of the uninterrupted run. Lookups occur in three places. (i) The fusion need-sets, the copy of layer k − 1 into layer k at initialization, and the layer below the active one read active slices of batch i. (ii) A push read, and the need-set entries for push targets, read a push target of an active slice, which lies in an earlier batch (Theorem 4.6), claimed before batch i. (iii) The packer and its rank gather run after the last batch, when every slice is claimed. Exit reads go to the exit twin's file, whose layer-alias table is its own. By Theorem 8.12 a material has at most 31 batches, so every claimed index fits the layer map and the checkpoint's 31-row table. ∎

### 8.5 Saturation

**Theorem 8.14 (saturation is sound).** Suppose the saturation test holds for layer k − 1 on every fusion of a batch, so the stored layer equals clean DTZ cell by cell. Then V\_l = V\_{k−1} for every l ∈ \[k − 1, 30\].

*Proof.* The check gives V\_{k−1} = DTZ, which equals V\_∞ by Lemma 8.9. Lemma 8.8 gives V\_{k−1} ⪯ V\_l ⪯ V\_∞ = V\_{k−1}, and antisymmetry gives equality. ∎

The saturation test is applied only while the commit flag is clear. Initialization sets this flag when it classifies a fresh decisive entry differing from the layer below. The flag can only delay saturation, and soundness rests on Theorem 8.14 alone. Layer-k cells written before a saturation break are never read: budget k aliases to the last real layer, and gathering and packing read only real layers.

### 8.6 Pack layout and reading

**Theorem 8.15 (pack).** For each cell with decisive DTZ, the DTC packer writes the DTZ, as decoded from the DTZ file, in row 0. In every row j for 1 ≤ j ≤ 30 it writes V\_{30−j} or DRAW (Lemma 8.16), read from the layer that budget 30 − j aliases to, which holds V\_{30−j} (Theorem 8.14, Lemma 8.11). The column in pack order is a decisive prefix and a DRAW suffix (Lemma 8.8, Corollary 8.10), hypothesis (M) of Theorem 11.15, so the codec round-trips it.

Row 0 is 0x7FF exactly for a cell whose head-layer entry is ILLEGAL and for a legal cell whose DTZ is DRAW. The lower rows of such a cell keep stale values from the reused buffer. Since 0x7FF has no rank (Lemma 8.17), the encoder writes a don't-care constant record for the cell without reading those rows, exactly and deterministically. No reader decodes an ILLEGAL or DRAW class (Lemma 12.16). ∎

**Lemma 8.16 (alias rows).** For every batch there is a K ≥ 1 such that its layer map sends budget b to b for b < K and to K − 1 for b ≥ K. Consequently the DTC packer writes each of rows 1 to 30 exactly once for every cell whose row 0 is not 0x7FF, and in particular for every cell with decisive DTZ.

*Proof.* K is the layer count of Theorem 8.12, or K = k after a saturation break following layer k − 1 (Theorem 8.14); the row that stays 0 has K = 1, and a resume restores the uninterrupted run's rows (Theorem 10.18). So the budgets aliased to real layers form intervals partitioning \[0, 29\], and the packer, visiting real layers from K − 1 downward, writes rows 30 − b once per interval and row 0 at K − 1. It skips the lower real layers only when row 0 is 0x7FF, which a decisive DTZ value, at most 2045 (RANGE, Section 13.1), never is. ∎

**Lemma 8.17 (DTC rank coverage).** Every value other than 0x7FF that the DTC packer writes into a read column has a rank in the layered rank table, and 0x7FF has none.

*Proof.* Let L be the largest layer count. Per cell, the gather reads the layer that budget L − 1 aliases to, which is the head layer K − 1 as K − 1 ≤ L − 1 ≤ 29, and every other real layer. These are exactly the packer's layers (Lemma 8.16). A DTC entry contributes its value, at most 100, unless it is DRAW, ILLEGAL or, in loss-only mode, a win. Unless it is 0x7FF, the DTZ value contributes whenever the top entry is not ILLEGAL, which is exactly when the packer reads it. The packer writes 0x7FF for DRAW and ILLEGAL, so every other value it writes has a rank and 0x7FF has none; loss-only win values may lack a rank but are never read (Theorem 12.21). Skipped groups hold no cell of an owning pawn slice, since a cell's pawn slice is non-decreasing in its index, so no owned cell is skipped. ∎

**Theorem 8.18 (reader).** The DTC reader returns (b\*, \|V\_{b\*}\|) of Definition 8.2 or DRAW. For cursed and blessed cells it returns DTZ only.

*Proof.* Segments, the maximal constant runs, are scanned from the smallest budget upward; the first with value at most P(r) contains b\*, and its smallest budget, reported as the order, is the exact minimum. Pawnless materials have no push, so V\_b = V\_∞ for every b (Lemma 8.11 with λ = N = 0), which on a WIN or LOSE cell is the clean DTZ (Lemma 8.9), and pawnless pricing applies the same test, DTZ ≤ P(r). The reader and pawnless pricing price only a cell whose exact class is WIN or LOSE. For a CURSED\_WIN or BLESSED\_LOSS cell, every V\_b with b ≤ 29 is D (Corollary 8.10) and V\_30 = V\_∞ = D (Lemma 8.9), so the set of Definition 8.2 is empty, and the cursed DTZ of row 0 is returned without a price. ∎

**Theorem 8.19 (reconstruction).** For every clock r, DTC win derivation and DTC loss derivation return DTC(x, r) whenever the cells they read are available.

*Proof.* **Loss.** For each budget k, the worst child value and the drawn flag implement the loss rule of Definition 8.1 over the children's 31-entry curves. The rule, by kind of move, is as follows.

- A conversion raises every budget to 1.
- A loser's push reads the child at budget k.
- A quiet move or an exit (read in the exit table at budget k) raises by the child's value + 1.
- For a double push, if an en-passant reply wins for the opponent under the 50-move rule, the combined value is W\_1 and the push raises every budget to 1. Otherwise the combined value is a win exactly when the table child's is, at the same distance, and the child's curve records every other case as DRAWN.

Anything over 100 is drawn. If the pushed position's only legal moves are en-passant captures and none wins for the opponent, its combined value is not a win, so the push does not lose and x is not a LOSE cell; this case never reaches loss derivation. At a LOSE cell, the table child of a double push without a winning en-passant reply is therefore a WIN, whose curve the rule reads. The derivation takes the minimum fitting k.

- **Win.** The candidates are (0, 1) for a conversion into a child c with WDL(c) = LOSE, (b\*(c, 0) + 1, 1) for a push whose combined child value is a loss at some budget, and (b\*(c, r + 1), \|V\_{b\*(c, r + 1)}(c)\| + 1) for a quiet move or an exit whose child c has a non-empty set in Definition 8.2 at clock r + 1 with V\_{b\*(c, r + 1)}(c) a loss, the child of an exit being read in the exit table at clock r + 1; those with value ≤ P(r) are kept. Let B = b\*(x, r) and n = \|V\_B(x)\|, and recall P(r + 1) = max(P(r) − 1, 0) for finite r.
  - *A candidate reaches (B, n).* A conversion witness wins at every budget, so (B, n) = (0, 1), a kept candidate. A push witness gives b\*(c, 0) ≤ B − 1, as V\_{B−1}(c) is a loss of distance ≤ 100 = P(0) for B ≤ 29, and by Lemma 8.20 for B = 30, the DTZ row; its value 1 ≤ P(r). A quiet or exit witness has V\_B(c) = L\_{n−1} (in the exit table for an exit) with n − 1 ≤ P(r + 1), so b′ = b\*(c, r + 1) ≤ B; b′ < B would give V\_{b′}(c) = L\_m with m ≤ P(r + 1) ≤ 99, so x would win at budget b′ within m + 1 ≤ P(r) plies, against the minimality of B. So the candidate is (B, n).
  - *No candidate beats (B, n).* Every surviving candidate is a genuine win at its own order, so its order is at least B, and at order B its value is at least n. ∎

**The unclocked entry.** DTC is defined only under the 50-move rule. For a pawnful material, an unclocked probe reads or derives a DTC cell with the unbounded budget P = 100 = P(0), giving quiet moves and exits clock 1 and pushes clock 0, that is, exactly as at clock 0, so an unclocked derivation returns exactly when the same derivation at clock 0 returns. For a clocked probe this holds at every r ≤ 99, since the candidates (0, 1) that end a win derivation early fit every P(r) ≥ 1; at r = 100 none fits, so a win derivation visits every move and may refuse where clock 0 returns. The DTZ it takes is row 0 of a read cell (Theorem 8.15), the clean DTZ of a win or loss derivation (Lemma 12.19), or the cursed value of a cursed derivation (Theorem 12.20), none of which depends on the clock. The probe reports this DTZ and withholds the price.

**Lemma 8.20 (push orders).** Let a non-converting push lead from x to a child c whose value is a clean loss. Then b\*(c, 0) ≤ 29, so the push candidate's order b\*(c, 0) + 1 is at most 30.

*Proof.* A non-converting push keeps N and lowers λ, so λ − N ≤ 29 at c (Theorem 8.12), and V\_29(c) = V\_∞(c) (Lemma 8.11) is the clean loss L\_{z(c)} (Lemma 8.9) with z(c) ≤ 100 = P(0) (Corollary 2.5); so b\*(c, 0) ≤ 29. For a double push whose child has en-passant replies, the combined answer is DTC(c, 0) or the loss (0, 1) (Theorem 12.38), and the bound holds for both. ∎

## 9. Concurrency

The parallel passes are interleaving-independent: every table, frontier bit and aggregate a pass produces is a function of the state before the pass, and passes race only through idempotent or single-valued stores.

Dispatches are separated by the thread pool's mutex and its promise and future synchronization, so races occur only inside one dispatch. Cells, flags and bytes are read and written by plain, non-atomic, naturally aligned loads and stores, and the arguments below use only that each such load returns some value stored to that location.

### 9.1 Work partition

**Theorem 9.1.** For S ≤ E and C > 0, the shared range iterator hands out pairwise-disjoint ranges whose union is exactly \[S, E). Each range lies inside one aligned block of C indices, and a range has length C only if it is that whole block.

*Proof.* Each call takes a distinct atomic ticket. With a = ⌈S/C⌉·C, the first ticket gets the head \[S, min(a, E)) when S < a, and each later ticket the next aligned block of \[a, E), clipped to E; emptiness is monotone in the ticket. ∎

Hence each chunk id (range start divided by chunk size) has exactly one owner. The owner is the only thread that clears the chunk's frontier byte; other threads only set it to 1 (Lemma 9.3).

### 9.2 Interleaving independence

Fix a DTZ, DTM or DTC pass (stm, ply, phase) over group g. Let opp be the other color. The pass runs a mark dispatch Mk and then a main dispatch B.

**Lemma 9.2 (ownership).** Within Mk or B, a stm cell is read or written only by the thread that owns its chunk. Apart from the stm cell at the loop index, every retrograde routine and the loss check read and write opp cells only.

*Proof.* Stm cells are read and written only at the index of the chunk loop over the thread's own range (Theorem 9.1): the *cwin* promotion, the change-flag clear and the loss write. The loss check, change marking, mate-in-one marking and win propagation access only opp, the opponent of the board's side to move stm. ∎

**Lemma 9.3 (the race inventory).** The only concurrent conflicting accesses are the following. By Lemma 9.2 a conflict involves an opp cell or a shared flag: the frontier and dirty bytes and, in DTC initialization, the commit flag. The only opp accesses are the load and flag OR of change marking in Mk, and in B the loads of the loss check and the load and store of the two win routines:

| Dispatch | Location | Conflict | Values stored |
| --- | --- | --- | --- |
| Mk | opp cell | read-then-OR vs read-then-OR | only v \| CHANGE |
| B | opp cell | load vs read-then-store | only W = WIN(ply+1); in DTC, W with the cell's own provenance bits |
| B | opp cell | read-then-store vs read-then-store | only W |
| both | frontier byte, dirty byte | test-then-set | only 1 |
| DTC init | the commit flag | read-then-store vs read-then-store | only true |

**Lemma 9.4 (no lost update).** In a mark dispatch, every cell that qualifies for change marking and has CHANGE clear ends at v \| CHANGE with its frontier bit marked, and every other cell is unchanged. Both outcomes are functions of the pre-dispatch state.

*Proof.* The flag OR is a non-atomic 16-bit OR of the single constant CHANGE (bit 15 in DTZ and DTC, bit 14 in DTM). Flag ORs run only in mark dispatches and full opp writes only in main dispatches. Hence every load returns v or v \| CHANGE and every store writes v \| CHANGE. Change marking skips a cell with CHANGE set; otherwise it ORs CHANGE into the cell and marks its frontier bit. Qualification ignores CHANGE, and a non-qualifying cell is never written. The first thread to reach a qualifying cell with CHANGE clear reads the flag clear, so the cell ends at v \| CHANGE with its frontier bit marked. ∎

**Lemma 9.5 (one value per pass).** In B every opp store writes W = WIN(ply + 1), and a predecessor of a loss that B propagates ends at W exactly when its pre-dispatch value qualifies.

*Proof.* A reverified loss has distance exactly ply and a propagating loss has value ply; at ply 0 only mate-in-one marking writes, storing WIN(1) under one predicate. Whether a thread writes depends only on the value it read, and a thread reading W never writes. Hence such a predecessor ends at W exactly when its pre-dispatch value qualifies; which losses B propagates is fixed by the pre-dispatch state and the verdicts of the loss check, which do not depend on the interleaving (Lemma 9.7). In DTC, W is seeded from the old word with the cell's own provenance bits, so it is still a function of the pre-dispatch value. ∎

**Lemma 9.6 (DTM at ply 0).** In the passes P(W, 0) and P(B, 0), the DTM generator runs no loss check, and its only opp stores are the win(1) writes of mate-in-one marking.

*Proof.* Initialization writes every in-pair cell of both colors of the fusion and writes intermediates without the change flag, so no flag survives from an earlier fusion. At ply 0 the mark sub-phase is skipped and no bound is due, so nothing is re-verified. A resumed run never executes ply 0, since checkpoints record a positive ply. The only writes are thus those of mate-in-one marking, which writes win(1) from stored losses of value 0. By Lemma 9.5 concurrent writers store only win(1), and a thread reading win(1) skips the cell. ∎

**Lemma 9.7 (verdicts are stable).** If the loss check reads a child while it changes from v to W, its verdict is the same for either value.

*Proof.* A qualifying v is one of:

- a draw, which is not a win and so disqualifies;
- a cursed win in the CLEAN phase, which disqualifies;
- a win of value > ply + 1, which disqualifies because its value is not below ply.

W itself, a win of value ply + 1 ≥ ply, also disqualifies. DTM and DTC have the same structure. ∎

**Theorem 9.8 (interleaving independence).** The cells of both colors, the frontier bitmaps, the dirty bytes and the aggregates (combined by OR and max) produced by each dispatch are functions of the pre-dispatch state and of (stm, ply, phase, g) alone. DTM50 initialization and layer building are pull-style, and their only races are test-then-set stores of the value 1: on dirty bytes, and during initialization on group frontier bytes.

*Proof.* In a DTZ, DTM or DTC initialization dispatch each thread writes only its own cells of both colors and reads only sub-tables, exit tables, earlier batches and, in DTC at budget k > 0, its own cells at budget k − 1, none of which the dispatch writes. In a pass each chunk runs sequentially except for opponent reads (Lemma 9.2). A concurrently written opponent cell receives only W (Lemma 9.5; Lemma 9.6 for DTM at ply 0). A thread reading W does not write, and the loss check gives the same verdict for either value (Lemma 9.7), so control flow is unaffected. Lemma 9.4 with Lemma 9.5 (or 9.6) fixes the final opponent state. In a DTM50 dispatch each thread writes only layers of its own cells and reads only layers or files no thread writes during the dispatch, except its own layer-0 cache at h = 0, which it reads just before overwriting and no other thread reads. In every dispatch, stm chunk frontier bytes and DTM50 chunk frontier bytes are accessed only by the chunk's owner (Theorem 9.1). Every other shared byte (opponent frontier bytes, dirty bytes, frontier group bytes, the DTC commit flag) is only set to 1 (true) by test-then-set, or cleared by the main thread between dispatches. It therefore ends at 1 exactly when it was 1 before the dispatch or a writer existed, which the pre-dispatch state determines. OR and max are commutative and associative. ∎

## 10. Paging and checkpoint soundness

Paging is semantically invisible. Every cell a dispatch touches is resident, dirty data is never lost, and a resumed run produces the same tables as an uninterrupted one.

### 10.1 Coverage of the need-set

**Lemma 10.1 (reach of a quiet move).** A quiet non-pawn move from pawn slice p and king slice k lands, if anywhere, in pawn slice p or mirror(p). Its king slice is k or one of neighbors(k).

*Proof.* The king-neighbor table is built from the same king-slice lookups that the quiet-move index map uses. A non-identity transform mirrors the pawns by the same procedure as mirror(s). Non-king moves keep both slices, and stabilized slices only occur for pawnless materials (Theorem 4.7). ∎

**Lemma 10.2 (groups).** Let σ be the number of slices per group and E = σS. A table of T cells has T/S = Pn·K slices. Group g holds the slices \[gσ, min((g + 1)σ, Pn·K)), and hence the cells \[gE, min((g + 1)σ, Pn·K)·S). For every cell p < T, the group of p's slice ⌊p/S⌋ is ⌊p/E⌋, and addressing p through its slice gives the same entry as addressing it directly. In particular, groups are unions of whole slices, the range a dispatch scans for group g is exactly the cells of g, and the frontier group bit that a write sets is the bit of the written cell's group.

*Proof.* The nested-floor identity gives ⌊⌊p/S⌋/σ⌋ = ⌊p/(σS)⌋ = g, every division being exact by Lemmas 3.4 and 3.7. Through its slice t = ⌊p/S⌋, cell p sits at offset (t − gσ)·S + (p − tS) = p − gE of g's array. ∎

**Theorem 10.3 (coverage).** During every DTZ and DTM dispatch, every accessed cell lies in a resident group.

*Proof.* The stm group g is always needed. Opponent accesses lie in the king-neighbor reach, which is marked across every active pawn slice; Lemma 10.1 applies because fusion selection closes the active set under mirroring. Initialization also needs the push-target reach for both colors. The working set loads every needed group and evicts only unneeded ones. No paging occurs between window refills, and a group skipped as inactive cannot become active before the window reaches it. ∎

**Theorem 10.4 (coverage, DTM50).** Every cell a DTM50 dispatch reads or writes lies in a resident group.

- *Proof.* **Layer passes.** A cell reads its own layer-0 cache and writes its own layer h. The layer builder skips zeroing moves and reads only non-zeroing children. Of those:
  - a quiet child is read in the opponent's layer h + 1, in pawn slice p or mirror(p) and within the king-neighbor reach (Lemma 10.1);
  - an exit reads another file;
  - at h + 1 = 100 the child is decided by a mate test and nothing is read.

  The need-set marks the mover's layers 0 and h for the group and the opponent's layer h + 1 across the king-neighbor reach; the pinned form marks the same three layers on the fusion's groups, which contain every king slice of every active pawn slice.
- **Initialization at clock 99.** A cell writes layers 0 and 99 of both colors, reads push targets in layer 0 and captures and en-passant replies in sub-tables; a quiet move reaches clock 100 and reads nothing. The need-set marks layers 0 and 99 of the group and the layer-0 push-target reach, for both colors. ∎

**Theorem 10.5 (coverage, DTC).** Every cell a DTC dispatch reads or writes lies in a resident group.

- *Proof.* **Initialization of budget k.** A cell writes its layer k and reads its own entry at budget k − 1 through the layer map at k − 1, which is the same layer for all active pawn slices since they belong to the current batch and share one alias row. A push reads the target slice at budgets k − 1 and k through the layer map for t, with the king slice unchanged (pawnful king slices have no stabilizer). The push-target need-set marks exactly these two layers of that group for every target, for both colors. Captures and exits read other files.
- **Iteration.** The loss check and retrograde routines access only layer k (the DTC read uses the current layer), reaching quiet children and predecessors within the king-neighbor reach; zeroing moves are counted, not read. The need-set marks the mover's group and the opponent's king-neighbor reach in layer k.
- **The saturation test** reads only the fusion's own cells in one layer per stored color and marks exactly those groups. For a symmetric material it checks only White, which suffices since V\_k, defined by the game, is color-mirror invariant and each stored layer equals V\_k (Theorem 8.3). ∎

**Theorem 10.6 (pinned phases).** When a phase is pinned instead of paged by windows, every cell it reads or writes lies in a resident group.

*Proof.* A pinned phase loads its pin set once through the working set, which loads every needed group and evicts only unneeded ones. It then does not page until the phase ends, so the pin set must cover the phase. The pin flag is set before the first access of every paging phase (initialization and iteration in DTZ, DTM and DTM50; initialization, the saturation test and iteration in DTC). Every paging phase reassigns it on entry, to true only if its own pin set fits, and fusion selection also clears it. Hence a pin never outlives its phase. Each pin set covers its phase:

- The set of all groups contains everything.
- DTZ and DTM iteration pin, for both colors, every group meeting the king-slice run of an active pawn slice. The stm cells lie in active pawn slices, and the opponent cells in the king-neighbor reach, which lies in active pawn slices by Lemma 10.1 because fusion selection closes the active set under mirroring.
- DTZ and DTM initialization also pin the full king-slice run of every push target, which contains the push-target reach.
- DTM50 layer passes pin the three layers of the need-set of Theorem 10.4 on the same groups. DTM50 initialization pins, for both colors, layer 0 on those groups and the push-target runs, and layer 99 on those groups, covering its need-set.
- DTC pins exactly the layers of its need-set (Theorem 10.5) on the same groups, together with the push-target layers during initialization.

With Theorems 10.3, 10.4 and 10.5 for window-paged phases, every access in every phase lies in a resident group. ∎

### 10.2 No lost data

Let L(g) be a group's logical content: its RAM array if resident, the unpacked chunks if packed, otherwise its spill file.

**Lemma 10.7 (dirty invariant).** Every paging operation preserves L(g) on every cell that has been written. Moreover, a clear dirty flag implies that the non-RAM state reproduces L(g) on those cells. Every write goes through a cell write or a flag OR, which set the bit.

*Proof.* No group is ever both resident and packed: each change of form creates one form and discards the other in one step, and a new group has neither. So L(g) is well defined, and each transition keeps the bytes or writes them out before dropping them:

- A load from packed chunks unpacks exactly, and the dirty bit carries over.
- A load from disk reads exactly the file and clears the bit.
- Evicting a resident group packs it and keeps the bit.
- Evicting a packed dirty group writes the file, then clears the bit.
- A flush writes the file of every dirty group, resident or packed, then clears its bit.
- A clean group is dropped only when its file already holds L(g), or when it was never spilled and so has no written cell.

Writes set the bit. ∎

**Lemma 10.8 (initial contents).** A group created without a spill file is clean. For DTZ, DTM and DTC it holds uninitialized bytes, none ever read: initialization writes every cell of every active pawn slice, reads touch only active or earlier-batch cells, and fusions partition every batch. DTM50 groups are zero-initialized. A cell with no zeroing move has layer 0 written only at h = 0, layer passes read layer 0 to decide legality, and gathering ranks reads layers h ≥ 1 of cells illegal for one color. Zero decodes as DRAW, not ILLEGAL, so the legality test is exact and no spurious rank enters the table. ∎

**Lemma 10.9 (round trip).** Packing, unpacking, spilling and loading a group are lossless LZ4 round trips over the same chunk boundaries, so a reload returns identical bytes. ∎

### 10.3 The save cache

**Theorem 10.10.** The save cache has two properties:

- (a) A pinned group is always LOADING or RESIDENT, so it is never evicted.
- (b) It is deadlock-free.

* *Proof.* (a) A pin is taken only on a new entry, which starts LOADING; on a RESIDENT entry; or on a PACKED entry, which becomes LOADING in the same critical section. Hence a PACKED entry has no pins. RESIDENT becomes EVICTING only when the pin count is zero, and an acquire that finds EVICTING waits.
* (b) Every wait is discharged by the thread holding the in-flight load or eviction. That thread releases the mutex for its I/O, then re-locks, updates the state and wakes all waiters in one critical section, so no wake-up is lost. The sweep returns instead of waiting when every group is pinned, so no wait depends on a pin release, and the wait-for graph is acyclic. ∎

**Lemma 10.11 (sub-table block cache).** When sub-tables are read through the prober, every read of a block returns the decompressed bytes of that block of the requesting reader's file, whatever the block-cache budget.

*Proof.* Each block cache takes a fresh epoch from a never-repeating global counter. A thread-local hit requires the same epoch and block number, so it returns a block built for this cache and block. A shared entry is keyed by owning cache and block number and built by that owner for that block. A destroyed cache removes its entries, so a live entry belongs to the cache now holding its key. Blocks are decompressed from an unchanging read-only mapping (Theorem 4.17) and are immutable once built: eviction only drops a reference, and the loser of an insert race keeps its own copy. Hence the budget, even 0, changes only which copies are retained, and the caller always gets the block it built or found. ∎

### 10.4 Checkpoints

**Lemma 10.12 (reseeding is safe).** Call a cell a *trigger* for a pass if the pass would act on it:

- a win of value ply or ply − 1;
- a flagged or due intermediate;
- a loss of value ply; or, at cursed ply 1, a pending cwin or closs cell, which the pass promotes or re-checks. The chunk of such a cell holds a pending intermediate and so stays flagged.

If two frontiers F₁ and F₂ both flag, in their chunk and group bits, every chunk containing a trigger, a pass produces identical cells under either. A chunk only one of them flags holds no trigger, so scanning it writes nothing, and a chunk both flag is cleared or kept by the same cells. Each resulting frontier again flags every chunk containing a trigger of the next pass: the frontier rules of Lemma 5.6 maintain this property from any frontier that has it, whatever other chunks it flags, since a set bit only causes a scan. By induction over passes, two runs from the same cells whose frontiers have this property make identical writes. This covers the all-ones frontier written on resume, and the frontier left by a pass pair that only the resumed run executes. ∎

**Lemma 10.13 (quiescence).** For DTZ, suppose a phase is quiescent at ply q: neither pass reported activity (Theorem 5.16), and q > M. Then every later pass of that phase is a no-op until the cursed continuation, and that continuation acts only on cells of value ≥ 100 that originated in the CLEAN phase.

*Proof.* Each of the four sites that create a value or flag reports activity: the *cwin* promotion, a mark sub-phase finding a win of value ply or ply − 1 (which precedes any change marking), a confirmed loss with its propagation, and propagation of a stored loss. The only other write, clearing CHANGE after a failed check, creates nothing, and no pass after ply q makes it. A mark-sub-phase flag is consumed by the next pass of the cell's color (Lemma 5.6), so after the pair at q only flags from the mark sub-phase of P(B, q) could remain; it marks from black wins of value q, and finding one is activity. A later pass finds no win of its marked value, so it sets no flag either. As in Lemma 10.14, no value of distance ≥ q below 100 exists after ply q (boundary values of distance 100 and 101 are acted on only from ply 100). A pass at p > q could create only a loss of value p, which needs a quiet child win of value p − 1 ≥ q or a bound p above every initial bound, or a win p + 1 from such a loss. The cursed continuation starts at ply 100 from the clean L\_100 and W\_100 cells, the only values of distance ≥ 100 the CLEAN phase leaves (Theorem 5.16). ∎

**Lemma 10.14 (DTC quiescence).** Fix a fusion of a DTC budget layer, and let M be the largest initial value or exit bound among its cells. Suppose the pass pair at ply q reports no activity and q > M. Then every pass pair at a ply p > q writes nothing and returns false.

*Proof.* A pass at ply p reports activity exactly when its mark sub-phase finds a win of value p or p − 1, a re-verification finds a loss of value p, or its cell sub-phase finds a stored loss of value p. Otherwise it writes only to clear a change flag on a failed re-check. It creates values only in the last two cases: a loss of value p and wins of value p + 1 from it.

**Every decisive value above M sits on a chain.** A loss of value v > M + 1 with a quiet move has contribution v attained by a quiet child win of value v − 1, because its bound is at most M and a zeroing move contributes 1 ≤ M + 1. A win of value v > M + 1 was written from a loss of value v − 1. So every distance from M + 1 up to v is attained.

**No value of distance q or more exists after ply q.** The pair at q reported no activity, so no win of value q − 1 or q and no loss of value q lay in a flagged chunk. A chunk is cleared only if it holds no pending intermediate and its largest classified value m has m + 1 < ply. Retrograde writes re-flag their chunk. A loss confirmed at ply p does not, but its chunk is kept at ply p by the re-verified intermediate and up to ply p + 1 by m ≥ p. Likewise, a win in 1 written at ply 0 keeps its chunk up to ply 2. So every cell of value ≥ q − 1 is in a flagged chunk at ply q, and by the chain property a value of distance ≥ q would force one of distance exactly q, a contradiction.

**Nothing new appears later.** By induction on p > q, a pass at p could create only a loss of value p, or a win of value p + 1 from it. The loss would need a quiet child of value p − 1 ≥ q or a bound p > M, and neither exists. The mark sub-phases at q found no win of the marked value, so no change flag remains to clear, and each later pass writes nothing. ∎

**Lemma 10.15 (quiescence across the restart table).** For DTZ, suppose the CLEAN loop settles (f = true). Let the pass pair at a ply p > M, in either phase, write nothing, and let the next pass pair be a CURSED pair at a ply q > p. Then that pair writes nothing, not even a change-flag clear, and its loop ends after it.

*Proof.* Three facts hold after the pair at p.

(i) No clean value of distance ≥ 100 and no boundary seed exists. The CLEAN loop settled at a ply c with M < c ≤ 100, and no W\_100 or L\_100 exists. For c ≤ 99, the witness chain of Theorem 5.16 from a clean value of distance 100 meets distance c, and that value lies in a flagged chunk at ply c (as in Lemma 10.14), so the pair at c would have reported activity. For c = 100, an L\_100 is confirmed or propagated at ply 100 and a W\_100 is found by its mark sub-phase at ply 100, each of which is activity. A CW\_101 comes only from a clean L\_100 (Lemma 5.11) and a BL\_101 only through a W\_100 child (Lemma 5.13), so every cursed value descends from an initialization seed recorded in pc₀.

(ii) No cursed value of distance ≥ p exists. If the pair at p is CLEAN, a later CURSED pair implies pc₀ false (restart table), so no seed exists and by (i) the cursed band is empty. If it is CURSED, by (i) every cursed value is an initial entry, a distance-1 value written at cursed ply 1 (the *cwin* promotion or the re-check of a *closs* cell), or a value of distance v witnessed by a cursed quiet child of distance v − 1. Descending witnesses from distance v > M thus meets every distance from M + 1 to v, as the descent ends at distance ≤ M or 1. So, as in Lemma 10.14, a cursed value of distance ≥ p forces one of distance exactly p in a flagged chunk at ply p, and then the pair at p would have written.

(iii) No cell carries the change flag. A mark-sub-phase flag is consumed by the next pass of the cell's color, which confirms the cell or clears the flag, with the chunk kept flagged until then (Lemma 5.6). Only flags from the mark sub-phase of P(B, p) could remain, and it marks from black wins of value mcv(B, p) = p, which would have made the pair at p write (Theorem 5.16).

A pass of the pair at q acts only on triggers (Lemma 10.12). As q > p ≥ 1, the *cwin* promotion and the cursed-ply-1 re-check do not occur. A win the CURSED phase marks is tagged or of value ≥ 100, and would have value q or q − 1 ≥ p. A loss it propagates is tagged or of value ≥ 100, and would have value q. Facts (i) and (ii) exclude both. Fact (iii) excludes a flagged intermediate, and Theorem 5.4 excludes a due one, whose bound would be q > M. So there is no trigger, and since the change flag is cleared only inside a check, nothing is written. As q > M, the stopping rule of Theorem 5.16 ends the loop. ∎

**Lemma 10.16 (the DTC threshold).** The threshold M with which the pass loop of a DTC fusion runs is exactly the largest initial value or exit bound of Lemma 10.14, and M ≤ 100. The same holds for the layer-wide maximum M\_L of Theorem 10.18.

*Proof.* Initialization takes the maximum over the words it writes: each freshly classified final's value, each freshly classified intermediate's bound, and each copied word's value or bound. Seeded intermediates (bound 0 by Lemma 8.4), copied intermediates (bound 0) and ILLEGAL cells add nothing, and fresh classification of a legal cell never yields ILLEGAL. It yields L\_0; W\_1; a win W\_e through an exit to a loss of value e − 1, e ≤ 100; a loss L\_d with no quiet move, 1 ≤ d ≤ 100 since d is 1 for a zeroing loss or else an exit-loss distance ≤ 100; or an intermediate whose bound is an exit-loss distance ≤ 100. So M is the maximum of every initial value and bound, M ≤ 100, and M\_L is the maximum of the fusions' M. ∎

**Theorem 10.17 (resume, DTZ, DTM, DTM50).** A run interrupted and resumed with the same binary, inputs and options writes the same tables as an uninterrupted run.

*Proof.* At the interrupt the main thread is between dispatches, and flushing makes the spill files hold L(g) (Lemma 10.7). The DTZ checkpoint records (batch, fusion, phase, pending flag, ply, maximum), the DTM checkpoint records (batch, fusion, ply, maximum), and the DTM50 checkpoint records (batch, fusion, layer). Each also records the effective paging budget, which fixes the fusions; a pawnless material has a single fusion under any budget, so its checkpoint records budget 0 and matches every run. A checkpoint whose budget differs from the run's is treated as absent, and the run starts afresh. Everything else is recomputed deterministically from these or is paging state.

Only the frontier (restored by Lemma 10.12) and the local finished flag are lost. Most of the argument concerns the DTZ solve. We first describe its loops and how a checkpoint resumes them, then prove a Claim about CURSED pairs at plies p ≥ 100, and finally compare the interrupted and uninterrupted runs by cases.

The DTZ solve runs ply 0 once and then up to three loops:

- the CLEAN loop, over plies 1 to 100;
- the first cursed loop, over plies 1 to 100, only when pc₀ is true;
- the continuation, a cursed loop starting at ply max(p, 99) + 1 after a first cursed loop ending at p, or at ply 101 when pc₀ is false and the CLEAN loop did not settle.

Since checkpoints record a positive ply, ply 0 is never resumed. A loop settles at the first ply q > M whose pass pair reports no activity (Theorem 5.16). Here, in Lemmas 10.13 and 10.15 and in the Claim below, a pass pair "writes" when either pass reports activity. Each of these results also shows that a pair it covers changes no cell, change flags included, so an added pair that writes nothing leaves every cell as the uninterrupted run has it. The run returns after the CLEAN loop if that loop settles and pc₀ is false; after the first cursed loop if both it and the CLEAN loop settled; and after the continuation when it settles or reaches its last ply.

After every pass pair at a positive ply the solver checks for an interrupt. On an interrupt it records phase, ply and a pending flag, and abandons the solve. The pending flag is pc₀ in the CLEAN loop, "the CLEAN loop did not settle" in the first cursed loop, and false in the continuation. A resumed run treats its loop as unsettled. From a CLEAN checkpoint it continues the CLEAN loop. From a CURSED checkpoint below ply 100 it continues the first cursed loop with the recorded flag, and from a CURSED checkpoint at ply 100 or above it resumes the continuation.

*Claim.* Let the pass pair at a ply p ≥ 100 be a CURSED pair that writes nothing, with p > M, and let the next pair be the CURSED pair at p + 1. Then that pair writes nothing, and its loop ends after it.

*Proof.* Call a value relevant if the CURSED phase acts on it: a win or loss that is tagged or of value ≥ 100. A relevant loss of value v ≥ 101 with v > M is not initial, since initial values are ≤ M. So it has a quiet move, and its contribution v is neither a bound (≤ M) nor a zeroing 1; it therefore comes from a quiet child win of value v − 1 ≥ 100, which is relevant. A relevant win of value v ≥ 101 with v > M is not initial, so win propagation wrote it from a final loss of value v − 1 ≥ 100, also relevant.

So a relevant value of distance p + 1 forces one of distance exactly p. At ply p the mark sub-phase of its color finds a relevant win of value p and the cell sub-phase propagates a relevant loss of value p. Either would have made the pair at p write, since the value's chunk was flagged, as in Lemma 10.14. A chunk is cleared only if it holds no pending intermediate and its largest classified value m has m + 1 < ply. Each value-creating write flags its chunk or finds it held by the re-verified intermediate, and m ≥ p keeps it flagged through ply p. Hence after the pair at p no relevant value of distance p or p + 1 exists.

A mark-sub-phase change flag is consumed by the next pass of the cell's color (Lemma 5.6). Only the mark sub-phase of P(B, p) could leave flags, and it marks from relevant black wins of value p, of which there are none.

A pass at p + 1 acts only on triggers (Lemma 10.12). Relevant wins of value p + 1 or p, relevant losses of value p + 1 and flagged intermediates do not exist. A due intermediate would have bound p + 1 > M, which Theorem 5.4 excludes. The cursed-ply-1 promotion and re-check do not occur as p + 1 > 1. So neither pass writes, and as p + 1 > M the stopping rule ends the loop. ∎

We now compare the runs. Let the interrupt occur after the pass pair at ply p of loop L; we argue by cases on L and p.

Suppose first that L had not settled at p. Then the resumed run follows the uninterrupted path. This is immediate below ply 100, and above ply 100 in the continuation. At ply 100, an unsettled CLEAN loop leads both runs to the same next loop with the same pending flag, and an unsettled first cursed loop or continuation leads both to the continuation at ply 101.

Suppose L settled at p < 100 and is the CLEAN loop or the first cursed loop after a settled CLEAN loop. The resumed run adds a pair in L at p + 1, which writes nothing (Lemma 10.13). So L settles again and the runs continue identically.

Suppose L is the first cursed loop and settled at p < 100 while the CLEAN loop did not settle. The uninterrupted run enters the continuation at ply 100, and the resumed run adds the pair at p + 1 in L. If p ≤ 98, that pair writes nothing (Lemma 10.13), so L settles at p + 1 ≤ 99 and the resumed run also enters the continuation at ply 100. If p = 99, the resumed pair at ply 100 is the first pair of the uninterrupted continuation, a CURSED pair at ply 100 on the same state. If it writes, or 100 ≤ M, both runs go on to ply 101. Otherwise the uninterrupted run ends, and the resumed run adds the pair at ply 101; by the Claim with p = 100, that pair writes nothing and ends the run.

Suppose L is the continuation and settled at p ≥ 100. The resumed run adds the pair at p + 1, which by the Claim writes nothing and ends the run. This case includes the checkpoint (CURSED, false, 100) of a continuation begun at ply 100 after an unsettled CLEAN loop.

In the remaining cases L settled at ply 100. A first cursed loop settling at 100 after an unsettled CLEAN loop leads both runs to the continuation at ply 101. Otherwise the lost finished flag changes the branch, and f = true in each of the following cases.

- From (CLEAN, pc₀, 100) with pc₀ false, the resumed run enters the continuation at ply 101 instead of returning. That pair writes nothing and ends the run (Lemma 10.15 with p = 100).
- With pc₀ true, the resumed run runs the first cursed loop unchanged but reads f as false. If that loop does not settle, both runs reach the continuation at ply 101. If it settles at q, the resumed run continues from ply max(q, 99) + 1 instead of returning; that pair writes nothing and ends the run (Lemma 10.15).
- A first cursed loop settling at ply 100 after a settled CLEAN loop writes (CURSED, false, 100). From it the resumed run enters the continuation at ply 101 instead of returning, and Lemma 10.15 again applies.

It remains to treat interruptions inside added pairs and the DTM and DTM50 generators, and to show that loads trust only files of the same run. An interruption inside an added pair, or inside the first cursed loop of the pc₀-true case, resumes on the same path. The result that covered that pair (Lemma 10.13, Lemma 10.15 or the Claim) then applies to the next pair. The DTM generator stops only after its break test, so it loses nothing. The DTM50 generator tests for an interrupt only at the start of each layer, before any dispatch of that layer, with initialization counted as layer 99, and records that layer. At that point the layers above it are complete and flushed and no cell of the recorded layer has been written; in particular, when the recorded layer is 0, the layer-0 cache still holds INV. The resumed run builds the recorded layer from the same inputs. Its all-ones frontier adds only scans of chunks that hold no legal in-pair cell, which write nothing: a layer pass clears a chunk exactly when the chunk holds no such cell, and legality is fixed by layer 0 once initialization has run.

The checkpoint is removed before any mutation, so one exists only for a state flushed before it was written. A fresh start deletes every spill file before any write, also for DTM50 and DTC. Hence every file a load trusts comes from the same run, and every new DTM50 group is zero-initialized as Lemma 10.8 requires. ∎

**Theorem 10.18 (resume, DTC).** Under the hypotheses of Theorem 10.17, a resumed DTC run writes the same tables as an uninterrupted run.

*Proof.* The checkpoint records batch, layer, fusion, last finished ply and the layer-wide maximum M\_L, with the full layer-alias table and the effective paging budget, checked as in Theorem 10.17. Skipping initialization and the saturation test for the interrupted layer is sound: the uninterrupted run had initialized every fusion of the layer, whose cells are in the spill files, and found the layer unsaturated. The remaining fusions iterate with M\_L instead of their own M ≤ M\_L, and the interrupted fusion restarts after its last finished ply. The loop continues while a pass reports activity or the ply is at most the threshold. Hence the resumed loop stops no earlier than the uninterrupted one, which stops at some q > M with no activity, or after its last ply. By Lemma 10.14 every extra resumed pass writes nothing, including the one after a ply at which the uninterrupted run had finished; this covers the unrecorded finished flag. The all-ones frontier is safe by Lemma 10.12, since a DTC pass has triggers of the same form as a DTZ pass. So both runs execute the same writing passes on the same cells. ∎

**Lemma 10.19 (interrupts).** No table is saved from an interrupted generation, and a save that has begun completes.

*Proof.* A generator tests only hard interrupt requests. On one it flushes, writes the checkpoint and propagates the interrupt. The interrupt is not handled as an ordinary failure, so the main program exits before the save. The save runs only after the generator returns normally, after its last batch. Soft requests are tested only between materials, and no request is tested during a save. ∎

### 10.5 Deterministic output

**Theorem 10.20.** The bytes of every table file are independent of thread count and scheduling.

*Proof.* Final cells are deterministic by Theorem 9.8 and Section 10.2. Each block is a pure function of the cells and the permutation and is compressed independently. The permutation search is exhaustive with ties to the lowest index, dictionary samples come from fixed block ids, histograms are integer sums, and rank ties break by value. File offsets are a sequential prefix sum of compressed sizes, concurrent writes go to disjoint ranges, and the checksum is computed sequentially in block order. ∎

**Lemma 10.21 (options).** The command-line options determine which materials and metrics are built, which directories are searched, the resources used and which sub-table reader is used. They change no written value.

*Proof.* Material names not denoting a constructible material are rejected, and duplicates are removed by key after canonical renaming (Lemma 4.13). Every combination of enabled metrics reads only complete, exact inputs (Theorem 4.17). Search-directory options only append read-only directories (Lemma 4.16). The thread count is at least 1 and does not affect output (Theorem 10.20). The block-cache budget does not change what a read returns (Lemma 10.11), and the resident-memory budget affects residency (Sections 10.1 and 10.2) and, through the effective paging budget, the partition of each batch into fusions (Theorem 10.17). The exactness results (Theorems 5.17, 6.12, 7.2 and 8.3) hold for every such partition, and the DTC saturation decision is taken over every fusion of a batch (Theorem 8.14), so no written value depends on the partition. The two sub-table readers, through the prober and flat, agree at every consulted cell (Lemma 11.17, Theorem 11.10). The information, enumeration and estimation options return before any table is written. ∎

## 11. Lossless encoding

Every cell the reader consults is recovered exactly, except that an even cursed DTZ value in the 1-byte tier decodes one ply short, with its class preserved. Notation: eb ∈ {1, 2} is the rank width in bytes, D = 0xFFFF is the layered "no rank" sentinel, and 0x7FF is the ILLEGAL value code.

### 11.1 Don't-care filling

**Lemma 11.1 (projection round trip).** For every final DTZ entry e, decoding the storage code of e returns the WDL class of e. ∎

**Lemma 11.2 (don't-cares collapse).** The storage value is 0x7FF exactly for DRAW, ILLEGAL and, in loss-only mode, win-class entries (WIN and CURSED\_WIN). Every other stored value is at most 0x7FE.

*Proof.* ILLEGAL has neither the WIN nor the LOSS flag, so it reads as a draw. Decisive values are at most 0x7FE: 1-byte clean values are at most 100, halved cursed values at most 0x3FF, two-byte DTZ values are raw and at most 2045 (RANGE), DTM and DTM50 store v >> 1 ≤ 0x3FF, and DTC values are at most 100 or a DTZ. ∎

**Theorem 11.3 (rank-stream filling).** Let w be one block's storage values and w′ the result of rank-stream filling. Then:

- (i) w′ᵢ = wᵢ for every wᵢ ≠ 0x7FF;
- (ii) if filling succeeds, no w′ᵢ equals 0x7FF;
- (iii) every w′ᵢ is one of the block's original non-don't-care values, so it has a rank.

Filling fails exactly when the whole block is don't-care.

*Proof.* Invariant: no position below the scan start holds 0x7FF. Each step fills a maximal 0x7FF run from a neighbor that is not 0x7FF (on the right by maximality, on the left by the invariant). Writes stay in the run and the scan end grows strictly, so the scan terminates. In the 1-byte tier the values are compacted in place: step i reads bytes 2i, 2i+1 and writes byte i ≤ 2i, so no unread input is overwritten. ∎

**Theorem 11.4 (WDL filling).** Classify each cell as FREE (ILLEGAL), CAPPED (relaxed with a cap) or FIXED. For a CAPPED cell i, let the admissible set be A(i) = {s ≠ ILLEGAL : rank(s) ≤ rank(capᵢ)}. After WDL filling, every FIXED cell is unchanged and every CAPPED cell lies in A(i). Here rank(s) is the class rank of the decoded storage code s, so boundary codes enter the stitch like any other code. BOUNDARY\_LOSS is admissible in every CAPPED cell and BOUNDARY\_WIN in every CAPPED cell with cap WIN, and the reader recovers the exact code in both cases (Theorem 12.31(b)).

Filling fails exactly when every cell of the block is FREE; a block whose cells are all FREE or CAPPED, with at least one CAPPED, is filled.

*Proof.* An all-FREE block is rejected before any pass runs. Otherwise the stitch pass fills each maximal wild run from a FIXED neighbor (DRAW if the whole block is wild), writing the fill where the admissibility test accepts it and elsewhere the cap, which lies in A(i). The relz pass copies a byte only after the test checks every nibble (FIXED equals its original, CAPPED lies in A(i)). For destination p + len it reads src + (len mod (p − src)) < p, which the current copy has not written and the test has already checked, so an overlapping copy repeats its period as an LZ4 match does. Both candidates satisfy the invariant. The relz result replaces the stitch result only when its trial LZ4 compression is strictly smaller. ∎

Only the relaxable codes BLESSED\_LOSS, DRAW, CURSED\_WIN and WIN have caps, so LOSE and boundary cells are FIXED. Every assignment in A(i) reads back correctly (Theorem 12.31), so membership in A(i) is all the relaxed reader needs (Section 12.4).

**Lemma 11.5 (padding nibble).** When T is odd, the high nibble of the last byte of a WDL frame holds LOSE, is FIXED, and is never read.

*Proof.* The packed bytes are zeroed and only indices below T are written, so the nibble holds code 0 (LOSE) and no position maps to it; being neither ILLEGAL nor capped, it is FIXED and kept by Theorem 11.4. It has only two effects: an all-ILLEGAL last block is stored rather than skipped, and the nibble may fill a trailing wild run, which is admissible because LOSE has the lowest class rank. ∎

### 11.2 Rank tables

**Theorem 11.6.** The rank table restricted to \[0, \|S\|) is a bijection onto the set S of values with non-zero count, and the inverse table is its inverse on S. The writer and reader agree on the table and on eb.

*Proof.* Bins are distinct values in a strict total order (count descending, then value ascending), ranked by position. The histogram counts, by the encoder's storage value, exactly the cells that are neither draws nor, in loss-only mode, wins. These are the cells whose storage value is not 0x7FF (Lemma 11.2), so every lookup has a rank. Each cell counts once. Every block is dispatched once (compare-and-swap over disjoint consecutive ranges, or fetch-and-increment). Only the compressed source feeds the histogram, not the dictionary or permutation-search passes. Blocks count before preparation, so skipped blocks count too. The storage permutation is a bijection (Theorem 11.29), and a singular WDL frame counts in one pass. DTM values v >> 1 ≤ 1023 fit the 2048 bins. The one-byte tier is chosen exactly when its table has at most 256 entries, so storing each rank in one byte is lossless. This eb is recorded in the header and used by the helper and the reader. Layered tables, ordered by the number of containing layers (descending) and then by value, are gathered with the encoder's own predicates (every cell for DTM50, every real layer for DTC). The encoder therefore never meets a value outside them; such a value would read as the DRAW sentinel. The 32-bit header word key << 2 \| color count keeps the key's low 30 bits, and readers compare the key masked to them. ∎

**Lemma 11.7 (reused rank tables).** When transcribe re-encodes a DTZ or DTM payload frame in a run that is neither loss-only nor relaxed, it reuses the source frame's rank table R and eb without a histogram. Every storage value it encodes has a rank in R that fits in eb bytes.

*Proof.* Singular and dropped frames are handled separately, and such a run refuses loss-only and relaxed sources. Hence R and eb are the generator's for a full payload frame, and R holds every decisive storage value (Theorem 11.6). A legal DRAW cell is read without a rank, and its 0x7FF is replaced by rank-stream filling (Theorem 11.3). A decisive cell lies in no skipped block (Theorem 11.24), so it reads as some s ∈ R, and re-encoding returns s: two-byte DTZ decodes to s; one-byte cursed or blessed decodes to 2s − 1 with s ≥ 1 (Theorem 11.9), keeps its class and re-stores as ⌈(2s − 1)/2⌉ = s; one-byte clean values are at most 100 and raw; DTM decodes to 2s + 1 or 2s and re-stores as s (Theorem 11.10). The inverse rebuilt from R inverts it. The source chose eb = 1 only when \|R\| ≤ 256, and \|R\| ≤ 2047 always, so ranks fit in eb bytes. ∎

**Corollary 11.8 (singular frames).** A singular frame always uses eb = 1. Indeed, one-byte stored values are the image of (raw value, cursedness), so \|table₁\| ≤ 2·\|table₂\|. The two-byte tier needs \|table₁\| > 256, hence \|table₂\| ≥ 129 > 1. The writer makes a frame singular only when its one value is at most 255, and stores that value in one byte. Hence every singular distance frame decodes as the one-byte tier (Theorem 11.9) and round-trips its stored value exactly.

An empty rank table gives a singular frame with value 0. Such a frame has no decisive cell or, in loss-only mode, only win cells, which are never read (Theorem 12.21).

A layered frame with no decisive value in any layer is singular, and its reader, called only for decisive classes, is never called. Conversely, a layered frame is singular only when its rank table is empty. The generators and loss-only transcription write a singular layered frame exactly when the gathered table is empty, and transcription otherwise copies a singular source frame. So a singular layered frame holds no decisive value in any layer and, in loss-only mode, no loss-class value. ∎

### 11.3 Halving

**Theorem 11.9 (DTZ).** Let v be a decisive DTZ value with class w.

- If eb = 2, or w is WIN or LOSE, then v is stored raw and decoded exactly.
- If eb = 1 and w is CURSED\_WIN or BLESSED\_LOSS, the stored value is s = ⌈v/2⌉ and the decoded value is v̂ = 2s − 1. So v̂ = v when v is odd, and v̂ = v − 1 when v is even.
- The class is always preserved: v̂ > 100 exactly when v > 100, and a cursed cell with v ≤ 100 keeps its cursed tag.

*Proof.* The class comes from the WDL read, so it is exact. For v = 2m + 1, s = m + 1 and v̂ = v; for v = 2m, s = m and v̂ = v − 1. Both v = 101 and v = 102 give v̂ = 101 > 100. The formula needs s ≥ 1, and v = 0 would be a cursed checkmate, impossible because a mated position is LOSE at every clock (Lemma 2.4). ∎

**Theorem 11.10 (DTM and DTM50).** Both store s = v >> 1. A win decodes to 2s + 1 and a loss to 2s, and this is exactly v.

*Proof.* It suffices that win distances are odd and loss distances even, since v = 2(v >> 1) + \[v is a win\]. For DTM this is Theorem 6.13. A DTM50 layer h ∈ \[0, 99\] holds V\_h(x) = val(x, h) (Theorem 7.2). Induct on n over all positions y, with or without an en-passant right, and clocks h where val(y, h) is defined. A loss(0) is checkmate, also at the boundary clock 100, whose value is loss(0) or DRAW. For n ≥ 1 and h ≤ 99, by Lemma 7.1 extended to en-passant positions (Section 2.1), win(n) is 1 + the least child loss and loss(n) is 1 + the greatest child win. The child value is val(y·m, 0) after a zeroing move m and val(y·m, h + 1) otherwise. The child, possibly in another material (capture, promotion, exit) or with the opponent's en-passant right after a double push, has distance n − 1, so parity follows by induction. The decoder takes win or loss from the WDL class, which for a decisive DTM50 layer is the class of DTM (Theorem 7.7), the outcome in Γ∞. ∎

### 11.4 The change-point codec

Fix a layered column c\[0..L−1\] with L = 101 (DTM50) or 31 (DTC). Its hypothesis is:

- **(M)** Once some layer has rank D, every later layer does too.

(M) holds by Theorem 7.7 for DTM50 and by Lemma 8.8 and Corollary 8.10 (Theorem 8.15) for DTC. The encoder stops at the first D. DRAW has no rank (its storage value 0x7FF is not in the rank table), so a column ending in DRAW is marked by the draw-end bit.

**Lemma 11.11 (extraction).** The encoder's scan produces change points 0 = cp₀ < … < cp\_{k−1} ≤ L − 1 and ranks cr\_j = rank(c\[cp\_j\]). The rank is constant on each segment \[cp\_j, cp\_{j+1}), and only the last rank can be D. Define de ("ends in DRAW") as cr\_{k−1} = D. Then k ≤ L ≤ 127 and every change point is h ≤ 100 < 128, so bit 7 of every h byte and of the k byte is free for the draw-end flag. ∎

**Lemma 11.12 (record widths).** The encoder writes, and both readers assume, the same record widths:

| State | Condition | Width, de = 0 | Width, de = 1 |
| --- | --- | --- | --- |
| CONST | k = 1 | eb | — |
| SINGLE | k = 2 | 1 + 2·eb | 1 + eb |
| DOUBLE | k = 3 | 2 + 3·eb | 2 + 2·eb |
| MULTI | k ≥ 4 | 1 + B + k·eb | 1 + B + (k − 1)·eb |

Here B, the bitmap size in bytes for L layers, is 16 for DTM50 and 4 for DTC; the writer and the prober compute it by identical definitions from the same L. The block reader and the sequential column decoder recompute the same payload layout (header, state bits, streams, 4-byte alignment, MULTI directory and MULTI stream). ∎

**Lemma 11.13 (random access).** Records of each state are appended in increasing position order. For a position p, the stride-256 snapshot plus a popcount over at most 256 two-bit states gives idx(p), the number of earlier positions with the same state; the snapshot plus a hint-bit popcount gives n\_short, the number of short (draw-end) records among the first idx SINGLE records. The hint bitmaps are not stored: the loader walks each stream with the widths of Lemma 11.12, so by induction on j it is at the start of record j and hint bit j is that record's draw-end bit. The byte offset of the SINGLE record is

off = n\_short·(1 + eb) + (idx − n\_short)·(1 + 2eb),

the sum of the preceding records' widths. DOUBLE records use widths 2 + 2eb and 2 + 3eb in the same formula, MULTI records the prefix-sum directory, and CONST records sit at idx·eb. ∎

**Lemma 11.14 (MULTI slot).** For a MULTI record at layer ℓ, the popcount of bitmap bits at or below ℓ, minus one, is the j with cp\_j ≤ ℓ < cp\_{j+1}; bit 0 is always set, so j ≥ 0. ∎

**Theorem 11.15 (codec correctness).** Assume (M) and rank(c\[0\]) ≠ D. For every layer j with rank(c\[j\]) ≠ D, every decoder returns c\[j\]; at the other layers the prober returns the DRAW sentinel.

*Proof.* Four decoders read the format of Lemma 11.12: the prober's, the generator's sequential column decoder and flat layer-1 reader for DTM50 sub-tables, and transcribe's source reader. The prober and transcribe locate records by Lemma 11.13, hint rebuild included; the other two walk each stream in record order, so by induction their cursor starts each record. The flat reader needs only layer 1, selected by h₁ (SINGLE, DOUBLE) or bit 1 (MULTI), never the omitted last rank since k ≥ 4. The draw-end flag (the hint bit, bit 7 of the h byte, or bit 7 of the MULTI k byte) equals de. Each rule returns c\[j\] wherever rank(c\[j\]) ≠ D, and the prober's returns DRAW elsewhere:

- **CONST:** return cr₀.
- **SINGLE:** return r₀ below h; at or above h, DRAW if de, else r₁.
- **DOUBLE:** the same, with thresholds h₁ and h₂.
- **MULTI:** the slot read is the one Lemma 11.14 gives, that is, the index j, counting set bits in bit order from 0, of the last set bit of the bitmap at or below the layer, and the omitted last rank is exactly the de case. ∎

The hypothesis rank(c\[0\]) ≠ D holds wherever a reader decodes: DTM50 and DTC reads take the class from WDL and return DRAW or ILLEGAL before decoding (Lemma 12.16).

**Lemma 11.16 (stale DTM50 rows).** In a DTM50 block, let a column have head row 0x7FF (its cell is ILLEGAL at layer 0 or its DTM is a draw). Only its head and layer-0 rows are written for the current block; its other rows keep stale values from an earlier block in the reused buffer. The encoder output nevertheless depends only on the written rows, and it is exact.

*Proof.* The encoder reads the head row first. 0x7FF has no rank, since the table holds only decisive storage values, at most 0x3FF (Lemma 11.2, Theorem 11.10). For such a head the encoder writes a CONST record holding the final rank of the nearest earlier column in the block whose record does not end in DRAW, or rank 0 if there is none, and reads no other row. That rank is a don't-care but always a valid index into the table, which is non-empty, since a layered frame with an empty table is singular and has no blocks (Corollary 11.8). The column is an ILLEGAL or DRAW cell, whose value every reader discards by class (the remark above), or, in loss-only transcription, an unpriced win cell, which is never read (Theorem 12.21). Every other column is fully written: its head is fixed by the layer-0 write, which comes first, and each later layer is written for every column whose head is not 0x7FF. So every value the encoder reads is either a decisive storage value, which has a rank because the table is gathered with the encoder's own predicates from every cell of every layer (Theorem 11.6), or the 0x7FF of a DRAW layer below a decisive head, which it reads as D, the draw-end marking of Theorem 11.15. The same holds for the generator's DTC blocks, whose lower layers are written only for columns with a decisive head, and for transcribe's layered blocks, which write only the head of an unpriced column and a priced column only through its first DRAW layer. ∎

**Lemma 11.17 (flat readers at DRAW layers).** The generator's flat reader for DTM50 sub-tables returns V\_0(x) at every legal cell x, including cells whose packed layer 1 has rank D. Its flat reader for DTM sub-tables returns DRAW throughout a skipped block, every legal cell of which is a DRAW.

*Proof.* The flat DTM50 reader takes the class from the WDL file, decodes the rank it read only for WIN and LOSE, and otherwise returns DRAW. Its stream cursors advance by the widths of Lemma 11.12 regardless, so the walk stays aligned as in Theorem 11.15. By the class of x:

**DRAW.** V\_0(x) = D by Theorem 7.12(a).

**CURSED\_WIN or BLESSED\_LOSS.** V\_0(x) = D by Theorem 7.12(c). The column is DTM then DRAW from packed layer 1 on (Theorem 7.9), a SINGLE record with h = 1 and the draw-end flag, so no rank is read at layer 1.

**WIN or LOSE.** The first DRAW packed layer is 102 − z ≥ 2 or absent (Theorem 7.9), so layer 1 is not D, and Theorem 11.15 gives c\[1\], which decodes to V\_0(x) by Theorem 11.10.

A skipped block is filled with DRAW. By Theorem 11.24, no legal cell of a skipped layered block has a decisive DTM row. Cursed and blessed cells are decisive in Γ∞, so all its legal cells are DRAWs, with V\_0 = D. A skipped DTM block is entirely don't-care, and the don't-cares of a full-format DTM frame are DRAW and ILLEGAL (Lemma 11.2). ∎

### 11.5 Offsets and the skip sentinel

**Theorem 11.18.** For non-decreasing offsets O\[0..n\], the offset vector returns get(i) = O\[i\] and get2(i) = (O\[i\], O\[i+1\]).

*Proof.* Samples use width bitwidth(O\[n\]) and deltas, non-negative since O is monotone, width bitwidth(max delta), in disjoint regions whose starts the reader recomputes identically. A value v of width w at bit offset b is written by ORing v << b into a zeroed buffer, spilling into the next 64-bit word when b + w > 64, and reading reverses this. Every section is followed by at least 8 readable bytes (later sections, padding, data, the 8-byte checksum), covering the reader's over-read. ∎

**Lemma 11.19 (header layout).** For every table file, the writer and the reader use the same fields in the same order and at the same widths, and the writer's predicted file size equals the number of bytes it writes.

*Proof.* Writer, readers and shrinking tool emit and parse the same sequence: magic, key word (Theorem 11.6), one frame header per color (singular, dropped or payload, a DTZ, DTM or layered payload header ending in its rank table), WDL dictionaries, offset sections (each followed, in a layered file, by the block's uncompressed sizes). A singular layered frame stores 0, which the layered readers accept. The predicted size sums each write's bytes and rounds up to a multiple of 64 exactly where the writer pads. Every value fits its field: a WDL tail is below its block size, which is at most 2¹⁶, so it fits its 2-byte field, while the block size has a 4-byte field; DTZ, DTM and layered tails have 4-byte fields; a dictionary at most 32 KiB, a rank count at most 2048 (stored values are below 0x800), a singular value at most 255 (Corollary 11.8). ∎

**Lemma 11.20 (data placement).** For every payload color with offset vector O and every block k, the bytes of block k occupy the file offsets \[c₀ + O\[k\], c₀ + O\[k + 1\]), where c₀ is the start of the color's data region as the reader computes it.

*Proof.* Stored offsets are prefix sums of compressed block sizes, and the writer places block k at its region base plus that sum. Writer, reader and shrinking tool compute the same bases: from the header end of Lemma 11.19, each rounds up to a multiple of 64 before a color's region and advances by its compressed total after it. ∎

**Lemma 11.21 (checksum).** The checksum that the writer appends equals the XXH64 hash of all preceding bytes of the file, which is what the reader verifies, and the file size is ≡ 8 (mod 64).

*Proof.* Header fields and padding are hashed as written. Block data, written concurrently into the disjoint ranges of Lemma 11.20, are hashed serially in block order after all block writes finish, so streaming XXH64 sees the file's bytes in increasing order. The last write pads to a multiple of 64, and the 8-byte digest follows. The shrinking tool writes and hashes sequentially. ∎

**Lemma 11.22 (block sizes in transcribe).** For every block size that transcribe admits, its WDL, DTZ and DTM files satisfy the field widths and offset bounds the readers assume. A WDL block size that the dictionary builder cannot use stops the run before the file is written.

*Proof.* A WDL block holds at most 2¹⁶ bytes, so its tail fits the 2-byte field (Lemma 11.19) and the checkpoints cover it (Lemma 11.26). The dictionary builder checks that the sampled bytes are a multiple of 4096 before the WDL file is written. A DTZ or DTM block size is a multiple of 1024 below 2³², so eb divides it and it fits the 4-byte fields; readers hold sizes, positions and offsets at 64 bits. ∎

**Lemma 11.23 (layered block sizes).** A DTM50 or DTC block of at most 2²⁴ bytes has a decoded payload, and a cached block with its prefix and hint tables, of fewer than 2³² bytes, so every 32-bit count and offset that the writer and both readers keep for it is exact.

*Proof.* A block of b ≤ 2²⁴ bytes holds n = b / eb positions. Let L be the layer count (101 for DTM50, 31 for DTC) and B the bitmap size (16 and 4). We bound the payload first and then the cached block.

Each position costs 2 state bits and one record, whose width is:

- CONST: eb bytes;
- SINGLE: at most 1 + 2eb bytes;
- DOUBLE: at most 2 + 3eb bytes;
- MULTI: at most 1 + B + L·eb bytes, together with a 4-byte directory entry.

The payload adds the 24-byte header, one directory sentinel and at most 6 alignment bytes. So a DTM50 payload is at most 34 + n(21.25 + 101·eb) bytes. This is below 2³¹ for eb = 1 (n ≤ 2²⁴) and for eb = 2 (n ≤ 2²³). A DTC payload is smaller.

The readers prepend a 36-byte block descriptor and append a 24-byte prefix entry per 256 positions and one hint bit per SINGLE and per DOUBLE record, under n/2 + 64 bytes in all. So the cached block is below 2³¹ + 2²³.

Every count is at most n and every offset below the cached size. The generators' layered block sizes and the layered block sizes transcribe admits are at most 2²⁴. ∎

**Theorem 11.24 (skip sentinel).** get2(k)\[0\] = get2(k)\[1\] iff block k is entirely don't-care: for a WDL block, every cell is ILLEGAL; for a DTZ or DTM block, every storage value is 0x7FF (Lemma 11.2); for a layered block, as in the proof.

*Proof.* LZ4 output is at least 1 byte and LZMA output at least 5, so the offsets are equal exactly when the source block is empty or its preparation fails, that is, for an all-don't-care block (Theorem 11.3 for DTZ and DTM, Theorem 11.4 for WDL). A WDL block of FREE and CAPPED cells with at least one CAPPED is stored with its stitch fill, and when T is odd the last WDL block holds the FIXED padding nibble (Lemma 11.5), so it is never skipped. A layered block is skipped exactly when no cell has a decisive head row (the DTM row for DTM50, the DTZ row for DTC), in loss-only mode no cell of a loss class. Its readers are called only for decisive classes, and in loss-only mode never for win cells (Theorem 12.21), whose head row is decisive (Theorem 7.7 for DTM, Theorem 5.17 for DTZ), so no queried cell lies in a skipped layered block. ∎

### 11.6 LZ4 point reads

Model a valid LZ4 block as records S\_t. Each record has literals, then a match with offset off\_t ≥ 1 and length at least 4, except the last record, which has literals only. The virtual output is out\[−\|dict\| .. U − 1\], and a match satisfies out\[q\] = out\[q − off\_t\].

**Lemma 11.25 (checkpoints).** For each stride c, the index stores the input offset and output start a\_t of the first record whose output end exceeds 256c. So a\_t ≤ 256c. ∎

**Lemma 11.26 (checkpoint coverage).** Every checkpoint slot c < ⌈U/256⌉ is written, and every position 0 ≤ q < U selects such a slot.

*Proof.* After the last record the output position is U, so every slot c with 256c < U is written, and ⌊q/256⌋ ≤ ⌊(U − 1)/256⌋ < ⌈U/256⌉. ∎

**Lemma 11.27 (period folding).** Let q lie inside a match, with into = q − (a\_t + lit\_t). Then out\[q\] = out\[a\_t + lit\_t + (into mod off) − off\], and this position is strictly below the match start. The read's step, q − (⌊into/off⌋ + 1)·off when into ≥ off, computes exactly this. ∎

**Theorem 11.28.** For 0 ≤ pos < U, the point read returns the byte a full decompression would produce at pos, and it terminates.

*Proof.* Invariant: out\[target\] = out\[pos\]. A literal target is returned, a negative one reads the dictionary, and a match target moves to the strictly smaller position of Lemma 11.27; the target is bounded below by −\|dict\|, so the walk terminates. Each block is compressed after reloading exactly the stored dictionary (at most 32 KiB, within LZ4's window), and no reference crosses a block boundary. A failed dictionary training stores size 0, and compressing with an empty dictionary equals compressing with none. ∎

### 11.7 Storage permutations

**Theorem 11.29.** For every valid Lehmer code perm < n!, the storage permutation and its inverse map are mutually inverse bijections on \[0, T) that keep the slice digits fixed, so the save phase can pin groups by storage range and then read logical indices, which lie in the same slice and hence in a pinned group. The reader's layout equals the writer's map, so reader slot s holds the entry of the position it indexes.

*Proof.* Factorial-base decoding picks from the remaining classes at each step, yielding a permutation σ. The mixed-radix codes L(d) (native order) and S\_σ(d) (σ order) over the same within digits are bijections onto \[0, S), where S is the within size; the storage permutation and its inverse are L∘S\_σ⁻¹ and S\_σ∘L⁻¹. In S\_σ∘L⁻¹, L⁻¹ divides by L's weights from the highest class down; only the first class in native order has weight 1, and its digit is the remainder left after the others, taken without division, and every other weight is a product of radices above 1. In L∘S\_σ⁻¹, S\_σ⁻¹ takes the digits from the lowest class in σ order up, each as the remainder of the running quotient divided by that class's radix; a radix above 1 is divided by Lemma 3.4, and a radix-1 digit is 0 (Lemma 11.30). The writer stores at slot s the entry of logical index L∘S\_σ⁻¹(s) plus the slice offset, and the reader computes s = slice offset + S\_σ(d) (Theorem 3.15), so it reads L∘S\_σ⁻¹(S\_σ(d)) = L(d), the generator's index of that position. ∎

**Lemma 11.30 (degenerate radices).** A storage digit of radix 1 is 0, and the odometer that builds the orbit weight table has at least one digit.

*Proof.* A radix-1 digit is set to 0 without dividing. The odometer serves only pawnless materials without castling rights, and bare kings are never generated, so such a material has a populated class of a man that is neither king nor pawn. ∎

**Lemma 11.31 (layered permutation).** The permutation code in a DTM50 or DTC frame header is the code its blocks were packed with, and it is valid for the material.

*Proof.* The header code built the packing permutation and is copied from a frame of the same material: the same-color DTM (for DTM50) or DTZ (for DTC) frame, the source layered frame, or, in loss-only transcription, the frame just written. Each such code was validated on reading as a Lehmer code below n! over the same populated classes, produced as one, or is 0 when there is no permutation; the reader rebuilds the layout from it, so Theorem 11.29 applies. ∎

**Lemma 11.32 (one position per column).** In every DTM50 and DTC block, all rows of a column, and the WDL class and DTZ value read for it, belong to one position, although a material's metrics may use different storage permutations. In every file of the material, the prober reads the column of the queried position.

*Proof.* A DTM50 block covers storage indices p₀ + k under the permutation P of the same-color DTM frame's code, which is the DTM50 header code (Lemma 11.31). Column k reads every DTM50 layer, and the DTM file, at ℓ\_k = P⁻¹(p₀ + k); the DTM reader maps ℓ\_k by the same code. So the head row and rows 1 to 100 belong to ℓ\_k. The WDL class decoding the head is also read at ℓ\_k, through the WDL frame's own permutation.

DTC is alike, with P the permutation of the DTZ frame's code. All 31 rows of column k and the DTZ read for row 0 use ℓ\_k. The layer-map row selecting the stored layers is that of ℓ\_k's pawn-slice batch.

The storage permutation keeps the slice digits, so pinning the block's storage range pins every ℓ\_k. Reader slot s of every file, indexed from that file's own header, holds the entry of the position it indexes (Theorems 3.15 and 11.29). ∎

These proofs require a little-endian host: the layered encoder writes its ranks as little-endian bytes but its header counts and MULTI directory in host order, and the readers use host-order copies. chesstb and transcribe refuse to run on big-endian hosts; shrink and the probe library have the same requirement.

## 12. Reconstruction theorems

Every reduced frame is rebuilt exactly by one ply of minimax over its child tables. The prober returns either the true value or a refusal.

Notation: inv reverses the class order, swapping WIN with LOSE and CURSED\_WIN with BLESSED\_LOSS. For a move m to child y, write κ(m) for the class it offers the mover and δ(m) for its DTZ contribution:

- a zeroing move gives κ = inv(V(y)) with V taken at clock 0, and δ = 1;
- a quiet move or exit gives κ = inv(c(y)) and δ = 1 + d(y), except that an exit into a cursed or blessed child has δ = f(d(y)) (Definition 5.19), and κ is demoted to cursed or blessed when δ > 100.

Then c(x) = max κ, the true 50-move class by Section 2.

### 12.1 Structural lemmas

**Lemma 12.1 (quiet children stay home).** Let T be an asymmetric material and let x have side to move c, which holds no castling right in T. Every quiet child of x lies in T, has the opposite side to move, and is not mirrored.

*Proof.* A quiet move keeps the piece multiset and the pawns, hence every opposing pair. It cannot change the opponent's rights, nor c's, which are empty. Routing depends only on the pieces, the pawns, the rights and which files exist, so it picks the child's file as it picked the parent's. ∎

Routing sends a position with castling rights to the pair-and-castling table if its WDL file exists, and otherwise to the castling table, never to a generic table. A position without rights goes to its pair table when its WDL file exists, and otherwise to the generic table, which indexes every placement of the material, including those with opposing pairs (Lemma 4.9).

**Lemma 12.2 (routing is total and canonical).** A position with castling rights always has a castling table: the castling-only configuration of its material, with the rooks that hold rights stripped, is defined. For every configuration that routing selects, the position's own key, formed by stripping the same men and adding the opposing pair and the rights in the position's orientation, is one of the configuration's two keys. So the position is indexed either as it stands or through the color mirror.

*Proof.* The castling-only configuration is undefined only when no rook holds a castling right. A castling right is held by a rook on its square, so the configuration is defined. The pair-and-castling configuration is undefined only when the position has no canonical opposing pair. A configuration sorts its men by color and swaps the rights exactly when it swaps the colors, so its two keys are those of the two color orientations of the stripped men with the pair and the rights, and it holds rights exactly when the position does. The position's own key strips the same men (one pawn of each color for a pair, exactly the rooks holding rights) and adds the same pair and rights in its own orientation, so it is one of the two keys. ∎

**Lemma 12.3 (table lookup).** Every table the prober uses for a material is the file of that material that comes first in the current search order. A material recorded as missing yields only a refusal or an exact fallback.

*Proof.* Tables are keyed by the material's minimal key, and the configuration opened under a key is that key's canonical configuration, so the file is indexed with the requested material's configuration. Files are found by name, with exact case, in the order the directories were added. A file whose header key differs from the material's is rejected. That key counts only men, so castling and pair tables are told apart from the generic table of the same men by their file names, which differ only in case.

A table is stored only after its file is loaded and checked, so no partly loaded table is used. Directories are only appended, never reordered or removed, so a file that was the first match when opened stays the first match.

Each thread's private entries carry the generation of the table set and are used only under the current generation. Generations come from one global counter starting at 1, so they are distinct across prober instances and never equal the empty tag 0. Invalidation clears the shared tables and advances the generation under their lock, and a file loaded across a generation change is discarded.

A material recorded as missing that has since gained a file in a new directory yields a refusal, routing to the generic or castling table (exact by Lemmas 12.2 and 4.9), or the fallback from DTM50 to DTM or from DTC to DTZ (exact by Theorems 7.12 and 12.39). ∎

**Lemma 12.4.** shrink, and transcribe in its shrinking mode, drop at most one frame, and the other frame is normal or singular. ∎

**Lemma 12.5 (castling protection).** In a WDL table, a dropped frame's side holds no castling right. Both tools mark a side's frame droppable exactly when that side holds no castling right. ∎

**Lemma 12.6 (rights from the file name).** The number of castling rights that shrink reads for each side from the material's file name equals that side's number of rights.

*Proof.* In the name, White's men precede Black's king, which is the second K, since the opposing-pair marker is a lowercase p. Each rook holding a right is a lowercase r in its own side's part. So splitting at the second K separates the sides. ∎

**Lemma 12.7 (shrink preserves retained frames).** Every frame that shrink retains reads, after shrinking, exactly the values it read before, and keeps its flags. shrink drops no frame from a file that has a loss-only frame.

*Proof.* Block offsets are relative to their frame's data section, which every reader locates by aligning to 64 bytes after the headers. shrink copies each retained frame's offset sections, dictionaries, data and flag byte unchanged, and places each data section at the next 64-byte boundary. So every retained offset addresses the same bytes, and the singular, loss-only and relaxed flags survive. The output is a 64-byte-aligned body followed by the 8-byte checksum, which is the length the loaders accept. A loss-only file marks every frame loss-only, a singular one included, and shrink drops nothing from it, as Lemma 12.22 requires. ∎

**Lemma 12.8 (flags of dropped frames).** When shrink or transcribe drops a frame, it writes that frame's flag byte as the dropped flag alone, so the frame's loss-only and relaxed flags are lost. No reader uses them.

*Proof.* Frame location and the table WDL read send a dropped asymmetric frame to derivation before consulting any other flag, and the capture bound and the relaxed DTZ test apply only to stored frames. Neither tool drops a frame of a symmetric material, whose unstored Black frame the loader creates with White's flags (Lemma 12.22). ∎

**Lemma 12.9 (termination).** Every recursive call does one of the following:

1. it stays at the same position and depth, along a fixed acyclic order of functions;
2. it moves to a child, with depth + 1;
3. it moves to a grandchild, with depth + 2, in the en-passant conversion test.

The same-depth calls of case 1 are the metric probes inside the full probe, the table read inside the WDL probe, the dispatch from the table WDL read to WDL derivation or the capture bound, the dispatch from the stored-code read to the capture bound, and the dispatch from a metric probe, such as the DTZ or DTC probe, to its derivation. So the depth of a call is its number of plies from the root. Every derivation and the capture bound refuse at depth ≥ 64, the depth cap. The other recursive calls, the en-passant loops, are captures, which strictly lower the number of men. So every probe terminates. ∎

Call WDL derivation, the capture bound, DTZ derivation, DTM derivation, layered DTM50 derivation, flat DTM50 derivation and DTC derivation the *guarded* functions. A depth refusal occurs exactly when a guarded function is entered at depth ≥ 64.

**Theorem 12.10 (probe depth).** Let D(m) be the longest path from a root with at most m men in the graph G defined in the proof, counted in guarded-entry depth. Every line of nested calls from a root position with at most m men, through any public entry point and any table set the tools can produce, enters guarded functions only at depths up to D(m), and G attains D(m):

| m | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- |
| D(m) | 5 | 8 | 12 | 17 | 21 | 25 |

Since 25 < 64, no probe of a table with at most 8 men is ever refused for depth.

*Proof.* Every line of nested calls is simulated, ply for ply, by a path of a finite graph G that admits every call the prober can make, and more. The longest path of G, evaluated exhaustively, is therefore an upper bound. A path attaining it (Table 12.1) shows that no argument using only the facts G encodes can lower the bound.

*Step 1: plies and roots.* Depth counts plies (Lemma 12.9). Every entry point (the public probe for all metrics, the public WDL probe, and the DTZ, WDL and DTM root rankers) calls the full probe or the WDL probe at depth 0. The WDL probe's calls are a subset of the full probe's, so a root is a full-probe call on any position with at most m men, any clock and any en-passant square.

*Step 2: abstract positions.* A position maps to σ = (side to move, one record per side, ep). Each side's record holds:

- the multiset of its pawns' *lives*: the number of forward steps to promotion, counting the promotion itself, so a pawn on its start rank has life 6 and a pawn on the seventh rank has life 1;
- r, its rooks that hold a castling right;
- n, its other non-king men.

The ep bit records that the last ply was a double push. It is set after every double push, whether or not an en-passant capture is legal. Every legal move maps to one abstract move:

- **R, captures and promotions.** Any capturer (a plain piece, the king, a rook holding a right, or a pawn of life x, which becomes x − 1, or promotes when x = 1) takes any enemy man. A pawn of life 1 promotes.
- **S, single push.** A pawn of life x ≥ 2 becomes x − 1.
- **D, double push.** A pawn of life 6 becomes 4, and the ep bit is set.
- **Q, a quiet move that keeps the table.** It exists only if the side has n > 0 or r = 0.
- **E, a quiet move that gives up castling.** A king move or castling sets r to 0, and a move of a rook holding a right lowers r by 1. The rights removed are added to n.
- **En passant.** It is available only if the ep bit is set, the side to move has a pawn of life 3 and the other side a pawn of life 4. The capturer's life becomes 2.

A child is probed only if it has at least 3 men. The table identity is (pawn count, r, n) per side; S, D and Q keep it, and R and E change it. The abstraction forgets geometry, blocking and legality, so it only adds moves. It covers opposing-pair files too: a pair forms or breaks only by a capture or promotion, never by a push, since a push keeps a pawn on its file and a pawn cannot pass an enemy pawn on the same file. So a pair file is one more file choice at a separator.

*Step 3: frames.* Along a path, each table visit fixes one frame assignment:

- **WDL:** no drop, or a drop of White or of Black. A dropped side holds no right (Lemma 12.5). Retained frames are relaxed.
- **DTZ and DTM:** a drop of White or of Black, or loss-only. The retained DTZ frame is relaxed. A loss-only file drops nothing and is never relaxed.
- **DTM50 and DTC:** the same options, or absent.

The assignment is chosen freely per path and per file, since shrink acts on one file at a time. Every omitted option only removes calls:

- a normal or singular frame reads where a dropped frame would derive;
- an unrelaxed frame skips the capture bound, or, for DTZ, stored-mode DTZ derivation;
- a missing WDL, DTZ or DTM file makes its probe return nothing.

A missing DTM50 or DTC file adds calls, so that option is kept. Without DTM50, the full probe reads DTM. When neither the DTM50 probe nor the DTC probe, read or derived, supplies DTZ, it probes DTZ.

A material whose sides have only pawns (and castling rooks), with equal counts on both sides, is symmetric, and its dropped frames are read through the mirror. Every other material may be taken as asymmetric, which only adds derivations.

*Step 4: classes.* Each node on a path carries its stored class q ∈ {L, BL, D, CW, W}. Only necessary conditions are imposed.

- **Parent and child.** A child's class c satisfies inv(c) ≤ q, or the move is quiet with c = L and q = CW (demotion over 100, Lemma 12.11).
- **Double pushes.** A child reached by a double push is probed in full with its en-passant square. When an en-passant capture is available, its stored class is free: the parent bounds the combined class, not the stored one, and the capture may even be the child's only move. Otherwise its stored class is its class, constrained as any child's.
- **Pruning.** Each derivation keeps its own pruning filter, which only removes calls:
  - DTZ derivation keeps the DTZ lift of inv(c) ≥ q;
  - DTM derivation keeps inv(fold(c)) ≥ fold(q), where fold maps CURSED\_WIN to WIN and BLESSED\_LOSS to LOSE;
  - layered DTM50 derivation keeps inv(c) ≥ q;
  - flat DTM50 derivation keeps inv(fold(c)) ≥ fold(q);
  - DTC win derivation continues only through c = L;
  - DTC cursed derivation continues through quiet children with c ∈ {L, BL} for CW, and c ∈ {W, CW} for BL;
  - a LOSE parent forces its children to WIN without a WDL probe in DTZ derivation, DTM derivation and both DTM50 derivations, and DTM derivation also forces them from BL.

*Step 5: edges.* The edges of G are the calls of Table 12.2, one per call site, including every call a derivation makes. G has no early exit, since a pinned result or a refusal only stops a derivation's loop sooner.

By Steps 2–5, every line of nested calls in the prober is the image of a path in G of the same length, with the frame choices and classes the real files and positions have. So every such line from a root with at most m men enters guarded functions only at depths up to D(m).

*Step 6: evaluation.* G is finite: at most 8 men, lives in 1..6 and r ≤ 2 per side, with the classes, the frame assignments and the function labels. A memoized depth-first search from every root computes each node's largest guarded-entry depth below it and raises an error if a node recurs on its own stack. None does, so G restricted to what the roots reach is acyclic and the computed values are exact longest paths. They are the values tabulated in the theorem. The program is in Appendix A. ∎

**Table 12.1: a path of G attaining D(8) = 25.** The root has White K + pawn (life 6) against Black K + four pawns of life 1 and one of life 6, White to move, class CURSED\_WIN. Each row gives the depth at which a guarded function is entered, and the ply it makes.

| Depth | Guarded function (side, class) | Its ply | Frame that makes it recurse |
| --- | --- | --- | --- |
| 0 | DTM derivation, from the full probe (w, CW) | double push | DTM: loss-only, which cannot price a cursed win; no DTM50 file |
| 1 | DTM derivation, from the full probe (b, CW) | double push | DTM: loss-only |
| 2 | full DTZ derivation, from the full probe (w, BL) | quiet move | no DTC file, so DTZ is probed; DTZ: w dropped |
| 3 | stored-mode DTZ derivation (b, CW) | single push | DTZ: b relaxed |
| 4, 6, 8 | WDL derivation (w) | single push, to the seventh rank at 8 | WDL: w dropped in each table |
| 5, 7, 9 | capture bound (b) | promotion | WDL: b relaxed |
| 10 | WDL derivation (w) | quiet move |  |
| 11 | capture bound (b) | promotion of the last pawn of life 1 |  |
| 12 | capture bound (w) | king captures (the side reducing changes) | WDL: w relaxed |
| 13, 15 | WDL derivation (b) | single push | WDL: b dropped |
| 14, 16 | capture bound (w) | capture |  |
| 17 | WDL derivation (b) | quiet move |  |
| 18 | capture bound (w) | promotion |  |
| 19 | WDL derivation (b) | quiet move |  |
| 20 | capture bound (w) | capture |  |
| 21 | capture bound (b) | capture of the promoted piece (the side reducing changes) | WDL: b relaxed |
| 22, 24 | WDL derivation (w) | quiet move | WDL: w dropped |
| 23 | capture bound (b) | promotion |  |
| 25 | capture bound (b), entered in K v K+Q | none: every capture bares the kings |  |

No en-passant capture is available after either double push. Each entry to the capture bound has a stored code other than WIN, which a capped cell may hold (Lemma 12.29). The first table's dropped WDL frame beside a relaxed DTZ file is what shrink produces from a transcribed file (Step 3).

**Table 12.2: the edges of G.** Each row is one call: the caller, when it makes the call, the function it calls, and the plies the call adds to the depth. A probe of a bare-kings child is answered DRAW without a call.

| Caller | When | Calls | Plies |
| --- | --- | --- | --: |
| full probe | always | table WDL read of the position | 0 |
| full probe | the class is not DRAW and a DTM50 file exists | DTM50 probe, unclocked, and at the probe's clock for a WIN or LOSE class unless a shortcut decides it (Theorems 7.11, 7.12) | 0 |
| full probe | the class is not DRAW, no DTM50 file exists and a DTM file does | DTM probe | 0 |
| full probe | the material has pawns and the class is not DRAW | DTC probe | 0 |
| full probe | the class is not DRAW and neither the DTM50 probe nor the DTC probe, read or derived, supplied DTZ | DTZ probe | 0 |
| full probe | an en-passant square is set | full probe of each en-passant capture's child | 1 |
| WDL probe | always | table WDL read of the position | 0 |
| WDL probe | an en-passant square is set | table WDL read of each en-passant capture's child | 1 |
| table WDL read | the side to move's frame is dropped and the material is asymmetric | WDL derivation | 0 |
| table WDL read | the frame read is relaxed and its stored class is neither WIN nor ILLEGAL (a boundary win counts as WIN) | capture bound | 0 |
| stored-code read | the frame read is relaxed and its stored code is neither WIN nor ILLEGAL | capture bound | 0 |
| WDL derivation | each zeroing child | WDL probe | 1 |
| WDL derivation | each quiet child | stored-code read | 1 |
| capture bound | each capture or promotion | WDL probe | 1 |
| DTZ probe | the frame is unreadable | full DTZ derivation | 0 |
| DTZ probe | the frame is relaxed and the cell's class is a win | stored-mode DTZ derivation | 0 |
| DTZ derivation | each zeroing child, unless the parent is LOSE (every child is then a win) | WDL probe | 1 |
| full DTZ derivation | each quiet child | table WDL read (skipped from LOSE), then DTZ probe | 1 |
| DTM probe | the frame is unreadable | DTM derivation | 0 |
| DTM50 probe | the frame is unreadable | flat DTM50 derivation when unclocked, layered DTM50 derivation at a clock | 0 |
| DTM, flat and layered DTM50 derivation | each double push | full probe of the child | 1 |
| DTM, flat and layered DTM50 derivation | each other child (layered: below clock 100) | table WDL read (skipped from LOSE, and in DTM derivation also from BLESSED\_LOSS), then the child's DTM or DTM50 probe, in any table | 1 |
| DTC probe | the frame is unreadable | DTC win, loss or cursed derivation, by class | 0 |
| DTC win derivation | each child | WDL probe | 1 |
| DTC win derivation | each non-conversion child of class LOSE | full probe for a double push, DTC probe otherwise | 1 |
| DTC loss derivation | each double push | WDL probe of each en-passant capture's child | 2 |
| DTC cursed derivation | each child | WDL probe | 1 |
| DTC cursed derivation | each quiet child of a counted class | DTC probe | 1 |

### 12.2 Dropped WDL frames

**Lemma 12.11 (boundary codes are exactly sufficient).** For a quiet move to y, κ(m) = I(c₀(y)), where c₀ is the stored code and I is its inversion. The clock-free class c(y) alone determines κ except in one case: c(y) ∈ {WIN, LOSE} with d(y) = 100.

*Proof.* A clean LOSE child with d ≤ 99 gives the parent WIN; with d = 100 the parent's distance is 101 > 100, giving CURSED\_WIN. The WIN case is dual, and cursed, blessed and draw children never change class under demotion. The storage code marks exactly these two cases, as the boundary loss and boundary win codes, which I maps to CURSED\_WIN and BLESSED\_LOSS; every other code maps to its plain inverse:

| Child code | I(code) | True κ |
| --- | --- | --- |
| LOSE (d ≤ 99) | WIN | WIN |
| BOUNDARY\_LOSS (d = 100) | CURSED\_WIN | CURSED\_WIN |
| BLESSED\_LOSS | CURSED\_WIN | CURSED\_WIN |
| DRAW | DRAW | DRAW |
| CURSED\_WIN | BLESSED\_LOSS | BLESSED\_LOSS |
| BOUNDARY\_WIN (d = 100) | BLESSED\_LOSS | BLESSED\_LOSS |
| WIN (d ≤ 99) | LOSE | LOSE |

Zeroing moves reset the clock, so they need no marker. ∎

**Theorem 12.12 (dropped WDL frame).** Let T be asymmetric with frame c dropped. Then WDL derivation at x returns c(x), or refuses, for every legal x with side to move c.

*Proof.* The side to move holds no right (Lemma 12.5), so Lemma 12.1 applies. By move kind:

- a bare-kings child gives DRAW;
- a zeroing child gives inv(the WDL probe of y) = κ (Theorem 12.36, and Lemma 12.37 for en passant);
- a quiet child lies in the retained frame (Lemma 12.4), whose stored-code read returns c₀(y) (by Theorem 12.31(b) when the frame is relaxed), and I(c₀(y)) = κ (Lemma 12.11).

Stopping early at WIN is sound since κ ≤ WIN. With no legal move the result is LOSE in check and DRAW otherwise. The recursion is well-founded: the retained frame is read, never re-derived, and every other recursion strictly lowers μ (Lemma 2.1). ∎

**Lemma 12.13 (singular WDL frames keep their boundary codes).** The singularity test declares a WDL frame singular only if no cell stores a boundary code or is cursed or blessed. So every cell of a singular WIN or LOSE frame is clean with d ≤ 99 and stores its class, the code Lemma 12.11 requires, and transcribe copies the frame's byte unchanged. Hence in Theorem 12.12 a quiet child in a singular retained frame reads its exact code. ∎

### 12.3 Dropped and loss-only distance frames

**Lemma 12.14 (pruning and skipping).** Suppose a derivation maximizes over a known set K, prunes moves whose class is below the parent's true class w, and records unknown-distance moves with an upper bound u ≥ κ. If no child class was unknown, K is non-empty, and every u ranks below the best over K, then that best equals w and its distance is the true distance. These conditions are the derivation's acceptance test for its best move.

*Proof.* Pruned moves have κ < w, and unknown moves have κ ≤ u < best ≤ w. So w is attained in K, and every move achieving w lies in K. ∎

For DTZ, a child of known class c but unknown distance is recorded with u = the DTZ lift of inv(c). This u is an upper bound: the move offers inv(c), demoted when δ > 100, and demotion turns WIN into CURSED\_WIN, which lowers κ, and LOSE into BLESSED\_LOSS, which the lift covers. Ties refuse, since a tie in class can still change the distance.

**Theorem 12.15 (dropped or loss-only DTZ, DTM, DTM50).** Each of the following derivations refuses or returns the stated value:

- DTZ derivation returns the true DTZ for clean classes, and for cursed classes a value in \[DTZ − 1, DTZ + 1\], or in \[DTZ − 1, DTZ\] (the 1 from Theorem 11.9) when no castling exit along the nest of derivations composes over a rounded derived value. A read is *rounded* when it is a cursed DTZ read from a one-byte or singular frame (a singular frame decodes in the one-byte tier) or an odd row 0 of a cursed DTC cell. An option is rounded when the child value it composes is rounded, and a derived value is rounded when any option it compares is rounded, whichever option attains the optimum, except that a derivation stopping early at distance 1, which is exact, is not rounded. Stored-mode DTZ derivation returns an exact 1 or its stored read, rounded exactly when that read is. Two probes of one position, whatever their sources, differ by at most 1, and agree when neither is rounded;
- DTM derivation returns DTM exactly;
- layered DTM50 derivation returns the layer-h value, propagating a child clock of 0 after a zeroing move and h + 1 otherwise.

*Proof.* **DTZ.** Moves get (κ, δ) as at the start of this section, demoted over 100 as in the solver; a LOSE parent forces its children to WIN unprobed. Pruning uses the DTZ lift, raising LOSE to BLESSED\_LOSS, since a quiet WIN child at d = 100 offers BLESSED\_LOSS. A quiet move giving up a castling right reads its child in the rights-dropped twin. A clean child composes as u + 1, as in the solver (Definition 5.19), and exactly, clean values being exact by induction on the nest. Clean reads are exact (Theorem 11.9). The options of class WIN at a WIN parent are zeroing moves into LOSE and quiet moves or exits into clean LOSE children with 1 + d ≤ 100, and every child of a LOSE parent is a clean WIN, since a CURSED\_WIN child would offer BLESSED\_LOSS > LOSE. So a clean value is an optimum over exact options, no cursed value enters it, and demotion over 100 is decided on exact distances. A cursed child, whose true value x the solver composes as f(x) = (x + 1) \| 1 = x + 1 + \[x odd\], composes as f(y) when its value y is exact and as y + 2 when y is rounded. A one-byte cursed DTZ read is the odd value 2s − 1 for x ∈ {2s − 1, 2s} (Theorem 11.9), and a two-byte one is exact: a frame is written two-byte only when its one-byte alphabet does not fit, and a frame extracted from a DTC pack (Theorem 12.23) has the one-byte alphabet of the frame its row 0 was decoded from, so it is two-byte only if that frame was. The row 0 of a DTC cell is the DTZ as its source file decoded it (Theorem 8.15), in a tier the pack does not record. A one-byte decode of a cursed value is odd, so an even cursed row 0 is exact, and an odd one, exact or with x ∈ {r, r + 1}, counts as rounded. A value derived from exact options is exact, and so is an exit over an exact child. Let d = y − x, so that (y + 2) − f(x) = 1 + d − \[x odd\]. We show by induction on the nest that every rounded value has d ∈ {−1, 0, 1}, with d = 1 only when x is odd. A read has d ≤ 0. In every derivation the child of a non-exit move is read, never derived. It lies in the same table with the other side to move, and a table has at most one dropped frame and never one beside a loss-only frame (Lemmas 12.22 and 12.25). So when the parent's frame is dropped the child's frame is stored, and is read directly or, when relaxed, by stored-mode DTZ derivation, which returns an exact 1 or the stored value. In a loss-only file only win-class parents are derived, and the non-exit children they probe are of a loss class (Lemma 12.16), which is stored. Hence a non-exit move keeps its child's d ≤ 0, and the invariant holds for it although 1 + x has the opposite parity to x. Only the child of an exit, in the twin table, can be derived. An exit maps d = −1 to 0 or −1, d = 0 to 0 or 1, and d = 1, where x is odd, to 1, and its true value f(x) is odd. So an exit over a rounded child keeps the invariant; over a rounded read, odd with x ∈ {y, y + 1}, it gives y + 2 = f(y) = f(x). Minimum and maximum preserve the bound: if every option is within one ply of its true value, the optimum of the reported options is within one ply of the true optimum, and it is not above it when no option is. A derived value above its true value has an overshooting option at the true optimum, necessarily an exit over a rounded derived value, so its true value is odd. Without such an exit, no option overshoots. Lemma 12.14 concludes.

*Two probes.* An unrounded value is exact, so two unrounded probes agree. Suppose two probes of x report X − 1 and X + 1 for the true optimum X. If the optimum is a minimum, take an option j that the first probe reports at X − 1; if a maximum, one that the second reports at X + 1. The one-ply bound makes the true value of j equal to X, and the other probe's extremum forces it to report j at the other endpoint, so the first probe reports j one short and the second one long. A read never overshoots, so j is an exit over a rounded derived child in the second probe. There an overshoot needs the child to have d = 0 at an even true value t or d = 1 at an odd one, and in the first an undershoot needs d = −1 at an odd t. So t is odd and the two probes report t + 1 and t − 1 at the child: the same configuration one exit deeper, which a finite nest cannot sustain. A read at the root differs from a derivation by at most 1 likewise, a read being short only at an even value and a derivation long only at an odd one. Appendix B checks every local step of both inductions exhaustively.

**DTM.** Cursed classes fold into decisive ones. Children with en-passant rights go through the full probe.

**DTM50.** Zeroing moves reset the clock and quiet moves, castling included, increment it; a child at clock 100 loses only if mated. A higher clock only removes the winner's options (Theorem 7.7), so at a WIN parent a pruned child cannot be lost at its layer. At a LOSE parent, a child of unknown layer value is recorded with its clock-0 class WIN, offering LOSE. This is no upper bound, since the child may be a DRAW at h + 1. Such a DRAW matters only when best = LOSE, where the record already forces a refusal. When best is DRAW, the parent's ceiling at this layer, a tie changes nothing. ∎

**Lemma 12.16 (decisive arguments).** Every distance read, every distance derivation and the draw-flip reader are entered only with a class other than DRAW and ILLEGAL, and the layered DTM50 read only at a clock below 100.

*Proof.* In the full probe an ILLEGAL class returns at once and a DRAW class reports its metrics drawn (Theorem 7.12(a)). The layered DTM50 read runs only when the 50-move fold is WIN or LOSE, at a clock below 100. The draw-flip reader runs only from the flat DTM50 read, with its class, and a child's class is passed on only when it is not ILLEGAL. In derivations c(x) = max κ is exact, since WDL reads are exact or refused (Theorems 12.12, 12.31 and 12.36). In each derivation, children receive the following classes:

- **DTZ derivation.** A WIN parent keeps only children whose DTZ lift of inv(c(y)) is WIN, so c(y) = LOSE. A CURSED\_WIN parent keeps c(y) ∈ {LOSE, BLESSED\_LOSS}, and a LOSE parent forces WIN. A BLESSED\_LOSS parent has no child of class DRAW, BLESSED\_LOSS or LOSE, which would offer κ ≥ DRAW > c(x), so c(y) ∈ {CURSED\_WIN, WIN}.
- **DTM derivation.** A WIN or CURSED\_WIN parent keeps c(y) ∈ {LOSE, BLESSED\_LOSS}, and a LOSE or BLESSED\_LOSS parent forces WIN.
- **Layered DTM50 derivation.** A WIN parent keeps only LOSE (a BLESSED\_LOSS child offers CURSED\_WIN < WIN), and a LOSE parent forces WIN. A child at clock 100 or more is valued as terminal before any read.
- **Flat DTM50 derivation.** As for DTZ derivation, pruning through fold: a WIN or CURSED\_WIN parent keeps c(y) ∈ {LOSE, BLESSED\_LOSS}, a LOSE parent forces WIN, and a BLESSED\_LOSS parent sees only c(y) ∈ {CURSED\_WIN, WIN}.
- **DTC derivation.** Win derivation reads only LOSE children, and loss derivation reads its children with WIN. Cursed derivation reads c(y) ∈ {LOSE, BLESSED\_LOSS} at a CURSED\_WIN parent and, as for DTZ derivation, c(y) ∈ {CURSED\_WIN, WIN} at a BLESSED\_LOSS parent. ∎

**Lemma 12.17 (forcing from BLESSED\_LOSS).** At a BLESSED\_LOSS parent, DTM derivation gives every child without en-passant rights the class WIN. It returns what it would return with the children's true classes.

*Proof.* The parent is lost in Γ∞, so every child is won (Definition 6.1), of class WIN or CURSED\_WIN. The derivation, and any derivation at the child, uses the child's class only through fold, which maps both to WIN, and the child's DTM read uses it only through whether it is a win class, in frame location and decoding (Theorem 11.10). So the values are unchanged. ∎

**Theorem 12.18 (flat DTM50 derivation).** Flat DTM50 derivation returns DTM exactly. The DTZ it returns is exact.

*Proof.* **DTM.** The derivation uses DTM derivation's class map, pruning and skip records, without the early exit; it forces WIN only from a LOSE parent, a subset of DTM derivation's forcing parents, and every unforced class it reads is exact. Only child distances differ. A child without en-passant rights is read from layer 0 of its DTM50 file unclocked, or derived by flat DTM50 derivation when that frame is dropped or loss-only at a win class. Layer 0 holds DTM as v >> 1 (Lemma 7.8, Theorem 11.10), decoded by parity from the child's class, cursed and clean classes alike (Theorem 11.10). A child that is read is not of class DRAW, hence decisive in Γ∞, and not of a win class in a loss-only frame, so its cell is in no singular DTM50 frame (Corollary 11.8) and no skipped block (Theorem 11.24), and layer 0 gives its DTM. A child with en-passant rights goes through the full probe. By induction on the nest, the proof of Theorem 12.15 (DTM) applies.

**DTZ.** Each move is offered as (inv(c(y)), δ), recorded unknown, or, with known class but unknown DTZ, recorded with u = the DTZ lift of inv(c(y)); zeroing moves, double pushes and bare-kings captures have δ = 1. Only WIN and CURSED\_WIN parents prune, and a pruned move offers κ ≤ DRAW < c(x), so by Lemma 12.14 a returned DTZ has class c(x) and is the optimum over offered moves of that class. A clean child's DTZ comes from the exact draw flip (Corollary 7.10) or a flat derivation, and a cursed child's only from a flat derivation. If neither is available, the move is recorded with u, and the parent refuses when u ≥ c(x). The proof proceeds by induction on the nest.

*Clean parent.* A WIN parent's best option is a zeroing move into LOSE or a quiet move into a clean LOSE child with δ ≤ 100; lower options do not matter. Every child of a LOSE parent is a clean WIN, since a CURSED\_WIN child would offer BLESSED\_LOSS > LOSE. Clean exits compose as u + 1 (Definition 5.19), so by induction the result is exact.

*Cursed parent.* Its options of class c(x) are exact zeroing moves (δ = 1), quiet moves into a clean child at distance 100 (δ = 101), and quiet moves into a cursed child whose derived DTZ is val′(y) by induction, no cursed DTZ being read. A non-exit offers s(val′(y)) at distance 1 + val′(y), and an exit offers f(val′(y)), composed as the solver composes it (Definition 5.19). So the result is exact. ∎

**Lemma 12.19 (DTZ in DTC win and loss derivation).** Whenever DTC win derivation or DTC loss derivation returns, the DTZ it returns is the clean DTZ of x, and it is decisive.

*Proof.* **Win.** DTZ(x) is the minimum over WIN options: 1 for a zeroing move into LOSE, 1 + z(y) ≤ 100 for a quiet one. The derivation minimizes over exactly the moves into LOSE, with 1 for conversions and pushes and 1 + (the child's DTZ) for quiet moves; moves into other classes offer at most CURSED\_WIN. A quiet LOSE child with z(y) = 100 contributes 101, never the minimum, since x ∈ A\_100 (Corollary 2.5) has a WIN option of at most 100: a zeroing move into LOSE or a quiet move into B\_99 ⊆ LOSE, which contributes or makes the derivation refuse, so the set is non-empty. An early stop at (0, 1) is a conversion or a quiet mate in one, with DTZ 1. A quiet child's DTZ is row 0 of its DTC cell (Theorem 8.15) or from a nested DTC loss derivation, exact by induction.

**Loss.** DTZ(x) is the maximum over all options: 1 for each zeroing move and 1 + z(y) for each quiet move, whose child is a clean WIN. The derivation takes 1 for every conversion and push, including a double push whose en-passant reply wins, and 1 + (row 0 of the child's curve) for every quiet move. With no legal move x is checkmated and DTZ is 0; otherwise every move contributes or makes the derivation refuse.

Row 0 of a decisive cell is decisive (Theorem 8.15), and no skipped block holds a decisive cell (Theorem 11.24). So no contribution is drawn, and the returned DTZ is decisive. ∎

**Theorem 12.20 (DTC cursed derivation).** For a CURSED\_WIN or BLESSED\_LOSS cell, DTC cursed derivation returns no price and a DTZ within the tolerance of Theorem 12.15.

*Proof.* Let x be CURSED\_WIN. Its CURSED\_WIN options are a zeroing move into BLESSED\_LOSS (CW\_1), a quiet move or exit into BLESSED\_LOSS, and a quiet move or exit into LOSE with z = 100 (CW\_101); having no WIN option, it has no zeroing move into LOSE and no quiet move or exit into LOSE with z ≤ 99. The derivation visits every move, ignores children outside {LOSE, BLESSED\_LOSS}, which offer at most DRAW, stops at the minimal CW\_1, and otherwise minimizes 1 over zeroing moves, 1 + (the child's DTZ) over quiet moves and exits into clean children, and the composition of Definition 5.19 over exits into cursed or blessed children.

Let x be BLESSED\_LOSS. No capture bares the kings, which would offer DRAW > c(x). Its BLESSED\_LOSS options are a zeroing move into CURSED\_WIN (BL\_1), a quiet move or exit into CURSED\_WIN, and a quiet move or exit into WIN with z = 100 (BL\_101). Its LOSE options, a zeroing move into WIN and a quiet move or exit into WIN with z ≤ 99, are dominated, and the derivation excludes exactly these, using the WIN child's exact clean DTZ, before maximizing the same compositions as for CURSED\_WIN.

In both cases a non-zeroing child's DTZ is row 0 of its DTC cell, as decoded (Theorem 8.15), or from a nested DTC derivation, and an unknown child makes the derivation refuse. The child of a non-exit move is read, never derived: it lies in the other frame of the same DTC file, which is stored when the parent's frame is dropped (Lemmas 12.22 and 12.25), and in a loss-only file only a CURSED\_WIN parent is derived, whose counted children are of class LOSE or BLESSED\_LOSS, which are stored. Only the child of an exit can be derived, as the proof of Theorem 12.15 (DTZ) requires. Otherwise the optimum is over a non-empty set, since x has an option of class c(x) and each contributes. The value follows Definition 5.19 except that a cursed exit over a rounded child composes as u + 2, an odd cursed row 0 being rounded and a derived cursed value being rounded exactly when the child of some counted quiet move or exit is rounded (a derivation ended at CW\_1 is exact), and a read may give u − 1 for an even cursed u (Theorem 11.9), the deviations bounded in the proof of Theorem 12.15 (DTZ). The priced fields stay empty, correctly, since a cursed or blessed cell is D in every layer (Corollary 8.10). ∎

**Theorem 12.21 (loss-only frames).** Under frame location, a loss-only frame is unreadable exactly for win classes. A win is rebuilt by derivation: every quiet or zeroing child whose distance it reads is a loss for its side to move, and losses are stored. A double-push child is probed in full with its en-passant square and may itself be derived. The recursion terminates by the depth cap (Lemma 12.9), and it is well-founded because the double-push child has lower pawn life (Lemma 2.1). ∎

**Lemma 12.22 (dropped plus loss-only).** No derivation meets a table with one frame dropped and the other loss-only.

*Proof.* The tools never produce the combination for an asymmetric material. For a symmetric one the loader marks the unstored Black frame dropped and copies White's loss-only flag to it, and frame location reads that frame through the mirror. ∎

**Theorem 12.23 (pack extraction).** A DTM file that transcribe extracts from a DTM50 file stores DTM exactly. A DTZ file that it extracts from a DTC file decodes to the generator's DTZ within the tolerance of Theorem 11.9: exactly, except that an even cursed or blessed value may decode one ply short, with its class preserved.

*Proof.* **DTM from DTM50.** The extraction requires the WDL frame of every stored DTM50 frame and reads WDL unrelaxed, so classes are exact. A DRAW class gives DRAW; otherwise it reads the first rank of the cell's record, the rank of packed layer 0 in all four states (Lemma 11.12). Layer 0 holds DTM as v >> 1 (Lemma 7.8, Theorem 11.10), decoded by parity from the class, a cursed or blessed class as its Γ∞ win or loss, and halving the decoded value again is lossless (Theorem 11.10). In a source that is not loss-only, a singular frame has no decisive value in any layer (Corollary 11.8) and a skipped block no decisive DTM row (Theorem 11.24), so their legal cells are DRAW and nothing is read there.

**DTZ from DTC.** Row 0 holds, unencoded, the DTZ decoded from the DTZ file (Theorem 8.15), so full-width decoding gives v̂ = v, or v̂ = v − 1 odd (Theorem 11.9). Re-encoding v̂ is exact in the two-byte tier and for clean classes. In the one-byte tier with a cursed or blessed class, an odd v̂ is a fixed point of s = ⌈v̂/2⌉ followed by 2s − 1, and an even v̂, then equal to v, decodes to v − 1. The class comes from WDL. In a source that is not loss-only, a singular frame has no decisive row and a skipped block no decisive DTZ row (Theorem 11.24), so their cells are DRAW.

**Loss-only.** A loss-only source is accepted only by a loss-only run and omits exactly the win-class values, so in a normal frame, skipped blocks included, an unstored value is read only at a win-class cell, where the loss-only storage value discards it (Lemma 11.2). A singular frame of a loss-only source stores no decisive value, so its cells are DRAW or win-class. It is written as singular 0 with the loss-only flag, and is read neither at a DRAW cell (Lemma 12.16) nor at a win-class cell, where a loss-only frame is unreadable (Theorem 12.21). ∎

**Lemma 12.24 (symmetric materials).** The WDL, DTZ, DTM, DTM50 and DTC files of a material either all store one frame or all store two, and the prober treats a material as symmetric exactly when its files store one frame. Every read of the unstored Black frame is exact.

*Proof.* Every generator stores only White to move exactly when the material's key equals its color mirror's, for all five metrics, and the prober applies the same test to the same configuration (Lemma 12.3). For a one-frame file the loader marks Black dropped with White's flags, and frame location reads a Black position through the color mirror, a White-to-move position of the same material and value (Lemma 4.9, Color). So a mirrored DTZ, DTM, DTM50 or DTC read gets the class of the position it indexes, with which its packed columns were written. ∎

**Lemma 12.25 (output frame states).** Every file that transcribe writes has frame states among those of Step 3 of Theorem 12.10. A WDL file has at most one dropped frame, whose side holds no castling right, and relaxed frames only in a relaxed run. A DTZ frame may be loss-only or relaxed, never both. A DTM, DTM50 or DTC frame is never relaxed. No file has a dropped frame beside a loss-only one.

*Proof.* **WDL.** A payload frame is relaxed exactly in a relaxed run, and a singular frame is copied without flags (Lemma 12.13). A frame is dropped only if its source frame was dropped, by a tool (Lemmas 12.4 and 12.5), or if the shrinking mode drops it, which it does only when no frame is dropped and only on a side holding no right (Lemma 12.5). **DTZ.** A relaxed loss-only run is refused, and a relaxed distance run on a symmetric material becomes loss-only with relaxation cleared. A loss-only run refuses a dropped source frame and drops nothing. **DTM, DTM50 and DTC.** As for DTZ, except never relaxed: DTM is written unrelaxed and the layered writer has no relaxed mode. **Symmetric materials.** One frame is stored (Lemma 12.24) and nothing is dropped. So Step 3 holds, and no dropped frame lies beside a loss-only one, as Lemma 12.22 requires. ∎

**Lemma 12.26 (files written twice).** When one transcribe run writes the DTZ or DTM file of a material twice, both writes use the same mode, and the surviving file is the extraction of Theorem 12.23. A loss-only DTC or DTM50 file takes its permutation codes from the DTZ or DTM file written just before it. A material whose outputs already exist keeps its files unchanged.

*Proof.* transcribe writes DTZ, DTC, DTM and DTM50 in that order, or WDL alone. It writes DTZ or DTM twice only in a loss-only run, both writes loss-only, or when extracting from DTC or DTM50, both full and equally shrunk. No relaxed file is written twice: DTM is never relaxed, and a relaxed pawnful run is refused for DTC on an asymmetric material and becomes loss-only on a symmetric one. The second write is the extraction of Theorem 12.23. The file supplying a loss-only DTC or DTM50 frame's permutation code is published under its final name before reopening, holds the same colors, is read with the writer's header layout, and gives code 0 from a singular frame. The existing-output test depends only on which files exist. ∎

**Lemma 12.27 (output over input).** If transcribe or shrink writes a file over one of its own inputs, the file written is a correct re-encoding of that input, and no later read in the same run returns a wrong value.

*Proof.* transcribe refuses an output directory textually equal to an input directory, as given, and for WDL also as the source file's directory was opened. Under an alias, each metric finishes every read of its memory-mapped source before writing its output under a temporary name and renaming it, and the mapped source keeps its old contents until released, so the replacement re-encodes the old file. In a relaxed WDL run whose output aliases a WDL input directory, a later material looking up a replaced sub-table finds a relaxed or dropped frame, so opening fails and the run is refused (Lemma 12.30). A replaced file with all frames singular carries no flag and is exact (Lemma 12.13). shrink writes a temporary file, releases its input and renames, skips an unshrinkable file when output is input, and lists a directory before writing into it. ∎

**Lemma 12.28 (admitted sources).** transcribe admits the following sources, and in each admitted combination it reads only values that the source defines:

a WDL run: a WDL file, possibly with a dropped frame, and never a relaxed one;

a full DTZ run: a full DTZ file, possibly with a dropped frame;

a relaxed DTZ run: a full or relaxed DTZ file, possibly with a dropped frame (Lemma 12.34);

a loss-only DTZ run: a full, relaxed or loss-only DTZ file without a dropped frame;

a DTM run: a full or loss-only DTM file under the same rules, never a relaxed one;

a DTC or DTM50 run: a full or loss-only file, never a relaxed one, from which a loss-only run always extracts a DTZ or DTM file and a full run does so on request (Theorem 12.23);

every run: WDL companion files opened in full format and unrelaxed.

*Proof.* The source checks admit a loss-only source only in a loss-only run, a relaxed source only in a relaxed or loss-only run (Lemma 12.34), and a dropped source frame only outside loss-only runs. A full source defines every value; a dropped source frame is not read, and the output drops it too. A relaxed source omits only win-class cells, which a relaxed run does not store and a loss-only run discards; a loss-only source omits only win-class values, which a loss-only run discards through its storage value (Lemma 11.2). A cursed read of a stored 0 yields no valid distance but keeps its win class, so it maps to the don't-care 0x7FF. Such a read occurs only at a discarded cell of this kind, or at a win-class cell of an emptied relaxed singular frame (Lemma 12.35) read by a loss-only run, also discarded. ∎

### 12.4 Relaxed WDL

Let B(x) be the maximum over capture and promotion moves of inv(V(y)), with the empty maximum taken as LOSE.

**Lemma 12.29.** B(x) ≤ c(x), since those moves are zeroing and κ ≤ max κ. A cell is capped exactly when its code c₀ is relaxable (BLESSED\_LOSS, DRAW, CURSED\_WIN or WIN; never LOSE and never a boundary code) and rank(B) ≥ rank(c₀). Every admissible stored s then satisfies rank(cls(s)) ≤ rank(c₀) (Theorem 11.4). ∎

**Lemma 12.30 (the transcriber's cap).** For every legal cell x with a relaxable code c₀, transcribe caps x exactly when rank(B(x)) ≥ rank(c₀).

*Proof.* transcribe's maximum B′ over captures and promotions starts from ILLEGAL (rank −1). Without such moves B′ has rank −1 and B = LOSE rank 0, below every relaxable c₀, so neither caps. Otherwise each child class is read from a full-format sub-table (Lemma 4.9, a symmetric child as White on the mirrored position), which is exact (Theorem 5.17). transcribe opens, under its material key, a table for every material the sub-table read can reach from a capture or promotion: capture, pair-breaking, promotion and capture-promotion children, surviving pair tables, and children of captures and promotions that also remove a castling right. Opening fails for a missing material other than bare kings and for a table with a dropped, loss-only or relaxed frame, so a child without a table is bare kings, given its value DRAW. Hence B′ = B when such moves exist, and in every case both cap alike. ∎

**Theorem 12.31 (relaxed WDL is exact).**

- (a) max(B, cls(s)) = c(x) for every admissible s.
- (b) The stored-code read returns c₀(x) exactly, boundary codes included, or refuses.

*Proof.*

(a) In the capped case, B ≥ c₀ = c(x) ≥ B, so B = c(x) and cls(s) ≤ c(x). In the uncapped case, s = c₀ = c(x) ≥ B.

(b) Order the storage codes by the preference of the side to move: LOSE < BOUNDARY\_LOSS < BLESSED\_LOSS < DRAW < CURSED\_WIN < BOUNDARY\_WIN < WIN. A boundary code is the longest clean value of its sign, so it sits between its class and the adjacent cursed class; on plain codes the order is the class rank. On a relaxed frame the stored-code read returns a stored WIN or ILLEGAL code unchanged. For every other code it computes B, refuses if B is unknown, and returns the larger of the stored code and B's plain code in this order. Each boundary code occurs both as a genuine marker and as filler in a capped cell.

- **Genuine BOUNDARY\_WIN** has DTZ 100; a winning capture or promotion would give DTZ 1, so B ≤ CURSED\_WIN, which is below it, and the code survives.
- **Filler BOUNDARY\_WIN** is admissible only under cap WIN, forcing B = WIN, which is above it. The read returns WIN, and c₀ is plain WIN, since boundary cells are never capped.
- **Genuine BOUNDARY\_LOSS** is a loss, so B = LOSE, which is below it, and the code survives.
- **Filler BOUNDARY\_LOSS** lies in a capped cell of class at least BLESSED\_LOSS, so B = c(x) is above it and replaces it.
- **Every other code** falls into three cases. A stored WIN is returned unchanged and equals c₀ even as filler, since its cap is then WIN. A fixed code survives, since B ≤ c(x). A capped code has B = c(x); it is raised to that class's plain code when B outranks it, and already has that class otherwise.

So derivation of a dropped frame sees exact codes, and Lemma 12.11 applies. ∎

### 12.5 Relaxed DTZ

**Theorem 12.32 (relaxed DTZ).** A win-class cell is omitted exactly when some zeroing move preserves its class, and its DTZ is then 1. For every win-class cell of a relaxed frame, the probe returns the true DTZ, within the tolerance of Theorem 11.9, or refuses.

*Proof.* The transcriber's predicate and the probe's zeroing-first test (stored-mode DTZ derivation) examine the same moves (captures, promotions and pawn pushes) and the same child class V(y), with the same en-passant rule for double pushes. If the predicate holds, the probe finds a class-preserving zeroing move at distance 1, the minimum; otherwise the cell was stored and is returned. Losses are never omitted, and a quiet mate in one is omitted only when a zeroing move also preserves its class, its DTZ being 1 either way. ∎

**Lemma 12.33 (inputs of the relaxation predicate).** transcribe evaluates the predicate of Theorem 12.32 only on exact classes: the cell's own class and the class of each capture, promotion and pawn-push child.

*Proof.* transcribe reads its own WDL file unrelaxed and unmirrored, and a capture or promotion child from its full-format sub-table (Lemma 12.30). A pawn push keeps the material and passes the move, so its child's class is read from the own WDL file's other frame at the quiet-move index, a double push combined with its en-passant replies as in Theorem 12.32. transcribe refuses a dropped own WDL frame under a stored distance frame, and either WDL frame dropped in a relaxed pawnful run. A symmetric distance run becomes loss-only, never relaxed. So a relaxed pawnful run reads two stored frames and a pawnless run only the cell's own stored frame. Stored frames, normal or singular, give exact classes (Lemma 11.1, Lemma 12.13). ∎

**Lemma 12.34 (relaxed sources).** transcribe accepts a relaxed DTZ file as a source only for a relaxed or a loss-only run, and neither run reads a value that the source omitted.

*Proof.* A relaxed run stores exactly the legal cells where the predicate of Theorem 12.32 fails; the predicate depends only on exact classes (Lemmas 12.30 and 12.33), so it selects the same cells as the source's run, and every cell read was stored by the source. The source omitted only win-class cells, which a loss-only run discards (Lemma 11.2). A skipped source block holds only DRAW, ILLEGAL or omitted cells (Theorem 11.24), which the output also treats as don't-cares, so values read there are never stored. ∎

**Lemma 12.35 (singular relaxed frames).** Let a relaxed DTZ frame be singular. Then it was copied from a singular source frame, or written because relaxation left its rank table empty, or written because every cell the run stores has the same storage value, of at most 255. At every win-class cell the probe returns the true DTZ, within the tolerance of Theorem 11.9, or refuses, and it never returns the stored value 0 of an emptied frame.

*Proof.* The relaxed flag is kept on singular frames and survives shrink (Lemma 12.7), so at a win-class cell the probe runs stored-mode DTZ derivation. It returns 1 when a zeroing move preserves the class, the true DTZ (Theorem 12.32); refuses when a zeroing child's class is unknown and no such move was found; and otherwise returns the stored value, since the predicate failed and the cell was stored. A frame copied from a singular source is exact at every cell the source stored (Corollary 11.8), which includes every cell the new run stores (Lemma 12.34). A frame whose stored cells share one storage value of at most 255 is written and decoded singular in the one-byte tier (Corollary 11.8), so each stored cell reads as in a one-byte frame (Theorem 11.9). A frame emptied by relaxation stores no cell, so it has no loss cell, since loss cells are never omitted, and every win-class cell satisfies the predicate. Its singular 0, which has no cursed decoding (Theorem 11.9 requires s ≥ 1), is never returned: DRAW cells are not read (Theorem 7.12(a)), and win-class cells give 1 or refuse. ∎

### 12.6 En passant

**Theorem 12.36.** Let xᵉ be x with en-passant rights, E its legal en-passant captures, and N its legal non-en-passant moves. Then V(xᵉ) is:

- max(c(x), max over m ∈ E of inv(V(y\_m))) when N ≠ ∅;
- max over m ∈ E of inv(V(y\_m)) when N = ∅ and E ≠ ∅.

The WDL probe and the full probe implement this rule.

*Proof.* The moves of xᵉ are N ∪ E, en-passant captures are zeroing, N's values do not depend on the en-passant right, and a position with en-passant rights has clock 0 in play; the probe accepts any clock, and Theorem 12.38 gives the distances at the given clock. When N = ∅ the table's terminal value (e.g. stalemate) does not apply and is discarded. The full probe refuses on an unresolved en-passant child, or on an unknown table class, since N was then never valued. ∎

**Lemma 12.37 (unresolved en-passant children).** If some en-passant child of xᵉ is unresolved, the WDL probe returns the combined class only when it is WIN, and refuses otherwise. So the WDL probe returns a table WIN without walking E, and stops walking E at the first capture worth WIN.

*Proof.* A WIN is final regardless of the missing capture, since no option exceeds WIN, and it is a real option of xᵉ wherever it came from:

- **From a resolved en-passant capture.** That capture is a legal move of xᵉ.
- **From the table.** A cell without non-en-passant moves is terminal and stores LOSE (mate) or DRAW (stalemate), so a table WIN implies a non-en-passant move, which keeps its value in xᵉ (Theorem 12.36).

A capture left unwalked after a WIN is no worse than an unresolved one, so the early returns give the same class. Any other class could be exceeded by the unresolved capture and is refused. Every caller treats ILLEGAL as a refusal: WDL derivation, the capture bound, DTZ derivation, DTC derivation, the en-passant conversion test and the root rankers. ∎

**Theorem 12.38 (en-passant distances).** In the setting of Theorem 12.36, value each y\_m at clock 0, let a bare-kings y\_m count as DRAW with every distance 0, and let r be the probe's clock. Whenever the full probe returns a distance for xᵉ, it is the following value, within the tolerance of Theorem 12.15 for a cursed DTZ:

**DTZ:** the ≻-maximum of val(x), when N ≠ ∅, and the options τ(cls(y\_m)) of m ∈ E;

**DTM:** the maximum in Γ∞ of DTM(x), when N ≠ ∅, and the options of m ∈ E, which are W\_{d+1} when DTM(y\_m) = L\_d, L\_{d+1} when DTM(y\_m) = W\_d, and D otherwise;

**DTM50 at r ≤ 99:** the maximum of DTM50\_r(x), when N ≠ ∅, and the options of m ∈ E, formed in the same way from DTM50\_0(y\_m);

**DTC at r ≤ 99:** the win (0, 1) if some cls(y\_m) = LOSE; otherwise DTC(x, r) if N ≠ ∅ and DTC(x, r) is a win; otherwise DRAW if some cls(y\_m) is BLESSED\_LOSS, DRAW or CURSED\_WIN; otherwise DTC(x, r) if N ≠ ∅, and the loss (0, 1) if N = ∅.

At r ≥ 100, DTC and DTM50 are draws (Theorem 7.12(b)).

*Proof.* Each m ∈ E is a zeroing capture into y\_m at clock 0, and N's moves keep their values in x (Theorem 12.36). So each metric of xᵉ is its recurrence over x's value on N and E's options, or over E's options alone when N = ∅, where the probe discards x's terminal value, starts from the first capture, and merges the rest as below. The probe merges DTC and DTM50 only at r ≤ 99. Otherwise it reads each y\_m unclocked, which leaves the class, DTZ and DTM of y\_m unchanged (Lemma 12.41), and the fixed answer at r ≥ 100 stands.

**DTZ.** The probe keeps the ≻-maximum of x's (class, DTZ) and the options τ(cls(y\_m)), each of distance 1 when decisive; a DRAW option carries no distance, and a merged answer of class DRAW reports its DTZ as drawn. A strictly higher class replaces the running value whatever its distance, and within a class ≻ decides. At an equal winning class, distance 1 attains the minimum; at an equal losing class, it never exceeds x's DTZ, which is at least 1 as N ≠ ∅. So the result is exact or carries the tolerance of x's own DTZ, and if x's DTZ is unknown and no option has a strictly higher class, no DTZ is returned.

**DTM and DTM50.** DTM merges by the order of Γ∞ values, cursed classes folded into decisive ones as in Theorem 12.15 (DTM), and DTM50 by the order of values at clock r. Neither compares WDL classes first: a WIN x may mate later than a capture into a BLESSED\_LOSS child, and may have a drawn DTM50 value at clock r. A capture whose child supplies no DTM, or no DTM50 value at clock 0, withholds the field, being a legal move of unknown value.

**DTC.** By Definition 8.1, applied as to a double-push child, V\_k(xᵉ) combines V\_k(x) with E's conversion values. A capture into LOSE wins with t = 1 at every k, giving (0, 1). A capture into BLESSED\_LOSS, DRAW or CURSED\_WIN folds to D and turns every non-winning layer into D. By Lemma 8.8 layers below a win are wins or D and layers below a loss are losses or D, so if DTC(x, r) is a win it stands; otherwise x has no win fitting P(r), and none at all if DTC(x, r) is a loss, so the answer is DRAW. A capture into WIN loses with t = 1: if N ≠ ∅, x is not checkmated, every loss of x contributes at least 1, and no layer changes; if N = ∅, such captures alone give L\_1 at every layer. The probe's lexicographic merge from x's DTC answer implements these cases. Without a DTC answer for x, it sets one only when an option's folded class strictly exceeds cls(x): a winning capture, giving (0, 1), or a drawing capture over a LOSE or BLESSED\_LOSS x, whose layers hold no win (Lemma 8.8, Corollary 8.10), giving DRAW. ∎

### 12.7 Soundness of refusal

**Theorem 12.39.** Whenever a derivation, the WDL probe or the full probe returns a value, that value is the true value, within the cursed tolerance of Theorem 12.15 for DTZ. In every other case it returns a refusal.

*Proof.* A skipped child is recorded as "unknown class", forcing a refusal, or with an upper bound u ≥ κ (Lemma 12.14). The one exception, the DTM50 LOSE-parent record, is handled in Theorem 12.15. Early exits return only provably optimal values: WIN, δ = 1 at a WIN or CURSED\_WIN parent, or the DTC win (0, 1). The DTC unbounded fold goes blind only on an unreadable DTZ row, which a decisive class never has: row 0 of a decisive cell is decisive (Theorem 8.15), and skipped blocks hold no decisive cell (Theorem 11.24). ∎

**Lemma 12.40 (zero-distance losses).** If the full probe returns class LOSE with DTZ 0, the position is checkmated.

*Proof.* Every DTZ source gives LOSE with 0 only at checkmate. A stored DTZ, singular frames included, decodes a clean value exactly (Theorem 11.9), and L\_0 is checkmate (Definition 5.1, Lemma 5.2). The draw-flip reader returns z(x) (Corollary 7.10), and z(x) = 0 means x ∈ B\_0. Row 0 of a DTC cell is the decoded DTZ (Theorem 8.15). DTZ, flat DTM50 and DTC loss derivation give every option δ ≥ 1, so they return 0 for a decisive class only without legal moves, which with LOSE is checkmate. The en-passant merge writes 0 only with DRAW: when N = ∅ and E ≠ ∅ a capture of distance 1 replaces x's terminal value, and when E = ∅ x's value stands. ∎

The prober's domain consists of the legal positions with at least one legal move, each with a clock r ≤ 100, or unclocked (the 50-move rule ignored). Terminal positions occur only as children inside derivations and root rankers, where the class separates checkmate (LOSE) from stalemate (DRAW) (Section 2.2, Definition 5.1), and a LOSE with DTZ 0 is a checkmate (Lemma 12.40).

**Lemma 12.41 (unclocked probes).** Unclocked, the full probe returns the same class, DTZ and DTM as at clock 0, and neither DTC nor DTM50.

*Proof.* By induction on the number of men, the induction covering the en-passant children. Class, DTZ and DTM do not depend on the clock, the drawn-clock test fires in neither case, and the probe takes DTZ from the same sources in the same order (Lemma 7.14), so the reported values coincide, not only the true ones. The unclocked probe skips the clocked DTM50 read, reads a pawnful DTC cell only for its DTZ row (Section 8.6), and withholds DTC and DTM50 from its answer, en-passant merge included. ∎

**Lemma 12.42 (answer fields).** In every answer of the full probe: (a) a refusal, or the class ILLEGAL, carries no metric; (b) a decisive DTC answer has the probe's class, and a drawn DTC answer has order and distance 0; (c) a DTM50 answer of WIN or LOSE has the probe's class, and a drawn DTM50 answer has distance 0.

*Proof.* (a) A refusal or an ILLEGAL class is returned before any metric is set. (b) Without en passant, DTC comes from pawnless pricing or the DTC read or derivation, and is decisive only when priced; a priced value is decisive only with the class of DTZ (Corollary 8.10), and a cursed or blessed class is never priced (Theorems 8.18 and 12.20). With en passant, the merge of Theorem 12.38 uses folded classes: a priced win comes from x's win or a capture into LOSE, so the merged class is WIN; a priced loss comes from x's loss or, when N = ∅, from captures all into WIN, so the merged class is LOSE; and a capture raising the merged class above LOSE offers a folded class of at least DRAW, replacing a loss. At r ≥ 100 the answer is DRAW (Theorem 12.38). A drawn DTC answer carries order and distance 0: both fields start at 0, and every write to them in a drawn answer is 0. (c) The layered read runs only for WIN or LOSE and returns that class or DRAW (Theorem 7.7, Corollary 2.5.2); the shortcut of Theorem 7.11(a) copies the class; and at r ≥ 100 the answer is LOSE only for a LOSE cell (Theorem 7.12(b)). In the en-passant merge a capture child, valued at clock 0, has DTM50 answer LOSE only with class LOSE, making the merged class WIN, and a drawn answer with class CURSED\_WIN, DRAW or BLESSED\_LOSS (Theorem 7.12(c)); so a capture raising the merged class above LOSE offers DRAW or WIN, outranking a loss, or withholds the field. A drawn DTM50 answer carries distance 0 by the same argument. ∎

**Theorem 12.43 (root rankers).** The DTM root ranker returns the exact value of every root move at the root's clock. The DTZ root ranker returns, for every root move, its exact class from clock 0, a rank that reflects the root's clock (Corollary 12.44), and its DTZ distance. The distance is exact for a clean move and within the tolerance of Theorem 12.15 for a cursed or blessed one; a move re-ranked by DTC reports its DTC distance instead. The WDL root ranker returns, for every root move, the inverse of its child's exact class, ranked by Fathom's rank, which does not read the clock. Each ranker refuses the whole ranking when a child's class, or a distance the ranker needs, is refused.

*Proof.* A move's value is the inverse of its child's value at the clock the move leaves: 0 after a zeroing move, r + 1 after a quiet one. Ignoring the 50-move rule, the DTZ ranker ranks with r = 0: it takes a zeroing child at clock 0 and a quiet child at clock 1, the clocks the moves leave. A quiet child probed at clock 0 would be priced against the ply limit P(0) = 100 instead of P(1) = 99, and its value plus the move's ply could then exceed the root's limit P(0). The DTM ranker reads only DTM. Under the rule, the DTM ranker takes the child's DTM50 value at the clock the move leaves. When r ≥ 100 the root is drawn (Section 2.2), and the ranker reports every move as drawn at the root clock: as a cursed win, blessed loss or draw by its DTM class, with a draw-band rank. At r = 100 the DTZ ranker gives no move rank ±1000, so it re-ranks none, and it needs a quiet child only for its class and DTZ, which do not depend on the clock; the probe answers a clock of 100 or more as in Theorem 7.12(b). The WDL ranker inverts the child's WDL probe class.

For the DTZ ranker, a quiet move's distance is d = the child's DTZ + 1, which is 1 when the child is checkmated, a loss with DTZ 0 (Lemma 12.40). A castling exit into a cursed child of reported value y composes as in derivation (Theorem 12.15): f(y) when y is exact and y + 2 when y is rounded. The move is cursed or blessed when its child is cursed or blessed or d > 100, as with s in §5.1. By Theorem 12.39, d is exact when clean. Otherwise the child's value is within one ply of its true value and satisfies the invariant of the proof of Theorem 12.15, overshooting only at an odd true value. A non-exit adds one ply to both the reported and the true value, and an exit keeps the invariant, so d is within the tolerance of Theorem 12.15. A cursed or blessed move is reported as ±(100 + d), and a zeroing move into a cursed child as ±101, as in Fathom's score. A clean win of distance v takes rank 1000 exactly when v + r ≤ 100, that is, by Lemma 2.4 and the exactness of clean DTZ, when it wins at the root clock. Every other win, clean or cursed, of reported distance v takes rank max(1, 1000 − (v + r)), which is below 900. So cursed wins rank below clean wins and blessed losses above clean losses, and outside ranks ±1000 the rank is monotone in d in the direction of ≻. A cursed win has rank at least 1 and a blessed loss at most −1, and equal ranks sort by reported distance, so a cursed or blessed move past 900 plies keeps its side of the draws and its order in its band. A clean loss of distance v takes rank −1000 when 2\|v\| + r < 100. Every other loss, clean or blessed, of reported distance v takes rank min(−1, −1000 + \|v\| + r), so every loss at the root clock ranks at most −900, and above −1000 a longer loss ranks no lower. With a flagged repetition, every win takes rank max(1, 1000 − (v + r)), ordering wins by v.

*Re-ranking by DTC.* A move is re-ranked when it has rank 1000 without a flagged repetition, or rank −1000, and it is a conversion or its child has a decisive DTC answer at the clock the move leaves. It then gets rank 1000 − o as a win or −1000 + o as a loss, and reports t. Here (o, t) is the candidate of Theorem 8.19 for a win and the loss contribution of Definition 8.1 for a loss:

- (0, 1) for a conversion;
- (b\*(c) + 1, 1) for a winner's push, which spends a unit;
- (b\*(c), 1) for a loser's push;
- (b\*(c), \|V(c)\| + 1) for a quiet move.

*Bands.* As o ≤ 30 (Lemma 8.20), a re-ranked win keeps rank at least 970, above every win that does not win at the root clock, and a re-ranked loss has rank in \[−1000, −970\]. A loss at the root clock that is not re-ranked keeps −1000 when 2\|v\| + r < 100. Otherwise it has rank −1000 + \|v\| + r in \[−950, −900\], since \|v\| + r ≥ max(\|v\|, 100 − \|v\|) ≥ 50 there. So every loss at the root clock lies in \[−1000, −900\]. The re-ranked ones lie below the band \[−950, −900\], ordered by (o, t), and a loss that keeps −1000 sorts at or below them.

*Order among winning moves.* Within the rank-1000 band the order is lexicographic in (o, t), smallest first. A move winning at the root clock has a clean-loss child c with z(c) ≤ P at the child's clock. Push budget 30 is the DTZ row, since V\_30 denotes clean DTZ, so \|V\_30(c)\| = z(c) ≤ P: budget 30 fits the ply limit at the child's clock, and c has a decisive DTC answer when available (Definition 8.2). So at a root without a flagged repetition, when every winning move's child supplies it, the first move of a winning root attains the minimal (order, value) of Definition 8.2 (Theorem 8.19). A winning move whose child supplies none keeps rank 1000 and its DTZ distance, sorting ahead of every re-ranked move of order at least 1. A losing one keeps rank −1000 and its DTZ distance. En-passant captures are scanned last.

*Refusals.* Each child value is exact, within the tolerance of Theorem 12.15 for a cursed DTZ, or refused (Theorems 12.36 and 12.39). A refused child class, quiet-child DTZ (DTZ ranker), or DTM or DTM50 value (DTM ranker) refuses the whole ranking. ∎

**Corollary 12.44 (ranker bands).** Let the DTZ root ranker apply the 50-move rule at root clock r. A root move has rank at least 900 exactly when it wins at the root clock, and rank at most −900 exactly when it loses at the root clock. Every other winning move has rank in \[1, 899\], and every other losing move rank in \[−899, −1\]. Without the rule, every move that wins in Γ∞ has rank at least 1 and every move that loses in Γ∞ rank at most −1. So Fathom's score, a mate score exactly when the rank reaches ±900 under the rule and ±1 without it, is a mate score exactly for the moves decisive at the root clock, or in Γ∞ when the rule is off.

*Proof.* A clean win of distance v wins at the root clock exactly when v + r ≤ 100 (Lemma 2.4), and then has rank 1000, 1000 − o ≥ 970 after DTC re-ranking, or 1000 − (v + r) ≥ 900 with a flagged repetition (Theorem 12.43); otherwise its rank is at most 899. A cursed win, reported at 100 + d with d ≥ 1, ranks max(1, 1000 − (100 + d + r)) ≤ 899 and does not win at the root clock. Both rank at least 1. A clean loss of distance v loses at the root clock exactly when \|v\| + r ≤ 100: a zeroing move into WIN has \|v\| = 1 and loses exactly when r ≤ 99, and a quiet move exactly when its child's DTZ is at most 99 − r (Lemma 2.4 at clock r + 1). It then ranks at most −900, or −970 after re-ranking. Otherwise 2\|v\| + r ≥ 102, so its rank is −1000 + \|v\| + r, lowered to −1 when larger, in \[−899, −1\]; a blessed loss, with \|v\| = 100 + d ≥ 101, lies there too. Without the rule r = 0, every win ranks at least 1 and every loss at most −1. ∎

**Lemma 12.45 (the color mirror at the root).** Let φ swap the colors and the side to move, flip the rank of every square, and give each castling right to the other color with its rook square rank-flipped. For every position x with en-passant square e, φ maps the legal moves of x bijectively onto the legal moves of φx with the rank-flipped square, preserving the kind of each move, and every probe answer at φx equals the answer at x. A root ranker that probes φx returns, for each move m of φx, the move φ⁻¹(m) of x with the value of m. An en-passant square that admits no legal capture is ignored in both orientations.

*Proof.* Chess is invariant under φ: moves of every kind and attacks map to their images, preserving legality, check, checkmate and stalemate, and φ exchanges the en-passant conditions (e on the sixth rank with White to move or the third with Black, a mover's pawn beside e's file on the fifth or fourth rank, an enemy pawn on e's file). φ preserves zeroing and the clock, and no definition of val(·, c), DTM, the WDL class, DTZ or a DTC layer refers to color (Definitions 2.3, 5.19 and 8.1), so the true values agree. The prober's answers agree as computed, not only as values, cursed DTZ values, rounding and refusals included. For a material other than its own color mirror, routing orients x and φx by the material key (Lemma 12.2) and mirrors exactly one of them, so both probes are the same probe of the same position. For a material equal to its mirror, neither is mirrored; every read of a position with Black to move goes through the mirror to the White frame (Lemma 12.24), so x and φx read the same cells under the same frame flags, and every derivation at φx visits the images under φ of the moves it visits at x, with the same move kinds, en-passant squares and numbers of castling rights. No derivation's result depends on the order in which it visits moves: a running best is replaced only by a strictly better value, skip records and folds are order-free, and each early stop returns a value fixed by the option that triggers it. By induction on the nest, the two probes return the same class, distances and rounding, and refuse together. A ranker maps a move back by flipping both squares and keeping the promotion piece and the en-passant and castling marks, which is φ⁻¹, and recognizes captures from the board. With no legal en-passant capture at e, E is empty in both orientations and the probe returns the value of x. ∎

## 13. Requirements and precision

### 13.1 Requirements

| Id | Requirement | Used in |
| --- | --- | --- |
| RANGE | Every DTM, every DTM50 distance and every cursed DTZ is at most 2045, and every cursed DTZ that a castling exit reads in its twin table is at most 2044. A value of 2047 would coincide with the ILLEGAL code 0x7FF. It arises as a value one ply beyond a child value of 2046 (a win beyond a loss, or a loss beyond a win), or as a composed exit value (u + 1) \| 1 with u ≥ 2045, which initialization writes as a seed even when the cell's final value is smaller, or as the prober's exit y + 2 over a rounded twin value y = 2045. The twin bound 2044 excludes both: a twin of true value at most 2044 is reported at most 2044 (Theorem 12.15), so y + 2 ≤ 2046, and (u + 1) \| 1 ≤ 2045. | §5, §6, §7, §8, §11, §12, Appendix B |
| LE | The host is little-endian; chesstb and transcribe refuse to start otherwise, and shrink and the probe library assume it | §11, §12 |

### 13.2 Stored precision

Clean DTZ is always exact, and cursed DTZ is exact under Definition 5.19. The one-byte tier decodes an even cursed value one ply short (Theorem 11.9), and a derived cursed DTZ is within one ply of the true value, however many castling exits its derivations cross (Theorem 12.15). The WDL class, and therefore every game-theoretic result under the 50-move rule, is exact whenever it is returned.

## 14. Conclusion

We have proved that the game-theoretic core of ChessTB is correct under the requirements of Section 13.1. Every WDL class it computes, stores and serves is exact. So is every clean DTZ, every DTM, every DTM50 layer, and every DTC price at every clock (Theorem 8.19). Cursed DTZ is computed exactly under Definition 5.19. The one-byte tier decodes an even cursed value one ply short, and a derived cursed DTZ is within one ply of the true value (Theorem 12.15).

The proofs rest on four structural facts:

1. **Every cycle is a quiet cycle** (Lemma 2.1). All retrograde work is therefore confined to one pawn slice and its mirror, and every other input is final before it is read.
2. **The 50-move rule reduces to one integer per position** (Lemma 2.4). The same statement yields the WDL classes, the boundary codes, the monotone DTM50 stack and the embedded DTZ.
3. **Every reduction is invertible by a local argument.** The retained frame, the stored losses, the capture bound and the zeroing test each supply the information one ply of minimax needs, and the two boundary codes supply the rest (Lemma 12.11).
4. **Every concurrent store is idempotent or single-valued**, so the lock-free passes are deterministic.

## Appendix A. Depth model

This program evaluates the graph G of Theorem 12.10. Each function c\_X lists the calls of one prober function as (plies, callee, arguments), following Table 12.2. In the program, PI is the full probe, PWI the WDL probe, WI the table WDL read, RSt the stored-code read, RB the capture bound, WD WDL derivation, PDZ and ZD the DTZ probe and DTZ derivation, PDM and DM the DTM probe and DTM derivation, PD50, D50F and D50L the DTM50 probe and its flat and layered derivations, and PDC and DC the DTC probe and DTC derivation. val returns the largest depth, relative to a call, at which a guarded function is entered at or below it, and raises an error if a call recurs on its own stack. The program computes, for each m from 3 to 8, the maximum over roots of exactly m men. These maxima increase with m, so they equal D(m).

```python
# Exact longest line of nested probes over an abstract position space.
# Each model function returns its outgoing calls as (plies, function, args); a guarded
# function (a derivation or the capture bound, which test the depth cap) scores 0
# at its own entry. value(call) = the largest entry depth of a guarded function at or
# below the call, relative to the call's depth; NEG if none.
#
# A side is (lives, r, n): lives = sorted pawn lives (6 = start rank, 1 = seventh rank),
# r = rooks holding a castling right, n = other non-king men.
# pos = (stm, side0, side1, ep): ep = the side not to move has just double-pushed.
# A = frame assignment of the current file set, fixed along the path within a table:
#   (wdl, dtz, dtm, d50, dtc), None = not yet consulted on this path.
#   wdl in {'N', 0, 1}: the dropped color (retained frames are relaxed)
#   dtz, dtm in {0, 1, 'LO'}: dropped color (twin relaxed for DTZ), or loss-only
#   dtc in {'A', 0, 1, 'LO'}: 'A' = no DTC file, so the full probe reads DTZ
#   d50 in {'A', 0, 1, 'LO'}: 'A' = no DTM50 file, so the full probe reads DTM
# Classes: 0 L, 1 BL, 2 D, 3 CW, 4 W.
import sys
sys.setrecursionlimit(1000000)
NEG = -10**9
L, BL, D, CW, W = 0, 1, 2, 3, 4
def inv(q): return 4 - q
def win_class(q): return q in (W, CW)
def fold(q): return W if q == CW else (L if q == BL else q)
FRESH = (None,) * 5
OPTS = {0: ('N', 0, 1), 1: (0, 1, 'LO'), 2: (0, 1, 'LO'), 3: ('A', 0, 1, 'LO'), 4: ('A', 0, 1, 'LO')}
IWDL, IDTZ, IDTM, ID50, IDTC = range(5)

def men(s): return len(s[0]) + s[1] + s[2]
def total(p): return 2 + men(p[1]) + men(p[2])
def tid(s): return (len(s[0]), s[1], s[2])
def sym(p): return tid(p[1]) == tid(p[2]) and p[1][2] == 0 and p[2][2] == 0
def pawnful(p): return len(p[1][0]) + len(p[2][0]) > 0
def side(p, c): return p[1] if c == 0 else p[2]
def mk(c, a, b, ep=False):
    s0, s1 = (a, b) if c == 0 else (b, a)
    return (1 - c, s0, s1, ep)
def rm(t, x):
    t = list(t); t.remove(x); return tuple(sorted(t))
def add(t, x): return tuple(sorted(t + (x,)))

def moves(p):
    """(child, kind): R capture/promotion, S single push, D double push, Q quiet in table,
       E quiet move giving up a castling right."""
    c = p[0]; me = side(p, c); op = side(p, 1 - c)
    lv, r, n = me; olv, orr, on = op
    targets = [(rm(olv, x), orr, on) for x in set(olv)]
    if on: targets.append((olv, orr, on - 1))
    if orr: targets.append((olv, orr - 1, on))
    capturers = []
    if n or r == 0: capturers.append(me)
    if r: capturers.append((lv, 0, n + r))
    if r: capturers.append((lv, r - 1, n + 1))
    for x in set(lv):
        capturers.append((add(rm(lv, x), x - 1), r, n) if x > 1 else (rm(lv, x), r, n + 1))
    for t in targets:
        for a in capturers: yield mk(c, a, t), 'R'
    for x in set(lv):
        if x == 1: yield mk(c, (rm(lv, 1), r, n + 1), op), 'R'
        else: yield mk(c, (add(rm(lv, x), x - 1), r, n), op), 'S'
        if x == 6: yield mk(c, (add(rm(lv, 6), 4), r, n), op, True), 'D'
    if n or r == 0: yield mk(c, me, op), 'Q'
    if r:
        yield mk(c, (lv, 0, n + r), op), 'E'
        if r == 2: yield mk(c, (lv, 1, n + 1), op), 'E'

def ep_caps(p):
    if not p[3]: return
    c = p[0]; me = side(p, c); op = side(p, 1 - c)
    if 3 in me[0] and 4 in op[0]:
        yield mk(c, (add(rm(me[0], 3), 2), me[1], me[2]), (rm(op[0], 4), op[1], op[2]))

def child_A(A, k): return A if k in ('S', 'D', 'Q') else FRESH
def allowed(P, q, quiet): return inv(q) <= P or (quiet and q == L and P == CW)
def live(p): return total(p) >= 3

def unreadable(p, mode, cls):
    if mode == 'LO': return win_class(cls)
    if mode in ('N', 'A'): return False
    return mode == p[0] and not sym(p)

def with_slot(A, i):
    """Options of slot i: the assigned value, or every value when unassigned."""
    if A[i] is not None: return [(A[i], A)]
    return [(o, A[:i] + (o,) + A[i + 1:]) for o in OPTS[i]]

def dp_classes(y, P, win_only_L=False):
    """Stored classes a double-push child can have. With a live en-passant capture the
       stored class is free (the capture may be the only move); otherwise it is the
       child's class, constrained as any child."""
    if any(live(z) for z in ep_caps(y)): return range(5)
    if win_only_L: return [L] if allowed(P, L, False) else []
    return [q for q in range(5) if allowed(P, q, False)]

GUARDED = {'WD', 'RB', 'ZD', 'DM', 'D50L', 'D50F', 'DC'}

# ---------- outgoing calls of each function ----------
def c_WI(p, A):
    for o, A2 in with_slot(A, IWDL):
        if o != 'N' and o == p[0] and not sym(p):
            if side(p, p[0])[1] == 0: yield 0, 'WD', (p, A2)
        else:
            yield 0, 'RB', (p,)

def c_RB(p):
    for y, k in moves(p):
        if k == 'R' and live(y): yield 1, 'PWI', (y, FRESH)

def c_RSt(p, A):
    yield 0, 'RB', (p,)

def c_PWI(p, A):
    yield 0, 'WI', (p, A)
    for z in ep_caps(p):
        if live(z): yield 1, 'WI', (z, FRESH)

def c_WD(p, A):
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        if k in ('R', 'S', 'D'): yield 1, 'PWI', (y, A2)
        else: yield 1, 'RSt', (y, A2)

def c_PDZ(p, cls, A):
    for o, A2 in with_slot(A, IDTZ):
        if unreadable(p, o, cls): yield 0, 'ZD', (p, cls, True, A2)
        elif o != 'LO' and win_class(cls): yield 0, 'ZD', (p, cls, False, A2)

def c_ZD(p, P, full, A):
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        if k in ('R', 'S', 'D'):
            if P != L: yield 1, 'PWI', (y, A2)
        elif full:
            if P == L:
                yield 1, 'PDZ', (y, W, A2)
            else:
                yield 1, 'WI', (y, A2)
                for q in range(5):
                    if q == D or not allowed(P, q, True): continue
                    lift = BL if inv(q) == L else inv(q)
                    if lift < P: continue
                    yield 1, 'PDZ', (y, q, A2)

def c_PDM(p, cls, A):
    for o, A2 in with_slot(A, IDTM):
        if unreadable(p, o, cls): yield 0, 'DM', (p, cls, A2)

def c_DM(p, P, A):
    pinned = fold(P)
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        quiet = k in ('Q', 'E')
        if k == 'D':
            for q in dp_classes(y, P):
                yield 1, 'PI', (y, q, A2, False)
            continue
        if pinned == L: qs = [W]
        else:
            yield 1, 'WI', (y, A2)
            qs = [q for q in (L, BL) if allowed(P, q, quiet)]
        for q in qs: yield 1, 'PDM', (y, q, A2)

def c_PD50(p, cls, A, flat):
    for o, A2 in with_slot(A, ID50):
        if o != 'A' and unreadable(p, o, cls):
            yield 0, ('D50F' if flat else 'D50L'), (p, cls, A2)

def c_D50L(p, P, A):
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        if k == 'D':
            for q in dp_classes(y, P):
                yield 1, 'PI', (y, q, A2, True)
            continue
        if P == L: q = W
        else:
            yield 1, 'WI', (y, A2)
            q = L
        if allowed(P, q, k in ('Q', 'E')): yield 1, 'PD50', (y, q, A2, False)

def c_D50F(p, P, A):
    pinned = fold(P)
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        quiet = k in ('Q', 'E')
        if k == 'D':
            for q in dp_classes(y, P):
                yield 1, 'PI', (y, q, A2, False)
            continue
        if P == L: qs = [W]
        else:
            yield 1, 'WI', (y, A2)
            qs = [q for q in range(5) if allowed(P, q, quiet) and inv(fold(q)) >= pinned]
        for q in qs: yield 1, 'PD50', (y, q, A2, True)

def c_PDC(p, cls, A):
    if not pawnful(p): return
    for o, A2 in with_slot(A, IDTC):
        if unreadable(p, o, cls): yield 0, 'DC', (p, cls, A2)

def c_DC(p, P, A):
    for y, k in moves(p):
        if not live(y): continue
        A2 = child_A(A, k)
        conv = k == 'R'
        quiet = k in ('Q', 'E')
        if P == W:
            yield 1, 'PWI', (y, A2)
            if conv: continue
            if k == 'D':
                for q in dp_classes(y, P, True): yield 1, 'PI', (y, q, A2, True)
            else: yield 1, 'PDC', (y, L, A2)
        elif P == L:
            if k == 'D':
                for z in ep_caps(y):
                    if live(z): yield 2, 'PWI', (z, FRESH)
        else:
            yield 1, 'PWI', (y, A2)
            if not quiet: continue
            for q in ((L, BL) if P == CW else (W, CW)):
                if allowed(P, q, True): yield 1, 'PDC', (y, q, A2)

def c_PI(p, P, A, layered):
    yield 0, 'WI', (p, A)
    if P != D:
        for o, A2 in with_slot(A, ID50):
            # A readable DTM50 cell of a clean class supplies DTZ through its draw flip.
            dtz_known = False
            if o == 'A':
                yield 0, 'PDM', (p, P, A2)
            else:
                yield 0, 'PD50', (p, P, A2, True)
                if layered and P in (W, L): yield 0, 'PD50', (p, P, A2, False)
                if not unreadable(p, o, P) and P in (W, L): dtz_known = True
            if not pawnful(p):
                if not dtz_known: yield 0, 'PDZ', (p, P, A2)
                continue
            # A readable DTC cell supplies DTZ; a derived one may refuse; no file supplies none.
            for oc, A3 in with_slot(A2, IDTC):
                known = dtz_known
                if oc != 'A':
                    if unreadable(p, oc, P): yield 0, 'DC', (p, P, A3)
                    else: known = True
                if not known: yield 0, 'PDZ', (p, P, A3)
    for z in ep_caps(p):
        if live(z):
            for q in range(5): yield 1, 'PI', (z, q, FRESH, True)

CALLS = {k[2:]: v for k, v in globals().items() if k.startswith('c_')}
MEMO = {}
ONSTACK = set()
WDLW = {'WI', 'RSt', 'PWI', 'WD'}   # RB takes no assignment
DTZW = {'PDZ', 'ZD'}
def norm(fn, args):
    """WDL functions consult only the WDL slot, DTZ functions only WDL and DTZ."""
    a = args[-1]
    if fn in WDLW: a = (a[0], None, None, None, None)
    elif fn in DTZW: a = (a[0], a[1], None, None, None)
    else: return args
    return args[:-1] + (a,)
def val(fn, args):
    args = norm(fn, args)
    key = (fn, args)
    if key in MEMO: return MEMO[key]
    if key in ONSTACK: raise RuntimeError('cycle %r' % (key,))
    ONSTACK.add(key)
    best = 0 if fn in GUARDED else NEG
    for k, f2, a2 in CALLS[fn](*args):
        v = val(f2, a2)
        if v > NEG and v + k > best: best = v + k
    ONSTACK.discard(key)
    MEMO[key] = best
    return best

def sides(m):
    for np in range(0, m + 1):
        for r in range(0, 3):
            n = m - np - r
            if n < 0: continue
            def lv(k, lo):
                if k == 0: yield (); return
                for x in range(lo, 7):
                    for rest in lv(k - 1, x): yield (x,) + rest
            for t in lv(np, 1): yield (t, r, n)

def roots(M):
    for a in range(0, M - 1):
        b = M - 2 - a
        for s0 in sides(a):
            for s1 in sides(b):
                for stm in (0, 1):
                    yield (stm, s0, s1, False)
                    if 4 in (s1 if stm == 0 else s0)[0]: yield (stm, s0, s1, True)

if __name__ == '__main__':
    for M in range(3, 9):
        print(M, max(val('PI', (p, P, FRESH, True)) for p in roots(M) for P in range(5)))
```

## Appendix B. Cursed DTZ invariant

This program checks the local steps that the proof of Theorem 12.15 composes by induction on the nest, for every true value up to 2045 and every twin value an exit reads up to 2044 (RANGE, Section 13.1):

- every read the prober meets is within the invariant, odd when rounded and exact when not, and composes exactly across an exit; a non-exit move keeps its error, a step checked over reads only, since a non-exit child is always read (Theorem 12.15);
- an exit keeps the invariant, overshoots only over a rounded derived value, and never composes the ILLEGAL code 0x7FF;
- an optimum, minimum or maximum, keeps the invariant, is exact over exact options, and is not above the true optimum when no option overshoots;
- two probes that report opposite endpoints have an option of the true optimum's value with opposite errors, which at an exit forces the same at an odd child one exit deeper;
- a read and a derivation of one position never sit at opposite endpoints.

Options more than three plies apart cannot interact, so a window around each value covers every configuration. The program prints ok.

```python
# Local steps of the cursed DTZ invariant of Theorem 12.15, checked for every true value
# up to 2045, and every twin value an exit reads up to 2044 (RANGE). A reported value is
# y = x + d for the true value x. The invariant (inv) is d in {-1, 0, 1} with d = 1 only
# when x is odd; an exact value has d = 0. A rounded read is odd, with d <= 0. f is the
# solver's exit (Definition 5.19).
TOP = 2045
EXIT_TOP = 2044
ILLEGAL = 0x7FF
def f(z): return (z + 1) | 1
def inv(x, d): return d in (-1, 0, 1) and (d < 1 or x % 2 == 1)

def exit_(y, rounded): return y + 2 if rounded else f(y)

def reads(x):
    """(reported, rounded) for every read of true value x the prober can meet."""
    yield x, False                          # two-byte DTZ: exact
    yield 2 * ((x + 1) // 2) - 1, True      # one-byte DTZ: 2s - 1 (Theorem 11.9)
    yield x, x % 2 == 1                     # row 0 from a two-byte source, flagged by parity

def states(x):
    """(d, rounded, read) for every value of true value x the invariant allows."""
    for y, rounded in reads(x): yield y - x, rounded, True
    yield 0, False, False                   # an exact derived value
    for d in (-1, 0, 1):
        if inv(x, d): yield d, True, False  # a rounded derived value

for x in range(1, TOP + 1):
    for y, rounded in reads(x):
        d = y - x
        assert inv(x, d) and d <= 0 and (d == 0 or x % 2 == 0)
        assert not rounded or y % 2 == 1                 # a rounded read is odd
        assert rounded or d == 0                         # an unrounded read is exact
        assert inv(x + 1, d)                             # a non-exit move over a read (d <= 0)

for x in range(1, EXIT_TOP + 1):
    for y, rounded in reads(x):
        assert exit_(y, rounded) == f(x)                 # an exit over any read is exact
    for d, rounded, read in states(x):
        out = exit_(x + d, rounded)
        assert out < ILLEGAL                             # never the ILLEGAL code
        e = out - f(x)
        assert inv(f(x), e)                              # exits keep the invariant
        if e == 1: assert rounded and not read           # only over rounded derived values

def opt(pick, opts):
    return pick(x for x, _ in opts), pick(x + d for x, d in opts)

# Optima: the reported optimum of options within the invariant is within it, exact when
# every option is exact, and not above the true optimum when no option overshoots.
# Options further than 3 plies apart cannot interact, so a window around each x covers all.
for x1 in range(1, TOP + 1):
    for x2 in range(max(1, x1 - 3), min(TOP, x1 + 3) + 1):
        for d1 in (-1, 0, 1):
            for d2 in (-1, 0, 1):
                if not (inv(x1, d1) and inv(x2, d2)): continue
                for pick in (min, max):
                    X, Y = opt(pick, [(x1, d1), (x2, d2)])
                    assert inv(X, Y - X)
                    if d1 == d2 == 0: assert Y == X
                    if max(d1, d2) <= 0: assert Y <= X

# Two probes A and B of one option set. If they report X - 1 and X + 1, some option of
# true value X is reported one short by one probe and one long by the other; and at an
# exit such a pair of outputs comes only from such a pair at an odd child one exit deeper.
for t in range(1, EXIT_TOP + 1):
    for dA in (-1, 0, 1):
        for dB in (-1, 0, 1):
            if not (inv(t, dA) and inv(t, dB)): continue
            eA, eB = exit_(t + dA, True) - f(t), exit_(t + dB, True) - f(t)
            if {eA, eB} == {-1, 1}: assert {dA, dB} == {-1, 1} and t % 2 == 1
for x1 in range(1, TOP + 1):
    for x2 in range(max(1, x1 - 3), min(TOP, x1 + 3) + 1):
        for a1 in (-1, 0, 1):
            for b1 in (-1, 0, 1):
                for a2 in (-1, 0, 1):
                    for b2 in (-1, 0, 1):
                        if not all(inv(x, d) for x, d in ((x1, a1), (x1, b1), (x2, a2), (x2, b2))):
                            continue
                        for pick in (min, max):
                            X, YA = opt(pick, [(x1, a1), (x2, a2)])
                            _, YB = opt(pick, [(x1, b1), (x2, b2)])
                            if {YA - X, YB - X} == {-1, 1}:
                                assert any(x == X and {a, b} == {-1, 1}
                                           for x, a, b in ((x1, a1, b1), (x2, a2, b2)))
# A read at the root beside a derivation: a read is short only at an even value, a
# derivation long only at an odd one, so they never sit at opposite endpoints.
for x in range(1, TOP + 1):
    for y, _ in reads(x):
        for d in (-1, 0, 1):
            if inv(x, d): assert abs((x + d) - y) <= 1
print("ok")
```
