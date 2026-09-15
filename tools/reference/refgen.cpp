#include "refgen.h"
#include "retro.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace ref {

namespace {

constexpr char TYPE_LETTER[] = " PNBRQK";

// Working markers, never visible outside a solve.
constexpr Wdl UNDECIDED = static_cast<Wdl>(6);
constexpr uint16_t DTM_UNDECIDED = 0xFFFF;
constexpr uint8_t DTC_UNDECIDED = 255;
constexpr uint32_t MAX_DISTANCE = 4000;

Wdl invert(Wdl w)
{
	switch (w)
	{
		case Wdl::LOSE:         return Wdl::WIN;
		case Wdl::BLESSED_LOSS: return Wdl::CURSED_WIN;
		case Wdl::CURSED_WIN:   return Wdl::BLESSED_LOSS;
		case Wdl::WIN:          return Wdl::LOSE;
		default:                return w;
	}
}

bool winning(Wdl w) { return w == Wdl::WIN || w == Wdl::CURSED_WIN; }
bool losing(Wdl w) { return w == Wdl::LOSE || w == Wdl::BLESSED_LOSS; }

// A DTZ option: a class and the plies to the zeroing move that fixes it.
struct Dtz_Option
{
	Wdl w = Wdl::LOSE;
	uint16_t d = 0;
};

// Class first; within a winning class the shorter distance, within a losing
// class the longer. Draws are equal whatever their distance.
bool better(const Dtz_Option& a, const Dtz_Option& b)
{
	if (a.w != b.w) return static_cast<int>(a.w) > static_cast<int>(b.w);
	if (winning(a.w)) return a.d < b.d;
	if (losing(a.w)) return a.d > b.d;
	return false;
}

bool better(const Outcome& a, const Outcome& b)
{
	if (a.kind != b.kind) return a.kind > b.kind;
	if (a.kind == Outcome::WIN) return a.plies < b.plies;
	if (a.kind == Outcome::LOSS) return a.plies > b.plies;
	return false;
}

// The mover's view of a move into a position worth `child` to the opponent.
Outcome step_back(Outcome child)
{
	switch (child.kind)
	{
		case Outcome::WIN:  return { Outcome::LOSS, static_cast<uint16_t>(child.plies + 1) };
		case Outcome::LOSS: return { Outcome::WIN, static_cast<uint16_t>(child.plies + 1) };
		default:            return {};
	}
}

template <typename F>
void parallel_for(int threads, uint64_t n, F&& f)
{
	if (n == 0) return;
	if (threads <= 1 || n < 4096)
	{
		f(0, uint64_t{ 0 }, n);
		return;
	}
	const uint64_t chunk = std::max<uint64_t>(512, n / (static_cast<uint64_t>(threads) * 64));
	std::atomic<uint64_t> next{ 0 };
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; ++t)
		pool.emplace_back([&, t] {
			for (;;)
			{
				const uint64_t begin = next.fetch_add(chunk);
				if (begin >= n) return;
				f(t, begin, std::min(n, begin + chunk));
			}
		});
	for (auto& th : pool) th.join();
}

std::string format(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	return buf;
}

// Squares of the a1-d1-d4 triangle, numbered 0 to 9.
struct Triangle
{
	int code[64];
	int square[10];
	Triangle()
	{
		for (int& c : code) c = -1;
		int n = 0;
		for (int r = 0; r < 4; ++r)
			for (int f = r; f < 4; ++f)
			{
				code[make_square(f, r)] = n;
				square[n++] = make_square(f, r);
			}
	}
};

const Triangle& triangle()
{
	static const Triangle t;
	return t;
}

// Bit 0 mirrors files, bit 1 mirrors ranks, bit 2 then swaps files and ranks.
int transform_square(int transform, int sq)
{
	int f = file_of(sq), r = rank_of(sq);
	if (transform & 1) f = 7 - f;
	if (transform & 2) r = 7 - r;
	if (transform & 4) std::swap(f, r);
	return make_square(f, r);
}

}  // namespace

// ------------------------------------------------------------------ materials

bool Material::parse(const std::string& name, Material& out, std::string* error)
{
	auto fail = [&](const char* why) {
		if (error) *error = std::string(why) + ": " + name;
		return false;
	};
	Material m;
	int side = -1;
	int pairs = 0;
	for (char c : name)
	{
		if (c == 'K')
		{
			if (++side > 1) return fail("more than two kings");
			continue;
		}
		if (side < 0) return fail("a material starts with the white king");
		const Color col = static_cast<Color>(side);
		switch (c)
		{
			case 'Q': ++m.counts[col][QUEEN]; break;
			case 'R': ++m.counts[col][ROOK]; break;
			case 'B': ++m.counts[col][BISHOP]; break;
			case 'N': ++m.counts[col][KNIGHT]; break;
			case 'P': ++m.counts[col][PAWN]; break;
			case 'p': ++m.counts[col][PAWN]; ++pairs; break;
			case 'r': ++m.counts[col][ROOK]; ++m.rights[col]; break;
			default: return fail("unknown piece letter");
		}
	}
	if (side != 1) return fail("a material needs both kings");
	if (m.rights[WHITE] > 2 || m.rights[BLACK] > 2) return fail("at most two castling rights per side");
	if (pairs != 0 && pairs != 2) return fail("a pair has one pawn on each side");
	out = m;
	return true;
}

std::string Material::name() const
{
	std::string s;
	for (Color c : { WHITE, BLACK })
	{
		s += 'K';
		for (Piece_Type t : { QUEEN, ROOK, BISHOP, KNIGHT, PAWN })
		{
			int n = counts[c][t];
			if (t == ROOK)
			{
				s.append(rights[c], 'r');
				n -= rights[c];
			}
			s.append(static_cast<size_t>(n), TYPE_LETTER[t]);
		}
	}
	return s;
}

Material Material::of(const Board& b)
{
	Material m;
	for (int sq = 0; sq < 64; ++sq)
	{
		const Piece p = b.squares[sq];
		if (p == NO_PIECE || type_of(p) == KING) continue;
		++m.counts[color_of(p)][type_of(p)];
	}
	for (Color c : { WHITE, BLACK })
		m.rights[c] = static_cast<uint8_t>((b.castle[c][A_SIDE] != NO_SQUARE) + (b.castle[c][H_SIDE] != NO_SQUARE));
	return m;
}

int Material::men() const
{
	int n = 2;
	for (const auto& side : counts)
		for (uint8_t v : side) n += v;
	return n;
}

