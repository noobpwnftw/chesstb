#pragma once

#include "util/defines.h"

#include <filesystem>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

class Fleet_Lock
{
public:
	Fleet_Lock() = default;
	explicit Fleet_Lock(std::filesystem::path path)
		: m_path(std::move(path))
	{
		m_fd = ::open(m_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
		if (m_fd < 0) return;
		if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0)
		{
			::close(m_fd);
			m_fd = -1;
		}
	}
	~Fleet_Lock() { release(); }

	Fleet_Lock(Fleet_Lock&& other) noexcept
		: m_path(std::move(other.m_path)), m_fd(other.m_fd)
	{
		other.m_fd = -1;
	}
	Fleet_Lock& operator=(Fleet_Lock&& other) noexcept
	{
		if (this != &other)
		{
			release();
			m_path = std::move(other.m_path);
			m_fd = other.m_fd;
			other.m_fd = -1;
		}
		return *this;
	}

	Fleet_Lock(const Fleet_Lock&) = delete;
	Fleet_Lock& operator=(const Fleet_Lock&) = delete;

	NODISCARD bool held() const { return m_fd >= 0; }

	void remove_file()
	{
		if (m_path.empty()) return;
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
	}

private:
	void release()
	{
		if (m_fd < 0) return;
		::flock(m_fd, LOCK_UN);
		::close(m_fd);
		m_fd = -1;
	}

	std::filesystem::path m_path;
	int m_fd = -1;
};
