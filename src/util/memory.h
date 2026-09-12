#pragma once

#include "defines.h"
#include "filesystem.h"
#include "span.h"
#include "utility.h"

#include "zstd/common/xxhash.h"

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

struct Serial_Memory_Writer
{
	Serial_Memory_Writer(Span<uint8_t> span) :
		m_caret(span.begin()),
		m_begin(span.begin()),
		m_end(span.end())
	{}

	template <typename T>
	void write(const Identity<T>& val)
	{
		ASSERT(m_caret + sizeof(val) <= m_end);
		std::memcpy(m_caret, &val, sizeof(val));
		m_caret += sizeof(val);
	}

	void write(Const_Span<uint8_t> data)
	{
		ASSERT(m_caret + data.size() <= m_end);
		std::memcpy(m_caret, data.begin(), data.size());
		m_caret += data.size();
	}

	NODISCARD Span<uint8_t> reserve(size_t bytes)
	{
		ASSERT(m_caret + bytes <= m_end);
		uint8_t* const begin = m_caret;
		m_caret += bytes;
		return Span<uint8_t>(begin, bytes);
	}

	void write_end_checksum(uint64_t init)
	{
		ASSERT(std::distance(m_begin, m_end) >= 8);
		const uint64_t hash_data = XXH64(
			m_begin,
			num_bytes_written(),
			init
		);
		reinterpret_cast<uint64_t*>(m_end - 8)[0] = hash_data;
	}

	void zero_align(size_t alignment)
	{
		const size_t misalignment = (m_caret - m_begin) % alignment;
		if (misalignment == 0)
			return;

		const size_t missing_bytes = alignment - misalignment;
		ASSERT(m_caret + missing_bytes <= m_end);

		std::memset(m_caret, 0, missing_bytes);
		m_caret += missing_bytes;
	}

	NODISCARD size_t num_bytes_written() const
	{
		return m_caret - m_begin;
	}

	NODISCARD uint8_t* caret()
	{
		return m_caret;
	}

	NODISCARD const uint8_t* caret() const
	{
		return m_caret;
	}

	NODISCARD uint8_t* begin()
	{
		return m_begin;
	}

	NODISCARD const uint8_t* begin() const
	{
		return m_begin;
	}

	NODISCARD uint8_t* end()
	{
		return m_end;
	}

	NODISCARD const uint8_t* end() const
	{
		return m_end;
	}

private:
	uint8_t* m_caret;
	uint8_t* m_begin;
	uint8_t* m_end;
};

struct Serial_Memory_Reader
{
	Serial_Memory_Reader(Const_Span<uint8_t> span) :
		m_caret(span.begin()),
		m_begin(span.begin()),
		m_end(span.end())
	{}

	template <typename T>
	NODISCARD T read()
	{
		T val;
		ASSERT(m_caret + sizeof(val) <= m_end);
		std::memcpy(&val, m_caret, sizeof(val));
		m_caret += sizeof(val);
		return val;
	}

	void read(Span<uint8_t> dst)
	{
		ASSERT(m_caret + dst.size() <= m_end);
		std::memcpy(dst.begin(), m_caret, dst.size());
		m_caret += dst.size();
	}

	NODISCARD bool is_end_checksum_ok(uint64_t init) const
	{
		if (std::distance(m_begin, m_end) < 8)
			return false;
		const uint64_t crc = reinterpret_cast<const uint64_t*>(m_end - 8)[0];
		return XXH64(m_begin, std::distance(m_begin, m_end) - 8, init) == crc;
	}

	void advance(size_t size)
	{
		ASSERT(m_caret + size <= m_end);
		m_caret += size;
	}

	void align(size_t alignment)
	{
		const size_t misalignment = (m_caret - m_begin) % alignment;
		if (misalignment == 0)
			return;

		ASSERT(m_caret + (alignment - misalignment) <= m_end);
		m_caret += alignment - misalignment;
	}

	NODISCARD size_t num_bytes_read() const
	{
		return m_caret - m_begin;
	}

	NODISCARD const uint8_t* caret() const
	{
		return m_caret;
	}

	NODISCARD const uint8_t* begin() const
	{
		return m_begin;
	}

	NODISCARD const uint8_t* end() const
	{
		return m_end;
	}

private:
	const uint8_t* m_caret;
	const uint8_t* m_begin;
	const uint8_t* m_end;
};
