// Tests for the reference board and reverse move generation.
//
//   perft         the board's move generation against published perft counts
//   bijection     over whole move trees, every forward move is recovered from
//                 the position it reaches (completeness), and every predecessor
//                 returned reaches its position by a legal move (soundness)
//   quiet         the scheduling helper for_each_quiet_unmove offers every quiet
//                 predecessor that predecessors() finds
//
//   ./test_retro [--quick]

#include "board.h"
#include "retro.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace ref;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what)
{
	if (ok) return;
	++g_failures;
	std::printf("  FAIL  %s\n", what.c_str());
}

Board parse(const char* fen)
{
	Board b;
	std::string err;
	if (!Board::from_fen(fen, b, &err))
	{
		std::printf("  bad FEN in test: %s\n", err.c_str());
		std::exit(2);
	}
	return b;
}

uint64_t perft(const Board& b, int depth)
{
	Move moves[MAX_MOVES];
	const int n = b.legal_moves(moves);
	if (depth == 1) return static_cast<uint64_t>(n);
	uint64_t total = 0;
	for (int i = 0; i < n; ++i)
	{
		Board child = b;
		child.make(moves[i]);
		total += perft(child, depth - 1);
	}
	return total;
}

struct Perft_Case
{
	const char* fen;
	std::vector<uint64_t> counts;
};

void run_perft(const char* title, const std::vector<Perft_Case>& cases, int max_depth)
{
	std::printf("\n[perft] %s\n", title);
	for (const Perft_Case& c : cases)
	{
		const Board b = parse(c.fen);
		std::printf("  %-72s", c.fen);
		bool ok = true;
		const int depth = std::min<int>(max_depth, static_cast<int>(c.counts.size()));
		for (int d = 1; d <= depth; ++d)
		{
			const uint64_t n = perft(b, d);
			std::printf(" %llu", static_cast<unsigned long long>(n));
			if (n != c.counts[d - 1])
			{
				ok = false;
				check(false, std::string("perft depth ") + std::to_string(d) + " of " + c.fen
					+ ": got " + std::to_string(n) + ", expected " + std::to_string(c.counts[d - 1]));
			}
		}
		std::printf("  %s\n", ok ? "ok" : "MISMATCH");
	}
}

// Published perft counts (chessprogramming.org), standard chess.
const std::vector<Perft_Case> STANDARD_PERFT = {
	{ "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", { 20, 400, 8902, 197281 } },
	{ "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", { 48, 2039, 97862, 4085603 } },
	{ "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", { 14, 191, 2812, 43238, 674624 } },
	{ "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", { 6, 264, 9467, 422333 } },
	{ "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", { 44, 1486, 62379, 2103487 } },
	{ "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", { 46, 2079, 89890, 3894594 } },
};

// python-chess 1.11.2 with chess960=True.
const std::vector<Perft_Case> CHESS960_PERFT = {
	{ "bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9", { 21, 528, 12189 } },
	{ "2nnrbkr/p1qppppp/8/1ppb4/6PP/3PP3/PPP2P2/BQNNRBKR w HEhe - 1 9", { 21, 807, 18002 } },
	{ "b1q1rrkb/pppppppp/3nn3/8/P7/1PPP4/4PPPP/BQNNRKRB w GE - 1 9", { 20, 479, 10471 } },
	{ "1r2k1r1/8/8/8/8/8/8/1R2K1R1 w GBgb - 0 1", { 25, 525, 12297 } },
	{ "4k3/8/8/8/8/8/8/rR2K1Rr w GB - 0 1", { 10, 194, 3664 } },
	{ "2r1kr2/8/8/8/8/8/8/1R4KR w Hfc - 0 1", { 22, 497, 10941 } },
};

