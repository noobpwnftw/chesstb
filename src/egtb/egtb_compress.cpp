#include "egtb/egtb_compress.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_probe.h"

#include "chess/index_permutation.h"
#include "chess/piece_config.h"

#include "util/allocation.h"
#include "util/progress_bar.h"
#include "util/filesystem.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"

#include <atomic>

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

namespace {
template <size_t N>
Value_Rank_Table build_rank_table_impl(const std::array<uint64_t, N>& hist)
{
	struct Bin { uint16_t value; uint64_t count; };
	std::vector<Bin> bins;
	bins.reserve(N);
	for (size_t v = 0; v < N; ++v)
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
	t.value_to_rank.assign(N, Value_Rank_Table::NO_RANK);
	for (size_t r = 0; r < bins.size(); ++r)
	{
		t.ranks.push_back(bins[r].value);
		t.value_to_rank[bins[r].value] = static_cast<uint16_t>(r);
	}
	return t;
}
}  // namespace

Value_Rank_Table Value_Rank_Table::build_1b(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist)
{
	return build_rank_table_impl(hist);
}

Value_Rank_Table Value_Rank_Table::build_2b(const std::array<uint64_t, Value_Histogram::HIST_BINS>& hist)
{
	return build_rank_table_impl(hist);
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
		return default_index_permutation_config(epsi);

	const uint32_t candidates = FACTORIAL[n];
	const size_t source_block_size = compressor->source_bytes_per_block(block_size);
	const size_t bound_size = compressor->compress_bound(source_block_size);

	uint32_t best = default_index_permutation_config(epsi);
	uint32_t best_ix = candidates;
	uint64_t best_score = std::numeric_limits<uint64_t>::max();

	for (uint32_t ix = 0; ix < candidates; ++ix)
	{
		const uint32_t perm = ix;
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

		if (score < best_score || (score == best_score && ix < best_ix))
		{
			best_score = score;
			best_ix = ix;
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
	const EGTB_Info& info,
	size_t entry_bytes,
	size_t block_size,
	size_t max_workers,
	Value_Rank_Table rank_table,
	LZMA_Rank_Compress_Helper::Storage_Fn storage_fn,
	bool silent
)
{
	const std::string task_name = std::string("save_compress_egtb ") + std::to_string(static_cast<int>(color));

	if (info.win_cnt[color] + info.lose_cnt[color] == 0)
	{
		if (!silent) printf("%s: singular\n", task_name.c_str());
		return Compressed_EGTB::make_singular(WDL_Entry::DRAW);
	}

	auto compressed_blocks = compress_blocks(
		thread_pool,
		src,
		block_size,
		std::make_unique<LZMA_Rank_Compress_Helper>(rank_table, entry_bytes, storage_fn),
		task_name,
		max_workers,
		silent
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
			if (!blocks[k].empty()) cur += blocks[k].size();
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

	Memory_Mapped_File write_map;
	if (!write_map.create(file_path.c_str(), file_size + 8))
		abort();

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

		for (const auto& block : t.compressed_blocks())
			writer.write(Const_Span(block));

		writer.zero_align(64);
	}

	if (writer.num_bytes_written() != file_size)
		print_and_abort("file size is wrong.\n");

	writer.write_end_checksum(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE));

	write_map.close();
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
			if (!blocks[k].empty()) cur += blocks[k].size();
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

	Memory_Mapped_File write_map;
	write_map.create(file_path.c_str(), file_size + 8);

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

		for (const auto& block : t.compressed_blocks())
			writer.write(Const_Span(block));
		writer.zero_align(64);
	}

	if (writer.num_bytes_written() != file_size)
		print_and_abort("file size is wrong.\n");

	writer.write_end_checksum(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE));

	write_map.close();
}

