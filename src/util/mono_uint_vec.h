#pragma once

// Succinct integer vectors for EGTB offset tables.
//
//   Mono_Uint_Vec  - block-sampled delta coder for a monotonically
//                    non-decreasing uint64 sequence (the cumulative block
//                    offsets). Stores one full "sample" base value per
//                    2^log2_bu values, and each value as a fixed-width delta
//                    from its block's base. get2(i) returns {O[i], O[i+1]}, so
//                    a block's byte offset and compressed size both come from
//                    one lookup (comp_size[i] = O[i+1] - O[i]).
//   Min0_Uint_Vec  - minimal-bit-width packed array for arbitrary uints
//                    (DTM50's per-block usz, which is not monotonic).
//
// Bit I/O is unaligned and reads up to two 8-byte words, so every packed region
// MUST be followed by >= 8 bytes of slack (the on-disk layout aligns
// each region to 8 and is always followed by more data or the file checksum;
// the in-memory builders pad explicitly).

#include "util/defines.h"
#include "util/math.h"
#include "util/span.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

// Bits needed to represent `v` (0 -> 0, so an all-zero stream costs 0 bits).
NODISCARD INLINE constexpr uint8_t mono_bit_width(uint64_t v)
{
	return v == 0 ? uint8_t{ 0 }
	              : static_cast<uint8_t>(64 - __builtin_clzll(v));
}

NODISCARD INLINE constexpr uint64_t mono_low_mask(unsigned width)
{
	return width >= 64 ? ~uint64_t{ 0 } : ((uint64_t{ 1 } << width) - 1);
}

// Read `width` (<=64) bits starting at bit `bitpos`. Requires >= 8 readable
// bytes past the last touched byte (see header note).
NODISCARD INLINE uint64_t mono_read_bits(const uint8_t* base, size_t bitpos, unsigned width)
{
	if (width == 0) return 0;
	const size_t byte = bitpos >> 3;
	const unsigned bit = static_cast<unsigned>(bitpos & 7);
	uint64_t lo;
	std::memcpy(&lo, base + byte, 8);
	uint64_t v = lo >> bit;
	if (bit + width > 64)
	{
		uint64_t hi;
		std::memcpy(&hi, base + byte + 8, 8);
		v |= hi << (64 - bit);
	}
	return v & mono_low_mask(width);
}

// OR `val` (< 2^width) into a zero-initialized buffer at bit `bitpos`. Writes
// in ascending, non-overlapping order; buffer needs 8 bytes slack past the end.
INLINE void mono_write_bits(uint8_t* base, size_t bitpos, uint64_t val, unsigned width)
{
	if (width == 0) return;
	const size_t byte = bitpos >> 3;
	const unsigned bit = static_cast<unsigned>(bitpos & 7);
	uint64_t lo;
	std::memcpy(&lo, base + byte, 8);
	lo |= val << bit;
	std::memcpy(base + byte, &lo, 8);
	if (bit + width > 64)
	{
		uint64_t hi;
		std::memcpy(&hi, base + byte + 8, 8);
		hi |= val >> (64 - bit);
		std::memcpy(base + byte + 8, &hi, 8);
	}
}

// ---------------------------------------------------------------------------
// Mono_Uint_Vec: monotonic delta-coded sequence.
// On-disk/in-memory blob layout: [sample_bits | pad to 8][delta_bits].
// The 3 width/param bytes live in the caller's section header, not the blob.
// ---------------------------------------------------------------------------
struct Mono_Uint_Vec
{
	static constexpr uint8_t DEFAULT_LOG2_BU = 6;  // 64 values per sample

	const uint8_t* m_base = nullptr;  // start of sample region
	const uint8_t* m_delta = nullptr; // start of delta region
	size_t   m_num_values = 0;
	uint8_t  m_log2_bu = 0;
	uint8_t  m_sample_width = 0;
	uint8_t  m_offset_width = 0;

	Mono_Uint_Vec() = default;

	// num_values is the count of stored offsets (== block_cnt + 1).
	Mono_Uint_Vec(const uint8_t* blob, size_t num_values,
	              uint8_t log2_bu, uint8_t sample_width, uint8_t offset_width)
		: m_base(blob), m_num_values(num_values), m_log2_bu(log2_bu),
		  m_sample_width(sample_width), m_offset_width(offset_width)
	{
		const size_t num_samples = ceil_div(num_values, size_t{ 1 } << log2_bu);
		const size_t sample_bytes = ceil_div(num_samples * sample_width, size_t{ 8 });
		m_delta = blob + ceil_to_multiple(sample_bytes, size_t{ 8 });
	}

	NODISCARD INLINE uint64_t get(size_t i) const
	{
		const size_t sb = i >> m_log2_bu;
		const uint64_t base = mono_read_bits(m_base, sb * m_sample_width, m_sample_width);
		const uint64_t delta = mono_read_bits(m_delta, i * m_offset_width, m_offset_width);
		return base + delta;
	}

	// {O[i], O[i+1]}; valid for i in [0, num_values-2] (i.e. any block id).
	NODISCARD INLINE std::array<uint64_t, 2> get2(size_t i) const
	{
		const size_t sb0 = i >> m_log2_bu;
		const size_t sb1 = (i + 1) >> m_log2_bu;
		const uint64_t d0 = mono_read_bits(m_delta, i * m_offset_width, m_offset_width);
		const uint64_t d1 = mono_read_bits(m_delta, (i + 1) * m_offset_width, m_offset_width);
		const uint64_t b0 = mono_read_bits(m_base, sb0 * m_sample_width, m_sample_width);
		const uint64_t b1 = (sb1 == sb0) ? b0
			: mono_read_bits(m_base, sb1 * m_sample_width, m_sample_width);
		return { b0 + d0, b1 + d1 };
	}

