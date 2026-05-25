#include "probe/probe.h"

#include "egtb/egtb_entry.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/egtb_gen.h"

#include "chess/chess.h"
#include "chess/move.h"
#include "chess/piece_config.h"
#include "chess/position.h"

#include "util/compress.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Enough slots to avoid thrashing during derive_dropped child probes.
constexpr size_t BLOCK_CACHE_SLOTS = 32;

// Identifies a Per_Color across heap-address reuse for the TL cache key.
inline uint64_t next_probe_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

struct TL_Block_FIFO
{
	static constexpr size_t N = 8;   // power of two
	uint64_t  epoch[N]    = {};
	size_t    block_id[N] = {};
	Block_Ptr bytes[N];
	size_t    next = 0;
};

// File-format constants shared with egtb_compress.cpp.
constexpr uint8_t SINGULAR_FLAG = 0x80;
constexpr uint8_t DROPPED_FLAG  = 0x40;
constexpr uint64_t CHECKSUM_INIT = 0xf0f0f0f0f0f0;

constexpr const char* WDL_EXT = ".lzw";
constexpr const char* DTC_EXT = ".lzdtc";
constexpr const char* DTM_EXT = ".lzdtm";
constexpr const char* DTM50_EXT = ".lzdtm50";
constexpr size_t DTM50_HMC_COUNT = 100;
// Sentinel for internal probe_impl callers that don't want DTM50 populated
// (root probers using DTZ-only ordering). Public probe() defaults rule50 to
// 0 so default callers still get the fresh-window DTM50 layer.
constexpr unsigned PROBE_IMPL_SKIP_DTM50 = ~0u;

bool find_dtm50_in_dirs(const Piece_Config& ps, uint16_t hmc,
                        const std::vector<std::filesystem::path>& dirs,
                        std::filesystem::path* out)
{
	const std::string fn = "h" + std::to_string(hmc) + DTM50_EXT;
	for (const auto& d : dirs)
	{
		const auto p = d / ps.name() / fn;
		if (std::filesystem::exists(p))
		{
			if (out) *out = p;
			return true;
		}
	}
	return false;
}

bool find_in_dirs(const Piece_Config& ps, const char* ext,
                  const std::vector<std::filesystem::path>& dirs,
                  std::filesystem::path* out)
{
	const std::string name = ps.name() + ext;
	for (const auto& d : dirs)
	{
		auto p = path_join(d, name);
		if (std::filesystem::exists(p))
		{
			if (out) *out = std::move(p);
			return true;
		}
	}
	return false;
}

// Literal key detects whether canonicalization swapped colors.
struct Config_And_Literal_Key { Piece_Config cfg; Material_Key literal_key; };

Config_And_Literal_Key piece_config_and_literal_key_from_position(const Position& pos)
{
	std::array<Piece, MAX_MAN> pieces;
	size_t n = 0;
	Material_Key literal_key;
	for (Piece pc : ALL_PIECES)
	{
		const size_t cnt = pos.piece_bb(pc).num_set_bits();
		for (size_t i = 0; i < cnt; ++i)
		{
			pieces[n++] = pc;
			literal_key.add_piece(pc);
		}
	}
	return { Piece_Config(Const_Span<Piece>(pieces.data(), n)), literal_key };
}

Position mirror_for_canonical(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
	{
		const Piece p = pos.piece_at(sq);
		if (p != PIECE_NONE)
			swapped.put_piece(piece_opp_color(p), sq_rank_mirror(sq));
	}
	swapped.set_turn(color_opp(pos.turn()));
	return swapped;
}

// Per-color block cache with on-demand decompression.
struct WDL_Probe_File
{
	struct Block_Index_Entry
	{
		uint64_t data_offset;
		uint32_t comp_size;
	};

	struct Per_Color
	{
		size_t block_size = 0;
		size_t tail_size = 0;
		size_t block_cnt = 0;
		std::vector<Block_Index_Entry> index;
		const uint8_t* compressed_data = nullptr;
		LZ4_Dict dict;

		const uint64_t epoch = next_probe_epoch();
		mutable std::mutex mu;  // guards cache state and decompressor
		std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
		std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
		size_t next_slot = 0;
		size_t live = 0;
		std::unique_ptr<LZ4_Decompress_Helper> decomp;
	};

	bool is_singular[COLOR_NB] = { false, false };
	bool is_dropped[COLOR_NB]  = { false, false };
	WDL_Entry single_val[COLOR_NB] = { WDL_Entry::DRAW, WDL_Entry::DRAW };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped;

	void load(const Piece_Config& ps, const std::filesystem::path& path);
	NODISCARD WDL_Entry read(Color c, Board_Index pos);

private:
	// Valid only while pc.mu is held.
	NODISCARD Block_Ptr get_block_locked(Per_Color& pc, size_t block_id);
};

void WDL_Probe_File::load(const Piece_Config& ps, const std::filesystem::path& path)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error("Cannot open WDL file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid WDL file size " + path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(CHECKSUM_INIT))
		throw std::runtime_error("Invalid WDL checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(EGTB_Magic::WDL_MAGIC))
		throw std::runtime_error("Invalid WDL magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in WDL " + path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	size_t block_cnt[COLOR_NB]{}, block_size[COLOR_NB]{}, tail_size[COLOR_NB]{};
	size_t dict_size[COLOR_NB]{}, offset_bits[COLOR_NB]{}, data_size[COLOR_NB]{};
	const uint8_t* lp_dict[COLOR_NB]{};
	const uint8_t* offset_tb[COLOR_NB]{};
	const uint8_t* data_ptr[COLOR_NB]{};

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			single_val[i] = static_cast<WDL_Entry>(reader.read<uint8_t>());
		}
		else if (flag & DROPPED_FLAG)
		{
			// Derived at probe time; no payload follows.
			is_dropped[i] = true;
		}
		else
		{
			offset_bits[i] = reader.read<uint8_t>();
			tail_size[i]   = reader.read<uint16_t>();
			block_size[i]  = reader.read<uint32_t>();
			block_cnt[i]   = reader.read<uint32_t>();
			data_size[i]   = reader.read<uint64_t>();
		}
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		dict_size[i] = reader.read<uint16_t>();
		if (dict_size[i] != 0)
		{
			lp_dict[i] = reader.caret();
			reader.advance(dict_size[i]);
			reader.align(2);
		}
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		offset_tb[i] = reader.caret();
		reader.advance((2 + offset_bits[i]) * block_cnt[i]);
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		data_ptr[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = Piece_Config_For_Gen(ps).num_positions();
	const size_t expected_uncompressed = ceil_div(num_positions, WDL_ENTRY_PACK_RATIO);

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;

		const size_t num_full = tail_size[i] != 0 ? block_cnt[i] - 1 : block_cnt[i];
		const size_t file_sz  = block_size[i] * num_full + tail_size[i];
		if (file_sz != expected_uncompressed)
			throw std::runtime_error("WDL decompressed size mismatch " + path.string());

		Per_Color& pc = per_color[i];
		pc.block_size = block_size[i];
		pc.tail_size  = tail_size[i];
		pc.block_cnt  = block_cnt[i];
		pc.compressed_data = data_ptr[i];
		pc.dict = LZ4_Dict::load(Const_Span(lp_dict[i], lp_dict[i] + dict_size[i]));

		pc.index.resize(block_cnt[i]);
		for (size_t idx = 0; idx < block_cnt[i]; ++idx)
		{
			Serial_Memory_Reader br(Const_Span(offset_tb[i] + (offset_bits[i] + 2) * idx, 2 + 4 + 2));
			const uint16_t comp_size = br.read<uint16_t>();
			size_t off = br.read<uint32_t>();
			if (offset_bits[i] == 6)
				off += static_cast<size_t>(br.read<uint16_t>()) << 32;
			pc.index[idx].data_offset = off;
			pc.index[idx].comp_size   = comp_size;
		}
	}
}

Block_Ptr WDL_Probe_File::get_block_locked(Per_Color& pc, size_t block_id)
{
	for (size_t i = 0; i < pc.live; ++i)
	{
		if (pc.block_id[i] == block_id)
			return pc.data[i];
	}

	if (!pc.decomp)
		pc.decomp = std::make_unique<LZ4_Decompress_Helper>(pc.dict, pc.block_size);

	const auto& ie = pc.index[block_id];
	const size_t out_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const auto decompressed = pc.decomp->decompress(
		Const_Span<uint8_t>(pc.compressed_data + ie.data_offset, ie.comp_size), out_sz);

	size_t slot;
	if (pc.live < BLOCK_CACHE_SLOTS)
	{
		slot = pc.live++;
	}
	else
	{
		slot = pc.next_slot;
		pc.next_slot = (pc.next_slot + 1) % BLOCK_CACHE_SLOTS;
	}
	pc.block_id[slot] = block_id;
	pc.data[slot] = std::make_shared<const std::vector<uint8_t>>(
		decompressed.begin(), decompressed.end());
	return pc.data[slot];
}

