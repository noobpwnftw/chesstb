#pragma once

#include "egtb/egtb_entry.h"
#include "egtb/egtb_format.h"
#include "egtb/piece_config_for_gen.h"

#include "util/defines.h"
#include "util/memory.h"
#include "util/param.h"
#include "util/span.h"
#include "util/thread_pool.h"
#include "util/compress.h"

inline constexpr size_t WDL_BLOCK_SIZE   =   64 * 1024;
inline constexpr size_t DTZ_BLOCK_SIZE   = 1024 * 1024;
inline constexpr size_t DTC_BLOCK_SIZE   = 1024 * 1024;
inline constexpr size_t DTM_BLOCK_SIZE   = 1024 * 1024;
inline constexpr size_t DTM50_BLOCK_SIZE = 1024 * 1024;

// A tail is one short of a block, and each header stores it outright.
static_assert(WDL_BLOCK_SIZE   - 1 <= UINT16_MAX);
static_assert(DTZ_BLOCK_SIZE   - 1 <= UINT32_MAX);
static_assert(DTC_BLOCK_SIZE   - 1 <= UINT32_MAX);
static_assert(DTM_BLOCK_SIZE   - 1 <= UINT32_MAX);
static_assert(DTM50_BLOCK_SIZE - 1 <= UINT32_MAX);

// hist_1b: indexed by dtz_value_for_storage (cursed halved).
// hist_2b: indexed by raw e.value().
// One gather pass populates both;
// entry_bytes picks the tier based on rank_1b.rank_to_value.size() <= 256.
struct Value_Histogram
{
	static constexpr size_t HIST_BINS = 2048;
	static_assert(DTZ_Final_Entry::ILLEGAL_VAL == DTM_Final_Entry::ILLEGAL_VAL);
	static_assert(DTZ_Final_Entry::ILLEGAL_VAL < HIST_BINS);

	std::array<uint64_t, HIST_BINS> hist_1b{};  // indexed by halved storage value
	std::array<uint64_t, HIST_BINS> hist_2b{};  // indexed by raw value
};

// Frequency-ranked value permutation. Built per-tier from its hist.
// Compress writes ranks; load reverse-maps so probe path is unchanged.
struct Value_Rank_Table
{
	static constexpr size_t LUT_SIZE = Value_Histogram::HIST_BINS;
	static constexpr uint16_t NO_RANK = 0xFFFFu;

	// rank_to_value[r] = value at rank r (descending frequency).
	std::vector<uint16_t> rank_to_value;

	// value_to_rank[v] = rank for value v, or NO_RANK if absent. LUT_SIZE entries.
	std::vector<uint16_t> value_to_rank;

	NODISCARD static Value_Rank_Table build(const std::array<uint64_t, LUT_SIZE>& hist);
};

// Changepoint packing for a metric whose cell is a run of layers: per position
// the layer indices where the value changes, and the values at them. DTM50
// stacks 101 layers by halfmove clock; DTC has 29 separately solved budget
// points plus the terminal clean changepoint supplied by DTZ. Both put that
// embedded endpoint at index 0, followed by layers that only ever turn a cell
// DRAW, so both want the same states and the same trailing-DRAW hint.
//
// LAYERS is the stack height, and the MULTI changepoint bitmap is cut to it in
// whole 32-bit words: DTC's 30 layers take 4 bytes, DTM50's 101 take 16. k
// already shares its byte with the draw-end hint, and a word-sized width leaves
// the read side masking without a per-byte tail. The slack is zeros, which the
// LZMA pass eats.

// The pack ranks by layer count rather than frequency, and carries the width the
// ranks are written at; the table itself is the shared one.
struct Layered_Rank_Table : Value_Rank_Table
{
	uint8_t entry_bytes;

	Layered_Rank_Table() { value_to_rank.assign(LUT_SIZE, NO_RANK); }

	NODISCARD uint16_t rank_of(uint16_t value) const
	{
		ASSERT(value < LUT_SIZE);
		ASSERT(value_to_rank[value] != NO_RANK);
		return value_to_rank[value];
	}
};

INLINE void write_rank_bytes(std::vector<uint8_t>& dst, uint16_t r, uint8_t eb)
{
	if (eb == 1) {
		dst.push_back(static_cast<uint8_t>(r));
	} else {
		dst.push_back(static_cast<uint8_t>(r & 0xFF));
		dst.push_back(static_cast<uint8_t>(r >> 8));
	}
}

