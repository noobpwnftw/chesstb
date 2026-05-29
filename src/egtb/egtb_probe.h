#pragma once

#include "egtb/egtb_entry.h"
#include "egtb/piece_config_for_gen.h"

#include "chess/chess.h"
#include "chess/index_permutation.h"
#include "chess/piece_config.h"

#include "util/compress.h"
#include "util/defines.h"
#include "util/filesystem.h"
#include "util/math.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"
#include "util/param.h"
#include "util/thread_pool.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

NODISCARD inline bool tb_file_is_full_format(const std::filesystem::path& path, EGTB_Magic expected_magic)
{
	constexpr uint8_t SINGULAR_FLAG = 0x80;
	constexpr uint8_t DROPPED_FLAG  = 0x40;

	Memory_Mapped_File m;
	if (!m.open_readonly(path.c_str())) return false;
	const Const_Span<uint8_t> s = m.data_span();
	if (s.size() < 12) return false;

	Serial_Memory_Reader r(s);
	if (r.read<uint32_t>() != narrowing_static_cast<uint32_t>(expected_magic)) return false;

	const uint32_t key_and_table_num = r.read<uint32_t>();
	const Fixed_Vector<Color, 2> colors = egtb_table_colors(key_and_table_num & 3);
	// Per-color normal headers start with the serialized index permutation
	// (a 4-byte uint32). DTC, DTM, and DTM50 then share a 27-byte payload header
	// plus inline rank table (field names differ but byte counts match); WDL has
	// a 22-byte payload header.
	const bool egtb_table = expected_magic == EGTB_Magic::DTC_MAGIC
	                     || expected_magic == EGTB_Magic::DTM_MAGIC
	                     || expected_magic == EGTB_Magic::DTM50_MAGIC;

	for (size_t i = 0; i < colors.size(); ++i)
	{
		if (static_cast<size_t>(s.end() - r.caret()) < 1) return false;
		const uint8_t flag = r.read<uint8_t>();
		if (flag & DROPPED_FLAG) return false;
		if (flag & SINGULAR_FLAG)
		{
			if (static_cast<size_t>(s.end() - r.caret()) < 1) return false;
			r.advance(1);
		}
		else if (egtb_table)
		{
			if (static_cast<size_t>(s.end() - r.caret()) < 31) return false;
			r.advance(29);
			const uint16_t num_ranks = r.read<uint16_t>();
			if (static_cast<size_t>(s.end() - r.caret()) < num_ranks * 2u) return false;
			r.advance(num_ranks * 2);
		}
		else
		{
			if (static_cast<size_t>(s.end() - r.caret()) < 26) return false;
			r.advance(26);
		}
	}
	return true;
}

struct EGTB_Paths
{
	static inline const std::string WDL_EXT = ".lzw";
	static inline const std::string DTC_EXT = ".lzdtc";
	static inline const std::string DTM_EXT = ".lzdtm";
	static inline const std::string DTM50_EXT = ".lzdtm50";
	static inline const std::string INFO_EXT = ".info";
	static inline const std::string DTC_SLICE_EXT[COLOR_NB] = { ".w.dtcs", ".b.dtcs" };
	static inline const std::string DTM_SLICE_EXT[COLOR_NB] = { ".w.dtms", ".b.dtms" };
	static inline const std::string DTM50_SLICE_EXT[COLOR_NB] = { ".w.dtm50s", ".b.dtm50s" };
	static inline const std::string DTC_CKPT_EXT = ".dtc.ckpt";
	static inline const std::string DTM_CKPT_EXT = ".dtm.ckpt";
	static inline const std::string DTM50_CKPT_EXT = ".dtm50.ckpt";
	static inline const std::string DTM_SUB_FLAT_EXT[COLOR_NB] = { ".dtm.subflat.w", ".dtm.subflat.b" };
	static inline const std::string DTM50_SUB_FLAT_EXT[COLOR_NB] = { ".dtm50.subflat.w", ".dtm50.subflat.b" };

	EGTB_Paths() = default;

	void add_dtc_path(std::filesystem::path s) { m_dtc_paths.emplace_back(std::move(s)); }
	void add_wdl_path(std::filesystem::path s) { m_wdl_paths.emplace_back(std::move(s)); }
	void add_dtm_path(std::filesystem::path s) { m_dtm_paths.emplace_back(std::move(s)); }
	void add_dtm50_path(std::filesystem::path s) { m_dtm50_paths.emplace_back(std::move(s)); }
	void set_tmp_path(std::filesystem::path s) { m_tmp_path = std::move(s); }

	void init_directories() const
	{
		std::filesystem::create_directories(m_tmp_path);
		for (const auto& p : m_wdl_paths) std::filesystem::create_directories(p);
		for (const auto& p : m_dtc_paths) std::filesystem::create_directories(p);
		for (const auto& p : m_dtm_paths) std::filesystem::create_directories(p);
		for (const auto& p : m_dtm50_paths) std::filesystem::create_directories(p);
	}

