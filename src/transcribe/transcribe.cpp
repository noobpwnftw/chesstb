#include "transcribe/transcribe.h"

#include "egtb/relax_bound.h"
#include "egtb/egtb_gen.h"
#include "egtb/egtb_probe.h"

#include "util/math.h"
#include "util/progress_bar.h"

#include <atomic>
#include <stdexcept>

namespace {

// A table's don't-care slots, from the index geometry -- the source file's
// run-stitch left its ILLEGAL cells holding a neighbour's value, so this cannot
// be read off the cell. Three tests, the ones the generator's init pass fills
// ILLEGAL with: an index gap, a duplicate representation on a king slice with a
// stabilizer, and a board not chess-legal for this side to move.
struct Legal_Slots
{
	Legal_Slots(const Piece_Config_For_Gen& epsi, Color color) :
		m_epsi(&epsi), m_pos(epsi, BOARD_INDEX_ZERO, color),
		m_has_stab(&epsi.king_slice_manager().slice_has_stabilizer) {}

	// Valid only right after operator() returned true.
	NODISCARD Position_For_Gen& position() { return m_pos; }

	NODISCARD bool operator()(size_t logical)
	{
		const Board_Index idx = static_cast<Board_Index>(logical);
		// Stepping is a mixed-radix increment where seeking is a decompose.
		if (m_prev != BOARD_INDEX_NONE && logical == static_cast<size_t>(m_prev) + 1)
			++m_pos;
		else
			m_pos.set_board_index(idx);
		m_prev = idx;
		// The full test opens with the layout check, so gaps fall out here too.
		if (!m_pos.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
			return false;
		return !(*m_has_stab)[m_pos.index().king_slice_id]
		    || board_index_of_position(*m_epsi, m_pos.board_unchecked()) == idx;
	}

private:
	const Piece_Config_For_Gen* m_epsi;
	Position_For_Gen m_pos;
	Board_Index m_prev = BOARD_INDEX_NONE;
	const decltype(King_Slice_Manager::slice_has_stabilizer)* m_has_stab;
};

// One bit per logical index, set where the slot holds a position. Geometry only:
// a stored cell's class still decides whether its value is real, which the rank
// sources settle per cell and the DTM50 gather folds into its own bitmap.
struct Legal_Bitmap
{
	std::vector<uint8_t> bits;

	NODISCARD bool operator()(size_t logical) const
	{
		return ((bits[logical >> 3] >> (logical & 7)) & 1u) != 0;
	}
	void set(size_t logical)
	{
		bits[logical >> 3] |= static_cast<uint8_t>(1u << (logical & 7));
	}
};

// A chunk is a whole number of bytes, so threads never share one.
constexpr size_t LEGAL_CHUNK_POSITIONS = size_t{ 1 } << 14;
static_assert(LEGAL_CHUNK_POSITIONS % 8 == 0);

NODISCARD Legal_Bitmap build_legal_bitmap(In_Out_Param<Thread_Pool> thread_pool,
                                          const Piece_Config_For_Gen& epsi, Color color)
{
	const size_t num_positions = epsi.num_positions();
	Legal_Bitmap out;
	out.bits.assign((num_positions + 7) / 8, 0);

	const size_t num_chunks = ceil_div(num_positions, LEGAL_CHUNK_POSITIONS);
	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), num_chunks));
	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
		Legal_Slots legal(epsi, color);
		for (;;)
		{
			const size_t k = next.fetch_add(1, std::memory_order_relaxed);
			if (k >= num_chunks) return;
			const size_t lo = k * LEGAL_CHUNK_POSITIONS;
			const size_t hi = std::min(lo + LEGAL_CHUNK_POSITIONS, num_positions);
			for (size_t i = lo; i < hi; ++i)
				if (legal(i)) out.set(i);
		}
	});
	return out;
}

