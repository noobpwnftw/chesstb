#pragma once

#include "defines.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr size_t DEFAULT_BLOCK_CACHE_BYTES = 64ull * 1024 * 1024;
constexpr size_t TL_BLOCK_CACHE_SLOTS = 128;
constexpr size_t TL_OBJECT_CACHE_SLOTS = 16;
static_assert((TL_BLOCK_CACHE_SLOTS & (TL_BLOCK_CACHE_SLOTS - 1)) == 0);
static_assert((TL_OBJECT_CACHE_SLOTS & (TL_OBJECT_CACHE_SLOTS - 1)) == 0);

INLINE uint64_t next_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

// Byte-budgeted LRU of decoded blocks, shared by every table. Eviction reclaims
// across tables, so one lock covers the whole structure; dropping an entry only
// releases the pool's reference, never a live reader's.
// Both lookups try_lock: blocks are immutable deterministic decodes, so a caller
// that finds the pool busy decodes its own copy rather than waiting.
class Block_Pool
{
public:
	// Null if not pooled, or if the pool was busy.
	NODISCARD Block_Ptr try_find(const void* owner, size_t block_id)
	{
		std::unique_lock<std::mutex> lk(m_mu, std::try_to_lock);
		if (!lk.owns_lock()) return nullptr;
		auto it = m_index.find(Key{ owner, block_id });
		if (it == m_index.end()) return nullptr;
		m_lru.splice(m_lru.end(), m_lru, it->second);
		return it->second->blk;
	}

	// Returns the winner's copy if another thread decoded the same block first, or
	// `blk` unpublished if the pool was busy.
	NODISCARD Block_Ptr try_insert_or_get(const void* owner, size_t block_id, Block_Ptr blk)
	{
		const Key key{ owner, block_id };
		const size_t bytes = blk->size();

		std::unique_lock<std::mutex> lk(m_mu, std::try_to_lock);
		if (!lk.owns_lock()) return blk;

		auto it = m_index.find(key);
		if (it != m_index.end())
		{
			m_lru.splice(m_lru.end(), m_lru, it->second);
			return it->second->blk;
		}

		// Index last: a throwing node allocation must not leave the index holding
		// an iterator to a node that was never linked.
		m_lru.push_back(Entry{ key, blk, bytes });
		m_index.emplace(key, std::prev(m_lru.end()));
		m_cur_bytes += bytes;
		evict_locked();
		return blk;
	}

	// Keys are owner addresses, so a dead table's blocks must go with it or a
	// later allocation at the same address would alias them.
	void drop_owner(const void* owner)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		for (auto it = m_lru.begin(); it != m_lru.end(); )
		{
			if (it->key.first != owner) { ++it; continue; }
			m_cur_bytes -= it->bytes;
			m_index.erase(it->key);
			it = m_lru.erase(it);
		}
	}

	void set_max_bytes(size_t n)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_max_bytes = n;
		evict_locked();
	}

	NODISCARD size_t max_bytes() const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		return m_max_bytes;
	}

	NODISCARD size_t cur_bytes() const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		return m_cur_bytes;
	}

private:
	using Key = std::pair<const void*, size_t>;

	struct Key_Hash
	{
		size_t operator()(const Key& k) const
		{
			return std::hash<const void*>()(k.first) * 1099511628211ull + k.second;
		}
	};

	struct Entry
	{
		Key key;
		Block_Ptr blk;
		size_t bytes;
	};

	void evict_locked()
	{
		while (m_cur_bytes > m_max_bytes && m_lru.size() > 1)
		{
			const Entry& e = m_lru.front();
			m_cur_bytes -= e.bytes;
			m_index.erase(e.key);
			m_lru.pop_front();
		}
	}

	mutable std::mutex m_mu;
	size_t m_max_bytes = DEFAULT_BLOCK_CACHE_BYTES;
	size_t m_cur_bytes = 0;
	std::list<Entry> m_lru;  // front = least recently used
	std::unordered_map<Key, std::list<Entry>::iterator, Key_Hash> m_index;
};

// Pool used by caches whose owner doesn't inject one (the generator-side probe
// readers in egtb_probe.h own their caches directly). Held by shared_ptr so it
// outlives any cache referencing it, including through thread_local teardown.
INLINE const std::shared_ptr<Block_Pool>& default_block_pool()
{
	static const std::shared_ptr<Block_Pool> pool = std::make_shared<Block_Pool>();
	return pool;
}

struct TL_Block_FIFO
{
	static constexpr size_t N = TL_BLOCK_CACHE_SLOTS;
	uint64_t epoch[N] = {};
	size_t block_id[N] = {};
	Block_Ptr bytes[N];
	size_t next = 0;
};

// `V` is shared_ptr where a hit must keep its object alive past an invalidation.
template <typename T, typename K = uint32_t, typename V = T*>
struct TL_Cache
{
	static constexpr size_t N = TL_OBJECT_CACHE_SLOTS;
	uint64_t epoch[N] = {};
	K key[N] = {};
	V val[N] = {};
	size_t rr = 0;

