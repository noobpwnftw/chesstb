#include "allocation.h"

#include "defines.h"

#include "system/system.h"

#if defined(OS_WINDOWS)

#include <Windows.h>

#elif defined(OS_LINUX)

#include <cstdlib>
#include <sys/mman.h>

#else

#error "Unsupported OS"

#endif

#include <cstdio>

NODISCARD void* allocate_large_pages(size_t bytes)
{
#if defined(OS_WINDOWS)

	static size_t s_large_page_size = []() -> size_t {
		const size_t large_page_size = GetLargePageMinimum();
		if (large_page_size == 0)
			goto no_large_pages;

		HANDLE hProcessToken;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hProcessToken))
			goto no_large_pages;

		TOKEN_PRIVILEGES tp;
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid))
			goto no_large_pages;

		if (!AdjustTokenPrivileges(hProcessToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr))
		{
			CloseHandle(hProcessToken);
			goto no_large_pages;
		}

		CloseHandle(hProcessToken);

		printf("INFO: Using large pages.\n");
		return large_page_size;

	no_large_pages:
		printf("WARNING: Not using large pages.\n");
		return 0;
	}();

	const size_t large_page_size = s_large_page_size;

	if (large_page_size == 0)
		return nullptr;

	const size_t allocation_size = ceil_to_multiple(bytes, large_page_size);
	return VirtualAlloc(nullptr, allocation_size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);

#elif defined(OS_LINUX)

	if (bytes == 0)
		return nullptr;

	const size_t allocation_size = large_page_bytes(bytes);
	void* const ptr = mmap(nullptr, allocation_size, PROT_READ | PROT_WRITE,
	                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED)
		return nullptr;
	madvise(ptr, allocation_size, MADV_HUGEPAGE);
	return ptr;
#else

#error "Unsupported OS"

#endif
}

void deallocate_large_pages(void* ptr, size_t bytes)
{
#if defined(OS_WINDOWS)

	(void)bytes;
	if (ptr != nullptr)
		VirtualFree(ptr, 0, MEM_RELEASE);

#elif defined(OS_LINUX)

	if (ptr != nullptr)
		munmap(ptr, large_page_bytes(bytes));

#else

#error "Unsupported OS"

#endif
}