	NODISCARD bool find_wdl_file(const Piece_Config& ps, std::filesystem::path* tb = nullptr) const
	{
		std::filesystem::path found;
		if (!find_tb_file(ps, WDL_EXT, m_wdl_paths, &found)) return false;
		if (!tb_file_is_full_format(found, EGTB_Magic::WDL_MAGIC)) return false;
		if (tb) *tb = std::move(found);
		return true;
	}
	NODISCARD bool find_dtc_file(const Piece_Config& ps, std::filesystem::path* tb = nullptr) const
	{
		std::filesystem::path found;
		if (!find_tb_file(ps, DTC_EXT, m_dtc_paths, &found)) return false;
		if (!tb_file_is_full_format(found, EGTB_Magic::DTC_MAGIC)) return false;
		if (tb) *tb = std::move(found);
		return true;
	}
	NODISCARD bool find_dtm_file(const Piece_Config& ps, std::filesystem::path* tb = nullptr) const
	{
		std::filesystem::path found;
		if (!find_tb_file(ps, DTM_EXT, m_dtm_paths, &found)) return false;
		if (!tb_file_is_full_format(found, EGTB_Magic::DTM_MAGIC)) return false;
		if (tb) *tb = std::move(found);
		return true;
	}
	// dtm50/<name>.lzdtm50 — one packed file per material, all 100 hmc layers.
	NODISCARD bool find_dtm50_file(const Piece_Config& ps, std::filesystem::path* tb = nullptr) const
	{
		std::filesystem::path found;
		if (!find_tb_file(ps, DTM50_EXT, m_dtm50_paths, &found)) return false;
		if (!tb_file_is_full_format(found, EGTB_Magic::DTM50_MAGIC)) return false;
		if (tb) *tb = std::move(found);
		return true;
	}

	NODISCARD std::filesystem::path wdl_save_path(const Piece_Config& ps) const { return path_join(m_wdl_paths[0], ps.name() + WDL_EXT); }
	NODISCARD std::filesystem::path dtc_save_path(const Piece_Config& ps) const { return path_join(m_dtc_paths[0], ps.name() + DTC_EXT); }
	NODISCARD std::filesystem::path dtc_info_save_path(const Piece_Config& ps) const { return path_join(m_dtc_paths[0], ps.name() + INFO_EXT); }
	NODISCARD std::filesystem::path dtm_save_path(const Piece_Config& ps) const { return path_join(m_dtm_paths[0], ps.name() + DTM_EXT); }
	NODISCARD std::filesystem::path dtm_info_save_path(const Piece_Config& ps) const { return path_join(m_dtm_paths[0], ps.name() + INFO_EXT); }
	NODISCARD std::filesystem::path dtm50_save_path(const Piece_Config& ps) const
	{
		return path_join(m_dtm50_paths[0], ps.name() + DTM50_EXT);
	}
	NODISCARD std::filesystem::path dtm50_info_save_path(const Piece_Config& ps) const
	{
		return path_join(m_dtm50_paths[0], ps.name() + INFO_EXT);
	}

	NODISCARD std::filesystem::path dtc_slice_save_path(const Piece_Config& ps, Color c) const
	{
		return path_join(m_dtc_paths[0], ps.name() + DTC_SLICE_EXT[c]);
	}
	NODISCARD bool find_dtc_slice_file(const Piece_Config& ps, Color c, std::filesystem::path* tb = nullptr) const
	{
		return find_tb_file(ps, DTC_SLICE_EXT[c], m_dtc_paths, tb);
	}
	NODISCARD std::filesystem::path dtm_slice_save_path(const Piece_Config& ps, Color c) const
	{
		return path_join(m_dtm_paths[0], ps.name() + DTM_SLICE_EXT[c]);
	}

	NODISCARD std::filesystem::path dtc_checkpoint_path(const Piece_Config& ps) const
	{
		return path_join(m_tmp_path, ps.name() + DTC_CKPT_EXT);
	}
	NODISCARD std::filesystem::path dtm_checkpoint_path(const Piece_Config& ps) const
	{
		return path_join(m_tmp_path, ps.name() + DTM_CKPT_EXT);
	}
	NODISCARD std::filesystem::path dtm50_checkpoint_path(const Piece_Config& ps) const
	{
		return path_join(m_tmp_path, ps.name() + DTM50_CKPT_EXT);
	}
	NODISCARD std::filesystem::path dtm_sub_flat_path(const Piece_Config& ps, Color c) const
	{
		return path_join(m_tmp_path, ps.name() + DTM_SUB_FLAT_EXT[c]);
	}
	NODISCARD std::filesystem::path dtm50_sub_flat_path(const Piece_Config& ps, Color c) const
	{
		return path_join(m_tmp_path, ps.name() + DTM50_SUB_FLAT_EXT[c]);
	}

private:
	std::filesystem::path m_tmp_path = "./tmp/";
	std::vector<std::filesystem::path> m_dtc_paths = { "./dtc/" };
	std::vector<std::filesystem::path> m_wdl_paths = { "./wdl/" };
	std::vector<std::filesystem::path> m_dtm_paths = { "./dtm/" };
	std::vector<std::filesystem::path> m_dtm50_paths = { "./dtm50/" };