WDL_Entry WDL_Probe_File::read(Color c, Board_Index pos)
{
	if (is_singular[c]) return single_val[c];
	ASSERT(!is_dropped[c]);

	Per_Color& pc = per_color[c];
	const size_t packed_byte = static_cast<size_t>(pos) / WDL_ENTRY_PACK_RATIO;
	const size_t block_id    = packed_byte / pc.block_size;
	const size_t in_block    = packed_byte % pc.block_size;

	if (pc.index[block_id].comp_size == 0)
		return WDL_Entry::ILLEGAL;

	thread_local TL_Block_FIFO tl;
	const uint8_t* data = nullptr;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == pc.epoch && tl.block_id[i] == block_id)
			{ data = tl.bytes[i]->data(); break; }
	if (!data)
	{
		std::lock_guard<std::mutex> lk(pc.mu);
		Block_Ptr blk = get_block_locked(pc, block_id);
		const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
		tl.epoch[s]    = pc.epoch;
		tl.block_id[s] = block_id;
		tl.bytes[s]    = blk;
		data = blk->data();
	}
	Packed_WDL_Entries entry;
	std::memcpy(&entry, data + in_block, sizeof(entry));
	return get_wdl_value(entry, static_cast<size_t>(pos) % WDL_ENTRY_PACK_RATIO);
}

struct DTC_Probe_File
{
	struct Per_Color
	{
		size_t entry_bytes = 0;
		size_t block_size = 0;
		size_t tail_size = 0;
		size_t block_cnt = 0;
		const uint8_t* offset_tb = nullptr;
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;

		const uint64_t epoch = next_probe_epoch();
		mutable std::mutex mu;
		std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
		std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
		size_t next_slot = 0;
		size_t live = 0;
		std::unique_ptr<LZMA_Decompress_Helper> decomp;
	};

	bool is_singular[COLOR_NB] = { false, false };
	bool is_dropped[COLOR_NB]  = { false, false };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped;

	void load(const Piece_Config& ps, const std::filesystem::path& path);
	NODISCARD DTC_Final_Entry read(Color c, Board_Index pos, WDL_Entry wdl);

private:
	NODISCARD Block_Ptr get_block_locked(Per_Color& pc, size_t block_id);
};

void DTC_Probe_File::load(const Piece_Config& ps, const std::filesystem::path& path)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error("Cannot open DTC file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTC file size " + path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(CHECKSUM_INIT))
		throw std::runtime_error("Invalid DTC checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(EGTB_Magic::DTC_MAGIC))
		throw std::runtime_error("Invalid DTC magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = Material_Key(key_and_table_num >> 2);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTC " + path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	size_t data_size[COLOR_NB]{};

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			const WDL_Entry sv = static_cast<WDL_Entry>(reader.read<uint8_t>());
			if (sv != WDL_Entry::DRAW)
				throw std::runtime_error("DTC singular value must be DRAW");
		}
		else if (flag & DROPPED_FLAG)
		{
			is_dropped[i] = true;
		}
		else
		{
			Per_Color& pc = per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(DTC_Final_Entry))
				throw std::runtime_error("Bad DTC entry_bytes " + path.string());
			pc.tail_size  = reader.read<uint32_t>();
			pc.block_size = reader.read<uint32_t>();
			pc.block_cnt  = reader.read<uint32_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		per_color[i].offset_tb = reader.caret();
		reader.advance(per_color[i].block_cnt * 8);
	}
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = Piece_Config_For_Gen(ps).num_positions();
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTC decompressed size mismatch " + path.string());
	}
}

Block_Ptr DTC_Probe_File::get_block_locked(Per_Color& pc, size_t block_id)
{
	for (size_t i = 0; i < pc.live; ++i)
	{
		if (pc.block_id[i] == block_id)
			return pc.data[i];
	}

	const size_t decode_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const size_t positions = decode_sz / pc.entry_bytes;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	const size_t dsz  = dso & 0xFFFFF;
	const size_t doff = dso >> 20;

	size_t slot;
	if (pc.live < BLOCK_CACHE_SLOTS)
	{
		slot = pc.live++;
	}
	else
	{
		slot = pc.next_slot;
		pc.next_slot = (pc.next_slot + 1) % BLOCK_CACHE_SLOTS;
	}
	pc.block_id[slot] = block_id;
	auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

	if (dsz != 0)
	{
		if (!pc.decomp)
			pc.decomp = std::make_unique<LZMA_Decompress_Helper>(pc.block_size);
		const Const_Span<uint8_t> raw = pc.decomp->decompress(
			Const_Span(pc.compressed_data + doff, dsz), decode_sz);

		const auto& r2v = pc.rank_to_value;
		uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
		if (pc.entry_bytes == 1)
		{
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[raw[k]];
		}
		else
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[in[k]];
		}
	}

	pc.data[slot] = buf;
	return pc.data[slot];
}

DTC_Final_Entry DTC_Probe_File::read(Color c, Board_Index pos, WDL_Entry wdl)
{
	if (is_singular[c]) return DTC_Final_Entry::make_draw();
	ASSERT(!is_dropped[c]);

	Per_Color& pc = per_color[c];
	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	if ((dso & 0xFFFFF) == 0)
		return DTC_Final_Entry::make_illegal();

	thread_local TL_Block_FIFO tl;
	const uint8_t* data = nullptr;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == pc.epoch && tl.block_id[i] == block_id)
			{ data = tl.bytes[i]->data(); break; }
	if (!data)
	{
		std::lock_guard<std::mutex> lk(pc.mu);
		Block_Ptr blk = get_block_locked(pc, block_id);
		const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
		tl.epoch[s]    = pc.epoch;
		tl.block_id[s] = block_id;
		tl.bytes[s]    = blk;
		data = blk->data();
	}
	DTC_Final_Entry stored;
	std::memcpy(&stored, data + in_block_pos * sizeof(uint16_t), sizeof(stored));
	return dtc_entry_from_storage(stored, wdl, pc.entry_bytes);
}

// DTM uses the DTC container layout with a different magic and decoder.
struct DTM_Probe_File
{
	struct Per_Color
	{
		size_t entry_bytes = 0;
		size_t block_size = 0;
		size_t tail_size = 0;
		size_t block_cnt = 0;
		const uint8_t* offset_tb = nullptr;
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;

		const uint64_t epoch = next_probe_epoch();
		mutable std::mutex mu;
		std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
		std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
		size_t next_slot = 0;
		size_t live = 0;
		std::unique_ptr<LZMA_Decompress_Helper> decomp;
	};

	bool is_singular[COLOR_NB] = { false, false };
	bool is_dropped[COLOR_NB]  = { false, false };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped;

	void load(const Piece_Config& ps, const std::filesystem::path& path);
	NODISCARD DTM_Final_Entry read(Color c, Board_Index pos, WDL_Entry wdl);

private:
	NODISCARD Block_Ptr get_block_locked(Per_Color& pc, size_t block_id);
};

void DTM_Probe_File::load(const Piece_Config& ps, const std::filesystem::path& path)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error("Cannot open DTM file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM file size " + path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(CHECKSUM_INIT))
		throw std::runtime_error("Invalid DTM checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(EGTB_Magic::DTM_MAGIC))
		throw std::runtime_error("Invalid DTM magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = Material_Key(key_and_table_num >> 2);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM " + path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	size_t data_size[COLOR_NB]{};

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			const WDL_Entry sv = static_cast<WDL_Entry>(reader.read<uint8_t>());
			if (sv != WDL_Entry::DRAW)
				throw std::runtime_error("DTM singular value must be DRAW");
		}
		else if (flag & DROPPED_FLAG)
		{
			is_dropped[i] = true;
		}
		else
		{
			Per_Color& pc = per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(DTM_Final_Entry))
				throw std::runtime_error("Bad DTM entry_bytes " + path.string());
			pc.tail_size  = reader.read<uint32_t>();
			pc.block_size = reader.read<uint32_t>();
			pc.block_cnt  = reader.read<uint32_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		per_color[i].offset_tb = reader.caret();
		reader.advance(per_color[i].block_cnt * 8);
	}
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = Piece_Config_For_Gen(ps).num_positions();
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTM decompressed size mismatch " + path.string());
	}
}