bool Material::has_pawns() const
{
	return counts[WHITE][PAWN] + counts[BLACK][PAWN] > 0;
}

bool Material::bare_kings() const
{
	return men() == 2;
}

uint64_t Material::key() const
{
	uint64_t k = 0;
	for (Color c : { WHITE, BLACK })
		for (Piece_Type t : { PAWN, KNIGHT, BISHOP, ROOK, QUEEN })
			k = k * 16 + counts[c][t];
	return k * 9 + rights[WHITE] * 3 + rights[BLACK];
}

// --------------------------------------------------------------------- layout

Layout::Layout(const Material& m) : material(m)
{
	symmetry = m.has_rights() ? Symmetry::NONE : m.has_pawns() ? Symmetry::FILE_MIRROR : Symmetry::DIHEDRAL;

	groups.push_back({ WHITE, KING, m.rights[WHITE] > 0, 1 });
	groups.push_back({ BLACK, KING, m.rights[BLACK] > 0, 1 });
	for (Color c : { WHITE, BLACK })
		for (Piece_Type t : { QUEEN, ROOK, BISHOP, KNIGHT })
		{
			int n = m.counts[c][t];
			if (t == ROOK && m.rights[c] > 0)
			{
				groups.push_back({ c, ROOK, true, m.rights[c] });
				n -= m.rights[c];
			}
			if (n > 0) groups.push_back({ c, t, false, n });
		}
	for (Color c : { WHITE, BLACK })
		if (m.counts[c][PAWN] > 0)
		{
			groups.push_back({ c, PAWN, false, m.counts[c][PAWN] });
			pawn_count += m.counts[c][PAWN];
		}

	within_slice = 1;
	slices = 1;
	for (size_t g = 0; g < groups.size(); ++g)
		for (int i = 0; i < groups[g].count; ++i)
		{
			if (groups[g].type == PAWN) slices *= 48;
			else if (groups[g].holds_right) within_slice *= 8;
			else if (g == 0) within_slice *= symmetry == Symmetry::DIHEDRAL ? 10 : symmetry == Symmetry::FILE_MIRROR ? 32 : 64;
			else within_slice *= 64;
		}
}

namespace {

int domain_of(const Layout& l, size_t g)
{
	const Layout::Group& grp = l.groups[g];
	if (grp.type == PAWN) return 48;
	if (grp.holds_right) return 8;
	if (g == 0) return l.symmetry == Symmetry::DIHEDRAL ? 10 : l.symmetry == Symmetry::FILE_MIRROR ? 32 : 64;
	return 64;
}

int code_of(const Layout& l, size_t g, int sq)
{
	const Layout::Group& grp = l.groups[g];
	if (grp.type == PAWN) return (rank_of(sq) >= 1 && rank_of(sq) <= 6) ? sq - 8 : -1;
	if (grp.holds_right) return rank_of(sq) == home_rank(grp.color) ? file_of(sq) : -1;
	if (g == 0 && l.symmetry == Symmetry::DIHEDRAL) return triangle().code[sq];
	if (g == 0 && l.symmetry == Symmetry::FILE_MIRROR) return file_of(sq) <= 3 ? rank_of(sq) * 4 + file_of(sq) : -1;
	return sq;
}

int square_of(const Layout& l, size_t g, int code)
{
	const Layout::Group& grp = l.groups[g];
	if (grp.type == PAWN) return code + 8;
	if (grp.holds_right) return make_square(code, home_rank(grp.color));
	if (g == 0 && l.symmetry == Symmetry::DIHEDRAL) return triangle().square[code];
	if (g == 0 && l.symmetry == Symmetry::FILE_MIRROR) return make_square(code % 4, code / 4);
	return code;
}

}  // namespace

bool Layout::decode(uint64_t entry, Board& b) const
{
	b.clear();
	b.stm = (entry & 1) ? BLACK : WHITE;
	uint64_t rest = entry / 2;
	uint64_t pawn_part = rest / within_slice;
	uint64_t piece_part = rest % within_slice;

	std::array<int, 32> squares{};
	std::array<size_t, 32> group_of{};
	int n = 0;
	for (size_t g = 0; g < groups.size(); ++g)
		for (int i = 0; i < groups[g].count; ++i)
			group_of[n++] = g;

	for (int i = n; i-- > 0;)
	{
		const size_t g = group_of[i];
		const int d = domain_of(*this, g);
		int code;
		if (groups[g].type == PAWN)
		{
			code = static_cast<int>(pawn_part % 48);
			pawn_part /= 48;
		}
		else
		{
			code = static_cast<int>(piece_part % static_cast<uint64_t>(d));
			piece_part /= static_cast<uint64_t>(d);
		}
		squares[i] = square_of(*this, g, code);
	}

	for (int i = 0; i < n; ++i)
	{
		if (b.squares[squares[i]] != NO_PIECE) return false;
		b.squares[squares[i]] = make_piece(groups[group_of[i]].color, groups[group_of[i]].type);
	}

	for (Color c : { WHITE, BLACK })
	{
		if (material.rights[c] == 0) continue;
		const int ksq = b.king_square(c);
		std::vector<int> rooks;
		for (int i = 0; i < n; ++i)
			if (groups[group_of[i]].color == c && groups[group_of[i]].type == ROOK && groups[group_of[i]].holds_right)
				rooks.push_back(squares[i]);
		std::sort(rooks.begin(), rooks.end());
		if (rooks.size() == 1)
		{
			b.castle[c][file_of(rooks[0]) > file_of(ksq) ? H_SIDE : A_SIDE] = static_cast<int8_t>(rooks[0]);
		}
		else
		{
			if (!(file_of(rooks[0]) < file_of(ksq) && file_of(ksq) < file_of(rooks[1]))) return false;
			b.castle[c][A_SIDE] = static_cast<int8_t>(rooks[0]);
			b.castle[c][H_SIDE] = static_cast<int8_t>(rooks[1]);
		}
	}
	return true;
}