// Width of a MULTI record's changepoint bitmap for a stack of `layers`: whole
// 32-bit words, so both sides mask it a word at a time with no per-byte tail.
// DTC's 30 layers take 4 bytes, DTM50's 101 take 16.
NODISCARD constexpr size_t layered_bitmap_bytes(size_t layers)
{
	return ((layers + 31) / 32) * 4;
}

inline constexpr size_t DTM50_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTM50_PACK_LAYERS);
inline constexpr size_t DTC_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTC_PACK_LAYERS);

// Per-position state across the pack layers: 00=CONST, 01=SINGLE (1 transition),
// 10=DOUBLE (2), 11=MULTI (3+). ILLEGAL is folded into CONST as last_legal_rank
// to keep LZMA runs long.
//
// Draw-end hint: a cell that goes DRAW at some layer and stays there is the
// dominant pattern, whatever the layers count. Non-CONST states carry a hint bit;
// when set, the final rank is omitted and the decoder synthesizes DRAW. The bit
// lives in the MSB of the final h byte (SINGLE/DOUBLE) or of the change-count
// byte (MULTI; k fits 7 bits). DRAW arrives as the don't-care value and takes no
// rank slot, so storage 0 stays WIN-in-1 / LOSS-in-0 at every layer.
//
// Random access: state_bits popcount → j-th SINGLE/DOUBLE, and the loader
// reconstructs a hint bitmap from record MSBs to compute the byte offset.
// MULTI uses multi_dir since k varies.
template <size_t LAYERS>
struct Layered_Block_Encoder
{
	static_assert(LAYERS <= 127, "the changepoint count shares a byte with the draw-end hint");

	static constexpr size_t BITMAP_BYTES = layered_bitmap_bytes(LAYERS);

	// A don't-care takes no rank, so NO_RANK finds one at any layer: at layer 0 it
	// means the cell is DRAW or ILLEGAL outright, later it means the cell flipped
	// to DRAW. Both are the WDL companion's to answer.
	static constexpr uint16_t DRAW_SENTINEL = Layered_Rank_Table::NO_RANK;

	const Layered_Rank_Table* ranks = nullptr;
	// Stack height actually in use. A metric whose height varies per material
	// sets it; LAYERS is the ceiling the arrays and the bitmap are cut to.
	size_t layers = LAYERS;
	std::vector<uint8_t> state_bits;        // 2 bits/position
	std::vector<uint8_t> const_stream;      // 1 rank/position in state==CONST
	std::vector<uint8_t> single_stream;     // variable: 2 B or 1+2·eb B
	std::vector<uint8_t> double_stream;     // variable: 2+2·eb B or 2+3·eb B
	std::vector<uint32_t> multi_dir;        // cumulative byte offsets
	std::vector<uint8_t> multi_stream;      // [k|hint, bitmap, (k-hint) ranks]
	std::vector<uint8_t> out;

