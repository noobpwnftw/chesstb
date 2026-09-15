// Compare chesstb tables against the reference generator, position by position.
//
// The reference (board, retro, refgen) shares no code with chesstb. This file
// is the only place the two meet: each reference position is converted to a
// chesstb Position and probed through chesstb's own prober and table readers.
//
//   WDL     class, from wdl/ and dtz/ tables
//   DTZ     plies; cursed and blessed values may differ by one ply, which is
//           the rounding chesstb documents for its one-byte tier and for
//           castling exits (check_fixedpoint allows the same)
//   DTM     plies, read from dtm/ and, separately, from the dtm50/ pack
//   DTM50   class and plies at each requested halfmove clock
//   DTC     the value at every push budget, read from the dtc/ curve, and the
//           order and value the prober reports at each requested clock
//
// A position just after a double push, with an en passant capture available,
// is not stored in any table: the prober works it out from the stored value and
// the capture. Each such position is checked for every metric above against
// the reference value with the capture added.
//
//   ./check_reference --tables DIR -r KQK,KRPKR [-t N] [--closure]
//                     [--hmc all|0,1,50,99] [--rule50 0,50,99] [--limit N]
//                     [--no-dtm] [--no-dtm50] [--no-dtc]

#include "board.h"
#include "refgen.h"

#include "chess/pair_group.h"
#include "chess/piece_config.h"
#include "chess/position.h"
#include "probe/probe.h"
#include "probe/table_files.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

struct Options
{
	std::string tables = ".";
	std::vector<std::string> materials;
	int threads = std::max(1u, std::thread::hardware_concurrency());
	bool closure = false;
	bool dtm = true;
	bool dtm50 = true;
	bool dtc = true;
	std::vector<int> hmc;      // empty: every clock
	std::vector<unsigned> rule50 = { 0, 50, 99 };
	size_t limit = 10;
	uint64_t curve_memory = uint64_t{ 2 } << 30;
};

const char* wdl_name(int w)
{
	switch (w)
	{
		case 0: return "LOSE";
		case 1: return "BLESSED_LOSS";
		case 2: return "DRAW";
		case 3: return "CURSED_WIN";
		case 4: return "WIN";
		default: return "ILLEGAL";
	}
}

ref::Wdl invert(ref::Wdl w)
{
	return static_cast<ref::Wdl>(4 - static_cast<int>(w));
}

// For the side to move: class first, then the shorter win or the longer loss.
bool better(ref::Outcome a, ref::Outcome b)
{
	if (a.kind != b.kind) return a.kind > b.kind;
	if (a.kind == ref::Outcome::WIN) return a.plies < b.plies;
	if (a.kind == ref::Outcome::LOSS) return a.plies > b.plies;
	return false;
}

bool better_dtz(ref::Wdl a, unsigned ad, ref::Wdl b, unsigned bd)
{
	if (a != b) return static_cast<int>(a) > static_cast<int>(b);
	if (a == ref::Wdl::WIN || a == ref::Wdl::CURSED_WIN) return ad < bd;
	if (a == ref::Wdl::LOSE || a == ref::Wdl::BLESSED_LOSS) return ad > bd;
	return false;
}

// The mover's outcome from a capture into a position worth `child` to the opponent.
ref::Outcome after_capture(ref::Outcome child)
{
	if (child.kind == ref::Outcome::DRAW) return {};
	return { child.kind == ref::Outcome::WIN ? ref::Outcome::LOSS : ref::Outcome::WIN,
	         static_cast<uint16_t>(child.plies + 1) };
}

std::string outcome_text(ref::Outcome o)
{
	if (o.kind == ref::Outcome::DRAW) return "draw";
	return std::string(o.kind == ref::Outcome::WIN ? "win " : "loss ") + std::to_string(o.plies);
}

template <typename F>
void parallel_for(int threads, uint64_t n, F&& f)
{
	std::atomic<uint64_t> next{ 0 };
	const uint64_t chunk = std::max<uint64_t>(256, n / (static_cast<uint64_t>(threads) * 64 + 1));
	std::vector<std::thread> pool;
	for (int t = 0; t < threads; ++t)
		pool.emplace_back([&] {
			for (;;)
			{
				const uint64_t begin = next.fetch_add(chunk);
				if (begin >= n) return;
				const uint64_t end = std::min(n, begin + chunk);
				for (uint64_t i = begin; i < end; ++i) f(i);
			}
		});
	for (auto& th : pool) th.join();
}