	NODISCARD bool find_tb_file(
		const Piece_Config& ps,
		const std::string& ext,
		const std::vector<std::filesystem::path>& paths,
		std::filesystem::path* tb = nullptr) const
	{
		const std::string name = ps.name() + ext;
		for (const auto& dir : paths)
		{
			const auto path = path_join(dir, name);
			if (std::filesystem::exists(path))
			{
				if (tb != nullptr) *tb = path;
				return true;
			}
		}
		return false;
	}
};

struct WDL_File_For_Probe;

void load_wdl_table(
	Out_Param<WDL_File_For_Probe> wdl,
	const Piece_Config& ps,
	std::filesystem::path sub_wdl,
	EGTB_Magic wdl_magic);

struct WDL_File_For_Probe
{
	struct Per_Color
	{
		Index_Permutation_Plan plan;
		size_t block_size = 0;
		size_t tail_size = 0;
		size_t block_cnt = 0;
		Mono_Uint_Vec offsets;          // (block_cnt + 1) cumulative offsets
		const uint8_t* compressed_data = nullptr;
		LZ4_Dict dict;
	};

	struct Per_Thread_Color_Cache
	{
		static constexpr size_t CAP = 32;
		size_t block_id[CAP];
		std::vector<uint8_t> data[CAP];
		size_t next_slot = 0;  // FIFO head: next slot to evict/fill (oldest).
		size_t live = 0;
		std::unique_ptr<LZ4_Decompress_Helper> decomp;
		Per_Thread_Color_Cache()
		{
			for (size_t i = 0; i < CAP; ++i) block_id[i] = SIZE_MAX;
		}
	};

	WDL_File_For_Probe() :
		m_is_singular{ false, false },
		m_single_val{ WDL_Entry::DRAW, WDL_Entry::DRAW }
	{}

	WDL_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool) :
		WDL_File_For_Probe()
	{
		open(egtb_files, ps, thread_pool);
	}

	WDL_File_For_Probe(const WDL_File_For_Probe&) = delete;
	WDL_File_For_Probe(WDL_File_For_Probe&&) noexcept = default;
	WDL_File_For_Probe& operator=(const WDL_File_For_Probe&) = delete;
	WDL_File_For_Probe& operator=(WDL_File_For_Probe&&) noexcept = default;
	~WDL_File_For_Probe() { close(); }

	// Pool is held only to size per-thread caches; the load itself is
	// sequential (block-cached, no eager decode).
	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		std::filesystem::path path;
		if (!egtb_files.find_wdl_file(ps, &path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name());

		m_caches.clear();
		m_caches.resize(std::max<size_t>(1, thread_pool->num_workers()));
		load_wdl_table(out_param(*this), ps, path, EGTB_Magic::WDL_MAGIC);
	}

	void close()
	{
		m_lzw_file.close();
		for (Color c : { WHITE, BLACK })
		{
			m_per_color[c] = Per_Color{};
			m_is_singular[c] = false;
			m_single_val[c] = WDL_Entry::DRAW;
		}
		m_caches.clear();
	}

	NODISCARD WDL_Entry read(Color color, Board_Index pos, size_t thread_id) const
	{
		if (m_is_singular[color]) return m_single_val[color];
		const auto& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t packed_byte = storage_pos / WDL_ENTRY_PACK_RATIO;
		ASSERT(pc.block_size > 0);
		const size_t block_id = packed_byte / pc.block_size;
		const size_t in_block = packed_byte % pc.block_size;

		// Equal-offsets sentinel marks an all-ILLEGAL block: save_wdl_table
		// emits no payload bytes, so we must short-circuit before LZ4.
		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return WDL_Entry::ILLEGAL;

		ASSERT(thread_id < m_caches.size());
		auto& cache = m_caches[thread_id][color];
		const uint8_t* decompressed_block = get_block(pc, cache, block_id);

		Packed_WDL_Entries entry;
		std::memcpy(&entry, decompressed_block + in_block, sizeof(Packed_WDL_Entries));
		return wdl_from_storage(get_wdl_value(entry, storage_pos % WDL_ENTRY_PACK_RATIO));
	}

	bool m_is_singular[COLOR_NB];
	WDL_Entry m_single_val[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzw_file;
	mutable std::vector<std::array<Per_Thread_Color_Cache, COLOR_NB>> m_caches;

private:
	NODISCARD const uint8_t* get_block(
		const Per_Color& pc, Per_Thread_Color_Cache& cache, size_t block_id) const
	{
		for (size_t i = 0; i < cache.live; ++i)
		{
			if (cache.block_id[i] == block_id)
				return cache.data[i].data();
		}

		size_t slot;
		if (cache.live < Per_Thread_Color_Cache::CAP)
		{
			slot = cache.live++;
		}
		else
		{
			slot = cache.next_slot;
			cache.next_slot = (cache.next_slot + 1) % Per_Thread_Color_Cache::CAP;
		}

		if (!cache.decomp)
			cache.decomp = std::make_unique<LZ4_Decompress_Helper>(pc.dict, pc.block_size);

		const auto pair = pc.offsets.get2(block_id);
		const size_t doff = pair[0];
		const size_t dsz  = pair[1] - pair[0];
		const size_t out_sz =
			(block_id == pc.block_cnt - 1 && pc.tail_size != 0)
			? pc.tail_size
			: pc.block_size;
		const auto decompressed = cache.decomp->decompress(
			Const_Span<uint8_t>(pc.compressed_data + doff, dsz),
			out_sz);

		cache.data[slot].assign(decompressed.begin(), decompressed.end());
		cache.block_id[slot] = block_id;
		return cache.data[slot].data();
	}
};

