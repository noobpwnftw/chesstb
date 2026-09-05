#include "transcribe/transcribe.h"

#include "transcribe/relax_dtz.h"

#include "egtb/relax_bound.h"
#include "egtb/egtb_gen.h"
#include "egtb/egtb_probe.h"

#include "util/math.h"
#include "util/progress_bar.h"

#include <atomic>
#include <optional>
#include <stdexcept>

namespace {

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
		m_pos.seek(idx);
		if (!m_pos.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
			return false;
		return !(*m_has_stab)[m_pos.index().king_slice_id]
		    || board_index_of_position(*m_epsi, m_pos.board_unchecked()) == idx;
	}

private:
	const Piece_Config_For_Gen* m_epsi;
	Position_For_Gen m_pos;
	const decltype(King_Slice_Manager::slice_has_stabilizer)* m_has_stab;
};

// A set bit means that the cell's value must survive compression. Illegal and
// deliberately omitted cells are both passed to the compressor as don't-cares.
struct Stored_Bitmap
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

constexpr size_t LEGAL_CHUNK_POSITIONS = size_t{ 1 } << 14;
static_assert(LEGAL_CHUNK_POSITIONS % 8 == 0);

// Compute omissions before choosing a permutation so that sampling and the
// final compression pass see the same cells.
NODISCARD Stored_Bitmap build_stored_bitmap(In_Out_Param<Thread_Pool> thread_pool,
                                           const Piece_Config_For_Gen& epsi, Color color,
                                           const Relax_DTZ* relax)
{
	const size_t num_positions = epsi.num_positions();
	Stored_Bitmap out;
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
				const Board_Index idx = static_cast<Board_Index>(i);
				if (relax != nullptr && relax->can_omit(legal.position(), color, idx)) continue;
				out.set(i);
			}
		}
	});
	return out;
}

template <typename Reader>
NODISCARD Block_Source make_source_block_source(
	const Reader& src,
	const Stored_Bitmap& stored,
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
		[&src, &stored, color, plan, source_block_bytes, source_total_bytes](
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
				const Entry e = stored(logical)
					? src.read(color, static_cast<Board_Index>(logical))
					: Entry::make_illegal();
				std::memcpy(scratch.data() + i * kEntry, &e, kEntry);
			}
			return Const_Span<uint8_t>(scratch.data(), this_block);
		}
	};
}

template <Rank_Storage_Mode Mode>
FORCE_INLINE void gather_histogram_block(
	Const_Span<uint8_t> block, In_Out_Param<Value_Histogram> h)
{
	const uint16_t illegal_1b = rank_storage_value<Mode>(DTZ_Final_Entry::ILLEGAL_VAL, 1);
	const uint16_t illegal_2b = rank_storage_value<Mode>(DTZ_Final_Entry::ILLEGAL_VAL, 2);
	const size_t entries = block.size() / sizeof(uint16_t);
	for (size_t i = 0; i < entries; ++i)
	{
		uint16_t bits;
		std::memcpy(&bits, block.data() + i * sizeof(uint16_t), sizeof(bits));
		const uint16_t v1 = rank_storage_value<Mode>(bits, 1);
		const uint16_t v2 = rank_storage_value<Mode>(bits, 2);
		if (v1 != illegal_1b) ++h->hist_1b[v1];
		if (v2 != illegal_2b) ++h->hist_2b[v2];
	}
}