Position to_position(const ref::Board& b)
{
	Position pos;
	pos.clear();
	for (int sq = 0; sq < 64; ++sq)
	{
		const ref::Piece p = b.squares[sq];
		if (p == ref::NO_PIECE) continue;
		const Color c = ref::color_of(p) == ref::WHITE ? WHITE : BLACK;
		Piece_Type t = PIECE_TYPE_NONE;
		switch (ref::type_of(p))
		{
			case ref::PAWN:   t = PAWN; break;
			case ref::KNIGHT: t = KNIGHT; break;
			case ref::BISHOP: t = BISHOP; break;
			case ref::ROOK:   t = ROOK; break;
			case ref::QUEEN:  t = QUEEN; break;
			case ref::KING:   t = KING; break;
			default: break;
		}
		pos.put_piece(piece_make(c, t), static_cast<Square>(sq));
	}
	pos.set_turn(b.stm == ref::WHITE ? WHITE : BLACK);
	for (int c = 0; c < 2; ++c)
		for (int side : { ref::A_SIDE, ref::H_SIDE })
			if (b.castle[c][side] != ref::NO_SQUARE)
				pos.set_castling_right(c == 0 ? WHITE : BLACK, side == ref::H_SIDE,
					static_cast<Square>(b.castle[c][side]));
	return pos;
}

// The material and orientation chesstb stores a position under, for reading a
// table file directly. With `pair_mode`, the canonical opposing pair is taken
// out of the free pawns, as chesstb does for a pair table.
struct Stored_Form
{
	Piece_Config ps;
	Position pos;
};

Stored_Form stored_form(const Position& input, bool pair_mode)
{
	Square pair_white = SQ_END, pair_black = SQ_END;
	if (pair_mode)
	{
		Square ws[16], bs[16];
		size_t nw = 0, nb = 0;
		Bitboard wb = input.piece_bb(WHITE_PAWN);
		while (wb) ws[nw++] = wb.pop_first_square();
		Bitboard bb = input.piece_bb(BLACK_PAWN);
		while (bb) bs[nb++] = bb.pop_first_square();
		pair_mode = Pair_Group::find_canonical(Const_Span<Square>(ws, nw), Const_Span<Square>(bs, nb),
		                                       pair_white, pair_black);
	}
	Square rooks[4];
	size_t nr = 0;
	Piece_Config::Castling_Rights_Counts rights{ 0, 0 };
	for (Color c : { WHITE, BLACK })
		for (bool h : { false, true })
			if (input.can_castle(c, h))
			{
				rooks[nr++] = input.castling_rook_square(c, h);
				rights[c] += 1;
			}
	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	Material_Key literal;
	for (Piece pc : ALL_PIECES)
	{
		Bitboard bb = input.piece_bb(pc);
		while (bb)
		{
			const Square s = bb.pop_first_square();
			if (s == pair_white || s == pair_black) continue;
			bool held = false;
			for (size_t i = 0; i < nr; ++i) held |= rooks[i] == s;
			if (held) continue;
			pieces[n++] = pc;
			literal.add_piece(pc);
		}
	}
	if (pair_mode) literal.add_pair();
	if (nr) literal.add_castling(rights[WHITE], rights[BLACK]);
	Piece_Config ps(Const_Span<Piece>(pieces.data(), n), rights);
	if (pair_mode) ps.mark_opposing_pair();
	if (literal == ps.base_material_key()) return { ps, input };
	return { ps, input.mirror() };
}

struct Tally
{
	std::atomic<uint64_t> checked{ 0 };
	std::atomic<uint64_t> mismatched{ 0 };
	std::atomic<uint64_t> missing{ 0 };
	std::mutex mu;
	std::vector<std::string> samples;

	void sample(size_t limit, const std::string& line)
	{
		std::lock_guard<std::mutex> lk(mu);
		if (samples.size() < limit) samples.push_back(line);
	}
};

struct Checker : ref::Sink
{
	Options opt;
	Probe_Tables z_tables;     // WDL and DTZ
	Probe_Tables m_tables;     // DTM from dtm/
	Probe_Tables p_tables;     // DTM and DTM50 from dtm50/
	Probe_Tables c_tables;     // DTC
	std::map<std::string, std::map<std::string, std::unique_ptr<Tally>>> tallies;
	std::mutex tally_mu;
	std::map<std::string, std::vector<std::vector<uint8_t>>> dtc_layers;
	// Materials requested as pair tables: only positions holding an opposing
	// pair are in those tables.
	std::set<uint64_t> pair_keys;
	// The generator, for the sub-tables an en passant capture reaches.
	const ref::Generator* gen = nullptr;
	bool have_dtm_dir = false;
	bool have_pack_dir = false;

	bool pair_table(const ref::Table& t) const { return pair_keys.count(t.layout.material.key()) != 0; }