	NODISCARD Const_Span<uint8_t> encode(const uint16_t* values, size_t num_positions)
	{
		ASSERT(ranks != nullptr);
		ASSERT(layers >= 1 && layers <= LAYERS);
		const uint8_t eb = ranks->entry_bytes;
		const size_t sb_bytes = (num_positions * 2 + 7) / 8;
		state_bits.assign(sb_bytes, 0);
		const_stream.clear();
		single_stream.clear();
		double_stream.clear();
		multi_dir.clear();
		multi_dir.push_back(0);
		multi_stream.clear();

		std::array<uint16_t, LAYERS> cp{};
		std::array<uint16_t, LAYERS> cr{};

		uint16_t last_legal_rank = 0;  // any rank works; ILLEGAL never decodes
		uint32_t num_single = 0;
		uint32_t num_double = 0;

		for (size_t i = 0; i < num_positions; ++i)
		{
			// A don't-care at layer 0 settles the cell; no later layer is read.
			const uint16_t first = ranks->value_to_rank[values[i]];
			if (first == DRAW_SENTINEL)
			{
				// CONST (state 00). Emit last_legal_rank to extend LZMA runs.
				write_rank_bytes(const_stream, last_legal_rank, eb);
				continue;
			}

			size_t k = 0;
			uint16_t prev = first;
			cp[k] = 0; cr[k] = prev; ++k;
			for (size_t h = 1; h < layers; ++h)
			{
				const uint16_t r = ranks->value_to_rank[values[h * num_positions + i]];
				if (r == prev) continue;
				cp[k] = static_cast<uint16_t>(h); cr[k] = r; prev = r; ++k;
				if (r == DRAW_SENTINEL) break;
			}

			if (k == 1)
			{
				ASSERT(cr[0] != DRAW_SENTINEL);
				last_legal_rank = cr[0];
				write_rank_bytes(const_stream, cr[0], eb);
			}
			else if (k == 2)
			{
				set_state(i, 1);
				const uint16_t h1 = cp[1];
				ASSERT(h1 >= 1 && h1 <= layers - 1);
				ASSERT(cr[0] != DRAW_SENTINEL);  // monotonicity: DRAW is terminal
				const bool draw_end = (cr[1] == DRAW_SENTINEL);
				single_stream.push_back(
					static_cast<uint8_t>(h1 | (draw_end ? 0x80u : 0u)));
				write_rank_bytes(single_stream, cr[0], eb);
				if (!draw_end)
				{
					write_rank_bytes(single_stream, cr[1], eb);
					last_legal_rank = cr[1];
				}
				++num_single;
			}
			else if (k == 3)
			{
				set_state(i, 2);
				const uint16_t h1 = cp[1], h2 = cp[2];
				ASSERT(h1 >= 1 && h2 >= 2 && h1 < h2 && h2 <= layers - 1);
				ASSERT(cr[0] != DRAW_SENTINEL && cr[1] != DRAW_SENTINEL);
				const bool draw_end = (cr[2] == DRAW_SENTINEL);
				double_stream.push_back(static_cast<uint8_t>(h1));
				double_stream.push_back(
					static_cast<uint8_t>(h2 | (draw_end ? 0x80u : 0u)));
				write_rank_bytes(double_stream, cr[0], eb);
				write_rank_bytes(double_stream, cr[1], eb);
				if (!draw_end)
				{
					write_rank_bytes(double_stream, cr[2], eb);
					last_legal_rank = cr[2];
				}
				++num_double;
			}
			else
			{
				set_state(i, 3);
				const bool draw_end = (cr[k - 1] == DRAW_SENTINEL);
				for (size_t j = 0; j + 1 < k; ++j)
					ASSERT(cr[j] != DRAW_SENTINEL);
				const size_t base = multi_stream.size();
				multi_stream.push_back(
					static_cast<uint8_t>(k | (draw_end ? 0x80u : 0u)));
				uint8_t bm[BITMAP_BYTES] = { 0 };
				for (size_t j = 0; j < k; ++j)
				{
					const uint16_t h = cp[j];
					bm[h / 8] |= static_cast<uint8_t>(1u << (h % 8));
				}
				multi_stream.insert(multi_stream.end(), bm, bm + BITMAP_BYTES);
				const size_t to_write = draw_end ? (k - 1) : k;
				for (size_t j = 0; j < to_write; ++j)
					write_rank_bytes(multi_stream, cr[j], eb);
				if (!draw_end) last_legal_rank = cr[k - 1];
				multi_dir.push_back(static_cast<uint32_t>(multi_stream.size() - base));
			}
		}

		for (size_t i = 1; i < multi_dir.size(); ++i)
			multi_dir[i] += multi_dir[i - 1];

		const uint32_t np32 = static_cast<uint32_t>(num_positions);
		const uint32_t num_multi = static_cast<uint32_t>(multi_dir.size() - 1);

		const uint32_t ss_bytes32 = static_cast<uint32_t>(single_stream.size());
		const uint32_t ds_bytes32 = static_cast<uint32_t>(double_stream.size());

		size_t multi_dir_off =
			4 + 4 + 4 + 4 + 4 + 4                   // np, ns, nd, nm, ss_bytes, ds_bytes
			+ sb_bytes                              // state_bits (2 bpp)
			+ const_stream.size()                   // const_stream
			+ single_stream.size()                  // SINGLE records
			+ double_stream.size();                 // DOUBLE records
		multi_dir_off += (4 - (multi_dir_off & 3)) & 3;

		size_t total =
			multi_dir_off
			+ multi_dir.size() * 4                  // multi_dir (cumulative offsets)
			+ multi_stream.size();                  // multi_stream
		total += (4 - (total & 3)) & 3;

		out.resize(total);
		Serial_Memory_Writer w{ Span<uint8_t>(out) };
		w.write<uint32_t>(np32);
		w.write<uint32_t>(num_single);
		w.write<uint32_t>(num_double);
		w.write<uint32_t>(num_multi);
		w.write<uint32_t>(ss_bytes32);
		w.write<uint32_t>(ds_bytes32);
		w.write(Const_Span<uint8_t>(state_bits.data(), sb_bytes));
		if (!const_stream.empty())
			w.write(Const_Span<uint8_t>(const_stream.data(), const_stream.size()));
		if (!single_stream.empty())
			w.write(Const_Span<uint8_t>(single_stream.data(), single_stream.size()));
		if (!double_stream.empty())
			w.write(Const_Span<uint8_t>(double_stream.data(), double_stream.size()));
		ASSERT(w.num_bytes_written() <= multi_dir_off);
		w.zero_align(4);
		w.write(Const_Span<uint8_t>(
			reinterpret_cast<const uint8_t*>(multi_dir.data()),
			multi_dir.size() * 4));
		if (!multi_stream.empty())
			w.write(Const_Span<uint8_t>(multi_stream.data(), multi_stream.size()));
		w.zero_align(4);
		ASSERT(w.num_bytes_written() == total);
		return Const_Span<uint8_t>(out.data(), out.size());
	}

private:
	void set_state(size_t pos, uint8_t s)
	{
		const size_t bit_off = pos * 2;
		state_bits[bit_off / 8] |= static_cast<uint8_t>((s & 3u) << (bit_off % 8));
	}
};

