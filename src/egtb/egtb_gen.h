#pragma once

#include "egtb/egtb_probe.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/piece_group.h"
#include "egtb/king_slice_manager.h"
#include "egtb/pawn_slice_manager.h"
#include "egtb/symmetry.h"

#include "chess/chess.h"
#include "chess/bitboard.h"
#include "chess/position.h"
#include "chess/move.h"
#include "chess/piece_config.h"

#include "util/allocation.h"
#include "util/compress.h"
#include "util/defines.h"
#include "util/division.h"
#include "util/enum.h"
#include "util/fixed_vector.h"
#include "util/intrin.h"
#include "util/math.h"
#include "util/param.h"

#include <filesystem>
#include "util/span.h"
#include "util/thread_pool.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

// Shared SIGINT-driven interrupt flag. Both DTC and DTM gens poll this in their
// per-ply iterate loops and unwind via their own *_Interrupted exception, so a
// single SIGINT handler in main.cpp can serve any mode.
void egtb_request_interrupt() noexcept;
NODISCARD bool egtb_is_interrupt_requested() noexcept;

// Reads and validates a checkpoint. Returns false (leaving *out untouched) when
// the file is missing, short, or fails the magic/version guard.
template <class T>
bool read_checkpoint(const std::filesystem::path& p, T* out)
{
	std::error_code ec;
	if (!std::filesystem::exists(p, ec)) return false;
	std::ifstream fp(p, std::ios::binary);
	if (!fp) return false;
	T c{};
	fp.read(reinterpret_cast<char*>(&c), sizeof(c));
	if (!fp || fp.gcount() != sizeof(c)) return false;
	if (c.magic != T::MAGIC) return false;
	if (c.version != T::VERSION) return false;
	*out = c;
	return true;
}

// Atomically replaces the checkpoint via write-tmp + rename.
template <class T>
void write_checkpoint(const std::filesystem::path& p, const T& c)
{
	std::filesystem::create_directories(p.parent_path());
	const auto tmp = p.string() + ".tmp";
	{
		std::ofstream fp(tmp, std::ios::binary | std::ios::trunc);
		fp.write(reinterpret_cast<const char*>(&c), sizeof(c));
		fp.flush();
	}
	std::error_code ec;
	std::filesystem::rename(tmp, p, ec);
}

inline void remove_checkpoint(const std::filesystem::path& p)
{
	std::error_code ec;
	std::filesystem::remove(p, ec);
}

// Position_For_Gen: Position + Board_Index, lazily filled from group indices.
// Always operates in the canonical frame.

struct Position_For_Gen
{
	Position_For_Gen(const Piece_Config_For_Gen& epsi, Board_Index pos, Color turn = WHITE) :
		m_epsi(&epsi),
		m_board_index(pos),
		m_turn(turn),
		m_cached_board_index(BOARD_INDEX_NONE),
		m_legal(false)
	{
		m_epsi->decompose_board_index(pos, out_param(m_index));
	}

	INLINE Position_For_Gen& operator++()
	{
		m_board_index = static_cast<Board_Index>(static_cast<size_t>(m_board_index) + 1);
		m_epsi->step_to_next(inout_param(m_index));
		return *this;
	}

	NODISCARD INLINE bool operator<(Board_Index other) const
	{
		return m_board_index < other;
	}

	// Default accessor: runs layout-legality check (overlap + slice-range).
	NODISCARD const Position& board() const
	{
		init_board<false>();
		return m_board;
	}

	NODISCARD Position& board()
	{
		init_board<false>();
		return m_board;
	}

	// Fast accessor: caller vouches index is layout-legal; skips checks.
	NODISCARD const Position& board_unchecked() const
	{
		init_board<true>();
		return m_board;
	}

	NODISCARD Position& board_unchecked()
	{
		init_board<true>();
		return m_board;
	}

	NODISCARD const Decomposed_Board_Index& index() const { return m_index; }

	// Canonical-frame placements, lazily filled alongside m_board.
	NODISCARD const std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements() const
	{
		init_board<false>();
		return m_placements;
	}
	NODISCARD const std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements_unchecked() const
	{
		init_board<true>();
		return m_placements;
	}

	void get_fen(Span<char> out) const
	{
		init_board<false>();
		m_board.to_fen(out);
	}

	void set_turn(Color c)
	{
		m_turn = c;
		if (m_cached_board_index == m_board_index)
			m_board.set_turn(c);
	}

	enum struct Legality_Lower_Bound { LAYOUT, CHESS_LEGAL };

	NODISCARD bool is_legal(Legality_Lower_Bound bound = Legality_Lower_Bound::LAYOUT) const
	{
		init_board<false>();
		if (!m_legal) return false;
		if (bound == Legality_Lower_Bound::LAYOUT) return true;
		return m_board.is_legal();
	}

	NODISCARD Board_Index board_index() const { return m_board_index; }

	void set_board_index(Board_Index pos)
	{
		m_epsi->decompose_board_index(pos, out_param(m_index));
		m_board_index = pos;
		m_cached_board_index = BOARD_INDEX_NONE;
	}

	NODISCARD const Piece_Config_For_Gen& epsi() const { return *m_epsi; }

private:
	const Piece_Config_For_Gen* m_epsi;
	Board_Index m_board_index;
	Color m_turn;
	Decomposed_Board_Index m_index;