uint64_t Layout::entry_under(const Board& b, int transform) const
{
	std::array<std::array<int, 16>, 16> sq{};
	std::array<int, 16> seen{};
	for (int s = 0; s < 64; ++s)
	{
		const Piece p = b.squares[s];
		if (p == NO_PIECE) continue;
		const Color c = color_of(p);
		const Piece_Type t = type_of(p);
		bool holds = false;
		if (t == KING) holds = material.rights[c] > 0;
		else if (t == ROOK) holds = b.castle[c][A_SIDE] == s || b.castle[c][H_SIDE] == s;
		size_t g = 0;
		while (g < groups.size() && !(groups[g].color == c && groups[g].type == t && groups[g].holds_right == holds)) ++g;
		if (g == groups.size() || seen[g] >= groups[g].count) return UINT64_MAX;
		sq[g][seen[g]++] = transform_square(transform, s);
	}

	uint64_t piece_part = 0, pawn_part = 0;
	for (size_t g = 0; g < groups.size(); ++g)
	{
		if (seen[g] != groups[g].count) return UINT64_MAX;
		std::sort(sq[g].begin(), sq[g].begin() + seen[g]);
		for (int i = 0; i < seen[g]; ++i)
		{
			const int code = code_of(*this, g, sq[g][i]);
			if (code < 0) return UINT64_MAX;
			if (groups[g].type == PAWN) pawn_part = pawn_part * 48 + static_cast<uint64_t>(code);
			else piece_part = piece_part * static_cast<uint64_t>(domain_of(*this, g)) + static_cast<uint64_t>(code);
		}
	}
	return (pawn_part * within_slice + piece_part) * 2 + (b.stm == BLACK ? 1 : 0);
}

uint64_t Layout::canonical_entry(const Board& b) const
{
	if (symmetry == Symmetry::NONE) return entry_under(b, 0);

	const int wk = b.king_square(WHITE);
	if (wk == NO_SQUARE) return UINT64_MAX;
	int f = file_of(wk), r = rank_of(wk);

	if (symmetry == Symmetry::FILE_MIRROR) return entry_under(b, f > 3 ? 1 : 0);

	int t = 0;
	if (f > 3) { t |= 1; f = 7 - f; }
	if (r > 3) { t |= 2; r = 7 - r; }
	if (r > f) { t |= 4; std::swap(f, r); }
	const uint64_t e = entry_under(b, t);
	if (f != r) return e;
	return std::min(e, entry_under(b, t ^ 4));
}

int Layout::slice_advancement(uint64_t slice) const
{
	int total = 0;
	for (size_t g = groups.size(); g-- > 0;)
	{
		if (groups[g].type != PAWN) continue;
		for (int i = 0; i < groups[g].count; ++i)
		{
			const int sq = static_cast<int>(slice % 48) + 8;
			slice /= 48;
			total += groups[g].color == WHITE ? rank_of(sq) : 7 - rank_of(sq);
		}
	}
	return total;
}

// ----------------------------------------------------------------- generator

std::vector<Material> Generator::successors(const Material& m)
{
	std::set<uint64_t> seen;
	std::vector<Material> out;
	auto add_with_rights = [&](Material x) {
		for (Color c : { WHITE, BLACK })
			x.rights[c] = std::min<uint8_t>(x.rights[c], x.counts[c][ROOK]);
		// Any move may also cost rights; take every smaller combination.
		for (int wr = 0; wr <= x.rights[WHITE]; ++wr)
			for (int br = 0; br <= x.rights[BLACK]; ++br)
			{
				Material y = x;
				y.rights = { static_cast<uint8_t>(wr), static_cast<uint8_t>(br) };
				if (y.bare_kings() || y == m) continue;
				if (seen.insert(y.key()).second) out.push_back(y);
			}
	};

	add_with_rights(m);
	for (Color victim : { WHITE, BLACK })
	{
		const Color captor = opposite(victim);
		for (Piece_Type t : { PAWN, KNIGHT, BISHOP, ROOK, QUEEN })
		{
			if (m.counts[victim][t] == 0) continue;
			Material x = m;
			--x.counts[victim][t];
			add_with_rights(x);
			// A capturing pawn may promote at the same time.
			if (m.counts[captor][PAWN] > 0 && t != PAWN)
				for (Piece_Type promo : { KNIGHT, BISHOP, ROOK, QUEEN })
				{
					Material y = x;
					--y.counts[captor][PAWN];
					++y.counts[captor][promo];
					add_with_rights(y);
				}
		}
	}
	for (Color c : { WHITE, BLACK })
		if (m.counts[c][PAWN] > 0)
			for (Piece_Type promo : { KNIGHT, BISHOP, ROOK, QUEEN })
			{
				Material x = m;
				--x.counts[c][PAWN];
				++x.counts[c][promo];
				add_with_rights(x);
			}
	return out;
}

struct Generator::Impl
{
	Generator_Options opt;
	std::unordered_map<uint64_t, std::unique_ptr<Table>> tables;
	mutable std::atomic<uint64_t> verify_failures{ 0 };
	mutable std::atomic<uint64_t> anomalies{ 0 };
	std::set<uint64_t> reported;   // materials already passed to a sink's on_table
	Sink* sink = nullptr;

	const Table* find(const Material& m) const
	{
		const auto it = tables.find(m.key());
		return it == tables.end() ? nullptr : it->second.get();
	}

	void log(const std::string& s) { if (sink) sink->on_log(s); }

	// ------------------------------------------------------------ children

	struct Child
	{
		const Table* table = nullptr;   // nullptr: bare kings
		uint64_t entry = 0;
		bool zeroing = false;           // a capture or a pawn move
		bool conversion = false;        // a capture or a promotion
		bool pawn_move = false;
	};

	Child child_of(const Table& t, const Board& b, const Move& m, Board& after) const
	{
		after = b;
		after.make(m);
		Child c;
		c.pawn_move = type_of(b.squares[m.from]) == PAWN;
		c.conversion = m.is_capture() || m.promotion != NO_TYPE;
		c.zeroing = c.pawn_move || m.is_capture();
		if (!c.conversion && after.castle == b.castle)
		{
			c.table = &t;
		}
		else
		{
			const Material mm = Material::of(after);
			if (mm.bare_kings()) return c;
			c.table = find(mm);
			if (!c.table)
			{
				std::fprintf(stderr, "internal error: %s reaches %s, which was not built\n",
					t.layout.material.name().c_str(), mm.name().c_str());
				std::abort();
			}
		}
		c.entry = c.table->layout.canonical_entry(after);
		if (c.entry == UINT64_MAX)
		{
			std::fprintf(stderr, "internal error: no entry for %s in %s\n", after.fen().c_str(),
				c.table->layout.material.name().c_str());
			std::abort();
		}
		return c;
	}