// =============================================================================
// DTM_File_For_Probe: sub-tablebase reader used by the DTM generator. File
// format matches save_egtb_table (same as DTC), but values decode to DTM_Final_Entry
// — DTM has no zeroing, so reading a sub-TB after a capture/promotion yields
// the child's exact ply-to-mate, not just WDL.
// =============================================================================

struct DTM_File_For_Probe;

void load_dtm_table(
	Out_Param<DTM_File_For_Probe> dtm,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm,
	EGTB_Magic dtm_magic);

struct DTM_File_For_Probe
{
	struct Per_Color
	{
		Index_Permutation_Plan plan;
		size_t entry_bytes = 0;            // on-disk rank-index width: 1 or 2 bytes (storage is parity-halved in both)
		size_t block_size = 0;             // on-disk rank bytes per full block
		size_t tail_size = 0;
		size_t block_cnt = 0;
		Mono_Uint_Vec offsets;             // (block_cnt + 1) cumulative offsets
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;  // rank -> raw storage value
	};

	struct Per_Thread_Color_Cache
	{
		static constexpr size_t CAP = 64;
		size_t block_id[CAP];
		// Decoded buffers hold 2-byte storage values regardless of the on-disk
		// rank-index width; the 1B tier widens at decode time via r2v[raw[k]].
		std::vector<uint8_t> data[CAP];
		size_t next_slot = 0;  // FIFO head: next slot to evict/fill (oldest).
		size_t live = 0;
		std::unique_ptr<LZMA_Decompress_Helper> decomp;
		Per_Thread_Color_Cache()
		{
			for (size_t i = 0; i < CAP; ++i) block_id[i] = SIZE_MAX;
		}
	};

	DTM_File_For_Probe() :
		m_is_singular{ false, false },
		m_single_val{ WDL_Entry::DRAW, WDL_Entry::DRAW }
	{}

