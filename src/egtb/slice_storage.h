#pragma once

#include "egtb/egtb_gen.h"
#include "egtb/egtb_entry.h"

#include "util/defines.h"
#include "util/filesystem.h"
#include "util/span.h"

#include <cstdint>
#include <filesystem>

struct Slice_File_Header
{
	uint64_t magic;
	uint64_t num_slices;
	uint32_t within_slice_size;
	uint32_t entry_size;
	uint64_t reserved;
};
static_assert(sizeof(Slice_File_Header) == 32);

struct Slice_Index_Entry
{
	uint64_t data_offset;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
};
static_assert(sizeof(Slice_Index_Entry) == 16);

template <typename EntryT, typename... OtherEntryTs>
void save_slice_file(
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& src,
	const std::filesystem::path& path,
	uint64_t magic);

void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic);

void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic);