	// The opponent's en passant replies to a double push, as values of positions
	// after the capture, each passed to `visit` with the table and entry.
	template <typename F>
	void for_each_ep_reply(const Board& after_push, F&& visit) const
	{
		if (after_push.ep == NO_SQUARE) return;
		Move moves[MAX_MOVES];
		const int n = after_push.legal_moves(moves);
		for (int i = 0; i < n; ++i)
		{
			if (!moves[i].is_ep()) continue;
			Board g = after_push;
			g.make(moves[i]);
			const Material gm = Material::of(g);
			if (gm.bare_kings())
			{
				visit(nullptr, uint64_t{ 0 });
				continue;
			}
			const Table* gt = find(gm);
			visit(gt, gt->layout.canonical_entry(g));
		}
	}

	// Class of the position after a double push for the side that may now take
	// en passant: its best of the plain value and every capture.
	Wdl class_after_double_push(const Board& after_push, Wdl plain) const
	{
		Wdl best = plain;
		for_each_ep_reply(after_push, [&](const Table* gt, uint64_t ge) {
			const Wdl mine = invert(gt ? gt->wdl[ge] : Wdl::DRAW);
			if (static_cast<int>(mine) > static_cast<int>(best)) best = mine;
		});
		return best;
	}

	template <typename Read>
	Outcome outcome_after_double_push(const Board& after_push, Outcome plain, Read&& read) const
	{
		Outcome best = plain;
		for_each_ep_reply(after_push, [&](const Table* gt, uint64_t ge) {
			const Outcome mine = step_back(gt ? read(*gt, ge) : Outcome{});
			if (better(mine, best)) best = mine;
		});
		return best;
	}

	// --------------------------------------------------------------- scheduling

	struct Verdict
	{
		bool qualifies = false;
		uint32_t dist = 0;
		uint16_t payload = 0;
	};

	template <typename F>
	void for_each_entry(const Table& t, const std::vector<uint64_t>& slices, F&& f) const
	{
		const uint64_t per = t.layout.slice_entries();
		parallel_for(opt.threads, slices.size() * per, [&](int thread, uint64_t begin, uint64_t end) {
			for (uint64_t i = begin; i < end; ++i)
			{
				const uint64_t e = slices[i / per] * per + i % per;
				if (t.wdl[e] != Wdl::INVALID) f(thread, e);
			}
		});
	}

	template <typename F>
	void quiet_predecessors(const Table& t, uint64_t e, F&& f) const
	{
		Board b;
		t.layout.decode(e, b);
		for_each_quiet_unmove(b, [&](int now, int before) {
			const Piece p = b.squares[now];
			const Color c = color_of(p);
			if (type_of(p) == KING && b.has_rights(c)) return;
			if (type_of(p) == ROOK && (b.castle[c][A_SIDE] == now || b.castle[c][H_SIDE] == now)) return;
			Board prev = b;
			prev.squares[before] = p;
			prev.squares[now] = NO_PIECE;
			prev.stm = opposite(b.stm);
			const uint64_t pe = t.layout.canonical_entry(prev);
			if (pe != UINT64_MAX && t.wdl[pe] != Wdl::INVALID) f(pe);
		});
	}

	// Settle the entries of one batch in increasing distance. `assess(e)` says
	// whether e would settle given what is settled now, and at what distance;
	// an entry is written only in the pass matching that distance, after the
	// whole pass has been assessed, so every value read is from an earlier pass.
	template <typename Undecided, typename Assess, typename Settle>
	uint64_t settle_batch(const Table& t, const std::vector<uint64_t>& slices, uint32_t max_dist,
	                      Undecided&& undecided, Assess&& assess, Settle&& settle)
	{
		const int threads = std::max(1, opt.threads);
		std::vector<std::vector<uint64_t>> buckets;
		auto add_later = [&](std::vector<std::vector<std::pair<uint32_t, uint64_t>>>& later) {
			for (auto& list : later)
			{
				for (const auto& [d, e] : list)
				{
					if (d > max_dist) continue;
					if (d >= buckets.size()) buckets.resize(d + 1);
					buckets[d].push_back(e);
				}
				list.clear();
			}
		};

		{
			std::vector<std::vector<std::pair<uint32_t, uint64_t>>> later(threads);
			for_each_entry(t, slices, [&](int thread, uint64_t e) {
				if (!undecided(e)) return;
				const Verdict v = assess(e);
				if (v.qualifies && v.dist >= 1) later[thread].push_back({ v.dist, e });
			});
			add_later(later);
		}

		uint64_t settled_total = 0;
		std::vector<uint64_t> dirty;
		for (uint32_t d = 1; d <= max_dist; ++d)
		{
			std::vector<uint64_t> cand;
			if (d < buckets.size()) cand.swap(buckets[d]);
			cand.insert(cand.end(), dirty.begin(), dirty.end());
			dirty.clear();
			if (cand.empty())
			{
				bool pending = false;
				for (size_t i = d + 1; i < buckets.size() && !pending; ++i) pending = !buckets[i].empty();
				if (!pending) break;
				continue;
			}
			std::sort(cand.begin(), cand.end());
			cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

			std::vector<std::vector<std::pair<uint64_t, uint16_t>>> now(threads);
			std::vector<std::vector<std::pair<uint32_t, uint64_t>>> later(threads);
			parallel_for(threads, cand.size(), [&](int thread, uint64_t begin, uint64_t end) {
				for (uint64_t i = begin; i < end; ++i)
				{
					const uint64_t e = cand[i];
					if (!undecided(e)) continue;
					const Verdict v = assess(e);
					if (!v.qualifies) continue;
					if (v.dist == d) now[thread].push_back({ e, v.payload });
					else if (v.dist > d) later[thread].push_back({ v.dist, e });
					else ++anomalies;
				}
			});

			std::vector<uint64_t> settled;
			for (auto& list : now)
				for (const auto& [e, payload] : list)
				{
					settle(e, d, payload);
					settled.push_back(e);
				}
			settled_total += settled.size();
			add_later(later);

			std::vector<std::vector<uint64_t>> preds(threads);
			parallel_for(threads, settled.size(), [&](int thread, uint64_t begin, uint64_t end) {
				for (uint64_t i = begin; i < end; ++i)
					quiet_predecessors(t, settled[i], [&](uint64_t p) {
						if (undecided(p)) preds[thread].push_back(p);
					});
			});
			for (auto& list : preds) dirty.insert(dirty.end(), list.begin(), list.end());
		}
		return settled_total;
	}

	// ------------------------------------------------------------- WDL and DTZ

	struct Dtz_Eval
	{
		Dtz_Option best;
		bool have = false;
		bool unknown = false;
	};

