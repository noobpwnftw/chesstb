#pragma once

#include "defines.h"
#include "endian.h"
#include "filesystem.h"
#include "span.h"
#include "utility.h"

#include "zstd/common/xxhash.h"

// Table files end in an XXH64 of the bytes before it.
struct Serial_File_Writer
{
	Serial_File_Writer() : m_hash(XXH64_createState())
	{
		if (m_hash == nullptr) print_and_abort("Could not allocate checksum state\n");
	}

	~Serial_File_Writer() { XXH64_freeState(m_hash); }

	bool create(const char* path, uint64_t checksum_init)
	{
		if (!m_file.create(path)) return false;
		XXH64_reset(m_hash, checksum_init);
		return true;
	}

	template <typename T>
	void write(const Identity<T>& value)
	{
		write(Const_Span<uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
	}

	void write(Const_Span<uint8_t> data)
	{
		if (!m_file.write_at(m_offset, data)) print_and_abort("File write failed\n");
		XXH64_update(m_hash, data.data(), data.size());
		m_offset += data.size();
	}

	void zero_align(size_t alignment)
	{
		const size_t missing = (alignment - m_offset % alignment) % alignment;
		const uint8_t zeros[64]{};
		ASSERT(missing <= sizeof(zeros));
		write(Const_Span<uint8_t>(zeros, missing));
	}

	NODISCARD uint64_t reserve(size_t bytes)
	{
		const uint64_t offset = m_offset;
		m_offset += bytes;
		return offset;
	}

	void hash(Const_Span<uint8_t> data) { XXH64_update(m_hash, data.data(), data.size()); }

	NODISCARD bool write_at(uint64_t offset, Const_Span<uint8_t> data) const
	{
		return m_file.write_at(offset, data);
	}

	NODISCARD size_t num_bytes_written() const { return m_offset; }

	void write_end_checksum()
	{
		const uint64_t checksum = XXH64_digest(m_hash);
		if (!m_file.write_at(m_offset, Const_Span<uint8_t>(
				reinterpret_cast<const uint8_t*>(&checksum), sizeof(checksum))))
			print_and_abort("Checksum write failed\n");
		m_offset += sizeof(checksum);
	}

	void close_file() { m_file.close_file(); }

private:
	Positional_Output_File m_file;
	XXH64_state_t* m_hash;
	uint64_t m_offset = 0;
};

NODISCARD inline bool end_checksum_ok(Const_Span<uint8_t> file, uint64_t init)
{
	if (file.size() < 8) return false;
	return XXH64(file.data(), file.size() - 8, init) == read_le<uint64_t>(file.data() + file.size() - 8);
}
