#include "egtb/egtb_probe.h"

void load_wdl_table(
	Out_Param<WDL_File_For_Probe> wdl,
	const Piece_Config& ps,
	std::filesystem::path sub_wdl,
	EGTB_Magic wdl_magic
)
{
	if (!wdl->m_lzw_file.open_readonly(sub_wdl.c_str()))
		throw std::runtime_error("Could not open WDL file trying to load " + sub_wdl.string());

	const Const_Span<uint8_t> input = wdl->m_lzw_file.data_span();

	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid WDL file size trying to load " + sub_wdl.string());

	Serial_Memory_Reader reader(input);

	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid WDL file checksum trying to load " + sub_wdl.string());

	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t block_size[COLOR_NB]{ 0, 0 };
	size_t tail_size[COLOR_NB]{ 0, 0 };

	size_t dict_size[COLOR_NB]{ 0, 0 };
	const uint8_t* lp_dict[COLOR_NB]{ nullptr, nullptr };
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };
	size_t data_size[COLOR_NB]{ 0, 0 };
	const uint8_t* offset_tb[COLOR_NB]{ nullptr, nullptr };

	const uint32_t magic = reader.read<uint32_t>();

	if (magic != narrowing_static_cast<uint32_t>(wdl_magic))
		throw std::runtime_error("Invalid WDL file magic trying to load " + sub_wdl.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key.value() != ps.min_material_key().value())
		throw std::runtime_error("Wrong material key in WDL file " + sub_wdl.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	for (const Color i : table_colors)
	{
		if (reader.read<uint8_t>() & 0x80)
		{
			wdl->m_is_singular[i] = true;
			wdl->m_single_val[i] = reader.read<uint8_t>();
		}
		else
		{
			wdl->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid WDL index permutation for " + sub_wdl.string());
			wdl->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);

			tail_size[i] = reader.read<uint16_t>();
			block_size[i] = reader.read<uint32_t>();
			block_cnt[i] = reader.read<uint64_t>();
			data_size[i] = reader.read<uint64_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		dict_size[i] = reader.read<uint16_t>();
		if (dict_size[i] != 0)
		{
			lp_dict[i] = reader.caret();
			reader.advance(dict_size[i]);
		}
	}

	uint8_t mono_params[COLOR_NB][3]{};  // log2_bu, sample_width, offset_width
	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		mono_params[i][0] = reader.read<uint8_t>();
		mono_params[i][1] = reader.read<uint8_t>();
		mono_params[i][2] = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		offset_tb[i] = reader.caret();
		reader.advance(Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, mono_params[i][0], mono_params[i][1], mono_params[i][2]));
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	for (const Color i : table_colors)
	{
		if (wdl->m_is_singular[i])
			continue;

		ASSERT(data[i] != nullptr);
		ASSERT(offset_tb[i] != nullptr);

		const size_t num_full_sized_blocks =
			tail_size[i] != 0
			? block_cnt[i] - 1
			: block_cnt[i];
		const size_t file_sz = block_size[i] * num_full_sized_blocks + tail_size[i];
		if (file_sz != ceil_div(epsi.num_positions(), WDL_ENTRY_PACK_RATIO))
			throw std::runtime_error("Invalid decompressed size of WDL table from " + sub_wdl.string());

		auto& pc = wdl->m_per_color[i];
		pc.block_size = block_size[i];
		pc.tail_size = tail_size[i];
		pc.block_cnt = block_cnt[i];
		pc.compressed_data = data[i];
		pc.dict = LZ4_Dict::load(Const_Span(lp_dict[i], lp_dict[i] + dict_size[i]));
		pc.offsets = Mono_Uint_Vec(offset_tb[i], block_cnt[i] + 1,
		                           mono_params[i][0], mono_params[i][1], mono_params[i][2]);
	}
}