	mutable Board_Index m_cached_board_index;
	mutable Position m_board;
	mutable std::array<Piece_Group::Placement, PIECE_CLASS_NB> m_placements{};
	mutable bool m_legal;

	template <bool ASSUME_LEGAL>
	void init_board() const
	{
		if (m_cached_board_index == m_board_index) return;
		m_legal = m_epsi->template fill_board<ASSUME_LEGAL>(
			m_index, out_param(m_board), out_param(m_placements));
		if (m_legal) m_board.set_turn(m_turn);
		m_cached_board_index = m_board_index;
	}
};

// Shared_Board_Index_Iterator: atomic work-stealing range allocator over
// [start_idx, end_idx). Threads call next_range() for disjoint chunks.

struct Shared_Board_Index_Iterator
{
private:
	template <typename IterT>
	struct Sentineled_Self_Iterator
	{
		struct iterator_sentinel {};
		NODISCARD friend bool operator==(const IterT& lhs, const iterator_sentinel&) { return lhs.is_end(); }
		NODISCARD friend bool operator!=(const IterT& lhs, const iterator_sentinel&) { return !lhs.is_end(); }
		NODISCARD IterT& begin() { return static_cast<IterT&>(*this); }
		NODISCARD iterator_sentinel end() { return {}; }
	};

public:

	struct Chunk_Iterator : Sentineled_Self_Iterator<Chunk_Iterator>
	{
		explicit Chunk_Iterator(Shared_Board_Index_Iterator& provider) : m_provider(&provider) { ++*this; }
		NODISCARD bool is_end() const { return m_chunk_start == m_chunk_end; }
		Chunk_Iterator& operator++()
		{
			auto [s, e] = m_provider->next_range();
			m_chunk_start = s; m_chunk_end = e;
			return *this;
		}
		NODISCARD std::pair<Board_Index, Board_Index> operator*() const { return { m_chunk_start, m_chunk_end }; }
	private:
		Shared_Board_Index_Iterator* m_provider;
		Board_Index m_chunk_start, m_chunk_end;
	};

	struct Index_Iterator : Sentineled_Self_Iterator<Index_Iterator>
	{
		explicit Index_Iterator(Shared_Board_Index_Iterator& provider) : m_provider(&provider)
		{
			auto [s, e] = m_provider->next_range();
			m_chunk_curr = s; m_chunk_end = e;
		}
		NODISCARD bool is_end() const { return m_chunk_curr == m_chunk_end; }
		Index_Iterator& operator++()
		{
			m_chunk_curr = static_cast<Board_Index>(static_cast<size_t>(m_chunk_curr) + 1);
			if (m_chunk_curr == m_chunk_end)
			{
				auto [s, e] = m_provider->next_range();
				m_chunk_curr = s; m_chunk_end = e;
			}
			return *this;
		}
		NODISCARD Board_Index operator*() const { return m_chunk_curr; }
	private:
		Shared_Board_Index_Iterator* m_provider;
		Board_Index m_chunk_curr, m_chunk_end;
	};

	struct Board_Iterator : Sentineled_Self_Iterator<Board_Iterator>
	{
		Board_Iterator(Shared_Board_Index_Iterator& provider, const Piece_Config_For_Gen& epsi, Color turn = WHITE) :
			m_provider(&provider),
			m_chunk(m_provider->next_range()),
			m_pos_gen(epsi, m_chunk.first, turn)
		{}
		NODISCARD bool is_end() const { return m_chunk.first == m_chunk.second; }
		Board_Iterator& operator++()
		{
			m_chunk.first = static_cast<Board_Index>(static_cast<size_t>(m_chunk.first) + 1);
			if (m_chunk.first == m_chunk.second)
			{
				m_chunk = m_provider->next_range();
				if (!is_end()) m_pos_gen.set_board_index(m_chunk.first);
			}
			else ++m_pos_gen;
			return *this;
		}
		NODISCARD const Position_For_Gen& operator*() const { return m_pos_gen; }
		NODISCARD       Position_For_Gen& operator*()       { return m_pos_gen; }
	private:
		Shared_Board_Index_Iterator* m_provider;
		std::pair<Board_Index, Board_Index> m_chunk;
		Position_For_Gen m_pos_gen;
	};

	Shared_Board_Index_Iterator(Board_Index start_idx, Board_Index end_idx, size_t chunk_size) :
		m_start_idx(start_idx),
		m_end_idx(end_idx),
		m_chunk_size(chunk_size),
		m_current_chunk_index(0)
	{}

	Shared_Board_Index_Iterator(const Shared_Board_Index_Iterator&) = delete;

	NODISCARD std::pair<Board_Index, Board_Index> next_range()
	{
		const size_t chunk_index = m_current_chunk_index.fetch_add(1);
		const size_t start = static_cast<size_t>(m_start_idx);
		const size_t end   = static_cast<size_t>(m_end_idx);
		// Emit globally m_chunk_size-aligned chunks: an unaligned head if
		// needed, boundary-aligned full chunks, then an unaligned tail if
		// needed. Per-chunk consumers can identify a full bitmap chunk with
		// cs % m_chunk_size == 0 && ce - cs == m_chunk_size.
		const size_t aligned_start = (start + m_chunk_size - 1) / m_chunk_size * m_chunk_size;
		const size_t has_head = (start < aligned_start) ? 1 : 0;
		size_t chunk_start, chunk_end;
		if (chunk_index == 0 && has_head)
		{
			chunk_start = start;
			chunk_end = std::min(aligned_start, end);
		}
		else
		{
			const size_t k = chunk_index - has_head;
			chunk_start = std::min(aligned_start + k * m_chunk_size, end);
			chunk_end = std::min(chunk_start + m_chunk_size, end);
		}
		return { static_cast<Board_Index>(chunk_start), static_cast<Board_Index>(chunk_end) };
	}