	static bool has_pair(const ref::Board& b)
	{
		for (int w = 0; w < 64; ++w)
		{
			if (b.squares[w] != ref::make_piece(ref::WHITE, ref::PAWN)) continue;
			for (int r = ref::rank_of(w) + 1; r <= 6; ++r)
				if (b.squares[ref::make_square(ref::file_of(w), r)] == ref::make_piece(ref::BLACK, ref::PAWN)) return true;
		}
		return false;
	}

	explicit Checker(const Options& o) : opt(o)
	{
		for (const std::string& name : opt.materials)
		{
			ref::Material m;
			if (name.find('p') != std::string::npos && ref::Material::parse(name, m)) pair_keys.insert(m.key());
		}
		const std::filesystem::path root(opt.tables);
		for (Probe_Tables* t : { &z_tables, &m_tables, &p_tables, &c_tables })
			t->add_wdl_path(root / "wdl");
		z_tables.add_dtz_path(root / "dtz");
		m_tables.add_dtm_path(root / "dtm");
		p_tables.add_dtm50_path(root / "dtm50");
		c_tables.add_dtc_path(root / "dtc");
		have_dtm_dir = std::filesystem::exists(root / "dtm");
		have_pack_dir = std::filesystem::exists(root / "dtm50");
	}

	Tally& tally(const std::string& material, const std::string& metric)
	{
		std::lock_guard<std::mutex> lk(tally_mu);
		auto& slot = tallies[material][metric];
		if (!slot) slot = std::make_unique<Tally>();
		return *slot;
	}

	void on_log(const std::string& line) override
	{
		std::printf("  %s\n", line.c_str());
		std::fflush(stdout);
	}

	bool reported(const ref::Table& t) const
	{
		if (opt.closure) return true;
		for (const std::string& name : opt.materials)
		{
			ref::Material m;
			if (ref::Material::parse(name, m) && m == t.layout.material) return true;
		}
		return false;
	}

	// `b` with each en passant square a double push just played could have left,
	// where a legal capture uses it. The push itself must have been legal: the
	// side now to move was not in check before it.
	static std::vector<ref::Board> ep_variants(const ref::Board& b)
	{
		std::vector<ref::Board> out;
		const ref::Color pusher = ref::opposite(b.stm);
		const int dir = pusher == ref::WHITE ? 1 : -1;
		for (int file = 0; file < 8; ++file)
		{
			ref::Board v = b;
			v.ep = static_cast<int8_t>(ref::make_square(file, pusher == ref::WHITE ? 2 : 5));
			if (!ref::ep_square_plausible(v, v.ep) || !v.has_legal_ep()) continue;
			ref::Board before = v;
			before.ep = ref::NO_SQUARE;
			before.squares[v.ep + 8 * dir] = ref::NO_PIECE;
			before.squares[v.ep - 8 * dir] = ref::make_piece(pusher, ref::PAWN);
			if (before.in_check(b.stm)) continue;
			out.push_back(v);
		}
		return out;
	}

	// What each en passant capture in `v` reaches, as the reference holds it.
	struct Ep_Capture
	{
		ref::Wdl wdl = ref::Wdl::DRAW;
		ref::Outcome dtm;
		ref::Outcome dtm50;   // at a halfmove clock of zero
	};

	std::vector<Ep_Capture> ep_captures(const ref::Board& v) const
	{
		std::vector<Ep_Capture> out;
		ref::Move moves[ref::MAX_MOVES];
		const int n = v.legal_moves(moves);
		for (int i = 0; i < n; ++i)
		{
			if (!moves[i].is_ep()) continue;
			ref::Board child = v;
			child.make(moves[i]);
			const ref::Material m = ref::Material::of(child);
			Ep_Capture c;
			if (!m.bare_kings())
			{
				const ref::Table* sub = gen->table(m);
				const uint64_t ce = sub->layout.canonical_entry(child);
				c.wdl = sub->wdl[ce];
				if (!sub->dtm.empty()) c.dtm = ref::unpack(sub->dtm[ce]);
				if (!sub->dtm50_0.empty()) c.dtm50 = ref::unpack(sub->dtm50_0[ce]);
			}
			out.push_back(c);
		}
		return out;
	}

	ref::Outcome probe_outcome(WDL_Entry w, unsigned plies) const
	{
		if (w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN) return { ref::Outcome::WIN, static_cast<uint16_t>(plies) };
		if (w == WDL_Entry::LOSE || w == WDL_Entry::BLESSED_LOSS) return { ref::Outcome::LOSS, static_cast<uint16_t>(plies) };
		return {};
	}

