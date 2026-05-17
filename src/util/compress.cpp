#include "compress.h"

#include "util/defines.h"
#include "util/fixed_vector.h"
#include "util/allocation.h"
#include "util/progress_bar.h"

#include <algorithm>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>

Compressed_Block_Store::Compressed_Block_Store(std::filesystem::path path, size_t num_blocks, size_t block_size)
{
	if (num_blocks * block_size <= INLINE_SOURCE_BYTE_LIMIT)
	{
		m_blocks.resize(num_blocks);
		return;
	}

	m_offsets.assign(num_blocks, 0);
	m_sizes.assign(num_blocks, 0);
	m_path = std::move(path);
}

Compressed_Block_Store::Compressed_Block_Store(Compressed_Block_Store&& other) noexcept
{
	swap(other);
}

Compressed_Block_Store& Compressed_Block_Store::operator=(Compressed_Block_Store&& other) noexcept
{
	// Hand our current state (incl. any spill file) to other, which frees it on
	// destruction. In practice the target is always a fresh/empty store.
	swap(other);
	return *this;
}

void Compressed_Block_Store::swap(Compressed_Block_Store& other) noexcept
{
	using std::swap;
	swap(m_blocks, other.m_blocks);
	swap(m_offsets, other.m_offsets);
	swap(m_sizes, other.m_sizes);
	swap(m_total_size, other.m_total_size);
	swap(m_tmp_files, other.m_tmp_files);
	swap(m_path, other.m_path);
	swap(m_out, other.m_out);
	swap(m_map, other.m_map);
}

size_t Compressed_Block_Store::total_size() const
{
	if (spilled())
		return m_total_size;

	size_t total = 0;
	for (const auto& b : m_blocks)
		total += b.size();
	return total;
}

void Compressed_Block_Store::set(size_t block_id, std::vector<uint8_t>&& block)
{
	if (block.empty())
	{
		clear(block_id);
		return;
	}

	// Heap-backed: workers write distinct indices of a pre-sized vector, no lock.
	if (!spilled())
	{
		m_blocks[block_id] = std::move(block);
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);

	if (!m_out.is_open())  // create the spill file lazily, on the first write
	{
		std::filesystem::create_directories(m_path.parent_path());
		m_out.open(m_path, std::ios::binary | std::ios::trunc);
		if (!m_out)
			throw std::runtime_error("Could not open compressed block spill file: " + m_path.string());
		m_tmp_files.track_path(m_path);
	}

	m_offsets[block_id] = m_total_size;
	m_sizes[block_id] = block.size();
	m_out.write(reinterpret_cast<const char*>(block.data()), block.size());
	if (!m_out)
		throw std::runtime_error("Write error on compressed block spill file: " + m_path.string());
	m_total_size += block.size();
}