Block_Ptr DTM_Probe_File::get_block_locked(Per_Color& pc, size_t block_id)
{
	for (size_t i = 0; i < pc.live; ++i)
	{
		if (pc.block_id[i] == block_id)
			return pc.data[i];
	}

	const size_t decode_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const size_t positions = decode_sz / pc.entry_bytes;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	const size_t dsz  = dso & 0xFFFFF;
	const size_t doff = dso >> 20;

	size_t slot;
	if (pc.live < BLOCK_CACHE_SLOTS)
	{
		slot = pc.live++;
	}
	else
	{
		slot = pc.next_slot;
		pc.next_slot = (pc.next_slot + 1) % BLOCK_CACHE_SLOTS;
	}
	pc.block_id[slot] = block_id;
	auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

	if (dsz != 0)
	{
		if (!pc.decomp)
			pc.decomp = std::make_unique<LZMA_Decompress_Helper>(pc.block_size);
		const Const_Span<uint8_t> raw = pc.decomp->decompress(
			Const_Span(pc.compressed_data + doff, dsz), decode_sz);

		const auto& r2v = pc.rank_to_value;
		uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
		if (pc.entry_bytes == 1)
		{
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[raw[k]];
		}
		else
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
			for (size_t k = 0; k < positions; ++k)
				out[k] = r2v[in[k]];
		}
	}

	pc.data[slot] = buf;
	return pc.data[slot];
}

DTM_Final_Entry DTM_Probe_File::read(Color c, Board_Index pos, WDL_Entry wdl)
{
	if (is_singular[c]) return DTM_Final_Entry::make_draw();
	ASSERT(!is_dropped[c]);

	Per_Color& pc = per_color[c];
	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	if ((dso & 0xFFFFF) == 0)
		return DTM_Final_Entry::make_illegal();

	thread_local TL_Block_FIFO tl;
	const uint8_t* data = nullptr;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == pc.epoch && tl.block_id[i] == block_id)
			{ data = tl.bytes[i]->data(); break; }
	if (!data)
	{
		std::lock_guard<std::mutex> lk(pc.mu);
		Block_Ptr blk = get_block_locked(pc, block_id);
		const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
		tl.epoch[s]    = pc.epoch;
		tl.block_id[s] = block_id;
		tl.bytes[s]    = blk;
		data = blk->data();
	}
	uint16_t stored;
	std::memcpy(&stored, data + in_block_pos * sizeof(uint16_t), sizeof(stored));
	return dtm_entry_from_storage(stored, wdl);
}

// DTM50 layer file. Same container as DTM_Probe_File; differs only in the
// magic and the storage→entry mapping (layer-aware: dispatches by m_hmc).
struct DTM50_Probe_File
{
	struct Per_Color
	{
		size_t entry_bytes = 0;
		size_t block_size = 0;
		size_t tail_size = 0;
		size_t block_cnt = 0;
		const uint8_t* offset_tb = nullptr;
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;

		const uint64_t epoch = next_probe_epoch();
		mutable std::mutex mu;
		std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
		std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
		size_t next_slot = 0;
		size_t live = 0;
		std::unique_ptr<LZMA_Decompress_Helper> decomp;
	};

	bool is_singular[COLOR_NB] = { false, false };
	bool is_dropped[COLOR_NB]  = { false, false };
	Per_Color per_color[COLOR_NB];
	Memory_Mapped_File mapped;
	// hmc this file represents; chooses the decode variant (hmc=0 vs k>0).
	uint16_t m_hmc = 0;

	void load(const Piece_Config& ps, const std::filesystem::path& path);
	NODISCARD DTM_Final_Entry read(Color c, Board_Index pos, WDL_Entry wdl);

private:
	NODISCARD Block_Ptr get_block_locked(Per_Color& pc, size_t block_id);
};

void DTM50_Probe_File::load(const Piece_Config& ps, const std::filesystem::path& path)
{
	if (!mapped.open_readonly(path.c_str()))
		throw std::runtime_error("Cannot open DTM50 file " + path.string());

	const Const_Span<uint8_t> input = mapped.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM50 file size " + path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(CHECKSUM_INIT))
		throw std::runtime_error("Invalid DTM50 checksum " + path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		throw std::runtime_error("Invalid DTM50 magic " + path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = Material_Key(key_and_table_num >> 2);
	if (key != ps.min_material_key())
		throw std::runtime_error("Wrong material key in DTM50 " + path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);

	size_t data_size[COLOR_NB]{};

	for (Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & SINGULAR_FLAG)
		{
			is_singular[i] = true;
			const WDL_Entry sv = static_cast<WDL_Entry>(reader.read<uint8_t>());
			if (sv != WDL_Entry::DRAW)
				throw std::runtime_error("DTM50 singular value must be DRAW");
		}
		else if (flag & DROPPED_FLAG)
		{
			is_dropped[i] = true;
		}
		else
		{
			Per_Color& pc = per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != 1 && pc.entry_bytes != sizeof(DTM_Final_Entry))
				throw std::runtime_error("Bad DTM50 entry_bytes " + path.string());
			pc.tail_size  = reader.read<uint32_t>();
			pc.block_size = reader.read<uint32_t>();
			pc.block_cnt  = reader.read<uint32_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		per_color[i].offset_tb = reader.caret();
		reader.advance(per_color[i].block_cnt * 8);
	}
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		reader.align(64);
		per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = Piece_Config_For_Gen(ps).num_positions();
	for (Color i : table_colors)
	{
		if (is_singular[i] || is_dropped[i]) continue;
		Per_Color& pc = per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTM50 decompressed size mismatch " + path.string());
	}
}

Block_Ptr DTM50_Probe_File::get_block_locked(Per_Color& pc, size_t block_id)
{
	for (size_t i = 0; i < pc.live; ++i)
		if (pc.block_id[i] == block_id) return pc.data[i];

	const size_t decode_sz =
		(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
	const size_t positions = decode_sz / pc.entry_bytes;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	const size_t dsz  = dso & 0xFFFFF;
	const size_t doff = dso >> 20;

	size_t slot;
	if (pc.live < BLOCK_CACHE_SLOTS) { slot = pc.live++; }
	else { slot = pc.next_slot; pc.next_slot = (pc.next_slot + 1) % BLOCK_CACHE_SLOTS; }
	pc.block_id[slot] = block_id;
	auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

	if (dsz != 0)
	{
		if (!pc.decomp) pc.decomp = std::make_unique<LZMA_Decompress_Helper>(pc.block_size);
		const Const_Span<uint8_t> raw = pc.decomp->decompress(
			Const_Span(pc.compressed_data + doff, dsz), decode_sz);

		const auto& r2v = pc.rank_to_value;
		uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
		if (pc.entry_bytes == 1)
			for (size_t k = 0; k < positions; ++k) out[k] = r2v[raw[k]];
		else
		{
			const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
			for (size_t k = 0; k < positions; ++k) out[k] = r2v[in[k]];
		}
	}
	pc.data[slot] = buf;
	return pc.data[slot];
}

DTM_Final_Entry DTM50_Probe_File::read(Color c, Board_Index pos, WDL_Entry wdl)
{
	if (is_singular[c]) return DTM_Final_Entry::make_draw();
	// is_dropped is handled by probe_dtm50_internal — callers must check first.
	ASSERT(!is_dropped[c]);

	Per_Color& pc = per_color[c];
	const size_t positions_per_block = pc.block_size / pc.entry_bytes;
	const size_t block_id = static_cast<size_t>(pos) / positions_per_block;
	const size_t in_block_pos = static_cast<size_t>(pos) % positions_per_block;

	const uint64_t dso = reinterpret_cast<const uint64_t*>(pc.offset_tb + block_id * 8)[0];
	if ((dso & 0xFFFFF) == 0) return DTM_Final_Entry::make_illegal();

	thread_local TL_Block_FIFO tl;
	const uint8_t* data = nullptr;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == pc.epoch && tl.block_id[i] == block_id)
			{ data = tl.bytes[i]->data(); break; }
	if (!data)
	{
		std::lock_guard<std::mutex> lk(pc.mu);
		Block_Ptr blk = get_block_locked(pc, block_id);
		const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
		tl.epoch[s]    = pc.epoch;
		tl.block_id[s] = block_id;
		tl.bytes[s]    = blk;
		data = blk->data();
	}
	uint16_t stored;
	std::memcpy(&stored, data + in_block_pos * sizeof(uint16_t), sizeof(stored));
	return m_hmc == 0
		? dtm50_entry_from_storage(stored, wdl)
		: dtm50_layered_entry_from_storage(stored, wdl);
}

constexpr int MAX_DERIVE_DEPTH = 16;

NODISCARD WDL_Entry invert_wdl(WDL_Entry w)
{
	switch (w)
	{
		case WDL_Entry::WIN:          return WDL_Entry::LOSE;
		case WDL_Entry::CURSED_WIN:   return WDL_Entry::BLESSED_LOSS;
		case WDL_Entry::DRAW:         return WDL_Entry::DRAW;
		case WDL_Entry::BLESSED_LOSS: return WDL_Entry::CURSED_WIN;
		case WDL_Entry::LOSE:         return WDL_Entry::WIN;
		case WDL_Entry::ILLEGAL:      return WDL_Entry::ILLEGAL;
	}
	return WDL_Entry::ILLEGAL;
}

NODISCARD WDL_Entry fold_dtm_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN) return WDL_Entry::WIN;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::LOSE;
	return w;
}

