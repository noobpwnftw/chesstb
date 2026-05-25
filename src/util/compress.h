#pragma once

#define LZ4_STATIC_LINKING_ONLY
#define LZ4_HC_STATIC_LINKING_ONLY

#include "lz4/lz4.h"
#include "lz4/lz4hc.h"

#include "zstd/zdict.h"

#include "LZMA/LzmaLib.h"

#include "util/defines.h"
#include "util/span.h"
#include "util/thread_pool.h"
#include "util/param.h"

#include <cstring>
#include <functional>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <functional>
#include <vector>

// Represents an lz4 dictionary.
struct LZ4_Dict
{
	// Empty dictionary (used as a placeholder before load/make is called).
	LZ4_Dict() = default;

	// Copies the raw dictionary data from given memory span.
	NODISCARD static LZ4_Dict load(Const_Span<uint8_t> data)
	{
		return LZ4_Dict(data);
	}

	// Creates a dictionary for given data, divided into samples of sample_size.
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

// A polymorphic compressor.
struct Compress_Helper
{
	NODISCARD virtual size_t compress_bound(size_t size) const = 0;

	NODISCARD virtual std::vector<uint8_t> compress(Const_Span<uint8_t> src) = 0;

	// Compresses src into dest. Returns bytes written. Throws on error.
	NODISCARD virtual size_t compress(Span<uint8_t> dest, Const_Span<uint8_t> src) = 0;

	// Maps probe-visible (output) block bytes to the source-side bytes the
	// helper consumes. Identity by default; DTC helpers override since they
	// pack raw 2-byte entries into entry_bytes-wide ranks.
	NODISCARD virtual size_t source_bytes_per_block(size_t output_block_bytes) const
	{
		return output_block_bytes;
	}

	// Per-thread copy so stateful compressors (e.g. LZ4 with a dict) don't share state.
	NODISCARD virtual std::unique_ptr<Compress_Helper> clone() const = 0;

	virtual ~Compress_Helper() {};
};

// A compressor utilizing LZ4.
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

// A compressor utilizing LZMA.
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

	NODISCARD std::vector<uint8_t> compress(Const_Span<uint8_t> src);

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

// A polymorphic decompressor.
// It stores its own buffer, and therefore requires specifying maximum
// uncompressed data size on construction.
// This is for performance reasons, some compressors (LZ4) benefit from
// placing the dictionary at the start of the buffer, so for repeated decompression
// it is useful to give the decompressor more control.
// Because of that the decompressors are NOT thread safe.
struct Decompress_Helper
{
	// Decompresses the source block into internal memory.
	// If the decompressed size differes from the expected_size a std::runtime_error is thrown.
	// Returns the view of the decompressed data that points to an internal buffer.
	// The buffer is likely to be overwritten during the next decompress call, so be
	// careful with dangling references.
	NODISCARD virtual Const_Span<uint8_t> decompress(Const_Span<uint8_t> src, size_t expected_size) const = 0;

	virtual ~Decompress_Helper() {};
};

// A decompressor utilizing LZ4.
struct LZ4_Decompress_Helper : public Decompress_Helper
{
	LZ4_Decompress_Helper(const LZ4_Dict& dict, size_t output_size);

	NODISCARD Const_Span<uint8_t> decompress(Const_Span<uint8_t> src, size_t expected_size) const override
	{
		int ret;
		if (m_dict_size > 0)
		{
			ret = LZ4_decompress_safe_usingDict(
				reinterpret_cast<const char*>(src.data()),
				reinterpret_cast<char*>(m_output_buffer.get()) + m_dict_size,
				narrowing_static_cast<int>(src.size()),
				narrowing_static_cast<int>(m_max_output_size),
				reinterpret_cast<const char*>(m_output_buffer.get()),
				narrowing_static_cast<int>(m_dict_size)
			);
		}
		else
		{
			ret = LZ4_decompress_safe(
				reinterpret_cast<const char*>(src.data()),
				reinterpret_cast<char*>(m_output_buffer.get()),
				narrowing_static_cast<int>(src.size()),
				narrowing_static_cast<int>(m_max_output_size)
			);
		}

		if (ret <= 0 || static_cast<size_t>(ret) != expected_size)
			throw std::runtime_error("LZ4 error when trying to decompress a block.");

		return Const_Span(m_output_buffer.get() + m_dict_size, static_cast<size_t>(ret));
	}

private:
	std::unique_ptr<uint8_t[]> m_output_buffer;
	size_t m_dict_size;
	size_t m_max_output_size;
};

// A decompressor utilizing LZMA.
struct LZMA_Decompress_Helper : public Decompress_Helper
{
	LZMA_Decompress_Helper(size_t output_size);

	NODISCARD Const_Span<uint8_t> decompress(Const_Span<uint8_t> src, size_t expected_size) const override
	{
		if (src.size() < LZMA_PROPS_SIZE)
			throw std::runtime_error("Input too small");

		size_t out_sz = expected_size;
		size_t in_sz = src.size() - LZMA_PROPS_SIZE;
		const uint8_t* props = src.data() + in_sz;

		const int ret = LzmaUncompress(
			m_output_buffer.get(),
			&out_sz,
			src.data(),
			&in_sz,
			props,
			LZMA_PROPS_SIZE
		);

		if (ret != SZ_OK || out_sz != expected_size)
			throw std::runtime_error("LZMA error when trying to decompress a block.");

		return Const_Span(m_output_buffer.get(), out_sz);
	}

private:
	std::unique_ptr<uint8_t[]> m_output_buffer;
	size_t m_max_output_size;
};

// `get(block_id, scratch)` returns one block as a Const_Span. Sources that
// materialize blocks on demand (e.g. a sliced EGTB faulted in from paging
// files) write into `scratch` and return a view of it; zero-copy sources can
// return a stable view directly.
//
// `total_size` is in probe-visible (output) bytes; the helper translates to
// source-side via `Compress_Helper::source_bytes_per_block`. Block ids run
// [0, ceil_div(total_size, block_size)).
//
// `get` is called concurrently across workers with different ids — it must
// be thread-safe (typically: lock around any shared paging state it touches).
struct Block_Source
{
	size_t total_size;
	std::function<Const_Span<uint8_t>(size_t block_id, Span<uint8_t> scratch)> get;
};

// max_workers caps the number of pool workers that actually pick up blocks
// (the others early-return). 0 = use all pool workers. Used by the paged
// save path: under a memory budget, the underlying source can only afford
// so many groups resident concurrently; capping workers caps that pressure.
// silent=true skips the per-block progress bar (caller does its own reporting).
NODISCARD std::vector<std::vector<uint8_t>> compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor,
	std::string task_name,
	size_t max_workers = 0,
	bool silent = false
);