NODISCARD Value_Histogram gather_histogram(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	Rank_Storage_Mode storage_mode)
{
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
				switch (storage_mode)
				{
				case Rank_Storage_Mode::DTZ:
					gather_histogram_block<Rank_Storage_Mode::DTZ>(block, inout_param(h));
					break;
				case Rank_Storage_Mode::DTZ_LOSS_ONLY:
					gather_histogram_block<Rank_Storage_Mode::DTZ_LOSS_ONLY>(block, inout_param(h));
					break;
				case Rank_Storage_Mode::DTM:
					gather_histogram_block<Rank_Storage_Mode::DTM>(block, inout_param(h));
					break;
				case Rank_Storage_Mode::DTM_LOSS_ONLY:
					gather_histogram_block<Rank_Storage_Mode::DTM_LOSS_ONLY>(block, inout_param(h));
					break;
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

NODISCARD Block_Source make_wdl_block_source(
	const Source_WDL& src,
	const Piece_Config_For_Gen& epsi,
	const Stored_Bitmap& stored,
	Color color,
	Index_Permutation_Plan plan,
	size_t num_positions,
	size_t block_size,
	const Relax_Bound* relax)
{
	const size_t total_packed_bytes = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);
	return Block_Source{
		total_packed_bytes,
		[&src, &epsi, &stored, color, plan, num_positions, total_packed_bytes, block_size, relax](
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
				if (relax ? legal(logical) : stored(logical))
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

NODISCARD Value_Rank_Table rank_table_from_source(const Source_Rank_Per_Color& pc)
{
	Value_Rank_Table out;
	out.rank_to_value = pc.rank_to_value;
	out.value_to_rank.assign(Value_Rank_Table::LUT_SIZE, Value_Rank_Table::NO_RANK);
	for (size_t i = 0; i < out.rank_to_value.size(); ++i)
		out.value_to_rank[out.rank_to_value[i]] = static_cast<uint16_t>(i);
	return out;
}

template <typename Reader>
NODISCARD Compressed_EGTB transcribe_rank_color(
	In_Out_Param<Thread_Pool> thread_pool,
	const Reader& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t block_size,
	Rank_Storage_Mode storage_mode,
	const std::filesystem::path& spill_path,
	size_t perm_samples,
	Out_Param<uint32_t> out_perm,
	const Value_Rank_Table& source_ranks,   // empty: gather one instead
	size_t source_entry_bytes,
	const char* task_name,
	const Relax_DTZ* relax)
{
	const size_t num_positions = epsi.num_positions();
	const Stored_Bitmap stored = build_stored_bitmap(thread_pool, epsi, color, relax);

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
			src, stored, color, make_index_permutation_plan(epsi, 0),
			num_positions, block_size, /*entry_bytes=*/1);
		const Value_Histogram hist =
			gather_histogram(thread_pool, hist_src, block_size, storage_mode);

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
					src, stored, color, make_index_permutation_plan(epsi, perm),
					num_positions, block_size, entry_bytes);
			},
			block_size,
			std::make_unique<LZMA_Rank_Compress_Helper>(chosen, entry_bytes, storage_mode),
			perm_samples,
			task_name);
	}

	Block_Source out_src = make_source_block_source(
		src, stored, color, make_index_permutation_plan(epsi, *out_perm),
		num_positions, block_size, entry_bytes);
	return save_compress_egtb(thread_pool, out_src, color, entry_bytes, block_size,
	                          spill_path, /*max_workers=*/0, chosen, storage_mode);
}

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

template <typename Source>
NODISCARD Stored_Bitmap build_priced_bitmap(In_Out_Param<Thread_Pool> thread_pool,
                                           const Source& src,
                                           const Piece_Config_For_Gen& epsi,
                                           Color color, bool loss_only)
{
	const size_t num_positions = epsi.num_positions();
	Stored_Bitmap out;
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

void ranks_from_source(const Source_Layered_Per_Color& pc, Layered_Rank_Table& out)
{
	out.rank_to_value = pc.rank_to_value;
	out.entry_bytes = (out.rank_to_value.size() <= 256) ? 1 : 2;
	for (size_t i = 0; i < out.rank_to_value.size(); ++i)
		out.value_to_rank[out.rank_to_value[i]] = static_cast<uint16_t>(i);
}

template <typename Source>
NODISCARD bool gather_layered_ranks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t num_positions,
	bool loss_only,
	Layered_Rank_Table& out,
	Stored_Bitmap& priced)
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

template <typename Source> struct Pack_Encoder;
template <> struct Pack_Encoder<Source_DTM50> { using Type = DTM50_Block_Encoder; };
template <> struct Pack_Encoder<Source_DTC>   { using Type = DTC_Block_Encoder; };

template <typename Source>
NODISCARD bool fill_pack_chunk(const Source& src, Color color,
                               const Index_Permutation_Plan& perm_plan,
                               const Stored_Bitmap& priced,
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

template <typename Source>
NODISCARD Layered_Compressed_Color compress_layered_color(
	In_Out_Param<Thread_Pool> thread_pool,
	const Source& src,
	const Piece_Config_For_Gen& epsi,
	Color color,
	size_t num_positions,
	size_t block_size,
	uint32_t index_perm,
	const Stored_Bitmap& priced,
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
				print_and_abort("Block too large for offset encoding\n");
			out.usizes[b] = payload.size();
			out.compressed_blocks.set(b, Const_Span<uint8_t>(compressed));
			progress_bar += 1;
		}
	});

	out.total_compressed_size = out.compressed_blocks.total_size();

	progress_bar.set_finished();
	return out;
}

