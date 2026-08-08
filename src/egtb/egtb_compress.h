#pragma once

#include "egtb/egtb_entry.h"
#include "egtb/egtb_format.h"
#include "egtb/piece_config_for_gen.h"

#include "util/defines.h"
#include "util/param.h"
#include "util/span.h"
#include "util/thread_pool.h"
#include "util/compress.h"

constexpr size_t WDL_BLOCK_SIZE   =   64 * 1024;
constexpr size_t DTC_BLOCK_SIZE   = 1024 * 1024;
constexpr size_t DTM_BLOCK_SIZE   = 1024 * 1024;
constexpr size_t DTM50_BLOCK_SIZE = 1024 * 1024;

// hist_1b: indexed by dtc_value_for_storage (cursed halved).
// hist_2b: indexed by raw e.value().
// One gather pass populates both; entry_bytes picks the tier based on rank_1b.ranks.size() <= 256.
struct Value_Histogram
{
	static constexpr size_t HIST_BINS = 2048;

	std::array<uint64_t, HIST_BINS> hist_1b{};  // indexed by halved storage value
	std::array<uint64_t, HIST_BINS> hist_2b{};  // indexed by raw value
};

// Frequency-ranked value permutation. Built per-tier from its hist.
// Compress writes ranks; load reverse-maps so probe path is unchanged.
struct Value_Rank_Table
{
	// ranks[r] = value at rank r (descending frequency).
	std::vector<uint16_t> ranks;

	// value_to_rank[v] = rank for value v, or NO_RANK if absent.
	std::vector<uint16_t> value_to_rank;

	static constexpr uint16_t NO_RANK = 0xFFFFu;

	NODISCARD static Value_Rank_Table build(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist);
};

struct Compressed_EGTB
{
	static Compressed_EGTB make_singular(uint8_t sv)
	{
		Compressed_EGTB info{};
		info.set_singular(sv);
		return info;
	}

	Compressed_EGTB(
		Compressed_Block_Store&& compressed_blocks,
		size_t src_blk_sz,
		size_t tail_blk_sz,
		std::optional<LZ4_Dict> d,
		size_t entry_bytes,
		Value_Rank_Table rank_table = {}
	);

	Compressed_EGTB() :
		m_is_singular(false),
		m_entry_bytes(0),
		m_single_val(0),
		m_block_size(0),
		m_tail_size(0),
		m_total_compressed_size(0)
	{
	}

	NODISCARD bool is_singular() const
	{
		return m_is_singular;
	}

	NODISCARD uint8_t single_val() const
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

	NODISCARD const Compressed_Block_Store& compressed_blocks() const
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

	uint8_t m_single_val;

	size_t m_block_size;
	size_t m_tail_size;

	Compressed_Block_Store m_compressed_blocks;
	size_t m_total_compressed_size;

	std::optional<LZ4_Dict> m_dict;
	Value_Rank_Table m_rank_table;

	void set_singular(uint8_t val)
	{
		m_is_singular = true;
		m_single_val = val;
	}
};

NODISCARD bool prepare_packed_wdl_entries_for_compression(Span<Packed_WDL_Entries> data);

// Maps raw entry bits to the tier-specific on-disk storage value
// (see dtc_storage_fn / dtm_storage_fn). Shared by the rank compressors.
using Storage_Fn = uint16_t(*)(uint16_t /*raw_bits*/, size_t /*entry_bytes*/);

// LZMA + rank-encoded compressor for 16-bit entries. ILLEGAL cells are
// run-stitched with neighboring legal ranks pre-compress. `storage_fn` maps
// raw bits to tier-specific on-disk value (see dtc_storage_fn / dtm_storage_fn).
struct LZMA_Rank_Compress_Helper : public Compress_Helper
{
	// `rank_table` must outlive every clone (captured by pointer).
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
	std::vector<uint8_t> m_scratch;
};

// DTC: 2-byte raw (low 11 bits), 1-byte halves cursed (dtc_value_for_storage).
// DTM: both tiers halve via dtm_value_for_storage (parity-lossless).
NODISCARD inline uint16_t dtc_storage_fn(uint16_t bits, size_t entry_bytes)
{
	DTC_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	if (e.is_draw()) return DTC_Final_Entry::ILLEGAL_VAL;
	if (entry_bytes == 2) return e.value();
	return dtc_value_for_storage(e);
}

NODISCARD inline uint16_t dtm_storage_fn(uint16_t bits, size_t /*entry_bytes*/)
{
	DTM_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	if (e.is_draw()) return DTM_Final_Entry::ILLEGAL_VAL;
	return dtm_value_for_storage(e);
}

NODISCARD std::optional<LZ4_Dict> make_dict_for_wdl(
	const Block_Source& src,
	size_t block_size
);

NODISCARD uint32_t choose_storage_permutation_config(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	const std::function<Block_Source(uint32_t)>& make_source,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor,
	size_t max_samples,
	const char* task_name
);

// Caller handles WDL singularity via pre-probe; this compresses unconditionally.
// max_workers caps pool fan-out (0 = unlimited).
NODISCARD Compressed_EGTB save_compress_wdl(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	std::filesystem::path spill_path,
	size_t max_workers = 0
);

NODISCARD Compressed_EGTB save_compress_egtb(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	size_t entry_bytes,
	size_t block_size,
	std::filesystem::path spill_path,
	size_t max_workers = 0,
	Value_Rank_Table rank_table = {},
	Storage_Fn storage_fn = &dtc_storage_fn
);

void save_wdl_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
);

void save_egtb_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
);
