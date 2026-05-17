#include "probe/table_files.h"

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

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
// into O(STRIDE). n_single/n_double double as the hint-bit index at each stride
// boundary, so the n_*_short popcounts cached alongside them also bound the
// SINGLE/DOUBLE short/long byte-offset scan. 256 keeps the table at 24B/stride.
constexpr size_t DTM50_PREFIX_STRIDE = 256;

struct DTM50_Prefix_Entry
{
	uint32_t n_const;
	uint32_t n_single;
	uint32_t n_double;
	uint32_t n_multi;
	uint32_t n_single_short;  // popcount(single_hints, [0, n_single))
	uint32_t n_double_short;  // popcount(double_hints, [0, n_double))
};
static_assert(sizeof(DTM50_Prefix_Entry) == 24);

struct DTM50_Cached_Block
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
static_assert(sizeof(DTM50_Cached_Block) % 4 == 0);

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

void build_dtm50_prefix_index(
	const uint8_t* state_bits, const uint8_t* single_hints, const uint8_t* double_hints,
	size_t num_positions, DTM50_Prefix_Entry* out, size_t n_strides)
{
	uint32_t nc = 0, ns = 0, nd = 0, nm = 0;
	uint32_t ns_short = 0, nd_short = 0;  // popcount(*_hints, [0, ns)/[0, nd))
	for (size_t s = 0; s < n_strides; ++s)
	{
		out[s] = { nc, ns, nd, nm, ns_short, nd_short };
		const size_t p_lo = s * DTM50_PREFIX_STRIDE;
		const size_t p_hi = std::min(p_lo + DTM50_PREFIX_STRIDE, num_positions);
		const uint32_t ns_before = ns;
		const uint32_t nd_before = nd;
		const uint32_t nm_before = nm;
		popcount_state_range(state_bits, p_lo * 2, p_hi * 2, ns, nd, nm);
		const uint32_t added_nonc = (ns - ns_before) + (nd - nd_before) + (nm - nm_before);
		nc += static_cast<uint32_t>(p_hi - p_lo) - added_nonc;
		// Extend over just the hints this stride added; the windows partition the
		// bitmaps, so the whole build stays O(num_single + num_double).
		ns_short += static_cast<uint32_t>(popcount_bit_range(single_hints, ns_before, ns));
		nd_short += static_cast<uint32_t>(popcount_bit_range(double_hints, nd_before, nd));
	}
}

// NONCONST sums 01/10/11; CONST is derived as span - NONCONST since masked-out
// groups read as 00.
enum class State_Class : uint8_t { SINGLE, DOUBLE, MULTI, NONCONST };

template <State_Class CLS>
NODISCARD INLINE uint32_t tally_state_word(uint64_t w)
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
NODISCARD INLINE uint32_t popcount_state_class(const uint8_t* sb, size_t bit_lo, size_t bit_hi)
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
	const uint8_t* state_bits, const DTM50_Prefix_Entry* prefix, size_t pos)
{
	const size_t bit_pos = pos * 2;
	const uint8_t state_at_pos = (state_bits[bit_pos / 8] >> (bit_pos % 8)) & 3u;

	const size_t stride_id = pos / DTM50_PREFIX_STRIDE;
	const DTM50_Prefix_Entry snap = prefix[stride_id];
	const size_t bit_lo = stride_id * DTM50_PREFIX_STRIDE * 2;

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
				static_cast<uint32_t>(pos - stride_id * DTM50_PREFIX_STRIDE);
			const uint32_t added_nonc =
				popcount_state_class<State_Class::NONCONST>(state_bits, bit_lo, bit_pos);
			index = snap.n_const + walk_positions - added_nonc;
			break;
		}
	}
	return State_Prefix{ index, state_at_pos };
}

