#include <algorithm>
#include <memory>
#include <vector>
#include <exception>
#include <sstream>

#include "strategy.hpp"
#include "game.hpp"
#include "interact.hpp"

namespace {

/* TODO(fyquah): Globals? Ewwwwww. */
static bool glob_is_stock_pile_explored;
static std::vector<card_t> glob_stock_pile;

enum location_tag_t
{
  LOC_WASTE_PILE = 0,
  LOC_TABLEAU = 1,
  LOC_FOUNDATION = 2
};

class Location
{
public:
  virtual uint32_t index () = 0;
  virtual uint32_t sub_index () = 0;
  virtual location_tag_t tag () = 0;
  virtual std::string to_string() = 0;
};

class WastePile : public Location
{
public:
  uint32_t index ()
  {
    throw std::exception();
  }

  uint32_t sub_index ()
  {
    throw std::exception();
  }

  location_tag_t tag()
  {
    return LOC_WASTE_PILE;
  }

  std::string to_string() {
    return std::string("waste_pile");
  }
};

class Tableau : public Location
{
private:
  uint32_t sub_index_;
  uint32_t index_;

public:

  Tableau(uint32_t index_, uint32_t sub_index_)
    : index_(index_), sub_index_(sub_index_) {}

  uint32_t index()
  {
    return index_;
  }

  uint32_t sub_index()
  {
    return sub_index_;
  }

  location_tag_t tag()
  {
    return LOC_TABLEAU;
  }

  std::string to_string() {
    std::stringstream ss;
    ss << "Tableau deck " << index_ << " sub-index " << sub_index_;
    return ss.str();
  }
};

class Foundation : public Location
{
private:
  uint32_t index_;

public:
  Foundation(uint32_t index_) : index_(index_) {}

  uint32_t index()
  {
    return index_;
  }

  uint32_t sub_index()
  {
    throw std::exception();
  }

  location_tag_t tag()
  {
    return LOC_FOUNDATION;
  }

  std::string to_string() {
    std::stringstream ss;
    ss << "Foundation " << index_;
    return ss.str();
  }
};

class Move 
{
public:
  std::shared_ptr<Location> from;
  std::shared_ptr<Location> to;

  Move(
      const std::shared_ptr<Location> & from,
      const std::shared_ptr<Location> & to
  ) : from(from), to(to) {}

