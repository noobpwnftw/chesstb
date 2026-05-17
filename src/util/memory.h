#pragma once

#include "defines.h"
#include "span.h"

#include "zstd/common/xxhash.h"

struct Serial_Memory_Writer
{
	Serial_Memory_Writer(Span<uint8_t> span) :
		m_caret(span.begin()),
		m_begin(span.begin()),
		m_end(span.end())
	{}

	// Prevent type deduction so that it must be specified explicitly.
	// This is to avoid mistakes where the type passed is not desired.
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