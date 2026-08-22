#pragma once

#include "probe/table_files.h"

#include "util/cache.h"
#include "util/compress.h"
#include "util/defines.h"

#include <array>
#include <cstdint>
#include <cstring>

// Width of a MULTI record's changepoint bitmap for a stack of `layers`: whole
// 32-bit words, so it is masked a word at a time with no per-byte tail. DTC's 30
// layers take 4 bytes, DTM50's 101 take 16.
NODISCARD constexpr size_t layered_bitmap_bytes(size_t layers)
{
	return ((layers + 31) / 32) * 4;
}

// The MULTI state's changepoint bitmap, cut to each metric's stack height in
// whole 32-bit words (see Layered_Block_Encoder).
inline constexpr size_t DTM50_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTM50_PACK_LAYERS);
inline constexpr size_t DTC_MULTI_BITMAP_BYTES = layered_bitmap_bytes(DTC_PACK_LAYERS);

// Read side of the changepoint pack (see Layered_Block_Encoder): block layout,
// the per-stride prefix index that keeps a random read off an O(block) scan, and
// the locator that resolves one position to its record. Only the MULTI bitmap
// helpers depend on the stack height, and they take its width as a template
// argument. What differs beyond that is only what a metric asks of a record,
// which is its own read().

// Popcount of bits [lo, hi) of `bm`. Reads only the spanned bytes; the
// unaligned head and partial tail fold into masked word loads, no per-byte loop.
NODISCARD INLINE size_t popcount_bit_range(const uint8_t* bm, size_t lo, size_t hi)
{
	if (lo >= hi) return 0;
	size_t pc = 0;
	size_t byte = lo >> 3;
	size_t remaining = hi - lo;
	const size_t head = lo & 7;  // bits to skip in the first byte
	if (head)
	{
		const uint8_t b = static_cast<uint8_t>(bm[byte] >> head);
		const size_t avail = 8 - head;
		if (remaining <= avail)
			return __builtin_popcount(
				static_cast<uint8_t>(b & ((1u << remaining) - 1u)));
		pc += __builtin_popcount(b);
		remaining -= avail;
		++byte;
	}
	while (remaining >= 64)
	{
		uint64_t w;
		std::memcpy(&w, bm + byte, 8);
		pc += __builtin_popcountll(w);
		byte += 8;
		remaining -= 64;
	}
	if (remaining)  // 1..63 bits, byte-aligned
	{
		uint64_t w = 0;
		std::memcpy(&w, bm + byte, (remaining + 7) / 8);
		w &= (uint64_t{1} << remaining) - 1;
		pc += __builtin_popcountll(w);
	}
	return pc;
}

// 2-bit state vector:
//   00 CONST   01 SINGLE   10 DOUBLE   11 MULTI
// Position p occupies bits 2p, 2p+1 within its byte.
// read() only uses the count for the class at `pos`, so we resolve the class
// first and tally just that one rather than all four.
struct State_Prefix
{
	uint32_t index;        // count of same-class positions before `pos`
	uint8_t  state_at_pos; // 0 CONST, 1 SINGLE, 2 DOUBLE, 3 MULTI
};

// Per-stride snapshots that turn the per-read scans from O(block_positions)
// into O(STRIDE). SINGLE/DOUBLE widths vary with a hint bit the loader builds
// from the record MSBs, so a snapshot carries that bitmap's prefix count too.
// 256 keeps the table at 24B/stride.
constexpr size_t LAYERED_PREFIX_STRIDE = 256;

struct Layered_Prefix_Entry
{
	uint32_t n_const;
	uint32_t n_single;
	uint32_t n_double;
	uint32_t n_multi;
	uint32_t n_single_short;  // popcount(single_hints, [0, n_single))
	uint32_t n_double_short;  // popcount(double_hints, [0, n_double))
};
static_assert(sizeof(Layered_Prefix_Entry) == 24);

struct Layered_Cached_Block
{
	uint32_t state_bits_off;
	uint32_t const_stream_off;
	uint32_t single_hints_off;
	uint32_t single_stream_off;
	uint32_t double_hints_off;
	uint32_t double_stream_off;
	uint32_t multi_dir_off;
	uint32_t multi_data_off;
	uint32_t prefix_off;
};
static_assert(sizeof(Layered_Cached_Block) % 4 == 0);