	// WDL, DTZ and DTM of the en passant forms of entry `e`.
	void check_ep(const ref::Table& t, uint64_t e, const ref::Board& b)
	{
		const std::vector<ref::Board> variants = ep_variants(b);
		if (variants.empty()) return;
		const std::string name = t.layout.material.name();
		Tally& wdl = tally(name, "WDL ep");
		Tally& dtz = tally(name, "DTZ ep");
		Tally& dtm = tally(name, "DTM ep");
		for (const ref::Board& v : variants)
		{
			ref::Wdl rw = t.wdl[e];
			unsigned rz = rw == ref::Wdl::DRAW ? 0 : t.dtz[e];
			bool exact = false;   // DTZ taken from the capture, so no rounding applies
			ref::Outcome rm = opt.dtm ? ref::unpack(t.dtm[e]) : ref::Outcome{};
			for (const Ep_Capture& c : ep_captures(v))
			{
				const ref::Wdl mine = invert(c.wdl);
				if (better_dtz(mine, 1, rw, rz))
				{
					rw = mine;
					rz = 1;
					exact = true;
				}
				const ref::Outcome o = after_capture(c.dtm);
				if (better(o, rm)) rm = o;
			}

			const Position pos = to_position(v);
			const Square ep = static_cast<Square>(v.ep);
			const Probe_Result z = z_tables.probe(pos, ep, IGNORE_50MR);
			wdl.checked++;
			if (z.status != Probe_Result::Status::OK || z.wdl == WDL_Entry::ILLEGAL)
			{
				wdl.missing++;
				wdl.sample(opt.limit, "no probe result: " + v.fen());
				continue;
			}
			if (static_cast<int>(z.wdl) != static_cast<int>(rw))
			{
				wdl.mismatched++;
				wdl.sample(opt.limit, v.fen() + "  chesstb " + wdl_name(static_cast<int>(z.wdl))
					+ "  reference " + wdl_name(static_cast<int>(rw)));
				continue;
			}
			if (rw != ref::Wdl::DRAW)
			{
				dtz.checked++;
				const bool cursed = !exact && (rw == ref::Wdl::CURSED_WIN || rw == ref::Wdl::BLESSED_LOSS);
				const int diff = std::abs(static_cast<int>(z.dtz) - static_cast<int>(rz));
				if (!z.has_dtz)
				{
					dtz.missing++;
					dtz.sample(opt.limit, "no DTZ: " + v.fen());
				}
				else if (cursed ? diff > 1 : diff != 0)
				{
					dtz.mismatched++;
					dtz.sample(opt.limit, v.fen() + "  " + wdl_name(static_cast<int>(rw)) + "  chesstb dtz "
						+ std::to_string(z.dtz) + "  reference " + std::to_string(rz));
				}
			}

			if (!opt.dtm) continue;
			for (auto [tables, dir, present] : { std::make_tuple(&m_tables, "dtm/", have_dtm_dir),
			                                     std::make_tuple(&p_tables, "dtm50/", have_pack_dir) })
			{
				if (!present) continue;
				const Probe_Result r = tables->probe(pos, ep, IGNORE_50MR);
				dtm.checked++;
				if (r.status != Probe_Result::Status::OK || r.wdl == WDL_Entry::ILLEGAL || !r.has_dtm)
				{
					dtm.missing++;
					dtm.sample(opt.limit, std::string("no DTM from ") + dir + ": " + v.fen());
					continue;
				}
				const ref::Outcome co = probe_outcome(r.wdl, r.dtm);
				if (co != rm)
				{
					dtm.mismatched++;
					dtm.sample(opt.limit, v.fen() + "  " + dir + "  chesstb " + outcome_text(co)
						+ "  reference " + outcome_text(rm));
				}
			}
		}
	}

	// DTM50 of the en passant forms of a position whose stored value is `stored`.
	void check_ep_dtm50(const ref::Table& t, const ref::Board& b, uint16_t stored, int hmc)
	{
		const std::vector<ref::Board> variants = ep_variants(b);
		if (variants.empty()) return;
		Tally& tl = tally(t.layout.material.name(), "DTM50 ep");
		for (const ref::Board& v : variants)
		{
			ref::Outcome ro = ref::unpack(stored);
			for (const Ep_Capture& c : ep_captures(v))
			{
				const ref::Outcome o = after_capture(c.dtm50);
				if (better(o, ro)) ro = o;
			}
			const Probe_Result r = p_tables.probe(to_position(v), static_cast<Square>(v.ep), static_cast<unsigned>(hmc));
			tl.checked++;
			if (r.status != Probe_Result::Status::OK || !r.has_dtm50)
			{
				tl.missing++;
				tl.sample(opt.limit, "no DTM50 at clock " + std::to_string(hmc) + ": " + v.fen());
				continue;
			}
			ref::Outcome co;
			if (r.dtm50_wdl == WDL_Entry::WIN) co = { ref::Outcome::WIN, static_cast<uint16_t>(r.dtm50) };
			else if (r.dtm50_wdl == WDL_Entry::LOSE) co = { ref::Outcome::LOSS, static_cast<uint16_t>(r.dtm50) };
			if (co != ro)
			{
				tl.mismatched++;
				tl.sample(opt.limit, v.fen() + " clock " + std::to_string(hmc) + "  chesstb " + outcome_text(co)
					+ "  reference " + outcome_text(ro));
			}
		}
	}