// 5-class WDL → DTM50's 3-class: cursed/blessed are unreachable under 50MR.
NODISCARD WDL_Entry fold_dtm50_wdl(WDL_Entry w)
{
	if (w == WDL_Entry::CURSED_WIN)   return WDL_Entry::DRAW;
	if (w == WDL_Entry::BLESSED_LOSS) return WDL_Entry::DRAW;
	return w;
}

NODISCARD bool move_is_zeroing(const Position& pos, Move m)
{
	return piece_type(pos.piece_at(m.from())) == PAWN
	    || pos.piece_at(m.to()) != PIECE_NONE
	    || m.is_ep_capture();
}

NODISCARD int wdl_rank(WDL_Entry w)
{
	switch (w) {
		case WDL_Entry::WIN:          return 4;
		case WDL_Entry::CURSED_WIN:   return 3;
		case WDL_Entry::DRAW:         return 2;
		case WDL_Entry::BLESSED_LOSS: return 1;
		case WDL_Entry::LOSE:         return 0;
		case WDL_Entry::ILLEGAL:      return -1;
	}
	return -1;
}

NODISCARD bool prefer_new(WDL_Entry new_wdl, uint16_t new_dtc,
                          WDL_Entry old_wdl, uint16_t old_dtc)
{
	const int rn = wdl_rank(new_wdl), ro = wdl_rank(old_wdl);
	if (rn != ro) return rn > ro;
	const bool win_side = (new_wdl == WDL_Entry::WIN || new_wdl == WDL_Entry::CURSED_WIN);
	const bool loss_side = (new_wdl == WDL_Entry::LOSE || new_wdl == WDL_Entry::BLESSED_LOSS);
	if (win_side)  return new_dtc < old_dtc;
	if (loss_side) return new_dtc > old_dtc;
	return false;
}

bool is_symmetric_material(const Piece_Config& ps)
{
	const auto [mat_key, mir_key] = ps.material_keys();
	return mat_key == mir_key;
}

struct Child_Pos { Position pos; Piece_Config ps; bool is_kk; bool is_zeroing; };

Child_Pos make_child(const Position& parent, Move m)
{
	const bool zeroing = move_is_zeroing(parent, m);
	Position pos = parent;
	(void)pos.do_move(m);

	auto [cps, lit] = piece_config_and_literal_key_from_position(pos);
	if (lit != cps.base_material_key())
		pos = mirror_for_canonical(pos);

	const bool is_kk = (cps.num_pieces() <= 2);
	return Child_Pos{std::move(pos), std::move(cps), is_kk, zeroing};
}

void add_ep_moves(const Position& pos, Square ep_square, Move_List& ml)
{
	if (ep_square == SQ_END) return;
	const Color me = pos.turn();
	const Rank target_rank = (me == WHITE) ? RANK_6 : RANK_3;
	const Rank pawn_rank   = (me == WHITE) ? RANK_5 : RANK_4;
	if (sq_rank(ep_square) != target_rank) return;

	const File target_file = sq_file(ep_square);
	for (int df : { -1, +1 })
	{
		const int f = static_cast<int>(target_file) + df;
		if (f < 0 || f >= 8) continue;
		const Square from = sq_make(pawn_rank, static_cast<File>(f));
		if (pos.piece_at(from) != piece_make(me, PAWN)) continue;

		const Square cap_sq = sq_make(pawn_rank, target_file);
		if (pos.piece_at(cap_sq) != piece_make(color_opp(me), PAWN)) continue;

		ml.add(Move::make_ep_capture(from, ep_square));
	}
}

template <typename T, typename K = uint32_t>
struct TL_Probe_Cache
{
	static constexpr size_t N = 4;
	uint64_t    epoch[N] = {};
	K           key  [N] = {};
	T*          val  [N] = {};
	size_t      rr       = 0;

	bool lookup(uint64_t impl_epoch, K k, T*& out) const
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				{ out = val[i]; return true; }
		return false;
	}
	void insert(uint64_t impl_epoch, K k, T* v)
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				{ val[i] = v; return; }
		const size_t i = (rr++) & (N - 1);
		epoch[i] = impl_epoch; key[i] = k; val[i] = v;
	}
};

}  // namespace

struct Probe_Tables::Impl
{
	const uint64_t epoch = next_probe_epoch();
	std::vector<std::filesystem::path> wdl_dirs = { "./wdl/" };
	std::vector<std::filesystem::path> dtc_dirs = { "./dtc/" };
	std::vector<std::filesystem::path> dtm_dirs = { "./dtm/" };
	std::vector<std::filesystem::path> dtm50_dirs = { "./dtm50/" };

	// Use material-key integers to avoid per-probe string hashing.
	mutable std::shared_mutex                                 wdl_mu;
	std::unordered_map<uint32_t, std::unique_ptr<WDL_Probe_File>> wdl_cache;
	mutable std::shared_mutex                                 dtc_mu;
	std::unordered_map<uint32_t, std::unique_ptr<DTC_Probe_File>> dtc_cache;
	mutable std::shared_mutex                                 dtm_mu;
	std::unordered_map<uint32_t, std::unique_ptr<DTM_Probe_File>> dtm_cache;
	// DTM50 cache key is (material_key << 8 | hmc) — one DTM50_Probe_File per
	// (material, layer) since each layer lives in its own .lzdtm50 file.
	mutable std::shared_mutex                                 dtm50_mu;
	std::unordered_map<uint64_t, std::unique_ptr<DTM50_Probe_File>> dtm50_cache;
	mutable std::shared_mutex                                 epsi_mu;
	std::unordered_map<uint32_t, std::unique_ptr<Piece_Config_For_Gen>> epsi_cache;

	std::atomic<size_t> largest_pieces{0};

	NODISCARD const Piece_Config_For_Gen& get_epsi(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Probe_Cache<const Piece_Config_For_Gen> tl;
		const Piece_Config_For_Gen* hit;
		if (tl.lookup(epoch, k, hit)) return *hit;
		{
			std::shared_lock rlk(epsi_mu);
			auto it = epsi_cache.find(k);
			if (it != epsi_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return *it->second; }
		}
		auto e = std::make_unique<Piece_Config_For_Gen>(ps);
		std::unique_lock wlk(epsi_mu);
		auto [it, inserted] = epsi_cache.try_emplace(k, std::move(e));
		const Piece_Config_For_Gen* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return *raw;
	}