using DTM50_Block_Encoder = Layered_Block_Encoder<DTM50_PACK_LAYERS>;
using DTC_Block_Encoder = Layered_Block_Encoder<DTC_PACK_LAYERS>;

// Either changepoint pack, whole stack in one file: DTM50's 101 layers (the
// unbounded DTM plus 100 hmc) as .lzdtm50; DTC's 30 (the clean terminal
// changepoint supplied by DTZ plus 29 separately solved predecessors) as
// .lzdtc. Design rationale (2-bit state, draw-end hint, skip blocks, DRAW
// excluded from the rank table) lives in README
// "Universal DTM", and the budget ordering in "DTC".
//
// File layout:
//   uint32  magic = the metric's, DTM50_MAGIC or DTC_MAGIC
//   uint32  key_and_table_num
//   per-color header:
//     uint8  flag (SINGULAR 0x80, DROPPED 0x40, LOSS_ONLY 0x20; the last two
//                  are mutually exclusive)
//     if DROPPED: nothing further, the prober rebuilds the frame
//     else if SINGULAR: uint8 0
//     else:
//       uint32 index_perm   (populated-class storage-order permutation index)
//       uint8  entry_bytes  (1 or 2)
//       uint32 block_positions
//       uint64 block_cnt
//       uint32 tail_positions
//       uint64 data_size
//       uint16 num_ranks
//       uint16 rank_to_value[num_ranks]    W/L storage values, frequency-sorted
//   per-color offset section (has_payload only): delta-coded succinct index
//     [u8 log2_bu, sample_width, offset_width, usz_width]
//     Mono_Uint_Vec blob over (block_cnt+1) cumulative offsets
//     Min0_Uint_Vec blob over block_cnt usz values
//     skip-block sentinel: get2(i)[0] == get2(i)[1].
//   align 64
//   per-color compressed data (has_payload only), ceil64-aligned tail
//   end-checksum (8 bytes, xxhash with EGTB_CHECKSUM_INIT_VALUE)
//
// Per-block uncompressed payload (LZMA-compressed before write):
//   uint32 num_positions
//   uint32 num_single, num_double, num_multi       (state==01/10/11 counts;
//                                                   num_const = np - others)
//   uint32 single_stream_bytes, double_stream_bytes
//   uint8  state_bits[ceil(np*2/8)]                2 bpp: 00 CONST  01 SINGLE
//                                                         10 DOUBLE 11 MULTI
//   uint8  const_stream[num_const * eb]            ILLEGAL emits last_legal_rank
//   uint8  single_stream[...]   per SINGLE: uint8 h|(draw_end ? 0x80 : 0),
//                                           rank r0, [rank r1 if !draw_end]
//   uint8  double_stream[...]   per DOUBLE: uint8 h1, uint8 h2|(draw_end?0x80:0),
//                                           rank r0, r1, [r2 if !draw_end]
//   align 4
//   uint32 multi_dir[num_multi + 1]                cumulative byte offsets
//   uint8  multi_stream[...]    per MULTI: uint8 k|(draw_end ? 0x80 : 0),
//                                          uint8 cp_bitmap[ceil(LAYERS/32)*4]
//                                                       (the stack height in
//                                                        bits used),
//                                          rank r0..r_{k-1}  (k-1 if draw_end)