	NODISCARD Chunk_Iterator chunks() { return Chunk_Iterator(*this); }
	NODISCARD Index_Iterator indices() { return Index_Iterator(*this); }
	NODISCARD Board_Iterator boards(const Piece_Config_For_Gen& epsi, Color turn = WHITE)
	{
		return Board_Iterator(*this, epsi, turn);
	}

	NODISCARD size_t num_indices() const { return m_end_idx - m_start_idx; }
	NODISCARD Board_Index end_index() const { return m_end_idx; }

private:
	Board_Index m_start_idx, m_end_idx;
	size_t m_chunk_size;
	std::atomic<size_t> m_current_chunk_index;
};

// Sliced_EGTB_File_For_Gen: entry storage partitioned into slice-groups, each
// one Huge_Array of M consecutive logical slices. Always paged; caller-driven
// budget controls eviction.

void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic);
void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic);

// Per-worker LZ4 compress scratch save_group_raw() holds; reserve one per concurrent
// eviction worker on top of resident group bytes.
size_t spill_compress_scratch_bytes(size_t bytes_per_group);

template <typename MainEntryT, typename... OtherEntryTs>
struct Sliced_EGTB_File_For_Gen
{
	static constexpr size_t NUM_ENTRY_VARIANTS = 1 + sizeof...(OtherEntryTs);
	static constexpr size_t ENTRY_SIZE = sizeof(MainEntryT);
	static_assert(((sizeof(OtherEntryTs) == ENTRY_SIZE) && ...));
	static_assert(ENTRY_SIZE == 1 || ENTRY_SIZE == 2 || ENTRY_SIZE == 4 || ENTRY_SIZE == 8);

	using Underlying_Entry_Type = Unsigned_Int_Of_Size<ENTRY_SIZE>;

	Sliced_EGTB_File_For_Gen() = default;
	~Sliced_EGTB_File_For_Gen() { close(); }