	void on_table(const ref::Table& t) override
	{
		const bool pair = pair_table(t);
		const std::string name = t.layout.material.name();
		Tally& wdl = tally(name, "WDL");
		Tally& dtz = tally(name, "DTZ");
		Tally& dtm = tally(name, "DTM dtm/");
		Tally& pack = tally(name, "DTM dtm50/");
		const bool have_dtm = std::filesystem::exists(std::filesystem::path(opt.tables) / "dtm");
		const bool have_pack = std::filesystem::exists(std::filesystem::path(opt.tables) / "dtm50");

		parallel_for(opt.threads, t.layout.size(), [&](uint64_t e) {
			if (t.wdl[e] == ref::Wdl::INVALID) return;
			ref::Board b;
			t.layout.decode(e, b);
			if (pair && !has_pair(b)) return;
			const Position pos = to_position(b);
			const int w = static_cast<int>(t.wdl[e]);
			check_ep(t, e, b);

			const Probe_Result z = z_tables.probe(pos, IGNORE_50MR);
			wdl.checked++;
			if (z.status != Probe_Result::Status::OK || z.wdl == WDL_Entry::ILLEGAL)
			{
				wdl.missing++;
				wdl.sample(opt.limit, "no probe result: " + b.fen());
				return;
			}
			if (static_cast<int>(z.wdl) != w)
			{
				wdl.mismatched++;
				wdl.sample(opt.limit, b.fen() + "  chesstb " + wdl_name(static_cast<int>(z.wdl))
					+ "  reference " + wdl_name(w));
			}
			else if (t.wdl[e] != ref::Wdl::DRAW)
			{
				dtz.checked++;
				if (!z.has_dtz)
				{
					dtz.missing++;
					dtz.sample(opt.limit, "no DTZ: " + b.fen());
				}
				else
				{
					const bool cursed = t.wdl[e] == ref::Wdl::CURSED_WIN || t.wdl[e] == ref::Wdl::BLESSED_LOSS;
					const int diff = std::abs(static_cast<int>(z.dtz) - static_cast<int>(t.dtz[e]));
					if (cursed ? diff > 1 : diff != 0)
					{
						dtz.mismatched++;
						dtz.sample(opt.limit, b.fen() + "  " + wdl_name(w) + "  chesstb dtz "
							+ std::to_string(z.dtz) + "  reference " + std::to_string(t.dtz[e]));
					}
				}
			}

			if (!opt.dtm) return;
			const ref::Outcome ro = ref::unpack(t.dtm[e]);
			auto check_dtm = [&](Probe_Tables& tables, Tally& tl) {
				const Probe_Result r = tables.probe(pos, IGNORE_50MR);
				tl.checked++;
				if (r.status != Probe_Result::Status::OK || r.wdl == WDL_Entry::ILLEGAL || !r.has_dtm)
				{
					tl.missing++;
					tl.sample(opt.limit, "no DTM: " + b.fen());
					return;
				}
				const bool win = r.wdl == WDL_Entry::WIN || r.wdl == WDL_Entry::CURSED_WIN;
				const bool loss = r.wdl == WDL_Entry::LOSE || r.wdl == WDL_Entry::BLESSED_LOSS;
				ref::Outcome co;
				if (win) co = { ref::Outcome::WIN, static_cast<uint16_t>(r.dtm) };
				else if (loss) co = { ref::Outcome::LOSS, static_cast<uint16_t>(r.dtm) };
				if (co != ro)
				{
					tl.mismatched++;
					tl.sample(opt.limit, b.fen() + "  chesstb " + outcome_text(co) + "  reference " + outcome_text(ro));
				}
			};
			if (have_dtm) check_dtm(m_tables, dtm);
			if (have_pack) check_dtm(p_tables, pack);
		});
	}

