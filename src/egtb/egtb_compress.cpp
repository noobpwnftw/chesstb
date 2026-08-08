#include "chess/piece_config.h"

#include "egtb/egtb_compress.h"
#include "egtb/egtb_entry.h"
#include "egtb/index_permutation_plan.h"

#include "util/allocation.h"
#include "util/progress_bar.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"

Compressed_EGTB::Compressed_EGTB(
	Compressed_Block_Store&& compressed_blocks,
	size_t src_blk_sz,
	size_t tail_blk_sz,
	std::optional<LZ4_Dict> d,
	size_t entry_bytes,
	Value_Rank_Table rank_table
) :
	m_is_singular(false),
	m_entry_bytes(narrowing_static_cast<uint8_t>(entry_bytes)),
	m_single_val(0),
	m_block_size(src_blk_sz),
	m_tail_size(tail_blk_sz),
	m_compressed_blocks(std::move(compressed_blocks)),
	m_total_compressed_size(0),
	m_dict(std::move(d)),
	m_rank_table(std::move(rank_table))
{
	m_total_compressed_size = this->m_compressed_blocks.total_size();
}

namespace {
// Run-stitch ILLEGAL spans with a neighboring value so the don't-care cells
// disappear into surrounding runs. Returns false if every cell is ILLEGAL.
template <typename T>
NODISCARD bool prepare_entries_for_compression(Span<T> data, T illegal_val)
{
	const size_t size = data.size();
	for (size_t begin = 0, end = 0; begin < size; begin = end)
	{
		while (begin < size && data[begin] != illegal_val)
			++begin;
		if (begin == size) break;

		end = begin + 1;
		while (end < size && data[end] == illegal_val)
			++end;

		const bool has_left  = (begin > 0);
		const bool has_right = (end < size);

		if (!has_left && !has_right)
			return false;

		T fill_value;
		if (!has_left)
			fill_value = data[end];
		else if (!has_right)
			fill_value = data[begin - 1];
		else if (data[begin - 1] == data[end])
			fill_value = data[begin - 1];
		else
		{
			const T left_v  = data[begin - 1];
			const T right_v = data[end];
			size_t left_run = 0;
			for (size_t i = begin; i-- > 0 && data[i] == left_v; ) ++left_run;
			size_t right_run = 0;
			for (size_t i = end; i < size && data[i] == right_v; ++i) ++right_run;
			fill_value = (left_run >= right_run) ? left_v : right_v;
		}

		std::fill(data.begin() + begin, data.begin() + end, fill_value);
	}
	return true;
}
}  // namespace

Value_Rank_Table Value_Rank_Table::build(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist)
{
	struct Bin { uint16_t value; uint64_t count; };
	std::vector<Bin> bins;
	bins.reserve(Value_Histogram::HIST_BINS);
	for (size_t v = 0; v < Value_Histogram::HIST_BINS; ++v)
		if (hist[v] != 0)
			bins.push_back({static_cast<uint16_t>(v), hist[v]});

	// Descending by count; break ties by value so the table is deterministic.
	std::sort(bins.begin(), bins.end(),
		[](const Bin& a, const Bin& b) {
			if (a.count != b.count) return a.count > b.count;
			return a.value < b.value;
		});

	Value_Rank_Table t;
	t.ranks.reserve(bins.size());
	t.value_to_rank.assign(Value_Histogram::HIST_BINS, Value_Rank_Table::NO_RANK);
	for (size_t r = 0; r < bins.size(); ++r)
	{
		t.ranks.push_back(bins[r].value);
		t.value_to_rank[bins[r].value] = static_cast<uint16_t>(r);
	}
	return t;
}

bool prepare_packed_wdl_entries_for_compression(Span<Packed_WDL_Entries> data)
{
	if (data.size() == 0)
		return true;

	auto dst_buf = cpp20::make_unique_for_overwrite<WDL_Stored[]>(data.size() * WDL_ENTRY_PACK_RATIO);
	const Span unpacked_span(dst_buf.get(), data.size() * WDL_ENTRY_PACK_RATIO);

	unpack_wdl_entries(data, unpacked_span);

	if (!prepare_entries_for_compression<WDL_Stored>(unpacked_span, WDL_Stored::ILLEGAL))
		return false;

	pack_wdl_entries(unpacked_span, data);
	return true;
}