void Compressed_Block_Store::clear(size_t block_id)
{
	if (!spilled())
	{
		m_blocks[block_id].clear();
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_offsets[block_id] = 0;
	m_sizes[block_id] = 0;
}

Const_Span<uint8_t> Compressed_Block_Store::block(size_t block_id) const
{
	if (!spilled())
	{
		const auto& b = m_blocks[block_id];
		return b.empty() ? Const_Span<uint8_t>() : Const_Span<uint8_t>(b);
	}

	const size_t sz = m_sizes[block_id];
	if (sz == 0)
		return {};

	std::lock_guard<std::mutex> lock(m_mutex);

	// First read after the write phase: finalize the spill file, then mmap it.
	if (m_out.is_open())
	{
		m_out.flush();
		m_out.close();
		if (!m_out)
			throw std::runtime_error("Close error on compressed block spill file: " + m_path.string());
	}
	if (m_map.data() == nullptr && !m_map.open_readonly(m_path))
		throw std::runtime_error("Could not mmap compressed block spill file: " + m_path.string());

	return Const_Span<uint8_t>(m_map.data() + m_offsets[block_id], sz);
}

LZ4_Dict::LZ4_Dict(
	Const_Span<uint8_t> data,
	size_t dict_size,
	size_t sample_size
) :
	m_dict(dict_size)
{
	if (data.size() % sample_size != 0)
		throw std::runtime_error("LZ4 dict sample size must divide the data size.");

	const size_t sample_count = data.size() / sample_size;

	if (sample_count == 0)
		throw std::runtime_error("LZ4 dict no samples.");

	const std::vector<size_t> sample_sizes(sample_count, sample_size);

	const size_t new_size = ZDICT_trainFromBuffer(
		m_dict.data(),
		m_dict.size(),
		data.data(),
		sample_sizes.data(),
		narrowing_static_cast<unsigned int>(sample_count)
	);

	if (ZDICT_isError(new_size))
		m_dict.clear();
	else
	{
		ASSUME(new_size <= m_dict.size());
		m_dict.resize(new_size);
	}
}

LZ4_Decompress_Helper::LZ4_Decompress_Helper(const LZ4_Dict& dict, size_t max_output_size) :
	m_output_buffer(cpp20::make_unique_for_overwrite<uint8_t[]>(dict.size() + max_output_size)),
	m_dict_size(dict.size()),
	m_max_output_size(max_output_size)
{
	if (dict.size())
		std::memcpy(m_output_buffer.get(), dict.data(), dict.size());
}

std::vector<uint8_t> LZ4_Compress_Helper::compress(
	Const_Span<uint8_t> src
)
{
	const size_t bound_size = compress_bound(src.size());
	auto compressed_block_buffer = cpp20::make_unique_for_overwrite<uint8_t[]>(bound_size);
	const size_t out_sz = compress(
		Span(compressed_block_buffer.get(), bound_size),
		src
	);
	return std::vector(compressed_block_buffer.get(), compressed_block_buffer.get() + out_sz);
}

std::vector<uint8_t> LZMA_Compress_Helper::compress(Const_Span<uint8_t> src)
{
	const size_t bound_size = compress_bound(src.size());
	auto compressed_block_buffer = cpp20::make_unique_for_overwrite<uint8_t[]>(bound_size);
	const size_t out_sz = compress(
		Span(compressed_block_buffer.get(), bound_size),
		src
	);
	return std::vector(compressed_block_buffer.get(), compressed_block_buffer.get() + out_sz);
}

LZMA_Decompress_Helper::LZMA_Decompress_Helper(size_t max_output_size) :
	m_output_buffer(cpp20::make_unique_for_overwrite<uint8_t[]>(max_output_size)),
	m_max_output_size(max_output_size)
{
}

Compressed_Block_Store compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor_factory,
	std::string task_name,
	std::filesystem::path spill_path,
	size_t max_workers
)
{
	const size_t source_block_size = compressor_factory->source_bytes_per_block(block_size);

	const size_t total = src.total_size;
	const size_t num_blocks = ceil_div(total, block_size);
	Compressed_Block_Store compressed_blocks(std::move(spill_path), num_blocks, block_size);
	std::atomic<size_t> next_block_id(0);

	const size_t pool_workers = thread_pool->num_workers();
	const size_t effective_workers = (max_workers == 0)
		? pool_workers
		: std::min(max_workers, pool_workers);

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8;
	const size_t PRINT_PERIOD = ceil_div(PRINT_PERIOD_BYTES * effective_workers, block_size);
	Concurrent_Progress_Bar progress_bar(num_blocks, PRINT_PERIOD, task_name);

	thread_pool->run_sync_task_on_all_threads([&](size_t thread_id) {
		if (thread_id >= effective_workers) return;
		std::unique_ptr<Compress_Helper> c_helper = compressor_factory->clone();

		const size_t bound_size = c_helper->compress_bound(source_block_size);
		// One scratch pair per worker (allocated once, reused for every block
		// this worker claims). `scratch` is only touched by sources that
		// materialize on demand; zero-copy sources return a stable view.
		auto compressed_block_buffer = cpp20::make_unique_for_overwrite<uint8_t[]>(bound_size);
		auto scratch_buffer          = cpp20::make_unique_for_overwrite<uint8_t[]>(source_block_size);

		for (;;)
		{
			const size_t block_id = next_block_id.fetch_add(1);
			if (block_id >= num_blocks) return;

			const Const_Span<uint8_t> block = src.get(
				block_id, Span<uint8_t>(scratch_buffer.get(), source_block_size));
			if (block.size() == 0)
			{
				progress_bar += 1;
				continue;
			}

			const size_t out_sz = c_helper->compress(
				Span(compressed_block_buffer.get(), bound_size),
				block
			);

			compressed_blocks.set(
				block_id,
				std::vector<uint8_t>(compressed_block_buffer.get(), compressed_block_buffer.get() + out_sz));

			progress_bar += 1;
		}
	});

	progress_bar.set_finished();

	return compressed_blocks;
}
