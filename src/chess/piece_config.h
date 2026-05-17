#pragma once

#include "chess.h"

#include "util/defines.h"
#include "util/span.h"

#include <vector>
#include <map>
#include <set>

// One class per piece type per color.
// Order within a color is K, N, B, R, Q, P — kings first / pawns last (both
// handled by dedicated slice managers), middle in ascending material so the
// innermost index dimension is the lowest-impact piece. Adjacent linear-index
// entries then often share EGTB values, lengthening runs for the block compressor.
enum Piece_Type_Class : int8_t {
	KINGS, KNIGHTS, BISHOPS, ROOKS, QUEENS, PAWNS, PIECE_TYPE_CLASS_NB = 6
};

enum Piece_Class : int8_t {
	BLACK_KINGS, BLACK_KNIGHTS, BLACK_BISHOPS, BLACK_ROOKS, BLACK_QUEENS, BLACK_PAWNS,
	WHITE_KINGS, WHITE_KNIGHTS, WHITE_BISHOPS, WHITE_ROOKS, WHITE_QUEENS, WHITE_PAWNS,
	PIECE_CLASS_START = 0, PIECE_CLASS_END = 12, PIECE_CLASS_NONE = -1, PIECE_CLASS_NB = 12
};

constexpr Piece_Class& operator++(Piece_Class& p_class)
{
	p_class = static_cast<Piece_Class>(static_cast<int>(p_class) + 1);
	return p_class;
}

NODISCARD constexpr Piece_Class make_piece_class(Color color, Piece_Type_Class pt_class)
{
	ASSERT(pt_class < PIECE_TYPE_CLASS_NB);
	return static_cast<Piece_Class>((color == WHITE ? WHITE_KINGS : BLACK_KINGS) + pt_class);
}

constexpr std::array<Piece_Class, PIECE_NB> PIECE_TO_PIECE_CLASS = []() {
	std::array<Piece_Class, PIECE_NB> arr{};
	for (auto& v : arr) v = PIECE_CLASS_NONE;
	arr[BLACK_KING]   = BLACK_KINGS;
	arr[BLACK_QUEEN]  = BLACK_QUEENS;
	arr[BLACK_ROOK]   = BLACK_ROOKS;
	arr[BLACK_BISHOP] = BLACK_BISHOPS;
	arr[BLACK_KNIGHT] = BLACK_KNIGHTS;
	arr[BLACK_PAWN]   = BLACK_PAWNS;
	arr[WHITE_KING]   = WHITE_KINGS;
	arr[WHITE_QUEEN]  = WHITE_QUEENS;
	arr[WHITE_ROOK]   = WHITE_ROOKS;
	arr[WHITE_BISHOP] = WHITE_BISHOPS;
	arr[WHITE_KNIGHT] = WHITE_KNIGHTS;
	arr[WHITE_PAWN]   = WHITE_PAWNS;
	return arr;
}();

constexpr Piece_Class piece_class(Piece p)
{
	return PIECE_TO_PIECE_CLASS[p];
}

struct Unique_Piece_Configs;

// Represents a set of pieces on the board, stored in canonical ordering.
// "Canonical" here means side ordering by total strength
// (stronger side becomes WHITE), then piece ordering by strength within each side.
struct Piece_Config
{
	// Uppercase letters are ordinary piece types (color set by side ordering).
	// Lowercase 'p' is a frozen opposing pawn pair: one white + one black pawn
	// locked head-to-head, indexed jointly. At most one 'p' per material.
	static constexpr char VALID_PIECES[] = "KQRBNPp";

	static constexpr std::array<int16_t, PIECE_NB> PIECE_STRENGTH_FOR_SIDE_ORDER = []() {
		std::array<int16_t, PIECE_NB> arr{};
		// Kings present on both sides — value irrelevant for side ordering.
		arr[WHITE_KING]   = arr[BLACK_KING]   = 0;
		arr[WHITE_QUEEN]  = arr[BLACK_QUEEN]  = 900;
		arr[WHITE_ROOK]   = arr[BLACK_ROOK]   = 500;
		arr[WHITE_BISHOP] = arr[BLACK_BISHOP] = 330;
		arr[WHITE_KNIGHT] = arr[BLACK_KNIGHT] = 320;
		arr[WHITE_PAWN]   = arr[BLACK_PAWN]   = 100;
		return arr;
	}();

