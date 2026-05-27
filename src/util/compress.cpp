#include "compress.h"

#include "util/defines.h"
#include "util/fixed_vector.h"
#include "util/allocation.h"
#include "util/progress_bar.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>

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

std::vector<std::vector<uint8_t>> compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor_factory,
	std::string task_name,
	size_t max_workers,
	bool silent
)
{
	const size_t source_block_size = compressor_factory->source_bytes_per_block(block_size);

	const size_t total = src.total_size;
	const size_t num_blocks = ceil_div(total, block_size);
	std::vector<std::vector<uint8_t>> compressed_blocks(num_blocks);
	std::atomic<size_t> next_block_id(0);

	const size_t pool_workers = thread_pool->num_workers();
	const size_t effective_workers = (max_workers == 0)
		? pool_workers
		: std::min(max_workers, pool_workers);

	constexpr size_t PRINT_PERIOD_BYTES = 1024 * 1024 * 8;
	const size_t PRINT_PERIOD = ceil_div(PRINT_PERIOD_BYTES * effective_workers, block_size);
	std::optional<Concurrent_Progress_Bar> progress_bar;
	if (!silent) progress_bar.emplace(num_blocks, PRINT_PERIOD, task_name);

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
				if (progress_bar) *progress_bar += 1;
				continue;
			}

			const size_t out_sz = c_helper->compress(
				Span(compressed_block_buffer.get(), bound_size),
				block
			);

			compressed_blocks[block_id] = std::vector(compressed_block_buffer.get(), compressed_block_buffer.get() + out_sz);

			if (progress_bar) *progress_bar += 1;
		}
	});

	if (progress_bar) progress_bar->set_finished();

	return compressed_blocks;
}