namespace {
constexpr size_t WDL_DICT_MAX_SIZE = 1024 * 32;
constexpr size_t WDL_DICT_MAX_TOTAL_SAMPLES_SIZE = WDL_DICT_MAX_SIZE * 1024;
constexpr size_t WDL_DICT_SAMPLE_BLOCK_SIZE = 4096;
constexpr size_t WDL_DICT_MIN_BLOCKS_TO_MAKE = 256;
}  // namespace

std::optional<LZ4_Dict> make_dict_for_wdl(
	const Block_Source& src,
	size_t block_size
)
{
	const size_t block_cnt = src.total_size / block_size;
	if (block_cnt < WDL_DICT_MIN_BLOCKS_TO_MAKE)
		return std::nullopt;

	const size_t num_blocks_to_use = std::min(WDL_DICT_MAX_TOTAL_SAMPLES_SIZE / block_size, block_cnt);
	const size_t split = std::max(block_cnt / num_blocks_to_use, (size_t)1);
	const size_t buf_size = num_blocks_to_use * block_size;

	auto dist_buf = cpp20::make_unique_for_overwrite<uint8_t[]>(buf_size);
	auto scratch  = cpp20::make_unique_for_overwrite<uint8_t[]>(block_size);

	for (size_t i = 0; i < num_blocks_to_use; ++i)
	{
		const size_t bid = i * split;
		const auto blk = src.get(bid, Span<uint8_t>(scratch.get(), block_size));
		// num_blocks_to_use <= block_cnt = total/block_size, so sampled ids
		// never hit the trailing partial block.
		ASSERT(blk.size() == block_size);
		std::memcpy(dist_buf.get() + i * block_size, blk.data(), block_size);
	}

	return LZ4_Dict::make(
		Const_Span<uint8_t>(dist_buf.get(), buf_size),
		WDL_DICT_MAX_SIZE,
		WDL_DICT_SAMPLE_BLOCK_SIZE
	);
}

uint32_t choose_storage_permutation_config(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	const std::function<Block_Source(uint32_t)>& make_source,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor,
	size_t max_samples,
	const char* task_name)
{
	const size_t n = epsi.num_populated_classes();
	if (n <= 1)
		return 0;

	const uint32_t candidates = FACTORIAL[n];
	const size_t source_block_size = compressor->source_bytes_per_block(block_size);
	const size_t bound_size = compressor->compress_bound(source_block_size);

	uint32_t best = 0;
	uint64_t best_score = std::numeric_limits<uint64_t>::max();

	for (uint32_t perm = 0; perm < candidates; ++perm)
	{
		const Block_Source src = make_source(perm);
		const size_t num_blocks = ceil_div(src.total_size, block_size);
		if (num_blocks == 0)
			continue;

		const size_t sample_cnt = std::min(num_blocks, max_samples);
		std::atomic<size_t> next_sample{0};
		const size_t workers = std::min<size_t>(thread_pool->num_workers(), sample_cnt);

		const auto partial = thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t) -> uint64_t {
			auto scratch = cpp20::make_unique_for_overwrite<uint8_t[]>(source_block_size);
			auto out = cpp20::make_unique_for_overwrite<uint8_t[]>(bound_size);
			auto helper = compressor->clone();

			uint64_t local_score = 0;
			for (;;)
			{
				const size_t sample_id = next_sample.fetch_add(1, std::memory_order_relaxed);
				if (sample_id >= sample_cnt) return local_score;
				const size_t block_id = static_cast<size_t>(
					static_cast<uint64_t>(sample_id) * num_blocks / sample_cnt);
				const Const_Span<uint8_t> block = src.get(
					block_id, Span<uint8_t>(scratch.get(), source_block_size));
				if (block.size() == 0) continue;
				local_score += helper->compress(Span<uint8_t>(out.get(), bound_size), block);
			}
		});

		uint64_t score = 0;
		for (const uint64_t s : partial)
			score += s;

		if (score < best_score)
		{
			best_score = score;
			best = perm;
		}
	}

	if (task_name != nullptr)
	{
		std::printf("%s: storage_perm=%u\n", task_name, best);
	}
	return best;
}

