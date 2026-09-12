#include "compress.h"

#include "util/defines.h"
#include "util/fixed_vector.h"
#include "util/allocation.h"
#include "util/progress_bar.h"
#include "util/utility.h"

#include <algorithm>
#include <memory>
#include <cstring>
#include <filesystem>
#include <thread>

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
	std::filesystem::create_directories(m_path.parent_path());
	if (!m_out.create(m_path))
		print_and_abort("Could not open compressed block spill file: %s\n", m_path.string().c_str());
	m_tmp_files.track_path(m_path);
}

Compressed_Block_Store::Compressed_Block_Store(Compressed_Block_Store&& other) noexcept
{
	swap(other);
}

Compressed_Block_Store& Compressed_Block_Store::operator=(Compressed_Block_Store&& other) noexcept
{
	swap(other);
	return *this;
}

void Compressed_Block_Store::swap(Compressed_Block_Store& other) noexcept
{
	using std::swap;
	swap(m_blocks, other.m_blocks);
	swap(m_offsets, other.m_offsets);
	swap(m_sizes, other.m_sizes);
	swap(m_tmp_files, other.m_tmp_files);
	swap(m_path, other.m_path);
	swap(m_out, other.m_out);
	swap(m_map, other.m_map);

	const uint64_t total_size = m_total_size.load(std::memory_order_relaxed);
	m_total_size.store(other.m_total_size.load(std::memory_order_relaxed), std::memory_order_relaxed);
	other.m_total_size.store(total_size, std::memory_order_relaxed);

	const size_t writer_state = m_writer_state.load(std::memory_order_relaxed);
	m_writer_state.store(other.m_writer_state.load(std::memory_order_relaxed), std::memory_order_relaxed);
	other.m_writer_state.store(writer_state, std::memory_order_relaxed);

	const bool finalized = m_finalized.load(std::memory_order_relaxed);
	m_finalized.store(other.m_finalized.load(std::memory_order_relaxed), std::memory_order_relaxed);
	other.m_finalized.store(finalized, std::memory_order_relaxed);
}

size_t Compressed_Block_Store::total_size() const
{
	if (spilled())
		return m_total_size.load(std::memory_order_relaxed);

	size_t total = 0;
	for (const auto& b : m_blocks)
		total += b.size();
	return total;
}

void Compressed_Block_Store::set(size_t block_id, Const_Span<uint8_t> block)
{
	if (block.size() == 0)
	{
		clear(block_id);
		return;
	}

	if (!spilled())
	{
		m_blocks[block_id].assign(block.begin(), block.end());
		return;
	}

	size_t state = m_writer_state.load(std::memory_order_relaxed);
	for (;;)
	{
		if (state & WRITES_CLOSED)
			print_and_abort("Write to a compressed block spill file already being read: %s\n", m_path.string().c_str());
		if (m_writer_state.compare_exchange_weak(
				state, state + 1, std::memory_order_acquire, std::memory_order_relaxed))
			break;
	}

	const uint64_t offset = m_total_size.fetch_add(block.size(), std::memory_order_relaxed);
	m_offsets[block_id] = offset;
	m_sizes[block_id] = block.size();

	const bool ok = m_out.write_at(offset, block);
	m_writer_state.fetch_sub(1, std::memory_order_release);

	if (!ok)
		print_and_abort("Write error on compressed block spill file: %s\n", m_path.string().c_str());
}

void Compressed_Block_Store::clear(size_t block_id)
{
	if (!spilled())
	{
		m_blocks[block_id].clear();
		return;
	}

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

	if (!m_finalized.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(m_mutex);

		if (!m_finalized.load(std::memory_order_relaxed))
		{
			m_writer_state.fetch_or(WRITES_CLOSED, std::memory_order_acq_rel);
			if (m_out.is_open())
			{
				while (m_writer_state.load(std::memory_order_acquire) != WRITES_CLOSED)
					std::this_thread::yield();
				m_out.close_file();
			}

			if (m_map.data() == nullptr && !m_map.open_readonly(m_path))
				print_and_abort("Could not mmap compressed block spill file: %s\n", m_path.string().c_str());

			m_finalized.store(true, std::memory_order_release);
		}
	}

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

Span<uint8_t> decompress_scratch(size_t bytes)
{
	struct Buffer
	{
		std::unique_ptr<uint8_t[]> data;
		size_t capacity = 0;
	};
	thread_local Buffer buffer;

	if (buffer.capacity < bytes)
	{
		buffer.data = cpp20::make_unique_for_overwrite<uint8_t[]>(bytes);
		buffer.capacity = bytes;
	}

	return Span<uint8_t>(buffer.data.get(), bytes);
}

Compressed_Block_Store compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor_factory,
	size_t max_workers,
	std::string task_name,
	std::filesystem::path spill_path
)
{
	const size_t source_block_size = compressor_factory->source_bytes_per_block(block_size);

	const size_t total = src.total_size;
	const size_t num_blocks = ceil_div(total, block_size);
	Compressed_Block_Store compressed_blocks(std::move(spill_path), num_blocks, block_size);
	std::atomic<size_t> next_block_id(0);

	const size_t pool_workers = thread_pool->num_workers();
	const size_t capped_workers = (max_workers == 0)
		? pool_workers
		: std::min(max_workers, pool_workers);
	const size_t workers = std::max<size_t>(1, std::min(capped_workers, num_blocks));

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8;
	const size_t PRINT_PERIOD = ceil_div(PRINT_PERIOD_BYTES * workers, block_size);
	Concurrent_Progress_Bar progress_bar(num_blocks, PRINT_PERIOD, task_name);

	thread_pool->run_sync_task_on_multiple_threads(workers, [&](size_t worker_id) {
		std::unique_ptr<Compress_Helper> c_helper = compressor_factory->clone();

		const size_t bound_size = c_helper->compress_bound(source_block_size);
		auto compressed_block_buffer = cpp20::make_unique_for_overwrite<uint8_t[]>(bound_size);
		auto scratch_buffer          = cpp20::make_unique_for_overwrite<uint8_t[]>(source_block_size);

		for (;;)
		{
			size_t first_block = next_block_id.load(std::memory_order_relaxed);
			size_t end_block;
			for (;;)
			{
				if (first_block >= num_blocks) break;
				end_block = src.next_dispatch(first_block, block_size);
				end_block = std::min(end_block, num_blocks);
				ASSERT(end_block > first_block);
				if (next_block_id.compare_exchange_weak(
						first_block, end_block, std::memory_order_relaxed))
					break;
			}
			if (first_block >= num_blocks) break;

			for (size_t block_id = first_block; block_id < end_block; ++block_id)
			{
				const Const_Span<uint8_t> block = src.get(
					worker_id, block_id, Span<uint8_t>(scratch_buffer.get(), source_block_size));
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
					Const_Span<uint8_t>(compressed_block_buffer.get(), out_sz));

				progress_bar += 1;
			}
		}
		src.finish_worker(worker_id);
	});

	progress_bar.set_finished();

	return compressed_blocks;
}