// Cached buffer: [DTM50_Cached_Block][payload][DTM50_Prefix_Entry prefix...].
// Offsets are cached so read() does not reparse the payload layout every probe.
// Skipped blocks are short-circuited in read() before reaching here.
Block_Ptr dtm50_get_block(DTM50_Rank_Per_Color& pc, size_t block_id)
{
	const auto pair = pc.offsets.get2(block_id);
	const size_t doff = pair[0];
	const size_t dsz  = pair[1] - pair[0];
	const size_t usz  = pc.usizes.get(block_id);
	ASSERT(dsz != 0);  // read() short-circuits the skip sentinel before us
	ASSERT((usz & 3) == 0);

	LZMA_Decompress_Helper& dc = pc.decomp_for([&] {
		const size_t ppb = pc.block_positions;
		const size_t eb = pc.entry_bytes;
		const size_t max_payload =
			24                                                         // header
			+ (ppb * 2 + 7) / 8                                        // state_bits
			+ ppb * eb                                                 // all CONST
			+ (ppb + 7) / 8 + ppb * (1 + 2 * eb)                       // all SINGLE
			+ (ppb + 7) / 8 + ppb * (2 + 3 * eb)                       // all DOUBLE
			+ (ppb + 1) * 4 + ppb * (1 + 16 + DTM50_PACK_LAYERS * eb)  // all MULTI
			+ 3;                                                       // tail alignment
		return std::make_unique<LZMA_Decompress_Helper>(max_payload);
	});
	const Const_Span<uint8_t> raw = dc.decompress(
		Const_Span(pc.compressed_data + doff, dsz), usz);

	const uint8_t* payload = raw.data();
	uint32_t num_positions, num_single, num_double, num_multi;
	uint32_t single_stream_bytes, double_stream_bytes;
	std::memcpy(&num_positions,        payload,      4);
	std::memcpy(&num_single,           payload + 4,  4);
	std::memcpy(&num_double,           payload + 8,  4);
	std::memcpy(&num_multi,            payload + 12, 4);
	std::memcpy(&single_stream_bytes,  payload + 16, 4);
	std::memcpy(&double_stream_bytes,  payload + 20, 4);

	const size_t eb = pc.entry_bytes;
	const size_t num_const = num_positions - num_single - num_double - num_multi;
	const size_t sb_bytes = (num_positions * 2 + 7) / 8;
	const size_t sh_bytes = (num_single + 7) / 8;
	const size_t dh_bytes = (num_double + 7) / 8;

	DTM50_Cached_Block meta{};
	size_t p = 24;
	meta.state_bits_off = static_cast<uint32_t>(p); p += sb_bytes;
	meta.const_stream_off = static_cast<uint32_t>(p); p += num_const * eb;
	meta.single_hints_off = static_cast<uint32_t>(p); p += sh_bytes;
	meta.single_stream_off = static_cast<uint32_t>(p); p += single_stream_bytes;
	meta.double_hints_off = static_cast<uint32_t>(p); p += dh_bytes;
	meta.double_stream_off = static_cast<uint32_t>(p); p += double_stream_bytes;
	p += (4 - (p & 3)) & 3;
	meta.multi_dir_off = static_cast<uint32_t>(p);
	p += (num_multi + 1) * 4;
	meta.multi_data_off = static_cast<uint32_t>(p);

	const size_t payload_off = sizeof(DTM50_Cached_Block);
	const size_t n_strides = (num_positions + DTM50_PREFIX_STRIDE - 1) / DTM50_PREFIX_STRIDE;
	const size_t prefix_bytes = n_strides * sizeof(DTM50_Prefix_Entry);
	const size_t prefix_off = payload_off + usz;
	meta.prefix_off = static_cast<uint32_t>(prefix_off);

	auto buf = std::make_shared<std::vector<uint8_t>>(prefix_off + prefix_bytes, 0);
	std::memcpy(buf->data(), &meta, sizeof(meta));
	std::memcpy(buf->data() + payload_off, raw.data(), usz);

	const uint8_t* state_bits   = buf->data() + payload_off + meta.state_bits_off;
	const uint8_t* single_hints = buf->data() + payload_off + meta.single_hints_off;
	const uint8_t* double_hints = buf->data() + payload_off + meta.double_hints_off;
	auto* prefix = reinterpret_cast<DTM50_Prefix_Entry*>(buf->data() + meta.prefix_off);
	build_dtm50_prefix_index(state_bits, single_hints, double_hints,
	                         num_positions, prefix, n_strides);

	return buf;
}

}  // namespace

void DTM50_Traits::on_singular(Serial_Memory_Reader& reader, Per_Color&)
{
	if (reader.read<uint8_t>() != 0)
		throw std::runtime_error("DTM50 singular value must be DRAW");
}

