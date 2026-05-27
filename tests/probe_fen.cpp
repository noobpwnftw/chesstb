// FEN-based probe diagnostic.
//
//   ./run_probe "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
//   ./run_probe --children "8/8/8/6B1/3k4/3B4/p7/1K6 w - - 0 1"
//   ./run_probe --wdl ./wdl --dtc ./dtc --dtm ./dtm "8/8/8/8/4k3/8/Q7/K7 w"

#include "probe/probe.h"

#include "chess/attack.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include <cstdio>
#include <cstdlib>
#include <array>
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
	std::string wdl_dir = "./wdl/";
	std::string dtc_dir = "./dtc/";
	std::string dtm_dir = "./dtm/";
	std::string dtm50_dir = "./dtm50/";
	bool dump_children = false;
	size_t child_limit = 9999;
	unsigned rule50 = 0;
};

struct Probe
{
	std::string fen;
	bool stm_override = false;
	Color stm = WHITE;
};

[[noreturn]] void usage(const char* argv0)
{
	std::fprintf(stderr,
		"Usage:\n"
		"  %s [options] \"FEN\" [\"FEN\" ...]\n"
		"  %s [options] \"FEN\" w|b\n"
		"\n"
		"Options:\n"
		"  --children        dump legal child moves and post-move disk WDL/DTC/DTM/DTM50\n"
		"  --limit N         max children to print (default: all)\n"
		"  --wdl DIR         WDL directory (default ./wdl/)\n"
		"  --dtc DIR         DTC directory (default ./dtc/)\n"
		"  --dtm DIR         DTM directory (default ./dtm/)\n"
		"  --dtm50 DIR       DTM50 directory (default ./dtm50/)\n"
		"  --rule50 N        halfmove clock at the root for DTM50 layer selection (default 0)\n"
		"  --help            this help\n",
		argv0, argv0);
	std::exit(2);
}

bool is_color_token(const std::string& s) { return s == "w" || s == "W" || s == "b" || s == "B"; }
Color parse_color_token(const std::string& s) { return (s == "b" || s == "B") ? BLACK : WHITE; }

std::string fen_ep_token(const std::string& fen)
{
	std::istringstream ss(fen);
	std::string token;
	for (int i = 0; i < 4; ++i)
		if (!(ss >> token)) return "-";
	return token;
}

Square fen_ep_square(const std::string& fen)
{
	const std::string ep = fen_ep_token(fen);
	return ep == "-" ? SQ_END : square_from_string(ep.c_str());
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

Position mirror_for_canonical(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
		if (Piece q = pos.piece_at(sq); q != PIECE_NONE)
			swapped.put_piece(piece_opp_color(q), sq_rank_mirror(sq));
	swapped.set_turn(color_opp(pos.turn()));
	return swapped;
}

Root_Pos canonical_root_position(const Position& input, const Probe& probe)
{
	Root_Pos root{input, canonical_ps(input), fen_ep_square(probe.fen), false};
	if (literal_material_key(input) != root.ps.base_material_key())
	{
		root.pos = mirror_for_canonical(input);
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
		case Probe_Result::Status::ILLEGAL_POS:  std::printf("%sillegal position\n", prefix); return;
		case Probe_Result::Status::OK:           break;
	}
	std::printf("%swdl=%s", prefix, wdl_name(r.wdl));
	if (r.has_dtc) std::printf(" dtz=%u", static_cast<unsigned>(r.dtc));
	else           std::printf(" dtc=<missing>");
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
                   const Position& root, const Piece_Config& root_ps, Square ep_square)
{
	Move_List ml;
	root.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	add_ep_moves(root, ep_square, ml);

	size_t printed = 0;
	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!root.is_pseudo_legal_move_legal(m)) continue;
		if (printed >= opt.child_limit) break;

		const bool zeroing = move_is_zeroing(root, m);
		const unsigned child_rule50 = zeroing ? 0u : opt.rule50 + 1u;

		Position child = root;
		const Piece captured = child.do_move(m);
		const Piece_Config child_ps = canonical_ps(child);
		const bool same_material = (child_ps.name() == root_ps.name());
		const bool is_kk = child_ps.num_pieces() <= 2;

		char mbuf[8] = {};
		m.to_string(mbuf);
		const bool is_cap = captured != PIECE_NONE;
		const bool is_promo = m.is_promotion();
		const bool is_pawn = piece_type(root.piece_at(m.from())) == PAWN;
		const char* kind =
			m.is_ep_capture() ? "ep" :
			is_cap || is_promo ? "conversion" :
			is_pawn           ? "pawn-push" :
			                    "quiet";

		std::printf("    %-5s %-10s mat=%-8s stm=%s hmc=%u",
			mbuf, kind, child_ps.name().c_str(), color_name(child.turn()), child_rule50);
		if (!same_material) std::printf(" sub");
		std::printf("\n");

		Probe_Result r;
		if (is_kk)
		{
			r.status = Probe_Result::Status::OK;
			r.wdl = WDL_Entry::DRAW;
			r.has_dtc = true; r.dtc = 0;
			r.has_dtm = true; r.dtm = 0;
			r.has_dtm50 = true; r.dtm50_wdl = WDL_Entry::DRAW; r.dtm50 = 0;
		}
		else
		{
			r = tables->probe(child, child_rule50);
		}
		print_probe_result(r, "      ");
		++printed;
	}
}