Compressed_EGTB::Compressed_EGTB(
	std::vector<std::vector<uint8_t>>&& compressed_blocks,
	size_t src_blk_sz,
	size_t tail_blk_sz,
	std::optional<LZ4_Dict> d,
	size_t entry_bytes,
	Value_Rank_Table rank_table
) :
	m_is_singular(false),
	m_entry_bytes(narrowing_static_cast<uint8_t>(entry_bytes)),
	m_single_val(WDL_Entry::DRAW),
	m_block_size(src_blk_sz),
	m_tail_size(tail_blk_sz),
	m_compressed_blocks(std::move(compressed_blocks)),
	m_total_compressed_size(0),
	m_dict(std::move(d)),
	m_rank_table(std::move(rank_table))
{
	for (const auto& block : this->m_compressed_blocks)
		m_total_compressed_size += block.size();
}

void load_wdl_table(
	Out_Param<WDL_File_For_Probe> wdl,
	const Piece_Config& ps,
	std::filesystem::path sub_wdl,
	EGTB_Magic wdl_magic
)
{
	if (!wdl->m_lzw_file.open_readonly(sub_wdl.c_str()))
		throw std::runtime_error("Could not open WDL file trying to load " + sub_wdl.string());

	const Const_Span<uint8_t> input = wdl->m_lzw_file.data_span();

	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid WDL file size trying to load " + sub_wdl.string());

	Serial_Memory_Reader reader(input);

	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid WDL file checksum trying to load " + sub_wdl.string());

	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t block_size[COLOR_NB]{ 0, 0 };
	size_t tail_size[COLOR_NB]{ 0, 0 };

	size_t dict_size[COLOR_NB]{ 0, 0 };
	const uint8_t* lp_dict[COLOR_NB]{ nullptr, nullptr };
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };
	size_t data_size[COLOR_NB]{ 0, 0 };
	const uint8_t* offset_tb[COLOR_NB]{ nullptr, nullptr };

	const uint32_t magic = reader.read<uint32_t>();

	if (magic != narrowing_static_cast<uint32_t>(wdl_magic))
		throw std::runtime_error("Invalid WDL file magic trying to load " + sub_wdl.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in WDL file " + sub_wdl.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	for (const Color i : table_colors)
	{
		if (reader.read<uint8_t>() & 0x80)
		{
			wdl->m_is_singular[i] = true;
			wdl->m_single_val[i] = static_cast<WDL_Entry>(reader.read<uint8_t>());
		}
		else
		{
			wdl->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid WDL index permutation for " + sub_wdl.string());
			wdl->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);

			tail_size[i] = reader.read<uint16_t>();
			block_size[i] = reader.read<uint32_t>();
			block_cnt[i] = reader.read<uint64_t>();
			data_size[i] = reader.read<uint64_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		dict_size[i] = reader.read<uint16_t>();
		if (dict_size[i] != 0)
		{
			lp_dict[i] = reader.caret();
			reader.advance(dict_size[i]);
		}
	}

	uint8_t mono_params[COLOR_NB][3]{};  // log2_bu, sample_width, offset_width
	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		mono_params[i][0] = reader.read<uint8_t>();
		mono_params[i][1] = reader.read<uint8_t>();
		mono_params[i][2] = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		offset_tb[i] = reader.caret();
		reader.advance(Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, mono_params[i][0], mono_params[i][1], mono_params[i][2]));
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		ASSERT(data[i] != nullptr);
		ASSERT(offset_tb[i] != nullptr);

		const size_t num_full_sized_blocks =
			tail_size[i] != 0
			? block_cnt[i] - 1
			: block_cnt[i];
		const size_t file_sz = block_size[i] * num_full_sized_blocks + tail_size[i];
		if (file_sz != ceil_div(epsi.num_positions(), WDL_ENTRY_PACK_RATIO))
			throw std::runtime_error("Invalid decompressed size of WDL table from " + sub_wdl.string());

		auto& pc = wdl->m_per_color[i];
		pc.block_size = block_size[i];
		pc.tail_size = tail_size[i];
		pc.block_cnt = block_cnt[i];
		pc.compressed_data = data[i];
		pc.dict = LZ4_Dict::load(Const_Span(lp_dict[i], lp_dict[i] + dict_size[i]));
		pc.offsets = Mono_Uint_Vec(offset_tb[i], block_cnt[i] + 1,
		                           mono_params[i][0], mono_params[i][1], mono_params[i][2]);
	}
}