	DTM_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		size_t num_threads) :
		DTM_File_For_Probe()
	{
		open(egtb_files, ps, num_threads);
	}

	DTM_File_For_Probe(const DTM_File_For_Probe&) = delete;
	DTM_File_For_Probe(DTM_File_For_Probe&&) noexcept = default;
	DTM_File_For_Probe& operator=(const DTM_File_For_Probe&) = delete;
	DTM_File_For_Probe& operator=(DTM_File_For_Probe&&) noexcept = default;
	~DTM_File_For_Probe() { close(); }

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		size_t num_threads)
	{
		std::filesystem::path dtm_path;
		if (!egtb_files.find_dtm_file(ps, &dtm_path))
			throw std::runtime_error("Could not find a DTM file for " + ps.name());
		// DTM gen runs after DTC ('--dtm' is additive), so the WDL companion
		// for class info is guaranteed to be present.
		std::filesystem::path wdl_path;
		if (!egtb_files.find_wdl_file(ps, &wdl_path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name()
				+ " (DTM consumes DTC's wdl/ output)");

		m_caches.clear();
		m_caches.resize(std::max<size_t>(1, num_threads));

		m_wdl.m_caches.clear();
		m_wdl.m_caches.resize(std::max<size_t>(1, num_threads));
		load_wdl_table(out_param(m_wdl), ps, wdl_path, EGTB_Magic::WDL_MAGIC);

		load_dtm_table(out_param(*this), ps, dtm_path, EGTB_Magic::DTM_MAGIC);
	}

	void close()
	{
		m_lzdtm_file.close();
		m_wdl.close();
		for (Color c : { WHITE, BLACK })
		{
			m_per_color[c] = Per_Color{};
			m_is_singular[c] = false;
			m_single_val[c] = WDL_Entry::DRAW;
		}
		m_caches.clear();
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos, size_t thread_id) const
	{
		const WDL_Entry w = m_wdl.read(color, pos, thread_id);
		if (w == WDL_Entry::ILLEGAL) return DTM_Final_Entry::make_illegal();
		if (w == WDL_Entry::DRAW)    return DTM_Final_Entry::make_draw();

		// WIN/LOSE classes (incl. cursed in DTC's WDL — folded here): need a
		// ply value from the .lzdtm payload. A singular table here implies a
		// WDL/DTM mismatch (singular DTM is always DRAW); treat as ILLEGAL.
		if (m_is_singular[color])
			return DTM_Final_Entry::make_illegal();

		const Per_Color& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t positions_per_block = pc.block_size / pc.entry_bytes;
		const size_t block_id    = storage_pos / positions_per_block;
		const size_t in_block_pos = storage_pos % positions_per_block;

		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return DTM_Final_Entry::make_illegal();

		ASSERT(thread_id < m_caches.size());
		auto& cache = m_caches[thread_id][color];
		const uint8_t* decompressed = get_block(pc, cache, block_id);

		uint16_t stored;
		std::memcpy(&stored, decompressed + in_block_pos * sizeof(uint16_t), sizeof(stored));
		return dtm_entry_from_storage(stored, w);
	}

	bool m_is_singular[COLOR_NB];
	WDL_Entry m_single_val[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzdtm_file;
	WDL_File_For_Probe m_wdl;
	mutable std::vector<std::array<Per_Thread_Color_Cache, COLOR_NB>> m_caches;

private:
	NODISCARD const uint8_t* get_block(
		const Per_Color& pc, Per_Thread_Color_Cache& cache, size_t block_id) const
	{
		for (size_t i = 0; i < cache.live; ++i)
		{
			if (cache.block_id[i] == block_id)
				return cache.data[i].data();
		}

		size_t slot;
		if (cache.live < Per_Thread_Color_Cache::CAP)
		{
			slot = cache.live++;
		}
		else
		{
			slot = cache.next_slot;
			cache.next_slot = (cache.next_slot + 1) % Per_Thread_Color_Cache::CAP;
		}

		const size_t decode_sz =
			(block_id == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
		const size_t positions = decode_sz / pc.entry_bytes;

		const auto pair = pc.offsets.get2(block_id);
		const size_t doff = pair[0];
		const size_t dsz  = pair[1] - pair[0];

		auto& buf = cache.data[slot];
		buf.assign(positions * sizeof(uint16_t), 0);

		if (dsz != 0)
		{
			if (!cache.decomp)
				cache.decomp = std::make_unique<LZMA_Decompress_Helper>(pc.block_size);
			const Const_Span<uint8_t> raw = cache.decomp->decompress(
				Const_Span<uint8_t>(pc.compressed_data + doff, dsz), decode_sz);

			const auto& r2v = pc.rank_to_value;
			uint16_t* out = reinterpret_cast<uint16_t*>(buf.data());
			if (pc.entry_bytes == 1)
			{
				// 1B rank-index tier: raw byte indexes into rank_to_value; the
				// half-bit dropped at encode is recovered via WDL class at read().
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

		cache.block_id[slot] = block_id;
		return buf.data();
	}
};

// =============================================================================
// DTM_Sub_File_Flat: alternative sub-DTM reader used by the DTM generator. On
// open(), decompresses the full sub-table once into a per-color tmp file and
// memory-maps it; read() is a flat indexed memcpy of a DTM_Final_Entry. Trades
// extra disk + RAM (page cache) at load for predictable O(1) reads with no
// LZMA, no block cache, no per-thread state. Use this when the generator's
// access pattern is random over a sub-TB (DTM init/iteration) and the block
// LRU in DTM_File_For_Probe would thrash.
// =============================================================================

struct DTM_Sub_File_Flat;

void load_dtm_sub_flat(
	Out_Param<DTM_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool);

struct DTM_Sub_File_Flat
{
	struct Per_Color
	{
		Memory_Mapped_File file;
		Index_Permutation_Plan plan;
	};

	DTM_Sub_File_Flat() = default;

	DTM_Sub_File_Flat(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		open(egtb_files, ps, thread_pool);
	}

	DTM_Sub_File_Flat(const DTM_Sub_File_Flat&) = delete;
	DTM_Sub_File_Flat(DTM_Sub_File_Flat&&) noexcept = default;
	DTM_Sub_File_Flat& operator=(const DTM_Sub_File_Flat&) = delete;
	DTM_Sub_File_Flat& operator=(DTM_Sub_File_Flat&&) noexcept = default;
	~DTM_Sub_File_Flat() { close(); }

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		load_dtm_sub_flat(out_param(*this), egtb_files, ps, thread_pool);
	}

	void close()
	{
		for (Color c : { WHITE, BLACK })
			m_per_color[c].file.close();
		m_tmp_files.clear();
	}

	// thread_id is accepted (and ignored) so this reader is a drop-in for
	// DTM_File_For_Probe::read at the call site — swap the type, keep the call.
	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos, size_t /*thread_id*/ = 0) const
	{
		const auto& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		DTM_Final_Entry e;
		std::memcpy(&e,
		            pc.file.data() + storage_pos * sizeof(DTM_Final_Entry),
		            sizeof(DTM_Final_Entry));
		return e;
	}

	Per_Color m_per_color[COLOR_NB];
	Temporary_File_Tracker m_tmp_files;
};

// =============================================================================
// DTM50_File_For_Probe: lazy hmc=0 reader for DTM50's rs-pack. This mirrors
// DTM_File_For_Probe's block-cache shape, but only materializes the reset-clock
// column because sub-TB reads after captures/promotions/pawn moves always enter
// a child at hmc=0. DTM50_Sub_File_Flat is usually better for random-heavy
// generator access; this exists as a non-flat drop-in option.
// =============================================================================

struct DTM50_File_For_Probe;

void load_dtm50_table(
	Out_Param<DTM50_File_For_Probe> dtm50,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm50);

struct DTM50_File_For_Probe
{
	static constexpr size_t HMC_COUNT = 100;

	struct Per_Color
	{
		Index_Permutation_Plan plan;
		size_t entry_bytes = 0;       // rank-index width: 1 or 2 bytes
		size_t block_positions = 0;   // positions per full rs-pack block
		size_t tail_positions = 0;
		size_t block_cnt = 0;
		Mono_Uint_Vec offsets;        // (block_cnt + 1) cumulative offsets
		Min0_Uint_Vec usizes;         // uncompressed rs-pack payload sizes
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;
	};

	struct Per_Thread_Color_Cache
	{
		static constexpr size_t CAP = 64;
		size_t block_id[CAP];
		// Decoded hmc=0 storage values, widened to uint16_t independent of
		// rank-index width.
		std::vector<uint8_t> data[CAP];
		size_t next_slot = 0;
		size_t live = 0;
		std::unique_ptr<LZMA_Decompress_Helper> decomp;
		Per_Thread_Color_Cache()
		{
			for (size_t i = 0; i < CAP; ++i) block_id[i] = SIZE_MAX;
		}
	};

	DTM50_File_For_Probe() :
		m_is_singular{ false, false },
		m_single_val{ WDL_Entry::DRAW, WDL_Entry::DRAW }
	{}

	DTM50_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		size_t num_threads) :
		DTM50_File_For_Probe()
	{
		open(egtb_files, ps, num_threads);
	}

	DTM50_File_For_Probe(const DTM50_File_For_Probe&) = delete;
	DTM50_File_For_Probe(DTM50_File_For_Probe&&) noexcept = default;
	DTM50_File_For_Probe& operator=(const DTM50_File_For_Probe&) = delete;
	DTM50_File_For_Probe& operator=(DTM50_File_For_Probe&&) noexcept = default;
	~DTM50_File_For_Probe() { close(); }

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		size_t num_threads)
	{
		std::filesystem::path dtm50_path;
		if (!egtb_files.find_dtm50_file(ps, &dtm50_path))
			throw std::runtime_error("Could not find a DTM50 file for " + ps.name());
		std::filesystem::path wdl_path;
		if (!egtb_files.find_wdl_file(ps, &wdl_path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name()
				+ " (DTM50 consumes DTC's wdl/ output)");

		m_caches.clear();
		m_caches.resize(std::max<size_t>(1, num_threads));

		m_wdl.m_caches.clear();
		m_wdl.m_caches.resize(std::max<size_t>(1, num_threads));
		load_wdl_table(out_param(m_wdl), ps, wdl_path, EGTB_Magic::WDL_MAGIC);

		load_dtm50_table(out_param(*this), ps, dtm50_path);
	}

	void close()
	{
		m_lzdtm50_file.close();
		m_wdl.close();
		for (Color c : { WHITE, BLACK })
		{
			m_per_color[c] = Per_Color{};
			m_is_singular[c] = false;
			m_single_val[c] = WDL_Entry::DRAW;
		}
		m_caches.clear();
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos, size_t thread_id) const
	{
		const WDL_Entry w = m_wdl.read(color, pos, thread_id);
		if (w == WDL_Entry::ILLEGAL) return DTM_Final_Entry::make_illegal();
		if (w == WDL_Entry::DRAW
		    || w == WDL_Entry::CURSED_WIN
		    || w == WDL_Entry::BLESSED_LOSS)
			return DTM_Final_Entry::make_draw();

		if (m_is_singular[color])
			return DTM_Final_Entry::make_illegal();

		const Per_Color& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t block_id = storage_pos / pc.block_positions;
		const size_t pos_in_block = storage_pos % pc.block_positions;

		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return DTM_Final_Entry::make_illegal();

		ASSERT(thread_id < m_caches.size());
		auto& cache = m_caches[thread_id][color];
		const uint8_t* decoded = get_block(pc, cache, block_id);

		uint16_t stored;
		std::memcpy(&stored, decoded + pos_in_block * sizeof(uint16_t), sizeof(stored));
		return dtm50_entry_from_storage(stored, w);
	}

	bool m_is_singular[COLOR_NB];
	WDL_Entry m_single_val[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzdtm50_file;
	WDL_File_For_Probe m_wdl;
	mutable std::vector<std::array<Per_Thread_Color_Cache, COLOR_NB>> m_caches;

private:
	NODISCARD const uint8_t* get_block(
		const Per_Color& pc, Per_Thread_Color_Cache& cache, size_t block_id) const
	{
		for (size_t i = 0; i < cache.live; ++i)
			if (cache.block_id[i] == block_id)
				return cache.data[i].data();

		size_t slot;
		if (cache.live < Per_Thread_Color_Cache::CAP)
		{
			slot = cache.live++;
		}
		else
		{
			slot = cache.next_slot;
			cache.next_slot = (cache.next_slot + 1) % Per_Thread_Color_Cache::CAP;
		}

		const size_t positions =
			(block_id == pc.block_cnt - 1 && pc.tail_positions != 0)
			? pc.tail_positions
			: pc.block_positions;

		auto& buf = cache.data[slot];
		buf.assign(positions * sizeof(uint16_t), 0);

		const auto pair = pc.offsets.get2(block_id);
		const size_t doff = pair[0];
		const size_t dsz  = pair[1] - pair[0];
		if (dsz != 0)
		{
			if (!cache.decomp)
			{
				const size_t ppb = pc.block_positions;
				const size_t eb = pc.entry_bytes;
				const size_t max_payload =
					24
					+ (ppb * 2 + 7) / 8
					+ ppb * eb
					+ (ppb + 7) / 8 + ppb * (1 + 2 * eb)
					+ (ppb + 7) / 8 + ppb * (2 + 3 * eb)
					+ (ppb + 1) * 4 + ppb * (1 + 16 + HMC_COUNT * eb)
					+ 3;
				cache.decomp = std::make_unique<LZMA_Decompress_Helper>(max_payload);
			}

			const size_t usz = pc.usizes.get(block_id);
			const Const_Span<uint8_t> raw = cache.decomp->decompress(
				Const_Span<uint8_t>(pc.compressed_data + doff, dsz), usz);

			const uint8_t* p = raw.data();
			uint32_t np32, num_single, num_double, num_multi, ss_bytes32, ds_bytes32;
			std::memcpy(&np32,       p,      4);
			std::memcpy(&num_single, p + 4,  4);
			std::memcpy(&num_double, p + 8,  4);
			std::memcpy(&num_multi,  p + 12, 4);
			std::memcpy(&ss_bytes32, p + 16, 4);
			std::memcpy(&ds_bytes32, p + 20, 4);
			ASSERT(np32 == positions);

			const size_t eb = pc.entry_bytes;
			const size_t num_const = np32 - num_single - num_double - num_multi;
			const size_t sb_bytes = (np32 * 2 + 7) / 8;
			const size_t sh_bytes = (num_single + 7) / 8;
			const size_t dh_bytes = (num_double + 7) / 8;
			p += 24;

			const uint8_t* state_bits    = p; p += sb_bytes;
			const uint8_t* const_stream  = p; p += num_const * eb;
			const uint8_t* single_hints  = p; p += sh_bytes;
			const uint8_t* single_stream = p; p += ss_bytes32;
			const uint8_t* double_hints  = p; p += dh_bytes;
			const uint8_t* double_stream = p; p += ds_bytes32;
			p += (4 - ((p - raw.data()) & 3)) & 3;
			const uint32_t* multi_dir = reinterpret_cast<const uint32_t*>(p);
			p += (num_multi + 1) * 4;
			const uint8_t* multi_data = p;

			const size_t single_short = 1 + eb;
			const size_t single_long  = 1 + 2 * eb;
			const size_t double_short = 2 + 2 * eb;
			const size_t double_long  = 2 + 3 * eb;
			size_t const_idx = 0, single_idx = 0, double_idx = 0, multi_idx = 0;
			size_t single_off = 0, double_off = 0;

			const auto& r2v = pc.rank_to_value;
			uint16_t* out = reinterpret_cast<uint16_t*>(buf.data());
			for (size_t k = 0; k < positions; ++k)
			{
				const size_t bit_off = k * 2;
				const uint8_t state = (state_bits[bit_off / 8] >> (bit_off % 8)) & 3u;
				uint16_t rank;
				switch (state)
				{
					case 0: {
						if (eb == 1) rank = const_stream[const_idx];
						else { uint16_t r; std::memcpy(&r, const_stream + const_idx * 2, 2); rank = r; }
						++const_idx;
						break;
					}
					case 1: {
						const uint8_t* entry = single_stream + single_off;
						if (eb == 1) rank = entry[1];
						else { uint16_t r; std::memcpy(&r, entry + 1, 2); rank = r; }
						const bool draw_end =
							(single_hints[single_idx >> 3] >> (single_idx & 7)) & 1u;
						single_off += draw_end ? single_short : single_long;
						++single_idx;
						break;
					}
					case 2: {
						const uint8_t* entry = double_stream + double_off;
						if (eb == 1) rank = entry[2];
						else { uint16_t r; std::memcpy(&r, entry + 2, 2); rank = r; }
						const bool draw_end =
							(double_hints[double_idx >> 3] >> (double_idx & 7)) & 1u;
						double_off += draw_end ? double_short : double_long;
						++double_idx;
						break;
					}
					default: {
						const uint8_t* entry = multi_data + multi_dir[multi_idx];
						if (eb == 1) rank = entry[17];
						else { uint16_t r; std::memcpy(&r, entry + 17, 2); rank = r; }
						++multi_idx;
						break;
					}
				}
				out[k] = r2v[rank];
			}
			ASSERT(const_idx == num_const && single_idx == num_single
			    && double_idx == num_double && multi_idx == num_multi);
		}

		cache.block_id[slot] = block_id;
		return buf.data();
	}
};

// =============================================================================
// DTM50_Sub_File_Flat: layer-0 sub-table reader for DTM50 — same shape as
// DTM_Sub_File_Flat but reads dtm50/<name>.lzdtm50 and decodes with
// dtm50_entry_from_storage (cursed/blessed → DRAW).
// =============================================================================

struct DTM50_Sub_File_Flat;

void load_dtm50_sub_flat(
	Out_Param<DTM50_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool);

struct DTM50_Sub_File_Flat
{
	struct Per_Color
	{
		Memory_Mapped_File file;
		Index_Permutation_Plan plan;
	};

	DTM50_Sub_File_Flat() = default;

	DTM50_Sub_File_Flat(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		open(egtb_files, ps, thread_pool);
	}

	DTM50_Sub_File_Flat(const DTM50_Sub_File_Flat&) = delete;
	DTM50_Sub_File_Flat(DTM50_Sub_File_Flat&&) noexcept = default;
	DTM50_Sub_File_Flat& operator=(const DTM50_Sub_File_Flat&) = delete;
	DTM50_Sub_File_Flat& operator=(DTM50_Sub_File_Flat&&) noexcept = default;
	~DTM50_Sub_File_Flat() { close(); }

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		load_dtm50_sub_flat(out_param(*this), egtb_files, ps, thread_pool);
	}

	void close()
	{
		for (Color c : { WHITE, BLACK })
			m_per_color[c].file.close();
		m_tmp_files.clear();
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos, size_t /*thread_id*/ = 0) const
	{
		const auto& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		DTM_Final_Entry e;
		std::memcpy(&e,
		            pc.file.data() + storage_pos * sizeof(DTM_Final_Entry),
		            sizeof(DTM_Final_Entry));
		return e;
	}

	Per_Color m_per_color[COLOR_NB];
	Temporary_File_Tracker m_tmp_files;
};

struct EGTB_Info
{
	EGTB_Info() { clear(); }
	void clear() { std::memset(this, 0, sizeof(EGTB_Info)); }

	void maybe_update_longest_win(Color color, size_t idx, size_t value)
	{
		if (value > longest_win[color])
		{
			longest_win[color] = static_cast<uint16_t>(value);
			longest_idx[color] = idx;
		}
	}

	void add_result(Color color, WDL_Entry value, uint64_t weight = 1)
	{
		// CURSED_WIN/BLESSED_LOSS collapse into win/lose counts so the singular
		// fast-path in egtb_compress (win+draw==0 etc.) keeps working.
		switch (value)
		{
			case WDL_Entry::DRAW:         draw_cnt[color]    += weight; break;
			case WDL_Entry::LOSE:         lose_cnt[color]    += weight; break;
			case WDL_Entry::BLESSED_LOSS: lose_cnt[color]    += weight; break;
			case WDL_Entry::WIN:          win_cnt[color]     += weight; break;
			case WDL_Entry::CURSED_WIN:   win_cnt[color]     += weight; break;
			case WDL_Entry::ILLEGAL:      illegal_cnt[color] += weight; break;
		}
	}

	template <typename IterT>
	void consolidate_from(IterT begin, IterT end, Color color)
	{
		while (begin != end)
		{
			const auto& info = *begin;
			win_cnt[color]     += info.win_cnt[color];
			draw_cnt[color]    += info.draw_cnt[color];
			lose_cnt[color]    += info.lose_cnt[color];
			illegal_cnt[color] += info.illegal_cnt[color];
			if (longest_win[color] < info.longest_win[color]
			    || (longest_win[color] == info.longest_win[color]
			        && longest_idx[color] > info.longest_idx[color]))
			{
				longest_win[color] = info.longest_win[color];
				longest_idx[color] = info.longest_idx[color];
			}
			++begin;
		}
	}

	uint64_t win_cnt[COLOR_NB];
	uint64_t lose_cnt[COLOR_NB];
	uint64_t draw_cnt[COLOR_NB];
	uint64_t illegal_cnt[COLOR_NB];
	uint16_t longest_win[COLOR_NB];

	char longest_fen[COLOR_NB][MAX_FEN_LENGTH];
	uint64_t longest_idx[COLOR_NB];
};