  Move & operator=(const Move & other) {
    from = other.from;
    to = other.to;
    return *this;
  }
};

std::ostream& operator<<(std::ostream & out, const Move & move) {
  return out << "from " << move.from.get()->to_string()
    << " to " << move.to.get()->to_string();
}

static void update_glob_stock_pile(const game_state_t & state, Move move)
{
  if (move.from.get()->tag() == LOC_WASTE_PILE) {
    card_t card = state.waste_pile_top.get();

    glob_stock_pile.erase(
        std::remove(
          glob_stock_pile.begin(),
          glob_stock_pile.end(),
          card),
        glob_stock_pile.end());
  }
}

static bool possible_to_play_deuce(
    const game_state_t & state, const card_t & card)
{
  suite_t suite = card.suite;
  bool ret = 
    state.foundation[suite].is_some()
    && state.foundation[suite].get().number == ACE
    && card.number == DEUCE;

  if (ret) {
    std::cout
      << "Possible to promote "
      << card.to_string()
      << " to foundation"
      << std::endl;
  }

  return ret;
}

static inline std::shared_ptr<Location> loc_waste_pile(){
  return std::make_shared<WastePile>();
}

static inline std::shared_ptr<Location> loc_foundation(int index) {
  return std::make_shared<Foundation>(index);
}

static inline std::shared_ptr<Location> loc_tableau(int index, int subindex) {
  return std::make_shared<Tableau>(index, subindex);
}

static inline std::shared_ptr<Move> make_move(
    std::shared_ptr<Location> src,
    std::shared_ptr<Location> dest)
{
  return std::make_shared<Move>(src, dest);
}

/* Obvious moves are moves that strictly lead the game to a better
 * state.
 */
std::shared_ptr<Move> calculate_obvious_move(const game_state_t & state)
{
  std::vector<Move> ret;
  Option<card_t> waste_pile_top = state.waste_pile_top;
  const tableau_deck_t *tbl_deck = state.tableau;

  /* Rule 0: If there's an Ace play it. */
  if (waste_pile_top.is_some() && waste_pile_top.get().number == ACE) {
    const card_t & card = waste_pile_top.get();
    return std::make_shared<Move>(
      std::make_shared<WastePile>(),
      std::make_shared<Foundation>(card.suite));
  }
  
  for (int i = 0; i < 7 ; i++) {
    if (tbl_deck[i].cards.size() != 0
        && tbl_deck[i].cards.back().number == ACE) {
      const card_t & card = tbl_deck[i].cards.back();
      return std::make_shared<Move>(
        std::make_shared<Tableau>(
            i, tbl_deck[i].cards.size() - 1),
        std::make_shared<Foundation>(card.suite));
    }
  }

  /* Rule 1: Play deuce where possible */
  if (waste_pile_top.is_some()) {
    const card_t card = waste_pile_top.get();
    
    if (possible_to_play_deuce(state, card)) {
      return make_move(
          loc_waste_pile(),
          loc_foundation(uint32_t(card.suite)));
    }
  }
  
  for (int i = 0; i < 7 ; i++) {
    if (tbl_deck[i].cards.size() != 0) {
      const std::vector<card_t> & cards = tbl_deck[i].cards;
      const card_t & card = cards.back();

      if (possible_to_play_deuce(state, card)) {

        return make_move(
            loc_tableau(i, cards.size() - 1),
            loc_foundation(uint32_t(card.suite)));
      }
    }
  }

  /* Rule 2: If there is a move that frees a down card - play that move.
   * In case there are multiple such moves, play that one that frees the
   * one with the greatest number of hidden cards. (except for promoting
   * to foundation. That is a little risky)
   */
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>>
    downcard_freeing_candidates;

  for (int i = 0 ; i < 7 ; i++) {
    /* This implies cards.size() != 0 */
    if (tbl_deck[i].num_down_cards == 0) {
      continue;
    }

    card_t src = tbl_deck[i].cards.at(0);

    for (int j = 0 ; j < 7 ; j++) {
      if (i == j) {
        continue;
      }

      if (tbl_deck[j].cards.size() == 0) {

        if (src.number == KING) {
          downcard_freeing_candidates.push_back(
              std::make_pair(
                tbl_deck[i].num_down_cards,
                std::make_pair(i, j))
          );
        }

      } else {
        card_t dest = tbl_deck[j].cards.back();
  
        if (src.number == dest.number- 1 &&
            suite_color(src.suite) != suite_color(dest.suite)) {
          downcard_freeing_candidates.push_back(
              std::make_pair(
                tbl_deck[i].num_down_cards,
                std::make_pair(i, j))
          );
        }
      }
    }
  }

  if (downcard_freeing_candidates.size() != 0) {
    const auto p = std::max_element(
        downcard_freeing_candidates.begin(),
        downcard_freeing_candidates.end()
    );
    uint32_t src = p->second.first;
    uint32_t dest = p->second.second;

    std::cout << "Moving visible tableau from "
      << src << "("<< state.tableau[src].cards.back().to_string() << ")"
      << " -> "
      << dest << "(";

    if (state.tableau[dest].cards.size() == 0) {
      std::cout << "<NONE>";
    } else {
      std::cout << state.tableau[dest].cards.at(0).to_string();
    }

    std::cout << ") to release more hidden cards\n";

    return make_move(
        loc_tableau(src, 0),
        loc_tableau(dest, state.tableau[dest].cards.size() - 1)
    );
  }

  return std::shared_ptr<Move>(NULL);
}

static game_state_t perform_move(
    game_state_t state,
    std::shared_ptr<Move> move
)
{
  Location *from = move.get()->from.get();
  Location *to = move.get()->to.get();

  if (from->tag() == LOC_WASTE_PILE && to->tag() == LOC_TABLEAU) {
    return move_from_visible_pile_to_tableau(state, to->index());

  } else if (from->tag() == LOC_WASTE_PILE && to->tag() == LOC_FOUNDATION) {
    return move_from_visible_pile_to_foundation(state, to->index());

  } else if (from->tag() == LOC_TABLEAU && to->tag() == LOC_FOUNDATION) {
    return move_from_tableau_to_foundation(state, from->index(), to->index());

  } else if (from->tag() == LOC_TABLEAU && to->tag() == LOC_TABLEAU) {
    uint32_t src_deck = from->index();
    const tableau_position_t position = {
      .deck       = src_deck,
      .num_hidden = state.tableau[src_deck].num_down_cards,
      .position   = uint32_t(from->sub_index())
    };

    return move_from_column_to_column(state, position, to->index());
  }


  throw std::exception();
}

}