// Wire format: see save_egtb_table.
void load_dtm_table(
	Out_Param<DTM_File_For_Probe> dtm,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm,
	EGTB_Magic dtm_magic
)
{
	if (!dtm->m_lzdtm_file.open_readonly(sub_dtm.c_str()))
		throw std::runtime_error("Could not open DTM file trying to load " + sub_dtm.string());

	const Const_Span<uint8_t> input = dtm->m_lzdtm_file.data_span();

	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM file size trying to load " + sub_dtm.string());

	Serial_Memory_Reader reader(input);

	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM file checksum trying to load " + sub_dtm.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(dtm_magic))
		throw std::runtime_error("Invalid DTM file magic trying to load " + sub_dtm.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM file " + sub_dtm.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t data_size[COLOR_NB]{ 0, 0 };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			dtm->m_is_singular[i] = true;
			dtm->m_single_val[i] = static_cast<WDL_Entry>(reader.read<uint8_t>());
		}
		else
		{
			dtm->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid DTM index permutation for " + sub_dtm.string());
			dtm->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			auto& pc = dtm->m_per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != sizeof(DTM_Final_Entry) && pc.entry_bytes != 1)
				throw std::runtime_error("Bad DTM entry_bytes in " + sub_dtm.string());
			pc.tail_size  = reader.read<uint32_t>();
			pc.block_size = reader.read<uint32_t>();
			pc.block_cnt  = reader.read<uint64_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		auto& pc = dtm->m_per_color[i];
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1,
		                           log2_bu, sample_width, offset_width);
	}

	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		reader.align(64);
		dtm->m_per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		auto& pc = dtm->m_per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTM decompressed size mismatch " + sub_dtm.string());
	}
}

// Wire format: see save_dtm50_table.
void load_dtm50_table(
	Out_Param<DTM50_File_For_Probe> dtm50,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm50
)
{
	if (!dtm50->m_lzdtm50_file.open_readonly(sub_dtm50.c_str()))
		throw std::runtime_error("Could not open DTM50 file trying to load " + sub_dtm50.string());

	const Const_Span<uint8_t> input = dtm50->m_lzdtm50_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM50 file size trying to load " + sub_dtm50.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM50 file checksum trying to load " + sub_dtm50.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		throw std::runtime_error("Invalid DTM50 file magic trying to load " + sub_dtm50.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM50 file " + sub_dtm50.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t data_size[COLOR_NB]{ 0, 0 };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			dtm50->m_is_singular[i] = true;
			dtm50->m_single_val[i] = static_cast<WDL_Entry>(reader.read<uint8_t>());
			if (dtm50->m_single_val[i] != WDL_Entry::DRAW)
				throw std::runtime_error("DTM50 singular value must be DRAW in " + sub_dtm50.string());
		}
		else
		{
			dtm50->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid DTM50 index permutation for " + sub_dtm50.string());
			dtm50->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			auto& pc = dtm50->m_per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != 1 && pc.entry_bytes != 2)
				throw std::runtime_error("Bad DTM50 entry_bytes in " + sub_dtm50.string());
			pc.block_positions = reader.read<uint32_t>();
			pc.block_cnt       = reader.read<uint64_t>();
			pc.tail_positions  = reader.read<uint32_t>();
			data_size[i]       = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (dtm50->m_is_singular[i]) continue;
		auto& pc = dtm50->m_per_color[i];
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		const uint8_t usz_width    = reader.read<uint8_t>();
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		const uint8_t* usz_ptr = reader.caret();
		const size_t usz_bytes = Min0_Uint_Vec::on_disk_bytes(pc.block_cnt, usz_width);
		reader.advance(usz_bytes);
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1,
		                           log2_bu, sample_width, offset_width);
		pc.usizes = Min0_Uint_Vec(usz_ptr, pc.block_cnt, usz_width);
	}

	for (const Color i : table_colors)
	{
		if (dtm50->m_is_singular[i]) continue;
		reader.align(64);
		dtm50->m_per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}
}