void DTM50_Traits::parse_header(Serial_Memory_Reader& reader, Per_Color& pc,
                                const Position_Index_Config& index_cfg,
                                const std::filesystem::path& path)
{
	const uint32_t perm = reader.read<uint32_t>();
	if (!index_permutation_config_is_valid(index_cfg, perm))
		throw std::runtime_error("Invalid DTM50 index permutation config " + path.string());
	pc.layout = make_index_storage_layout(index_cfg, perm);

	pc.entry_bytes = reader.read<uint8_t>();
	if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(uint16_t))
		throw std::runtime_error("Bad DTM50 entry_bytes " + path.string());
	pc.block_positions = reader.read<uint32_t>();
	pc.block_cnt       = reader.read<uint64_t>();
	pc.tail_positions  = reader.read<uint32_t>();
	pc.data_size       = reader.read<uint64_t>();

	const size_t num_ranks = reader.read<uint16_t>();
	pc.rank_to_value.resize(num_ranks);
	for (size_t r = 0; r < num_ranks; ++r)
		pc.rank_to_value[r] = reader.read<uint16_t>();
}

void DTM50_Traits::finalize(Serial_Memory_Reader& reader, Per_Color (&per_color)[COLOR_NB],
                            const bool (&is_singular)[COLOR_NB], const bool (&is_dropped)[COLOR_NB],
                            const Fixed_Vector<Color, 2>& table_colors, const Piece_Config&,
                            const std::filesystem::path&)
{
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
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
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(per_color[i].data_size);
	}
}