	NODISCARD WDL_Probe_File* open_wdl(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Probe_Cache<WDL_Probe_File> tl;
		WDL_Probe_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(wdl_mu);
			auto it = wdl_cache.find(k);
			if (it != wdl_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<WDL_Probe_File> f;
		if (find_in_dirs(ps, WDL_EXT, wdl_dirs, &path))
		{
			f = std::make_unique<WDL_Probe_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(wdl_mu);
		auto [it, inserted] = wdl_cache.try_emplace(k, std::move(f));
		WDL_Probe_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTC_Probe_File* open_dtc(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Probe_Cache<DTC_Probe_File> tl;
		DTC_Probe_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtc_mu);
			auto it = dtc_cache.find(k);
			if (it != dtc_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTC_Probe_File> f;
		if (find_in_dirs(ps, DTC_EXT, dtc_dirs, &path))
		{
			f = std::make_unique<DTC_Probe_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(dtc_mu);
		auto [it, inserted] = dtc_cache.try_emplace(k, std::move(f));
		DTC_Probe_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTM_Probe_File* open_dtm(const Piece_Config& ps)
	{
		const uint32_t k = ps.min_material_key().value();
		thread_local TL_Probe_Cache<DTM_Probe_File> tl;
		DTM_Probe_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtm_mu);
			auto it = dtm_cache.find(k);
			if (it != dtm_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTM_Probe_File> f;
		if (find_in_dirs(ps, DTM_EXT, dtm_dirs, &path))
		{
			f = std::make_unique<DTM_Probe_File>();
			f->load(ps, path);
		}
		std::unique_lock wlk(dtm_mu);
		auto [it, inserted] = dtm_cache.try_emplace(k, std::move(f));
		DTM_Probe_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD DTM50_Probe_File* open_dtm50(const Piece_Config& ps, uint16_t hmc)
	{
		const uint64_t k = (static_cast<uint64_t>(ps.min_material_key().value()) << 8) | hmc;
		thread_local TL_Probe_Cache<DTM50_Probe_File, uint64_t> tl;
		DTM50_Probe_File* hit;
		if (tl.lookup(epoch, k, hit)) return hit;
		{
			std::shared_lock rlk(dtm50_mu);
			auto it = dtm50_cache.find(k);
			if (it != dtm50_cache.end())
				{ tl.insert(epoch, k, it->second.get()); return it->second.get(); }
		}
		std::filesystem::path path;
		std::unique_ptr<DTM50_Probe_File> f;
		if (find_dtm50_in_dirs(ps, hmc, dtm50_dirs, &path))
		{
			f = std::make_unique<DTM50_Probe_File>();
			f->load(ps, path);
			f->m_hmc = hmc;
		}
		std::unique_lock wlk(dtm50_mu);
		auto [it, inserted] = dtm50_cache.try_emplace(k, std::move(f));
		DTM50_Probe_File* raw = it->second.get();
		tl.insert(epoch, k, raw);
		return raw;
	}

	NODISCARD Probe_Result   probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth);
	NODISCARD WDL_Entry      probe_wdl_internal (const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD DTC_Final_Entry probe_dtc_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM_Final_Entry probe_dtm_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth);
	NODISCARD DTM_Final_Entry probe_dtm50_internal(const Piece_Config& ps, const Position& pos,
	                                                WDL_Entry wdl, unsigned rule50, int depth);
	NODISCARD WDL_Entry      derive_wdl(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD DTC_Final_Entry derive_dtc(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD DTM_Final_Entry derive_dtm(const Piece_Config& ps, const Position& pos, int depth);
	NODISCARD DTM_Final_Entry derive_dtm50(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth);

	Probe_Result apply_ep_overlay(const Position& root, const Probe_Result& no_ep, Square ep_square);

	void scan_paths();
};

namespace {

// Symmetric materials store only the WHITE-to-move frame.
Position mirror_symmetric_black_stm(const Position& pos)
{
	Position swapped;
	swapped.clear();
	for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
	{
		const Piece p = pos.piece_at(sq);
		if (p != PIECE_NONE)
			swapped.put_piece(piece_opp_color(p), sq_rank_mirror(sq));
	}
	swapped.set_turn(WHITE);
	return swapped;
}

struct Canonical_Root
{
	Piece_Config ps;
	Position pos;
	Square ep_square = SQ_END;
	bool mirrored = false;
};

Canonical_Root canonical_root_from_position(const Position& input, Square ep_square)
{
	auto [ps, literal_key] = piece_config_and_literal_key_from_position(input);
	Position pos = input;
	if (literal_key != ps.base_material_key())
	{
		Position swapped;
		swapped.clear();
		for (Square sq = SQ_A1; sq < SQ_END; sq = static_cast<Square>(sq + 1))
		{
			const Piece q = input.piece_at(sq);
			if (q != PIECE_NONE)
				swapped.put_piece(piece_opp_color(q), sq_rank_mirror(sq));
		}
		swapped.set_turn(color_opp(input.turn()));
		pos = swapped;
		if (ep_square != SQ_END)
			ep_square = sq_rank_mirror(ep_square);
		return { std::move(ps), std::move(pos), ep_square, true };
	}
	return { std::move(ps), std::move(pos), ep_square, false };
}

std::optional<Canonical_Root> canonical_root_from_config(
	const Piece_Config& ps, const Position& input, Square ep_square)
{
	const Material_Key literal_key = input.material_key();
	const auto [base_key, mirror_key] = ps.material_keys();
	if (literal_key == base_key)
		return Canonical_Root{ ps, input, ep_square, false };
	if (literal_key != mirror_key)
		return std::nullopt;

	Position pos = mirror_for_canonical(input);
	if (ep_square != SQ_END)
		ep_square = sq_rank_mirror(ep_square);
	return Canonical_Root{ ps, std::move(pos), ep_square, true };
}

Probe_Result illegal_probe_result()
{
	Probe_Result r;
	r.status = Probe_Result::Status::ILLEGAL_POS;
	return r;
}

Move rank_mirror_move(Move m)
{
	if (m.is_ep_capture())
		return Move::make_ep_capture(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
	if (m.is_promotion())
		return Move::make_promotion(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()), m.promotion());
	return Move::make_quiet(sq_rank_mirror(m.from()), sq_rank_mirror(m.to()));
}

}  // namespace

WDL_Entry Probe_Tables::Impl::probe_wdl_internal(const Piece_Config& ps, const Position& pos, int depth)
{
	WDL_Probe_File* w = open_wdl(ps);
	if (!w) return WDL_Entry::ILLEGAL;

	const Piece_Config_For_Gen& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return WDL_Entry::ILLEGAL;

	const Color stm = pos.turn();
	return w->is_dropped[stm]
		? derive_wdl(ps, pos, depth)
		: w->read(stm, idx);
}

DTC_Final_Entry Probe_Tables::Impl::probe_dtc_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	DTC_Probe_File* d = open_dtc(ps);
	if (!d) return DTC_Final_Entry::make_score(DTC_SCORE_ZERO);

	const Piece_Config_For_Gen& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return DTC_Final_Entry::make_score(DTC_SCORE_ZERO);

	const Color stm = pos.turn();
	return d->is_dropped[stm]
		? derive_dtc(ps, pos, depth)
		: d->read(stm, idx, wdl);
}

DTM_Final_Entry Probe_Tables::Impl::probe_dtm_internal(const Piece_Config& ps, const Position& pos, WDL_Entry wdl, int depth)
{
	DTM_Probe_File* d = open_dtm(ps);
	if (!d) return DTM_Final_Entry::make_draw();

	const Piece_Config_For_Gen& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return DTM_Final_Entry::make_illegal();

	const Color stm = pos.turn();
	return d->is_dropped[stm]
		? derive_dtm(ps, pos, depth)
		: d->read(stm, idx, wdl);
}

WDL_Entry Probe_Tables::Impl::derive_wdl(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return WDL_Entry::ILLEGAL;

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best = WDL_Entry::LOSE;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		const WDL_Entry cw = c.is_kk
			? WDL_Entry::DRAW
			: probe_wdl_internal(c.ps, c.pos, depth + 1);
		if (cw == WDL_Entry::ILLEGAL) continue;

		const WDL_Entry mw = invert_wdl(cw);
		if (wdl_rank(mw) > wdl_rank(best)) best = mw;
		have_candidate = true;
	}

	if (!any_legal) return pos.is_in_check() ? WDL_Entry::LOSE : WDL_Entry::DRAW;
	if (!have_candidate) return WDL_Entry::ILLEGAL;
	return best;
}

DTC_Final_Entry Probe_Tables::Impl::derive_dtc(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return DTC_Final_Entry::make_illegal();

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtc = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		DTC_Final_Entry cd;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			cd = DTC_Final_Entry::make_score(DTC_SCORE_ZERO);
		}
		else
		{
			cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			cd = probe_dtc_internal(c.ps, c.pos, cw, depth + 1);
			if (cd.is_illegal()) continue;
		}

		WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtc = c.is_zeroing
			? uint16_t{1}
			: static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.value()));
		if (my_dtc > DTC_Final_Entry::MAX_NON_CURSED_DTZ)
		{
			if (my_wdl == WDL_Entry::WIN)  my_wdl = WDL_Entry::CURSED_WIN;
			if (my_wdl == WDL_Entry::LOSE) my_wdl = WDL_Entry::BLESSED_LOSS;
		}

		if (!have_candidate || prefer_new(my_wdl, my_dtc, best_wdl, best_dtc))
		{
			best_wdl = my_wdl;
			best_dtc = my_dtc;
			have_candidate = true;
		}
	}

	if (!any_legal || best_wdl == WDL_Entry::DRAW)
		return DTC_Final_Entry::make_draw();
	if (!have_candidate)
		return DTC_Final_Entry::make_illegal();
	return DTC_Final_Entry::make_score(static_cast<DTC_Score>(best_dtc));
}

DTM_Final_Entry Probe_Tables::Impl::derive_dtm(const Piece_Config& ps, const Position& pos, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return DTM_Final_Entry::make_illegal();

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		WDL_Entry cw;
		DTM_Final_Entry cd;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			cd = DTM_Final_Entry::make_draw();
		}
		else
		{
			cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			cd = probe_dtm_internal(c.ps, c.pos, cw, depth + 1);
			if (cd.is_illegal()) continue;
		}

