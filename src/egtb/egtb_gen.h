#pragma once

#include "egtb/egtb_probe.h"
#include "chess/castling_group.h"
#include "egtb/piece_config_for_gen.h"
#include "egtb/piece_group.h"
#include "egtb/king_slice_manager.h"
#include "egtb/pawn_slice_manager.h"

#include "chess/chess.h"
#include "chess/position.h"
#include "chess/move.h"
#include "chess/piece_config.h"

#include "util/allocation.h"
#include "util/compress.h"
#include "util/defines.h"
#include "util/division.h"
#include "util/enum.h"
#include "util/fixed_vector.h"
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
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

enum class Interrupt_Request : uint8_t
{
	NONE = 0,
	SOFT,
	HARD,
};

void egtb_request_interrupt(Interrupt_Request req) noexcept;
NODISCARD bool egtb_is_interrupt_requested(bool hard) noexcept;

// Invalid or missing checkpoints leave `out` untouched.
template <class T>
NODISCARD bool read_checkpoint(const std::filesystem::path& p, T* out)
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
	if (ec)
		print_and_abort("rename failed: %s\n", ec.message().c_str());

	std::printf("\n  checkpoint written: rerun the same command to resume, or delete %s to start over (required if --mem changed)\n",
		p.string().c_str());
	std::fflush(stdout);
}

INLINE void remove_checkpoint(const std::filesystem::path& p)
{
	std::error_code ec;
	std::filesystem::remove(p, ec);
}

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

	void seek(Board_Index pos)
	{
		if (pos == m_board_index + 1)
		{
			m_epsi->step_to_next(inout_param(m_index));
			m_board_index = pos;
		}
		else if (pos != m_board_index)
		{
			m_epsi->decompose_board_index(pos, out_param(m_index));
			m_board_index = pos;
		}
	}

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
		m_legal = (m_legal && m_epsi->same_slice(m_cached_board_index, m_board_index))
			? m_epsi->template patch_board<ASSUME_LEGAL>(
				m_index, inout_param(m_board), inout_param(m_placements))
			: m_epsi->template fill_board<ASSUME_LEGAL>(
				m_index, out_param(m_board), out_param(m_placements));
		if (m_legal) m_board.set_turn(m_turn);
		m_cached_board_index = m_board_index;
	}
};

struct Iterator_Sentinel {};

template <typename IterT>
struct Sentineled_Self_Iterator
{
	NODISCARD friend bool operator!=(const IterT& lhs, Iterator_Sentinel) { return !lhs.is_end(); }
	NODISCARD IterT& begin() { return static_cast<IterT&>(*this); }
	NODISCARD Iterator_Sentinel end() { return {}; }
};

struct Shared_Board_Index_Iterator
{
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
		explicit Index_Iterator(Shared_Board_Index_Iterator& provider) : m_provider(&provider) { next_chunk(); }
		NODISCARD bool is_end() const { return m_chunk_curr == m_chunk_end; }
		Index_Iterator& operator++()
		{
			if (++m_chunk_curr == m_chunk_end) next_chunk();
			return *this;
		}
		NODISCARD Board_Index operator*() const { return m_chunk_curr; }
	private:
		void next_chunk()
		{
			auto [s, e] = m_provider->next_range();
			m_chunk_curr = s; m_chunk_end = e;
		}
		Shared_Board_Index_Iterator* m_provider;
		Board_Index m_chunk_curr, m_chunk_end;
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

private:
	Board_Index m_start_idx, m_end_idx;
	size_t m_chunk_size;
	std::atomic<size_t> m_current_chunk_index;
};

template <typename EntryT>
struct In_Pair_Cell
{
	Board_Index idx;
	EntryT e;
};

template <typename EntryT, typename TableT>
struct In_Pair_Cell_Iterator : Sentineled_Self_Iterator<In_Pair_Cell_Iterator<EntryT, TableT>>
{
	In_Pair_Cell_Iterator(const Piece_Config_For_Gen& epsi,
	                      const std::vector<uint8_t>& pid_in_pair,
	                      const TableT& tbl,
	                      Board_Index chunk_start, Board_Index chunk_end)
		: m_epsi(&epsi)
		, m_pid_in_pair(pid_in_pair.data())
		, m_tbl(&tbl)
		, m_within(epsi.within_slice_size())
		, m_row_end(static_cast<size_t>(chunk_start))
		, m_chunk_end(static_cast<size_t>(chunk_end))
	{
		next_row();
	}