// Cells in storage order, raw entry bits. `plan` is the *output* permutation;
// each read applies the source file's own on the way in.
template <typename Reader>
NODISCARD Block_Source make_source_block_source(
	const Reader& src,
	const Legal_Bitmap& legal,
	Color color,
	Index_Permutation_Plan plan,
	size_t num_positions,
	size_t block_size,
	size_t entry_bytes)
{
	using Entry = decltype(src.read(color, BOARD_INDEX_ZERO));
	constexpr size_t kEntry = sizeof(Entry);
	ASSERT(block_size % entry_bytes == 0);

	const size_t source_block_bytes = block_size * kEntry / entry_bytes;
	const size_t source_total_bytes = num_positions * kEntry;

	return Block_Source{
		num_positions * entry_bytes,
		[&src, &legal, color, plan, source_block_bytes, source_total_bytes](
			size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t>
		{
			const size_t block_off = block_id * source_block_bytes;
			const size_t this_block = std::min(source_block_bytes, source_total_bytes - block_off);
			ASSERT(scratch.size() >= this_block);

			const size_t first = block_off / kEntry;
			const size_t cnt = this_block / kEntry;
			for (size_t i = 0; i < cnt; ++i)
			{
				const size_t logical = storage_index_to_logical_index(plan, first + i);
				const Entry e = legal(logical)
					? src.read(color, static_cast<Board_Index>(logical))
					: Entry::make_illegal();
				std::memcpy(scratch.data() + i * kEntry, &e, kEntry);
			}
			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

// Both tiers in one pass, keyed as the compressor will key them: whatever
// storage_fn makes of the cell, with its ILLEGAL sentinel taking no rank. That
// carries the loss-only rule too, the same function deciding it.
//
// Counted off the block source, not the reader, so the don't-care fill is
// written once and cannot disagree with what gets compressed. Partitioning does
// not affect a histogram, so it runs over the identity permutation at 1B/entry.
NODISCARD Value_Histogram gather_histogram(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	Storage_Fn storage_fn)
{
	const uint16_t illegal_1b = storage_fn(DTZ_Final_Entry::ILLEGAL_VAL, 1);
	const uint16_t illegal_2b = storage_fn(DTZ_Final_Entry::ILLEGAL_VAL, 2);

	const size_t scratch_bytes = block_size * sizeof(uint16_t);
	const size_t num_blocks = ceil_div(src.total_size, block_size);
	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), num_blocks));

	std::atomic<size_t> next(0);
	auto locals = thread_pool->run_sync_task_on_multiple_threads(
		workers, [&](size_t) -> Value_Histogram {
			Value_Histogram h;
			std::vector<uint8_t> scratch(scratch_bytes);
			for (;;)
			{
				const size_t b = next.fetch_add(1, std::memory_order_relaxed);
				if (b >= num_blocks) return h;
				const Const_Span<uint8_t> block =
					src.get(b, Span<uint8_t>(scratch.data(), scratch.size()));
				const size_t entries = block.size() / sizeof(uint16_t);
				for (size_t i = 0; i < entries; ++i)
				{
					uint16_t bits;
					std::memcpy(&bits, block.data() + i * sizeof(uint16_t), sizeof(bits));
					const uint16_t v1 = storage_fn(bits, 1);
					const uint16_t v2 = storage_fn(bits, 2);
					if (v1 != illegal_1b) ++h.hist_1b[v1];
					if (v2 != illegal_2b) ++h.hist_2b[v2];
				}
			}
		});

	Value_Histogram out;
	for (const Value_Histogram& h : locals)
		for (size_t v = 0; v < Value_Histogram::HIST_BINS; ++v)
		{
			out.hist_1b[v] += h.hist_1b[v];
			out.hist_2b[v] += h.hist_2b[v];
		}
	return out;
}

// WDL cells, packed two to a byte, in output storage order. An all-ILLEGAL
// block returns empty so compress_blocks drops it.
// `relax` prices a cap off the position itself, which only Legal_Slots holds;
// without it the bitmap answers.
NODISCARD Block_Source make_wdl_block_source(
	const Source_WDL& src,
	const Piece_Config_For_Gen& epsi,
	const Legal_Bitmap& legal_bits,
	Color color,
	Index_Permutation_Plan plan,
	size_t num_positions,
	size_t block_size,
	const Relax_Bound* relax)
{
	const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
	return Block_Source{
		total_packed_bytes,
		[&src, &epsi, &legal_bits, color, plan, num_positions, total_packed_bytes, block_size, relax](
			size_t block_id, Span<uint8_t> scratch) -> Const_Span<uint8_t>
		{
			const size_t byte_off = block_id * block_size;
			const size_t byte_sz = std::min(block_size, total_packed_bytes - byte_off);
			ASSERT(scratch.size() >= byte_sz);

			std::memset(scratch.data(), 0, byte_sz);
			auto* packed = reinterpret_cast<Packed_WDL_Entries*>(scratch.data());

			thread_local std::vector<WDL_Stored> caps;
			caps.assign(relax ? byte_sz * WDL_ENTRY_PACK_RATIO : 0, NOT_RELAXED);

			const size_t first = byte_off * WDL_ENTRY_PACK_RATIO;
			const size_t end = std::min(first + byte_sz * WDL_ENTRY_PACK_RATIO, num_positions);
			Legal_Slots legal(epsi, color);
			for (size_t storage = first; storage < end; ++storage)
			{
				const size_t logical = storage_index_to_logical_index(plan, storage);
				WDL_Stored v = WDL_Stored::ILLEGAL;
				if (relax ? legal(logical) : legal_bits(logical))
				{
					v = src.read(color, static_cast<Board_Index>(logical));
					if (relax) caps[storage - first] = relax->cap_for(legal.position(), v);
				}
				set_wdl_entry(packed[storage / WDL_ENTRY_PACK_RATIO - byte_off],
				              storage % WDL_ENTRY_PACK_RATIO, v);
			}

			if (!prepare_wdl_entries_for_compression(
					Span<Packed_WDL_Entries>(packed, byte_sz),
					Const_Span<WDL_Stored>(caps.data(), caps.size())))
				return Const_Span<uint8_t>(scratch.data(), size_t{0});
			return Const_Span<uint8_t>(scratch.data(), byte_sz);
		}
	};
}

// The source's own table, in the order it already holds: ranks sort by value
// frequency, which a re-encode of the same values reproduces.
NODISCARD Value_Rank_Table rank_table_from_source(const Source_Rank_Per_Color& pc)
{
	Value_Rank_Table out;
	out.rank_to_value = pc.rank_to_value;
	out.value_to_rank.assign(Value_Rank_Table::LUT_SIZE, Value_Rank_Table::NO_RANK);
	for (size_t i = 0; i < out.rank_to_value.size(); ++i)
		out.value_to_rank[out.rank_to_value[i]] = static_cast<uint16_t>(i);
	return out;
}

// Mirrors the DTZ/DTM save blocks in the generator.
template <typename Reader>
NODISCARD Compressed_EGTB transcribe_rank_color(
	In_Out_Param<Thread_Pool> thread_pool,
	const Reader& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t block_size,
	Storage_Fn storage_fn,
	const std::filesystem::path& spill_path,
	size_t perm_samples,
	Out_Param<uint32_t> out_perm,
	const Value_Rank_Table& source_ranks,   // empty: gather one instead
	size_t source_entry_bytes,
	const char* task_name)
{
	const size_t num_positions = epsi.num_positions();
	const Legal_Bitmap legal = build_legal_bitmap(thread_pool, epsi, color);

	Value_Rank_Table chosen;
	size_t entry_bytes;
	if (!source_ranks.rank_to_value.empty())
	{
		chosen = source_ranks;
		entry_bytes = source_entry_bytes;
	}
	else
	{
		const Block_Source hist_src = make_source_block_source(
			src, legal, color, make_index_permutation_plan(epsi, 0),
			num_positions, block_size, /*entry_bytes=*/1);
		const Value_Histogram hist =
			gather_histogram(thread_pool, hist_src, block_size, storage_fn);

		Value_Rank_Table rank_1b = Value_Rank_Table::build(hist.hist_1b);
		Value_Rank_Table rank_2b = Value_Rank_Table::build(hist.hist_2b);
		entry_bytes = (rank_1b.rank_to_value.size() <= 256) ? 1 : 2;
		chosen = (entry_bytes == 1) ? std::move(rank_1b) : std::move(rank_2b);
	}

	*out_perm = 0;
	if (chosen.rank_to_value.size() > 1)
	{
		*out_perm = choose_storage_permutation_config(
			thread_pool, epsi,
			[&](uint32_t perm) {
				return make_source_block_source(
					src, legal, color, make_index_permutation_plan(epsi, perm),
					num_positions, block_size, entry_bytes);
			},
			block_size,
			std::make_unique<LZMA_Rank_Compress_Helper>(chosen, entry_bytes, storage_fn),
			perm_samples,
			task_name);
	}

	Block_Source out_src = make_source_block_source(
		src, legal, color, make_index_permutation_plan(epsi, *out_perm),
		num_positions, block_size, entry_bytes);
	return save_compress_egtb(thread_pool, out_src, color, entry_bytes, block_size,
	                          spill_path, /*max_workers=*/0, chosen, storage_fn);
}

// A cell's whole column, don't-cares restored. A cell the pack cannot price is
// marked at layer 0, where the encoder tests it: ILLEGAL there drops the column
// as one filler run.
NODISCARD INLINE bool pack_prices_class(WDL_Entry w, bool loss_only)
{
	const bool win_class = (w == WDL_Entry::WIN || w == WDL_Entry::CURSED_WIN);
	if (w == WDL_Entry::DRAW || w == WDL_Entry::ILLEGAL) return false;
	return !(loss_only && win_class);
}

template <typename Source>
INLINE void column_for_pack(const Source& src, Color color, size_t logical,
                            WDL_Entry w, bool loss_only, Span<uint16_t> col)
{
	if (!pack_prices_class(w, loss_only))
	{
		col[0] = DTM_Final_Entry::ILLEGAL_VAL;
		return;
	}
	src.read_column(color, static_cast<Board_Index>(logical), col.data(), 1);
}

// The don't-care verdict alone, for a re-encode that keeps the source's ranks:
// column_for_pack's test without the column read behind it.
template <typename Source>
NODISCARD Legal_Bitmap build_priced_bitmap(In_Out_Param<Thread_Pool> thread_pool,
                                           const Source& src,
                                           const Piece_Config_For_Gen& epsi,
                                           Color color, bool loss_only)
{
	const size_t num_positions = epsi.num_positions();
	Legal_Bitmap out;
	out.bits.assign((num_positions + 7) / 8, 0);

	const size_t num_chunks = ceil_div(num_positions, LEGAL_CHUNK_POSITIONS);
	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), num_chunks));
	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
		Legal_Slots legal(epsi, color);
		for (;;)
		{
			const size_t k = next.fetch_add(1, std::memory_order_relaxed);
			if (k >= num_chunks) return;
			const size_t lo = k * LEGAL_CHUNK_POSITIONS;
			const size_t hi = std::min(lo + LEGAL_CHUNK_POSITIONS, num_positions);
			for (size_t i = lo; i < hi; ++i)
			{
				if (!legal(i)) continue;
				const WDL_Entry w =
					wdl_from_storage(src.m_wdl.read(color, static_cast<Board_Index>(i)));
				if (!pack_prices_class(w, loss_only)) continue;
				out.set(i);
			}
		}
	});
	return out;
}