// Tally SINGLE/DOUBLE/MULTI in [bit_lo, bit_hi); CONST = span - sum.
INLINE void popcount_state_range(
	const uint8_t* sb, size_t bit_lo, size_t bit_hi,
	uint32_t& ns, uint32_t& nd, uint32_t& nm)
{
	constexpr uint64_t EVEN_MASK = 0x5555555555555555ull;
	size_t bit = bit_lo;
	while (bit + 64 <= bit_hi)
	{
		uint64_t w;
		std::memcpy(&w, sb + bit / 8, 8);
		const uint64_t lo = w & EVEN_MASK;
		const uint64_t hi = (w >> 1) & EVEN_MASK;
		ns += __builtin_popcountll(lo & ~hi);  // 01
		nd += __builtin_popcountll(hi & ~lo);  // 10
		nm += __builtin_popcountll(lo & hi);   // 11
		bit += 64;
	}
	if (bit < bit_hi)
	{
		const size_t rem = bit_hi - bit;
		uint64_t w = 0;
		std::memcpy(&w, sb + bit / 8, (rem + 7) / 8);
		const uint64_t mask = (rem == 64) ? ~uint64_t{0} : ((uint64_t{1} << rem) - 1);
		w &= mask;
		const uint64_t lo = w & EVEN_MASK;
		const uint64_t hi = (w >> 1) & EVEN_MASK;
		ns += __builtin_popcountll(lo & ~hi);
		nd += __builtin_popcountll(hi & ~lo);
		nm += __builtin_popcountll(lo & hi);
	}
}

// Record widths, by state and draw-end bit.
NODISCARD INLINE size_t single_width(const uint8_t* e, size_t eb)
{
	return (e[0] & 0x80u) ? 1 + eb : 1 + 2 * eb;
}

NODISCARD INLINE size_t double_width(const uint8_t* e, size_t eb)
{
	return (e[1] & 0x80u) ? 2 + 2 * eb : 2 + 3 * eb;
}

INLINE void build_layered_prefix_index(
	const uint8_t* state_bits, const uint8_t* single_hints, const uint8_t* double_hints,
	size_t num_positions, Layered_Prefix_Entry* out, size_t n_strides)
{
	uint32_t nc = 0, ns = 0, nd = 0, nm = 0;
	uint32_t ns_short = 0, nd_short = 0;  // popcount(*_hints, [0, ns)/[0, nd))
	for (size_t s = 0; s < n_strides; ++s)
	{
		out[s] = { nc, ns, nd, nm, ns_short, nd_short };
		const size_t p_lo = s * LAYERED_PREFIX_STRIDE;
		const size_t p_hi = std::min(p_lo + LAYERED_PREFIX_STRIDE, num_positions);
		const uint32_t ns_before = ns;
		const uint32_t nd_before = nd;
		const uint32_t nm_before = nm;
		popcount_state_range(state_bits, p_lo * 2, p_hi * 2, ns, nd, nm);
		const uint32_t added_nonc = (ns - ns_before) + (nd - nd_before) + (nm - nm_before);
		nc += static_cast<uint32_t>(p_hi - p_lo) - added_nonc;
		// Only the hints this stride added.
		ns_short += static_cast<uint32_t>(popcount_bit_range(single_hints, ns_before, ns));
		nd_short += static_cast<uint32_t>(popcount_bit_range(double_hints, nd_before, nd));
	}
}

// NONCONST sums 01/10/11; CONST is derived as span - NONCONST since masked-out
// groups read as 00.
enum class State_Class : uint8_t { SINGLE, DOUBLE, MULTI, NONCONST };

template <State_Class CLS>
NODISCARD uint32_t tally_state_word(uint64_t w)
{
	constexpr uint64_t EVEN_MASK = 0x5555555555555555ull;
	const uint64_t lo = w & EVEN_MASK;
	const uint64_t hi = (w >> 1) & EVEN_MASK;
	if constexpr (CLS == State_Class::SINGLE)      return __builtin_popcountll(lo & ~hi); // 01
	else if constexpr (CLS == State_Class::DOUBLE) return __builtin_popcountll(hi & ~lo); // 10
	else if constexpr (CLS == State_Class::MULTI)  return __builtin_popcountll(lo & hi);  // 11
	else                                           return __builtin_popcountll(lo | hi);  // !=00
}