	void on_dtm50_layer(const ref::Table& t, int hmc, const std::vector<uint64_t>& slices,
	                    const std::vector<uint16_t>& layer) override
	{
		if (!opt.dtm50) return;
		if (!opt.hmc.empty() && std::find(opt.hmc.begin(), opt.hmc.end(), hmc) == opt.hmc.end()) return;
		const bool pair = pair_table(t);
		Tally& tl = tally(t.layout.material.name(), "DTM50");
		const uint64_t per = t.layout.slice_entries();
		parallel_for(opt.threads, layer.size(), [&](uint64_t i) {
			const uint64_t e = slices[i / per] * per + i % per;
			if (t.wdl[e] == ref::Wdl::INVALID) return;
			ref::Board b;
			t.layout.decode(e, b);
			if (pair && !has_pair(b)) return;
			check_ep_dtm50(t, b, layer[i], hmc);
			const Probe_Result r = p_tables.probe(to_position(b), static_cast<unsigned>(hmc));
			tl.checked++;
			if (r.status != Probe_Result::Status::OK || !r.has_dtm50)
			{
				tl.missing++;
				tl.sample(opt.limit, "no DTM50 at clock " + std::to_string(hmc) + ": " + b.fen());
				return;
			}
			ref::Outcome co;
			if (r.dtm50_wdl == WDL_Entry::WIN) co = { ref::Outcome::WIN, static_cast<uint16_t>(r.dtm50) };
			else if (r.dtm50_wdl == WDL_Entry::LOSE) co = { ref::Outcome::LOSS, static_cast<uint16_t>(r.dtm50) };
			const ref::Outcome ro = ref::unpack(layer[i]);
			if (co != ro)
			{
				tl.mismatched++;
				tl.sample(opt.limit, b.fen() + " clock " + std::to_string(hmc) + "  chesstb " + outcome_text(co)
					+ "  reference " + outcome_text(ro));
			}
		});
	}

	struct Dtc_Source
	{
		std::unique_ptr<DTC_File> file;
		bool present = false;
	};
	std::map<std::string, Dtc_Source> dtc_files;
	std::mutex dtc_mu;

	DTC_File* dtc_file_for(const Piece_Config& ps)
	{
		std::lock_guard<std::mutex> lk(dtc_mu);
		Dtc_Source& src = dtc_files[ps.name()];
		if (!src.file && !src.present)
		{
			const std::filesystem::path path = std::filesystem::path(opt.tables) / "dtc" / (ps.name() + DTC_EXT);
			src.present = true;
			if (std::filesystem::exists(path))
			{
				src.file = std::make_unique<DTC_File>();
				src.file->load(ps, path);
			}
		}
		return src.file.get();
	}

	void on_dtc_budget(const ref::Table& t, int budget, const std::vector<uint8_t>& layer) override
	{
		if (!opt.dtc) return;
		const std::string name = t.layout.material.name();
		const bool pair = pair_table(t);
		Tally& tl = tally(name, "DTC curve");
		const bool keep = t.layout.size() * ref::DTC_BUDGETS <= opt.curve_memory;
		if (keep) dtc_layers[name].push_back(layer);

		parallel_for(opt.threads, t.layout.size(), [&](uint64_t e) {
			const ref::Wdl w = t.wdl[e];
			if (w != ref::Wdl::WIN && w != ref::Wdl::LOSE) return;
			ref::Board b;
			t.layout.decode(e, b);
			if (pair && !has_pair(b)) return;
			const Stored_Form sf = stored_form(to_position(b), pair);
			DTC_File* f = dtc_file_for(sf.ps);
			tl.checked++;
			if (!f)
			{
				tl.missing++;
				tl.sample(opt.limit, "no DTC file " + sf.ps.name());
				return;
			}
			Color frame = sf.pos.turn();
			Position read_pos = sf.pos;
			if (f->is_dropped[frame])
			{
				read_pos = sf.pos.mirror();
				frame = read_pos.turn();
			}
			DTC_Curve curve;
			f->read_curve(frame, read_pos, w == ref::Wdl::WIN ? WDL_Entry::WIN : WDL_Entry::LOSE, out_param(curve));
			const ref::Outcome ro = ref::dtc_unpack(layer[e]);
			const uint16_t want = ro.kind == ref::Outcome::DRAW ? DTC_Cell::DRAWN : ro.plies;
			if (curve.value[budget] != want)
			{
				tl.mismatched++;
				tl.sample(opt.limit, b.fen() + " budget " + std::to_string(budget) + "  chesstb "
					+ (curve.value[budget] == DTC_Cell::DRAWN ? std::string("drawn") : std::to_string(curve.value[budget]))
					+ "  reference " + outcome_text(ro));
			}
		});

		if (budget == ref::DTC_BUDGETS - 1 && keep) check_dtc_probe(t);
	}