// Wire format: see save_egtb_table.
void load_dtm_table(
	Out_Param<DTM_File_For_Probe> dtm,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm,
	EGTB_Magic dtm_magic
)
{
	if (!dtm->m_lzdtm_file.open_readonly(sub_dtm.c_str()))
		throw std::runtime_error("Could not open DTM file trying to load " + sub_dtm.string());

	const Const_Span<uint8_t> input = dtm->m_lzdtm_file.data_span();

	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM file size trying to load " + sub_dtm.string());

	Serial_Memory_Reader reader(input);

	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM file checksum trying to load " + sub_dtm.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(dtm_magic))
		throw std::runtime_error("Invalid DTM file magic trying to load " + sub_dtm.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key.value() != ps.min_material_key().value())
		throw std::runtime_error("Wrong material key in DTM file " + sub_dtm.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t data_size[COLOR_NB]{ 0, 0 };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			dtm->m_is_singular[i] = true;
			dtm->m_single_val[i] = reader.read<uint8_t>();
		}
		else
		{
			dtm->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid DTM index permutation for " + sub_dtm.string());
			dtm->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			auto& pc = dtm->m_per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != sizeof(DTM_Final_Entry) && pc.entry_bytes != 1)
				throw std::runtime_error("Bad DTM entry_bytes in " + sub_dtm.string());
			pc.tail_size  = reader.read<uint32_t>();
			pc.block_size = reader.read<uint32_t>();
			pc.block_cnt  = reader.read<uint64_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		auto& pc = dtm->m_per_color[i];
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			pc.block_cnt + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		pc.offsets = Mono_Uint_Vec(mono_ptr, pc.block_cnt + 1,
		                           log2_bu, sample_width, offset_width);
	}

	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		reader.align(64);
		dtm->m_per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	for (const Color i : table_colors)
	{
		if (dtm->m_is_singular[i]) continue;
		auto& pc = dtm->m_per_color[i];
		const size_t num_full = pc.tail_size != 0 ? pc.block_cnt - 1 : pc.block_cnt;
		const size_t src_sz   = pc.block_size * num_full + pc.tail_size;
		if (src_sz != num_positions * pc.entry_bytes)
			throw std::runtime_error("DTM decompressed size mismatch " + sub_dtm.string());
	}
}

// Wire format: see save_dtm50_table.
void load_dtm50_table(
	Out_Param<DTM50_File_For_Probe> dtm50,
	const Piece_Config& ps,
	std::filesystem::path sub_dtm50
)
{
	if (!dtm50->m_lzdtm50_file.open_readonly(sub_dtm50.c_str()))
		throw std::runtime_error("Could not open DTM50 file trying to load " + sub_dtm50.string());

	const Const_Span<uint8_t> input = dtm50->m_lzdtm50_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM50 file size trying to load " + sub_dtm50.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM50 file checksum trying to load " + sub_dtm50.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		throw std::runtime_error("Invalid DTM50 file magic trying to load " + sub_dtm50.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key.value() != ps.min_material_key().value())
		throw std::runtime_error("Wrong material key in DTM50 file " + sub_dtm50.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t data_size[COLOR_NB]{ 0, 0 };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			dtm50->m_is_singular[i] = true;
			if (reader.read<uint8_t>() != 0)
				throw std::runtime_error("DTM50 singular value must be DRAW in " + sub_dtm50.string());
		}
		else
		{
			dtm50->m_is_singular[i] = false;
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid DTM50 index permutation for " + sub_dtm50.string());
			dtm50->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			auto& pc = dtm50->m_per_color[i];
			pc.entry_bytes = reader.read<uint8_t>();
			if (pc.entry_bytes != 1 && pc.entry_bytes != 2)
				throw std::runtime_error("Bad DTM50 entry_bytes in " + sub_dtm50.string());
			pc.block_positions = reader.read<uint32_t>();
			pc.block_cnt       = reader.read<uint64_t>();
			pc.tail_positions  = reader.read<uint32_t>();
			data_size[i]       = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			pc.rank_to_value.resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				pc.rank_to_value[r] = reader.read<uint16_t>();
		}
	}

	for (const Color i : table_colors)
	{
		if (dtm50->m_is_singular[i]) continue;
		auto& pc = dtm50->m_per_color[i];
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

	for (const Color i : table_colors)
	{
		if (dtm50->m_is_singular[i]) continue;
		reader.align(64);
		dtm50->m_per_color[i].compressed_data = reader.caret();
		reader.advance(data_size[i]);
	}
}

