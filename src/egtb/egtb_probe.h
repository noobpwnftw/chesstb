#pragma once

#include "chess/chess.h"
#include "chess/piece_config.h"

#include "egtb/egtb_entry.h"
#include "egtb/egtb_format.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/index_permutation_plan.h"

#include "util/cache.h"
#include "util/compress.h"
#include "util/defines.h"
#include "util/memory.h"
#include "util/mono_uint_vec.h"

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
	// EGTB tables carry an inline rank table in their per-color header; WDL doesn't.
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
	static inline const std::string DTC_CKPT_EXT = ".dtc.ckpt";
	static inline const std::string DTM_CKPT_EXT = ".dtm.ckpt";
	static inline const std::string DTM50_CKPT_EXT = ".dtm50.ckpt";
	static inline const std::string DTM_SUB_FLAT_EXT[COLOR_NB] = { ".dtm.subflat.w", ".dtm.subflat.b" };
	static inline const std::string DTM50_SUB_FLAT_EXT[COLOR_NB] = { ".dtm50.subflat.w", ".dtm50.subflat.b" };
	static inline const std::string BLOCK_SPILL_EXT[COLOR_NB] = { ".blocks.w", ".blocks.b" };

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
	NODISCARD std::filesystem::path block_spill_path(const Piece_Config& ps, Color c) const
	{
		return path_join(m_tmp_path, ps.name() + BLOCK_SPILL_EXT[c]);
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

// Abstract sub-table reader. Implemented by the *_File_For_Probe (block-cache)
// and *_Sub_File_Flat (mmap'd flat) readers; a generator holds the base pointer
// so the strategy is runtime-selectable (--probe).
template <typename EntryT>
struct EGTB_Sub_Reader
{
	using Entry_Type = EntryT;
	virtual ~EGTB_Sub_Reader() = default;
	NODISCARD virtual EntryT read(Color color, Board_Index pos) const = 0;
};

struct WDL_File_For_Probe;

void load_wdl_table(
	Out_Param<WDL_File_For_Probe> wdl,
	const Piece_Config& ps,
	std::filesystem::path sub_wdl,
	EGTB_Magic wdl_magic);

struct WDL_File_For_Probe : public EGTB_Sub_Reader<WDL_Entry>
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

	WDL_File_For_Probe() :
		m_is_singular{ false, false },
		m_single_val{ 0, 0 }
	{}

	WDL_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool>) :
		WDL_File_For_Probe()
	{
		open(egtb_files, ps);
	}

	WDL_File_For_Probe(const WDL_File_For_Probe&) = delete;
	WDL_File_For_Probe(WDL_File_For_Probe&&) = delete;
	WDL_File_For_Probe& operator=(const WDL_File_For_Probe&) = delete;
	WDL_File_For_Probe& operator=(WDL_File_For_Probe&&) = delete;

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps)
	{
		std::filesystem::path path;
		if (!egtb_files.find_wdl_file(ps, &path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name());

		load_wdl_table(out_param(*this), ps, path, EGTB_Magic::WDL_MAGIC);
	}

	NODISCARD WDL_Entry read(Color color, Board_Index pos) const override
	{
		if (m_is_singular[color]) return static_cast<WDL_Entry>(m_single_val[color]);
		const auto& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t packed_byte = storage_pos / WDL_ENTRY_PACK_RATIO;
		ASSERT(pc.block_size > 0);
		const size_t block_id = packed_byte / pc.block_size;
		const size_t in_block = packed_byte % pc.block_size;

		// Equal-offsets sentinel = all-ILLEGAL block (no payload); skip LZ4.
		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return WDL_Entry::ILLEGAL;

		const uint8_t* decompressed_block =
			fetch_block_cached(m_cache[color], block_id, [&pc](
				Block_Cache<LZ4_Decompress_Helper>& cache, size_t bid) -> Block_Ptr
			{
				LZ4_Decompress_Helper& dc = cache.decomp_for(
					[&] { return std::make_unique<LZ4_Decompress_Helper>(pc.dict, pc.block_size); });
				const auto pr = pc.offsets.get2(bid);
				const size_t doff = pr[0];
				const size_t dsz  = pr[1] - pr[0];
				const size_t out_sz =
					(bid == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
				const auto decompressed = dc.decompress(
					Const_Span<uint8_t>(pc.compressed_data + doff, dsz), out_sz);
				return std::make_shared<const std::vector<uint8_t>>(
					decompressed.begin(), decompressed.end());
			});

		Packed_WDL_Entries entry;
		std::memcpy(&entry, decompressed_block + in_block, sizeof(Packed_WDL_Entries));
		return wdl_from_storage(get_wdl_value(entry, storage_pos % WDL_ENTRY_PACK_RATIO));
	}

	bool m_is_singular[COLOR_NB];
	uint8_t m_single_val[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzw_file;
	mutable Block_Cache<LZ4_Decompress_Helper> m_cache[COLOR_NB];
};

// DTM_File_For_Probe: block-cache sub-DTM reader. Decodes to DTM_Final_Entry
// (exact ply-to-mate, since DTM has no zeroing).
struct DTM_File_For_Probe;

void load_dtm_table(
	Out_Param<DTM_File_For_Probe> dtm,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm,
	EGTB_Magic dtm_magic);

struct DTM_File_For_Probe : public EGTB_Sub_Reader<DTM_Final_Entry>
{
	struct Per_Color
	{
		Index_Permutation_Plan plan;
		size_t entry_bytes = 0;            // rank-index width: 1 or 2 bytes
		size_t block_size = 0;             // rank bytes per full block
		size_t tail_size = 0;
		size_t block_cnt = 0;
		Mono_Uint_Vec offsets;             // (block_cnt + 1) cumulative offsets
		const uint8_t* compressed_data = nullptr;
		std::vector<uint16_t> rank_to_value;  // rank -> storage value
	};

	DTM_File_For_Probe() :
		m_is_singular{ false, false },
		m_single_val{ 0, 0 }
	{}

	DTM_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool>) :
		DTM_File_For_Probe()
	{
		open(egtb_files, ps);
	}

	DTM_File_For_Probe(const DTM_File_For_Probe&) = delete;
	DTM_File_For_Probe(DTM_File_For_Probe&&) = delete;
	DTM_File_For_Probe& operator=(const DTM_File_For_Probe&) = delete;
	DTM_File_For_Probe& operator=(DTM_File_For_Probe&&) = delete;

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps)
	{
		std::filesystem::path dtm_path;
		if (!egtb_files.find_dtm_file(ps, &dtm_path))
			throw std::runtime_error("Could not find a DTM file for " + ps.name());
		// DTM runs after DTC, so the WDL companion is present.
		std::filesystem::path wdl_path;
		if (!egtb_files.find_wdl_file(ps, &wdl_path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name()
				+ " (DTM consumes DTC's wdl/ output)");

		load_wdl_table(out_param(m_wdl), ps, wdl_path, EGTB_Magic::WDL_MAGIC);

		load_dtm_table(out_param(*this), ps, dtm_path, EGTB_Magic::DTM_MAGIC);
	}

	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos) const override
	{
		const WDL_Entry w = m_wdl.read(color, pos);
		if (w == WDL_Entry::ILLEGAL) return DTM_Final_Entry::make_illegal();
		if (w == WDL_Entry::DRAW)    return DTM_Final_Entry::make_draw();

		if (m_is_singular[color])
			return dtm_entry_from_storage(m_single_val[color], w);

		const Per_Color& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t positions_per_block = pc.block_size / pc.entry_bytes;
		const size_t block_id    = storage_pos / positions_per_block;
		const size_t in_block_pos = storage_pos % positions_per_block;

		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return DTM_Final_Entry::make_illegal();

		const uint8_t* decompressed =
			fetch_block_cached(m_cache[color], block_id, [&pc](
				Block_Cache<LZMA_Decompress_Helper>& cache, size_t bid) -> Block_Ptr
			{
				const size_t decode_sz =
					(bid == pc.block_cnt - 1 && pc.tail_size != 0) ? pc.tail_size : pc.block_size;
				const size_t positions = decode_sz / pc.entry_bytes;

				const auto pr = pc.offsets.get2(bid);
				const size_t doff = pr[0];
				const size_t dsz  = pr[1] - pr[0];

				auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

				if (dsz != 0)
				{
					LZMA_Decompress_Helper& dc = cache.decomp_for(
						[&] { return std::make_unique<LZMA_Decompress_Helper>(pc.block_size); });
					const Const_Span<uint8_t> raw = dc.decompress(
						Const_Span<uint8_t>(pc.compressed_data + doff, dsz), decode_sz);

					const auto& r2v = pc.rank_to_value;
					uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
					if (pc.entry_bytes == 1)
					{
						// 1B tier: raw byte indexes r2v; half-bit recovered via WDL class.
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
				return buf;
			});

		uint16_t stored;
		std::memcpy(&stored, decompressed + in_block_pos * sizeof(uint16_t), sizeof(stored));
		return dtm_entry_from_storage(stored, w);
	}

	bool m_is_singular[COLOR_NB];
	uint8_t m_single_val[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzdtm_file;
	WDL_File_For_Probe m_wdl;
	mutable Block_Cache<LZMA_Decompress_Helper> m_cache[COLOR_NB];
};

// DTM_Sub_File_Flat: decompresses the sub-table to a per-color tmp file once and
// mmaps it; read() is a flat memcpy. Better than the block-cache reader for the
// generator's random access (init/iteration), at the cost of disk + page cache.
struct DTM_Sub_File_Flat;

void load_dtm_sub_flat(
	Out_Param<DTM_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool);

struct DTM_Sub_File_Flat : public EGTB_Sub_Reader<DTM_Final_Entry>
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
	~DTM_Sub_File_Flat()
	{
		for (Color c : { WHITE, BLACK })
			m_per_color[c].file.close();
		m_tmp_files.clear();
	}

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		load_dtm_sub_flat(out_param(*this), egtb_files, ps, thread_pool);
	}

	// drop-in for the block-cache reader's read().
	NODISCARD DTM_Final_Entry read(Color color, Board_Index pos) const override
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

// DTM50_File_For_Probe: block-cache reader materializing only DTM50's hmc=0
// column (sub-TB reads after cap/promo/pawn always enter a child at hmc=0).
struct DTM50_File_For_Probe;

void load_dtm50_table(
	Out_Param<DTM50_File_For_Probe> dtm50,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm50);

struct DTM50_File_For_Probe : public EGTB_Sub_Reader<DTM50_Final_Entry>
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

	DTM50_File_For_Probe() :
		m_is_singular{ false, false }
	{}

	DTM50_File_For_Probe(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool>) :
		DTM50_File_For_Probe()
	{
		open(egtb_files, ps);
	}

	DTM50_File_For_Probe(const DTM50_File_For_Probe&) = delete;
	DTM50_File_For_Probe(DTM50_File_For_Probe&&) = delete;
	DTM50_File_For_Probe& operator=(const DTM50_File_For_Probe&) = delete;
	DTM50_File_For_Probe& operator=(DTM50_File_For_Probe&&) = delete;

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps)
	{
		std::filesystem::path dtm50_path;
		if (!egtb_files.find_dtm50_file(ps, &dtm50_path))
			throw std::runtime_error("Could not find a DTM50 file for " + ps.name());
		std::filesystem::path wdl_path;
		if (!egtb_files.find_wdl_file(ps, &wdl_path))
			throw std::runtime_error("Could not find a WDL file for " + ps.name()
				+ " (DTM50 consumes DTC's wdl/ output)");

		load_wdl_table(out_param(m_wdl), ps, wdl_path, EGTB_Magic::WDL_MAGIC);

		load_dtm50_table(out_param(*this), ps, dtm50_path);
	}

	NODISCARD DTM50_Final_Entry read(Color color, Board_Index pos) const override
	{
		const WDL_Entry w = m_wdl.read(color, pos);
		if (w == WDL_Entry::ILLEGAL) return DTM50_Final_Entry::make_illegal();
		if (w == WDL_Entry::DRAW
		    || w == WDL_Entry::CURSED_WIN
		    || w == WDL_Entry::BLESSED_LOSS)
			return DTM50_Final_Entry::make_draw();

		if (m_is_singular[color])
			return DTM50_Final_Entry::make_draw();

		const Per_Color& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		const size_t block_id = storage_pos / pc.block_positions;
		const size_t pos_in_block = storage_pos % pc.block_positions;

		const auto pair_skip = pc.offsets.get2(block_id);
		if (pair_skip[0] == pair_skip[1])
			return DTM50_Final_Entry::make_illegal();

		const uint8_t* decoded =
			fetch_block_cached(m_cache[color], block_id, [&pc](
				Block_Cache<LZMA_Decompress_Helper>& cache, size_t bid) -> Block_Ptr
		{
			const size_t positions =
				(bid == pc.block_cnt - 1 && pc.tail_positions != 0)
				? pc.tail_positions
				: pc.block_positions;

			auto buf = std::make_shared<std::vector<uint8_t>>(positions * sizeof(uint16_t), 0);

			const auto pair = pc.offsets.get2(bid);
			const size_t doff = pair[0];
			const size_t dsz  = pair[1] - pair[0];
			if (dsz != 0)
			{
				LZMA_Decompress_Helper& dc = cache.decomp_for([&] {
					const size_t ppb = pc.block_positions;
					const size_t eb = pc.entry_bytes;
					const size_t max_payload =
						24
						+ (ppb * 2 + 7) / 8
						+ ppb * eb
						+ (ppb + 7) / 8 + ppb * (1 + 2 * eb)
						+ (ppb + 7) / 8 + ppb * (2 + 3 * eb)
						+ (ppb + 1) * 4 + ppb * (1 + 16 + (HMC_COUNT + 1) * eb)
						+ 3;
					return std::make_unique<LZMA_Decompress_Helper>(max_payload);
				});

				const size_t usz = pc.usizes.get(bid);
				const Const_Span<uint8_t> raw = dc.decompress(
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
				auto read_rank = [eb](const uint8_t* p) -> uint16_t {
					if (eb == 1) return *p;
					uint16_t r; std::memcpy(&r, p, 2); return r;
				};
				uint16_t* out = reinterpret_cast<uint16_t*>(buf->data());
				// hmc=0 is stored at PACK LAYER 1 (pack layer 0 = flat DTM).
				for (size_t k = 0; k < positions; ++k)
				{
					const size_t bit_off = k * 2;
					const uint8_t state = (state_bits[bit_off / 8] >> (bit_off % 8)) & 3u;
					uint16_t stored;
					switch (state)
					{
						case 0: {
							stored = r2v[read_rank(const_stream + const_idx * eb)];
							++const_idx;
							break;
						}
						case 1: {
							const uint8_t* entry = single_stream + single_off;
							const bool draw_end =
								(single_hints[single_idx >> 3] >> (single_idx & 7)) & 1u;
							const uint16_t h1 = entry[0] & 0x7Fu;
							if (h1 > 1)
								stored = r2v[read_rank(entry + 1)];        // r0
							else if (!draw_end)
								stored = r2v[read_rank(entry + 1 + eb)];   // r1
							else
								stored = 0;                                // r1 omitted == DRAW
							single_off += draw_end ? single_short : single_long;
							++single_idx;
							break;
						}
						case 2: {
							const uint8_t* entry = double_stream + double_off;
							const bool draw_end =
								(double_hints[double_idx >> 3] >> (double_idx & 7)) & 1u;
							const uint16_t h1 = entry[0];  // h2 >= 2, so layer 1 < h2
							stored = (h1 > 1) ? r2v[read_rank(entry + 2)]       // r0
							                  : r2v[read_rank(entry + 2 + eb)]; // r1
							double_off += draw_end ? double_short : double_long;
							++double_idx;
							break;
						}
						default: {
							const uint8_t* entry = multi_data + multi_dir[multi_idx];
							const size_t rsel = ((entry[1] >> 1) & 1u) ? 1u : 0u;
							stored = r2v[read_rank(entry + 17 + rsel * eb)];
							++multi_idx;
							break;
						}
					}
					out[k] = stored;
				}
				ASSERT(const_idx == num_const && single_idx == num_single
				    && double_idx == num_double && multi_idx == num_multi);
			}
			return buf;
		});

		uint16_t stored;
		std::memcpy(&stored, decoded + pos_in_block * sizeof(uint16_t), sizeof(stored));
		return dtm50_entry_from_storage(stored, w);
	}

	bool m_is_singular[COLOR_NB];
	Per_Color m_per_color[COLOR_NB];
	Memory_Mapped_File m_lzdtm50_file;
	WDL_File_For_Probe m_wdl;
	mutable Block_Cache<LZMA_Decompress_Helper> m_cache[COLOR_NB];
};

// DTM50_Sub_File_Flat: flat mmap'd layer-0 reader; like DTM_Sub_File_Flat but
// decodes with dtm50_entry_from_storage (cursed/blessed -> DRAW).
struct DTM50_Sub_File_Flat;

void load_dtm50_sub_flat(
	Out_Param<DTM50_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool);

struct DTM50_Sub_File_Flat : public EGTB_Sub_Reader<DTM50_Final_Entry>
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
	~DTM50_Sub_File_Flat()
	{
		for (Color c : { WHITE, BLACK })
			m_per_color[c].file.close();
		m_tmp_files.clear();
	}

	void open(const EGTB_Paths& egtb_files, const Piece_Config& ps,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		load_dtm50_sub_flat(out_param(*this), egtb_files, ps, thread_pool);
	}

	NODISCARD DTM50_Final_Entry read(Color color, Board_Index pos) const override
	{
		const auto& pc = m_per_color[color];
		const size_t storage_pos = logical_index_to_storage_index(pc.plan, static_cast<size_t>(pos));
		DTM50_Final_Entry e;
		std::memcpy(&e,
		            pc.file.data() + storage_pos * sizeof(DTM50_Final_Entry),
		            sizeof(DTM50_Final_Entry));
		return e;
	}

	Per_Color m_per_color[COLOR_NB];
	Temporary_File_Tracker m_tmp_files;
};