game_state_t strategy_init(const game_state_t & initial_state)
{
  /* Due to the way the game is scored, it is okay for us to shuffle through
   * the initial cards to learn what is in the deck.
   */
  game_state_t state = initial_state;

  glob_is_stock_pile_explored = true;

  for (int i = 0 ; i < 24 ; i++) {
    state = draw_from_stock_pile(state);
    glob_stock_pile.push_back(state.waste_pile_top.get());

    while (true) {
      std::shared_ptr<Move> move = calculate_obvious_move(state);

      if (move == NULL) {
        break;
      }

      state = perform_move(state, move);

      if (move.get()->from.get()->tag() == LOC_WASTE_PILE) {
        glob_stock_pile.pop_back();
      }
    }
  }

  /* Knowledge about state: */
  for (int i = 0 ; i < glob_stock_pile.size() ; i++) {
    std::cout << i << " = " << glob_stock_pile[i].to_string() << "\n";
  }

  /* This gets us to square one */
  return reset_stock_pile(state);
}

class FindException : public std::exception {};

static int find_stock_pile_position(const game_state_t & state)
{
  if (!state.waste_pile_top.is_some()) {
    return -1;
  }

  const card_t card = state.waste_pile_top.get();

  for (int i = 0 ; i < glob_stock_pile.size(); i++) {
    if (card == glob_stock_pile[i]) {
      return i;
    }
  }

  throw FindException();
}

static std::vector<std::pair<Move, card_t>> compute_foundation_path(
        const game_state_t & state,
        const uint32_t src,
        bool * exists)
{
    const tableau_deck_t tbl_deck = state.tableau[src];
    card_t deck_card = tbl_deck.cards[0];
    Option<card_t> foundation_card_opt = state.foundation[deck_card.suite];
    std::vector<std::pair<Move, card_t>> ret;
    uint32_t foundation_pos = deck_card.suite;

    /* At this point, If there is an aces, it should have been in the
     * foundation (from the initial sweep or subsequent moves).
     * Hence, if foundation_card is None, there is no point looking
     * further
     */
    if (!foundation_card_opt.is_some()) {
      *exists = false;
      return ret;
    }
    card_t foundation_card = foundation_card_opt.get();
    uint32_t left_in_deck[7];

    for (int i = 0 ; i < 7 ; i++) {
      left_in_deck[i] = state.tableau[i].cards.size();
    }

    for (int looking_for = foundation_card.number + 1 ;
        looking_for < deck_card.number ;
        looking_for++) {

      for (card_t node : glob_stock_pile) {
        if (node.number == looking_for && node.suite == deck_card.suite) {

          Move move = Move(
              loc_waste_pile(),
              loc_foundation(foundation_pos)
          );
          ret.push_back(std::make_pair(move, node));

          goto found;
        }
      }

      for (int i = 0 ; i < 7 ; i++) {
        if (i == src || left_in_deck[i] <= 0) {
          continue;
        }

        card_t node = state.tableau[i].cards[left_in_deck[i] - 1];

        if (node.number == looking_for && node.suite == deck_card.suite) {
          Move move = Move(
              loc_tableau(i, left_in_deck[i] - 1),
              loc_foundation(foundation_pos));

          ret.push_back(std::make_pair(move, node));
          left_in_deck[i]--;

          goto found;
        }
      }

      *exists = false;
      return ret;
found:
      continue;
    }

    *exists = true;
    return ret;
}