// The source's own table, in the order it already holds: ranks sort by how many
// layers a value appears in, which a re-encode of the same values reproduces.
void ranks_from_source(const Source_Layered_Per_Color& pc, Layered_Rank_Table& out)
{
	out.rank_to_value = pc.rank_to_value;
	out.entry_bytes = (out.rank_to_value.size() <= 256) ? 1 : 2;
	for (size_t i = 0; i < out.rank_to_value.size(); ++i)
		out.value_to_rank[out.rank_to_value[i]] = static_cast<uint16_t>(i);
}

// The pack ranks by how many layers a value appears in, not how often. False if
// no layer holds a W/L value, which makes the frame singular-DRAW.
// `priced` comes back with a bit per logical index, set where the cell has a
// column to store: the same verdict column_for_pack reaches, so the save pass
// neither retests legality nor reads the WDL companion again.
template <typename Source>
NODISCARD bool gather_layered_ranks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t num_positions,
	bool loss_only,
	Layered_Rank_Table& out,
	Legal_Bitmap& priced)
{
	constexpr size_t layers = Source::LAYERS;
	using Seen = std::vector<std::array<uint8_t, Layered_Rank_Table::LUT_SIZE>>;

	priced.bits.assign((num_positions + 7) / 8, 0);
	constexpr size_t CHUNK_SIZE = LEGAL_CHUNK_POSITIONS;
	const size_t num_chunks = ceil_div(num_positions, CHUNK_SIZE);
	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), num_chunks));

	std::atomic<size_t> next(0);
	auto locals = thread_pool->run_sync_task_on_multiple_threads(
		workers, [&](size_t) -> std::unique_ptr<Seen> {
			auto seen = std::make_unique<Seen>(layers);
			Legal_Slots legal(epsi, color);
			std::vector<uint16_t> col(layers);
			for (;;)
			{
				const size_t c = next.fetch_add(1, std::memory_order_relaxed);
				if (c >= num_chunks) return seen;
				const size_t lo = c * CHUNK_SIZE;
				const size_t hi = std::min(lo + CHUNK_SIZE, num_positions);
				for (size_t i = lo; i < hi; ++i)
				{
					const WDL_Entry w = legal(i)
						? wdl_from_storage(src.m_wdl.read(color, static_cast<Board_Index>(i)))
						: WDL_Entry::ILLEGAL;
					column_for_pack(src, color, i, w, loss_only,
					                Span<uint16_t>(col.data(), col.size()));
					if (col[0] == DTM_Final_Entry::ILLEGAL_VAL) continue;
					priced.set(i);

					// The flip is terminal, so it ends the column.
					for (size_t lp = 0; lp < layers; ++lp)
					{
						const uint16_t v = col[lp];
						if (v == DTM_Final_Entry::ILLEGAL_VAL) break;
						(*seen)[lp][v] = 1;
					}
				}
			}
		});

	auto merged = std::make_unique<Seen>(layers);
	for (const auto& seen : locals)
		for (size_t lp = 0; lp < layers; ++lp)
			for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
				if ((*seen)[lp][v]) (*merged)[lp][v] = 1;

	std::array<uint64_t, Layered_Rank_Table::LUT_SIZE> score{};
	for (size_t lp = 0; lp < layers; ++lp)
		for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
			if ((*merged)[lp][v]) ++score[v];

	std::vector<uint16_t> values;
	values.reserve(64);
	for (size_t v = 0; v < Layered_Rank_Table::LUT_SIZE; ++v)
		if (score[v] != 0) values.push_back(static_cast<uint16_t>(v));
	if (values.empty()) return false;

	std::sort(values.begin(), values.end(), [&](uint16_t a, uint16_t b) {
		if (score[a] != score[b]) return score[a] > score[b];
		return a < b;
	});

	out.rank_to_value = std::move(values);
	out.entry_bytes = (out.rank_to_value.size() <= 256) ? 1 : 2;
	for (size_t i = 0; i < out.rank_to_value.size(); ++i)
		out.value_to_rank[out.rank_to_value[i]] = static_cast<uint16_t>(i);
	return true;
}