		if (cw == WDL_Entry::CURSED_WIN)   cw = WDL_Entry::WIN;
		if (cw == WDL_Entry::BLESSED_LOSS) cw = WDL_Entry::LOSE;

		const WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.value()));

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal)
		return pos.is_in_check() ? DTM_Final_Entry::make_loss(0) : DTM_Final_Entry::make_draw();
	if (!have_candidate)
		return DTM_Final_Entry::make_illegal();

	if (best_wdl == WDL_Entry::WIN)  return DTM_Final_Entry::make_win(best_dtm);
	if (best_wdl == WDL_Entry::LOSE) return DTM_Final_Entry::make_loss(best_dtm);
	return DTM_Final_Entry::make_draw();
}

Probe_Result Probe_Tables::Impl::probe_impl(const Piece_Config& ps, const Position& pos, unsigned rule50, int depth)
{
	if (pos.turn() == BLACK && is_symmetric_material(ps))
		return probe_impl(ps, mirror_symmetric_black_stm(pos), rule50, depth);

	Probe_Result r;
	WDL_Probe_File* w = open_wdl(ps);
	DTC_Probe_File* d = open_dtc(ps);
	DTM_Probe_File* m = open_dtm(ps);
	// rule50 ≥ 100 ⇒ auto-50MR DRAW (no layer to read). Other tables unaffected.
	const bool rule50_drawn = rule50 != PROBE_IMPL_SKIP_DTM50 && rule50 >= DTM50_HMC_COUNT;
	DTM50_Probe_File* m50 = nullptr;
	if (rule50 != PROBE_IMPL_SKIP_DTM50 && !rule50_drawn)
		m50 = open_dtm50(ps, static_cast<uint16_t>(rule50));
	if (!w && !d && !m && !m50 && !rule50_drawn) return r;

	const Piece_Config_For_Gen& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE)
	{
		r.status = Probe_Result::Status::ILLEGAL_POS;
		return r;
	}
	Position_For_Gen pfg(epsi, idx, pos.turn());
	if (!pfg.is_legal(Position_For_Gen::Legality_Lower_Bound::CHESS_LEGAL))
	{
		r.status = Probe_Result::Status::ILLEGAL_POS;
		return r;
	}

	r.status = Probe_Result::Status::OK;
	if (w) r.wdl = probe_wdl_internal(ps, pos, depth);
	if (d && w)
	{
		r.dtc = probe_dtc_internal(ps, pos, r.wdl, depth);
		r.has_dtc = !r.dtc.is_illegal();
	}
	if (m && w)
	{
		r.dtm = probe_dtm_internal(ps, pos, r.wdl, depth);
		r.has_dtm = !r.dtm.is_illegal();
	}
	if (rule50_drawn)
	{
		r.dtm50 = DTM_Final_Entry::make_draw();
		r.has_dtm50 = true;
	}
	else if (m50 && w)
	{
		r.dtm50 = probe_dtm50_internal(ps, pos, r.wdl, rule50, depth);
		r.has_dtm50 = !r.dtm50.is_illegal();
	}
	return r;
}

Probe_Result Probe_Tables::Impl::apply_ep_overlay(const Position& root,
                                                   const Probe_Result& no_ep,
                                                   Square ep_square)
{
	if (no_ep.status != Probe_Result::Status::OK || ep_square == SQ_END)
		return no_ep;

	Move_List eps;
	add_ep_moves(root, ep_square, eps);
	if (eps.empty()) return no_ep;

	Probe_Result best = no_ep;
	WDL_Entry best_dtc_wdl = no_ep.wdl;
	uint16_t  best_dtc     = no_ep.has_dtc ? static_cast<uint16_t>(no_ep.dtc.value()) : 0;
	WDL_Entry best_dtm_wdl = fold_dtm_wdl(no_ep.wdl);
	uint16_t  best_dtm     = no_ep.has_dtm ? static_cast<uint16_t>(no_ep.dtm.value()) : 0;
	WDL_Entry best_dtm50_wdl = fold_dtm50_wdl(no_ep.wdl);
	uint16_t  best_dtm50     = no_ep.has_dtm50 ? static_cast<uint16_t>(no_ep.dtm50.value()) : 0;

	for (size_t i = 0; i < eps.size(); ++i)
	{
		if (!root.is_pseudo_legal_move_legal(eps[i])) continue;
		Child_Pos child = make_child(root, eps[i]);
		Probe_Result cr;
		if (child.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtc = best.has_dtc;
			cr.dtc = DTC_Final_Entry::make_draw();
			cr.has_dtm = best.has_dtm;
			cr.dtm = DTM_Final_Entry::make_draw();
			cr.has_dtm50 = best.has_dtm50;
			cr.dtm50 = DTM_Final_Entry::make_draw();
		}
		else
		{
			cr = probe_impl(child.ps, child.pos, 0, 0);  // EP is zeroing
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			continue;

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		if (wdl_rank(my_wdl) > wdl_rank(best.wdl))
			best.wdl = my_wdl;

		if (best.has_dtc && cr.has_dtc)
		{
			const uint16_t my_dtc = 1;
			if (prefer_new(my_wdl, my_dtc, best_dtc_wdl, best_dtc))
			{
				best_dtc_wdl = my_wdl;
				best_dtc = my_dtc;
				best.dtc = DTC_Final_Entry::make_score(static_cast<DTC_Score>(my_dtc));
			}
		}

		if (best.has_dtm && cr.has_dtm)
		{
			const WDL_Entry my_dtm_wdl = fold_dtm_wdl(my_wdl);
			const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cr.dtm.value()));
			if (prefer_new(my_dtm_wdl, my_dtm, best_dtm_wdl, best_dtm))
			{
				best_dtm_wdl = my_dtm_wdl;
				best_dtm = my_dtm;
				if (my_dtm_wdl == WDL_Entry::WIN)
					best.dtm = DTM_Final_Entry::make_win(my_dtm);
				else if (my_dtm_wdl == WDL_Entry::LOSE)
					best.dtm = DTM_Final_Entry::make_loss(my_dtm);
				else
					best.dtm = DTM_Final_Entry::make_draw();
			}
		}

		if (best.has_dtm50 && cr.has_dtm50)
		{
			const WDL_Entry my_dtm50_wdl = fold_dtm50_wdl(my_wdl);
			const uint16_t my_dtm50 = static_cast<uint16_t>(1u + static_cast<uint16_t>(cr.dtm50.value()));
			if (prefer_new(my_dtm50_wdl, my_dtm50, best_dtm50_wdl, best_dtm50))
			{
				best_dtm50_wdl = my_dtm50_wdl;
				best_dtm50 = my_dtm50;
				if (my_dtm50_wdl == WDL_Entry::WIN)
					best.dtm50 = DTM_Final_Entry::make_win(my_dtm50);
				else if (my_dtm50_wdl == WDL_Entry::LOSE)
					best.dtm50 = DTM_Final_Entry::make_loss(my_dtm50);
				else
					best.dtm50 = DTM_Final_Entry::make_draw();
			}
		}
	}
	return best;
}

namespace {

// Count material characters before the extension.
size_t count_pieces_from_filename(const std::string& fname)
{
	size_t n = 0;
	for (char c : fname)
	{
		if (c == '.') break;
		++n;
	}
	return n;
}

}  // namespace