	// The reference's DTC answer with `plies` left: the smallest budget whose
	// value fits, where the unbounded budget is DTZ. -1 when none fits.
	static int dtc_want(const ref::Table& t, const std::vector<std::vector<uint8_t>>& layers, uint64_t e,
	                    unsigned plies, uint16_t& value)
	{
		const ref::Wdl w = t.wdl[e];
		if (w != ref::Wdl::WIN && w != ref::Wdl::LOSE) return -1;
		for (int k = 0; k <= ref::DTC_BUDGETS; ++k)
		{
			uint16_t v;
			if (k < ref::DTC_BUDGETS)
			{
				const ref::Outcome o = ref::dtc_unpack(layers[k][e]);
				if (o.kind == ref::Outcome::DRAW) continue;
				v = o.plies;
			}
			else
				v = t.dtz[e];
			if (v > plies) continue;
			value = v;
			return k;
		}
		return -1;
	}

	// DTC probe answers for the en passant forms of entry `e`. An en passant
	// capture converts at once: budget 0, one ply, when it reaches a strict loss
	// for the opponent (a win) or a strict win (a loss).
	void check_ep_dtc(const ref::Table& t, const std::vector<std::vector<uint8_t>>& layers, uint64_t e,
	                  const ref::Board& b)
	{
		const std::vector<ref::Board> variants = ep_variants(b);
		if (variants.empty()) return;
		Tally& tl = tally(t.layout.material.name(), "DTC probe ep");
		for (const ref::Board& v : variants)
		{
			const std::vector<Ep_Capture> captures = ep_captures(v);
			const Position pos = to_position(v);
			for (unsigned r50 : opt.rule50)
			{
				// For the side to move: 2 a priced win, 1 no price, 0 a priced loss.
				uint16_t value = 0;
				int order = dtc_want(t, layers, e, dtc_budget_plies(r50), value);
				int kind = order < 0 ? 1 : t.wdl[e] == ref::Wdl::WIN ? 2 : 0;
				for (const Ep_Capture& c : captures)
				{
					const ref::Wdl mine = invert(c.wdl);
					const int ck = mine == ref::Wdl::WIN ? 2 : mine == ref::Wdl::LOSE ? 0 : 1;
					// Budget 0 and one ply is the shortest price there is, so it takes
					// any win and never replaces a loss of the same kind.
					const bool take = ck != kind ? ck > kind : (ck == 2 && (order > 0 || value > 1));
					if (!take) continue;
					kind = ck;
					order = ck == 1 ? -1 : 0;
					value = ck == 1 ? 0 : 1;
				}

				const Probe_Result r = c_tables.probe(pos, static_cast<Square>(v.ep), r50);
				tl.checked++;
				if (r.status != Probe_Result::Status::OK)
				{
					tl.missing++;
					tl.sample(opt.limit, "no DTC probe at clock " + std::to_string(r50) + ": " + v.fen());
					continue;
				}
				const int got = !r.has_dtc ? 1 : r.dtc_wdl == WDL_Entry::WIN ? 2 : r.dtc_wdl == WDL_Entry::LOSE ? 0 : 1;
				const bool ok = got == kind
					&& (kind == 1 || (static_cast<int>(r.dtc_order) == order && r.dtc == value));
				if (!ok)
				{
					auto text = [](int k, int o, unsigned val) {
						if (k == 1) return std::string("drawn");
						return std::string(k == 2 ? "win " : "loss ") + std::to_string(o) + "/" + std::to_string(val);
					};
					tl.mismatched++;
					tl.sample(opt.limit, v.fen() + " clock " + std::to_string(r50) + "  chesstb "
						+ text(got, r.dtc_order, r.dtc) + "  reference " + text(kind, order, value));
				}
			}
		}
	}