// The pack's layer 0 read as a DTM table -- it carries that value function
// verbatim, which is why a loss-only pack can raise its own DTM companion.
struct Dtm_From_Pack
{
	const Source_DTM50* pack;

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos) const
	{
		const WDL_Entry w = wdl_from_storage(pack->m_wdl.read(color, pos));
		ASSERT(w != WDL_Entry::ILLEGAL);
		if (w == WDL_Entry::DRAW) return DTM_Final_Entry::make_draw();
		return dtm_entry_from_storage(pack->read_base(color, pos), w);
	}
};

// The pack's own encoder, whose ceiling is that metric's stack height.
template <typename Source> struct Pack_Encoder;
template <> struct Pack_Encoder<Source_DTM50> { using Type = DTM50_Block_Encoder; };
template <> struct Pack_Encoder<Source_DTC>   { using Type = DTC_Block_Encoder; };

// One block's columns, layer-major, as the encoder wants them. False when the
// block prices no position at all, which the pack stores as a skip.
template <typename Source>
NODISCARD bool fill_pack_chunk(const Source& src, Color color,
                               const Index_Permutation_Plan& perm_plan,
                               const Legal_Bitmap& priced,
                               size_t p_base, size_t this_bp, uint16_t* chunk)
{
	bool any_priced = false;
	for (size_t k = 0; k < this_bp; ++k)
	{
		const size_t logical = storage_index_to_logical_index(perm_plan, p_base + k);
		if (!priced(logical))
		{
			chunk[k] = DTM_Final_Entry::ILLEGAL_VAL;
			continue;
		}
		src.read_column(color, static_cast<Board_Index>(logical), chunk + k, this_bp);
		any_priced = true;
	}
	return any_priced;
}

