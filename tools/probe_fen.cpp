// FEN-based probe diagnostic.
//
//   ./tools/probe_fen "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
//   ./tools/probe_fen --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
//   ./tools/probe_fen --wdl ./wdl --dtz ./dtz --dtm ./dtm "8/8/8/8/4k3/8/Q7/K7 w"

#include "probe/probe.h"
#include "util/cache.h"

#include "chess/attack.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace {

const char* color_name(Color c) { return c == WHITE ? "W" : "B"; }

const char* wdl_name(WDL_Entry w)
{
	switch (w) {
		case WDL_Entry::ILLEGAL:      return "ILLEGAL";
		case WDL_Entry::LOSE:         return "LOSE";
		case WDL_Entry::BLESSED_LOSS: return "BLESSED_LOSS";
		case WDL_Entry::DRAW:         return "DRAW";
		case WDL_Entry::CURSED_WIN:   return "CURSED_WIN";
		case WDL_Entry::WIN:          return "WIN";
	}
	return "?";
}

std::string fen_of(const Position& p)
{
	char buf[128] = {};
	p.to_fen(Span(buf, sizeof(buf)));
	return std::string(buf);
}

struct Options
{
	// Search directories in order; the flags append to the conventional layout.
	std::vector<std::string> wdl_dirs   = { "./wdl/" };
	std::vector<std::string> dtz_dirs   = { "./dtz/" };
	std::vector<std::string> dtc_dirs   = { "./dtc/" };
	std::vector<std::string> dtm_dirs   = { "./dtm/" };
	std::vector<std::string> dtm50_dirs = { "./dtm50/" };
	bool dump_children = false;
	size_t child_limit = 9999;
	size_t cache_mib = DEFAULT_BLOCK_CACHE_BYTES / (1024 * 1024);
};


[[noreturn]] void usage(const char* argv0)
{
	std::fprintf(stderr,
		"Usage:\n"
		"  %s [options] \"FEN\" [\"FEN\" ...]\n"
		"\n"
		"Options:\n"
		"  --children        dump legal child moves and post-move disk WDL/DTZ/DTM/DTM50\n"
		"  --limit N         max children to print (default: all)\n"
		"  --wdl DIR         WDL directory (default ./wdl/)\n"
		"  --dtz DIR         DTZ directory (default ./dtz/)\n"
		"  --dtc DIR         DTC directory (default ./dtc/)\n"
		"  --dtm DIR         DTM directory (default ./dtm/)\n"
		"  --dtm50 DIR       DTM50 directory (default ./dtm50/)\n"
		"  --cache MiB       decoded-block cache budget across all tables (default 64)\n"
		"  --help            this help\n",
		argv0);
	std::exit(2);
}

std::string fen_field(const std::string& fen, int index)
{
	std::istringstream ss(fen);
	std::string token;
	for (int i = 0; i <= index; ++i)
		if (!(ss >> token)) return {};
	return token;
}

std::string fen_ep_token(const std::string& fen)
{
	const std::string ep = fen_field(fen, 3);
	return ep.empty() ? "-" : ep;
}

Square fen_ep_square(const std::string& fen)
{
	const std::string ep = fen_ep_token(fen);
	return ep == "-" ? SQ_END : square_from_string(ep.c_str());
}

unsigned fen_rule50(const std::string& fen)
{
	const std::string hmc = fen_field(fen, 4);
	if (hmc.empty() || hmc.find_first_not_of("0123456789") != std::string::npos)
		return 0;
	return static_cast<unsigned>(std::strtoul(hmc.c_str(), nullptr, 10));
}

struct Root_Pos {
	Position pos;
	Piece_Config ps;
	Square ep_square = SQ_END;
	bool mirrored = false;
};

Piece_Config canonical_ps(const Position& pos)
{
	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	for (Piece pc : ALL_PIECES)
		for (size_t i = 0, c = pos.piece_bb(pc).num_set_bits(); i < c; ++i)
			pieces[n++] = pc;
	return Piece_Config(Const_Span<Piece>(pieces.data(), n));
}

Material_Key literal_material_key(const Position& pos)
{
	Material_Key k;
	for (Piece pc : ALL_PIECES)
		for (size_t i = 0, c = pos.piece_bb(pc).num_set_bits(); i < c; ++i)
			k.add_piece(pc);
	return k;
}

Root_Pos canonical_root_position(const Position& input, const std::string& fen)
{
	Root_Pos root{input, canonical_ps(input), fen_ep_square(fen), false};
	if (literal_material_key(input) != root.ps.base_material_key())
	{
		root.pos = input.mirror();
		if (root.ep_square != SQ_END)
			root.ep_square = sq_rank_mirror(root.ep_square);
		root.mirrored = true;
	}
	return root;
}

