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
	uint64_t m_total_size = 0;  // also the append position of the next block
	Temporary_File_Tracker m_tmp_files;
	std::filesystem::path m_path;
	mutable Positional_Output_File m_out;
	std::atomic<size_t> m_writes_in_flight{0};
	mutable std::atomic<bool> m_finalized{false};
	mutable Memory_Mapped_File m_map;
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

// Point decompression of a dictionary-primed LZ4 block: a block is held as a
// strided checkpoint index rather than decompressed bytes, and a read enters at
// the nearest checkpoint, then chases match references back to a literal or into
// the dictionary. Nothing is materialized.

inline constexpr size_t LZ4_MIN_MATCH = 4;  // lz4.c's MINMATCH, not exported
inline constexpr size_t LZ4_POINT_STRIDE_LOG2 = 8;
inline constexpr size_t LZ4_POINT_STRIDE = size_t{ 1 } << LZ4_POINT_STRIDE_LOG2;

struct LZ4_Point_Checkpoint
{
	uint32_t in_off;   // the sequence's token, within the block
	uint32_t out_off;  // where its literals land in the output
};

NODISCARD INLINE size_t lz4_point_index_bytes(size_t out_size)
{
	return ((out_size + LZ4_POINT_STRIDE - 1) >> LZ4_POINT_STRIDE_LOG2)
	     * sizeof(LZ4_Point_Checkpoint);
}

// Parse-only pass: moves no data, drops a checkpoint every LZ4_POINT_STRIDE
// output bytes. `dst` must be lz4_point_index_bytes(out_size) long.
INLINE void lz4_build_point_index(Const_Span<uint8_t> src, Span<uint8_t> dst)
{
	LZ4_Point_Checkpoint* const cp =
		reinterpret_cast<LZ4_Point_Checkpoint*>(dst.data());
	const size_t num_cp = dst.size() / sizeof(LZ4_Point_Checkpoint);

	const uint8_t* const ibegin = src.data();
	const uint8_t* const iend = ibegin + src.size();
	const uint8_t* ip = ibegin;
	uint32_t op = 0;
	size_t next_cp = 0;

	while (ip < iend)
	{
		const LZ4_Point_Checkpoint here{ static_cast<uint32_t>(ip - ibegin), op };

		const unsigned token = *ip++;
		size_t lit_len = token >> 4;
		if (lit_len == 15) { unsigned s; do { s = *ip++; lit_len += s; } while (s == 255); }
		ip += lit_len;
		op += static_cast<uint32_t>(lit_len);

		if (ip < iend)  // a final sequence is literals only
		{
			ip += 2;  // match offset
			size_t match_len = token & 15;
			if (match_len == 15) { unsigned s; do { s = *ip++; match_len += s; } while (s == 255); }
			op += static_cast<uint32_t>(match_len + LZ4_MIN_MATCH);
		}

		while (next_cp < num_cp && (next_cp << LZ4_POINT_STRIDE_LOG2) < op)
			cp[next_cp++] = here;
	}
}

// Reads output byte `pos` through the index built above.
NODISCARD INLINE uint8_t lz4_point_read(
	Const_Span<uint8_t> src,
	Const_Span<uint8_t> dict,
	const uint8_t* index,
	size_t pos
)
{
	const LZ4_Point_Checkpoint* const cp =
		reinterpret_cast<const LZ4_Point_Checkpoint*>(index);
	const uint8_t* const ibegin = src.data();
	int64_t target = static_cast<int64_t>(pos);

	for (;;)
	{
		if (target < 0)  // chased back past the block, into the dict
			return dict[static_cast<size_t>(static_cast<int64_t>(dict.size()) + target)];

		const LZ4_Point_Checkpoint start = cp[target >> LZ4_POINT_STRIDE_LOG2];
		const uint8_t* ip = ibegin + start.in_off;
		int64_t op = start.out_off;

		for (;;)
		{
			const unsigned token = *ip++;
			int64_t lit_len = token >> 4;
			if (lit_len == 15) { unsigned s; do { s = *ip++; lit_len += s; } while (s == 255); }
			const uint8_t* const literals = ip;
			ip += lit_len;

			if (target < op + lit_len)
				return literals[target - op];
			op += lit_len;

			const int64_t match_off = ip[0] | (ip[1] << 8);
			ip += 2;
			int64_t match_len = token & 15;
			if (match_len == 15) { unsigned s; do { s = *ip++; match_len += s; } while (s == 255); }
			match_len += static_cast<int64_t>(LZ4_MIN_MATCH);

			if (target < op + match_len)
			{
				// Fold every period the match repeats in one step, so a long
				// run at a short offset costs one hop, not one per period.
				// Offsets are two bytes and positions 32 bits, so is the divide.
				const uint32_t into = static_cast<uint32_t>(target - op);
				const uint32_t period = static_cast<uint32_t>(match_off);
				target -= (into < period)
					? match_off
					: (static_cast<int64_t>(into / period) + 1) * match_off;
				break;
			}
			op += match_len;
		}
	}
}

INLINE void lzma_decompress_into(Span<uint8_t> dst, Const_Span<uint8_t> src)
{
	if (src.size() < LZMA_PROPS_SIZE)
		throw std::runtime_error("Input too small");

	size_t out_sz = dst.size();
	size_t in_sz = src.size() - LZMA_PROPS_SIZE;
	const uint8_t* props = src.data() + in_sz;

	const int ret = LzmaUncompress(
		dst.data(),
		&out_sz,
		src.data(),
		&in_sz,
		props,
		LZMA_PROPS_SIZE
	);

	if (ret != SZ_OK || out_sz != dst.size())
		throw std::runtime_error("LZMA error when trying to decompress a block.");
}

// Scratch for one decode, from a buffer this thread keeps between decodes and
// frees when it exits. Overwritten by the next call on this thread.
NODISCARD Span<uint8_t> decompress_scratch(size_t bytes);

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

NODISCARD Compressed_Block_Store compress_blocks(
	In_Out_Param<Thread_Pool> thread_pool,
	const Block_Source& src,
	size_t block_size,
	std::unique_ptr<Compress_Helper> compressor,
	std::string task_name,
	std::filesystem::path spill_path,
	size_t max_workers = 0
);
