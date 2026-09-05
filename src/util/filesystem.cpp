#include "filesystem.h"

#include "system/system.h"

#include "util/utility.h"

#include <algorithm>
#include <cerrno>

#if defined(OS_WINDOWS)

#include <Windows.h>

#elif defined(OS_LINUX)

#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

#else

#error "Unsupported OS"

#endif

bool Positional_Output_File::create(const char* file_name)
{
#if defined(OS_WINDOWS)

	m_handle = CreateFileA(
		file_name,
		GENERIC_WRITE,
		FILE_SHARE_READ,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);

	return m_handle != sys_common::INVALID_HANDLE_VALUE;

#elif defined(OS_LINUX)

	m_handle = open(file_name, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, (mode_t)0644);

	return m_handle != sys_common::INVALID_HANDLE_VALUE;

#else

#error "Unsupported OS"

#endif
}

bool Positional_Output_File::write_at(uint64_t offset, Const_Span<uint8_t> data) const
{
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	const uint8_t* p = data.data();
	size_t left = data.size();

#if defined(OS_WINDOWS)

	while (left != 0)
	{
		OVERLAPPED ov{};
		ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
		ov.OffsetHigh = static_cast<DWORD>(offset >> 32);

		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 30));
		DWORD written = 0;
		if (!WriteFile(m_handle, p, chunk, &written, &ov) || written == 0)
			return false;

		p += written;
		left -= written;
		offset += written;
	}

	return true;

#elif defined(OS_LINUX)

	while (left != 0)
	{
		const ssize_t written = pwrite(m_handle, p, left, static_cast<off_t>(offset));
		if (written < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		if (written == 0)
			return false;

		p += written;
		left -= static_cast<size_t>(written);
		offset += static_cast<uint64_t>(written);
	}

	return true;

#else

#error "Unsupported OS"

#endif
}

bool Positional_Output_File::flush() const
{
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

#if defined(OS_WINDOWS)

	return FlushFileBuffers(m_handle) != 0;

#elif defined(OS_LINUX)

	return fdatasync(m_handle) == 0;

#else

#error "Unsupported OS"

#endif
}

void Positional_Output_File::close_file()
{
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return;

#if defined(OS_WINDOWS)
	CloseHandle(m_handle);
#elif defined(OS_LINUX)
	close(m_handle);
#else
#error "Unsupported OS"
#endif

	m_handle = sys_common::INVALID_HANDLE_VALUE;
}

bool Memory_Mapped_File::open_readonly(const char* file_name)
{
#if defined(OS_WINDOWS)

	m_handle = CreateFileA(
		file_name,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | (m_advise == Access_Advice::RANDOM ? FILE_FLAG_RANDOM_ACCESS : 0),
		NULL
	);

	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	m_size = std::filesystem::file_size(file_name);
	const HANDLE fm = CreateFileMappingW(m_handle, NULL, PAGE_READONLY, 0, 0, NULL);
	m_data = reinterpret_cast<uint8_t*>(MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0));

	if (m_data == nullptr)
		print_and_abort("Could not mmap() %s\n", file_name);

	CloseHandle(fm);

	return true;

#elif defined(OS_LINUX)

	struct stat statbuf;
	m_handle = open(file_name, O_RDONLY);
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	fstat(m_handle, &statbuf);
	m_size = statbuf.st_size;
	m_data = reinterpret_cast<uint8_t*>(mmap(NULL, m_size, PROT_READ, MAP_PRIVATE, m_handle, 0));

	if (m_data == MAP_FAILED)
		print_and_abort("Could not mmap() %s\n", file_name);

	if (m_advise == Access_Advice::RANDOM)
		madvise(m_data, m_size, MADV_RANDOM);

	return true;

#else

#error "Unsupported OS"

#endif
}

bool Memory_Mapped_File::create(const char* file_name, size_t size)
{
#if defined(OS_WINDOWS)

	m_handle = CreateFileA(
		file_name,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | (m_advise == Access_Advice::RANDOM ? FILE_FLAG_RANDOM_ACCESS : 0),
		NULL
	);

	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	OVERLAPPED ov{};
	if (!LockFileEx(m_handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
		0, MAXDWORD, MAXDWORD, &ov))
	{
		CloseHandle(m_handle);
		m_handle = sys_common::INVALID_HANDLE_VALUE;
		return false;
	}

	LARGE_INTEGER li;
	li.QuadPart = static_cast<LONGLONG>(size);
	if (!SetFilePointerEx(m_handle, li, NULL, FILE_BEGIN) || !SetEndOfFile(m_handle))
		print_and_abort("Could not resize %s\n", file_name);
	m_size = size;

	const HANDLE fm = CreateFileMappingW(m_handle, NULL, PAGE_READWRITE, 0, 0, NULL);
	m_data = reinterpret_cast<uint8_t*>(MapViewOfFile(fm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));

	if (m_data == nullptr)
		print_and_abort("Could not mmap() %s\n", file_name);

	CloseHandle(fm);

	return true;

#elif defined(OS_LINUX)

	m_handle = open(file_name, O_CREAT | O_RDWR, (mode_t)0644);
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	if (flock(m_handle, LOCK_EX | LOCK_NB) != 0)
	{
		close(m_handle);
		m_handle = sys_common::INVALID_HANDLE_VALUE;
		return false;
	}

	if (ftruncate(m_handle, size) != 0)
		print_and_abort("Could not resize %s\n", file_name);
	if (posix_fallocate(m_handle, 0, size) != 0)
	{
		if (lseek64(m_handle, size - 1, SEEK_SET) == -1 || write(m_handle, "", 1) == -1)
			print_and_abort("Could not allocate() %s\n", file_name);
	}
	m_size = size;
	m_data = reinterpret_cast<uint8_t*>(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_handle, 0));

	if (m_data == MAP_FAILED)
		print_and_abort("Could not mmap() %s\n", file_name);

	if (m_advise == Access_Advice::RANDOM)
		madvise(m_data, m_size, MADV_RANDOM);

	return true;

#else

#error "Unsupported OS"

#endif
}

void Memory_Mapped_File::close_file()
{
#if defined(OS_WINDOWS)

	if (m_data != nullptr)
	{
		UnmapViewOfFile(m_data);
		m_data = nullptr;
	}

	if (m_handle != sys_common::INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_handle);
		m_handle = sys_common::INVALID_HANDLE_VALUE;
	}

#elif defined(OS_LINUX)

	if (m_advise == Access_Advice::RANDOM)
		madvise(m_data, m_size, MADV_NORMAL);

	if (m_data != nullptr)
	{
		munmap(m_data, m_size);
		m_data = nullptr;
	}

	if (m_handle != sys_common::INVALID_HANDLE_VALUE)
	{
		close(m_handle);
		m_handle = sys_common::INVALID_HANDLE_VALUE;
	}

#else

#error "Unsupported OS"

#endif

	m_size = 0;
}