struct Layered_Compressed_Color
{
	bool is_singular = false;
	bool is_dropped = false;

	// Neither state carries a payload section.
	NODISCARD bool has_payload() const { return !is_singular && !is_dropped; }

	Layered_Rank_Table ranks;
	uint32_t block_positions = 0;
	uint64_t block_cnt = 0;
	uint32_t tail_positions = 0;
	Compressed_Block_Store compressed_blocks;
	// Plain LZMA has no end-marker, so the decoder needs each block's usz.
	std::vector<uint64_t> usizes;
	size_t total_compressed_size = 0;
};

void save_layered_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Layered_Compressed_Color color_out[COLOR_NB],
	const std::filesystem::path& file_path,
	Fixed_Vector<Color, 2> colors,
	bool loss_only,
	EGTB_Magic magic);

struct Compressed_EGTB
{
	static Compressed_EGTB make_singular(uint8_t sv)
	{
		Compressed_EGTB info{};
		info.set_singular(sv);
		return info;
	}

	// A frame the prober rebuilds by one-ply minimax: a flag byte, no payload.
	static Compressed_EGTB make_dropped()
	{
		Compressed_EGTB info{};
		info.m_is_dropped = true;
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
		m_is_dropped(false),
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

	NODISCARD bool is_dropped() const
	{
		return m_is_dropped;
	}

	// Neither state carries a payload section.
	NODISCARD bool has_payload() const
	{
		return !m_is_singular && !m_is_dropped;
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
	bool m_is_dropped = false;
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

inline constexpr WDL_Stored NOT_RELAXED = WDL_Stored::ILLEGAL;

// `caps` is one entry per cell for a relaxed frame, or empty for an
// ordinary one.
NODISCARD bool prepare_wdl_entries_for_compression(
	Span<Packed_WDL_Entries> data, Const_Span<WDL_Stored> caps);

// Maps raw entry bits to the tier-specific on-disk storage value
// (see dtz_storage_fn / dtm_storage_fn). Shared by the rank compressors.
using Storage_Fn = uint16_t(*)(uint16_t /*raw_bits*/, size_t /*entry_bytes*/);

// LZMA + rank-encoded compressor for 16-bit entries. ILLEGAL cells are
// run-stitched with neighboring legal ranks pre-compress. `storage_fn` maps
// raw bits to tier-specific on-disk value (see dtz_storage_fn / dtm_storage_fn).
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

// Experiment switch only: the generator produces full tables, and a shipping
// form is a postprocessing concern. Flipping it stamps EGTB_LOSS_ONLY_FLAG,
// which tb_file_is_full_format rejects, so such a build cannot enter a later
// material's sub-table closure.
inline constexpr bool EGTB_GEN_LOSS_ONLY = false;

// The same switch for WDL's own don't-care, and the same caveat: such a build
// cannot enter a later material's sub-table closure.
inline constexpr bool EGTB_GEN_RELAXED = false;

// DTZ: 2-byte raw (low 11 bits), 1-byte halves cursed (dtz_value_for_storage).
// DTM: both tiers halve via dtm_value_for_storage (parity-lossless).
// Loss_Only additionally drops wins; each instantiation is its own Storage_Fn.
template <bool Loss_Only>
NODISCARD uint16_t dtz_storage_fn(uint16_t bits, size_t entry_bytes)
{
	DTZ_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	if (e.is_draw() || (Loss_Only && e.is_win())) return DTZ_Final_Entry::ILLEGAL_VAL;
	if (entry_bytes == 2) return e.value();
	return dtz_value_for_storage(e);
}

template <bool Loss_Only>
NODISCARD uint16_t dtm_storage_fn(uint16_t bits, size_t /*entry_bytes*/)
{
	DTM_Final_Entry e;
	std::memcpy(&e, &bits, sizeof(e));
	if (e.is_draw() || (Loss_Only && e.is_win())) return DTM_Final_Entry::ILLEGAL_VAL;
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
	size_t block_size,
	std::filesystem::path spill_path,
	size_t max_workers
);

NODISCARD Compressed_EGTB save_compress_egtb(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	size_t entry_bytes,
	size_t block_size,
	std::filesystem::path spill_path,
	size_t max_workers,
	Value_Rank_Table rank_table,
	Storage_Fn storage_fn
);

void save_wdl_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic,
	bool relaxed
);

void save_egtb_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic,
	bool loss_only
);