template <State_Class CLS>
NODISCARD uint32_t popcount_state_class(const uint8_t* sb, size_t bit_lo, size_t bit_hi)
{
	uint32_t count = 0;
	size_t bit = bit_lo;
	while (bit + 64 <= bit_hi)
	{
		uint64_t w;
		std::memcpy(&w, sb + bit / 8, 8);
		count += tally_state_word<CLS>(w);
		bit += 64;
	}
	if (bit < bit_hi)
	{
		const size_t rem = bit_hi - bit;
		uint64_t w = 0;
		std::memcpy(&w, sb + bit / 8, (rem + 7) / 8);
		const uint64_t mask = (rem == 64) ? ~uint64_t{0} : ((uint64_t{1} << rem) - 1);
		w &= mask;
		count += tally_state_word<CLS>(w);
	}
	return count;
}

NODISCARD INLINE State_Prefix state_prefix_indexed(
	const uint8_t* state_bits, const Layered_Prefix_Entry* prefix, size_t pos)
{
	const size_t bit_pos = pos * 2;
	const uint8_t state_at_pos = (state_bits[bit_pos / 8] >> (bit_pos % 8)) & 3u;

	const size_t stride_id = pos / LAYERED_PREFIX_STRIDE;
	const Layered_Prefix_Entry snap = prefix[stride_id];
	const size_t bit_lo = stride_id * LAYERED_PREFIX_STRIDE * 2;

	uint32_t index;
	switch (state_at_pos)
	{
		case 1:
			index = snap.n_single
			      + popcount_state_class<State_Class::SINGLE>(state_bits, bit_lo, bit_pos);
			break;
		case 2:
			index = snap.n_double
			      + popcount_state_class<State_Class::DOUBLE>(state_bits, bit_lo, bit_pos);
			break;
		case 3:
			index = snap.n_multi
			      + popcount_state_class<State_Class::MULTI>(state_bits, bit_lo, bit_pos);
			break;
		default: {
			// CONST: walked positions minus non-CONST in range.
			const uint32_t walk_positions =
				static_cast<uint32_t>(pos - stride_id * LAYERED_PREFIX_STRIDE);
			const uint32_t added_nonc =
				popcount_state_class<State_Class::NONCONST>(state_bits, bit_lo, bit_pos);
			index = snap.n_const + walk_positions - added_nonc;
			break;
		}
	}
	return State_Prefix{ index, state_at_pos };
}

