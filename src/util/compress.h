#pragma once

#define LZ4_STATIC_LINKING_ONLY
#define LZ4_HC_STATIC_LINKING_ONLY

#include "lz4/lz4.h"
#include "lz4/lz4hc.h"

#include "zstd/zdict.h"

#include "LZMA/LzmaLib.h"

#include "util/defines.h"
#include "util/filesystem.h"
#include "util/span.h"
#include "util/thread_pool.h"
#include "util/param.h"

#include <atomic>
#include <climits>
#include <cstring>
#include <functional>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

struct Compressed_Block_Store
{
	static constexpr size_t INLINE_SOURCE_BYTE_LIMIT = 1ull << 36;  // 64 GiB

	Compressed_Block_Store() = default;
	Compressed_Block_Store(std::filesystem::path path, size_t num_blocks, size_t block_size);
	~Compressed_Block_Store() = default;

	Compressed_Block_Store(const Compressed_Block_Store&) = delete;
	Compressed_Block_Store& operator=(const Compressed_Block_Store&) = delete;
	Compressed_Block_Store(Compressed_Block_Store&& other) noexcept;
	Compressed_Block_Store& operator=(Compressed_Block_Store&& other) noexcept;

	void set(size_t block_id, Const_Span<uint8_t> block);
	void clear(size_t block_id);

	NODISCARD size_t size() const { return spilled() ? m_sizes.size() : m_blocks.size(); }
	NODISCARD size_t block_size(size_t block_id) const { return spilled() ? m_sizes[block_id] : m_blocks[block_id].size(); }
	NODISCARD size_t total_size() const;
	NODISCARD Const_Span<uint8_t> block(size_t block_id) const;

private:
	std::vector<std::vector<uint8_t>> m_blocks;

	// Spill backing; active iff m_path is set.
	std::vector<uint64_t> m_offsets;
	std::vector<uint64_t> m_sizes;
	std::atomic<uint64_t> m_total_size{ 0 };  // also the append position of the next block
	Temporary_File_Tracker m_tmp_files;
	std::filesystem::path m_path;
	mutable Positional_Output_File m_out;
	static constexpr size_t WRITES_CLOSED = size_t{1} << (sizeof(size_t) * CHAR_BIT - 1);
	mutable std::atomic<size_t> m_writer_state{ 0 };  // high bit = closed; remainder = writes in flight
	mutable std::atomic<bool> m_finalized{ false };
	mutable Memory_Mapped_File m_map{ Memory_Mapped_File::Access_Advice::SEQUENTIAL };
	mutable std::mutex m_mutex;

	NODISCARD bool spilled() const { return !m_path.empty(); }
	void swap(Compressed_Block_Store& other) noexcept;
};

struct LZ4_Dict
{
	LZ4_Dict() = default;

	NODISCARD static LZ4_Dict load(Const_Span<uint8_t> data)
	{
		return LZ4_Dict(data);
	}

	// The sample size must divide the size of the data.
	// The dictionary will be of size <=dict_size bytes.
	NODISCARD static LZ4_Dict make(
		Const_Span<uint8_t> data,
		size_t dict_size,
		size_t sample_size
	)
	{
		return LZ4_Dict(data, dict_size, sample_size);
	}

	NODISCARD bool empty() const
	{
		return m_dict.empty();
	}

	NODISCARD size_t size() const
	{
		return m_dict.size();
	}

	NODISCARD const uint8_t* data() const
	{
		return m_dict.data();
	}

private:
	std::vector<uint8_t> m_dict;

	LZ4_Dict(Const_Span<uint8_t> data)
	{
		m_dict.assign(data.begin(), data.end());
	}

	LZ4_Dict(
		Const_Span<uint8_t> data,
		size_t dict_size,
		size_t sample_size
	);
};

struct Compress_Helper
{
	NODISCARD virtual size_t compress_bound(size_t size) const = 0;

	NODISCARD virtual std::vector<uint8_t> compress(Const_Span<uint8_t> src) = 0;

	NODISCARD virtual size_t compress(Span<uint8_t> dest, Const_Span<uint8_t> src) = 0;

	NODISCARD virtual size_t source_bytes_per_block(size_t output_block_bytes) const
	{
		return output_block_bytes;
	}

	NODISCARD virtual std::unique_ptr<Compress_Helper> clone() const = 0;

	virtual ~Compress_Helper() {};
};

// It is not thread safe. Use it as a factory and clone() when to be
// used in a multithreaded context.
struct LZ4_Compress_Helper : public Compress_Helper
{
	LZ4_Compress_Helper(const LZ4_Dict* dict) :
		m_lz4_stream(LZ4_createStreamHC()),
		m_dict(dict)
	{
	}

