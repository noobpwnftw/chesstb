#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

constexpr size_t BLOCK_CACHE_SLOTS = 512;
constexpr size_t TL_BLOCK_CACHE_SLOTS = 128;
constexpr size_t TL_OBJECT_CACHE_SLOTS = 16;
static_assert((TL_BLOCK_CACHE_SLOTS & (TL_BLOCK_CACHE_SLOTS - 1)) == 0);
static_assert((TL_OBJECT_CACHE_SLOTS & (TL_OBJECT_CACHE_SLOTS - 1)) == 0);

inline uint64_t next_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

// Worker ids index per-cache `decomps` vectors. Ids are recycled via a free
// list so the id space is bounded by *peak concurrent* workers, not by the
// total number of threads ever created — otherwise a process that spawns many
// short-lived probing threads would grow every cache's vector without bound.
inline std::mutex& worker_id_mu()
{
	static std::mutex m;
	return m;
}

inline std::vector<size_t>& worker_id_free()
{
	static std::vector<size_t> f;
	return f;
}

inline size_t alloc_worker_id()
{
	std::lock_guard<std::mutex> lk(worker_id_mu());
	std::vector<size_t>& free = worker_id_free();
	if (!free.empty())
	{
		const size_t id = free.back();
		free.pop_back();
		return id;
	}
	static size_t ctr = 0;
	return ctr++;
}

inline void free_worker_id(size_t id)
{
	std::lock_guard<std::mutex> lk(worker_id_mu());
	worker_id_free().push_back(id);
}

inline size_t worker_index()
{
	// The thread_local's destructor returns the id to the free list on thread
	// exit. A reused id inherits the prior owner's decompressor slots, which is
	// safe: at any instant only one live thread holds a given id.
	struct Guard
	{
		size_t id = alloc_worker_id();
		~Guard() { free_worker_id(id); }
	};
	thread_local Guard g;
	return g.id;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

inline size_t next_cache_slot(size_t& live, size_t& next_slot)
{
	if (live < BLOCK_CACHE_SLOTS) return live++;
	const size_t slot = next_slot;
	next_slot = (next_slot + 1) % BLOCK_CACHE_SLOTS;
	return slot;
}

struct TL_Block_FIFO
{
	static constexpr size_t N = TL_BLOCK_CACHE_SLOTS;
	uint64_t epoch[N] = {};
	size_t block_id[N] = {};
	Block_Ptr bytes[N];
	size_t next = 0;
};

template <typename T, typename K = uint32_t>
struct TL_Cache
{
	static constexpr size_t N = TL_OBJECT_CACHE_SLOTS;
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

// Shared per-color block cache: a few decompressed blocks plus one decompressor
// per worker (LZ4 for WDL, LZMA otherwise). Decompressors aren't thread-safe —
// each writes its own internal buffer — so per-worker copies let decode run
// without holding `mu`.
template <typename Decompressor>
struct Block_Cache
{
	const uint64_t epoch = next_epoch();
	mutable std::mutex mu;
	std::array<size_t, BLOCK_CACHE_SLOTS> block_id{};
	std::array<Block_Ptr, BLOCK_CACHE_SLOTS> data;
	std::unordered_map<size_t, size_t> block_slot;
	size_t next_slot = 0;
	size_t live = 0;

	// Indexed by worker_index(); unique_ptr keeps each helper's address stable
	// across vector growth, and only its owning worker ever touches a slot.
	mutable std::mutex decomp_mu;
	std::vector<std::unique_ptr<Decompressor>> decomps;

	Block_Cache()
	{
		block_slot.reserve(BLOCK_CACHE_SLOTS * 2);
	}

	// This worker's decompressor, built via `make` on first use.
	template <typename Make>
	Decompressor& decomp_for(Make&& make)
	{
		const size_t w = worker_index();
		thread_local TL_Cache<Decompressor, size_t> tl_decomps;
		Decompressor* dc = nullptr;
		if (tl_decomps.lookup(epoch, w, dc))
			return *dc;

		std::lock_guard<std::mutex> lk(decomp_mu);
		if (decomps.size() <= w) decomps.resize(w + 1);
		if (!decomps[w]) decomps[w] = make();
		dc = decomps[w].get();
		tl_decomps.insert(epoch, w, dc);
		return *dc;
	}
};

template <typename Cache>
inline Block_Ptr find_cached_block(Cache& cache, size_t block_id)
{
	auto it = cache.block_slot.find(block_id);
	if (it == cache.block_slot.end()) return nullptr;

	const size_t slot = it->second;
	if (slot < cache.live && cache.block_id[slot] == block_id)
		return cache.data[slot];

	cache.block_slot.erase(it);
	return nullptr;
}

template <typename Cache>
inline void insert_cached_block(Cache& cache, size_t block_id, Block_Ptr block)
{
	const size_t slot = next_cache_slot(cache.live, cache.next_slot);
	if (cache.data[slot])
		cache.block_slot.erase(cache.block_id[slot]);

	cache.block_id[slot] = block_id;
	cache.data[slot] = std::move(block);
	cache.block_slot[block_id] = slot;
}

// Thread-local fast path in front of the per-color cache. `mu` is held only to
// look up / insert the shared block array; `build` (decompression) runs
// unlocked, so workers decode in parallel. Concurrent builders of the same
// block race on insert and the loser's copy is discarded. FIFO entries are
// keyed by the cache's epoch, so one FIFO serves every cache of its type.
template <typename Cache, typename Build>
inline const uint8_t* fetch_block_cached(Cache& cache, size_t block_id, Build&& build)
{
	thread_local TL_Block_FIFO tl;
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == cache.epoch && tl.block_id[i] == block_id)
			return tl.bytes[i]->data();

	Block_Ptr blk;
	{
		std::lock_guard<std::mutex> lk(cache.mu);
		blk = find_cached_block(cache, block_id);
	}
	if (!blk)
	{
		blk = build(cache, block_id);

		std::lock_guard<std::mutex> lk(cache.mu);
		if (Block_Ptr raced = find_cached_block(cache, block_id))
		{
			blk = std::move(raced);
		}
		else
		{
			insert_cached_block(cache, block_id, blk);
		}
	}

	const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
	tl.epoch[s]    = cache.epoch;
	tl.block_id[s] = block_id;
	tl.bytes[s]    = blk;
	return blk->data();
}