// Unmake-perft counts and predecessor sets against the Python implementation,
// read from retro_expected.txt.
void run_python_parity(bool quick, const std::string& path)
{
	std::printf("\n[parity] against the Python implementation (%s)\n", path.c_str());
	std::FILE* f = std::fopen(path.c_str(), "r");
	if (!f)
	{
		check(false, "cannot open " + path);
		return;
	}
	const Retro_Options strict{ false, Validity::STRICT };
	char buf[1 << 16];
	std::string line;
	auto split = [](const std::string& s, char sep) {
		std::vector<std::string> out;
		size_t start = 0;
		for (;;)
		{
			const size_t p = s.find(sep, start);
			out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
			if (p == std::string::npos) break;
			start = p + 1;
		}
		return out;
	};
	while (std::fgets(buf, sizeof(buf), f))
	{
		line = buf;
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		const std::vector<std::string> parts = split(line, '|');
		if (parts.size() < 3) continue;
		const Board b = parse(parts[1].c_str());

		if (parts[0] == "unmake")
		{
			const std::vector<std::string> counts = split(parts[2], ',');
			std::printf("  unmake %-70s", parts[1].c_str());
			bool ok = true;
			for (size_t d = 1; d <= counts.size(); ++d)
			{
				if (quick && d > 2) break;
				const uint64_t want = std::stoull(counts[d - 1]);
				const uint64_t got = unmake_perft(b, static_cast<int>(d), strict);
				std::printf(" %llu", static_cast<unsigned long long>(got));
				if (got != want)
				{
					ok = false;
					check(false, "unmake-perft depth " + std::to_string(d) + " of " + parts[1] + ": got "
						+ std::to_string(got) + ", Python " + std::to_string(want));
				}
			}
			std::printf("  %s\n", ok ? "ok" : "MISMATCH");
		}
		else if (parts[0] == "preds" && parts.size() >= 4)
		{
			std::set<std::string> want;
			if (!parts[3].empty())
				for (const std::string& k : split(parts[3], ';')) want.insert(k);
			std::set<std::string> got;
			for (const Predecessor& p : predecessors(b, strict)) got.insert(p.board.key());
			std::vector<std::string> only_here, only_python;
			std::set_difference(got.begin(), got.end(), want.begin(), want.end(), std::back_inserter(only_here));
			std::set_difference(want.begin(), want.end(), got.begin(), got.end(), std::back_inserter(only_python));
			const bool ok = only_here.empty() && only_python.empty();
			std::printf("  preds  %-70s %zu/%zu  %s\n", parts[1].c_str(), got.size(), want.size(), ok ? "identical" : "DIFFER");
			for (size_t i = 0; i < std::min<size_t>(3, only_here.size()); ++i)
				std::printf("      only here:   %s\n", only_here[i].c_str());
			for (size_t i = 0; i < std::min<size_t>(3, only_python.size()); ++i)
				std::printf("      only Python: %s\n", only_python[i].c_str());
			check(ok, "predecessor set of " + parts[1]);
		}
	}
	std::fclose(f);
}

struct Tally
{
	uint64_t transitions = 0;
	uint64_t missed = 0;
	uint64_t predecessors = 0;
	uint64_t unsound = 0;
	uint64_t quiet_missing = 0;
};

bool same_move(const Move& a, const Move& b)
{
	return a.from == b.from && a.to == b.to && a.promotion == b.promotion && a.flags == b.flags
	    && a.rook_from == b.rook_from;
}

// Every predecessor reached by a quiet move of a knight, bishop, rook, queen or
// king must be among the helper's candidates.
uint64_t quiet_helper_gaps(const Board& target, const std::vector<Predecessor>& preds)
{
	std::set<std::pair<int, int>> offered;
	for_each_quiet_unmove(target, [&](int now, int before) { offered.insert({ now, before }); });
	uint64_t gaps = 0;
	for (const Predecessor& p : preds)
	{
		const Move& m = p.move;
		if (m.is_capture() || m.is_castle() || m.promotion != NO_TYPE) continue;
		if (type_of(p.board.squares[m.from]) == PAWN) continue;
		if (!offered.count({ m.to, m.from })) ++gaps;
	}
	return gaps;
}

void bijection_suite(const char* name, const char* fen, const Retro_Options& opt, bool quick, Tally& total)
{
	const Board root = parse(fen);
	std::vector<Board> nodes{ root };
	for (const Move& m : root.legal_moves())
	{
		Board child = root;
		child.make(m);
		nodes.push_back(child);
	}
	if (quick && nodes.size() > 8) nodes.resize(8);

	Tally t;
	for (const Board& q : nodes)
	{
		if (!is_valid(q, opt.validity, opt.chess960)) continue;
		const std::string q_key = q.key();

		for (const Move& m : q.legal_moves())
		{
			Board p = q;
			p.make(m);
			bool found = false;
			for (const Predecessor& pred : predecessors(p, opt))
				if (pred.board.key() == q_key) { found = true; break; }
			++t.transitions;
			if (!found)
			{
				++t.missed;
				if (t.missed <= 3)
					std::printf("      MISSED  %s  %s  ->  %s\n", q_key.c_str(), m.uci().c_str(), p.key().c_str());
			}
		}

		const std::vector<Predecessor> preds = predecessors(q, opt);
		for (const Predecessor& pred : preds)
		{
			++t.predecessors;
			bool legal = false;
			for (const Move& mm : pred.board.legal_moves())
				if (same_move(mm, pred.move)) legal = true;
			Board after = pred.board;
			after.make(pred.move);
			const bool ok = legal && after.key() == q_key && is_valid(pred.board, opt.validity, opt.chess960);
			if (!ok)
			{
				++t.unsound;
				if (t.unsound <= 3)
					std::printf("      UNSOUND %s  %s  ->  %s\n", pred.board.key().c_str(),
						pred.move.uci().c_str(), q_key.c_str());
			}
		}
		t.quiet_missing += quiet_helper_gaps(q, preds);
	}

	const bool pass = t.missed == 0 && t.unsound == 0 && t.quiet_missing == 0;
	std::printf("  %-12s complete %7llu/%-7llu  sound %6llu/%-6llu  quiet-helper gaps %llu  %s\n", name,
		static_cast<unsigned long long>(t.transitions - t.missed), static_cast<unsigned long long>(t.transitions),
		static_cast<unsigned long long>(t.predecessors - t.unsound), static_cast<unsigned long long>(t.predecessors),
		static_cast<unsigned long long>(t.quiet_missing), pass ? "PASS" : "FAIL");
	check(pass, std::string("bijection ") + name);
	total.transitions += t.transitions;
	total.missed += t.missed;
	total.predecessors += t.predecessors;
	total.unsound += t.unsound;
	total.quiet_missing += t.quiet_missing;
}