// The pack's own save loop, each block carrying its uncompressed size since
// plain LZMA has no end marker.
template <typename Source>
NODISCARD Layered_Compressed_Color compress_layered_color(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t num_positions,
	size_t block_size,
	uint32_t index_perm,
	const Legal_Bitmap& priced,
	std::filesystem::path spill_path,
	Layered_Rank_Table ranks)
{
	Layered_Compressed_Color out;
	out.ranks = std::move(ranks);

	const size_t bp = block_size / out.ranks.entry_bytes;
	const size_t bcnt = ceil_div(num_positions, bp);
	const size_t tail = num_positions - (bcnt - 1) * bp;
	out.block_positions = narrowing_static_cast<uint32_t>(bp);
	out.block_cnt = bcnt;
	out.tail_positions = (tail == bp) ? 0u : narrowing_static_cast<uint32_t>(tail);
	out.compressed_blocks = Compressed_Block_Store(std::move(spill_path), bcnt, block_size);
	out.usizes.resize(bcnt);

	const size_t workers = std::max<size_t>(1, std::min(thread_pool->num_workers(), bcnt));
	const auto perm_plan = make_index_permutation_plan(epsi, index_perm);

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8;
	const size_t print_period = ceil_div(PRINT_PERIOD_BYTES * workers, block_size);
	Concurrent_Progress_Bar progress_bar(bcnt, print_period,
		std::string("transcribe_") + Source::NAME + " " + std::to_string(static_cast<int>(color)));

	std::atomic<size_t> next(0);
	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) {
		std::vector<uint16_t> chunk(bp * Source::LAYERS);
		typename Pack_Encoder<Source>::Type encoder;
		encoder.ranks = &out.ranks;
		LZMA_Compress_Helper lzma;

		for (;;)
		{
			const size_t b = next.fetch_add(1, std::memory_order_relaxed);
			if (b >= bcnt) return;
			const size_t this_bp =
				(b == bcnt - 1 && out.tail_positions != 0) ? out.tail_positions : bp;

			if (!fill_pack_chunk(src, color, perm_plan, priced, b * bp, this_bp,
			                     chunk.data()))
			{
				// usz 0 in the offset table is the skip sentinel.
				out.usizes[b] = 0;
				out.compressed_blocks.clear(b);
				progress_bar += 1;
				continue;
			}

			const Const_Span<uint8_t> payload = encoder.encode(chunk.data(), this_bp);
			std::vector<uint8_t> compressed = lzma.compress(payload);
			if (compressed.size() > 0xFFFFFFFFu)
				throw std::runtime_error("rs block too large for offset encoding");
			out.usizes[b] = payload.size();
			out.compressed_blocks.set(b, std::move(compressed));
			progress_bar += 1;
		}
	});

	out.total_compressed_size = out.compressed_blocks.total_size();

	progress_bar.set_finished();
	return out;
}

// A distance frame decodes against the WDL companion, and shrink picks the
// larger frame per file, so the two can disagree on which color they dropped.
// The class has to be on disk for every frame being re-encoded.
template <typename Source>
void require_wdl_frames(const Source& src, const Piece_Config& ps)
{
	for (const Color c : src.m_colors)
		if (!src.m_per_color[c].is_dropped && src.m_wdl.m_per_color[c].is_dropped)
			throw std::runtime_error(ps.name() + ": WDL has "
				+ (c == WHITE ? "WHITE" : "BLACK") + " dropped, which that frame decodes against");
}

NODISCARD std::filesystem::path source_path(const std::filesystem::path& dir,
                                            const Piece_Config& ps, const std::string& ext)
{
	std::filesystem::path p = path_join(dir, ps.name() + ext);
	if (!std::filesystem::exists(p))
		throw std::runtime_error("No source table " + p.string());
	return p;
}

