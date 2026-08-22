#pragma once

#include "defines.h"

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

inline constexpr size_t DEFAULT_BLOCK_CACHE_BYTES = 64ull * 1024 * 1024;
inline constexpr size_t TL_OBJECT_CACHE_SLOTS = 16;
static_assert((TL_OBJECT_CACHE_SLOTS & (TL_OBJECT_CACHE_SLOTS - 1)) == 0);
static_assert(TL_OBJECT_CACHE_SLOTS >= 1);

INLINE uint64_t next_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

// Byte-budgeted CLOCK cache of decoded blocks, shared by every table. Eviction
// reclaims across tables, so one lock covers the whole structure; dropping an
// entry only releases the pool's reference, never a live reader's.
// A lookup marks its entry instead of reordering, so it takes the shared side
// and waits only behind a writer. An insert try_locks instead: it runs eviction,
// and blocks being immutable deterministic decodes, a caller that finds the pool
// busy keeps its own copy rather than queueing behind a sweep.
class Block_Pool
{
public:
	// Null if not pooled.
	NODISCARD Block_Ptr find(const void* owner, size_t block_id)
	{
		std::shared_lock<std::shared_mutex> lk(m_mu);
		auto it = m_index.find(Key{ owner, block_id });
		if (it == m_index.end()) return nullptr;
		it->second->used.store(true, std::memory_order_relaxed);
		return it->second->blk;
	}

	// Returns the winner's copy if another thread decoded the same block first, or
	// `blk` unpublished if the pool was busy.
	NODISCARD Block_Ptr try_insert_or_get(const void* owner, size_t block_id, Block_Ptr blk)
	{
		const Key key{ owner, block_id };
		const size_t bytes = blk->size();

		std::unique_lock<std::shared_mutex> lk(m_mu, std::try_to_lock);
		if (!lk.owns_lock()) return blk;

		auto it = m_index.find(key);
		if (it != m_index.end())
		{
			it->second->used.store(true, std::memory_order_relaxed);
			return it->second->blk;
		}

		// Index last: a throwing node allocation must not leave the index holding
		// an iterator to a node that was never linked.
		m_ring.emplace_back();
		Entry& e = m_ring.back();
		e.key = key;
		e.blk = blk;
		e.bytes = bytes;
		m_index.emplace(key, std::prev(m_ring.end()));
		m_cur_bytes += bytes;
		evict_locked();
		return blk;
	}

	// Keys are owner addresses, so a dead table's blocks must go with it or a
	// later allocation at the same address would alias them.
	void drop_owner(const void* owner)
	{
		std::lock_guard<std::shared_mutex> lk(m_mu);
		for (auto it = m_ring.begin(); it != m_ring.end(); )
		{
			if (it->key.first != owner) { ++it; continue; }
			m_cur_bytes -= it->bytes;
			m_index.erase(it->key);
			it = m_ring.erase(it);
		}
	}

	void set_max_bytes(size_t n)
	{
		std::lock_guard<std::shared_mutex> lk(m_mu);
		m_max_bytes = n;
		evict_locked();
	}

	NODISCARD size_t max_bytes() const
	{
		std::shared_lock<std::shared_mutex> lk(m_mu);
		return m_max_bytes;
	}

	NODISCARD size_t cur_bytes() const
	{
		std::shared_lock<std::shared_mutex> lk(m_mu);
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
		size_t bytes = 0;
		std::atomic<bool> used{ false };
	};

	// The hand clears a mark and moves that entry behind it, so one pass over the
	// ring reprieves each entry at most once before it comes back unmarked.
	void evict_locked()
	{
		while (m_cur_bytes > m_max_bytes && m_ring.size() > 1)
		{
			Entry& e = m_ring.front();
			if (e.used.exchange(false, std::memory_order_relaxed))
			{
				m_ring.splice(m_ring.end(), m_ring, m_ring.begin());
				continue;
			}
			m_cur_bytes -= e.bytes;
			m_index.erase(e.key);
			m_ring.pop_front();
		}
	}

	mutable std::shared_mutex m_mu;
	size_t m_max_bytes = DEFAULT_BLOCK_CACHE_BYTES;
	size_t m_cur_bytes = 0;
	std::list<Entry> m_ring;  // front = the hand
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

// `N` is a thread's reference window, which each tag names for itself. Deep
// enough to cover the blocks it comes back to, no deeper: a miss costs a shared
// lookup, or one duplicate decode when the pool is mid-insert, while every slot
// pins a block past the pool's reclaim and find() scans them all.
template <size_t N>
struct TL_Block_FIFO
{
	static_assert(N >= 1);
	static_assert((N & (N - 1)) == 0);

	static constexpr size_t slots = N;
	uint64_t epoch[N] = {};
	size_t block_id[N] = {};
	Block_Ptr bytes[N];
	size_t next = 0;
};

// `V` is shared_ptr where a hit must keep its object alive past an invalidation.
template <typename T, typename K, typename V>
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

// `K` void where the epoch is the whole identity: one value per table, so a
// thread holds up to N tables' worth and nothing distinguishes them further.
template <typename T, typename V>
struct TL_Cache<T, void, V>
{
	static constexpr size_t N = TL_OBJECT_CACHE_SLOTS;
	uint64_t epoch[N] = {};
	V val[N] = {};
	size_t rr = 0;

	NODISCARD const V* find(uint64_t impl_epoch) const
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch)
				return &val[i];
		return nullptr;
	}

	void insert(uint64_t impl_epoch, V v)
	{
		for (size_t i = 0; i < N; ++i)
			if (epoch[i] == impl_epoch)
				{ val[i] = std::move(v); return; }
		const size_t i = (rr++) & (N - 1);
		epoch[i] = impl_epoch;
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
	// call. Epochs start at 1, which is what keeps an unused slot from matching.
	template <typename Make>
	Decompressor& decomp_slot(uint64_t epoch, Make&& make)
	{
		thread_local TL_Cache<Decompressor, void, std::unique_ptr<Decompressor>> tl;
		if (const std::unique_ptr<Decompressor>* hit = tl.find(epoch))
			return **hit;

		std::unique_ptr<Decompressor> dc = make();
		Decompressor& ref = *dc;
		tl.insert(epoch, std::move(dc));
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
NODISCARD TL_Block_FIFO<Tag::TL_BLOCK_SLOTS>& tl_block_fifo()
{
	thread_local TL_Block_FIFO<Tag::TL_BLOCK_SLOTS> tl;
	return tl;
}

// Thread-local fast path in front of the shared pool: a hit takes no lock, and
// `build` runs unlocked so workers decode in parallel. `Tag` names the table type
// the FIFO belongs to; entries are keyed by the cache's epoch, and the slot's
// reference is what keeps the returned pointer valid against eviction — and is
// the sole owner of a block the pool was too busy to accept.
template <typename Tag, typename Cache, typename Build>
const uint8_t* fetch_block_cached(Cache& cache, size_t block_id, Build&& build)
{
	auto& tl = tl_block_fifo<Tag>();
	for (size_t i = 0; i < tl.slots; ++i)
		if (tl.epoch[i] == cache.epoch && tl.block_id[i] == block_id)
			return tl.bytes[i]->data();

	Block_Ptr blk = cache.pool->find(cache.pool_key(), block_id);
	if (!blk)
		blk = cache.pool->try_insert_or_get(cache.pool_key(), block_id, build(cache, block_id));

	const size_t s = (tl.next++) & (tl.slots - 1);
	tl.epoch[s]    = cache.epoch;
	tl.block_id[s] = block_id;
	tl.bytes[s]    = blk;
	return blk->data();
}
