#pragma once

#include "defines.h"
#include "span.h"

#include "system/system.h"

#include <filesystem>
#include <vector>
#include <utility>

template <typename HeadT>
NODISCARD inline std::filesystem::path path_join(HeadT&& p)
{
	return std::move(std::forward<HeadT>(p));
}

template <typename HeadT, typename... ArgsTs>
NODISCARD inline std::filesystem::path path_join(HeadT&& head, ArgsTs&&... args)
{
	return std::filesystem::path(std::forward<HeadT>(head)) / path_join(std::forward<ArgsTs>(args)...);
}

struct Temporary_File_Tracker
{
	Temporary_File_Tracker() = default;

	Temporary_File_Tracker(const Temporary_File_Tracker&) = delete;
	Temporary_File_Tracker(Temporary_File_Tracker&&) noexcept = default;

	Temporary_File_Tracker& operator=(const Temporary_File_Tracker&) = delete;
	Temporary_File_Tracker& operator=(Temporary_File_Tracker&&) noexcept = default;

	~Temporary_File_Tracker()
	{
		clear();
	}

	std::filesystem::path& track_path(std::filesystem::path s)
	{
		return m_paths.emplace_back(std::move(s));
	}

	void clear()
	{
		std::error_code ec;
		for (const auto& path : m_paths)
			std::filesystem::remove(path, ec);
		m_paths.clear();
	}

private:
	std::vector<std::filesystem::path> m_paths;
};

struct Memory_Mapped_File
{
	enum struct Access_Advice
	{
		NORMAL,
		RANDOM
	};

	Memory_Mapped_File() :
		m_data(nullptr),
		m_size(0),
		m_handle(sys_common::INVALID_HANDLE_VALUE),
		m_advise(Access_Advice::NORMAL)
	{
	}

	explicit Memory_Mapped_File(Access_Advice access) :
		m_data(nullptr),
		m_size(0),
		m_handle(sys_common::INVALID_HANDLE_VALUE),
		m_advise(access)
	{
	}

	Memory_Mapped_File(const Memory_Mapped_File&) = delete;
	Memory_Mapped_File(Memory_Mapped_File&& other) noexcept :
		m_data(std::exchange(other.m_data, nullptr)),
		m_size(std::exchange(other.m_size, 0)),
		m_handle(std::exchange(other.m_handle, sys_common::INVALID_HANDLE_VALUE)),
		m_advise(std::exchange(other.m_advise, Access_Advice::NORMAL))
	{
	}

	Memory_Mapped_File& operator=(const Memory_Mapped_File&) = delete;
	Memory_Mapped_File& operator=(Memory_Mapped_File&& other) noexcept
	{
		m_data = std::exchange(other.m_data, nullptr);
		m_size = std::exchange(other.m_size, 0);
		m_handle = std::exchange(other.m_handle, sys_common::INVALID_HANDLE_VALUE);
		m_advise = std::exchange(other.m_advise, Access_Advice::NORMAL);
		return *this;
	}

	~Memory_Mapped_File()
	{
		close();
	}

	bool open_readonly(const std::filesystem::path& path)
	{
		const std::string str = path.string();
		return open_readonly(str.c_str());
	}

	bool open_readonly(const char* file_name);

	bool create(const std::filesystem::path& path, size_t size)
	{
		const std::string str = path.string();
		return create(str.c_str(), size);
	}

	bool create(const char* file_name, size_t size);

	void close();

	NODISCARD Span<uint8_t> data_span()
	{
		return Span(m_data, m_size);
	}

	NODISCARD Const_Span<uint8_t> data_span() const
	{
		return Const_Span(m_data, m_size);
	}

	NODISCARD uint8_t* data()
	{
		return m_data;
	}

	NODISCARD const uint8_t* data() const
	{
		return m_data;
	}

	NODISCARD size_t size() const
	{
		return m_size;
	}

private:

	uint8_t* m_data;
	size_t m_size;
	sys_common::Native_Handle m_handle;
	Access_Advice m_advise;
};