NODISCARD std::filesystem::path dest_path(const Transcribe_Options& opt,
                                          const std::filesystem::path& in_dir,
                                          const Piece_Config& ps, const std::string& ext)
{
	if (opt.out_dir.empty())
		throw std::runtime_error("No output directory set");
	if (opt.out_dir.lexically_normal() == in_dir.lexically_normal())
		throw std::runtime_error("Output directory is the input directory: " + in_dir.string());
	std::filesystem::create_directories(opt.out_dir);
	return path_join(opt.out_dir, ps.name() + ext);
}

// One re-encode, shared by DTZ and DTM: open, run each stored color, write.
template <typename Reader>
void transcribe_rank_table(
	In_Out_Param<Thread_Pool> thread_pool,
	Reader& src,
	const Piece_Config& ps,
	size_t block_size,
	Storage_Fn storage_fn,
	EGTB_Magic magic,
	const std::filesystem::path& out_path,
	const Transcribe_Options& opt,
	const char* task_name)
{
	require_wdl_frames(src, ps);

	const Piece_Config_For_Gen epsi(ps);
	std::filesystem::create_directories(opt.tmp_dir);

	Compressed_EGTB save[COLOR_NB];
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		// A dropped frame has nothing to re-encode; it passes through as a drop.
		if (src.m_per_color[c].is_dropped)
		{
			save[c] = Compressed_EGTB::make_dropped();
			continue;
		}
		// One stored value over the whole frame: the header carries it outright.
		// Not under loss-only, where the value may be a win the output does not
		// store, and the frame ships EGTB_LOSS_ONLY_FLAG saying so.
		if (src.m_per_color[c].is_singular && !opt.loss_only)
		{
			save[c] = Compressed_EGTB::make_singular(
				static_cast<uint8_t>(src.m_per_color[c].single_val));
			continue;
		}
		// A re-encode that keeps the value function keeps the source's ranks;
		// dropping the wins does not.
		Value_Rank_Table source_ranks;
		if (!opt.loss_only)
			source_ranks = rank_table_from_source(src.m_per_color[c]);
		save[c] = transcribe_rank_color(
			thread_pool, src, epsi, c, block_size, storage_fn,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), source_ranks, src.m_per_color[c].entry_bytes,
			task_name);
	}
	save_egtb_table(ps, perm, save, out_path, src.m_colors, magic, opt.loss_only);
}

}  // namespace

void transcribe_dtz(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                  const std::filesystem::path& dtz_dir,
                  const std::filesystem::path& wdl_dir,
                  const Transcribe_Options& opt)
{
	const auto out = dest_path(opt, dtz_dir, ps, EGTB_Paths::DTZ_EXT);
	Source_DTZ src;
	src.open(ps, source_path(dtz_dir, ps, EGTB_Paths::DTZ_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only);

	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTZ_BLOCK_SIZE;
	transcribe_rank_table(thread_pool, src, ps, block_size,
	                    opt.loss_only ? &dtz_storage_fn<true> : &dtz_storage_fn<false>,
	                    EGTB_Magic::DTZ_MAGIC, out, opt, "transcribe_dtz");
}

void transcribe_dtm(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                  const std::filesystem::path& dtm_dir,
                  const std::filesystem::path& wdl_dir,
                  const Transcribe_Options& opt)
{
	const auto out = dest_path(opt, dtm_dir, ps, EGTB_Paths::DTM_EXT);
	Source_DTM src;
	src.open(ps, source_path(dtm_dir, ps, EGTB_Paths::DTM_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only);

	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTM_BLOCK_SIZE;
	transcribe_rank_table(thread_pool, src, ps, block_size,
	                    opt.loss_only ? &dtm_storage_fn<true> : &dtm_storage_fn<false>,
	                    EGTB_Magic::DTM_MAGIC, out, opt, "transcribe_dtm");
}

void transcribe_wdl(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt)
{
	const auto out = dest_path(opt, wdl_dir, ps, EGTB_Paths::WDL_EXT);
	Source_WDL src;
	src.open(ps, source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT));

	std::optional<Relax_Bound> bound;
	if (opt.relaxed)
	{
		EGTB_Paths paths;
		paths.add_wdl_path(wdl_dir);
		bound.emplace(ps, paths, thread_pool);
	}

	std::optional<Piece_Config_For_Gen> own_epsi;
	if (!bound) own_epsi.emplace(ps);
	const Piece_Config_For_Gen& epsi = bound ? bound->epsi() : *own_epsi;

	const size_t num_positions = epsi.num_positions();
	const size_t block_size = opt.block_size != 0 ? opt.block_size : WDL_BLOCK_SIZE;
	// 1024 against the rank path's 64: the same bytes at a sixteenth the block.
	const size_t samples = opt.perm_samples != 0 ? opt.perm_samples : 1024;
	std::filesystem::create_directories(opt.tmp_dir);

	Compressed_EGTB save[COLOR_NB];
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		if (src.m_per_color[c].is_dropped)
		{
			save[c] = Compressed_EGTB::make_dropped();
			continue;
		}
		// One class over the whole domain: nothing to lay out or compress.
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(
				static_cast<uint8_t>(src.m_per_color[c].single_val));
			continue;
		}

		const Legal_Bitmap legal = bound ? Legal_Bitmap{}
		                                 : build_legal_bitmap(thread_pool, epsi, c);
		perm[c] = choose_storage_permutation_config(
			thread_pool, epsi,
			[&](uint32_t p) {
				return make_wdl_block_source(src, epsi, legal, c,
					make_index_permutation_plan(epsi, p),
					num_positions, block_size, bound ? &*bound : nullptr);
			},
			block_size,
			// Dict-less though the save ships dict-primed: this scores a ranking,
			// not bytes, and a trained dict is perm-dependent.
			std::make_unique<LZ4_Compress_Helper>(nullptr),
			samples,
			"transcribe_wdl");

		Block_Source out_src = make_wdl_block_source(
			src, epsi, legal, c, make_index_permutation_plan(epsi, perm[c]),
			num_positions, block_size, bound ? &*bound : nullptr);
		save[c] = save_compress_wdl(thread_pool, out_src, c, block_size,
		                            path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
		                            /*max_workers=*/0);
	}
	save_wdl_table(ps, perm, save, out, src.m_colors, EGTB_Magic::WDL_MAGIC,
	               opt.relaxed);
}

