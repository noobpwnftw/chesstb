#pragma once

#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"

#include "chess/piece_config.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/fixed_vector.h"
#include "util/span.h"
#include "util/thread_pool.h"
#include "util/compress.h"

#include <filesystem>

#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

constexpr uint8_t EGTB_SINGULAR_FLAG = 0x80;
constexpr uint64_t EGTB_CHECKSUM_INIT_VALUE = 0xf0f0f0f0f0f0;

constexpr size_t WDL_BLOCK_SIZE = 64 * 1024;

// Number of hmc (50MR half-move counter) layers stored per DTM50 sub-table.
// Lives here rather than egtb_gen_dtm50.h because both the generator-side
// flat sub-loader (egtb_compress.cpp) and the probe-side block decoder
// (probe.cpp) need it for the .lzdtm50 block-payload format math, and neither
// can pull in egtb_gen_dtm50.h.
inline constexpr int DTM50_HMC_COUNT = 100;

// Per-color histograms gathered alongside W/D/L tally. hist_1b is indexed by
// dtc_value_for_storage(e) (cursed halved); hist_2b is indexed by raw e.value().
// Both are populated in one gather pass; the entry_bytes decision picks which
// rank table to encode against based on rank_1b.ranks.size() <= 256.
struct Value_Histogram
{
	static constexpr size_t HIST_BINS = 2048;

	std::array<uint64_t, HIST_BINS> hist_1b{};  // indexed by halved storage value
	std::array<uint64_t, HIST_BINS> hist_2b{};  // indexed by raw value
};

// Frequency-ranked permutation of values. Built per-tier from its hist:
// rank_1b is keyed by halved storage values, rank_2b by raw values. Compress
// writes ranks; load reverse-maps so the probe path is unchanged.
struct Value_Rank_Table
{
	// ranks[r] = value at rank r (descending frequency).
	std::vector<uint16_t> ranks;

	// value_to_rank[v] = rank for value v, or NO_RANK if absent.
	std::vector<uint16_t> value_to_rank;

	static constexpr uint16_t NO_RANK = 0xFFFFu;

	NODISCARD static Value_Rank_Table build_1b(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist);
	NODISCARD static Value_Rank_Table build_2b(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist);
};

struct Compressed_EGTB
{
	static Compressed_EGTB make_singular(WDL_Entry sv)
	{
		Compressed_EGTB info{};
		info.set_singular(sv);
		return info;
	}

	Compressed_EGTB(
		std::vector<std::vector<uint8_t>>&& compressed_blocks,
		size_t src_blk_sz,
		size_t tail_blk_sz,
		std::optional<LZ4_Dict> d,
		size_t entry_bytes,
		Value_Rank_Table rank_table = {}
	);

	Compressed_EGTB() :
		m_is_singular(false),
		m_entry_bytes(0),
		m_single_val(WDL_Entry::DRAW),
		m_block_size(0),
		m_tail_size(0),
		m_total_compressed_size(0)
	{
	}

	NODISCARD bool is_singular() const
	{
		return m_is_singular;
	}

	NODISCARD WDL_Entry single_val() const
	{
		ASSERT(m_is_singular);
		return m_single_val;
	}

	NODISCARD size_t block_size() const
	{
		return m_block_size;
	}

	NODISCARD size_t tail_size() const
	{
		return m_tail_size;
	}

	NODISCARD const auto& compressed_blocks() const
	{
		return m_compressed_blocks;
	}

	NODISCARD size_t total_compressed_size() const
	{
		return m_total_compressed_size;
	}

	NODISCARD const auto& dict() const
	{
		return m_dict;
	}

	NODISCARD size_t entry_bytes() const
	{
		return m_entry_bytes;
	}

	NODISCARD size_t num_blocks() const
	{
		return m_compressed_blocks.size();
	}

	NODISCARD const Value_Rank_Table& rank_table() const
	{
		return m_rank_table;
	}

private:
	bool m_is_singular;
	uint8_t m_entry_bytes;

	WDL_Entry m_single_val;

	size_t m_block_size;
	size_t m_tail_size;

	std::vector<std::vector<uint8_t>> m_compressed_blocks;
	size_t m_total_compressed_size;

	std::optional<LZ4_Dict> m_dict;
	Value_Rank_Table m_rank_table;

	void set_singular(WDL_Entry val)
	{
		m_is_singular = true;
		m_single_val = val;
	}
};

NODISCARD bool prepare_packed_wdl_entries_for_compression(Span<Packed_WDL_Entries> data);