	// Sort order within a side: K, Q, R, B, N, P (descending strength, kings first).
	static constexpr std::array<int8_t, PIECE_NB> PIECE_ORDER = []() {
		std::array<int8_t, PIECE_NB> ret{};
		ret[WHITE_OCCUPY] = 0;
		ret[BLACK_OCCUPY] = 0;
		int8_t i = 1;
		constexpr Color AllColors[] = { WHITE, BLACK };
		for (const Color color : AllColors)
		{
			ret[piece_make(color, KING)]   = i++;
			ret[piece_make(color, QUEEN)]  = i++;
			ret[piece_make(color, ROOK)]   = i++;
			ret[piece_make(color, BISHOP)] = i++;
			ret[piece_make(color, KNIGHT)] = i++;
			ret[piece_make(color, PAWN)]   = i++;
		}
		return ret;
	}();

	static void sort_pieces(Span<Piece> pieces);

	NODISCARD static bool is_constructible_from(const std::string& name)
	{
		if (name.empty() || name.size() > MAX_MAN)
			return false;

		if (name[0] != 'K')
			return false;

		if (std::count(name.begin(), name.end(), 'K') != 2)
			return false;

		if (name.find_first_not_of(VALID_PIECES) != std::string::npos)
			return false;

		// The single frozen pair is written as one 'p' on each side (e.g. KQpKp)
		// to make it obvious both colors hold a pawn. So there are either zero
		// 'p's or exactly two -- one before and one after the second king.
		const auto num_p = std::count(name.begin(), name.end(), 'p');
		if (num_p != 0 && num_p != 2)
			return false;
		if (num_p == 2)
		{
			size_t kings = 0, second_k = std::string::npos;
			for (size_t i = 0; i < name.size(); ++i)
				if (name[i] == 'K' && ++kings == 2) { second_k = i; break; }
			const auto p_white = std::count(name.begin(), name.begin() + second_k, 'p');
			if (p_white != 1)  // the other 'p' is necessarily on the black side
				return false;
		}

		return true;
	}

	NODISCARD static bool is_constructible_from(Const_Span<Piece> pieces)
	{
		if (pieces.size() < 2 || pieces.size() > MAX_MAN)
			return false;

		if (std::count(pieces.begin(), pieces.end(), WHITE_KING) != 1)
			return false;

		if (std::count(pieces.begin(), pieces.end(), BLACK_KING) != 1)
			return false;

		return true;
	}

	Piece_Config(const std::string& s) :
		m_num_pieces(0)
	{
		if (!is_constructible_from(s))
			throw std::runtime_error("Invalid PieceConfig: " + s);

		bool is_black = false;
		for (const char c : s)
		{
			// Lowercase 'p' is the frozen pair, not a board piece: it never
			// enters m_pieces and does not flip the white/black side boundary.
			if (c == 'p')
			{
				m_has_frozen_pair = true;
				continue;
			}

			const Piece_Type pt = piece_type(piece_from_char(c));
			if (m_num_pieces > 0 && pt == KING)
				is_black = true;

			const Piece p = piece_make(is_black ? BLACK : WHITE, pt);
			m_pieces[m_num_pieces++] = p;
		}

		sort_pieces(Span(m_pieces, m_num_pieces));

		for (const Piece p : Const_Span(m_pieces, m_pieces + m_num_pieces))
		{
			m_base_mat_key.add_piece(p);
			m_mirr_mat_key.add_piece(piece_opp_color(p));
		}
		if (m_has_frozen_pair)
		{
			m_base_mat_key.add_pair();
			m_mirr_mat_key.add_pair();
		}
	}

	Piece_Config(Const_Span<Piece> pcs) :
		m_num_pieces(0)
	{
		if (!is_constructible_from(pcs))
			throw std::runtime_error("Invalid PieceConfig.");

		std::memcpy(m_pieces, pcs.data(), pcs.size() * sizeof(Piece));

		sort_pieces(Span(m_pieces, pcs.size()));

		for (const Piece p : Const_Span(m_pieces, m_pieces + pcs.size()))
		{
			m_base_mat_key.add_piece(p);
			m_mirr_mat_key.add_piece(piece_opp_color(p));
		}
		m_num_pieces = pcs.size();
	}

	NODISCARD auto pieces() const
	{
		return Const_Span(m_pieces, m_pieces + m_num_pieces);
	}

	NODISCARD std::string name() const
	{
		// Pieces are sorted white-side first (each side led by its king). The
		// frozen pair contributes one pawn to each side, so emit a 'p' at the end
		// of the white side (just before the second king) and another at the very
		// end, giving e.g. "KQpKp" -- obvious that both colors hold a pawn, and it
		// round-trips through the string constructor.
		std::string s;
		bool seen_white_king = false;
		for (const Piece p : pieces())
		{
			const Piece_Type pt = piece_type(p);
			if (pt == KING)
			{
				if (seen_white_king && m_has_frozen_pair)
					s += 'p';
				seen_white_king = true;
			}
			s += piece_type_to_char(pt);
		}
		if (m_has_frozen_pair)
			s += 'p';
		return s;
	}