// Eager-decode variant of load_dtm_table: every block decompresses up front,
// resolves WDL class inline, and lands in a mmap'd per-color tmp file. Sub-TB
// reads become a flat indexed memcpy; no LZMA on the read path.
void load_dtm_sub_flat(
	Out_Param<DTM_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool
)
{
	std::filesystem::path dtm_path;
	if (!egtb_files.find_dtm_file(ps, &dtm_path))
		throw std::runtime_error("Could not find a DTM file for " + ps.name());
	std::filesystem::path wdl_path;
	if (!egtb_files.find_wdl_file(ps, &wdl_path))
		throw std::runtime_error("Could not find a WDL file for " + ps.name()
			+ " (DTM consumes DTC's wdl/ output)");

	// Companion WDL for class resolution; per-block workers below read it
	// concurrently via their own thread_id.
	const WDL_File_For_Probe wdl(egtb_files, ps, thread_pool);

	Memory_Mapped_File map_file;
	if (!map_file.open_readonly(dtm_path.c_str()))
		throw std::runtime_error("Could not open DTM file trying to load " + dtm_path.string());

	const Const_Span<uint8_t> input = map_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM file size trying to load " + dtm_path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM file checksum trying to load " + dtm_path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM_MAGIC))
		throw std::runtime_error("Invalid DTM file magic trying to load " + dtm_path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM file " + dtm_path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t entry_bytes[COLOR_NB]{ 0, 0 };
	size_t tail_size[COLOR_NB]{ 0, 0 };
	size_t block_size[COLOR_NB]{ 0, 0 };
	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t data_size[COLOR_NB]{ 0, 0 };
	bool is_singular[COLOR_NB]{ false, false };
	std::vector<uint16_t> rank_to_value[COLOR_NB];
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			is_singular[i] = true;
			reader.advance(1);  // single_val byte (unused; resolved via WDL below)
		}
		else
		{
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid flat DTM index permutation for " + dtm_path.string());
			flat->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			entry_bytes[i] = reader.read<uint8_t>();
			if (entry_bytes[i] != sizeof(DTM_Final_Entry) && entry_bytes[i] != 1)
				throw std::runtime_error("Bad DTM entry_bytes in " + dtm_path.string());
			tail_size[i]  = reader.read<uint32_t>();
			block_size[i] = reader.read<uint32_t>();
			block_cnt[i]  = reader.read<uint64_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			rank_to_value[i].resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				rank_to_value[i][r] = reader.read<uint16_t>();
		}
	}

	Mono_Uint_Vec offsets[COLOR_NB];
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		offsets[i] = Mono_Uint_Vec(mono_ptr, block_cnt[i] + 1,
		                           log2_bu, sample_width, offset_width);
	}

	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	const size_t file_bytes = num_positions * sizeof(DTM_Final_Entry);

	for (const Color i : table_colors)
	{
		const auto tmp_path = egtb_files.dtm_sub_flat_path(ps, i);
		flat->m_tmp_files.track_path(tmp_path);

		Memory_Mapped_File out_map(Memory_Mapped_File::Access_Advice::RANDOM);
		if (!out_map.create(tmp_path.c_str(), file_bytes))
			throw std::runtime_error("Could not create flat sub-DTM at " + tmp_path.string());
		DTM_Final_Entry* out = reinterpret_cast<DTM_Final_Entry*>(out_map.data());

		if (is_singular[i])
		{
			// DRAW everywhere; ILLEGAL slots are allocated but never read
			// (sub-TB callers resolve to legal child indices).
			std::memset(out, 0, file_bytes);
		}
		else
		{
			const size_t num_full = tail_size[i] != 0 ? block_cnt[i] - 1 : block_cnt[i];
			const size_t src_sz   = block_size[i] * num_full + tail_size[i];
			if (src_sz != num_positions * entry_bytes[i])
				throw std::runtime_error("DTM decompressed size mismatch " + dtm_path.string());

			// Blocks write disjoint output slices; atomic fetch_add load-
			// balances cheap dsz==0 skip blocks against LZMA-heavy ones.
			const auto& r2v = rank_to_value[i];
			const size_t blocks = block_cnt[i];
			const size_t positions_per_block = block_size[i] / entry_bytes[i];
			std::atomic<size_t> next_block(0);

			const auto& perm_plan = flat->m_per_color[i].plan;

			thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
				LZMA_Decompress_Helper dc_helper(block_size[i]);

				for (;;)
				{
					const size_t idx = next_block.fetch_add(1, std::memory_order_relaxed);
					if (idx >= blocks) return;

					const auto pair = offsets[i].get2(idx);
					const size_t doff = pair[0];
					const size_t dsz  = pair[1] - pair[0];

					const size_t decode_sz =
						(idx == blocks - 1 && tail_size[i] != 0) ? tail_size[i] : block_size[i];
					const size_t positions = decode_sz / entry_bytes[i];
					const size_t pos = idx * positions_per_block;

					// Skip block: encoder collapsed an all-don't-care span; see
					// singular-DRAW arm above for why DRAW-fill is correct.
					if (dsz == 0)
					{
						std::memset(out + pos, 0, positions * sizeof(DTM_Final_Entry));
						continue;
					}

					const Const_Span<uint8_t> raw = dc_helper.decompress(
						Const_Span<uint8_t>(data[i] + doff, dsz), decode_sz);

					if (entry_bytes[i] == 1)
					{
						for (size_t k = 0; k < positions; ++k)
						{
							const uint16_t stored = r2v[raw[k]];
							const size_t logical = storage_index_to_logical_index(perm_plan, pos + k);
							const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical), thread_id);
							out[pos + k] = dtm_entry_from_storage(stored, w);
						}
					}
					else
					{
						const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
						for (size_t k = 0; k < positions; ++k)
						{
							const uint16_t stored = r2v[in[k]];
							const size_t logical = storage_index_to_logical_index(perm_plan, pos + k);
							const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical), thread_id);
							out[pos + k] = dtm_entry_from_storage(stored, w);
						}
					}
				}
			});
		}

		flat->m_per_color[i].file = std::move(out_map);
	}
}