// Eager-decode variant of load_dtm_table: every block decompresses up front,
// resolves WDL class inline, and lands in a mmap'd per-color tmp file. Sub-TB
// reads become a flat indexed memcpy; no LZMA on the read path.
void load_dtm_sub_flat(
	Out_Param<DTM_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool
)
{
	std::filesystem::path dtm_path;
	if (!egtb_files.find_dtm_file(ps, &dtm_path))
		throw std::runtime_error("Could not find a DTM file for " + ps.name());
	std::filesystem::path wdl_path;
	if (!egtb_files.find_wdl_file(ps, &wdl_path))
		throw std::runtime_error("Could not find a WDL file for " + ps.name()
			+ " (DTM consumes DTC's wdl/ output)");

	// Companion WDL for class resolution; per-block workers below read it
	// concurrently through its shared block cache.
	const WDL_File_For_Probe wdl(egtb_files, ps, thread_pool);

	Memory_Mapped_File map_file;
	if (!map_file.open_readonly(dtm_path.c_str()))
		throw std::runtime_error("Could not open DTM file trying to load " + dtm_path.string());

	const Const_Span<uint8_t> input = map_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM file size trying to load " + dtm_path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM file checksum trying to load " + dtm_path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM_MAGIC))
		throw std::runtime_error("Invalid DTM file magic trying to load " + dtm_path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key.value() != ps.min_material_key().value())
		throw std::runtime_error("Wrong material key in DTM file " + dtm_path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t entry_bytes[COLOR_NB]{ 0, 0 };
	size_t tail_size[COLOR_NB]{ 0, 0 };
	size_t block_size[COLOR_NB]{ 0, 0 };
	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t data_size[COLOR_NB]{ 0, 0 };
	bool is_singular[COLOR_NB]{ false, false };
	uint8_t single_val[COLOR_NB]{ 0, 0 };
	std::vector<uint16_t> rank_to_value[COLOR_NB];
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			is_singular[i] = true;
			single_val[i] = reader.read<uint8_t>();
		}
		else
		{
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid flat DTM index permutation for " + dtm_path.string());
			flat->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			entry_bytes[i] = reader.read<uint8_t>();
			if (entry_bytes[i] != sizeof(DTM_Final_Entry) && entry_bytes[i] != 1)
				throw std::runtime_error("Bad DTM entry_bytes in " + dtm_path.string());
			tail_size[i]  = reader.read<uint32_t>();
			block_size[i] = reader.read<uint32_t>();
			block_cnt[i]  = reader.read<uint64_t>();
			data_size[i]  = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			rank_to_value[i].resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				rank_to_value[i][r] = reader.read<uint16_t>();
		}
	}

	Mono_Uint_Vec offsets[COLOR_NB];
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		reader.advance(1);  // usz_width
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		offsets[i] = Mono_Uint_Vec(mono_ptr, block_cnt[i] + 1,
		                           log2_bu, sample_width, offset_width);
	}

	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	const size_t file_bytes = num_positions * sizeof(DTM_Final_Entry);

	for (const Color i : table_colors)
	{
		const auto tmp_path = egtb_files.dtm_sub_flat_path(ps, i);

		Memory_Mapped_File out_map(Memory_Mapped_File::Access_Advice::RANDOM);
		if (!out_map.create(tmp_path.c_str(), file_bytes))
			throw std::runtime_error("Could not create flat sub-DTM at " + tmp_path.string());
		flat->m_tmp_files.track_path(tmp_path);
		DTM_Final_Entry* out = reinterpret_cast<DTM_Final_Entry*>(out_map.data());

		if (is_singular[i])
		{
			const uint16_t sv = single_val[i];
			constexpr size_t FILL_CHUNK = 1u << 16;
			std::atomic<size_t> next_chunk(0);
			thread_pool->run_sync_task_on_all_threads([&](size_t) {
				for (;;)
				{
					const size_t start = next_chunk.fetch_add(FILL_CHUNK, std::memory_order_relaxed);
					if (start >= num_positions) return;
					const size_t stop = std::min(start + FILL_CHUNK, num_positions);
					for (size_t p = start; p < stop; ++p)
					{
						const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(p));
						out[p] = dtm_entry_from_storage(sv, w);
					}
				}
			});
		}
		else
		{
			const size_t num_full = tail_size[i] != 0 ? block_cnt[i] - 1 : block_cnt[i];
			const size_t src_sz   = block_size[i] * num_full + tail_size[i];
			if (src_sz != num_positions * entry_bytes[i])
				throw std::runtime_error("DTM decompressed size mismatch " + dtm_path.string());

			// Blocks write disjoint output slices; atomic fetch_add load-
			// balances cheap dsz==0 skip blocks against LZMA-heavy ones.
			const auto& r2v = rank_to_value[i];
			const size_t blocks = block_cnt[i];
			const size_t positions_per_block = block_size[i] / entry_bytes[i];
			std::atomic<size_t> next_block(0);

			const auto& perm_plan = flat->m_per_color[i].plan;

			thread_pool->run_sync_task_on_all_threads([&](size_t) {
				LZMA_Decompress_Helper dc_helper(block_size[i]);

				for (;;)
				{
					const size_t idx = next_block.fetch_add(1, std::memory_order_relaxed);
					if (idx >= blocks) return;

					const auto pair = offsets[i].get2(idx);
					const size_t doff = pair[0];
					const size_t dsz  = pair[1] - pair[0];

					const size_t decode_sz =
						(idx == blocks - 1 && tail_size[i] != 0) ? tail_size[i] : block_size[i];
					const size_t positions = decode_sz / entry_bytes[i];
					const size_t pos = idx * positions_per_block;

					// Skip block: encoder collapsed an all-don't-care span; see
					// singular-DRAW arm above for why DRAW-fill is correct.
					if (dsz == 0)
					{
						std::memset(out + pos, 0, positions * sizeof(DTM_Final_Entry));
						continue;
					}

					const Const_Span<uint8_t> raw = dc_helper.decompress(
						Const_Span<uint8_t>(data[i] + doff, dsz), decode_sz);

					if (entry_bytes[i] == 1)
					{
						for (size_t k = 0; k < positions; ++k)
						{
							const uint16_t stored = r2v[raw[k]];
							const size_t logical = storage_index_to_logical_index(perm_plan, pos + k);
							const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical));
							out[pos + k] = dtm_entry_from_storage(stored, w);
						}
					}
					else
					{
						const uint16_t* in = reinterpret_cast<const uint16_t*>(raw.data());
						for (size_t k = 0; k < positions; ++k)
						{
							const uint16_t stored = r2v[in[k]];
							const size_t logical = storage_index_to_logical_index(perm_plan, pos + k);
							const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical));
							out[pos + k] = dtm_entry_from_storage(stored, w);
						}
					}
				}
			});
		}

		flat->m_per_color[i].file = std::move(out_map);
	}
}