	Dtz_Eval eval_dtz(const Table& t, const Board& b) const
	{
		Move moves[MAX_MOVES];
		const int n = b.legal_moves(moves);
		Dtz_Eval r;
		Board after;
		for (int i = 0; i < n; ++i)
		{
			const Child c = child_of(t, b, moves[i], after);
			Dtz_Option o;
			if (!c.table)
			{
				o = { Wdl::DRAW, 1 };
			}
			else
			{
				Wdl cw = c.table->wdl[c.entry];
				if (cw == UNDECIDED)
				{
					if (c.zeroing) ++anomalies;
					r.unknown = true;
					continue;
				}
				if (c.zeroing)
				{
					if (moves[i].is_double_push()) cw = class_after_double_push(after, cw);
					o = { invert(cw), 1 };
				}
				else if (cw == Wdl::DRAW)
				{
					o = { Wdl::DRAW, 0 };
				}
				else
				{
					o.d = static_cast<uint16_t>(c.table->dtz[c.entry] + 1);
					o.w = invert(cw);
					if (o.d > MAX_CLEAN_PLIES)
					{
						if (o.w == Wdl::WIN) o.w = Wdl::CURSED_WIN;
						if (o.w == Wdl::LOSE) o.w = Wdl::BLESSED_LOSS;
					}
				}
			}
			if (!r.have || better(o, r.best)) r.best = o;
			r.have = true;
		}
		return r;
	}

	void solve_dtz(Table& t, const std::vector<std::vector<uint64_t>>& batches)
	{
		for (const auto& slices : batches)
		{
			// Mate and stalemate first; they need no children.
			for_each_entry(t, slices, [&](int, uint64_t e) {
				Board b;
				t.layout.decode(e, b);
				Move moves[MAX_MOVES];
				if (b.legal_moves(moves) != 0) return;
				t.wdl[e] = b.in_check(b.stm) ? Wdl::LOSE : Wdl::DRAW;
				t.dtz[e] = 0;
			});

			auto undecided = [&](uint64_t e) { return t.wdl[e] == UNDECIDED; };
			auto settle = [&](uint64_t e, uint32_t d, uint16_t w) {
				t.wdl[e] = static_cast<Wdl>(w);
				t.dtz[e] = static_cast<uint16_t>(d);
			};

			// Clean wins and losses, within 100 plies of their zeroing move.
			settle_batch(t, slices, MAX_CLEAN_PLIES, undecided, [&](uint64_t e) {
				Board b;
				t.layout.decode(e, b);
				const Dtz_Eval z = eval_dtz(t, b);
				Verdict v;
				if (!z.have) return v;
				if (z.best.w == Wdl::WIN || (!z.unknown && z.best.w == Wdl::LOSE))
					v = { true, z.best.d, static_cast<uint16_t>(z.best.w) };
				return v;
			}, settle);

			// Cursed wins and blessed losses: every clean result is now known.
			settle_batch(t, slices, MAX_DISTANCE, undecided, [&](uint64_t e) {
				Board b;
				t.layout.decode(e, b);
				const Dtz_Eval z = eval_dtz(t, b);
				Verdict v;
				if (!z.have) return v;
				if (z.best.w == Wdl::CURSED_WIN || (!z.unknown && z.best.w == Wdl::BLESSED_LOSS))
					v = { true, z.best.d, static_cast<uint16_t>(z.best.w) };
				return v;
			}, settle);

			for_each_entry(t, slices, [&](int, uint64_t e) {
				if (t.wdl[e] != UNDECIDED) return;
				t.wdl[e] = Wdl::DRAW;
				t.dtz[e] = 0;
			});
		}
	}

	// --------------------------------------------------------------------- DTM

	struct Outcome_Eval
	{
		Outcome best;
		bool have = false;
		bool unknown = false;
	};

	Outcome_Eval eval_dtm(const Table& t, const Board& b) const
	{
		Move moves[MAX_MOVES];
		const int n = b.legal_moves(moves);
		Outcome_Eval r;
		Board after;
		for (int i = 0; i < n; ++i)
		{
			const Child c = child_of(t, b, moves[i], after);
			Outcome co;
			if (c.table)
			{
				const uint16_t raw = c.table->dtm[c.entry];
				if (raw == DTM_UNDECIDED)
				{
					if (c.zeroing) ++anomalies;
					r.unknown = true;
					continue;
				}
				co = unpack(raw);
				if (moves[i].is_double_push())
					co = outcome_after_double_push(after, co, [](const Table& gt, uint64_t ge) { return unpack(gt.dtm[ge]); });
			}
			const Outcome o = step_back(co);
			if (!r.have || better(o, r.best)) r.best = o;
			r.have = true;
		}
		return r;
	}

	void solve_dtm(Table& t, const std::vector<std::vector<uint64_t>>& batches)
	{
		std::vector<uint8_t> undecided_flag(t.layout.size(), 0);
		for (const auto& slices : batches)
		{
			for_each_entry(t, slices, [&](int, uint64_t e) {
				Board b;
				t.layout.decode(e, b);
				Move moves[MAX_MOVES];
				if (b.legal_moves(moves) != 0) return;
				t.dtm[e] = b.in_check(b.stm) ? pack({ Outcome::LOSS, 0 }) : uint16_t{ 0 };
			});

			settle_batch(t, slices, MAX_DISTANCE,
				[&](uint64_t e) { return t.dtm[e] == DTM_UNDECIDED; },
				[&](uint64_t e) {
					Board b;
					t.layout.decode(e, b);
					const Outcome_Eval z = eval_dtm(t, b);
					Verdict v;
					if (!z.have) return v;
					if (z.best.kind == Outcome::WIN || (!z.unknown && z.best.kind == Outcome::LOSS))
						v = { true, z.best.plies, static_cast<uint16_t>(z.best.kind) };
					return v;
				},
				[&](uint64_t e, uint32_t d, uint16_t kind) {
					t.dtm[e] = pack({ static_cast<Outcome::Kind>(kind), static_cast<uint16_t>(d) });
				});

			for_each_entry(t, slices, [&](int, uint64_t e) {
				if (t.dtm[e] == DTM_UNDECIDED) t.dtm[e] = 0;
			});
		}
	}

	// ------------------------------------------------------------------ DTM50

	// One batch's layers for every member of a castling family, keyed by table.
	struct Layer_Set
	{
		std::vector<uint64_t> slices;
		std::unordered_map<uint64_t, uint32_t> position;   // slice -> index in slices
		std::vector<uint16_t> next;                        // halfmove clock h + 1
		std::vector<uint16_t> cur;                         // halfmove clock h
	};