// Cached buffer: [Layered_Cached_Block][payload][prefix][single_hints][double_hints].
// Offsets are cached so read() does not reparse the payload layout every probe,
// and the hint bitmaps are built here.
// Skipped blocks are short-circuited in read() before reaching here.
template <typename Per_Color>
Block_Ptr layered_get_block(Per_Color& pc, size_t block_id)
{
	const auto pair = pc.offsets.get2(block_id);
	const size_t doff = pair[0];
	const size_t dsz  = pair[1] - pair[0];
	const size_t usz  = pc.usizes.get(block_id);
	ASSERT(dsz != 0);  // read() short-circuits the skip sentinel before us
	ASSERT((usz & 3) == 0);

	// Both sizes that place the payload are known before it is seen, so LZMA
	// decodes straight into the returned block.
	const size_t num_positions =
		(block_id == pc.block_cnt - 1 && pc.tail_positions != 0)
		? pc.tail_positions
		: pc.block_positions;

	const size_t payload_off = sizeof(Layered_Cached_Block);
	const size_t n_strides = (num_positions + LAYERED_PREFIX_STRIDE - 1) / LAYERED_PREFIX_STRIDE;
	const size_t prefix_bytes = n_strides * sizeof(Layered_Prefix_Entry);
	const size_t prefix_off = payload_off + usz;
	const size_t hints_off = prefix_off + prefix_bytes;

	auto buf = std::make_shared<std::vector<uint8_t>>(hints_off, 0);
	lzma_decompress_into(
		Span<uint8_t>(buf->data() + payload_off, usz),
		Const_Span<uint8_t>(pc.compressed_data + doff, dsz));

	const uint8_t* payload = buf->data() + payload_off;  // resize() below moves it
	uint32_t hdr_positions, num_single, num_double, num_multi;
	uint32_t single_stream_bytes, double_stream_bytes;
	std::memcpy(&hdr_positions,        payload,      4);
	std::memcpy(&num_single,           payload + 4,  4);
	std::memcpy(&num_double,           payload + 8,  4);
	std::memcpy(&num_multi,            payload + 12, 4);
	std::memcpy(&single_stream_bytes,  payload + 16, 4);
	std::memcpy(&double_stream_bytes,  payload + 20, 4);
	ASSERT(hdr_positions == num_positions);

	const size_t eb = pc.entry_bytes;
	const size_t num_const = num_positions - num_single - num_double - num_multi;
	const size_t sb_bytes = (num_positions * 2 + 7) / 8;
	const size_t sh_bytes = (num_single + 7) / 8;
	const size_t dh_bytes = (num_double + 7) / 8;

	Layered_Cached_Block meta{};
	size_t p = 24;
	meta.state_bits_off = static_cast<uint32_t>(p); p += sb_bytes;
	meta.const_stream_off = static_cast<uint32_t>(p); p += num_const * eb;
	meta.single_stream_off = static_cast<uint32_t>(p); p += single_stream_bytes;
	meta.double_stream_off = static_cast<uint32_t>(p); p += double_stream_bytes;
	p += (4 - (p & 3)) & 3;
	meta.multi_dir_off = static_cast<uint32_t>(p);
	p += (num_multi + 1) * 4;
	meta.multi_data_off = static_cast<uint32_t>(p);
	meta.prefix_off = static_cast<uint32_t>(prefix_off);
	meta.single_hints_off = static_cast<uint32_t>(hints_off - payload_off);
	meta.double_hints_off = static_cast<uint32_t>(hints_off - payload_off + sh_bytes);

	buf->resize(hints_off + sh_bytes + dh_bytes, 0);
	payload = buf->data() + payload_off;
	std::memcpy(buf->data(), &meta, sizeof(meta));

	uint8_t* single_hints = buf->data() + payload_off + meta.single_hints_off;
	uint8_t* double_hints = buf->data() + payload_off + meta.double_hints_off;
	const uint8_t* e = payload + meta.single_stream_off;
	for (uint32_t j = 0; j < num_single; ++j)
	{
		if (e[0] & 0x80u) single_hints[j >> 3] |= static_cast<uint8_t>(1u << (j & 7));
		e += single_width(e, eb);
	}
	e = payload + meta.double_stream_off;
	for (uint32_t j = 0; j < num_double; ++j)
	{
		if (e[1] & 0x80u) double_hints[j >> 3] |= static_cast<uint8_t>(1u << (j & 7));
		e += double_width(e, eb);
	}

	auto* prefix = reinterpret_cast<Layered_Prefix_Entry*>(buf->data() + meta.prefix_off);
	build_layered_prefix_index(payload + meta.state_bits_off, single_hints, double_hints,
	                         num_positions, prefix, n_strides);

	return buf;
}

// A position's packed record. `entry` is the rank cell for CONST.
struct Layered_Entry_Ref
{
	const uint8_t* entry;
	uint8_t state;    // 0 CONST, 1 SINGLE, 2 DOUBLE, 3 MULTI
	bool draw_end;
};