Compressed_EGTB save_compress_wdl(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	std::filesystem::path spill_path,
	size_t max_workers
)
{
	const std::string task_name = std::string("save_compress_wdl ") + std::to_string(static_cast<int>(color));

	auto dict = make_dict_for_wdl(src, WDL_BLOCK_SIZE);

	auto compressed_blocks = compress_blocks(
		thread_pool,
		src,
		WDL_BLOCK_SIZE,
		std::make_unique<LZ4_Compress_Helper>(dict.has_value() ? &*dict : nullptr),
		task_name,
		std::move(spill_path),
		max_workers
	);

	return Compressed_EGTB(
		std::move(compressed_blocks),
		WDL_BLOCK_SIZE,
		src.total_size % WDL_BLOCK_SIZE,
		std::move(dict),
		/*entry_bytes=*/0
	);
}

size_t LZMA_Rank_Compress_Helper::compress(Span<uint8_t> dest, Const_Span<uint8_t> src)
{
	ASSERT(src.size() % sizeof(uint16_t) == 0);
	const size_t entries = src.size() / sizeof(uint16_t);
	if (entries == 0) return 0;
	const size_t output_bytes = entries * m_entry_bytes;
	const size_t work_bytes = entries * sizeof(uint16_t);
	const size_t need_bytes = work_bytes > output_bytes ? work_bytes : output_bytes;
	if (m_scratch.size() < need_bytes) m_scratch.resize(need_bytes);

	const auto* const in = reinterpret_cast<const uint16_t*>(src.data());
	const uint16_t* const v2r = m_rank_table->value_to_rank.data();
	auto* const work = reinterpret_cast<uint16_t*>(m_scratch.data());

	// storage_fn maps both don't-care classes (DRAW and ILLEGAL) to the same
	// sentinel so the run-stitch pass folds them away; the probe recovers them
	// via the .lzw companion and never consults these bytes. illegal_v recovers
	// that sentinel value to drive the stitch.
	const uint16_t illegal_v = m_storage_fn(DTC_Final_Entry::ILLEGAL_VAL, m_entry_bytes);
	for (size_t i = 0; i < entries; ++i)
		work[i] = m_storage_fn(in[i], m_entry_bytes);

	if (!prepare_entries_for_compression<uint16_t>(Span<uint16_t>(work, entries), illegal_v))
		return 0;

	if (m_entry_bytes == 2)
	{
		for (size_t i = 0; i < entries; ++i)
			work[i] = v2r[work[i]];
	}
	else
	{
		uint8_t* const out = m_scratch.data();
		for (size_t i = 0; i < entries; ++i)
			out[i] = static_cast<uint8_t>(v2r[work[i]]);
	}

	return m_lzma.compress(dest, Const_Span<uint8_t>(m_scratch.data(), output_bytes));
}

Compressed_EGTB save_compress_egtb(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	Color color,
	size_t entry_bytes,
	size_t block_size,
	std::filesystem::path spill_path,
	size_t max_workers,
	Value_Rank_Table rank_table,
	Storage_Fn storage_fn
)
{
	const std::string task_name = std::string("save_compress_egtb ") + std::to_string(static_cast<int>(color));

	if (rank_table.ranks.size() <= 1)
	{
		printf("%s: singular\n", task_name.c_str());
		return Compressed_EGTB::make_singular(rank_table.ranks.size() == 0 ? 0 : static_cast<uint8_t>(rank_table.ranks[0]));
	}

	auto compressed_blocks = compress_blocks(
		thread_pool,
		src,
		block_size,
		std::make_unique<LZMA_Rank_Compress_Helper>(rank_table, entry_bytes, storage_fn),
		task_name,
		std::move(spill_path),
		max_workers
	);

	return Compressed_EGTB(
		std::move(compressed_blocks),
		block_size,
		src.total_size % block_size,
		std::nullopt,
		entry_bytes,
		std::move(rank_table)
	);
}