void print_probe_result(const Probe_Result& r, const char* prefix)
{
	switch (r.status)
	{
		case Probe_Result::Status::TB_NOT_FOUND: std::printf("%sTB not found\n", prefix); return;
		case Probe_Result::Status::OK:           break;
	}
	std::printf("%swdl=%s", prefix, wdl_name(r.wdl));
	if (r.has_dtz) std::printf(" dtz=%u", static_cast<unsigned>(r.dtz));
	if (r.has_dtc)
		std::printf(" dtc=%s/%u/%u", wdl_name(r.dtc_wdl),
			static_cast<unsigned>(r.dtc_order), static_cast<unsigned>(r.dtc));
	if (r.has_dtm) std::printf(" dtm=%u", static_cast<unsigned>(r.dtm));
	if (r.has_dtm50)
		std::printf(" dtm50=%s/%u", wdl_name(r.dtm50_wdl), static_cast<unsigned>(r.dtm50));
	std::printf("\n");
}

bool move_is_zeroing(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN
	    || pos.piece_at(m.to()) != PIECE_NONE
	    || m.is_ep_capture();
}

bool position_is_checkmate(const Position& pos)
{
	if (!pos.is_in_check()) return false;
	Position copy = pos;
	Move_List ml;
	copy.gen_pseudo_legal_moves(out_param(ml));
	for (size_t i = 0; i < ml.size(); ++i)
		if (copy.is_pseudo_legal_move_legal(ml[i])) return false;
	return true;
}

// Local copy of probe.cpp's same-named helper.
void add_ep_moves(const Position& pos, Square ep_square, Move_List& ml)
{
	if (ep_square == SQ_END) return;
	const Color me = pos.turn();
	const Rank target_rank = (me == WHITE) ? RANK_6 : RANK_3;
	const Rank pawn_rank   = (me == WHITE) ? RANK_5 : RANK_4;
	if (sq_rank(ep_square) != target_rank) return;

	const File target_file = sq_file(ep_square);
	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(target_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square from = sq_make(pawn_rank, static_cast<File>(f));
		if (pos.piece_at(from) != piece_make(me, PAWN)) continue;

		const Square cap_sq = sq_make(pawn_rank, target_file);
		if (pos.piece_at(cap_sq) != piece_make(color_opp(me), PAWN)) continue;

		ml.add(Move::make_ep_capture(from, ep_square));
	}
}

// Display-only; Probe_Tables applies the en-passant overlay.
size_t count_legal_ep_moves(const Position& pos, Square ep_square)
{
	Move_List ml;
	add_ep_moves(pos, ep_square, ml);
	size_t n = 0;
	for (size_t i = 0; i < ml.size(); ++i)
		if (pos.is_pseudo_legal_move_legal(ml[i])) ++n;
	return n;
}

void dump_children(const Options& opt, Probe_Tables* tables,
                   const Position& root, const Piece_Config& root_ps, Square ep_square,
                   unsigned rule50)
{
	Move_List ml;
	root.gen_pseudo_legal_moves(out_param(ml));
	add_ep_moves(root, ep_square, ml);
	const Position::Legality ctx = root.legality_context();

	size_t printed = 0;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!root.is_pseudo_legal_move_legal(m, ctx)) continue;
		if (printed >= opt.child_limit) break;

		const bool zeroing = move_is_zeroing(root, m);
		const unsigned child_rule50 = zeroing ? 0u : rule50 + 1u;
		const Square child_ep = root.ep_square_after_move(m);

		Position child = root;
		const Piece captured = child.do_move(m);
		const Piece_Config child_ps = canonical_ps(child);
		const bool same_material = (child_ps.name() == root_ps.name());

		char mbuf[8] = {};
		m.to_string(mbuf);
		const bool is_cap = captured != PIECE_NONE;
		const bool is_promo = m.is_promotion();
		const bool is_pawn = piece_type(root.piece_at(m.from())) == PAWN;
		// Mate ends the game, so it outranks whatever mechanism delivered it.
		const char* kind =
			position_is_checkmate(child) ? "mate-conv" :
			m.is_ep_capture()            ? "ep-conv" :
			is_cap || is_promo           ? "mat-conv" :
			is_pawn                      ? "pawn-conv" :
			                               "quiet";

		std::printf("    %-5s %-10s mat=%-8s stm=%s hmc=%u",
			mbuf, kind, child_ps.name().c_str(), color_name(child.turn()), child_rule50);
		if (!same_material) std::printf(" sub");
		std::printf("\n");

		Probe_Result r;
		if (child_ps.is_bare_kings())
		{
			r.status = Probe_Result::Status::OK;
			r.wdl = WDL_Entry::DRAW;
			r.has_dtz = true; r.dtz = 0;
			r.has_dtc = true; r.dtc_wdl = WDL_Entry::DRAW; r.dtc_order = 0; r.dtc = 0;
			r.has_dtm = true; r.dtm = 0;
			r.has_dtm50 = true; r.dtm50_wdl = WDL_Entry::DRAW; r.dtm50 = 0;
		}
		else
		{
			r = tables->probe(child, child_ep, child_rule50);
		}
		print_probe_result(r, "      ");
		++printed;
	}
}

