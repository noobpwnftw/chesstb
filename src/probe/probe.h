#pragma once

// Probe .lzw/.lzdtz/.lzdtc/.lzdtm/.lzdtm50 tables without depending on the generator.
//
// probe() and probe_root_*() are thread-safe after path setup. add_*_path()
// and init() must not run concurrently with probes.

#include "probe/entry.h"
#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/defines.h"

#include <filesystem>
#include <memory>
#include <vector>

struct Probe_Result
{
	enum struct Status : uint8_t
	{
		OK,
		TB_NOT_FOUND,
	};

	Status status = Status::TB_NOT_FOUND;
	WDL_Entry wdl = WDL_Entry::ILLEGAL;
	bool has_dtz = false;
	uint16_t dtz = 0;
	// DTC at the caller's rule50: the class that clock leaves, and where it is
	// decisive, the pushes the winner still owes with the plies to the next
	// zeroing move on the line that owes them. No budget fitting the clock is a
	// 50MR draw, which dtc_wdl reports rather than withholding, so has_dtc says
	// the metric is available and dtc_wdl what it found.
	bool has_dtc = false;
	WDL_Entry dtc_wdl = WDL_Entry::ILLEGAL;
	uint16_t dtc_order = 0;
	uint16_t dtc = 0;
	bool has_dtm = false;
	uint16_t dtm = 0;
	// DTM50 at the caller's rule50; cursed/blessed -> DRAW.
	bool has_dtm50 = false;
	WDL_Entry dtm50_wdl = WDL_Entry::ILLEGAL;
	uint16_t dtm50 = 0;
};

// Ranked legal move. Fields mirror Fathom's TbRootMove.
struct Root_Move
{
	Move      move;
	WDL_Entry wdl   = WDL_Entry::DRAW;
	int       dtz   = 0;   // +N means side-to-move wins in N plies
	int       rank  = 0;
	int       score = 0;
};

struct Probe_Tables
{
	Probe_Tables();
	~Probe_Tables();

	Probe_Tables(const Probe_Tables&) = delete;
	Probe_Tables& operator=(const Probe_Tables&) = delete;
	Probe_Tables(Probe_Tables&&) noexcept;
	Probe_Tables& operator=(Probe_Tables&&) noexcept;

	// Add a search directory. The loader uses the first hit per material.
	void add_wdl_path(std::filesystem::path dir);
	void add_dtz_path(std::filesystem::path dir);
	void add_dtc_path(std::filesystem::path dir);
	void add_dtm_path(std::filesystem::path dir);
	void add_dtm50_path(std::filesystem::path dir);

	// Add `dir`, or its wdl/dtz/dtc/dtm/dtm50 subdirs when present, and update largest().
	bool init(const std::filesystem::path& dir);

	// Maximum piece count, including kings, found on disk.
	NODISCARD size_t largest() const;
	void rescan();

	// Budget (bytes) for decoded blocks held across this instance's tables,
	// reclaimed least-recently-used. Safe to call while probes are running.
	void set_block_cache_bytes(size_t bytes);
	NODISCARD size_t block_cache_bytes() const;
	NODISCARD size_t block_cache_bytes_used() const;

	// rule50 selects the DTM50 layer and the DTC budget (default 0 = fresh
	// window). has_dtm50 and has_dtc stay false where no file answers; callers
	// can ignore either field.
	NODISCARD Probe_Result probe(const Position& pos, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Position& pos, Square ep_square, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Piece_Config& ps, const Position& pos, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50 = 0);

	// Search-time WDL probe. Fathom semantics reject nonzero rule50.
	NODISCARD WDL_Entry probe_wdl(const Position& pos, Square ep_square, unsigned rule50);
	NODISCARD WDL_Entry probe_wdl(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50);

	// Root rankers using Fathom tbRank/tbScore ordering. Empty on partial failure.
	//
	// The DTZ ranker prices its moves with DTC wherever a table carries it, the
	// pawnful materials, and reports it in the same three fields. `dtz` then
	// counts the plies to the next zeroing move on the line DTC recommends, the
	// one the clock affords with the fewest pawn pushes spent, and `rank` carries
	// those pushes: Fathom leaves 901..999 and -999..-901 unused, so a clean win
	// ranks 1000 minus the pushes still owed and a clean loss -1000 plus them.
	// Every ordering Fathom defines survives that, `score` is unchanged, and a
	// move no table prices keeps its rank exactly.
	NODISCARD std::vector<Root_Move> probe_root_dtz(
		const Position& pos, Square ep_square,
		unsigned rule50, bool use_rule50, bool has_repeated);
	NODISCARD std::vector<Root_Move> probe_root_wdl(
		const Position& pos, Square ep_square, bool use_rule50);
	// Rank legal moves by distance-to-mate. With use_rule50 the band is the
	// rule-true DTM50 verdict and clean wins/losses are ordered by the
	// 50MR-respecting mate distance; a forced mate that 50MR draws (cursed-by-table
	// or clock-expired) drops into a nerfed cursed/blessed band, ordered among
	// itself by flat DTM. Without use_rule50, 50MR is ignored: cursed folds to win,
	// blessed to loss, and every move is ranked by flat DTM. The dtz field carries
	// the signed mate distance in plies (+N = side to move mates in N). Empty on
	// partial failure, or if DTM50 is unavailable for any child when use_rule50 is
	// set (flat ranking needs only DTM).
	NODISCARD std::vector<Root_Move> probe_root_dtm(
		const Position& pos, Square ep_square, unsigned rule50, bool use_rule50);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
