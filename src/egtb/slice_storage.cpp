#include "egtb/slice_storage.h"
#include "egtb/egtb_entry.h"
#include "egtb/egtb_gen.h"

#include "util/compress.h"
#include "util/defines.h"

#include "lz4/lz4.h"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

std::vector<uint8_t> compress_bytes(LZ4_Compress_Helper& comp,
                                    const uint8_t* data, size_t bytes)
{
	std::vector<uint8_t> out(comp.compress_bound(bytes));
	const size_t n = comp.compress(Span<uint8_t>(out), Const_Span<uint8_t>(data, bytes));
	out.resize(n);
	return out;
}

void decompress_bytes_into(const uint8_t* src, size_t src_size,
                           uint8_t* dst, size_t want)
{
	const int ret = LZ4_decompress_safe(
		reinterpret_cast<const char*>(src),
		reinterpret_cast<char*>(dst),
		static_cast<int>(src_size),
		static_cast<int>(want));
	if (ret <= 0 || static_cast<size_t>(ret) != want)
		throw std::runtime_error("Slice LZ4 decompress failed");
}

}  // namespace

template <typename EntryT, typename... OtherEntryTs>
void save_slice_file(
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& src,
	const std::filesystem::path& path,
	uint64_t magic)
{
	const size_t num_slices = src.num_slices();
	const size_t within = src.within_slice_size();
	const size_t entry_size = sizeof(EntryT);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) throw std::runtime_error("Could not open slice file for writing: " + path.string());

	Slice_File_Header header{};
	header.magic = magic;
	header.num_slices = num_slices;
	header.within_slice_size = static_cast<uint32_t>(within);
	header.entry_size = static_cast<uint32_t>(entry_size);
	header.reserved = 0;
	out.write(reinterpret_cast<const char*>(&header), sizeof(header));

	std::vector<Slice_Index_Entry> index(num_slices);
	out.write(reinterpret_cast<const char*>(index.data()), index.size() * sizeof(Slice_Index_Entry));

	LZ4_Compress_Helper comp(nullptr);
	uint64_t cursor = sizeof(Slice_File_Header) + num_slices * sizeof(Slice_Index_Entry);
	const size_t slice_bytes = within * entry_size;
	const size_t spg = src.slices_per_group();
	const size_t ng  = src.num_groups();
	for (size_t g = 0; g < ng; ++g)
	{
		const bool was_resident = src.is_group_resident(g);
		if (!was_resident) src.load_group(g);

		const size_t s_begin = g * spg;
		const size_t s_end   = std::min(s_begin + spg, num_slices);
		for (size_t i = s_begin; i < s_end; ++i)
		{
			auto blk = compress_bytes(
				comp,
				reinterpret_cast<const uint8_t*>(src.slice_data(i)),
				slice_bytes);

			index[i].data_offset = cursor;
			index[i].compressed_size = static_cast<uint32_t>(blk.size());
			index[i].uncompressed_size = static_cast<uint32_t>(slice_bytes);
			cursor += blk.size();

			out.write(reinterpret_cast<const char*>(blk.data()), blk.size());
			if (!out) throw std::runtime_error("Write error on slice file: " + path.string());
		}

		if (!was_resident) src.evict_group(g);
	}

	out.seekp(static_cast<std::streamoff>(sizeof(Slice_File_Header)));
	out.write(reinterpret_cast<const char*>(index.data()), index.size() * sizeof(Slice_Index_Entry));

	if (!out) throw std::runtime_error("Write error on slice file: " + path.string());
}

template void save_slice_file<DTC_Final_Entry, DTC_Intermediate_Entry>(
	Sliced_EGTB_File_For_Gen<DTC_Final_Entry, DTC_Intermediate_Entry>&,
	const std::filesystem::path&, uint64_t);
template void save_slice_file<DTM_Final_Entry, DTM_Intermediate_Entry>(
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry, DTM_Intermediate_Entry>&,
	const std::filesystem::path&, uint64_t);
template void save_slice_file<DTM_Final_Entry>(
	Sliced_EGTB_File_For_Gen<DTM_Final_Entry>&,
	const std::filesystem::path&, uint64_t);

namespace {

struct Spill_Header
{
	uint64_t magic;
	uint64_t uncompressed_size;
	uint64_t compressed_size;
};
static_assert(sizeof(Spill_Header) == 24);

}  // namespace

void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic)
{
	const int src_size = narrowing_static_cast<int>(bytes);
	std::vector<uint8_t> out(static_cast<size_t>(LZ4_compressBound(src_size)));
	const int n = LZ4_compress_default(
		reinterpret_cast<const char*>(data),
		reinterpret_cast<char*>(out.data()),
		src_size,
		narrowing_static_cast<int>(out.size()));
	if (n <= 0) throw std::runtime_error("LZ4 compress failed for spill file: " + path.string());

	Spill_Header hdr{};
	hdr.magic = magic;
	hdr.uncompressed_size = bytes;
	hdr.compressed_size = static_cast<uint64_t>(n);

	std::ofstream fp(path, std::ios::binary | std::ios::trunc);
	if (!fp) throw std::runtime_error("Could not open spill file for write: " + path.string());
	fp.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
	fp.write(reinterpret_cast<const char*>(out.data()), n);
	if (!fp) throw std::runtime_error("Write error on spill file: " + path.string());
}

void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic)
{
	std::ifstream fp(path, std::ios::binary);
	if (!fp) throw std::runtime_error("Could not open spill file for read: " + path.string());

	Spill_Header hdr{};
	fp.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!fp) throw std::runtime_error("Spill file truncated header: " + path.string());
	if (hdr.magic != expected_magic)
		throw std::runtime_error("Spill file magic mismatch: " + path.string());

	if (hdr.uncompressed_size != bytes)
		throw std::runtime_error("Spill file size mismatch: " + path.string());

	std::vector<uint8_t> buf(hdr.compressed_size);
	fp.read(reinterpret_cast<char*>(buf.data()), buf.size());
	if (!fp) throw std::runtime_error("Spill file truncated payload: " + path.string());

	decompress_bytes_into(buf.data(), buf.size(), data, bytes);
}
