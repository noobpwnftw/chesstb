#pragma once

#include "defines.h"
#include "fixed_vector.h"

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
inline constexpr size_t TL_BLOCK_SHARDS = 64;
static_assert((TL_OBJECT_CACHE_SLOTS & (TL_OBJECT_CACHE_SLOTS - 1)) == 0);
static_assert(TL_OBJECT_CACHE_SLOTS >= 1);
static_assert((TL_BLOCK_SHARDS & (TL_BLOCK_SHARDS - 1)) == 0);

INLINE uint64_t next_epoch()
{
	static std::atomic<uint64_t> ctr{0};
	return ctr.fetch_add(1, std::memory_order_relaxed) + 1;
}

using Block_Ptr = std::shared_ptr<const std::vector<uint8_t>>;

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

INLINE const std::shared_ptr<Block_Pool>& default_block_pool()
{
	static const std::shared_ptr<Block_Pool> pool = std::make_shared<Block_Pool>();
	return pool;
}

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

template <typename T, typename K, typename V>
struct TL_Cache
{
	static constexpr size_t N = TL_OBJECT_CACHE_SLOTS;
	uint64_t epoch[N] = {};
	K key[N] = {};
	V val[N] = {};
	size_t rr = 0;

	NODISCARD const V* find(uint64_t impl_epoch, K k) const
	{
		for (size_t n = 1; n <= N; ++n)
		{
			const size_t i = (rr - n) & (N - 1);
			if (epoch[i] == impl_epoch && key[i] == k)
				return &val[i];
		}
		return nullptr;
	}

	void insert(uint64_t impl_epoch, K k, V v)
	{
		const size_t i = (rr++) & (N - 1);
		epoch[i] = impl_epoch;
		key[i] = k;
		val[i] = std::move(v);
	}
};

struct Block_Cache
{
	const uint64_t epoch = next_epoch();

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

	NODISCARD const void* pool_key() const { return this; }
};

template <typename Tag, bool Sharded>
NODISCARD TL_Block_FIFO<Tag::TL_BLOCK_SLOTS>& tl_block_fifo(uint64_t owner_epoch)
{
	if constexpr (Sharded)
	{
		thread_local Fixed_Vector<TL_Block_FIFO<Tag::TL_BLOCK_SLOTS>, TL_BLOCK_SHARDS>
			tls(TL_BLOCK_SHARDS);
		return tls[owner_epoch & (TL_BLOCK_SHARDS - 1)];
	}
	else
	{
		thread_local TL_Block_FIFO<Tag::TL_BLOCK_SLOTS> tl;
		return tl;
	}
}

template <typename Tag, bool Sharded = false, typename Cache, typename Build>
const uint8_t* fetch_block_cached(Cache& cache, size_t block_id, Build&& build)
{
	auto& tl = tl_block_fifo<Tag, Sharded>(cache.epoch);
	for (size_t n = 1; n <= tl.slots; ++n)
	{
		const size_t i = (tl.next - n) & (tl.slots - 1);
		if (tl.epoch[i] == cache.epoch && tl.block_id[i] == block_id)
			return tl.bytes[i]->data();
	}

	Block_Ptr blk;
	if constexpr (Sharded)
		blk = build(cache, block_id);
	else
	{
		blk = cache.pool->find(cache.pool_key(), block_id);
		if (!blk)
			blk = cache.pool->try_insert_or_get(
				cache.pool_key(), block_id, build(cache, block_id));
	}

	const size_t s = (tl.next++) & (tl.slots - 1);
	tl.epoch[s]    = cache.epoch;
	tl.block_id[s] = block_id;
	tl.bytes[s]    = blk;
	return blk->data();
}