void Probe_Tables::Impl::scan_paths()
{
	size_t lg = 0;
	auto scan_dir = [&](const std::filesystem::path& dir, const char* ext) {
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) return;
		for (auto& e : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			const std::string n = e.path().filename().string();
			if (n.size() < std::strlen(ext)) continue;
			if (n.compare(n.size() - std::strlen(ext), std::strlen(ext), ext) != 0) continue;
			const size_t cnt = count_pieces_from_filename(n);
			if (cnt > lg) lg = cnt;
		}
	};
	for (const auto& d : wdl_dirs) scan_dir(d, WDL_EXT);
	for (const auto& d : dtc_dirs) scan_dir(d, DTC_EXT);
	for (const auto& d : dtm_dirs) scan_dir(d, DTM_EXT);
	// DTM50 lives in per-material subfolders; the material name is the folder
	// name itself rather than the file basename.
	auto scan_dtm50_dir = [&](const std::filesystem::path& dir) {
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) return;
		for (auto& e : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			if (!e.is_directory(ec)) continue;
			const std::string n = e.path().filename().string();
			const size_t cnt = count_pieces_from_filename(n + ".");
			if (cnt > lg) lg = cnt;
		}
	};
	for (const auto& d : dtm50_dirs) scan_dtm50_dir(d);
	largest_pieces.store(lg, std::memory_order_release);
}

Probe_Tables::Probe_Tables() : m_impl(std::make_unique<Impl>()) {}
Probe_Tables::~Probe_Tables() = default;
Probe_Tables::Probe_Tables(Probe_Tables&&) noexcept = default;
Probe_Tables& Probe_Tables::operator=(Probe_Tables&&) noexcept = default;

void Probe_Tables::add_wdl_path(std::filesystem::path dir) { m_impl->wdl_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtc_path(std::filesystem::path dir) { m_impl->dtc_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtm_path(std::filesystem::path dir) { m_impl->dtm_dirs.emplace_back(std::move(dir)); }
void Probe_Tables::add_dtm50_path(std::filesystem::path dir) { m_impl->dtm50_dirs.emplace_back(std::move(dir)); }

bool Probe_Tables::init(const std::filesystem::path& dir)
{
	std::error_code ec;
	auto try_sub = [&](const char* sub) -> std::filesystem::path {
		auto p = dir / sub;
		return std::filesystem::is_directory(p, ec) ? p : dir;
	};
	m_impl->wdl_dirs.push_back(try_sub("wdl"));
	m_impl->dtc_dirs.push_back(try_sub("dtc"));
	m_impl->dtm_dirs.push_back(try_sub("dtm"));
	m_impl->dtm50_dirs.push_back(try_sub("dtm50"));
	m_impl->scan_paths();
	return m_impl->largest_pieces.load(std::memory_order_acquire) > 0;
}

size_t Probe_Tables::largest() const
{
	return m_impl->largest_pieces.load(std::memory_order_acquire);
}

void Probe_Tables::rescan() { m_impl->scan_paths(); }

Probe_Result Probe_Tables::probe(const Position& pos, unsigned rule50)
{
	const Canonical_Root root = canonical_root_from_position(pos, SQ_END);
	return m_impl->probe_impl(root.ps, root.pos, rule50, 0);
}

Probe_Result Probe_Tables::probe(const Position& pos, Square ep_square, unsigned rule50)
{
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Probe_Result base = m_impl->probe_impl(root.ps, root.pos, rule50, 0);
	return m_impl->apply_ep_overlay(root.pos, base, root.ep_square);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, unsigned rule50)
{
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, SQ_END);
	if (!root) return illegal_probe_result();
	return m_impl->probe_impl(root->ps, root->pos, rule50, 0);
}

Probe_Result Probe_Tables::probe(const Piece_Config& ps, const Position& pos, Square ep_square, unsigned rule50)
{
	const std::optional<Canonical_Root> root = canonical_root_from_config(ps, pos, ep_square);
	if (!root) return illegal_probe_result();
	const Probe_Result base = m_impl->probe_impl(root->ps, root->pos, rule50, 0);
	return m_impl->apply_ep_overlay(root->pos, base, root->ep_square);
}

WDL_Entry Probe_Tables::probe_wdl(const Position& pos, Square ep_square, unsigned rule50)
{
	if (rule50 != 0) return WDL_Entry::ILLEGAL;
	const Probe_Result r = probe(pos, ep_square);
	return r.status == Probe_Result::Status::OK ? r.wdl : WDL_Entry::ILLEGAL;
}

namespace {

// Recover WIN(1)/LOSS(0) that the layered decoder collapsed to DRAW at hmc>0.
// WDL=WIN: scan STM moves for a mating one. WDL=LOSE: STM mated iff in check
// with no legal moves.
NODISCARD DTM_Final_Entry recover_mate_at_hmc(const Position& pos, WDL_Entry wdl)
{
	if (wdl == WDL_Entry::WIN)
	{
		Position p = pos;
		Move_List ml;
		p.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
		for (size_t i = 0; i < ml.size(); ++i)
		{
			if (!p.is_pseudo_legal_move_legal(ml[i])) continue;
			Position child = p;
			(void)child.do_move(ml[i]);
			if (!child.is_in_check(child.turn())) continue;
			Move_List cml;
			child.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(cml));
			bool any_legal = false;
			for (size_t j = 0; j < cml.size() && !any_legal; ++j)
				if (child.is_pseudo_legal_move_legal(cml[j])) any_legal = true;
			if (!any_legal) return DTM_Final_Entry::make_win(1);
		}
		return DTM_Final_Entry::make_draw();
	}
	if (wdl == WDL_Entry::LOSE)
	{
		Position p = pos;
		if (!p.is_in_check(p.turn())) return DTM_Final_Entry::make_draw();
		Move_List ml;
		p.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
		for (size_t i = 0; i < ml.size(); ++i)
			if (p.is_pseudo_legal_move_legal(ml[i])) return DTM_Final_Entry::make_draw();
		return DTM_Final_Entry::make_loss(0);
	}
	return DTM_Final_Entry::make_draw();
}

}  // namespace

DTM_Final_Entry Probe_Tables::Impl::probe_dtm50_internal(
	const Piece_Config& ps, const Position& pos, WDL_Entry wdl, unsigned rule50, int depth)
{
	// Auto-50MR draw: no DTM50 layer applies.
	if (rule50 >= DTM50_HMC_COUNT) return DTM_Final_Entry::make_draw();
	const uint16_t hmc = static_cast<uint16_t>(rule50);
	DTM50_Probe_File* m = open_dtm50(ps, hmc);
	if (!m) return DTM_Final_Entry::make_illegal();

	const Piece_Config_For_Gen& epsi = get_epsi(ps);
	const Board_Index idx = board_index_of_position(epsi, pos);
	if (idx == BOARD_INDEX_NONE) return DTM_Final_Entry::make_illegal();

	const Color stm = pos.turn();
	const DTM_Final_Entry e = m->is_dropped[stm]
		? derive_dtm50(ps, pos, rule50, depth)
		: m->read(stm, idx, wdl);

	// Recover mate-in-≤1 if the layered decoder collapsed it to DRAW.
	if (hmc > 0 && e.is_draw() && (wdl == WDL_Entry::WIN || wdl == WDL_Entry::LOSE))
		return recover_mate_at_hmc(pos, wdl);
	return e;
}