// Eagerly decompress and remap the hmc=0 column of the rs-pack into a flat
// mmap'd array; higher-piece DTM50 generators read only that column.
// File / block layout is defined in egtb_gen_dtm50.cpp::save_to_disk.
void load_dtm50_sub_flat(
	Out_Param<DTM50_Sub_File_Flat> flat,
	const EGTB_Paths& egtb_files,
	const Piece_Config& ps,
	In_Out_Param<Thread_Pool> thread_pool
)
{
	std::filesystem::path dtm_path;
	if (!egtb_files.find_dtm50_file(ps, &dtm_path))
		throw std::runtime_error("Could not find a DTM50 file for " + ps.name());
	std::filesystem::path wdl_path;
	if (!egtb_files.find_wdl_file(ps, &wdl_path))
		throw std::runtime_error("Could not find a WDL file for " + ps.name()
			+ " (DTM50 consumes DTC's wdl/ output for class disambiguation)");

	const WDL_File_For_Probe wdl(egtb_files, ps, thread_pool);

	Memory_Mapped_File map_file;
	if (!map_file.open_readonly(dtm_path.c_str()))
		throw std::runtime_error("Could not open DTM50 file trying to load " + dtm_path.string());

	const Const_Span<uint8_t> input = map_file.data_span();
	if ((input.size() & 63) != 8)
		throw std::runtime_error("Invalid DTM50 file size trying to load " + dtm_path.string());

	Serial_Memory_Reader reader(input);
	if (!reader.is_end_checksum_ok(static_cast<uint64_t>(EGTB_CHECKSUM_INIT_VALUE)))
		throw std::runtime_error("Invalid DTM50 file checksum trying to load " + dtm_path.string());

	const uint32_t magic = reader.read<uint32_t>();
	if (magic != narrowing_static_cast<uint32_t>(EGTB_Magic::DTM50_MAGIC))
		throw std::runtime_error("Invalid DTM50 file magic trying to load " + dtm_path.string());

	const uint32_t key_and_table_num = reader.read<uint32_t>();
	const Material_Key key = static_cast<Material_Key>(key_and_table_num >> 2u);
	if (key.value() != ps.min_material_key().value())
		throw std::runtime_error("Wrong material key in DTM50 file " + dtm_path.string());

	const size_t table_num = key_and_table_num & 3;
	const Fixed_Vector<Color, 2> table_colors = egtb_table_colors(table_num);
	const Piece_Config_For_Gen epsi(ps);

	size_t entry_bytes[COLOR_NB]{ 0, 0 };
	size_t block_positions[COLOR_NB]{ 0, 0 };
	size_t block_cnt[COLOR_NB]{ 0, 0 };
	size_t tail_positions[COLOR_NB]{ 0, 0 };
	size_t data_size[COLOR_NB]{ 0, 0 };
	bool is_singular[COLOR_NB]{ false, false };
	std::vector<uint16_t> rank_to_value[COLOR_NB];
	const uint8_t* data[COLOR_NB]{ nullptr, nullptr };

	for (const Color i : table_colors)
	{
		const uint8_t flag = reader.read<uint8_t>();
		if (flag & EGTB_SINGULAR_FLAG)
		{
			is_singular[i] = true;
			reader.advance(1);  // singular_wdl byte; unused (DRAW everywhere)
		}
		else
		{
			const uint32_t perm = reader.read<uint32_t>();
			if (!index_permutation_config_is_valid(epsi, perm))
				throw std::runtime_error("Invalid flat DTM50 index permutation for " + dtm_path.string());
			flat->m_per_color[i].plan = make_index_permutation_plan(epsi, perm);
			entry_bytes[i] = reader.read<uint8_t>();
			if (entry_bytes[i] != 1 && entry_bytes[i] != 2)
				throw std::runtime_error("Bad DTM50 entry_bytes in " + dtm_path.string());
			block_positions[i] = reader.read<uint32_t>();
			block_cnt[i]       = reader.read<uint64_t>();
			tail_positions[i]  = reader.read<uint32_t>();
			data_size[i]       = reader.read<uint64_t>();

			const size_t num_ranks = reader.read<uint16_t>();
			rank_to_value[i].resize(num_ranks);
			for (size_t r = 0; r < num_ranks; ++r)
				rank_to_value[i][r] = reader.read<uint16_t>();
		}
	}

	Mono_Uint_Vec offsets[COLOR_NB];
	Min0_Uint_Vec usizes[COLOR_NB];
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		const uint8_t log2_bu      = reader.read<uint8_t>();
		const uint8_t sample_width = reader.read<uint8_t>();
		const uint8_t offset_width = reader.read<uint8_t>();
		const uint8_t usz_width    = reader.read<uint8_t>();
		const uint8_t* mono_ptr = reader.caret();
		const size_t mono_bytes = Mono_Uint_Vec::on_disk_bytes(
			block_cnt[i] + 1, log2_bu, sample_width, offset_width);
		reader.advance(mono_bytes);
		const uint8_t* usz_ptr = reader.caret();
		const size_t usz_bytes = Min0_Uint_Vec::on_disk_bytes(block_cnt[i], usz_width);
		reader.advance(usz_bytes);
		offsets[i] = Mono_Uint_Vec(mono_ptr, block_cnt[i] + 1,
		                           log2_bu, sample_width, offset_width);
		usizes[i] = Min0_Uint_Vec(usz_ptr, block_cnt[i], usz_width);
	}
	for (const Color i : table_colors)
	{
		if (is_singular[i]) continue;
		reader.align(64);
		data[i] = reader.caret();
		reader.advance(data_size[i]);
	}

	const size_t num_positions = epsi.num_positions();
	const size_t file_bytes = num_positions * sizeof(DTM50_Final_Entry);

	for (const Color i : table_colors)
	{
		const auto tmp_path = egtb_files.dtm50_sub_flat_path(ps, i);

		Memory_Mapped_File out_map(Memory_Mapped_File::Access_Advice::RANDOM);
		if (!out_map.create(tmp_path.c_str(), file_bytes))
			throw std::runtime_error("Could not create flat sub-DTM50 at " + tmp_path.string());
		flat->m_tmp_files.track_path(tmp_path);
		DTM50_Final_Entry* out = reinterpret_cast<DTM50_Final_Entry*>(out_map.data());

		if (is_singular[i])
		{
			// DRAW everywhere; see load_dtm_sub_flat singular arm.
			std::memset(out, 0, file_bytes);
			flat->m_per_color[i].file = std::move(out_map);
			continue;
		}

		const auto& r2v = rank_to_value[i];
		const size_t blocks = block_cnt[i];
		const size_t ppb = block_positions[i];
		const size_t eb = entry_bytes[i];
		std::atomic<size_t> next_block(0);

		const auto& perm_plan = flat->m_per_color[i].plan;

		// Strict upper bound: sums the worst case for each state region (they're
		// mutually exclusive per position, so this overestimates).
		const size_t max_payload =
			24                                                         // header
			+ (ppb * 2 + 7) / 8                                        // state_bits
			+ ppb * eb                                                 // all CONST
			+ (ppb + 7) / 8 + ppb * (1 + 2 * eb)                       // all SINGLE
			+ (ppb + 7) / 8 + ppb * (2 + 3 * eb)                       // all DOUBLE
			+ (ppb + 1) * 4 + ppb * (1 + 16 + DTM50_PACK_LAYERS * eb)  // all MULTI
			+ 3;                                                       // tail alignment

		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			LZMA_Decompress_Helper dc_helper(max_payload);

			for (;;)
			{
				const size_t bidx = next_block.fetch_add(1, std::memory_order_relaxed);
				if (bidx >= blocks) return;

				const auto pair = offsets[i].get2(bidx);
				const size_t doff = pair[0];
				const size_t dsz  = pair[1] - pair[0];
				const size_t usz  = usizes[i].get(bidx);

				const size_t this_bp =
					(bidx == blocks - 1 && tail_positions[i] != 0) ? tail_positions[i] : ppb;
				const size_t base_pos = bidx * ppb;

				// Skip sentinel: all-don't-care span, DRAW-fill.
				if (dsz == 0)
				{
					std::memset(out + base_pos, 0,
						this_bp * sizeof(DTM50_Final_Entry));
					continue;
				}

				const Const_Span<uint8_t> raw = dc_helper.decompress(
					Const_Span<uint8_t>(data[i] + doff, dsz), usz);

				const uint8_t* p = raw.data();
				uint32_t np32, num_single, num_double, num_multi, ss_bytes32, ds_bytes32;
				std::memcpy(&np32,       p,      4);
				std::memcpy(&num_single, p + 4,  4);
				std::memcpy(&num_double, p + 8,  4);
				std::memcpy(&num_multi,  p + 12, 4);
				std::memcpy(&ss_bytes32, p + 16, 4);
				std::memcpy(&ds_bytes32, p + 20, 4);
				ASSERT(np32 == this_bp);
				const size_t num_const  = np32 - num_single - num_double - num_multi;
				const size_t sb_bytes   = (np32 * 2 + 7) / 8;
				const size_t sh_bytes   = (num_single + 7) / 8;
				const size_t dh_bytes   = (num_double + 7) / 8;
				p += 24;

				const uint8_t* state_bits   = p;                            p += sb_bytes;
				const uint8_t* const_stream = p;                            p += num_const * eb;
				const uint8_t* single_hints = p;                            p += sh_bytes;
				const uint8_t* single_stream = p;                           p += ss_bytes32;
				const uint8_t* double_hints = p;                            p += dh_bytes;
				const uint8_t* double_stream = p;                           p += ds_bytes32;
				p += (4 - ((p - raw.data()) & 3)) & 3;
				const uint32_t* multi_dir = reinterpret_cast<const uint32_t*>(p);
				p += (num_multi + 1) * 4;
				const uint8_t* multi_data = p;

				// Reading from layer 1 (hmc=0). Pick the rank active at layer 1:
				// CONST is constant; SINGLE/DOUBLE pick r0 unless their first
				// transition is exactly at layer 1; MULTI pops the changepoint bit
				// at layer 1. The draw-end hint can fire here (a SINGLE whose
				// transition lands on layer 1 into DRAW): we emit stored 0 and let
				// the WDL companion resolve it to DRAW.
				const size_t single_short = 1 + eb;
				const size_t single_long  = 1 + 2 * eb;
				const size_t double_short = 2 + 2 * eb;
				const size_t double_long  = 2 + 3 * eb;
				size_t const_idx = 0, single_idx = 0, double_idx = 0, multi_idx = 0;
				size_t single_off = 0, double_off = 0;
				auto read_rank = [eb](const uint8_t* p) -> uint16_t {
					if (eb == 1) return *p;
					uint16_t r; std::memcpy(&r, p, 2); return r;
				};

				for (size_t k = 0; k < this_bp; ++k)
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
							const uint16_t h1 = entry[0];  // h2 >= 2, so layer 1 < h2 always
							stored = (h1 > 1) ? r2v[read_rank(entry + 2)]       // r0
							                  : r2v[read_rank(entry + 2 + eb)]; // r1
							double_off += draw_end ? double_short : double_long;
							++double_idx;
							break;
						}
						default: {
							const uint8_t* entry = multi_data + multi_dir[multi_idx];
							// cp[0]=0 (bit 0 always set); rsel = popcount(bits 0..1)-1.
							const size_t rsel = ((entry[1] >> 1) & 1u) ? 1u : 0u;
							stored = r2v[read_rank(entry + 17 + rsel * eb)];
							++multi_idx;
							break;
						}
					}
					const size_t logical = storage_index_to_logical_index(perm_plan, base_pos + k);
					const WDL_Entry w = wdl.read(i, static_cast<Board_Index>(logical));
					out[base_pos + k] = dtm50_entry_from_storage(stored, w);
				}
				ASSERT(const_idx == num_const && single_idx == num_single
				    && double_idx == num_double && multi_idx == num_multi);
			}
		});

		flat->m_per_color[i].file = std::move(out_map);
	}
}