	// The prober's DTC answer at a clock: the smallest budget whose value fits
	// the plies left, where the unbounded budget is DTZ.
	void check_dtc_probe(const ref::Table& t)
	{
		const std::string name = t.layout.material.name();
		const auto& layers = dtc_layers[name];
		const bool pair = pair_table(t);
		Tally& tl = tally(name, "DTC probe");
		parallel_for(opt.threads, t.layout.size(), [&](uint64_t e) {
			const ref::Wdl w = t.wdl[e];
			if (w == ref::Wdl::INVALID) return;
			ref::Board b;
			t.layout.decode(e, b);
			if (pair && !has_pair(b)) return;
			check_ep_dtc(t, layers, e, b);
			if (w == ref::Wdl::DRAW) return;
			const Position pos = to_position(b);
			for (unsigned r50 : opt.rule50)
			{
				uint16_t want_value = 0;
				const int want_order = dtc_want(t, layers, e, dtc_budget_plies(r50), want_value);
				const Probe_Result r = c_tables.probe(pos, r50);
				tl.checked++;
				if (r.status != Probe_Result::Status::OK || !r.has_dtc)
				{
					tl.missing++;
					tl.sample(opt.limit, "no DTC probe at clock " + std::to_string(r50) + ": " + b.fen());
					continue;
				}
				const bool priced = r.dtc_wdl == WDL_Entry::WIN || r.dtc_wdl == WDL_Entry::LOSE;
				const bool ok = want_order < 0
					? !priced
					: (priced && static_cast<int>(r.dtc_order) == want_order && r.dtc == want_value);
				if (!ok)
				{
					tl.mismatched++;
					tl.sample(opt.limit, b.fen() + " clock " + std::to_string(r50) + "  chesstb "
						+ (priced ? std::to_string(r.dtc_order) + "/" + std::to_string(r.dtc) : std::string("drawn"))
						+ "  reference " + (want_order < 0 ? std::string("drawn")
							: std::to_string(want_order) + "/" + std::to_string(want_value)));
				}
			}
		});
		dtc_layers.erase(name);
	}

	bool report()
	{
		bool clean = true;
		std::printf("\n%-10s %-12s %14s %12s %10s\n", "material", "metric", "checked", "mismatched", "missing");
		for (auto& [material, metrics] : tallies)
			for (auto& [metric, tl] : metrics)
			{
				if (tl->checked == 0) continue;
				std::printf("%-10s %-12s %14llu %12llu %10llu\n", material.c_str(), metric.c_str(),
					static_cast<unsigned long long>(tl->checked.load()),
					static_cast<unsigned long long>(tl->mismatched.load()),
					static_cast<unsigned long long>(tl->missing.load()));
				if (tl->mismatched || tl->missing) clean = false;
				for (const std::string& s : tl->samples) std::printf("    %s\n", s.c_str());
			}
		return clean;
	}
};

std::vector<std::string> split(const std::string& s, char sep)
{
	std::vector<std::string> out;
	std::stringstream ss(s);
	for (std::string item; std::getline(ss, item, sep);)
		if (!item.empty()) out.push_back(item);
	return out;
}

[[noreturn]] void usage(const char* argv0)
{
	std::fprintf(stderr,
		"usage: %s --tables DIR -r LIST [-t N] [--closure] [--hmc all|LIST] [--rule50 LIST]\n"
		"          [--limit N] [--no-dtm] [--no-dtm50] [--no-dtc]\n", argv0);
	std::exit(2);
}

}  // namespace

int main(int argc, char** argv)
{
	Options opt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		auto value = [&]() -> std::string { if (i + 1 >= argc) usage(argv[0]); return argv[++i]; };
		if (a == "--tables") opt.tables = value();
		else if (a == "-r") for (const auto& m : split(value(), ',')) opt.materials.push_back(m);
		else if (a == "-t") opt.threads = std::max(1, std::atoi(value().c_str()));
		else if (a == "--closure") opt.closure = true;
		else if (a == "--no-dtm") opt.dtm = false;
		else if (a == "--no-dtm50") opt.dtm50 = false;
		else if (a == "--no-dtc") opt.dtc = false;
		else if (a == "--limit") opt.limit = std::strtoull(value().c_str(), nullptr, 10);
		else if (a == "--hmc")
		{
			const std::string v = value();
			opt.hmc.clear();
			if (v != "all") for (const auto& h : split(v, ',')) opt.hmc.push_back(std::atoi(h.c_str()));
		}
		else if (a == "--rule50")
		{
			opt.rule50.clear();
			for (const auto& h : split(value(), ',')) opt.rule50.push_back(static_cast<unsigned>(std::atoi(h.c_str())));
		}
		else usage(argv[0]);
	}
	if (opt.materials.empty()) usage(argv[0]);

	Checker checker(opt);
	ref::Generator_Options gopt;
	gopt.threads = opt.threads;
	gopt.dtm = opt.dtm;
	gopt.dtm50 = opt.dtm50;
	gopt.dtc = opt.dtc;
	ref::Generator gen(gopt);
	checker.gen = &gen;

	for (const std::string& name : opt.materials)
	{
		ref::Material m;
		std::string err;
		if (!ref::Material::parse(name, m, &err))
		{
			std::fprintf(stderr, "%s\n", err.c_str());
			return 2;
		}
		std::printf("%s\n", name.c_str());
		if (!gen.build(m, checker, opt.closure, &err))
		{
			std::fprintf(stderr, "%s\n", err.c_str());
			return 1;
		}
	}
	const bool clean = checker.report();
	std::printf("\n%s\n", clean ? "no differences" : "DIFFERENCES FOUND");
	return clean ? 0 : 1;
}