// Locate `pos`'s record. False for a skip block (uniform DRAW), whose positions
// have no record: W/L would force a non-zero cell at some layer, and ILLEGAL is
// filtered upstream by the WDL guard.
template <typename Traits>
NODISCARD bool layered_locate_entry(
	typename Traits::Per_Color& pc, Board_Index pos, Layered_Entry_Ref& out)
{
	const size_t ppb = pc.block_positions;
	const size_t block_id = static_cast<size_t>(pos) / ppb;
	const size_t pos_in_block = static_cast<size_t>(pos) % ppb;

	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1]) return false;

	const uint8_t* buf_data = fetch_block_cached<Traits>(pc, block_id, layered_get_block<typename Traits::Per_Color>);

	const auto* meta = reinterpret_cast<const Layered_Cached_Block*>(buf_data);
	const uint8_t* payload = buf_data + sizeof(Layered_Cached_Block);
	const auto* prefix = reinterpret_cast<const Layered_Prefix_Entry*>(
		buf_data + meta->prefix_off);
	const size_t eb = pc.entry_bytes;

	const State_Prefix sp = state_prefix_indexed(
		payload + meta->state_bits_off, prefix, pos_in_block);
	const Layered_Prefix_Entry& snap = prefix[pos_in_block / LAYERED_PREFIX_STRIDE];
	const size_t idx = sp.index;

	out.state = sp.state_at_pos;
	switch (sp.state_at_pos)
	{
		case 0:
			out.entry = payload + meta->const_stream_off + idx * eb;
			out.draw_end = false;
			break;
		case 1: {
			// short = draw-end variant: [h, r0] against [h, r0, r1].
			const uint8_t* hints = payload + meta->single_hints_off;
			const size_t n_short = snap.n_single_short
			    + popcount_bit_range(hints, snap.n_single, idx);
			const size_t byte_off = n_short * (1 + eb) + (idx - n_short) * (1 + 2 * eb);
			out.entry = payload + meta->single_stream_off + byte_off;
			out.draw_end = ((hints[idx >> 3] >> (idx & 7)) & 1u) != 0;
			break;
		}
		case 2: {
			const uint8_t* hints = payload + meta->double_hints_off;
			const size_t n_short = snap.n_double_short
			    + popcount_bit_range(hints, snap.n_double, idx);
			const size_t byte_off = n_short * (2 + 2 * eb) + (idx - n_short) * (2 + 3 * eb);
			out.entry = payload + meta->double_stream_off + byte_off;
			out.draw_end = ((hints[idx >> 3] >> (idx & 7)) & 1u) != 0;
			break;
		}
		default: {
			const uint32_t* dir = reinterpret_cast<const uint32_t*>(
				payload + meta->multi_dir_off);
			out.entry = payload + meta->multi_data_off + dir[idx];
			out.draw_end = (out.entry[0] & 0x80u) != 0;
			break;
		}
	}
	return true;
}

template <typename Per_Color>
NODISCARD uint16_t layered_rank_value(const Per_Color& pc, const uint8_t* p)
{
	if (pc.entry_bytes == 1) return pc.rank_to_value[*p];
	uint16_t r;
	std::memcpy(&r, p, 2);
	return pc.rank_to_value[r];
}

// A MULTI record's changepoint bitmap, the BM_BYTES following the k byte, in
// words: word w bit b is layer 64 * w + b. A width under a word is zero-extended.
template <size_t BM_BYTES>
NODISCARD INLINE std::array<uint64_t, (BM_BYTES + 7) / 8>
layered_multi_bitmap(const uint8_t* entry)
{
	std::array<uint64_t, (BM_BYTES + 7) / 8> bm{};
	std::memcpy(bm.data(), entry + 1, BM_BYTES);
	return bm;
}

// Index of a MULTI record's highest set changepoint bit.
template <size_t BM_BYTES>
NODISCARD INLINE uint16_t layered_multi_last_changepoint(const uint8_t* entry)
{
	const auto bm = layered_multi_bitmap<BM_BYTES>(entry);
	for (size_t w = bm.size(); w-- > 0; )
		if (bm[w] != 0)
			return static_cast<uint16_t>(w * 64 + 63 - __builtin_clzll(bm[w]));
	return 0;
}

// Rank slot selected at `layer`: one less than the number of changepoints at or
// below it.
template <size_t BM_BYTES>
NODISCARD INLINE size_t layered_multi_rank_slot(const uint8_t* entry, uint16_t layer)
{
	const auto bm = layered_multi_bitmap<BM_BYTES>(entry);
	size_t pc = 0;
	for (size_t w = 0; w < bm.size() && layer >= w * 64; ++w)
	{
		const size_t bits = layer - w * 64 + 1;  // bits of this word at or below layer
		const uint64_t mask =
			(bits >= 64) ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
		pc += static_cast<size_t>(__builtin_popcountll(bm[w] & mask));
	}
	return pc - 1;
}