// Eagerly decompress and remap the hmc=0 column of the rs-pack into a flat
// mmap'd array; higher-piece DTM50 generators read only that column.
// File / block layout is defined in egtb_gen_dtm50.cpp::save_to_disk.
void load_dtm50_sub_flat(
	Out_Param<DTM50_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool
)
{
	std::filesystem::path dtm_path;
	if (!egtb_files.find_dtm50_file(ps, &dtm_path))
		throw std::runtime_error("Could not find a DTM50 file for " + ps.name());
	std::filesystem::path wdl_path;
	if (!egtb_files.find_wdl_file(ps, &wdl_path))
		throw std::runtime_error("Could not find a WDL file for " + ps.name()
			+ " (DTM50 consumes DTC's wdl/ output for class disambiguation)");

	const WDL_File_For_Probe wdl(egtb_files, ps, thread_pool);

	Memory_Mapped_File map_file;
	if (!map_file.open_readonly(dtm_path.c_str()))
		throw std::runtime_error("Could not open DTM50 file trying to load " + dtm_path.string());

	const Const_Span<uint8_t> input = map_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM50 file size trying to load " + dtm_path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM50 file checksum trying to load " + dtm_path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		throw std::runtime_error("Invalid DTM50 file magic trying to load " + dtm_path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM50 file " + dtm_path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t entry_bytes[COLOR_NB]{ 0, 0 };
	size_t block_positions[COLOR_NB]{ 0, 0 };
	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t tail_positions[COLOR_NB]{ 0, 0 };
	size_t data_size[COLOR_NB]{ 0, 0 };
	bool is_singular[COLOR_NB]{ false, false };
	std::vector<uint16_t> rank_to_value[COLOR_NB];
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			is_singular[i] = true;
			reader.advance(1);  // singular_wdl byte; unused (DRAW everywhere)
		}
		else
		{
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid flat DTM50 index permutation for " + dtm_path.string());
			flat->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			entry_bytes[i] = reader.read<uint8_t>();
			if (entry_bytes[i] != 1 && entry_bytes[i] != 2)
				throw std::runtime_error("Bad DTM50 entry_bytes in " + dtm_path.string());
			block_positions[i] = reader.read<uint32_t>();
			block_cnt[i]       = reader.read<uint64_t>();
			tail_positions[i]  = reader.read<uint32_t>();
			data_size[i]       = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			rank_to_value[i].resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				rank_to_value[i][r] = reader.read<uint16_t>();
		}
	}

	Mono_Uint_Vec offsets[COLOR_NB];
	Min0_Uint_Vec usizes[COLOR_NB];
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		const uint8_t usz_width    = reader.read<uint8_t>();
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		const uint8_t* usz_ptr = reader.caret();
		const size_t usz_bytes = Min0_Uint_Vec::on_disk_bytes(block_cnt[i], usz_width);
		reader.advance(usz_bytes);
		offsets[i] = Mono_Uint_Vec(mono_ptr, block_cnt[i] + 1,
		                           log2_bu, sample_width, offset_width);
		usizes[i] = Min0_Uint_Vec(usz_ptr, block_cnt[i], usz_width);
	}
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	const size_t file_bytes = num_positions * sizeof(DTM_Final_Entry);

	for (const Color i : table_colors)
	{
		const auto tmp_path = egtb_files.dtm50_sub_flat_path(ps, i);
		flat->m_tmp_files.track_path(tmp_path);

		Memory_Mapped_File out_map(Memory_Mapped_File::Access_Advice::RANDOM);
		if (!out_map.create(tmp_path.c_str(), file_bytes))
			throw std::runtime_error("Could not create flat sub-DTM50 at " + tmp_path.string());
		DTM_Final_Entry* out = reinterpret_cast<DTM_Final_Entry*>(out_map.data());

		if (is_singular[i])
		{
			// DRAW everywhere; see load_dtm_sub_flat singular arm.
			std::memset(out, 0, file_bytes);
			flat->m_per_color[i].file = std::move(out_map);
			continue;
		}

		const auto& r2v = rank_to_value[i];
		const size_t blocks = block_cnt[i];
		const size_t ppb = block_positions[i];
		const size_t eb = entry_bytes[i];
		std::atomic<size_t> next_block(0);

		const auto& perm_plan = flat->m_per_color[i].plan;

		// Strict upper bound: sums the worst case for each state region (they're
		// mutually exclusive per position, so this overestimates).
		const size_t max_payload =
			24                                                       // header
			+ (ppb * 2 + 7) / 8                                      // state_bits
			+ ppb * eb                                               // all CONST
			+ (ppb + 7) / 8 + ppb * (1 + 2 * eb)                     // all SINGLE
			+ (ppb + 7) / 8 + ppb * (2 + 3 * eb)                     // all DOUBLE
			+ (ppb + 1) * 4 + ppb * (1 + 16 + DTM50_HMC_COUNT * eb)  // all MULTI
			+ 3;                                                     // tail alignment

		thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
			LZMA_Decompress_Helper dc_helper(max_payload);

			for (;;)
			{
				const size_t bidx = next_block.fetch_add(1, std::memory_order_relaxed);
				if (bidx >= blocks) return;

				const auto pair = offsets[i].get2(bidx);
				const size_t doff = pair[0];
				const size_t dsz  = pair[1] - pair[0];
				const size_t usz  = usizes[i].get(bidx);

				const size_t this_bp =
					(bidx == blocks - 1 && tail_positions[i] != 0) ? tail_positions[i] : ppb;
				const size_t base_pos = bidx * ppb;

				// Skip sentinel: all-don't-care span, DRAW-fill.
				if (dsz == 0)
				{
					std::memset(out + base_pos, 0,
						this_bp * sizeof(DTM_Final_Entry));
					continue;
				}

				const Const_Span<uint8_t> raw = dc_helper.decompress(
					Const_Span<uint8_t>(data[i] + doff, dsz), usz);

				const uint8_t* p = raw.data();
				uint32_t np32, num_single, num_double, num_multi, ss_bytes32, ds_bytes32;
				std::memcpy(&np32,       p,      4);
				std::memcpy(&num_single, p + 4,  4);
				std::memcpy(&num_double, p + 8,  4);
				std::memcpy(&num_multi,  p + 12, 4);
				std::memcpy(&ss_bytes32, p + 16, 4);
				std::memcpy(&ds_bytes32, p + 20, 4);
				ASSERT(np32 == this_bp);
				const size_t num_const  = np32 - num_single - num_double - num_multi;
				const size_t sb_bytes   = (np32 * 2 + 7) / 8;
				const size_t sh_bytes   = (num_single + 7) / 8;
				const size_t dh_bytes   = (num_double + 7) / 8;
				p += 24;

				const uint8_t* state_bits   = p;                            p += sb_bytes;
				const uint8_t* const_stream = p;                            p += num_const * eb;
				const uint8_t* single_hints = p;                            p += sh_bytes;
				const uint8_t* single_stream = p;                           p += ss_bytes32;
				const uint8_t* double_hints = p;                            p += dh_bytes;
				const uint8_t* double_stream = p;                           p += ds_bytes32;
				p += (4 - ((p - raw.data()) & 3)) & 3;
				const uint32_t* multi_dir = reinterpret_cast<const uint32_t*>(p);
				p += (num_multi + 1) * 4;
				const uint8_t* multi_data = p;

				// hmc=0 read: r0 lives at a fixed entry offset per state
				// (CONST: 0, SINGLE: 1, DOUBLE: 2, MULTI: 17). Sequential walk
				// keeps per-state running indices + variable-length stream
				// offsets for SINGLE/DOUBLE. Draw-end hint can't fire here:
				// h >= 1 so hmc=0 is always strictly before the first transition.
				const size_t single_short = 1 + eb;
				const size_t single_long  = 1 + 2 * eb;
				const size_t double_short = 2 + 2 * eb;
				const size_t double_long  = 2 + 3 * eb;
				size_t const_idx = 0, single_idx = 0, double_idx = 0, multi_idx = 0;
				size_t single_off = 0, double_off = 0;

				for (size_t k = 0; k < this_bp; ++k)
				{
					const size_t bit_off = k * 2;
					const uint8_t state = (state_bits[bit_off / 8] >> (bit_off % 8)) & 3u;
					uint16_t rank;
					switch (state)
					{
						case 0: {
							if (eb == 1) rank = const_stream[const_idx];
							else { uint16_t r; std::memcpy(&r, const_stream + const_idx * 2, 2); rank = r; }
							++const_idx;
							break;
						}
						case 1: {
							const uint8_t* entry = single_stream + single_off;
							if (eb == 1) rank = entry[1];
							else { uint16_t r; std::memcpy(&r, entry + 1, 2); rank = r; }
							const bool draw_end =
								(single_hints[single_idx >> 3] >> (single_idx & 7)) & 1u;
							single_off += draw_end ? single_short : single_long;
							++single_idx;
							break;
						}
						case 2: {
							const uint8_t* entry = double_stream + double_off;
							if (eb == 1) rank = entry[2];
							else { uint16_t r; std::memcpy(&r, entry + 2, 2); rank = r; }
							const bool draw_end =
								(double_hints[double_idx >> 3] >> (double_idx & 7)) & 1u;
							double_off += draw_end ? double_short : double_long;
							++double_idx;
							break;
						}
						default: {
							const uint8_t* entry = multi_data + multi_dir[multi_idx];
							if (eb == 1) rank = entry[17];
							else { uint16_t r; std::memcpy(&r, entry + 17, 2); rank = r; }
							++multi_idx;
							break;
						}
					}
					const uint16_t stored = r2v[rank];
					const size_t logical = storage_index_to_logical_index(perm_plan, base_pos + k);
					const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical), thread_id);
					out[base_pos + k] = dtm50_entry_from_storage(stored, w);
				}
				ASSERT(const_idx == num_const && single_idx == num_single
				    && double_idx == num_double && multi_idx == num_multi);
			}
		});

		flat->m_per_color[i].file = std::move(out_map);
	}
}