void save_wdl_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
)
{
	// Pre-encode each color's offsets so file_size knows the section bytes.
	Mono_Uint_Vec::Encoded mono_enc[COLOR_NB];
	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular()) continue;
		const size_t n = t.num_blocks();
		std::vector<uint64_t> off(n + 1);
		size_t cur = 0;
		const auto& blocks = t.compressed_blocks();
		for (size_t k = 0; k < n; ++k)
		{
			off[k] = cur;
			cur += blocks.block_size(k);
		}
		off[n] = cur;
		ASSERT(cur == t.total_compressed_size());
		mono_enc[i] = Mono_Uint_Vec::encode(Const_Span<uint64_t>(off.data(), off.size()));
	}

	size_t file_size = 8;  // magic/key

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			file_size += 2;
		else
			// flag(1) + index permutation config(4) + tail(2) + block_size(4) + block_cnt(8) + total(8)
			file_size += 27;
	}

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		file_size += 2;
		if (t.dict().has_value())
			file_size += t.dict()->size();
	}
	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		file_size += MONO_SECTION_WIDTH_BYTES + mono_enc[i].on_disk_bytes;
	}

	file_size = ceil_to_multiple(file_size, (size_t)64);

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		file_size += t.total_compressed_size();
		file_size = ceil_to_multiple(file_size, (size_t)64);
	}

	const auto tmp = file_path.string() + ".tmp";
	Memory_Mapped_File write_map;
	if (!write_map.create(tmp.c_str(), file_size + 8))
		print_and_abort("Failed to create %s\n", tmp.c_str());

	Serial_Memory_Writer writer(write_map.data_span());

	writer.write<uint32_t>(narrowing_static_cast<uint32_t>(magic));
	writer.write<uint32_t>(narrowing_static_cast<uint32_t>((ps.min_material_key().value() << 2ull) + table_colors.size()));

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
		{
			writer.write<uint8_t>(narrowing_static_cast<uint8_t>(EGTB_SINGULAR_FLAG));
			writer.write<uint8_t>(narrowing_static_cast<uint8_t>(t.single_val()));
		}
		else
		{
			writer.write<uint8_t>(0);
			writer.write<uint32_t>(index_perm[i]);

			writer.write<uint16_t>(narrowing_static_cast<uint16_t>(t.tail_size()));
			writer.write<uint32_t>(narrowing_static_cast<uint32_t>(t.block_size()));
			writer.write<uint64_t>(narrowing_static_cast<uint64_t>(t.num_blocks()));
			writer.write<uint64_t>(narrowing_static_cast<uint64_t>(t.total_compressed_size()));
		}
	}

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		if (t.dict().has_value())
		{
			writer.write<uint16_t>(narrowing_static_cast<uint16_t>(t.dict()->size()));
			writer.write(Const_Span(t.dict()->data(), t.dict()->size()));
		}
		else
			writer.write<uint16_t>(0);
	}

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		const auto& m = mono_enc[i];
		writer.write<uint8_t>(m.log2_bu);
		writer.write<uint8_t>(m.sample_width);
		writer.write<uint8_t>(m.offset_width);
		writer.write<uint8_t>(0);  // usz_width
		writer.write(Const_Span<uint8_t>(m.blob.data(), m.on_disk_bytes));
	}

	writer.zero_align(64);

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		const auto& blocks = t.compressed_blocks();
		for (size_t k = 0; k < blocks.size(); ++k)
			if (blocks.block_size(k) != 0)
				writer.write(blocks.block(k));

		writer.zero_align(64);
	}

	if (writer.num_bytes_written() != file_size)
		print_and_abort("file size is wrong.\n");

	writer.write_end_checksum(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE));

	write_map.close();

	std::error_code ec;
	std::filesystem::rename(tmp, file_path, ec);
	if (ec)
		print_and_abort("rename failed: %s\n", ec.message().c_str());
}