template <typename Source>
void require_wdl_frames(const Source& src, const Piece_Config& ps, bool relaxed)
{
	for (const Color c : src.m_colors)
		if (!src.m_per_color[c].is_dropped && src.m_wdl.m_per_color[c].is_dropped)
			throw std::runtime_error(ps.name() + ": WDL has "
				+ (c == WHITE ? "WHITE" : "BLACK") + " dropped, which that frame decodes against");

	// A pawnless relaxed frame reads nothing but sub-tables, which gate their
	// own form. A push instead stays in the material and lands in the other
	// frame, so a pawnful one reads both of its own.
	if (!relaxed || !ps.has_pawns())
		return;

	// A symmetric material stores one frame and mirrors its twin. Source_WDL
	// reads by index and does not mirror, so that twin is simply unreachable.
	if (src.m_wdl.m_colors.size() != COLOR_NB)
		throw std::runtime_error(ps.name() + ": --relaxed cannot follow a pawn push on a"
			" symmetric material, whose second WDL frame is mirrored rather than stored");

	for (const Color c : { WHITE, BLACK })
		if (src.m_wdl.m_per_color[c].is_dropped)
			throw std::runtime_error(ps.name() + ": WDL has "
				+ (c == WHITE ? "WHITE" : "BLACK") + " dropped, which a relaxed pawn push reads");
}

NODISCARD std::filesystem::path source_path(const std::filesystem::path& dir,
                                            const Piece_Config& ps, const std::string& ext)
{
	std::filesystem::path p = path_join(dir, ps.name() + ext);
	if (!file_exists_case_exact(p))
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

template <typename Reader>
void transcribe_rank_table(
	In_Out_Param<Thread_Pool> thread_pool,
	Reader& src,
	const Piece_Config& ps,
	size_t block_size,
	Rank_Storage_Mode storage_mode,
	EGTB_Magic magic,
	const std::filesystem::path& out_path,
	const Transcribe_Options& opt,
	const char* task_name,
	const Relax_DTZ* relax)
{
	require_wdl_frames(src, ps, relax != nullptr);

	const Piece_Config_For_Gen epsi(ps);
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
		if (src.m_per_color[c].is_singular && !opt.loss_only)
		{
			save[c] = Compressed_EGTB::make_singular(
				static_cast<uint8_t>(src.m_per_color[c].single_val));
			continue;
		}
		Value_Rank_Table source_ranks;
		// Relaxation changes the value alphabet, so rebuild its rank table from
		// the cells that remain.
		if (!opt.loss_only && relax == nullptr)
			source_ranks = rank_table_from_source(src.m_per_color[c]);
		save[c] = transcribe_rank_color(
			thread_pool, src, epsi, c, block_size, storage_mode,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), source_ranks, src.m_per_color[c].entry_bytes,
			task_name, relax);
	}
	save_egtb_table(thread_pool, ps, perm, save, out_path, src.m_colors, magic,
	                opt.loss_only, relax != nullptr);
}

}  // namespace

void transcribe_dtz(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                  const std::filesystem::path& dtz_dir,
                  const std::filesystem::path& wdl_dir,
                  const Transcribe_Options& opt)
{
	const auto out = dest_path(opt, dtz_dir, ps, EGTB_Paths::DTZ_EXT);
	Source_DTZ src;
	// A relaxed source freed winning values only. This run reads none of them:
	// relaxed omits the same ones again, and loss-only drops every win anyway.
	src.open(ps, source_path(dtz_dir, ps, EGTB_Paths::DTZ_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only,
	         opt.relaxed || opt.loss_only);

	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTZ_BLOCK_SIZE;

	std::optional<Relax_DTZ> relax;
	if (opt.relaxed)
	{
		EGTB_Paths paths;
		paths.add_wdl_path(wdl_dir);
		relax.emplace(ps, paths, thread_pool, src.m_wdl);
	}

	transcribe_rank_table(thread_pool, src, ps, block_size,
	                    opt.loss_only ? Rank_Storage_Mode::DTZ_LOSS_ONLY
	                                  : Rank_Storage_Mode::DTZ,
	                    EGTB_Magic::DTZ_MAGIC, out, opt, "transcribe_dtz",
	                    relax ? &*relax : nullptr);
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
	                    opt.loss_only ? Rank_Storage_Mode::DTM_LOSS_ONLY
	                                  : Rank_Storage_Mode::DTM,
	                    EGTB_Magic::DTM_MAGIC, out, opt, "transcribe_dtm", nullptr);
}

