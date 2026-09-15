#pragma once

// A reference endgame tablebase generator, written to be checked by reading
// rather than to be fast, and sharing no code with chesstb.
//
// It solves every metric chesstb stores, from its definition:
//   WDL    win, cursed win, draw, blessed loss or loss under the fifty-move rule
//   DTZ    plies to the next zeroing move (a capture or a pawn move)
//   DTM    plies to mate, ignoring the fifty-move rule
//   DTM50  plies to mate under the fifty-move rule, at every halfmove clock
//   DTC    plies to the next zeroing move when the winning side may spend at most
//          k pawn pushes before a capture or promotion, for every budget k
//
// Materials may hold pawns, castling rights ('r': a rook with its right) and
// opposing pawn pairs ('p', which here are two ordinary pawns). Positions are
// stored without an en passant square, as chesstb stores them; a double push is
// valued with the opponent's en passant replies taken into account.
//
// Values are settled in increasing distance, one pass per ply, and each is
// written once, when nothing still unsettled could improve it. Which positions
// a pass looks at is decided by un-moving from the positions the previous pass
// settled; with Generator_Options::verify every settled value is afterwards
// re-derived from its children, so a scheduling slip cannot pass unnoticed.

#include "board.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ref {

// Classes in ascending order of preference for the side to move.
enum struct Wdl : uint8_t
{
	LOSE = 0,
	BLESSED_LOSS = 1,
	DRAW = 2,
	CURSED_WIN = 3,
	WIN = 4,
	INVALID = 7,   // not a legal, canonical entry
};

inline constexpr uint16_t MAX_CLEAN_PLIES = 100;
inline constexpr int HMC_LAYERS = 100;
inline constexpr int DTC_BUDGETS = 29;

// A distance with its sign: a win or a loss for the side to move, or a draw.
struct Outcome
{
	enum Kind : uint8_t { LOSS = 0, DRAW = 1, WIN = 2 };
	Kind kind = DRAW;
	uint16_t plies = 0;

	bool operator==(const Outcome& o) const { return kind == o.kind && (kind == DRAW || plies == o.plies); }
	bool operator!=(const Outcome& o) const { return !(*this == o); }
};

// DTM and DTM50 values: 0 a draw, 0x8000 | plies a win, 0x4000 | plies a loss.
inline uint16_t pack(Outcome o)
{
	if (o.kind == Outcome::DRAW) return 0;
	return static_cast<uint16_t>((o.kind == Outcome::WIN ? 0x8000 : 0x4000) | o.plies);
}

inline Outcome unpack(uint16_t v)
{
	if (v & 0x8000) return { Outcome::WIN, static_cast<uint16_t>(v & 0x0FFF) };
	if (v & 0x4000) return { Outcome::LOSS, static_cast<uint16_t>(v & 0x0FFF) };
	return {};
}

// DTC values fit a byte, since a budget layer holds nothing past 100 plies:
// 0 a draw, 1 + plies a win, 102 + plies a loss.
inline uint8_t dtc_pack(Outcome o)
{
	if (o.kind == Outcome::DRAW) return 0;
	return static_cast<uint8_t>((o.kind == Outcome::WIN ? 1 : 102) + o.plies);
}

inline Outcome dtc_unpack(uint8_t v)
{
	if (v == 0 || v > 202) return {};
	if (v <= 101) return { Outcome::WIN, static_cast<uint16_t>(v - 1) };
	return { Outcome::LOSS, static_cast<uint16_t>(v - 102) };
}

struct Material
{
	// counts[color][type], PAWN through QUEEN; one king per side is implied.
	std::array<std::array<uint8_t, 7>, 2> counts{};
	// Castling rights held per side, 0 to 2, each belonging to one of its rooks.
	std::array<uint8_t, 2> rights{};

	// chesstb spelling, white's men first: KRPKR, KrKR, KpKp. A pair material
	// parses to its two pawns, since the pair changes only chesstb's index.
	static bool parse(const std::string& name, Material& out, std::string* error = nullptr);
	std::string name() const;

	static Material of(const Board& b);

	int men() const;
	bool has_pawns() const;
	bool has_rights() const { return rights[WHITE] + rights[BLACK] > 0; }
	bool bare_kings() const;
	uint64_t key() const;