// DTC compressor (LZMA + run-stitch fill). Same input as the PPMd variant;
// helper rank-lookups, fills each illegal with the previous legal rank, packs
// at entry_bytes width, and bulk-LZMA-compresses.
// Generalized LZMA + rank-encoded compressor for 16-bit table entries. The
// `storage_fn` argument maps raw entry bits to their on-disk storage form for
// a given tier. DTC's 2-byte tier stores raw low-11-bit values and the 1-byte
// tier halves cursed only (dtc_storage_fn). DTM halves every classified value
// in BOTH tiers via the parity invariant (dtm_storage_fn) — class is folded
// in from the .lzw companion at decode so storage is lossless either way.
struct LZMA_Rank_Compress_Helper : public Compress_Helper
{
	using Storage_Fn = uint16_t(*)(uint16_t /*raw_bits*/, size_t /*entry_bytes*/);

	// `rank_table` must outlive every clone of this helper (capture by pointer).
	LZMA_Rank_Compress_Helper(const Value_Rank_Table& rank_table, size_t entry_bytes,
	                          Storage_Fn storage_fn)
		: m_rank_table(&rank_table), m_entry_bytes(entry_bytes),
		  m_storage_fn(storage_fn) {}

	NODISCARD size_t compress_bound(size_t source_size) const override
	{
		const size_t entries = source_size / sizeof(uint16_t);
		return m_lzma.compress_bound(entries * m_entry_bytes);
	}

	NODISCARD size_t source_bytes_per_block(size_t output_block_bytes) const override
	{
		return output_block_bytes * sizeof(uint16_t) / m_entry_bytes;
	}

	NODISCARD std::vector<uint8_t> compress(Const_Span<uint8_t> src) override
	{
		std::vector<uint8_t> out(compress_bound(src.size()));
		const size_t n = compress(Span<uint8_t>(out.data(), out.size()), src);
		out.resize(n);
		return out;
	}

	NODISCARD size_t compress(Span<uint8_t> dest, Const_Span<uint8_t> src) override;

	NODISCARD std::unique_ptr<Compress_Helper> clone() const override
	{
		return std::make_unique<LZMA_Rank_Compress_Helper>(*m_rank_table, m_entry_bytes, m_storage_fn);
	}

private:
	const Value_Rank_Table* m_rank_table;
	size_t m_entry_bytes;
	Storage_Fn m_storage_fn;
	LZMA_Compress_Helper m_lzma;
	std::vector<uint8_t> m_scratch;  // per-instance rank buffer, reused per block
};

// Storage helpers for LZMA_Rank_Compress_Helper. DTC: 2-byte tier is raw
// (low 11 bits), 1-byte tier halves cursed via dtc_value_for_storage.
// DTM: both tiers halve via dtm_value_for_storage (parity-lossless).
NODISCARD inline uint16_t dtc_storage_fn(uint16_t bits, size_t entry_bytes)
{
	if (entry_bytes == 2) return bits & DTC_Final_Entry::VALUE_MASK;
	DTC_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	return dtc_value_for_storage(e);
}

NODISCARD inline uint16_t dtm_storage_fn(uint16_t bits, size_t /*entry_bytes*/)
{
	DTM_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	return dtm_value_for_storage(e);
}

NODISCARD std::optional<LZ4_Dict> make_dict_for_wdl(
	const Block_Source& src,
	size_t block_size
);

// Caller handles WDL singularity (via a pre-probe); this function compresses
// unconditionally. `max_workers` caps pool fan-out (0 = unlimited).
NODISCARD Compressed_EGTB save_compress_wdl(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	size_t max_workers = 0
);

// silent=true suppresses the per-block progress bar and the "singular" line;
// callers (e.g. DTM50's per-hmc save loop) print their own one-line indicator.
NODISCARD Compressed_EGTB save_compress_egtb(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	const EGTB_Info& info,
	size_t entry_bytes,
	size_t block_size,
	size_t max_workers = 0,
	Value_Rank_Table rank_table = {},
	LZMA_Rank_Compress_Helper::Storage_Fn storage_fn = &dtc_storage_fn,
	bool silent = false
);

void save_wdl_table(
	const Piece_Config& ps,
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
);

void save_egtb_table(
	const Piece_Config& ps,
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
);

// Parse the WDL .lzw header into wdl's block index + per-color metadata.
// Blocks stay compressed; decoded on demand via the per-thread cache in read().
void load_wdl_table(
	Out_Param<WDL_File_For_Probe> wdl,
	const Piece_Config& ps,
	std::filesystem::path sub_wdl,
	EGTB_Magic wdl_magic
);