	uint16_t layer_value(const std::unordered_map<const Table*, Layer_Set>& layers, const Table& t, uint64_t e) const
	{
		const Layer_Set& ls = layers.at(&t);
		const uint64_t per = t.layout.slice_entries();
		const uint64_t slice = e / per;
		return ls.next[ls.position.at(slice) * per + e % per];
	}

	Outcome eval_dtm50(const Table& t, const Board& b, int hmc,
	                   const std::unordered_map<const Table*, Layer_Set>& layers) const
	{
		Move moves[MAX_MOVES];
		const int n = b.legal_moves(moves);
		if (n == 0) return b.in_check(b.stm) ? Outcome{ Outcome::LOSS, 0 } : Outcome{};

		bool saw_win = false, saw_draw = false;
		uint16_t best_win = UINT16_MAX, worst_loss = 0;
		Board after;
		for (int i = 0; i < n; ++i)
		{
			const Child c = child_of(t, b, moves[i], after);
			Outcome co;
			if (!c.table)
			{
				co = {};
			}
			else if (c.zeroing)
			{
				co = unpack(c.table->dtm50_0[c.entry]);
				if (moves[i].is_double_push())
					co = outcome_after_double_push(after, co, [](const Table& gt, uint64_t ge) { return unpack(gt.dtm50_0[ge]); });
			}
			else if (hmc + 1 >= HMC_LAYERS)
			{
				// The move that reaches clock 100 draws, unless it mates.
				co = after.is_checkmate() ? Outcome{ Outcome::LOSS, 0 } : Outcome{};
			}
			else
			{
				co = unpack(layer_value(layers, *c.table, c.entry));
			}

			if (co.kind == Outcome::LOSS)
			{
				saw_win = true;
				best_win = std::min<uint16_t>(best_win, static_cast<uint16_t>(co.plies + 1));
			}
			else if (co.kind == Outcome::WIN)
			{
				worst_loss = std::max<uint16_t>(worst_loss, static_cast<uint16_t>(co.plies + 1));
			}
			else
			{
				saw_draw = true;
			}
		}
		if (saw_win) return { Outcome::WIN, best_win };
		if (saw_draw) return {};
		return { Outcome::LOSS, worst_loss };
	}

	std::vector<Table*> family_of(const Material& m)
	{
		std::vector<Table*> family;
		for (int total = 0; total <= m.rights[WHITE] + m.rights[BLACK]; ++total)
			for (int wr = 0; wr <= m.rights[WHITE]; ++wr)
			{
				const int br = total - wr;
				if (br < 0 || br > m.rights[BLACK]) continue;
				Material x = m;
				x.rights = { static_cast<uint8_t>(wr), static_cast<uint8_t>(br) };
				const auto it = tables.find(x.key());
				if (it != tables.end()) family.push_back(it->second.get());
			}
		return family;
	}

	std::map<int, std::vector<uint64_t>, std::greater<int>> batches_by_advancement(const Table& t) const
	{
		std::map<int, std::vector<uint64_t>, std::greater<int>> out;
		const uint64_t per = t.layout.slice_entries();
		for (uint64_t s = 0; s < t.layout.slices; ++s)
		{
			bool any = false;
			for (uint64_t off = 0; off < per && !any; ++off)
				any = t.wdl[s * per + off] != Wdl::INVALID;
			if (any) out[t.layout.slice_advancement(s)].push_back(s);
		}
		return out;
	}

	void solve_dtm50(const std::vector<Table*>& family, const Table* report, Sink& out)
	{
		std::map<int, std::map<const Table*, std::vector<uint64_t>>, std::greater<int>> plan;
		for (const Table* t : family)
			for (auto& [adv, slices] : batches_by_advancement(*t))
				plan[adv][t] = slices;

		for (auto& [adv, per_table] : plan)
		{
			std::unordered_map<const Table*, Layer_Set> layers;
			for (auto& [t, slices] : per_table)
			{
				Layer_Set& ls = layers[t];
				ls.slices = slices;
				for (uint32_t i = 0; i < slices.size(); ++i) ls.position[slices[i]] = i;
				ls.next.assign(slices.size() * t->layout.slice_entries(), 0);
				ls.cur.assign(ls.next.size(), 0);
			}

			// Without castling rights a quiet move stays in its table, and below
			// clock 98 each layer is the same function of the layer above. A layer
			// can then differ from the one above only at quiet predecessors of the
			// entries that changed between the two layers above it, so only those
			// are evaluated again. A castling family is evaluated in full.
			const bool incremental = family.size() == 1 && !family[0]->layout.material.has_rights();
			const int threads = std::max(1, opt.threads);
			std::vector<uint64_t> changed;   // entries whose value at h + 1 differs from h + 2

			for (int hmc = HMC_LAYERS - 1; hmc >= 0; --hmc)
			{
				for (Table* t : family)
				{
					auto it = layers.find(t);
					if (it == layers.end()) continue;
					Layer_Set& ls = it->second;
					const uint64_t per = t->layout.slice_entries();
					auto local = [&](uint64_t e) { return ls.position.at(e / per) * per + e % per; };
					auto solve = [&](uint64_t e) {
						Board b;
						t->layout.decode(e, b);
						ls.cur[local(e)] = pack(eval_dtm50(*t, b, hmc, layers));
					};
					auto collect = [&](std::vector<std::vector<uint64_t>>& lists) {
						changed.clear();
						for (auto& list : lists) changed.insert(changed.end(), list.begin(), list.end());
					};

					if (!incremental || hmc >= HMC_LAYERS - 2)
					{
						for_each_entry(*t, ls.slices, [&](int, uint64_t e) { solve(e); });
						if (incremental && hmc == HMC_LAYERS - 2)
						{
							std::vector<std::vector<uint64_t>> diff(threads);
							for_each_entry(*t, ls.slices, [&](int thread, uint64_t e) {
								if (ls.cur[local(e)] != ls.next[local(e)]) diff[thread].push_back(e);
							});
							collect(diff);
						}
					}
					else
					{
						std::copy(ls.next.begin(), ls.next.end(), ls.cur.begin());
						std::vector<std::vector<uint64_t>> preds(threads);
						parallel_for(threads, changed.size(), [&](int thread, uint64_t begin, uint64_t end) {
							for (uint64_t i = begin; i < end; ++i)
								quiet_predecessors(*t, changed[i], [&](uint64_t p) { preds[thread].push_back(p); });
						});
						std::vector<uint64_t> redo;
						for (auto& list : preds) redo.insert(redo.end(), list.begin(), list.end());
						std::sort(redo.begin(), redo.end());
						redo.erase(std::unique(redo.begin(), redo.end()), redo.end());

						std::vector<std::vector<uint64_t>> diff(threads);
						parallel_for(threads, redo.size(), [&](int thread, uint64_t begin, uint64_t end) {
							for (uint64_t i = begin; i < end; ++i)
							{
								const uint64_t e = redo[i];
								solve(e);
								if (ls.cur[local(e)] != ls.next[local(e)]) diff[thread].push_back(e);
							}
						});
						collect(diff);
					}
					if (hmc == 0)
						for_each_entry(*t, ls.slices, [&](int, uint64_t e) {
							t->dtm50_0[e] = ls.cur[ls.position.at(e / per) * per + e % per];
						});
					if (t == report) out.on_dtm50_layer(*t, hmc, ls.slices, ls.cur);
				}
				for (auto& [t, ls] : layers) ls.next.swap(ls.cur);
			}
		}
	}

