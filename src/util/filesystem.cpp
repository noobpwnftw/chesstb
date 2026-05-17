#include "filesystem.h"

#include "system/system.h"

#include "util/utility.h"

#if defined(OS_WINDOWS)

#include <Windows.h>

#elif defined(OS_LINUX)

#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#else

#error "Unsupported OS"

#endif

bool Memory_Mapped_File::open_readonly(const char* file_name)
{
#if defined(OS_WINDOWS)

	m_handle = CreateFileA(
		file_name,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
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
	m_handle = ::open(file_name, O_RDONLY);
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	fstat(m_handle, &statbuf);
	m_size = statbuf.st_size;
	m_data = reinterpret_cast<uint8_t*>(mmap(NULL, m_size, PROT_READ, MAP_PRIVATE, m_handle, 0));

	if (m_data == MAP_FAILED)
		print_and_abort("Could not mmap() %s\n", file_name);

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
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | (m_advise == Access_Advice::RANDOM ? FILE_FLAG_RANDOM_ACCESS : 0),
		NULL
	);

	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	std::filesystem::resize_file(file_name, size);
	m_size = std::filesystem::file_size(file_name);
	if (m_size != size)
		print_and_abort("Could not allocate() %s\n", file_name);
	
	const HANDLE fm = CreateFileMappingW(m_handle, NULL, PAGE_READWRITE, 0, 0, NULL);
	m_data = reinterpret_cast<uint8_t*>(MapViewOfFile(fm, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));

	if (m_data == nullptr)
		print_and_abort("Could not mmap() %s\n", file_name);

	CloseHandle(fm);

	return true;

#elif defined(OS_LINUX)

	m_handle = ::open(file_name, O_CREAT | O_RDWR | O_TRUNC, (mode_t)0644);
	if (m_handle == sys_common::INVALID_HANDLE_VALUE)
		return false;

	m_size = size;
	if (posix_fallocate(m_handle, 0, size) != 0)
	{
		if (lseek64(m_handle, size - 1, SEEK_SET) == -1 || write(m_handle, "", 1) == -1)
			print_and_abort("Could not allocate() %s\n", file_name);
	}
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

void Memory_Mapped_File::close()
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
		::close(m_handle);
		m_handle = sys_common::INVALID_HANDLE_VALUE;
	}

#else

#error "Unsupported OS"

#endif
}