// Transcribe layer 0 of the pack as a standalone DTM table.
static void transcribe_dtm_from_pack(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source_DTM50& src,
	const Piece_Config& ps,
	const Piece_Config_For_Gen& epsi,
	const std::filesystem::path& out_path,
	const Transcribe_Options& opt)
{
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTM_BLOCK_SIZE;
	const Dtm_From_Pack dtm{ &src };

	Compressed_EGTB save[COLOR_NB];
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		if (src.m_per_color[c].is_dropped)
		{
			save[c] = Compressed_EGTB::make_dropped();
			continue;
		}
		// A singular pack stores no distance for layer 0 to extract; the value
		// goes unread under either flag.
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(0);
			continue;
		}
		// The pack's table spans every layer, not just the DTM one behind
		// layer 0, so this extract gathers its own.
		save[c] = transcribe_rank_color(
			thread_pool, dtm, epsi, c, block_size,
			opt.loss_only ? &dtm_storage_fn<true> : &dtm_storage_fn<false>,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), Value_Rank_Table{}, 0, "transcribe_dtm");
	}
	save_egtb_table(ps, perm, save, out_path, src.m_colors, EGTB_Magic::DTM_MAGIC,
	                opt.loss_only);
}

// The pack's unbounded row read as a DTZ table: it carries that table's plies
// verbatim, and the tier that halves cursed values comes from the histogram.
struct Dtz_From_Pack
{
	const Source_DTC* pack;

	NODISCARD DTZ_Final_Entry read(Color color, Board_Index pos) const
	{
		const WDL_Entry w = wdl_from_storage(pack->m_wdl.read(color, pos));
		ASSERT(w != WDL_Entry::ILLEGAL);
		if (w == WDL_Entry::DRAW) return DTZ_Final_Entry::make_draw();
		return dtz_entry_from_storage(pack->read_base(color, pos), w,
		                              sizeof(DTZ_Final_Entry));
	}
};

static void transcribe_dtz_from_pack(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source_DTC& src,
	const Piece_Config& ps,
	const Piece_Config_For_Gen& epsi,
	const std::filesystem::path& out_path,
	const Transcribe_Options& opt)
{
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTZ_BLOCK_SIZE;
	const Dtz_From_Pack dtz{ &src };

	Compressed_EGTB save[COLOR_NB];
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		if (src.m_per_color[c].is_dropped)
		{
			save[c] = Compressed_EGTB::make_dropped();
			continue;
		}
		// A singular pack stores no distance for the row to carry; the value goes
		// unread under either flag.
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(0);
			continue;
		}
		// The pack's own table spans every budget, not just the row this raises, so
		// the extract gathers its own.
		save[c] = transcribe_rank_color(
			thread_pool, dtz, epsi, c, block_size,
			opt.loss_only ? &dtz_storage_fn<true> : &dtz_storage_fn<false>,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), Value_Rank_Table{}, 0, "extract_dtz");
	}
	save_egtb_table(ps, perm, save, out_path, src.m_colors, EGTB_Magic::DTZ_MAGIC,
	                opt.loss_only);
}