	NODISCARD bool is_end() const { return m_data == nullptr; }

	In_Pair_Cell_Iterator& operator++()
	{
		++m_data;
		if (static_cast<size_t>(++m_idx) == m_row_end) next_row();
		return *this;
	}

	NODISCARD In_Pair_Cell<EntryT> operator*() const { return { m_idx, *m_data }; }

private:
	void next_row()
	{
		while (m_row_end < m_chunk_end)
		{
			const size_t lo = m_row_end;
			const size_t s = m_tbl->slice_id_of(lo);
			const size_t row_base = s * m_within;
			m_row_end = std::min(row_base + m_within, m_chunk_end);
			ASSERT(lo < m_row_end);
			if (!m_pid_in_pair[m_epsi->pawn_slice_of(static_cast<Board_Index>(lo))])
				continue;
			m_data = m_tbl->template slice_view_as<EntryT>(s) + (lo - row_base);
			m_idx = static_cast<Board_Index>(lo);
			return;
		}
		m_data = nullptr;
	}

	const Piece_Config_For_Gen* m_epsi;
	const uint8_t* m_pid_in_pair;
	const TableT* m_tbl;
	size_t m_within;
	size_t m_row_end;
	size_t m_chunk_end;
	const EntryT* m_data = nullptr;
	Board_Index m_idx{};
};

template <typename EntryT, typename TableT>
NODISCARD In_Pair_Cell_Iterator<EntryT, TableT> make_in_pair_cells(
	const Piece_Config_For_Gen& epsi, const std::vector<uint8_t>& pid_in_pair,
	const TableT& tbl, Board_Index chunk_start, Board_Index chunk_end)
{
	return In_Pair_Cell_Iterator<EntryT, TableT>(epsi, pid_in_pair, tbl, chunk_start, chunk_end);
}

void save_group_raw(const uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t magic);
void load_group_raw(uint8_t* data, size_t bytes,
                    const std::filesystem::path& path, uint64_t expected_magic);

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
		m_entries_per_group = m_within * m_slices_per_group;
		if (m_within > 1)
			m_within_div = Divider<uint64_t>(m_within);
		if (m_slices_per_group > 1)
			m_slices_per_group_div = Divider<uint64_t>(m_slices_per_group);
		if (m_entries_per_group > 1)
			m_entries_per_group_div = Divider<uint64_t>(m_entries_per_group);
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

	// Caller keeps the slice resident for the view's lifetime.
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

	NODISCARD std::pair<size_t, size_t> locate(size_t p) const
	{
		if (m_entries_per_group > 1)
		{
			const size_t g = p / m_entries_per_group_div;
			return { g, p - g * m_entries_per_group };
		}
		return { p, 0 };
	}

	template <size_t N = NUM_ENTRY_VARIANTS>
	NODISCARD std::enable_if_t<N == 1, MainEntryT> read(Board_Index pos) const
	{
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		MainEntryT e;
		const auto [g, off] = locate(p);
		std::memcpy(&e, m_groups[g].data() + off, sizeof(MainEntryT));
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
		const auto [g, off] = locate(p);
		std::memcpy(&e, m_groups[g].data() + off, sizeof(T));
		return e;
	}

	// Caller keeps the containing group resident.
	template <typename T = MainEntryT>
	NODISCARD const T& view_at(Board_Index pos) const
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [g, off] = locate(p);
		return *reinterpret_cast<const T*>(m_groups[g].data() + off);
	}

	template <typename T>
	void write(const T& tt, Board_Index pos)
	{
		static_assert(std::is_same_v<T, MainEntryT> || (std::is_same_v<T, OtherEntryTs> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [g, off] = locate(p);
		std::memcpy(m_groups[g].data() + off, &tt, sizeof(T));
		if (!m_dirty[g])
			m_dirty[g] = true;
	}

	template <typename T>
	void add_flags(Board_Index pos, T flags)
	{
		static_assert(sizeof(T) == sizeof(Underlying_Entry_Type));
		static_assert(MainEntryT::template is_allowed_flag_type<T> || (OtherEntryTs::template is_allowed_flag_type<T> || ...));
		const size_t p = static_cast<size_t>(pos);
		ASSERT(p < num_entries());
		const auto [g, off] = locate(p);
		m_groups[g].data()[off] |= static_cast<Underlying_Entry_Type>(flags);
		if (!m_dirty[g])
			m_dirty[g] = true;
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
		m_entries_per_group = 0;
		m_within_div = Divider<uint64_t>{};
		m_slices_per_group_div = Divider<uint64_t>{};
		m_entries_per_group_div = Divider<uint64_t>{};
	}

	void remove_disk_files(In_Out_Param<Thread_Pool> thread_pool)
	{
		if (m_disk_paths.size() <= 1)
		{
			std::error_code ec;
			for (const auto& p : m_disk_paths)
				std::filesystem::remove(p, ec);
			return;
		}

		std::atomic<size_t> next(0);
		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			std::error_code ec;
			for (;;)
			{
				const size_t g = next.fetch_add(1, std::memory_order_relaxed);
				if (g >= m_disk_paths.size()) return;
				std::filesystem::remove(m_disk_paths[g], ec);
			}
		});
	}

	NODISCARD size_t slices_per_group() const { return m_slices_per_group; }
	NODISCARD size_t num_groups() const { return m_groups.size(); }
	NODISCARD size_t group_id_of(size_t slice_id) const
	{
		return m_slices_per_group > 1 ? slice_id / m_slices_per_group_div : slice_id;
	}
	NODISCARD size_t group_id_of_index(size_t p) const
	{
		return m_entries_per_group > 1 ? p / m_entries_per_group_div : p;
	}
	NODISCARD size_t slice_id_of(size_t p) const
	{
		return m_within > 1 ? p / m_within_div : p;
	}

	NODISCARD size_t num_slices() const { return m_num_slices; }
	NODISCARD size_t within_slice_size() const { return m_within; }
	NODISCARD size_t num_entries() const { return m_num_slices * m_within; }

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

	void evict_all(In_Out_Param<Thread_Pool> thread_pool)
	{
		std::atomic<size_t> next(0);
		thread_pool->run_sync_task_on_all_threads([&](size_t) {
			for (;;)
			{
				const size_t g = next.fetch_add(1, std::memory_order_relaxed);
				if (g >= m_groups.size()) return;
				evict_group(g);
			}
		});
	}