	bool operator==(const Material& o) const { return counts == o.counts && rights == o.rights; }
	bool operator!=(const Material& o) const { return !(*this == o); }
};

enum struct Symmetry
{
	NONE,          // castling rights fix the board's orientation
	FILE_MIRROR,   // pawns fix the ranks; the board may be mirrored left to right
	DIHEDRAL,      // no pawns and no rights: all eight symmetries of the square
};

// How the positions of one material are numbered.
//
// An entry is ((slice * within_slice + placement) * 2 + side to move). The slice
// numbers the pawn placement, the placement numbers everyone else. Men of the
// same colour and type are interchangeable and stored in ascending square
// order. The white king is confined to the part of the board a symmetry leaves
// distinct; where two images of a position both qualify, the smaller entry is
// canonical. A king holding rights and a rook holding one are stored by file.
struct Layout
{
	struct Group
	{
		Color color;
		Piece_Type type;
		bool holds_right;
		int count;
	};

	Material material;
	Symmetry symmetry = Symmetry::NONE;
	std::vector<Group> groups;   // kings, then other men, pawns last
	int pawn_count = 0;
	uint64_t slices = 1;
	uint64_t within_slice = 1;

	explicit Layout(const Material& m);

	uint64_t size() const { return slices * within_slice * 2; }
	uint64_t slice_entries() const { return within_slice * 2; }
	uint64_t slice_of(uint64_t entry) const { return entry / 2 / within_slice; }

	// The position of an entry; false where men collide, or where rooks cannot
	// hold the material's castling rights.
	bool decode(uint64_t entry, Board& out) const;

	// The canonical entry for `b`, which must be of this material, or
	// UINT64_MAX. Any en passant square is ignored.
	uint64_t canonical_entry(const Board& b) const;

	// Total pawn advancement of a slice; every pawn move increases it.
	int slice_advancement(uint64_t slice) const;

private:
	uint64_t entry_under(const Board& b, int transform) const;
};

// What is kept for one material. DTM50 layers other than halfmove clock 0, and
// DTC budget layers, are passed to a Sink as they are solved instead.
struct Table
{
	Layout layout;
	std::vector<Wdl> wdl;
	std::vector<uint16_t> dtz;       // plies; 0 for a draw or a mated position
	std::vector<uint16_t> dtm;       // packed Outcome
	std::vector<uint16_t> dtm50_0;   // packed Outcome at halfmove clock 0

	explicit Table(const Material& m) : layout(m) {}
};

struct Sink
{
	virtual ~Sink() = default;

	// WDL, DTZ and DTM are solved and final.
	virtual void on_table(const Table&) {}

	// DTM50 at one halfmove clock for the listed slices. layer[i * slice_entries
	// + offset] is the packed Outcome of entry slices[i] * slice_entries + offset.
	virtual void on_dtm50_layer(const Table&, int hmc, const std::vector<uint64_t>& slices,
	                            const std::vector<uint16_t>& layer)
	{
		(void)hmc; (void)slices; (void)layer;
	}

	// DTC at one budget for every entry, as dtc_pack values.
	virtual void on_dtc_budget(const Table&, int budget, const std::vector<uint8_t>& layer)
	{
		(void)budget; (void)layer;
	}

	virtual void on_log(const std::string& line) { (void)line; }
};

struct Generator_Options
{
	int threads = 1;
	bool dtm = true;
	bool dtm50 = true;
	bool dtc = true;
	// Re-derive every settled WDL, DTZ, DTM and DTC value from its children and
	// count those that disagree.
	bool verify = false;
	// Refuse a material whose own tables would need more than this.
	uint64_t max_bytes = uint64_t{ 12 } << 30;
};

class Generator
{
public:
	explicit Generator(Generator_Options opt);
	~Generator();

	// Solve `target`, first solving every material it can reach. Results for
	// `target` go to `sink`, and with `report_closure` so do those for the
	// materials it depends on.
	bool build(const Material& target, Sink& sink, bool report_closure, std::string* error);

	const Table* table(const Material& m) const;

	// Materials one move can lead to: captures, promotions and moves that give
	// up castling rights, over-approximated where rights are involved. Bare
	// kings are left out.
	static std::vector<Material> successors(const Material& m);

	uint64_t verify_failures() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

}  // namespace ref