static std::vector<std::pair<Move, card_t>> compute_join_path(
    const game_state_t & state,
    uint32_t src_deck,
    uint32_t dest_deck,
    /* output */ bool *ptr_exists
)
{
  const card_t src = state.tableau[src_deck].cards.at(0);
  const Option<card_t> dest =
    (state.tableau[dest_deck].cards.size() == 0)
    ? Option<card_t>()
    : Option<card_t>(state.tableau[dest_deck].cards.back())
  ;
  std::vector<std::pair<Move, card_t>> ret;
  int left_in_deck[7];

  if (dest.is_some()) {
    card_t d = dest.get();

    bool p1 = (d.number <= src.number);
    bool p2 =
      ((d.number - src.number) % 2 == 0)
      && suite_color(d.suite) != suite_color(src.suite);
    bool p3 =
      ((d.number - src.number) % 2 == 1)
      && suite_color(d.suite) == suite_color(src.suite);

    if (p1 || p2 || p3) {
      std::cout << "This is an impossible move. Terminating" << std::endl;
      *ptr_exists = false;
      return ret;
    }
  }

  for (int i = 0 ; i < 7 ; i++) {
    left_in_deck[i] = state.tableau[i].cards.size();
  }

  const uint32_t limit = dest.is_some() ? dest.get().number - 1 : KING;

  for (card_t start = src; uint32_t(start.number) < limit; ) {

    std::cout << "=> Looking for continuation for " << start.to_string() << "\n";

    for (card_t node : glob_stock_pile) {
      if (node.number == start.number + 1
          && suite_color(node.suite) != suite_color(start.suite)) {

        Move move = Move(
            loc_waste_pile(),

            /* TODO(fyquah): Shouldn't really matter to have a random
             * subindex here. But it sure is inelegant.
             */
            loc_tableau(dest_deck, 0));
        ret.push_back(std::make_pair(move, node));

        std::cout << "---> Found in deck\n";

        goto found;
      }
    }

    for (int i = 0 ; i < 7 ; i++) {
      if (i == src_deck || i == dest_deck || left_in_deck[i] <= 0) {
        continue;
      }

      card_t node = state.tableau[i].cards[left_in_deck[i] - 1];

      if (node.number == start.number + 1
          && suite_color(node.suite) != suite_color(start.suite)) {
        Move move = Move(
            loc_tableau(i, left_in_deck[i] - 1),

            /* TODO(fyquah): Shouldn't really matter to have a random
             * subindex here. But it sure is inelegant.
             */
            loc_tableau(dest_deck, 0));

        ret.push_back(std::make_pair(move, node));
        left_in_deck[i]--;

        std::cout << "---> Found in tableau " << i << '\n';
        goto found;
      }
    }

    *ptr_exists = false;
    return ret;
found:
    start = ret.back().second;
  }

  *ptr_exists = true;
  std::reverse(ret.begin(), ret.end());
  return ret;
}

static game_state_t execute_path(
    game_state_t state,
    const std::vector<std::pair<Move, card_t>> & path,
    uint32_t src,
    std::shared_ptr<Location> dest
)
{
  for (int i = 0 ; i < path.size() ; i++) {
    Move move = path[i].first;
    card_t card = path[i].second;

    std::cout << "  Step " << i << ": " << move << " "  << card.to_string()
      << std::endl;

    if (move.from.get()->tag() == LOC_WASTE_PILE) {
      while (true) {

        if (!state.waste_pile_top.is_some()) {
          state = draw_from_stock_pile(state);

        } else if (state.waste_pile_top.get() == card) {
          break;

        } else if (state.stock_pile_size == 0) {
          state = reset_stock_pile(state);

        } else {
          state = draw_from_stock_pile(state);
        }
      }
    }

    update_glob_stock_pile(state, move);
    state = perform_move(state, std::make_shared<Move>(move));
  }

  std::shared_ptr<Move> final_move = make_move(
      loc_tableau(src, 0),
      dest
  );
  state = perform_move(state, final_move);
  std::cout << "Completed a successful cycle" << std::endl;
  return state;
}