void transcribe_wdl(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt)
{
	const auto out = dest_path(opt, wdl_dir, ps, EGTB_Paths::WDL_EXT);
	Source_WDL src;
	// Not opt.relaxed: a WDL cap is computed against the cell's own stored
	// code, so a second pass would cap against the first pass's fill and lose
	// slack for good. DTZ recomputes its predicate from WDL and is stable.
	src.open(ps, source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT),
	         /*allow_loss_only=*/false, /*allow_relaxed=*/false);

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
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(
				static_cast<uint8_t>(src.m_per_color[c].single_val));
			continue;
		}

		const Stored_Bitmap stored = bound ? Stored_Bitmap{}
		                                   : build_stored_bitmap(thread_pool, epsi, c, nullptr);
		perm[c] = choose_storage_permutation_config(
			thread_pool, epsi,
			[&](uint32_t p) {
				return make_wdl_block_source(src, epsi, stored, c,
					make_index_permutation_plan(epsi, p),
					num_positions, block_size, bound ? &*bound : nullptr);
			},
			block_size,
			std::make_unique<LZ4_Compress_Helper>(nullptr),
			samples,
			"transcribe_wdl");

		Block_Source out_src = make_wdl_block_source(
			src, epsi, stored, c, make_index_permutation_plan(epsi, perm[c]),
			num_positions, block_size, bound ? &*bound : nullptr);
		save[c] = save_compress_wdl(thread_pool, out_src, c, block_size,
		                            path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
		                            /*max_workers=*/0);
	}
	save_wdl_table(thread_pool, ps, perm, save, out, src.m_colors, EGTB_Magic::WDL_MAGIC,
	               opt.relaxed);
}

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
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(0);
			continue;
		}
		save[c] = transcribe_rank_color(
			thread_pool, dtm, epsi, c, block_size,
			opt.loss_only ? Rank_Storage_Mode::DTM_LOSS_ONLY
			              : Rank_Storage_Mode::DTM,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), Value_Rank_Table{}, 0, "transcribe_DTM", nullptr);
	}
	save_egtb_table(thread_pool, ps, perm, save, out_path, src.m_colors, EGTB_Magic::DTM_MAGIC,
	                opt.loss_only, /*relaxed=*/false);
}

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
		if (src.m_per_color[c].is_singular)
		{
			save[c] = Compressed_EGTB::make_singular(0);
			continue;
		}
		save[c] = transcribe_rank_color(
			thread_pool, dtz, epsi, c, block_size,
			opt.loss_only ? Rank_Storage_Mode::DTZ_LOSS_ONLY
			              : Rank_Storage_Mode::DTZ,
			path_join(opt.tmp_dir, ps.name() + EGTB_Paths::BLOCK_SPILL_EXT[c]),
			opt.perm_samples != 0 ? opt.perm_samples : 64,
			out_param(perm[c]), Value_Rank_Table{}, 0, "transcribe_DTZ", nullptr);
	}
	save_egtb_table(thread_pool, ps, perm, save, out_path, src.m_colors, EGTB_Magic::DTZ_MAGIC,
	                opt.loss_only, /*relaxed=*/false);
}

void transcribe_dtc(In_Out_Param<Thread_Pool> thread_pool, const Piece_Config& ps,
                    const std::filesystem::path& dtc_dir,
                    const std::filesystem::path& wdl_dir,
                    const Transcribe_Options& opt)
{
	const auto out_path = dest_path(opt, dtc_dir, ps, EGTB_Paths::DTC_EXT);
	Source_DTC src;
	src.open(ps, source_path(dtc_dir, ps, EGTB_Paths::DTC_EXT),
	         source_path(wdl_dir, ps, EGTB_Paths::WDL_EXT), opt.loss_only);

	require_wdl_frames(src, ps, /*relaxed=*/false);

	const Piece_Config_For_Gen epsi(ps);
	const size_t num_positions = epsi.num_positions();
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTC_BLOCK_SIZE;
	std::filesystem::create_directories(opt.tmp_dir);

	const auto dtz_path = path_join(opt.out_dir, ps.name() + EGTB_Paths::DTZ_EXT);
	if (opt.loss_only || opt.extract_dtz)
		transcribe_dtz_from_pack(thread_pool, src, ps, epsi, dtz_path, opt);
	if (opt.extract_dtz && !opt.loss_only)
		return;

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
		Stored_Bitmap priced;
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
	save_layered_table(thread_pool, ps, perm, color_out, out_path, src.m_colors, opt.loss_only,
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

	require_wdl_frames(src, ps, /*relaxed=*/false);

	const Piece_Config_For_Gen epsi(ps);
	const size_t num_positions = epsi.num_positions();
	const size_t block_size = opt.block_size != 0 ? opt.block_size : DTM50_BLOCK_SIZE;
	std::filesystem::create_directories(opt.tmp_dir);

	const auto dtm_path = path_join(opt.out_dir, ps.name() + EGTB_Paths::DTM_EXT);
	if (opt.loss_only || opt.extract_dtm)
		transcribe_dtm_from_pack(thread_pool, src, ps, epsi, dtm_path, opt);
	if (opt.extract_dtm && !opt.loss_only)
		return;

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
		if (src.m_per_color[c].is_singular)
		{
			color_out[c].is_singular = true;
			continue;
		}

		Layered_Rank_Table ranks;
		Stored_Bitmap priced;
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
	save_layered_table(thread_pool, ps, perm, color_out, out_path, src.m_colors, opt.loss_only,
	                   EGTB_Magic::DTM50_MAGIC);
}