	NODISCARD const std::array<size_t, PIECE_NB> piece_counts() const
	{
		std::array<size_t, PIECE_NB> counts;
		std::fill(counts.begin(), counts.end(), 0);
		for (const Piece piece : pieces())
			counts[piece] += 1;
		return counts;
	}

	NODISCARD bool operator==(const Piece_Config& other) const
	{
		return m_num_pieces == other.m_num_pieces
			&& m_has_frozen_pair == other.m_has_frozen_pair
			&& std::equal(m_pieces, m_pieces + m_num_pieces, other.m_pieces);
	}

	NODISCARD bool can_remove_piece(size_t idx) const
	{
		return idx < m_num_pieces && piece_type(m_pieces[idx]) != KING;
	}

	NODISCARD size_t num_pieces() const
	{
		return m_num_pieces;
	}

	NODISCARD bool has_frozen_pair() const
	{
		return m_has_frozen_pair;
	}

	NODISCARD bool has_any_free_attackers(Color color) const
	{
		for (const Piece p : pieces())
			if (piece_color(p) == color && is_piece_free_attacker(p))
				return true;
		return false;
	}

	NODISCARD bool has_any_free_attackers() const
	{
		// By the strength-based side ordering, if any side has free attackers
		// white has at least as many strength-wise → white has some.
		return has_any_free_attackers(WHITE);
	}

	NODISCARD Piece_Config with_removed_piece(size_t idx) const
	{
		if (!can_remove_piece(idx))
			throw std::runtime_error("Trying to remove a king from PieceConfig.");

		Piece pcs_cpy[MAX_MAN];
		std::memcpy(pcs_cpy, m_pieces, idx * sizeof(Piece));
		std::memcpy(pcs_cpy + idx, m_pieces + idx + 1, (m_num_pieces - idx - 1) * sizeof(Piece));
		Piece_Config res(Span(pcs_cpy, m_num_pieces - 1));
		// A non-pair piece was removed; the frozen pair (if any) rides along.
		if (m_has_frozen_pair)
			res.mark_frozen_pair();
		return res;
	}

	// Returns this config with the piece at idx replaced by `replacement`.
	// Used to enumerate promotion variants (pawn → Q/R/B/N).
	NODISCARD Piece_Config with_replaced_piece(size_t idx, Piece replacement) const
	{
		ASSERT(idx < m_num_pieces);
		Piece pcs_cpy[MAX_MAN];
		std::memcpy(pcs_cpy, m_pieces, m_num_pieces * sizeof(Piece));
		pcs_cpy[idx] = replacement;
		Piece_Config res(Span(pcs_cpy, m_num_pieces));
		// Promoting a free pawn leaves the frozen pair (if any) intact.
		if (m_has_frozen_pair)
			res.mark_frozen_pair();
		return res;
	}

	// Pair-breaking sub-config "p -> PP": a frozen-pair member captures the free
	// piece at `capture_idx` (an ordinary diagonal capture, or an en-passant
	// capture of a free pawn). Both former pair pawns survive as free pawns and
	// the captured piece is gone; the result no longer carries the pair.
	NODISCARD Piece_Config pair_broken_by_capture(size_t capture_idx) const;

	// Pair-breaking sub-config "p -> P": one frozen-pair member is captured by an
	// enemy piece. The surviving member (of color `survivor`) becomes a free
	// pawn; the result no longer carries the pair.
	NODISCARD Piece_Config pair_broken_survivor(Color survivor) const;

	// Pawns in this config, returned as (index, piece) pairs.
	NODISCARD std::vector<std::pair<size_t, Piece>> pawn_indices() const
	{
		std::vector<std::pair<size_t, Piece>> out;
		for (size_t i = 0; i < m_num_pieces; ++i)
			if (piece_type(m_pieces[i]) == PAWN)
				out.emplace_back(i, m_pieces[i]);
		return out;
	}

	// Returns the promotion-result configs: for each pawn, for each promoted type
	// (Q/R/B/N), the resulting Piece_Config (canonicalized — sides may swap).
	// Keyed by (original_pawn_piece, promoted_to_type).
	NODISCARD std::map<std::pair<Piece, Piece_Type>, Piece_Config> promotion_sub_configs() const;