	// --- builder ---
	struct Encoded
	{
		std::vector<uint8_t> blob;  // [sample | pad8][delta], + 8 bytes slack
		uint8_t log2_bu = 0;
		uint8_t sample_width = 0;
		uint8_t offset_width = 0;
		// On-disk bytes of the blob proper (excludes the trailing read-slack).
		size_t  on_disk_bytes = 0;
	};

	// Encode a monotonically non-decreasing sequence.
	static Encoded encode(Const_Span<uint64_t> values, uint8_t log2_bu = DEFAULT_LOG2_BU)
	{
		const size_t n = values.size();
		ASSERT(n > 0);
		const size_t bu = size_t{ 1 } << log2_bu;
		const size_t num_samples = ceil_div(n, bu);

		const uint8_t sample_width = mono_bit_width(values[n - 1]);

		uint64_t max_delta = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const uint64_t base = values[(i >> log2_bu) << log2_bu];
			const uint64_t d = values[i] - base;
			if (d > max_delta) max_delta = d;
		}
		const uint8_t offset_width = mono_bit_width(max_delta);

		const size_t sample_bytes = ceil_div(num_samples * sample_width, size_t{ 8 });
		const size_t sample_aligned = ceil_to_multiple(sample_bytes, size_t{ 8 });
		const size_t delta_bytes = ceil_div(n * offset_width, size_t{ 8 });
		const size_t on_disk = sample_aligned + delta_bytes;

		Encoded out;
		out.log2_bu = log2_bu;
		out.sample_width = sample_width;
		out.offset_width = offset_width;
		out.on_disk_bytes = on_disk;
		out.blob.assign(on_disk + 8, 0);  // +8 read slack

		uint8_t* sample = out.blob.data();
		uint8_t* delta = out.blob.data() + sample_aligned;
		for (size_t s = 0; s < num_samples; ++s)
			mono_write_bits(sample, s * sample_width, values[s << log2_bu], sample_width);
		for (size_t i = 0; i < n; ++i)
		{
			const uint64_t base = values[(i >> log2_bu) << log2_bu];
			mono_write_bits(delta, i * offset_width, values[i] - base, offset_width);
		}
		return out;
	}

	// Bytes a decoder must skip for the blob, given the params and value count.
	static size_t on_disk_bytes(size_t num_values, uint8_t log2_bu,
	                            uint8_t sample_width, uint8_t offset_width)
	{
		const size_t num_samples = ceil_div(num_values, size_t{ 1 } << log2_bu);
		const size_t sample_aligned =
			ceil_to_multiple(ceil_div(num_samples * sample_width, size_t{ 8 }), size_t{ 8 });
		return sample_aligned + ceil_div(num_values * offset_width, size_t{ 8 });
	}
};

// ---------------------------------------------------------------------------
// Min0_Uint_Vec: minimal-bit-width packed array (non-monotonic values).
// ---------------------------------------------------------------------------
struct Min0_Uint_Vec
{
	const uint8_t* m_data = nullptr;
	size_t  m_size = 0;
	uint8_t m_width = 0;

	Min0_Uint_Vec() = default;
	Min0_Uint_Vec(const uint8_t* data, size_t size, uint8_t width)
		: m_data(data), m_size(size), m_width(width) {}

	NODISCARD INLINE uint64_t get(size_t i) const
	{
		return mono_read_bits(m_data, i * m_width, m_width);
	}

	struct Encoded
	{
		std::vector<uint8_t> blob;  // packed values + 8 bytes slack
		uint8_t width = 0;
		size_t  on_disk_bytes = 0;
	};

	static Encoded encode(Const_Span<uint64_t> values)
	{
		uint64_t max_val = 0;
		for (size_t i = 0; i < values.size(); ++i)
			if (values[i] > max_val) max_val = values[i];
		const uint8_t width = mono_bit_width(max_val);
		const size_t bytes = ceil_div(values.size() * width, size_t{ 8 });

		Encoded out;
		out.width = width;
		out.on_disk_bytes = bytes;
		out.blob.assign(bytes + 8, 0);
		for (size_t i = 0; i < values.size(); ++i)
			mono_write_bits(out.blob.data(), i * width, values[i], width);
		return out;
	}

	static size_t on_disk_bytes(size_t size, uint8_t width)
	{
		return ceil_div(size * width, size_t{ 8 });
	}
};

// Per-color offset section layout used by all EGTB formats:
//   [u8 log2_bu][u8 sample_width][u8 offset_width][u8 usz_width]
//   align(8); mono blob   (mono_bytes)
//   align(8); min0 blob   (min0_bytes; 0 for non-DTM50)
//   align(8)              -- so next color or align(64) starts 8-aligned
NODISCARD INLINE constexpr size_t mono_section_bytes(size_t mono_bytes, size_t min0_bytes = 0)
{
	size_t s = 4;
	s = ceil_to_multiple(s, size_t{ 8 });
	s += mono_bytes;
	if (min0_bytes)
	{
		s = ceil_to_multiple(s, size_t{ 8 });
		s += min0_bytes;
	}
	s = ceil_to_multiple(s, size_t{ 8 });
	return s;
}