	// The slot retains ownership; callers read through the returned pointer rather
	// than copying the value out, so a shared_ptr V costs no refcount traffic.
	NODISCARD const V* find(uint64_t impl_epoch, K k) const
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				return &val[i];
		return nullptr;
	}

	void insert(uint64_t impl_epoch, K k, V v)
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch && key[i] == k)
				{ val[i] = std::move(v); return; }
		const size_t i = (rr++) & (N - 1);
		epoch[i] = impl_epoch;
		key[i] = k;
		val[i] = std::move(v);
	}
};

// Tag for decoders that carry nothing across calls, so there is no per-thread
// state to keep; selects the empty Worker_Decompressors specialization below.
struct Stateless_Decompressor {};

// Only for decoders carrying state across calls: LZ4 keeps the table's dictionary
// at the head of its own output buffer, so each thread needs a private copy.
template <typename Decompressor>
struct Worker_Decompressors
{
	// Thread-owned, so no lock; the reference is valid until this thread's next
	// call. One helper per table, so the epoch alone keys it and epochs start at 1,
	// which is what keeps an unused slot from matching.
	template <typename Make>
	Decompressor& decomp_slot(uint64_t epoch, Make&& make)
	{
		thread_local TL_Cache<Decompressor, uint32_t, std::unique_ptr<Decompressor>> tl;
		if (const std::unique_ptr<Decompressor>* hit = tl.find(epoch, 0))
			return **hit;

		std::unique_ptr<Decompressor> dc = make();
		Decompressor& ref = *dc;
		tl.insert(epoch, 0, std::move(dc));
		return ref;
	}
};

template <>
struct Worker_Decompressors<Stateless_Decompressor>
{
};

// Per-color identity for the shared block pool. Stateless decoders (LZMA — every
// table but WDL) instantiate `Block_Cache<>`; see decompress_scratch.
template <typename Decompressor = Stateless_Decompressor>
struct Block_Cache : Worker_Decompressors<Decompressor>
{
	const uint64_t epoch = next_epoch();

	// Shared ownership: a thread_local cache can hold its owner past the lifetime
	// of whatever opened it, and ~Block_Cache still has to reach the pool. Falls
	// back to the default pool rather than ever holding null.
	const std::shared_ptr<Block_Pool> pool;

	explicit Block_Cache(std::shared_ptr<Block_Pool> p = default_block_pool()) :
		pool(p ? std::move(p) : default_block_pool())
	{}

	~Block_Cache()
	{
		pool->drop_owner(this);
	}

	Block_Cache(const Block_Cache&) = delete;
	Block_Cache& operator=(const Block_Cache&) = delete;

	// Identity in the pool, taken from this subobject so it matches what
	// ~Block_Cache passes. Per_Color isn't standard-layout, so the derived
	// address is only the same by ABI convention, not by guarantee.
	NODISCARD const void* pool_key() const { return this; }

	// This worker's decompressor, built via `make` on first use.
	template <typename Make>
	Decompressor& decomp_for(Make&& make)
	{
		return this->decomp_slot(epoch, std::forward<Make>(make));
	}
};

// One FIFO per `Tag` per thread. Deducing the owner from `Cache`/`Build` instead
// would be accidental: two table types sharing a cache type, or two build
// functions with the same signature, silently collapse onto one FIFO.
template <typename Tag>
NODISCARD INLINE TL_Block_FIFO& tl_block_fifo()
{
	thread_local TL_Block_FIFO tl;
	return tl;
}

// Thread-local fast path in front of the shared pool: a hit takes no lock, and
// `build` runs unlocked so workers decode in parallel. `Tag` names the table type
// the FIFO belongs to; entries are keyed by the cache's epoch, and the slot's
// reference is what keeps the returned pointer valid against eviction — and is
// the sole owner of a block the pool was too busy to accept.
template <typename Tag, typename Cache, typename Build>
INLINE const uint8_t* fetch_block_cached(Cache& cache, size_t block_id, Build&& build)
{
	TL_Block_FIFO& tl = tl_block_fifo<Tag>();
	for (size_t i = 0; i < TL_Block_FIFO::N; ++i)
		if (tl.epoch[i] == cache.epoch && tl.block_id[i] == block_id)
			return tl.bytes[i]->data();

	Block_Ptr blk = cache.pool->try_find(cache.pool_key(), block_id);
	if (!blk)
		blk = cache.pool->try_insert_or_get(cache.pool_key(), block_id, build(cache, block_id));

	const size_t s = (tl.next++) & (TL_Block_FIFO::N - 1);
	tl.epoch[s]    = cache.epoch;
	tl.block_id[s] = block_id;
	tl.bytes[s]    = blk;
	return blk->data();
}