	~LZ4_Compress_Helper() override
	{
		LZ4_freeStreamHC(m_lz4_stream);
	}

	NODISCARD size_t compress_bound(size_t size) const override
	{
		return LZ4_compressBound(narrowing_static_cast<int>(size));
	}

	NODISCARD std::vector<uint8_t> compress(Const_Span<uint8_t> src) override;

	NODISCARD size_t compress(Span<uint8_t> dst, Const_Span<uint8_t> src) override
	{
		int ret;
		if (m_dict == nullptr)
		{
			ret = LZ4_compress_HC(
				reinterpret_cast<const char*>(src.data()),
				reinterpret_cast<char*>(dst.data()),
				narrowing_static_cast<int>(src.size()),
				narrowing_static_cast<int>(dst.size()),
				LZ4HC_CLEVEL_MAX
			);
		}
		else
		{
			LZ4_loadDictHC(m_lz4_stream, reinterpret_cast<const char*>(m_dict->data()), narrowing_static_cast<int>(m_dict->size()));
			LZ4_setCompressionLevel(m_lz4_stream, LZ4HC_CLEVEL_MAX);
			ret = LZ4_compress_HC_continue(
				m_lz4_stream,
				reinterpret_cast<const char*>(src.data()),
				reinterpret_cast<char*>(dst.data()),
				narrowing_static_cast<int>(src.size()),
				narrowing_static_cast<int>(dst.size())
			);
		}

		if (ret <= 0)
			throw std::runtime_error("LZ4 error when trying to compress a block.");

		return static_cast<size_t>(ret);
	}

	NODISCARD virtual std::unique_ptr<Compress_Helper> clone() const override
	{
		return std::make_unique<LZ4_Compress_Helper>(m_dict);
	}

private:
	LZ4_streamHC_t* m_lz4_stream;
	const LZ4_Dict* m_dict;
};

struct LZMA_Compress_Helper : public Compress_Helper
{
	static constexpr unsigned int DICT_SIZE = 1 << 20;
	static constexpr int LEVEL = 9;
	static constexpr int LC = 3;
	static constexpr int LP = 0;
	static constexpr int PB = 0;
	static constexpr int FB = 128;
	static constexpr int NUM_THREADS = 1;

	NODISCARD size_t compress_bound(size_t size) const override
	{
		return size + size / 10 + 65536 + LZMA_PROPS_SIZE;
	}

	NODISCARD std::vector<uint8_t> compress(Const_Span<uint8_t> src) override;

	NODISCARD size_t compress(Span<uint8_t> dest, Const_Span<uint8_t> src) override
	{
		uint8_t props[LZMA_PROPS_SIZE] = { 0 };

		size_t outPropsSize = LZMA_PROPS_SIZE;
		size_t out_sz = dest.size();
		const int ret = LzmaCompress(
			dest.data(),
			&out_sz,
			src.data(),
			src.size(),
			props,
			&outPropsSize,
			LEVEL,
			DICT_SIZE,
			LC, LP,
			PB, FB,
			NUM_THREADS
		);

		if (outPropsSize != LZMA_PROPS_SIZE)
			throw std::runtime_error("Unexpected number of out props from LZMA compression.");

		if (ret != SZ_OK)
			throw std::runtime_error("LZMA error when trying to compress a block.");

		if (out_sz + LZMA_PROPS_SIZE > dest.size())
			throw std::runtime_error("Destination buffer not sufficient to fit LZMA props.");

		memcpy(dest.data() + out_sz, props, sizeof(props));

		return out_sz + sizeof(props);
	}

	NODISCARD virtual std::unique_ptr<Compress_Helper> clone() const override
	{
		return std::make_unique<LZMA_Compress_Helper>();
	}
};

// Scratch for one decode, from a buffer this thread keeps between decodes and
// frees when it exits. Overwritten by the next call on this thread.
NODISCARD Span<uint8_t> decompress_scratch(size_t bytes);

// `total_size` is in probe-visible (output) bytes; the helper translates to
// source-side via `Compress_Helper::source_bytes_per_block`. Block ids run
// [0, ceil_div(total_size, block_size)).
//
// `get` is called concurrently across workers with different block ids — it must
// be thread-safe (typically: lock around any shared paging state it touches).
struct Block_Source
{
	size_t total_size;
	std::function<size_t(size_t first_block, size_t block_size)> next_dispatch;
	std::function<Const_Span<uint8_t>(size_t worker_id, size_t block_id, Span<uint8_t> scratch)> get;
	std::function<void(size_t worker_id)> finish_worker;
};

NODISCARD Compressed_Block_Store compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor,
	size_t max_workers,
	std::string task_name,
	std::filesystem::path spill_path
);