uint16_t DTM50_Traits::read(Per_Color& pc, bool is_singular, Board_Index pos,
                            WDL_Entry wdl, unsigned hmc)
{
	if (wdl == WDL_Entry::DRAW || wdl == WDL_Entry::ILLEGAL)
		return 0;
	const bool flat = (hmc == IGNORE_50MR);
	if (!flat && (wdl == WDL_Entry::CURSED_WIN || wdl == WDL_Entry::BLESSED_LOSS))
		return 0;
	if (is_singular) return 0;

	const size_t ppb = pc.block_positions;
	const size_t block_id = static_cast<size_t>(pos) / ppb;
	const size_t pos_in_block = static_cast<size_t>(pos) % ppb;

	// Skip-block (uniform DRAW): W/L would force a non-zero cell at some
	// layer, and ILLEGAL is filtered upstream by the WDL guard.
	const auto pair_skip = pc.offsets.get2(block_id);
	if (pair_skip[0] == pair_skip[1]) return 0;

	const uint8_t* buf_data = fetch_block_cached(pc, block_id, dtm50_get_block);

	const auto* meta = reinterpret_cast<const DTM50_Cached_Block*>(buf_data);
	const uint8_t* payload = buf_data + sizeof(DTM50_Cached_Block);
	const auto* prefix = reinterpret_cast<const DTM50_Prefix_Entry*>(
		buf_data + meta->prefix_off);
	const size_t eb = pc.entry_bytes;

	const uint8_t* state_bits    = payload + meta->state_bits_off;
	const uint8_t* const_stream  = payload + meta->const_stream_off;
	const uint8_t* single_hints  = payload + meta->single_hints_off;
	const uint8_t* single_stream = payload + meta->single_stream_off;
	const uint8_t* double_hints  = payload + meta->double_hints_off;
	const uint8_t* double_stream = payload + meta->double_stream_off;
	const uint32_t* multi_dir = reinterpret_cast<const uint32_t*>(
		payload + meta->multi_dir_off);
	const uint8_t* multi_data = payload + meta->multi_data_off;

	const State_Prefix sp = state_prefix_indexed(state_bits, prefix, pos_in_block);

	// short = draw-end variant.
	const size_t single_short = 1 + eb;
	const size_t single_long  = 1 + 2 * eb;
	const size_t double_short = 2 + 2 * eb;
	const size_t double_long  = 2 + 3 * eb;

	// DRAW is unpinned: the draw-end hint synthesizes stored=0 directly.
	// Unreachable at hmc=0 (h≥1); at hmc>0 the stored==0 path returns DRAW,
	// which recover_mate_at_hmc lifts back to WIN/LOSE if needed.
	const uint16_t layer = flat ? 0u : static_cast<uint16_t>(hmc + 1);
	uint16_t stored;
	switch (sp.state_at_pos)
	{
		case 0: {
			// CONST: one rank for all hmc.
			const size_t idx = sp.index;
			uint16_t rank;
			if (eb == 1) rank = const_stream[idx];
			else { uint16_t r; std::memcpy(&r, const_stream + idx * 2, 2); rank = r; }
			stored = pc.rank_to_value[rank];
			break;
		}
		case 1: {
			// SINGLE: one transition at h.
			const size_t idx = sp.index;
			const DTM50_Prefix_Entry& snap = prefix[pos_in_block / DTM50_PREFIX_STRIDE];
			const size_t n_short = snap.n_single_short
			    + popcount_bit_range(single_hints, snap.n_single, idx);
			const size_t byte_off = n_short * single_short + (idx - n_short) * single_long;
			const uint8_t* entry = single_stream + byte_off;
			const bool draw_end = (single_hints[idx >> 3] >> (idx & 7)) & 1u;
			const uint16_t h = entry[0] & 0x7Fu;
			if (layer < h)
			{
				uint16_t rank;
				if (eb == 1) rank = entry[1];
				else { uint16_t r; std::memcpy(&r, entry + 1, 2); rank = r; }
				stored = pc.rank_to_value[rank];
			}
			else if (draw_end)
			{
				stored = 0;
			}
			else
			{
				uint16_t rank;
				if (eb == 1) rank = entry[1 + eb];
				else { uint16_t r; std::memcpy(&r, entry + 1 + eb, 2); rank = r; }
				stored = pc.rank_to_value[rank];
			}
			break;
		}
		case 2: {
			// DOUBLE: transitions at h1 < h2.
			const size_t idx = sp.index;
			const DTM50_Prefix_Entry& snap = prefix[pos_in_block / DTM50_PREFIX_STRIDE];
			const size_t n_short = snap.n_double_short
			    + popcount_bit_range(double_hints, snap.n_double, idx);
			const size_t byte_off = n_short * double_short + (idx - n_short) * double_long;
			const uint8_t* entry = double_stream + byte_off;
			const bool draw_end = (double_hints[idx >> 3] >> (idx & 7)) & 1u;
			const uint16_t h1 = entry[0];
			const uint16_t h2 = entry[1] & 0x7Fu;
			size_t rsel;
			if      (layer < h1) rsel = 0;
			else if (layer < h2) rsel = 1;
			else                 rsel = 2;
			if (rsel == 2 && draw_end)
			{
				stored = 0;
			}
			else
			{
				const uint8_t* rp = entry + 2 + rsel * eb;
				uint16_t rank;
				if (eb == 1) rank = *rp;
				else { uint16_t r; std::memcpy(&r, rp, 2); rank = r; }
				stored = pc.rank_to_value[rank];
			}
			break;
		}
		default: {
			// MULTI: changepoint bitmap; rsel = popcount(bits ≤ layer) - 1.
			const size_t idx = sp.index;
			const uint8_t* entry = multi_data + multi_dir[idx];
			const uint8_t kbyte = entry[0];
			const bool draw_end = (kbyte & 0x80u) != 0;
			const size_t k = kbyte & 0x7Fu;
			uint64_t lo, hi;
			std::memcpy(&lo, entry + 1, 8);
			std::memcpy(&hi, entry + 9, 8);
			uint64_t mask_lo, mask_hi;
			if (layer < 63)
			{
				mask_lo = (uint64_t{1} << (layer + 1)) - 1;
				mask_hi = 0;
			}
			else if (layer == 63)
			{
				mask_lo = ~uint64_t{0};
				mask_hi = 0;
			}
			else
			{
				mask_lo = ~uint64_t{0};
				mask_hi = (uint64_t{1} << (layer - 63)) - 1;
			}
			const size_t rsel = static_cast<size_t>(__builtin_popcountll(lo & mask_lo))
			                  + static_cast<size_t>(__builtin_popcountll(hi & mask_hi)) - 1;
			if (rsel == k - 1 && draw_end)
			{
				stored = 0;
			}
			else
			{
				const uint8_t* rp = entry + 17 + rsel * eb;
				uint16_t rank;
				if (eb == 1) rank = *rp;
				else { uint16_t r; std::memcpy(&r, rp, 2); rank = r; }
				stored = pc.rank_to_value[rank];
			}
			break;
		}
	}

	if (flat)
		return dtm_value_from_storage(stored, wdl);
	return hmc == 0
		? dtm50_value_from_storage(stored, wdl)
		: dtm50_layered_value_from_storage(stored, wdl);
}

template struct Table_File<DTM50_Traits>;