	void create(size_t num_slices, size_t within_size,
	            std::filesystem::path disk_prefix,
	            uint64_t magic,
	            const std::string& filename_fmt)
	{
		m_within = within_size;
		m_num_slices = num_slices;
		m_slices_per_group = compute_slices_per_group(within_size);
		if (m_within > 1)
			m_within_div = Divider<uint64_t>(m_within);
		if (m_slices_per_group > 1)
			m_slices_per_group_div = Divider<uint64_t>(m_slices_per_group);
		const size_t ng = (num_slices == 0) ? 0
		                : (num_slices + m_slices_per_group - 1) / m_slices_per_group;
		m_groups.clear();
		m_groups.resize(ng);
		m_dirty.assign(ng, false);
		m_last_used.assign(ng, 0);
		m_disk_prefix = std::move(disk_prefix);
		m_disk_magic = magic;
		m_disk_filename_fmt = filename_fmt;
		m_disk_paths.assign(ng, {});
		for (size_t i = 0; i < ng; ++i)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), m_disk_filename_fmt.c_str(), i);
			m_disk_paths[i] = m_disk_prefix / buf;
		}
	}

	NODISCARD static size_t compute_slices_per_group(size_t within_size)
	{
		constexpr size_t MIN_GROUP_BYTES = 64ull * 1024ull * 1024ull;
		const size_t bytes_per_slice = within_size * sizeof(Underlying_Entry_Type);
		if (bytes_per_slice >= MIN_GROUP_BYTES) return 1;
		if (bytes_per_slice == 0) return 1;
		return (MIN_GROUP_BYTES + bytes_per_slice - 1) / bytes_per_slice;
	}

	NODISCARD const Underlying_Entry_Type* slice_data(size_t s) const
	{
		const size_t g = group_id_of(s);
		const size_t in_g = s - g * m_slices_per_group;
		return m_groups[g].data() + in_g * m_within;
	}

	// Zero-copy typed view over a slice's bytes — caller iterates as T directly.
	template <typename T = MainEntryT>
	NODISCARD const T* slice_view_as(size_t s) const
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...));
		return reinterpret_cast<const T*>(slice_data(s));
	}

	NODISCARD Underlying_Entry_Type* slice_data(size_t s)
	{
		const size_t g = group_id_of(s);
		const size_t in_g = s - g * m_slices_per_group;
		return m_groups[g].data() + in_g * m_within;
	}

	NODISCARD INLINE std::pair<size_t, size_t> split(size_t p) const
	{
		if (m_within > 1)
		{
			const size_t s = p / m_within_div;
			return { s, p - s * m_within };
		}
		return { p, 0 };
	}

	template <size_t N = NUM_ENTRY_VARIANTS>
	NODISCARD std::enable_if_t<N == 1, MainEntryT> read(Board_Index pos) const
	{
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		MainEntryT e;
		const auto [s, off] = split(p);
		std::memcpy(&e, slice_data(s) + off, sizeof(MainEntryT));
		return e;
	}

	template <typename T, size_t N = NUM_ENTRY_VARIANTS>
	NODISCARD std::enable_if_t<N != 1, T> read(Board_Index pos) const
	{
		static_assert(   std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...)
		              || std::is_base_of_v<T, MainEntryT> || (std::is_base_of_v<T, OtherEntryTs> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		T e;
		const auto [s, off] = split(p);
		std::memcpy(&e, slice_data(s) + off, sizeof(T));
		return e;
	}

	// Zero-copy typed view at a Board_Index. Caller must keep the slice resident.
	template <typename T = MainEntryT>
	NODISCARD const T& view_at(Board_Index pos) const
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [s, off] = split(p);
		return *reinterpret_cast<const T*>(slice_data(s) + off);
	}

	template <typename T>
	void write(const T& tt, Board_Index pos)
	{
		static_assert(std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [s, off] = split(p);
		std::memcpy(slice_data(s) + off, &tt, sizeof(T));
		mark_dirty(s);
	}

	template <typename T>
	void lock_add_flags(Board_Index pos, T flags)
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(MainEntryT::template is_allowed_flag_type<T> || (OtherEntryTs::template is_allowed_flag_type<T> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [s, off] = split(p);
		atomic_fetch_or(slice_data(s) + off, flags);
		mark_dirty(s);
	}

	template <typename T>
	void add_flags(Board_Index pos, T flags)
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(MainEntryT::template is_allowed_flag_type<T> || (OtherEntryTs::template is_allowed_flag_type<T> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [s, off] = split(p);
		slice_data(s)[off] |= static_cast<Underlying_Entry_Type>(flags);
		mark_dirty(s);
	}

	void close()
	{
		m_groups.clear();
		m_num_slices = 0;
		m_dirty.clear();
		m_last_used.clear();
		m_disk_paths.clear();
		m_within = 0;
		m_slices_per_group = 1;
		m_within_div = Divider<uint64_t>{};
		m_slices_per_group_div = Divider<uint64_t>{};
	}

	void remove_disk_files()
	{
		std::error_code ec;
		for (const auto& p : m_disk_paths)
			std::filesystem::remove(p, ec);
	}

	NODISCARD size_t slices_per_group() const { return m_slices_per_group; }
	NODISCARD size_t num_groups() const { return m_groups.size(); }
	NODISCARD size_t group_id_of(size_t slice_id) const
	{
		return m_slices_per_group > 1 ? slice_id / m_slices_per_group_div : slice_id;
	}
	NODISCARD size_t slice_id_of(size_t p) const
	{
		return m_within > 1 ? p / m_within_div : p;
	}

	NODISCARD size_t num_slices() const { return m_num_slices; }
	NODISCARD size_t within_slice_size() const { return m_within; }
	NODISCARD size_t num_entries() const { return m_num_slices * m_within; }

	void mark_dirty(size_t slice_id) { m_dirty[group_id_of(slice_id)] = true; }

	NODISCARD bool is_resident(size_t slice_id) const { return is_group_resident(group_id_of(slice_id)); }
	NODISCARD bool is_group_resident(size_t g) const { return m_groups[g].size() != 0; }
	NODISCARD bool is_group_dirty(size_t g) const { return m_dirty[g] != 0; }
	NODISCARD uint64_t last_used(size_t g) const { return m_last_used[g]; }
	void set_last_used(size_t g, uint64_t tick) { m_last_used[g] = tick; }

	void load_group(size_t g)
	{
		if (is_group_resident(g)) return;
		const bool on_disk = std::filesystem::exists(m_disk_paths[g]);
		alloc_group(g, !on_disk);
		if (on_disk)
		{
			load_group_raw(
				reinterpret_cast<uint8_t*>(m_groups[g].data()),
				group_bytes(g),
				m_disk_paths[g], m_disk_magic);
		}
		m_dirty[g] = false;
	}

	void evict_group(size_t g)
	{
		if (!is_group_resident(g)) return;
		if (m_dirty[g])
		{
			save_group_raw(
				reinterpret_cast<const uint8_t*>(m_groups[g].data()),
				group_bytes(g),
				m_disk_paths[g], m_disk_magic);
			m_dirty[g] = false;
		}
		m_groups[g] = Huge_Array<Underlying_Entry_Type>{};  // free
	}

	void evict_all()
	{
		// Evict serially: a threaded compress runs one save_group_raw per thread
		// concurrently, and the compressor's transient buffers blow the memory budget.
		for (size_t g = 0; g < m_groups.size(); ++g)
			evict_group(g);
	}

private:
	// Last group may hold fewer than m_slices_per_group slices.
	void alloc_group(size_t g, bool zero_init)
	{
		const size_t base = g * m_slices_per_group;
		ASSERT(base < m_num_slices);
		const size_t slices_in_g = std::min(m_slices_per_group, m_num_slices - base);
		const size_t group_entries = m_within * slices_in_g;
		if (zero_init && (MainEntryT::wants_zero_init_storage || (OtherEntryTs::wants_zero_init_storage || ...)))
			m_groups[g] = Huge_Array<Underlying_Entry_Type>(group_entries);
		else
			m_groups[g] = Huge_Array<Underlying_Entry_Type>(For_Overwrite_Tag{}, group_entries);
	}

	NODISCARD size_t group_bytes(size_t g) const
	{
		const size_t base = g * m_slices_per_group;
		const size_t slices_in_g = std::min(m_slices_per_group, m_num_slices - base);
		return slices_in_g * m_within * sizeof(Underlying_Entry_Type);
	}

	std::vector<Huge_Array<Underlying_Entry_Type>> m_groups;
	size_t m_num_slices = 0;
	size_t m_slices_per_group = 1;
	std::vector<uint8_t> m_dirty;          // bool-as-uint8 to avoid vector<bool>
	std::vector<uint64_t> m_last_used;     // per-group LRU tick (see last_used())
	std::vector<std::filesystem::path> m_disk_paths;
	std::filesystem::path m_disk_prefix;
	std::string m_disk_filename_fmt;
	uint64_t m_disk_magic = 0;
	size_t m_within = 0;
	// Only initialized when stride > 1 (Divider rejects divisor <= 1).
	Divider<uint64_t> m_within_div{};
	Divider<uint64_t> m_slices_per_group_div{};
};

struct EGTB_Info
{
	EGTB_Info() { clear(); }
	void clear() { std::memset(this, 0, sizeof(EGTB_Info)); }

	void maybe_update_longest_win(Color color, size_t idx, size_t value)
	{
		if (value > longest_win[color]
		    || (value == longest_win[color] && idx < longest_idx[color]))
		{
			longest_win[color] = static_cast<uint16_t>(value);
			longest_idx[color] = idx;
		}
	}

	void add_result(Color color, WDL_Entry value, uint64_t weight = 1)
	{
		// CURSED_WIN/BLESSED_LOSS collapse into win/lose counts so the singular
		// fast-path in egtb_compress (win+draw==0 etc.) keeps working.
		switch (value)
		{
			case WDL_Entry::DRAW:         draw_cnt[color]    += weight; break;
			case WDL_Entry::LOSE:         lose_cnt[color]    += weight; break;
			case WDL_Entry::BLESSED_LOSS: lose_cnt[color]    += weight; break;
			case WDL_Entry::WIN:          win_cnt[color]     += weight; break;
			case WDL_Entry::CURSED_WIN:   win_cnt[color]     += weight; break;
			case WDL_Entry::ILLEGAL:      illegal_cnt[color] += weight; break;
		}
	}

	template <typename IterT>
	void consolidate_from(IterT begin, IterT end, Color color)
	{
		while (begin != end)
		{
			const auto& info = *begin;
			win_cnt[color]     += info.win_cnt[color];
			draw_cnt[color]    += info.draw_cnt[color];
			lose_cnt[color]    += info.lose_cnt[color];
			illegal_cnt[color] += info.illegal_cnt[color];
			if (longest_win[color] < info.longest_win[color]
			    || (longest_win[color] == info.longest_win[color]
			        && longest_idx[color] > info.longest_idx[color]))
			{
				longest_win[color] = info.longest_win[color];
				longest_idx[color] = info.longest_idx[color];
			}
			++begin;
		}
	}

	uint64_t win_cnt[COLOR_NB];
	uint64_t lose_cnt[COLOR_NB];
	uint64_t draw_cnt[COLOR_NB];
	uint64_t illegal_cnt[COLOR_NB];
	uint16_t longest_win[COLOR_NB];

	char longest_fen[COLOR_NB][MAX_FEN_LENGTH];
	uint64_t longest_idx[COLOR_NB];
};

// On-demand group cache for the save_to_disk readers: acquire+release pins
// groups, loading on miss and evicting oldest unpinned past cap. I/O runs
// outside the lock (per-entry state + cv) so workers don't serialize.
template <typename EntryT, typename... OtherEntryTs>
struct Save_Group_Cache
{
	// (table_idx, group_id). DTC/DTM key on Color; DTM50 shares one budget
	// across all HMC_COUNT × 2 layer tables.
	using Key = std::pair<size_t, size_t>;

	enum struct State : uint8_t { LOADING, RESIDENT, EVICTING };
	struct Entry { int pins; State state; };

	std::vector<Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>*> tables;
	size_t cap;
	std::mutex mu;
	std::condition_variable cv;    // an entry left LOADING/EVICTING
	std::list<Key> fifo;           // load order, front = oldest; LOADING + RESIDENT
	std::map<Key, Entry> entries;

	Save_Group_Cache(Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
	                 Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
	                 size_t c)
		: tables{ w_tbl, b_tbl }, cap(c) { init_resident(); }

	Save_Group_Cache(std::vector<Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>*> tbls,
	                 size_t c)
		: tables(std::move(tbls)), cap(c) { init_resident(); }

	void acquire(size_t table_idx, size_t g)
	{
		std::unique_lock<std::mutex> lk(mu);
		const Key k{ table_idx, g };
		for (;;)
		{
			auto it = entries.find(k);
			if (it == entries.end())
			{
				// Reserve the slot (counted, unevictable), sweep to make room,
				// then load outside the lock.
				entries.emplace(k, Entry{ 1, State::LOADING });
				fifo.push_back(k);
				sweep(lk);
				auto& tbl = *tables[table_idx];
				if (!tbl.is_group_resident(g))
				{
					lk.unlock();
					tbl.load_group(g);
					lk.lock();
				}
				entries.find(k)->second.state = State::RESIDENT;
				cv.notify_all();
				return;
			}
			if (it->second.state == State::RESIDENT)
			{
				it->second.pins++;
				return;
			}
			// Busy (LOADING/EVICTING): wait, then re-evaluate.
			cv.wait(lk);
		}
	}

	void release(size_t table_idx, size_t g)
	{
		std::lock_guard<std::mutex> lk(mu);
		auto it = entries.find({ table_idx, g });
		if (it != entries.end()) it->second.pins--;
	}

	void purge(size_t table_idx)
	{
		std::lock_guard<std::mutex> lk(mu);
		for (auto it = fifo.begin(); it != fifo.end(); )
			it = (it->first == table_idx) ? fifo.erase(it) : std::next(it);
		for (auto it = entries.begin(); it != entries.end(); )
			it = (it->first.first == table_idx) ? entries.erase(it) : std::next(it);
		tables[table_idx] = nullptr;
	}

private:
	// Pull oldest unpinned RESIDENT victims from fifo under the lock, then free
	// them in one batch with the lock dropped.
	void sweep(std::unique_lock<std::mutex>& lk)
	{
		std::vector<Key> victims;
		for (auto it = fifo.begin();
		     it != fifo.end() && fifo.size() - victims.size() > cap; )
		{
			auto p = entries.find(*it);
			if (p != entries.end() && p->second.state == State::RESIDENT && p->second.pins == 0)
			{
				p->second.state = State::EVICTING;
				victims.push_back(*it);
				it = fifo.erase(it);
			}
			else
			{
				++it;
			}
		}
		if (victims.empty()) return;
		lk.unlock();
		for (const Key& v : victims) tables[v.first]->evict_group(v.second);
		lk.lock();
		for (const Key& v : victims) entries.erase(v);
		cv.notify_all();
	}

	void init_resident()
	{
		for (size_t ti = 0; ti < tables.size(); ++ti)
		{
			if (!tables[ti]) continue;
			auto& tbl = *tables[ti];
			for (size_t g = 0; g < tbl.num_groups(); ++g)
			{
				if (tbl.is_group_resident(g))
				{
					fifo.push_back({ ti, g });
					entries[{ ti, g }] = Entry{ 0, State::RESIDENT };
				}
			}
		}
	}
};

template <typename EntryT, typename... OtherEntryTs>
struct Pinned_Group_Range
{
	Save_Group_Cache<EntryT, OtherEntryTs...>* cache;
	size_t table_idx;
	std::vector<size_t> held;

	Pinned_Group_Range(Save_Group_Cache<EntryT, OtherEntryTs...>& c, size_t ti, size_t first_g, size_t last_g)
		: cache(&c), table_idx(ti)
	{
		held.reserve(last_g - first_g + 1);
		for (size_t g = first_g; g <= last_g; ++g)
		{
			cache->acquire(table_idx, g);
			held.push_back(g);
		}
	}
	~Pinned_Group_Range()
	{
		for (size_t g : held) cache->release(table_idx, g);
	}
	Pinned_Group_Range(const Pinned_Group_Range&) = delete;
	Pinned_Group_Range& operator=(const Pinned_Group_Range&) = delete;
};

template <typename EntryT, typename... OtherEntryTs>
NODISCARD Block_Source make_entry_block_source(
	Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& src,
	Save_Group_Cache<EntryT, OtherEntryTs...>& cache,
	Color color,
	Index_Permutation_Plan perm_plan,
	size_t block_size,
	size_t entry_bytes);

template <typename Local, typename EntryT, typename... OtherEntryTs, typename PerEntryFn>
NODISCARD std::vector<Local> gather_egtb_info_parallel(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	Save_Group_Cache<EntryT, OtherEntryTs...>& cache,
	size_t table_idx,
	Color color,
	size_t num_positions,
	size_t max_workers,
	EGTB_Info& info,
	PerEntryFn&& per_entry)
{
	struct Shard { EGTB_Info info; Local local; };

	auto& src = *cache.tables[table_idx];
	const size_t within = src.within_slice_size();
	const size_t spg = src.slices_per_group();
	const size_t ns  = src.num_slices();
	const size_t ng  = src.num_groups();
	if (ng == 0 || num_positions == 0) return {};

	const size_t capped = (max_workers == 0)
		? thread_pool->num_workers()
		: std::min(thread_pool->num_workers(), max_workers);
	const size_t workers = std::max<size_t>(1, std::min(capped, ng));

	std::atomic<size_t> next_group(0);
	auto shards = thread_pool->run_sync_task_on_multiple_threads(
		workers, [&](size_t) -> Shard {
			Shard shard{};
			Decomposed_Board_Index didx{};
			for (;;)
			{
				const size_t g = next_group.fetch_add(1, std::memory_order_relaxed);
				if (g >= ng) break;
				Pinned_Group_Range<EntryT, OtherEntryTs...> pin(cache, table_idx, g, g);
				const size_t s_begin = g * spg;
				const size_t s_end   = std::min(s_begin + spg, ns);
				for (size_t s = s_begin; s < s_end; ++s)
				{
					const size_t base = s * within;
					if (base >= num_positions) break;
					const size_t end = std::min(base + within, num_positions);
					const EntryT* row = src.template slice_view_as<EntryT>(s);
					epsi.decompose_board_index(static_cast<Board_Index>(base), out_param(didx));
					for (size_t idx = base; idx < end; ++idx)
					{
						const EntryT& e = row[idx - base];
						const uint64_t w = epsi.orbit_weight(didx);
						shard.info.add_result(color, e.wdl(), w);
						if (e.is_win())
							shard.info.maybe_update_longest_win(color, idx, e.value());
						per_entry(shard.local, idx, e);
						epsi.step_to_next(inout_param(didx));
					}
				}
			}
			return shard;
		});

	std::vector<Local> locals;
	locals.reserve(shards.size());
	for (auto& s : shards)
	{
		info.consolidate_from(&s.info, &s.info + 1, color);
		locals.push_back(std::move(s.local));
	}
	return locals;
}

// Position to canonical Board_Index for a given epsi.
NODISCARD std::array<Piece_Group::Placement, PIECE_CLASS_NB>
	placements_from_position(const Piece_Config_For_Gen& epsi, const Position& pos);

NODISCARD Board_Index canonical_board_index(
	const Piece_Config_For_Gen& epsi,
	std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements);

NODISCARD Board_Index board_index_of_position(const Piece_Config_For_Gen& epsi, const Position& pos);

// Shared generator scaffolding.
struct EGTB_Generator
{
	explicit EGTB_Generator(const Piece_Config& ps);

	NODISCARD const Piece_Config_For_Gen& epsi() const { return m_epsi; }

	NODISCARD INLINE Fixed_Vector<Color, 2> table_colors() const
	{
		Fixed_Vector<Color, 2> r;
		r.emplace_back(WHITE);
		if (!m_is_symmetric)
			r.emplace_back(BLACK);
		return r;
	}

	// Sub-material closure for `ps`: every Piece_Config reachable in one move
	// (capture, promotion, or capture-with-promote). DTC/WDL and DTM share the
	// dependency shape and differ only in the probe-file type they open.
	NODISCARD static std::map<Material_Key, Piece_Config>
	enumerate_sub_materials(const Piece_Config& ps);

	template <typename ProbeFileT>
	NODISCARD static auto
	open_sub_probes(const Piece_Config& ps, const EGTB_Paths& paths,
		In_Out_Param<Thread_Pool> thread_pool)
	{
		using EntryT = typename ProbeFileT::Entry_Type;
		std::map<Material_Key, std::unique_ptr<EGTB_Sub_Reader<EntryT>>> out;
		for (const auto& [key, sub] : enumerate_sub_materials(ps))
		{
			if (sub.num_pieces() <= 2 && !sub.has_frozen_pair()) continue;
			out[key] = std::make_unique<ProbeFileT>(paths, sub, thread_pool);
		}
		return out;
	}

protected:
	Piece_Config_For_Gen m_epsi;

	// One Piece_Config_For_Gen per reachable sub-material (capture/promo/both).
	std::map<Material_Key, Piece_Config_For_Gen> m_sub_epsi_by_material;

	// O(1) sub-TB lookup: [mover_color][captured | PIECE_NONE][promo | PIECE_TYPE_NONE].
	// nullptr => combination unreachable; mirror/read_color valid only where epsi != null.
	const Piece_Config_For_Gen* m_sub_epsi_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	bool                        m_sub_mirror_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	Color                       m_sub_read_color_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};

	// Frozen-pair child routing (only populated when m_epsi has a pair). On the
	// board a pair pawn is indistinguishable from a free pawn, so a capture that
	// involves the pair cannot be keyed by (mover, captured, promo) alone --
	// next_sub_index disambiguates by square and consults these.
	struct Sub_Entry
	{
		const Piece_Config_For_Gen* epsi = nullptr;
		bool  mirror = false;
		Color read_color = WHITE;
	};
	// p->P: the pair member of this color is captured; the other member is freed.
	// (Capturing a free piece -- p->p / p->PP -- routes to the full free-pawn
	// material through the ordinary (mover, captured, promo) arrays.)
	Sub_Entry m_pair_broken_survivor[COLOR_NB]{};

	bool m_is_symmetric;

	// Statistics written during compress; consumed by save_to_disk.
	EGTB_Info m_info;

	// Fast child Board_Index after a quiet (non-cap, non-promo, non-EP) move;
	// also the predecessor index for a pre-quiet retro move. Falls back to full
	// re-canonicalization for king moves, pawn moves, and self-stabilizer slices
	// (all can shift canonical orientation).
	NODISCARD Board_Index next_quiet_index(const Position_For_Gen& pos_gen, Move move) const;

	// Forward sub-move: apply `move`, look up sub-material, mirror if needed,
	// return Board_Index in the sub-epsi. Handles cap/promo/cap-promo uniformly.
	// Mutates pos_for_gen's board across the call and restores via undo_move.
	NODISCARD Board_Index next_sub_index(Position_For_Gen& pos_for_gen, Move move,
	                                     Out_Param<Color> sub_color,
	                                     Out_Param<const Piece_Config_For_Gen*> sub_epsi) const;

	// Sized so per-worker windows keep adjacent Board_Index ranges local.
	static constexpr size_t CHUNK_SIZE = CACHE_LINE_SIZE * CHAR_BIT * 64;

	NODISCARD Shared_Board_Index_Iterator make_slice_group_iterator(
		size_t group_id, size_t slices_per_group) const;

	// Active-pair-pid state. gen() loops set m_active_pawn_slices to the union
	// of pair_members for the fusion currently being built, then calls
	// refresh_active_metadata to derive the bitmap + group-id list.
	std::vector<int32_t> m_active_pawn_slices = { 0 };
	std::vector<uint8_t> m_pid_in_pair;        // [num_pawn_slices]
	std::vector<size_t>  m_pair_group_ids;     // groups touched by m_active_pawn_slices
	std::vector<std::vector<int32_t>> m_targets_by_pid;  // push targets per active pid

	// Reused by King_Slice_Manager::neighbors out-param.
	King_Slice_Manager::Neighbor_List m_scratch_nbrs;

	// Paging state shared across DTC and DTM gens. Sized/seeded by each
	// derived ctor once the table dimensions are known.
	size_t m_paging_budget_bytes = 0;
	uint64_t m_paging_tick = 0;

	// Per-color scan bitmap, indexed by group_id. Conservative tracking shared
	// by DTC and DTM gens: any write or CHANGE-flag flip sets the target
	// group's bit via mark_iter (over-marking only costs an extra scan; under-
	// marking would silently drop work). run_iter walks only marked groups and
	// clears the bit after scanning a group whose cells are all FINAL/ILLEGAL
	// with values far enough behind the current ply that no future action can
	// fire. A subsequent write reinstates the bit.
	std::vector<uint8_t> m_iter_groups[COLOR_NB];

	// Finer-grained companion to m_iter_groups, indexed by pos / CHUNK_SIZE.
	// Same set-on-write / clear-on-clean-scan rules, applied at chunk granularity
	// so run_iter can skip cold chunks within a still-warm group.
	std::vector<uint8_t> m_iter_chunks[COLOR_NB];

	// Per-color group bitmap reused by apply_working_set callers; rebuilt per
	// fusion phase.
	std::vector<uint8_t> m_scratch_need[COLOR_NB];

	// Size the retro-iteration bitmaps.
	void init_iter_state(size_t num_groups, size_t total_index_space)
	{
		const size_t num_chunks = (total_index_space + CHUNK_SIZE - 1) / CHUNK_SIZE;
		for (Color c : { WHITE, BLACK })
		{
			m_iter_groups[c].assign(num_groups, 0);
			m_iter_chunks[c].assign(num_chunks, 0);
		}
	}

	// Per-fusion seed: first sweep is full-scan, eviction narrows from there.
	// Non-pair groups/chunks are never visited (run_iter only walks
	// m_pair_group_ids), so over-marking them is harmless.
	void seed_iter_groups()
	{
		for (Color c : { WHITE, BLACK })
		{
			std::fill(m_iter_groups[c].begin(), m_iter_groups[c].end(), 1);
			std::fill(m_iter_chunks[c].begin(), m_iter_chunks[c].end(), 1);
		}
	}

	template <typename EntryT, typename... OtherEntryTs>
	INLINE void mark_iter(Color c, Board_Index pos,
	                      const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& f)
	{
		m_iter_groups[c][f.group_id_of(f.slice_id_of(static_cast<size_t>(pos)))] = 1;
		m_iter_chunks[c][static_cast<size_t>(pos) / CHUNK_SIZE] = 1;
	}

	template <typename EntryT, typename... OtherEntryTs>
	void refresh_active_metadata(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl);

	// Set m_active_pawn_slices to the deduped union of pair_members over `fusion`,
	// then rebuild m_targets_by_pid with each active pid's push targets. Shared by
	// the DTC / DTM / DTM50 gen() fusion loops. Caller still runs
	// refresh_active_metadata against its own table afterward.
	void set_active_fusion(const Pawn_Slice_Manager& psm,
	                       const std::vector<int32_t>& fusion);

	// Pack a topo-batch of pair_sids into fusion groups whose unioned group-set
	// fits m_paging_budget_bytes. Returns `{batch}` when budget is unbounded.
	// resident_layers: concurrent layer-copies per group (2 for DTM50, else 1).
	// include_push: count push-target pid ranges (mirrors include_push_in_iter).
	// num_workers: eviction concurrency, for the compress-scratch reserve.
	template <typename EntryT, typename... OtherEntryTs>
	NODISCARD std::vector<std::vector<int32_t>>
	compute_fusion_groups(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl,
	                      const std::vector<int32_t>& batch,
	                      size_t resident_layers, bool include_push,
	                      size_t num_workers) const;

	// Drive residency across one or more contiguous layers (starting at w_tbl/
	// b_tbl) toward needed_w/needed_b, indexed [layer * num_groups + group], then
	// evict globally-LRU non-needed groups until residency fits the budget.
	template <typename EntryT, typename... OtherEntryTs>
	void apply_working_set(
		In_Out_Param<Thread_Pool> thread_pool,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
		const std::vector<uint8_t>& needed_w,
		const std::vector<uint8_t>& needed_b);
};

// Working-set sizing for the gen pipeline. Used by --estimate and `--mem`
// callers to predict resident memory before launching gen. Shared between
// DTC and DTM since both store 2-byte entries (DTC_/DTM_Final_Entry) at
// the same slice/group layout.
struct Working_Set_Estimate
{
	size_t num_positions = 0;
	size_t total_table_bytes = 0;   // both colors, sizeof(*_Final_Entry) per cell
	size_t bytes_per_slice = 0;
	size_t slices_per_group = 0;
	size_t bytes_per_group = 0;
	size_t num_slices = 0;
	size_t num_groups = 0;

	size_t peak_per_group_iter_groups = 0;
	size_t peak_per_group_init_groups = 0;

	size_t peak_pair_iter_groups = 0;
	size_t peak_pair_init_groups = 0;

	size_t peak_batch_iter_groups = 0;
	size_t peak_batch_init_groups = 0;
};

// `include_push_in_iter`: DTM's iter working set unions push-target groups into
// opp's set (PAWN_EVAL inside run_iter reads opp's push-target pids). DTC keeps
// this false since pawn pushes there are zeroing and never reverse-marked.
NODISCARD Working_Set_Estimate compute_working_set(const Piece_Config& ps,
                                                   bool include_push_in_iter = false);