	// -------------------------------------------------------------------- DTC

	struct Budget_Layers
	{
		std::vector<uint8_t> below;   // budget k - 1
		std::vector<uint8_t> cur;     // budget k
	};

	Outcome_Eval eval_dtc(const Table& t, const Board& b, int budget,
	                      const std::unordered_map<const Table*, Budget_Layers>& layers) const
	{
		Move moves[MAX_MOVES];
		const int n = b.legal_moves(moves);
		Outcome_Eval r;
		Board after;
		const Outcome WIN_1{ Outcome::WIN, 1 };
		const Outcome LOSS_1{ Outcome::LOSS, 1 };
		for (int i = 0; i < n; ++i)
		{
			const Child c = child_of(t, b, moves[i], after);
			Outcome o;
			if (!c.table)
			{
				o = {};
			}
			else if (c.conversion)
			{
				const Wdl cw = c.table->wdl[c.entry];
				o = cw == Wdl::LOSE ? WIN_1 : cw == Wdl::WIN ? LOSS_1 : Outcome{};
			}
			else if (c.pawn_move)
			{
				// The opponent's standing after the push: lost if it is lost with
				// one budget fewer, won if it wins at this budget, else drawn.
				const Budget_Layers& bl = layers.at(c.table);
				Outcome::Kind opp = Outcome::DRAW;
				if (budget > 0 && dtc_unpack(bl.below[c.entry]).kind == Outcome::LOSS) opp = Outcome::LOSS;
				else if (dtc_unpack(bl.cur[c.entry]).kind == Outcome::WIN) opp = Outcome::WIN;
				if (moves[i].is_double_push())
					for_each_ep_reply(after, [&](const Table* gt, uint64_t ge) {
						const Wdl gw = gt ? gt->wdl[ge] : Wdl::DRAW;
						const Outcome::Kind mine = gw == Wdl::WIN ? Outcome::LOSS : gw == Wdl::LOSE ? Outcome::WIN : Outcome::DRAW;
						if (mine > opp) opp = mine;
					});
				o = opp == Outcome::LOSS ? WIN_1 : opp == Outcome::WIN ? LOSS_1 : Outcome{};
			}
			else
			{
				const uint8_t raw = layers.at(c.table).cur[c.entry];
				if (raw == DTC_UNDECIDED)
				{
					r.unknown = true;
					continue;
				}
				o = step_back(dtc_unpack(raw));
				if (o.kind != Outcome::DRAW && o.plies > MAX_CLEAN_PLIES) o = {};
			}
			if (!r.have || better(o, r.best)) r.best = o;
			r.have = true;
		}
		return r;
	}

	void solve_dtc(const std::vector<Table*>& family, const Table* report, Sink& out)
	{
		std::unordered_map<const Table*, Budget_Layers> layers;
		std::map<int, std::map<const Table*, std::vector<uint64_t>>, std::greater<int>> plan;
		for (const Table* t : family)
		{
			layers[t].below.assign(t->layout.size(), 0);
			for (auto& [adv, slices] : batches_by_advancement(*t))
				plan[adv][t] = slices;
		}

		for (int budget = 0; budget < DTC_BUDGETS; ++budget)
		{
			for (const Table* t : family) layers[t].cur.assign(t->layout.size(), DTC_UNDECIDED);

			for (auto& [adv, per_table] : plan)
			{
				for (const Table* t : family)
				{
					const auto it = per_table.find(t);
					if (it == per_table.end()) continue;
					const std::vector<uint64_t>& slices = it->second;
					std::vector<uint8_t>& cur = layers[t].cur;

					for_each_entry(*t, slices, [&](int, uint64_t e) {
						Board b;
						t->layout.decode(e, b);
						Move moves[MAX_MOVES];
						if (b.legal_moves(moves) != 0) return;
						cur[e] = b.in_check(b.stm) ? dtc_pack({ Outcome::LOSS, 0 }) : uint8_t{ 0 };
					});

					settle_batch(*t, slices, MAX_CLEAN_PLIES,
						[&](uint64_t e) { return cur[e] == DTC_UNDECIDED; },
						[&](uint64_t e) {
							Board b;
							t->layout.decode(e, b);
							const Outcome_Eval z = eval_dtc(*t, b, budget, layers);
							Verdict v;
							if (!z.have) return v;
							if (z.best.kind == Outcome::WIN || (!z.unknown && z.best.kind == Outcome::LOSS))
								v = { true, z.best.plies, static_cast<uint16_t>(z.best.kind) };
							return v;
						},
						[&](uint64_t e, uint32_t d, uint16_t kind) {
							cur[e] = dtc_pack({ static_cast<Outcome::Kind>(kind), static_cast<uint16_t>(d) });
						});

					for_each_entry(*t, slices, [&](int, uint64_t e) {
						if (cur[e] == DTC_UNDECIDED) cur[e] = 0;
					});

					if (opt.verify)
						for_each_entry(*t, slices, [&](int, uint64_t e) {
							Board b;
							t->layout.decode(e, b);
							const Outcome_Eval z = eval_dtc(*t, b, budget, layers);
							const Outcome stored = dtc_unpack(cur[e]);
							Move moves[MAX_MOVES];
							if (b.legal_moves(moves) == 0) return;
							if (z.best != stored) ++verify_failures;
						});
				}
			}
			for (const Table* t : family) if (t == report) out.on_dtc_budget(*t, budget, layers[t].cur);
			for (const Table* t : family) layers[t].below.swap(layers[t].cur);
		}
	}

	// ----------------------------------------------------------- verification