void probe_one(const Options& opt, Probe_Tables* tables, const Probe& probe)
{
	Position pos;
	try { pos = Position::from_fen(probe.fen); }
	catch (const std::exception& e) {
		std::printf("---- invalid FEN\n");
		std::printf("  input: %s\n", probe.fen.c_str());
		std::printf("  error: %s\n", e.what());
		return;
	}
	if (probe.stm_override) pos.set_turn(probe.stm);

	const Root_Pos root = canonical_root_position(pos, probe);

	std::printf("---- %s\n", root.ps.name().c_str());
	std::printf("  input: %s\n", probe.fen.c_str());
	if (root.mirrored)
		std::printf("  canonical: %s\n", fen_of(root.pos).c_str());
	if (fen_ep_token(probe.fen) != "-")
		std::printf("  ep=%s legal_ep_moves=%zu\n",
			fen_ep_token(probe.fen).c_str(),
			count_legal_ep_moves(root.pos, root.ep_square));

	const Probe_Result r = tables->probe(root.ps, root.pos, root.ep_square, opt.rule50);
	std::printf("  hmc=%u\n", opt.rule50);
	print_probe_result(r, "  result: ");

	if (opt.dump_children) dump_children(opt, tables, root.pos, root.ps, root.ep_square);
}

std::vector<Probe> parse_args(int argc, char** argv, Options* opt)
{
	std::vector<std::string> args;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--help" || a == "-h") usage(argv[0]);
		if (a == "--children" || a == "--moves") { opt->dump_children = true; continue; }
		if (a == "--limit") { if (++i >= argc) usage(argv[0]); opt->child_limit = std::strtoull(argv[i], nullptr, 10); continue; }
		if (a == "--wdl")   { if (++i >= argc) usage(argv[0]); opt->wdl_dir = argv[i]; continue; }
		if (a == "--dtc")   { if (++i >= argc) usage(argv[0]); opt->dtc_dir = argv[i]; continue; }
		if (a == "--dtm")   { if (++i >= argc) usage(argv[0]); opt->dtm_dir = argv[i]; continue; }
		if (a == "--dtm50") { if (++i >= argc) usage(argv[0]); opt->dtm50_dir = argv[i]; continue; }
		if (a == "--rule50"){ if (++i >= argc) usage(argv[0]); opt->rule50 = static_cast<unsigned>(std::strtoul(argv[i], nullptr, 10)); continue; }
		if (!a.empty() && a[0] == '-') usage(argv[0]);
		args.push_back(a);
	}
	if (args.empty()) usage(argv[0]);

	std::vector<Probe> probes;
	for (size_t i = 0; i < args.size(); ++i)
	{
		Probe p;
		p.fen = args[i];
		if (i + 1 < args.size() && is_color_token(args[i + 1]))
		{
			p.stm_override = true;
			p.stm = parse_color_token(args[++i]);
		}
		probes.push_back(std::move(p));
	}
	return probes;
}

}  // namespace

int main(int argc, char** argv)
{
	try {
		attack_init();

		Options opt;
		const std::vector<Probe> probes = parse_args(argc, argv, &opt);

		Probe_Tables tables;
		tables.add_wdl_path(opt.wdl_dir);
		tables.add_dtc_path(opt.dtc_dir);
		tables.add_dtm_path(opt.dtm_dir);
		tables.add_dtm50_path(opt.dtm50_dir);

		for (const Probe& p : probes) probe_one(opt, &tables, p);
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