// DTC rides the same pack, and its unbounded row is the DTZ table, so that table
// comes back out of it the way DTM does out of DTM50's layer 0.
void transcribe_dtc(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& dtc_dir,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt)
{
	const auto out_path = dest_path(opt, dtc_dir, ps, EGTB_Paths::DTC_EXT);
	Source_DTC src;
	src.open(ps, source_path(dtc_dir, ps, EGTB_Paths::DTC_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only);

	require_wdl_frames(src, ps);

	const Piece_Config_For_Gen epsi(ps);
	const size_t num_positions = epsi.num_positions();
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTC_BLOCK_SIZE;
	std::filesystem::create_directories(opt.tmp_dir);

	const auto dtz_path = path_join(opt.out_dir, ps.name() + EGTB_Paths::DTZ_EXT);
	if (opt.loss_only || opt.extract_dtz)
		transcribe_dtz_from_pack(thread_pool, src, ps, epsi, dtz_path, opt);
	if (opt.extract_dtz && !opt.loss_only)
		return;

	// Inherited, not searched: the unbounded row is the DTZ table's own payload, so
	// the layout that table was written under is the one this pack wants. Dropping
	// the wins changes that value function, and the loss-only DTZ raised above is
	// the table holding the layout it picked.
	std::array<uint32_t, COLOR_NB> inherited_perm{};
	for (const Color c : src.m_colors)
		inherited_perm[c] = src.m_per_color[c].plan.perm;
	if (opt.loss_only)
		inherited_perm = read_table_permutations(ps, dtz_path, EGTB_Magic::DTZ_MAGIC);

	Layered_Compressed_Color color_out[COLOR_NB]{};
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		if (src.m_per_color[c].is_dropped)
		{
			color_out[c].is_dropped = true;
			continue;
		}
		if (src.m_per_color[c].is_singular)
		{
			color_out[c].is_singular = true;
			continue;
		}

		Layered_Rank_Table ranks;
		Legal_Bitmap priced;
		if (opt.loss_only)
		{
			if (!gather_layered_ranks(thread_pool, src, epsi, c, num_positions, true,
			                          ranks, priced))
			{
				color_out[c].is_singular = true;
				continue;
			}
		}
		else
		{
			ranks_from_source(src.m_per_color[c], ranks);
			priced = build_priced_bitmap(thread_pool, src, epsi, c, false);
		}
		perm[c] = inherited_perm[c];
		color_out[c] = compress_layered_color(
			thread_pool, src, epsi, c, num_positions, block_size, perm[c], priced,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			std::move(ranks));
	}
	save_layered_table(ps, perm, color_out, out_path, src.m_colors, opt.loss_only,
	                   EGTB_Magic::DTC_MAGIC);
}

void transcribe_dtm50(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                      const std::filesystem::path& dtm50_dir,
                      const std::filesystem::path& wdl_dir,
                      const Transcribe_Options& opt)
{
	const auto out_path = dest_path(opt, dtm50_dir, ps, EGTB_Paths::DTM50_EXT);
	Source_DTM50 src;
	src.open(ps, source_path(dtm50_dir, ps, EGTB_Paths::DTM50_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only);

	require_wdl_frames(src, ps);

	const Piece_Config_For_Gen epsi(ps);
	const size_t num_positions = epsi.num_positions();
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTM50_BLOCK_SIZE;
	std::filesystem::create_directories(opt.tmp_dir);

	const auto dtm_path = path_join(opt.out_dir, ps.name() + EGTB_Paths::DTM_EXT);
	if (opt.loss_only || opt.extract_dtm)
		transcribe_dtm_from_pack(thread_pool, src, ps, epsi, dtm_path, opt);
	if (opt.extract_dtm && !opt.loss_only)
		return;

	// Inherited, not searched: layer 0 is the unbounded DTM, so the pack shares
	// that table's layout. Loss-only changes its value function, so the layout to
	// share is the loss-only DTM's, which the pack raises out of layer 0. It
	// usually chooses the same permutation as the full table: the search ranks how
	// strongly placement correlates with outcome, and removing wins only weakly
	// perturbs that ranking. Sharing guarantees the correct layout; it is not meant
	// to produce a different one.
	std::array<uint32_t, COLOR_NB> inherited_perm{};
	for (const Color c : src.m_colors)
		inherited_perm[c] = src.m_per_color[c].plan.perm;
	if (opt.loss_only)
		inherited_perm = read_table_permutations(ps, dtm_path, EGTB_Magic::DTM_MAGIC);

	Layered_Compressed_Color color_out[COLOR_NB]{};
	uint32_t perm[COLOR_NB] = { 0, 0 };
	for (const Color c : src.m_colors)
	{
		if (src.m_per_color[c].is_dropped)
		{
			color_out[c].is_dropped = true;
			continue;
		}
		// A singular frame has no payload to re-encode, and the flag the output
		// writes says how it reads: DRAW throughout, or wins to derive.
		if (src.m_per_color[c].is_singular)
		{
			color_out[c].is_singular = true;
			continue;
		}

		Layered_Rank_Table ranks;
		Legal_Bitmap priced;
		if (opt.loss_only)
		{
			// Dropping the wins changes the value set, so it has to be gathered.
			if (!gather_layered_ranks(thread_pool, src, epsi, c, num_positions, true,
			                          ranks, priced))
			{
				color_out[c].is_singular = true;
				continue;
			}
		}
		else
		{
			ranks_from_source(src.m_per_color[c], ranks);
			priced = build_priced_bitmap(thread_pool, src, epsi, c, false);
		}
		perm[c] = inherited_perm[c];
		color_out[c] = compress_layered_color(
			thread_pool, src, epsi, c, num_positions, block_size, perm[c], priced,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			std::move(ranks));
	}
	save_layered_table(ps, perm, color_out, out_path, src.m_colors, opt.loss_only,
	                   EGTB_Magic::DTM50_MAGIC);
}