// rule50-aware derive: each child tracks its own hmc (zeroing resets, quiet
// increments; ≥100 caps to DRAW unless the move is mate).
DTM_Final_Entry Probe_Tables::Impl::derive_dtm50(
	const Piece_Config& ps, const Position& pos, unsigned rule50, int depth)
{
	if (depth >= MAX_DERIVE_DEPTH) return DTM_Final_Entry::make_illegal();

	Move_List ml;
	pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));

	bool any_legal = false;
	bool have_candidate = false;
	WDL_Entry best_wdl = WDL_Entry::LOSE;
	uint16_t  best_dtm = 0;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!pos.is_pseudo_legal_move_legal(m)) continue;
		any_legal = true;

		Child_Pos c = make_child(pos, m);
		const unsigned child_rule50 = c.is_zeroing ? 0u : (rule50 + 1u);

		WDL_Entry cw;
		DTM_Final_Entry cd;
		if (c.is_kk)
		{
			cw = WDL_Entry::DRAW;
			cd = DTM_Final_Entry::make_draw();
		}
		else if (child_rule50 >= DTM50_HMC_COUNT)
		{
			// Quiet move past the 50MR window: DRAW unless the move is mate.
			Position& cb = c.pos;
			bool is_mate = false;
			if (cb.is_in_check(cb.turn()))
			{
				Move_List cml;
				cb.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(cml));
				bool any_legal = false;
				for (size_t j = 0; j < cml.size() && !any_legal; ++j)
					if (cb.is_pseudo_legal_move_legal(cml[j])) any_legal = true;
				is_mate = !any_legal;
			}
			if (is_mate) { cw = WDL_Entry::LOSE; cd = DTM_Final_Entry::make_loss(0); }
			else         { cw = WDL_Entry::DRAW; cd = DTM_Final_Entry::make_draw(); }
		}
		else
		{
			cw = probe_wdl_internal(c.ps, c.pos, depth + 1);
			if (cw == WDL_Entry::ILLEGAL) continue;
			cd = probe_dtm50_internal(c.ps, c.pos, cw, child_rule50, depth + 1);
			if (cd.is_illegal()) continue;
		}

		cw = fold_dtm50_wdl(cw);  // cursed/blessed → DRAW before inverting

		const WDL_Entry my_wdl = invert_wdl(cw);
		const uint16_t my_dtm = static_cast<uint16_t>(1u + static_cast<uint16_t>(cd.value()));

		if (!have_candidate || prefer_new(my_wdl, my_dtm, best_wdl, best_dtm))
		{
			best_wdl = my_wdl;
			best_dtm = my_dtm;
			have_candidate = true;
		}
	}

	if (!any_legal)
		return pos.is_in_check() ? DTM_Final_Entry::make_loss(0) : DTM_Final_Entry::make_draw();
	if (!have_candidate)
		return DTM_Final_Entry::make_illegal();

	if (best_wdl == WDL_Entry::WIN)  return DTM_Final_Entry::make_win(best_dtm);
	if (best_wdl == WDL_Entry::LOSE) return DTM_Final_Entry::make_loss(best_dtm);
	return DTM_Final_Entry::make_draw();
}

namespace {

// Positive values are wins for the side to move; negative values are losses.
int signed_dtz_of(const Probe_Result& r)
{
	if (!r.has_dtc) return 0;
	const int v = static_cast<int>(r.dtc.value());
	switch (r.wdl)
	{
		case WDL_Entry::WIN:          return  v;
		case WDL_Entry::CURSED_WIN:   return  v;
		case WDL_Entry::BLESSED_LOSS: return -v;
		case WDL_Entry::LOSE:         return -v;
		case WDL_Entry::DRAW:
		case WDL_Entry::ILLEGAL:
		default:                      return 0;
	}
}

// Fathom WdlToDtz mapping for zeroing moves.
int zeroing_signed_dtz(WDL_Entry my_wdl)
{
	switch (my_wdl)
	{
		case WDL_Entry::WIN:          return    1;
		case WDL_Entry::CURSED_WIN:   return  101;
		case WDL_Entry::DRAW:         return    0;
		case WDL_Entry::BLESSED_LOSS: return -101;
		case WDL_Entry::LOSE:         return   -1;
		case WDL_Entry::ILLEGAL:
		default:                      return    0;
	}
}

int fathom_dtz_rank(int v, unsigned cnt50, bool has_repeated)
{
	if (v > 0) return (static_cast<int>(v + cnt50) <= 99 && !has_repeated)
		? 1000 : 1000 - static_cast<int>(v + cnt50);
	if (v < 0) return (-v * 2 + static_cast<int>(cnt50) < 100)
		? -1000 : -1000 + static_cast<int>(-v + cnt50);
	return 0;
}

constexpr int TB_VALUE_PAWN    = 100;
constexpr int TB_VALUE_DRAW    =   0;
constexpr int TB_VALUE_MATE    = 32000;
constexpr int TB_MAX_MATE_PLY  = 255;

int fathom_dtz_score(int rank, int bound)
{
	if (rank >=  bound) return  TB_VALUE_MATE - TB_MAX_MATE_PLY - 1;
	if (rank >       0) return std::max( 3, rank - 800) * TB_VALUE_PAWN / 200;
	if (rank ==      0) return TB_VALUE_DRAW;
	if (rank >  -bound) return std::min(-3, rank + 800) * TB_VALUE_PAWN / 200;
	return -TB_VALUE_MATE + TB_MAX_MATE_PLY + 1;
}

}  // namespace

std::vector<Root_Move> Probe_Tables::probe_root_dtz(
	const Position& pos, Square ep_square,
	unsigned rule50, bool use_rule50, bool has_repeated)
{
	std::vector<Root_Move> out;
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Position& probe_pos = root.pos;

	Move_List ml;
	probe_pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);

	const int bound = use_rule50 ? 900 : 1;

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m)) continue;

		Child_Pos c = make_child(probe_pos, m);
		Probe_Result cr;
		if (c.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
			cr.has_dtc = true;
			cr.dtc = DTC_Final_Entry::make_draw();
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, PROBE_IMPL_SKIP_DTM50, 0);
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			return {};

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		int v;
		if (c.is_zeroing)
		{
			v = zeroing_signed_dtz(my_wdl);
		}
		else
		{
			if (!cr.has_dtc) return {};
			v = -signed_dtz_of(cr);
			if (v > 0) ++v;
			else if (v < 0) --v;
		}
		// Fathom reports mate-in-1 as 1, not the child-derived 2.
		if (v == 2 && c.pos.is_in_check())
		{
			Move_List cml;
			c.pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(cml));
			bool any = false;
			for (size_t j = 0; j < cml.size(); ++j)
				if (c.pos.is_pseudo_legal_move_legal(cml[j])) { any = true; break; }
			if (!any) v = 1;
		}

		const int rank = fathom_dtz_rank(v, rule50, has_repeated);
		const int score = fathom_dtz_score(rank, bound);
		out.push_back(Root_Move{root.mirrored ? rank_mirror_move(m) : m, my_wdl, v, rank, score});
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}

std::vector<Root_Move> Probe_Tables::probe_root_wdl(
	const Position& pos, Square ep_square, bool use_rule50)
{
	// Fathom WdlToRank/WdlToValue, indexed by v + 2 from side-to-move POV.
	static constexpr int WdlToRank[]  = { -1000, -899, 0, 899, 1000 };
	static constexpr int WdlToValue[] = {
		-TB_VALUE_MATE + TB_MAX_MATE_PLY + 1,
		TB_VALUE_DRAW - 2,
		TB_VALUE_DRAW,
		TB_VALUE_DRAW + 2,
		 TB_VALUE_MATE - TB_MAX_MATE_PLY - 1
	};

	auto wdl_to_v = [](WDL_Entry w) -> int {
		switch (w) {
			case WDL_Entry::WIN:          return  2;
			case WDL_Entry::CURSED_WIN:   return  1;
			case WDL_Entry::DRAW:         return  0;
			case WDL_Entry::BLESSED_LOSS: return -1;
			case WDL_Entry::LOSE:         return -2;
			default:                      return  0;
		}
	};

	std::vector<Root_Move> out;
	const Canonical_Root root = canonical_root_from_position(pos, ep_square);
	const Position& probe_pos = root.pos;

	Move_List ml;
	probe_pos.gen_pseudo_legal_moves<Position::Move_Kind::ALL>(out_param(ml));
	add_ep_moves(probe_pos, root.ep_square, ml);

	for (size_t i = 0; i < ml.size(); ++i)
	{
		const Move m = ml[i];
		if (!probe_pos.is_pseudo_legal_move_legal(m)) continue;

		Child_Pos c = make_child(probe_pos, m);
		Probe_Result cr;
		if (c.is_kk)
		{
			cr.status = Probe_Result::Status::OK;
			cr.wdl = WDL_Entry::DRAW;
		}
		else
		{
			cr = m_impl->probe_impl(c.ps, c.pos, PROBE_IMPL_SKIP_DTM50, 0);
		}
		if (cr.status != Probe_Result::Status::OK || cr.wdl == WDL_Entry::ILLEGAL)
			return {};

		const WDL_Entry my_wdl = invert_wdl(cr.wdl);
		int v = wdl_to_v(my_wdl);
		if (!use_rule50) v = v > 0 ? 2 : v < 0 ? -2 : 0;

		Root_Move r{root.mirrored ? rank_mirror_move(m) : m, my_wdl, 0, WdlToRank[v + 2], WdlToValue[v + 2]};
		out.push_back(r);
	}

	std::sort(out.begin(), out.end(),
		[](const Root_Move& a, const Root_Move& b) { return a.rank > b.rank; });
	return out;
}
