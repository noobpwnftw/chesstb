#pragma once

// Probe .lzw/.lzdtc/.lzdtm/.lzdtm50 tables without depending on the generator.
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
		ILLEGAL_POS,
	};

	Status status = Status::TB_NOT_FOUND;
	WDL_Entry wdl = WDL_Entry::ILLEGAL;
	bool has_dtc = false;
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
	WDL_Entry wdl  = WDL_Entry::DRAW;
	int       dtz  = 0;   // +N means side-to-move wins in N plies
	int       rank = 0;
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
	void add_dtc_path(std::filesystem::path dir);
	void add_dtm_path(std::filesystem::path dir);
	void add_dtm50_path(std::filesystem::path dir);

	// Add `dir`, or its wdl/dtc/dtm/dtm50 subdirs when present, and update largest().
	bool init(const std::filesystem::path& dir);

	// Maximum piece count, including kings, found on disk.
	NODISCARD size_t largest() const;
	void rescan();

	// rule50 selects the DTM50 layer (default 0 = fresh-window). has_dtm50
	// stays false when no DTM50 file is on disk; callers can ignore the field.
	NODISCARD Probe_Result probe(const Position& pos, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Position& pos, Square ep_square, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Piece_Config& ps, const Position& pos, unsigned rule50 = 0);
	NODISCARD Probe_Result probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50 = 0);

	// Search-time WDL probe. Fathom semantics reject nonzero rule50.
	NODISCARD WDL_Entry probe_wdl(const Position& pos, Square ep_square, unsigned rule50);

	// Root rankers using Fathom tbRank/tbScore ordering. Empty on partial failure.
	NODISCARD std::vector<Root_Move> probe_root_dtz(
		const Position& pos, Square ep_square,
		unsigned rule50, bool use_rule50, bool has_repeated);
	NODISCARD std::vector<Root_Move> probe_root_wdl(
		const Position& pos, Square ep_square, bool use_rule50);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