const char* domain_error(const Position& pos)
{
	if (pos.piece_bb(WHITE_KING).num_set_bits() != 1
	    || pos.piece_bb(BLACK_KING).num_set_bits() != 1)
		return "needs exactly one king per side";
	if ((pos.piece_bb(WHITE_PAWN) | pos.piece_bb(BLACK_PAWN)) & (RANK_1 | RANK_8))
		return "pawn on a back rank (pawns are indexed over ranks 2-7)";
	if (!pos.is_legal())
		return "side not to move is in check";
	return nullptr;
}

void probe_one(const Options& opt, Probe_Tables* tables, const std::string& fen)
{
	Position pos;
	try { pos = Position::from_fen(fen); }
	catch (const std::exception& e) {
		std::printf("---- invalid FEN\n");
		std::printf("  input: %s\n", fen.c_str());
		std::printf("  error: %s\n", e.what());
		return;
	}

	if (const char* err = domain_error(pos))
	{
		std::printf("---- outside table domain\n");
		std::printf("  input: %s\n", fen.c_str());
		std::printf("  error: %s\n", err);
		return;
	}

	const Root_Pos root = canonical_root_position(pos, fen);

	std::printf("---- %s\n", root.ps.name().c_str());
	std::printf("  input: %s\n", fen.c_str());
	if (root.mirrored)
		std::printf("  canonical: %s\n", fen_of(root.pos).c_str());
	if (fen_ep_token(fen) != "-")
		std::printf("  ep=%s legal_ep_moves=%zu\n",
			fen_ep_token(fen).c_str(),
			count_legal_ep_moves(root.pos, root.ep_square));

	const unsigned rule50 = fen_rule50(fen);
	const Probe_Result r = tables->probe(root.ps, root.pos, root.ep_square, rule50);
	std::printf("  hmc=%u\n", rule50);
	print_probe_result(r, "  result: ");

	if (opt.dump_children) dump_children(opt, tables, root.pos, root.ps, root.ep_square, rule50);
}

std::vector<std::string> parse_args(int argc, char** argv, Options* opt)
{
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--help" || a == "-h") usage(argv[0]);
		if (a == "--children" || a == "--moves") { opt->dump_children = true; continue; }
		if (a == "--limit") { if (++i >= argc) usage(argv[0]); opt->child_limit = std::strtoull(argv[i], nullptr, 10); continue; }
		if (a == "--wdl")   { if (++i >= argc) usage(argv[0]); opt->wdl_dirs.emplace_back(argv[i]); continue; }
		if (a == "--dtz")   { if (++i >= argc) usage(argv[0]); opt->dtz_dirs.emplace_back(argv[i]); continue; }
		if (a == "--dtc")   { if (++i >= argc) usage(argv[0]); opt->dtc_dirs.emplace_back(argv[i]); continue; }
		if (a == "--dtm")   { if (++i >= argc) usage(argv[0]); opt->dtm_dirs.emplace_back(argv[i]); continue; }
		if (a == "--dtm50") { if (++i >= argc) usage(argv[0]); opt->dtm50_dirs.emplace_back(argv[i]); continue; }
		if (a == "--cache") { if (++i >= argc) usage(argv[0]); opt->cache_mib = std::strtoull(argv[i], nullptr, 10); continue; }
		if (!a.empty() && a[0] == '-') usage(argv[0]);
		args.push_back(a);
	}
	if (args.empty()) usage(argv[0]);
	return args;
}

}  // namespace

int main(int argc, char** argv)
{
	try {
		Options opt;
		const std::vector<std::string> fens = parse_args(argc, argv, &opt);

		Probe_Tables tables;
		tables.set_block_cache_bytes(opt.cache_mib * 1024 * 1024);
		for (const auto& d : opt.wdl_dirs)   tables.add_wdl_path(d);
		for (const auto& d : opt.dtz_dirs)   tables.add_dtz_path(d);
		for (const auto& d : opt.dtc_dirs)   tables.add_dtc_path(d);
		for (const auto& d : opt.dtm_dirs)   tables.add_dtm_path(d);
		for (const auto& d : opt.dtm50_dirs) tables.add_dtm50_path(d);

		for (const std::string& fen : fens) probe_one(opt, &tables, fen);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