void save_egtb_table(
	const Piece_Config& ps,
	const uint32_t index_perm[COLOR_NB],
	const Compressed_EGTB save_info[COLOR_NB],
	std::filesystem::path file_path,
	const Fixed_Vector<Color, 2> table_colors,
	EGTB_Magic magic
)
{
	// Pre-encode each color's offsets so file_size knows the section bytes.
	Mono_Uint_Vec::Encoded mono_enc[COLOR_NB];
	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular()) continue;
		const size_t n = t.num_blocks();
		std::vector<uint64_t> off(n + 1);
		size_t cur = 0;
		const auto& blocks = t.compressed_blocks();
		for (size_t k = 0; k < n; ++k)
		{
			off[k] = cur;
			cur += blocks.block_size(k);
		}
		off[n] = cur;
		ASSERT(cur == t.total_compressed_size());
		mono_enc[i] = Mono_Uint_Vec::encode(Const_Span<uint64_t>(off.data(), off.size()));
	}

	size_t file_size = 8;  // magic/key

	for (const Color i : table_colors)
	{
		if (save_info[i].is_singular())
		{
			file_size += 2;
		}
		else
		{
			// flag(1)+index permutation config(4)+entry_bytes(1)+tail(4)+block_size(4)+block_cnt(8)+total(8)
			file_size += 30 + 2 + save_info[i].rank_table().ranks.size() * 2;
		}
	}

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		file_size += MONO_SECTION_WIDTH_BYTES + mono_enc[i].on_disk_bytes;
	}

	file_size = ceil_to_multiple(file_size, (size_t)64);

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		file_size += t.total_compressed_size();
		file_size = ceil_to_multiple(file_size, (size_t)64);
	}

	const auto tmp = file_path.string() + ".tmp";
	Memory_Mapped_File write_map;
	if (!write_map.create(tmp.c_str(), file_size + 8))
		print_and_abort("Failed to create %s\n", tmp.c_str());

	Serial_Memory_Writer writer(write_map.data_span());

	writer.write<uint32_t>(narrowing_static_cast<uint32_t>(magic));
	writer.write<uint32_t>(narrowing_static_cast<uint32_t>((ps.min_material_key().value() << 2ull) + table_colors.size()));

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
		{
			writer.write<uint8_t>(narrowing_static_cast<uint8_t>(EGTB_SINGULAR_FLAG));
			writer.write<uint8_t>(narrowing_static_cast<uint8_t>(t.single_val()));
		}
		else
		{
			writer.write<uint8_t>(0);
			writer.write<uint32_t>(index_perm[i]);
			writer.write<uint8_t>(narrowing_static_cast<uint8_t>(t.entry_bytes()));

			writer.write<uint32_t>(narrowing_static_cast<uint32_t>(t.tail_size()));
			writer.write<uint32_t>(narrowing_static_cast<uint32_t>(t.block_size()));
			writer.write<uint64_t>(narrowing_static_cast<uint64_t>(t.num_blocks()));
			writer.write<uint64_t>(narrowing_static_cast<uint64_t>(t.total_compressed_size()));

			// Inline rank table: num_ranks=0 means "no remap, stream stores storage values".
			const auto& rt = t.rank_table();
			writer.write<uint16_t>(narrowing_static_cast<uint16_t>(rt.ranks.size()));
			for (uint16_t v : rt.ranks)
				writer.write<uint16_t>(v);
		}
	}

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		const auto& m = mono_enc[i];
		writer.write<uint8_t>(m.log2_bu);
		writer.write<uint8_t>(m.sample_width);
		writer.write<uint8_t>(m.offset_width);
		writer.write<uint8_t>(0);  // usz_width
		writer.write(Const_Span<uint8_t>(m.blob.data(), m.on_disk_bytes));
	}

	writer.zero_align(64);

	for (const Color i : table_colors)
	{
		const Compressed_EGTB& t = save_info[i];
		if (t.is_singular())
			continue;

		const auto& blocks = t.compressed_blocks();
		for (size_t k = 0; k < blocks.size(); ++k)
			if (blocks.block_size(k) != 0)
				writer.write(blocks.block(k));
		writer.zero_align(64);
	}

	if (writer.num_bytes_written() != file_size)
		print_and_abort("file size is wrong.\n");

	writer.write_end_checksum(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE));

	write_map.close();

	std::error_code ec;
	std::filesystem::rename(tmp, file_path, ec);
	if (ec)
		print_and_abort("rename failed: %s\n", ec.message().c_str());
}