const std::vector<std::pair<const char*, const char*>> STANDARD_SUITE = {
	{ "startpos",  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" },
	{ "kiwipete",  "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1" },
	{ "ep-rich",   "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1" },
	{ "promotion", "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1" },
	{ "castling",  "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1" },
	{ "pos4",      "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1" },
	{ "pos5",      "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8" },
	{ "ep-target", "rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 3" },
};

const std::vector<std::pair<const char*, const char*>> CHESS960_SUITE = {
	{ "960-a",      "bqnb1rkr/pp3ppp/3ppn2/2p5/5P2/P2P4/NPP1P1PP/BQ1BNRKR w HFhf - 2 9" },
	{ "960-b",      "2nnrbkr/p1qppppp/8/1ppb4/6PP/3PP3/PPP2P2/BQNNRBKR w HEhe - 1 9" },
	{ "960-inner",  "1r2k1r1/8/8/8/8/8/8/1R2K1R1 w GBgb - 0 1" },
	{ "960-onhome", "2r1kr2/8/8/8/8/8/8/1R4KR w Hfc - 0 1" },
	{ "960-king-g", "4k3/8/8/8/8/8/8/5RKR w H - 0 1" },
	{ "960-king-b", "4k3/8/8/8/8/8/8/RKR5 w A - 0 1" },
};

void run_bijection(bool quick)
{
	Tally total;
	std::printf("\n[bijection] standard chess, table validity\n");
	for (const auto& [name, fen] : STANDARD_SUITE)
		bijection_suite(name, fen, Retro_Options{ false, Validity::TABLE }, quick, total);
	std::printf("\n[bijection] standard chess, python-chess validity\n");
	for (const auto& [name, fen] : STANDARD_SUITE)
		bijection_suite(name, fen, Retro_Options{ false, Validity::STRICT }, quick, total);
	std::printf("\n[bijection] Chess960\n");
	for (const auto& [name, fen] : CHESS960_SUITE)
		bijection_suite(name, fen, Retro_Options{ true, Validity::TABLE }, quick, total);
	std::printf("  %-12s complete %7llu/%-7llu  sound %6llu/%-6llu\n", "total",
		static_cast<unsigned long long>(total.transitions - total.missed),
		static_cast<unsigned long long>(total.transitions),
		static_cast<unsigned long long>(total.predecessors - total.unsound),
		static_cast<unsigned long long>(total.predecessors));
}

}  // namespace

int main(int argc, char** argv)
{
	bool quick = false;
	std::string expected = "retro_expected.txt";
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--quick") quick = true;
		else if (a == "--expected" && i + 1 < argc) expected = argv[++i];
		else
		{
			std::printf("usage: %s [--quick] [--expected FILE]\n", argv[0]);
			return 2;
		}
	}

	const auto t0 = std::chrono::steady_clock::now();
	run_perft("standard chess, published counts", STANDARD_PERFT, quick ? 3 : 4);
	run_perft("Chess960, python-chess counts", CHESS960_PERFT, 3);
	run_bijection(quick);
	run_python_parity(quick, expected);
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

	std::printf("\n%s (%d failure%s, %.1f s)\n", g_failures ? "FAILED" : "PASSED",
		g_failures, g_failures == 1 ? "" : "s", secs);
	return g_failures ? 1 : 0;
}
