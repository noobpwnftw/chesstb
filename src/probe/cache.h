#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

constexpr size_t BLOCK_CACHE_SLOTS = 32;

inline uint64_t next_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

inline Block_Ptr find_cached_block(
	const std::array<size_t, BLOCK_CACHE_SLOTS>& block_ids,
	const std::array<Block_Ptr, BLOCK_CACHE_SLOTS>& blocks,
	size_t live,
	size_t block_id)
{
	for (size_t i = 0; i < live; ++i)
		if (block_ids[i] == block_id) return blocks[i];
	return nullptr;
}

inline size_t next_cache_slot(size_t& live, size_t& next_slot)
{
	if (live < BLOCK_CACHE_SLOTS) return live++;
	const size_t slot = next_slot;
	next_slot = (next_slot + 1) % BLOCK_CACHE_SLOTS;
	return slot;
}

struct TL_Block_FIFO
{
	static constexpr size_t N = 8;
	uint64_t epoch[N] = {};
	size_t block_id[N] = {};
	Block_Ptr bytes[N];
	size_t next = 0;
};

// Shared per-color block cache: a small set of decompressed blocks plus the
// lazily-built decompressor. `Decompressor` is LZ4 for WDL, LZMA otherwise.
template <typename Decompressor>
struct Block_Cache
{
	const uint64_t epoch = next_epoch();
	mutable std::mutex mu;
	std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
	std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
	size_t next_slot = 0;
	size_t live = 0;
	std::unique_ptr<Decompressor> decomp;
};

// Thread-local fast path in front of the locked per-color cache. `get_locked`
// produces the block under `cache.mu` on a miss. Entries are keyed by the
// cache's unique epoch, so one FIFO safely serves every cache of its type.
template <typename Cache, typename GetLocked>
inline const uint8_t* fetch_block_cached(Cache& cache, size_t block_id, GetLocked&& get_locked)
{
	thread_local TL_Block_FIFO tl;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == cache.epoch && tl.block_id[i] == block_id)
			return tl.bytes[i]->data();

	std::lock_guard<std::mutex> lk(cache.mu);
	Block_Ptr blk = get_locked(cache, block_id);
	const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
	tl.epoch[s]    = cache.epoch;
	tl.block_id[s] = block_id;
	tl.bytes[s]    = blk;
	return blk->data();
}

template <typename T, typename K = uint32_t>
struct TL_Cache
{
	static constexpr size_t N = 4;
	uint64_t epoch[N] = {};
	K key[N] = {};
	T* val[N] = {};
	size_t rr = 0;

	bool lookup(uint64_t impl_epoch, K k, T*& out) const
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				{ out = val[i]; return true; }
		return false;
	}

	void insert(uint64_t impl_epoch, K k, T* v)
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				{ val[i] = v; return; }
		const size_t i = (rr++) & (N - 1);
		epoch[i] = impl_epoch;
		key[i] = k;
		val[i] = v;
	}
};