private:
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
	std::vector<uint8_t> m_dirty;
	std::vector<uint64_t> m_last_used;
	std::vector<std::filesystem::path> m_disk_paths;
	std::filesystem::path m_disk_prefix;
	std::string m_disk_filename_fmt;
	uint64_t m_disk_magic = 0;
	size_t m_within = 0;
	size_t m_entries_per_group = 0;
	Divider<uint64_t> m_within_div{};
	Divider<uint64_t> m_slices_per_group_div{};
	Divider<uint64_t> m_entries_per_group_div{};
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
		// Singular detection groups cursed results with their decisive class.
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

template <typename EntryT, typename... OtherEntryTs>
struct Save_Group_Cache
{
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
	void sweep(std::unique_lock<std::mutex>& lk)
	{
		std::vector<Key> victims;
		for (auto it = fifo.begin();
		     it != fifo.end() && fifo.size() > cap; )
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

template <typename Local, typename EntryT, typename... OtherEntryTs, typename PerEntryFn,
          typename TableForSliceFn>
NODISCARD std::vector<Local> gather_egtb_info_parallel(
	In_Out_Param<Thread_Pool> thread_pool,
	const Piece_Config_For_Gen& epsi,
	Save_Group_Cache<EntryT, OtherEntryTs...>& cache,
	Color color,
	size_t num_positions,
	size_t max_workers,
	EGTB_Info& info,
	PerEntryFn&& per_entry,
	TableForSliceFn&& table_for_slice)
{
	struct Shard { EGTB_Info info; Local local; };

	auto& src = *cache.tables[table_for_slice(0)];
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
			Decomposed_Board_Index didx;
			for (;;)
			{
				const size_t g = next_group.fetch_add(1, std::memory_order_relaxed);
				if (g >= ng) break;
				const size_t s_begin = g * spg;
				const size_t s_end   = std::min(s_begin + spg, ns);
				for (size_t s = s_begin; s < s_end; )
				{
					const size_t ti = table_for_slice(s);
					size_t run_end = s + 1;
					while (run_end < s_end && table_for_slice(run_end) == ti) ++run_end;
					Pinned_Group_Range<EntryT, OtherEntryTs...> pin(cache, ti, g, g);
					auto& ssrc = *cache.tables[ti];
					for (; s < run_end; ++s)
					{
						const size_t base = s * within;
						if (base >= num_positions) return shard;
						const size_t end = std::min(base + within, num_positions);
						const EntryT* row = ssrc.template slice_view_as<EntryT>(s);
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

NODISCARD std::array<Piece_Group::Placement, PIECE_CLASS_NB>
	placements_from_position(const Piece_Config_For_Gen& epsi, const Position& pos);

NODISCARD Board_Index canonical_board_index(
	const Piece_Config_For_Gen& epsi,
	std::array<Piece_Group::Placement, PIECE_CLASS_NB>& placements,
	Const_Span<Square> white_castling_rooks = Const_Span<Square>(nullptr, size_t(0)),
	Const_Span<Square> black_castling_rooks = Const_Span<Square>(nullptr, size_t(0)));

// Squares of one side's castling rooks, ascending by file.
NODISCARD size_t castling_rook_squares(const Position& pos, Color c,
                                       Square out[Castling_Group::MAX_RIGHTS]);

NODISCARD Board_Index board_index_of_position(const Piece_Config_For_Gen& epsi, const Position& pos);

struct Slice_Reach_Scratch
{
	std::vector<uint8_t> kid_seen;
	std::vector<uint8_t> kid_expanded;
	std::vector<int32_t> kids;
};

void mark_king_neighbor_reach(uint8_t* need, const Piece_Config_For_Gen& epsi,
                              const std::vector<int32_t>& active_pids,
                              size_t g_start, size_t g_end, size_t spg,
                              Slice_Reach_Scratch& scratch);

void mark_push_target_reach(uint8_t* need, const Piece_Config_For_Gen& epsi,
                            const std::vector<int32_t>& active_pids,
                            const std::vector<std::vector<int32_t>>& targets_by_active,
                            size_t g_start, size_t g_end, size_t spg);

template <typename EntryT>
using Table_Reader_Map =
	std::unordered_map<Material_Key, std::unique_ptr<Table_Reader<EntryT>>, Material_Key_Hash>;

struct EGTB_Generator
{
	explicit EGTB_Generator(const Piece_Config& ps);

	// Internal pointers make copies invalid.
	EGTB_Generator(const EGTB_Generator&) = delete;
	EGTB_Generator& operator=(const EGTB_Generator&) = delete;
	EGTB_Generator(EGTB_Generator&&) = default;

	NODISCARD const Piece_Config_For_Gen& epsi() const { return m_epsi; }

	NODISCARD Fixed_Vector<Color, 2> table_colors() const
	{
		Fixed_Vector<Color, 2> r;
		r.emplace_back(WHITE);
		if (!m_is_symmetric)
			r.emplace_back(BLACK);
		return r;
	}

	NODISCARD static std::map<Material_Key, Piece_Config>
	enumerate_sub_materials(const Piece_Config& ps);

	NODISCARD static std::map<Material_Key, Piece_Config>
	enumerate_exit_materials(const Piece_Config& ps);

	template <typename ProbeFileT,
	          typename MapT = Table_Reader_Map<typename ProbeFileT::Entry_Type>>
	NODISCARD static MapT
	open_probes(const std::map<Material_Key, Piece_Config>& materials,
		const EGTB_Paths& paths, In_Out_Param<Thread_Pool> thread_pool)
	{
		MapT out;
		for (const auto& [key, cfg] : materials)
		{
			if (cfg.is_bare_kings()) continue;
			out[key] = std::make_unique<ProbeFileT>(paths, cfg, thread_pool);
		}
		return out;
	}

protected:
	Piece_Config_For_Gen m_epsi;

	// Owns the pointees used by the move lookup tables.
	std::map<Material_Key, Piece_Config_For_Gen> m_sub_epsi_store;

	// Indexed by mover, captured piece, and promotion type.
	const Piece_Config_For_Gen* m_sub_epsi_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	bool                        m_sub_mirror_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};
	Color                       m_sub_read_color_by_move[COLOR_NB][PIECE_NB][PIECE_TYPE_NB]{};

	struct Sub_Entry
	{
		const Piece_Config_For_Gen* epsi = nullptr;
		bool  mirror = false;
		Color read_color = WHITE;
	};
	// The indexed pair member is captured; the other becomes free.
	Sub_Entry m_pair_broken_survivor[COLOR_NB]{};

	static constexpr size_t CASTLING_DROP_SLOTS = 1 + COLOR_NB * Castling_Group::MAX_RIGHTS;

	NODISCARD static size_t castling_drop_slot(Color giver, size_t dropped)
	{
		if (dropped == 0) return 0;
		return 1 + static_cast<size_t>(giver) * Castling_Group::MAX_RIGHTS + (dropped - 1);
	}

	// [rights the mover lost][captured man][that man held a right][promotion]
	Sub_Entry m_castling_zeroing[CASTLING_DROP_SLOTS][PIECE_NB][2][PIECE_TYPE_NB]{};

	// [rights the mover lost][the captured member's color]
	Sub_Entry m_castling_pair_survivor[CASTLING_DROP_SLOTS][COLOR_NB]{};

	struct Castling_Zeroing_Move
	{
		Piece_Config child;
		std::vector<Piece> literal_men;
		Piece_Config::Castling_Rights_Counts literal_rights{ 0, 0 };
		Color mover = WHITE;
		size_t dropped = 0;        // rights the mover gave up making the capture
		Piece victim = PIECE_NONE;
		bool victim_held_right = false;
		bool victim_is_pair_member = false;
		Piece_Type promo = PIECE_TYPE_NONE;
	};

	NODISCARD static std::vector<Castling_Zeroing_Move>
	enumerate_castling_zeroing_moves(const Piece_Config& ps);

	template <typename Resolve>
	void register_castling_zeroing_moves(const Piece_Config& ps, Resolve&& resolve_sub);

	Sub_Entry m_rights_dropped[COLOR_NB][Castling_Group::MAX_RIGHTS + 1]{};

	bool m_is_symmetric;

	EGTB_Info m_info;

	NODISCARD Board_Index NOINLINE next_quiet_index_fallback(
		const Position_For_Gen& pos_gen, Piece mover, Square from, Square to) const;

	NODISCARD FORCE_INLINE Board_Index next_quiet_index(
		const Position_For_Gen& pos_gen, Move move) const
	{
		ASSERT(!move.is_promotion());
		ASSERT(!move.is_ep_capture());

		const Position& board = pos_gen.board_unchecked();
		const Square from = move.from();
		const Square to   = move.to();
		const Piece mover = board.piece_at(from);
		ASSERT(mover != PIECE_NONE);
		ASSERT(board.is_empty(to));

		const Decomposed_Board_Index& dix = pos_gen.index();
		const Board_Index pos = pos_gen.board_index();
		const bool has_castling = m_epsi.has_castling();

		if (has_castling)
		{
			for (const Color cc : { WHITE, BLACK })
			{
				if (m_epsi.castling_rights(cc) == 0) continue;
				if (from == board.king_square(cc)) return BOARD_INDEX_NONE;
				for (const Square r : m_epsi.castling_rook_squares(dix.king_slice_id, cc))
					if (from == r)
						return BOARD_INDEX_NONE;
			}

			if (piece_type(mover) == KING)
			{
				const int32_t sid = m_epsi.king_slice_manager()
					.castling_slice_with_free_king(dix.king_slice_id, to);
				if (sid == SLICE_NONE) return BOARD_INDEX_NONE;
				return m_epsi.change_king_slice(pos, dix.king_slice_id, sid);
			}
		}
		else if (piece_type(mover) == KING)
		{
			Square wk = board.king_square(WHITE);
			Square bk = board.king_square(BLACK);
			if (piece_color(mover) == WHITE) wk = to; else bk = to;
			const auto& look = m_epsi.king_slice_manager().lookup(wk, bk);
			if (look.slice_id == SLICE_NONE) return BOARD_INDEX_NONE;
			if (look.transform != Symmetry_Transform::IDENTITY || look.has_diag_stabilizer)
				return next_quiet_index_fallback(pos_gen, mover, from, to);
			return m_epsi.change_king_slice(pos, dix.king_slice_id, look.slice_id);
		}

		if (m_epsi.king_slice_manager().slice_has_stabilizer[dix.king_slice_id])
			return next_quiet_index_fallback(pos_gen, mover, from, to);

		if (piece_type(mover) == PAWN)
		{
			const int32_t pushed = m_epsi.pawn_slice_manager().slice_after_pawn_push(
				dix.pawn_slice_id, piece_color(mover), from, to);
			return m_epsi.change_pawn_slice(pos, dix.pawn_slice_id, pushed);
		}

		const Piece_Class cls = piece_class(mover);
		const Piece_Group::Placement_Index moved =
			m_epsi.group(cls).compound_index_after_quiet_move(
				pos_gen.placements_unchecked()[cls], from, to);
		return m_epsi.change_single_group_index(pos, dix.within[cls], moved, cls);
	}

	NODISCARD FORCE_INLINE static std::pair<Piece, Piece_Type> decode_move_kind(
		Move m, const Position& p)
	{
		Piece captured = PIECE_NONE;
		if (m.is_castling())
			return { PIECE_NONE, PIECE_TYPE_NONE };
		if (m.is_ep_capture())
		{
			const Square cap_sq = sq_make(sq_rank(m.from()), sq_file(m.to()));
			captured = p.piece_at(cap_sq);
		}
		else if (!p.is_empty(m.to()))
		{
			captured = p.piece_at(m.to());
		}
		const Piece_Type promo = m.is_promotion() ? m.promotion() : PIECE_TYPE_NONE;
		return { captured, promo };
	}

	NODISCARD FORCE_INLINE size_t rights_dropped_by_move(
		const Position_For_Gen& pos_gen, Move move, Out_Param<Color> giver) const
	{
		if (!m_epsi.has_castling()) return 0;

		const Position& board = pos_gen.board_unchecked();
		const Square from = move.from();
		const int32_t ks = pos_gen.index().king_slice_id;
		for (const Color c : { WHITE, BLACK })
		{
			const size_t held = m_epsi.castling_rights(c);
			if (held == 0) continue;
			if (from == board.king_square(c)) { *giver = c; return held; }
			for (const Square r : m_epsi.castling_rook_squares(ks, c))
				if (from == r) { *giver = c; return 1; }
		}
		return 0;
	}

	struct Exit_Site
	{
		size_t      dropped = 0;
		Color       giver = WHITE;
		Color       read_color = WHITE;
		Board_Index index = Board_Index(0);
	};

	NOINLINE void resolve_castling_impl(Position_For_Gen& pos_for_gen, Move move,
	                              size_t dropped, Color giver,
	                              Out_Param<Exit_Site> out) const
	{
		ASSERT(dropped <= Castling_Group::MAX_RIGHTS);

		const Sub_Entry& e = m_rights_dropped[giver][dropped];
		ASSERT(e.epsi != nullptr);

		Position& p = pos_for_gen.board_unchecked();
		const uint8_t rights_before = p.castling();
		const Piece captured = p.do_move(move);
		const Board_Index idx = e.mirror
			? board_index_of_position(*e.epsi, p.mirror())
			: board_index_of_position(*e.epsi, p);
		p.undo_move(move, captured, rights_before);

		*out = Exit_Site{ dropped, giver, e.read_color, idx };
	}

	NODISCARD FORCE_INLINE bool resolve_castling(Position_For_Gen& pos_for_gen, Move move,
	                                         Out_Param<Exit_Site> out) const
	{
		Color giver = WHITE;
		const size_t dropped = rights_dropped_by_move(pos_for_gen, move, out_param(giver));
		if (dropped == 0) return false;
		resolve_castling_impl(pos_for_gen, move, dropped, giver, out);
		return true;
	}

	template <typename Map, typename Ptr>
	void bind_exit_readers(const Map& by_material,
	                       Ptr (&out)[COLOR_NB][Castling_Group::MAX_RIGHTS + 1]) const
	{
		for (const Color c : { WHITE, BLACK })
			for (size_t d = 0; d <= Castling_Group::MAX_RIGHTS; ++d)
			{
				out[c][d] = nullptr;
				const Sub_Entry& e = m_rights_dropped[c][d];
				if (e.epsi == nullptr) continue;
				const auto it = by_material.find(e.epsi->min_material_key());
				if (it != by_material.end()) out[c][d] = it->second.get();
			}
	}

	NODISCARD Board_Index next_sub_index(
		Position_For_Gen& pos_for_gen, Move move,
		Out_Param<Color> sub_color,
		Out_Param<const Piece_Config_For_Gen*> sub_epsi_out) const
	{
		Position& p = pos_for_gen.board_unchecked();
		const Color mover = p.turn();
		const auto [captured, promo] = decode_move_kind(move, p);

		const Piece_Config_For_Gen* sub;
		bool mirr;
		Color read_color;

		bool castling_zeroing_handled = false;
		if (m_epsi.has_castling() && captured != PIECE_NONE)
		{
			const Color victim = piece_color(captured);
			const Square cap_sq = move.is_ep_capture()
				? sq_make(sq_rank(move.from()), sq_file(move.to()))
				: move.to();

			bool victim_held_right = false;
			if (piece_type(captured) == ROOK)
				for (const Square r : m_epsi.castling_rook_squares(
				         pos_for_gen.index().king_slice_id, victim))
					if (cap_sq == r) { victim_held_right = true; break; }

			Color giver = WHITE;
			const size_t dropped = rights_dropped_by_move(pos_for_gen, move, out_param(giver));
			const size_t slot = castling_drop_slot(giver, dropped);

			const Sub_Entry* e = nullptr;
			if (m_epsi.pair_group())
			{
				const auto pd = m_epsi.pawn_slice_manager().decompose(
					pos_for_gen.index().pawn_slice_id);
				if (cap_sq == m_epsi.pair_group()->white_square(pd.pair_idx))
					e = &m_castling_pair_survivor[slot][WHITE];
				else if (cap_sq == m_epsi.pair_group()->black_square(pd.pair_idx))
					e = &m_castling_pair_survivor[slot][BLACK];
			}
			if (e == nullptr)
				e = &m_castling_zeroing[slot][captured][victim_held_right ? 1 : 0][promo];
			ASSERT(e->epsi != nullptr);
			sub = e->epsi; mirr = e->mirror; read_color = e->read_color;
			castling_zeroing_handled = true;
		}

		bool pair_handled = castling_zeroing_handled;
		if (!castling_zeroing_handled && m_epsi.pair_group() && captured != PIECE_NONE)
		{
			const auto pd = m_epsi.pawn_slice_manager().decompose(
				pos_for_gen.index().pawn_slice_id);
			const Square pair_w = m_epsi.pair_group()->white_square(pd.pair_idx);
			const Square pair_b = m_epsi.pair_group()->black_square(pd.pair_idx);

			const Square cap_sq = move.is_ep_capture()
				? sq_make(sq_rank(move.from()), sq_file(move.to()))
				: move.to();

			if (cap_sq == pair_w || cap_sq == pair_b)
			{
				const Color pc = (cap_sq == pair_w) ? WHITE : BLACK;
				const Sub_Entry& e = m_pair_broken_survivor[pc];
				sub = e.epsi; mirr = e.mirror; read_color = e.read_color;
				pair_handled = true;
			}
		}

		if (!pair_handled)
		{
			sub = m_sub_epsi_by_move[mover][captured][promo];
			mirr = m_sub_mirror_by_move[mover][captured][promo];
			read_color = m_sub_read_color_by_move[mover][captured][promo];
		}
		ASSERT(sub != nullptr);

		*sub_epsi_out = sub;
		*sub_color = read_color;

		const uint8_t rights_before = p.castling();
		const Piece captured_by_move = p.do_move(move);

		Board_Index result;
		if (mirr)
		{
			result = board_index_of_position(*sub, p.mirror());
		}
		else
		{
			result = board_index_of_position(*sub, p);
		}

		p.undo_move(move, captured_by_move, rights_before);
		return result;
	}

	static constexpr size_t CHUNK_SIZE = CACHE_LINE_SIZE * CHAR_BIT * 64;

	NODISCARD Shared_Board_Index_Iterator make_slice_group_iterator(
		size_t group_id, size_t slices_per_group) const;

	std::vector<int32_t> m_active_pawn_slices = { 0 };
	std::vector<uint8_t> m_pid_in_pair;        // [num_pawn_slices]
	std::vector<int32_t> m_pid_in_pair_marked;
	std::vector<size_t>  m_pair_group_ids;     // groups touched by m_active_pawn_slices
	std::vector<std::vector<int32_t>> m_targets_by_active;

	std::vector<uint8_t> m_all_groups;
	std::vector<uint8_t> m_fusion_groups;
	std::vector<uint8_t> m_fusion_init_groups;
	size_t m_fusion_group_count = 0;
	size_t m_fusion_init_group_count = 0;

	// A pinned phase needs no dispatch-time paging.
	bool m_phase_pinned = false;

	Slice_Reach_Scratch m_reach_scratch;

	void mark_king_neighbor_reach(uint8_t* need, size_t g_start, size_t g_end, size_t spg)
	{
		::mark_king_neighbor_reach(need, m_epsi, m_active_pawn_slices,
		                           g_start, g_end, spg, m_reach_scratch);
	}
	void mark_push_target_reach(uint8_t* need, size_t g_start, size_t g_end, size_t spg)
	{
		::mark_push_target_reach(need, m_epsi, m_active_pawn_slices, m_targets_by_active,
		                         g_start, g_end, spg);
	}

	size_t m_paging_budget_bytes = 0;
	uint64_t m_paging_tick = 0;

	// Conservative group frontier; every write reinstates its group.
	std::vector<uint8_t> m_iter_groups[COLOR_NB];

	// Chunk-level companion to the group frontier.
	std::vector<uint8_t> m_iter_chunks[COLOR_NB];

	std::vector<uint8_t> m_scratch_need[COLOR_NB];

	void init_iter_state(size_t num_groups, size_t total_index_space)
	{
		const size_t num_chunks = (total_index_space + CHUNK_SIZE - 1) / CHUNK_SIZE;
		for (Color c : { WHITE, BLACK })
		{
			m_iter_groups[c].assign(num_groups, 0);
			m_iter_chunks[c].assign(num_chunks, 0);
		}
	}

	void seed_iter_groups()
	{
		for (Color c : { WHITE, BLACK })
		{
			std::fill(m_iter_groups[c].begin(), m_iter_groups[c].end(), 1);
			std::fill(m_iter_chunks[c].begin(), m_iter_chunks[c].end(), 1);
		}
	}

	template <typename EntryT, typename... OtherEntryTs>
	void mark_iter(Color c, Board_Index pos,
	                      const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& f)
	{
		const size_t group = f.group_id_of_index(static_cast<size_t>(pos));
		if (m_iter_groups[c][group] == 0)
			m_iter_groups[c][group] = 1;

		const size_t chunk = static_cast<size_t>(pos) / CHUNK_SIZE;
		if (m_iter_chunks[c][chunk] == 0)
			m_iter_chunks[c][chunk] = 1;
	}

	template <typename EntryT, typename... OtherEntryTs>
	void refresh_active_metadata(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl);

	void set_active_fusion(const Pawn_Slice_Manager& psm,
	                       const std::vector<int32_t>& fusion);

	template <typename EntryT, typename... OtherEntryTs>
	NODISCARD std::vector<std::vector<int32_t>>
	compute_fusion_groups(const Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>& tbl,
	                      const std::vector<int32_t>& batch,
	                      size_t own_layers, size_t target_layers) const;

	template <typename EntryT, typename... OtherEntryTs>
	void apply_working_set(
		In_Out_Param<Thread_Pool> thread_pool,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
		const std::vector<uint8_t>& needed_w,
		const std::vector<uint8_t>& needed_b);

	template <typename EntryT, typename... OtherEntryTs>
	NODISCARD bool try_pin_phase(
		In_Out_Param<Thread_Pool> thread_pool,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* w_tbl,
		Sliced_EGTB_File_For_Gen<EntryT, OtherEntryTs...>* b_tbl,
		const std::vector<uint8_t>& needed_w,
		const std::vector<uint8_t>& needed_b,
		size_t group_charge);
};

// Generator working-set estimate for the common two-byte layout.
struct Working_Set_Estimate
{
	size_t num_positions = 0;
	size_t total_table_bytes = 0;   // both colors, sizeof(*_Final_Entry) per cell
	size_t bytes_per_slice = 0;
	size_t slices_per_group = 0;
	size_t bytes_per_group = 0;
	size_t num_slices = 0;
	size_t num_groups = 0;

	size_t peak_dispatch_iter_groups = 0;
	size_t peak_dispatch_init_groups = 0;

	size_t peak_pair_iter_groups = 0;
	size_t peak_pair_init_groups = 0;

	size_t peak_batch_iter_groups = 0;
	size_t peak_batch_init_groups = 0;
};

NODISCARD Working_Set_Estimate compute_working_set(const Piece_Config& ps);