static game_state_t enroute_to_obvious_by_peeking(
    const game_state_t & initial_state,
    bool *moved
)
{

  /* Rule 3a: Try to artifically move one deck to another using the help
   * of the wasted pile.
   */
  for (int src = 6; src >= 0 ; src--) {
    if (initial_state.tableau[src].num_down_cards == 0) {
      continue;
    }

    for (int dest = 0; dest < 7 ; dest++) {
      if (src == dest) {
        continue;
      }

      /* Try to find a path that would allow us to move the entire visible
       * stack at [src] to [dest].
       */
      std::cout << "Searching for " << src << " -> " << dest << "!"  << std::endl;
      bool exists;
      std::vector<std::pair<Move, card_t>> path = compute_join_path(
          initial_state, src, dest, &exists);

      if (exists) {
        std::cout << "Path exists! Executing path..." << std::endl;
        *moved = true;

        return execute_path(initial_state, path, src,
            loc_tableau(dest, initial_state.tableau[dest].cards.size() - 1));
      } else {
        std::cout << "Path not found!" << std::endl;
      }
    }
  }

  /* Rule 3b: Any pile that contains only visible cards are moved to
   * other piles. This should hopefully open some way more kings to move
   * into an potentially uncover some hidden cards.
   */
  for (int src = 6 ; src >= 0 ; src--) {
    const tableau_deck_t tbl_deck = initial_state.tableau[src];

    if (tbl_deck.num_down_cards != 0
        || tbl_deck.cards.size() == 0
        || tbl_deck.cards[0].number == KING /* Prevents throwing KING around */
        ) {
      continue;
    }

    for (int dest = 0 ; dest < 7 ; dest++) {
      if (src == dest) {
        continue;
      }

      bool exists;
      std::vector<std::pair<Move, card_t>> path = compute_join_path(
          initial_state, src, dest, &exists);

      if (exists) {
        *moved = true;
        return execute_path(initial_state, path, src,
            loc_tableau(dest, initial_state.tableau[dest].cards.size() - 1));
      }
    }
  }

  /* Rule 3c: Promote any cards to foundation if they can
   * immeadiately give us more new information. (this should cover
   * the case where we need to explicitly promote deuce).
   */
  std::cout << "Attempting lazy promotion!" << std::endl;
  for (int src = 0 ; src < 7 ; src++) {
    const tableau_deck_t tbl_deck = initial_state.tableau[src];


    /* We do not skip if [tbl_deck.num_down_cards == 0]. It might open up
     * space for King cards.
     */
    if (tbl_deck.cards.size() != 1) {
      continue;
    }

    card_t deck_card = tbl_deck.cards[0];
    Option<card_t> foundation_card = initial_state.foundation[deck_card.suite];
    bool exists;

    std::vector<std::pair<Move, card_t>> auxilary_path =
      compute_foundation_path(initial_state, src, &exists);

    if (exists) {
      *moved = true;
      return execute_path(initial_state, auxilary_path, src,
          loc_foundation(deck_card.suite));
    }
  }

  /* Rule XX: At the point of desperation. Just play anything that
   * increases the foundation piles' size. (At this point, we are probably
   * going to lose anyway ...)
   */

  std::cout << "Attempting lazy promotion!" << std::endl;
  for (int src = 0 ; src < 7 ; src++) {
    const tableau_deck_t tbl_deck = initial_state.tableau[src];


    /* We do not skip if [tbl_deck.num_down_cards == 0]. It might open up
     * space for King cards.
     */
    if (tbl_deck.cards.size() == 0) {
      continue;
    }

    card_t deck_card = tbl_deck.cards[0];
    Option<card_t> foundation_card = initial_state.foundation[deck_card.suite];
    bool exists;

    std::vector<std::pair<Move, card_t>> auxilary_path =
      compute_foundation_path(initial_state, src, &exists);

    if (exists) {
      *moved = true;
      return execute_path(initial_state, auxilary_path, src,
          loc_foundation(deck_card.suite));
    }
  }


  *moved = false;
  return initial_state;
}

game_state_t strategy_step(const game_state_t & start_state, bool *moved)
{
  std::cout << "Called step on " << std::endl;
  std::cout << start_state << std::endl;

  /* Rule 0 to 2 (the base rules) are in the obvious_move function. */
  game_state_t state = start_state;
  std::shared_ptr<Move> move = calculate_obvious_move(state);

  if (move != NULL) {
    *moved = true;
    update_glob_stock_pile(state, *move.get());
    return perform_move(state, move);
  }

  /* Rule 3: If there is no obvious way to do rule 0 to 2, let's cheat
   * by looking at the stock_pile to try to do rule rule 0 to 2.
   */
  state = enroute_to_obvious_by_peeking(state, moved);
  if (*moved) {
    return state;
  }

  *moved = false;
  return state;
}