	void verify_table(const Table& t)
	{
		std::vector<uint64_t> all;
		for (uint64_t s = 0; s < t.layout.slices; ++s) all.push_back(s);
		std::atomic<uint64_t> bad_dtz{ 0 }, bad_dtm{ 0 };
		for_each_entry(t, all, [&](int, uint64_t e) {
			Board b;
			t.layout.decode(e, b);
			Move moves[MAX_MOVES];
			if (b.legal_moves(moves) == 0)
			{
				const bool mate = b.in_check(b.stm);
				if (t.wdl[e] != (mate ? Wdl::LOSE : Wdl::DRAW) || t.dtz[e] != 0) ++bad_dtz;
				if (opt.dtm && t.dtm[e] != (mate ? pack({ Outcome::LOSS, 0 }) : 0)) ++bad_dtm;
				return;
			}
			const Dtz_Eval z = eval_dtz(t, b);
			const bool dtz_ok = t.wdl[e] == Wdl::DRAW
				? z.best.w == Wdl::DRAW
				: (z.best.w == t.wdl[e] && z.best.d == t.dtz[e]);
			if (!dtz_ok) ++bad_dtz;
			if (opt.dtm)
			{
				const Outcome_Eval m = eval_dtm(t, b);
				if (m.best != unpack(t.dtm[e])) ++bad_dtm;
			}
		});
		if (bad_dtz || bad_dtm)
			log(format("  verify %s: %llu DTZ and %llu DTM values disagree with their children",
				t.layout.material.name().c_str(), static_cast<unsigned long long>(bad_dtz.load()),
				static_cast<unsigned long long>(bad_dtm.load())));
		verify_failures += bad_dtz + bad_dtm;
	}

	// ------------------------------------------------------------------- build

	bool build_material(const Material& m, Sink& out, bool report, std::string* error)
	{
		const auto t_start = std::chrono::steady_clock::now();
		auto elapsed = [&] {
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
		};

		auto table = std::make_unique<Table>(m);
		Table& t = *table;
		const uint64_t n = t.layout.size();
		const uint64_t per_entry = sizeof(Wdl) + sizeof(uint16_t) * (1 + opt.dtm + opt.dtm50);
		const uint64_t bytes = n * per_entry + (m.has_pawns() && opt.dtc ? 2 * n : 0);
		if (bytes > opt.max_bytes)
		{
			if (error)
				*error = format("%s needs %.1f GiB, over the limit of %.1f GiB", m.name().c_str(),
					static_cast<double>(bytes) / (1 << 30), static_cast<double>(opt.max_bytes) / (1 << 30));
			return false;
		}

		t.wdl.assign(n, Wdl::INVALID);
		t.dtz.assign(n, 0);
		if (opt.dtm) t.dtm.assign(n, DTM_UNDECIDED);
		if (opt.dtm50) t.dtm50_0.assign(n, 0);

		std::vector<uint64_t> all_slices;
		for (uint64_t s = 0; s < t.layout.slices; ++s) all_slices.push_back(s);
		std::atomic<uint64_t> valid{ 0 };
		parallel_for(opt.threads, n, [&](int, uint64_t begin, uint64_t end) {
			Board b;
			for (uint64_t e = begin; e < end; ++e)
			{
				if (!t.layout.decode(e, b)) continue;
				if (b.in_check(opposite(b.stm))) continue;
				if (t.layout.canonical_entry(b) != e) continue;
				t.wdl[e] = UNDECIDED;
				++valid;
			}
		});

		std::vector<std::vector<uint64_t>> batches;
		for (auto& [adv, slices] : batches_by_advancement(t)) batches.push_back(slices);

		tables[m.key()] = std::move(table);
		sink = &out;

		solve_dtz(t, batches);
		const double t_dtz = elapsed();
		if (opt.dtm) solve_dtm(t, batches);
		const double t_dtm = elapsed();
		if (opt.verify) verify_table(t);
		if (report) out.on_table(t);

		std::vector<Table*> family = m.has_rights() ? family_of(m) : std::vector<Table*>{ &t };
		if (opt.dtm50) solve_dtm50(family, report ? &t : nullptr, out);
		const double t_dtm50 = elapsed();
		if (opt.dtc && m.has_pawns()) solve_dtc(family, report ? &t : nullptr, out);
		const double t_dtc = elapsed();

		log(format("%-10s %12llu entries  WDL/DTZ %.1fs  DTM %.1fs  DTM50 %.1fs  DTC %.1fs%s",
			m.name().c_str(), static_cast<unsigned long long>(valid.load()),
			t_dtz, t_dtm - t_dtz, t_dtm50 - t_dtm, t_dtc - t_dtm50,
			anomalies ? "  (internal ordering anomalies)" : ""));
		return true;
	}

	void closure(const Material& m, std::vector<Material>& order, std::set<uint64_t>& seen)
	{
		if (!seen.insert(m.key()).second) return;
		for (const Material& s : Generator::successors(m)) closure(s, order, seen);
		order.push_back(m);
	}
};

Generator::Generator(Generator_Options opt) : m_impl(std::make_unique<Impl>())
{
	m_impl->opt = opt;
	if (m_impl->opt.threads < 1) m_impl->opt.threads = 1;
}

Generator::~Generator() = default;

bool Generator::build(const Material& target, Sink& sink, bool report_closure, std::string* error)
{
	if (target.bare_kings())
	{
		if (error) *error = "bare kings need no table";
		return false;
	}
	std::vector<Material> order;
	std::set<uint64_t> seen;
	m_impl->closure(target, order, seen);
	for (const Material& m : order)
	{
		const bool report = report_closure || m == target;
		// A table built earlier as someone else's sub-table was never reported, and
		// its DTM50 and DTC layers are gone, so a target like that is built again.
		if (m_impl->find(m) && (!report || m_impl->reported.count(m.key()))) continue;
		if (!m_impl->build_material(m, sink, report, error)) return false;
		if (report) m_impl->reported.insert(m.key());
		// A parent reads only the class of a sub-table reached by a zeroing move,
		// so its DTZ is done with once reported. Castling families keep theirs:
		// losing a right is a quiet move into the smaller material.
		if (!target.has_rights() && !(m == target))
			std::vector<uint16_t>().swap(m_impl->tables[m.key()]->dtz);
	}
	return true;
}

const Table* Generator::table(const Material& m) const
{
	return m_impl->find(m);
}

uint64_t Generator::verify_failures() const
{
	return m_impl->verify_failures.load() + m_impl->anomalies.load();
}

}  // namespace ref