	// Returns whether removing the given piece would cause the
	// strength-ordered sides to swap (white becomes black, vice versa).
	NODISCARD bool needs_mirror_after_capture(Piece cap_piece) const
	{
		// By construction white side is always >= in strength,
		// so removing a black piece can never flip the order.
		if (piece_color(cap_piece) == BLACK)
			return false;

		size_t score[COLOR_NB] = { 0, 0 };
		for (const Piece p : pieces())
			score[piece_color(p)] += PIECE_STRENGTH_FOR_SIDE_ORDER[p];

		ASSERT(score[WHITE] >= score[BLACK]);
		ASSERT(piece_color(cap_piece) == WHITE);

		return score[BLACK] > score[WHITE] - PIECE_STRENGTH_FOR_SIDE_ORDER[cap_piece];
	}

	NODISCARD Unique_Piece_Configs sub_configs() const;
	NODISCARD std::map<Piece, Piece_Config> sub_configs_by_capture() const;
	NODISCARD Unique_Piece_Configs closure() const;

	void add_sub_configs_to(Unique_Piece_Configs& pss) const;
	void add_closure_in_dependency_order_to(Unique_Piece_Configs& pss, bool assume_contains_closures = false) const;

	NODISCARD Material_Key base_material_key() const
	{
		return m_base_mat_key;
	}

	NODISCARD std::pair<Material_Key, Material_Key> material_keys() const
	{
		return { m_base_mat_key, m_mirr_mat_key };
	}

	NODISCARD Material_Key min_material_key() const
	{
		return std::min(m_base_mat_key, m_mirr_mat_key);
	}

	// Sets the frozen-pair flag on an already-built config and stamps it into
	// both material keys (which were computed over the free pieces only). Used
	// to carry the pair onto capture/promotion sub-configs that keep it intact.
	void mark_frozen_pair()
	{
		m_has_frozen_pair = true;
		m_base_mat_key.add_pair();
		m_mirr_mat_key.add_pair();
	}

private:
	Piece m_pieces[MAX_MAN];
	size_t m_num_pieces;
	bool m_has_frozen_pair = false;
	Material_Key m_base_mat_key;
	Material_Key m_mirr_mat_key;
};

// Insertion-ordered set of Piece_Configs (uniqueness by material key).
struct Unique_Piece_Configs
{
	using Container_Type = std::vector<Piece_Config>;
	using iterator = typename Container_Type::iterator;
	using const_iterator = typename Container_Type::const_iterator;
	using reverse_iterator = typename Container_Type::reverse_iterator;
	using const_reverse_iterator = typename Container_Type::const_reverse_iterator;

	NODISCARD const Piece_Config& operator[](size_t idx) const { return m_piece_sets[idx]; }

	void clear()
	{
		m_mat_keys.clear();
		m_piece_sets.clear();
	}

	NODISCARD bool contains(const Piece_Config& ps) const
	{
		return m_mat_keys.find(ps.base_material_key()) != m_mat_keys.end();
	}

	void add_unique(Piece_Config ps)
	{
		if (!contains(ps))
		{
			m_mat_keys.insert(ps.base_material_key());
			m_piece_sets.emplace_back(std::move(ps));
		}
	}

	void add_unique(const Unique_Piece_Configs& pss)
	{
		for (const auto& ps : pss)
			add_unique(ps);
	}

	void remove(const Piece_Config& ps)
	{
		auto iter = std::find(m_piece_sets.begin(), m_piece_sets.end(), ps);
		if (iter != m_piece_sets.end())
		{
			m_mat_keys.erase(ps.base_material_key());
			m_piece_sets.erase(iter);
		}
	}

	template <typename FuncT>
	void remove_if(FuncT&& f)
	{
		auto new_end = std::remove_if(m_piece_sets.begin(), m_piece_sets.end(), std::forward<FuncT>(f));
		for (auto it = new_end; it != m_piece_sets.end(); ++it)
			m_mat_keys.erase(it->base_material_key());
		m_piece_sets.erase(new_end, m_piece_sets.end());
	}

	NODISCARD size_t size() const  { return m_piece_sets.size(); }
	NODISCARD bool   empty() const { return m_piece_sets.empty(); }

	NODISCARD const_iterator begin()  const { return m_piece_sets.begin(); }
	NODISCARD const_iterator cbegin() const { return m_piece_sets.cbegin(); }
	NODISCARD const_iterator end()    const { return m_piece_sets.end(); }
	NODISCARD const_iterator cend()   const { return m_piece_sets.cend(); }

	NODISCARD const_reverse_iterator rbegin()  const { return m_piece_sets.rbegin(); }
	NODISCARD const_reverse_iterator crbegin() const { return m_piece_sets.crbegin(); }
	NODISCARD const_reverse_iterator rend()    const { return m_piece_sets.rend(); }
	NODISCARD const_reverse_iterator crend()   const { return m_piece_sets.crend(); }

private:
	Container_Type m_piece_sets;
	std::set<Material_Key> m_mat_keys;
};
