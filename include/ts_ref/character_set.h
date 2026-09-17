#ifndef TS_REF_CHARACTER_SET_H_
#define TS_REF_CHARACTER_SET_H_

// A set of Unicode code points, stored as sorted disjoint ranges.
//
// This is the alphabet the lexer works over. A set rather than individual
// characters because the generated DFA has to answer "which transition does
// this character take?" without a 0x110000-entry table per state, and because
// subset construction needs to split overlapping transitions into disjoint
// pieces (see NfaCursor::Transitions).
//
// Corresponds to tree-sitter's CharacterSet in crates/generate/src/nfa.rs.

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace ts_ref {

// One past the largest Unicode code point. Used as the exclusive end of a
// range covering everything.
inline constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
inline constexpr std::uint32_t kCodePointEnd = kMaxCodePoint + 1;

class CharacterSet {
 public:
  // A half-open interval [start, end).
  struct Range {
    std::uint32_t start = 0;
    std::uint32_t end = 0;

    bool operator==(const Range&) const = default;
    auto operator<=>(const Range&) const = default;
  };

  CharacterSet() = default;

  static CharacterSet FromChar(std::uint32_t character);
  // inclusive on both ends, matching how character classes are written
  static CharacterSet FromRange(std::uint32_t first, std::uint32_t last);
  // every code point
  static CharacterSet Any();

  bool IsEmpty() const { return ranges_.empty(); }
  bool Contains(std::uint32_t character) const;
  std::size_t RangeCount() const { return ranges_.size(); }
  const std::vector<Range>& ranges() const { return ranges_; }

  // Smallest code point in the set; only valid when non-empty.
  std::uint32_t Min() const { return ranges_.front().start; }

  CharacterSet& AddChar(std::uint32_t character);
  CharacterSet& AddRange(std::uint32_t first, std::uint32_t last);
  CharacterSet& Add(const CharacterSet& other);

  CharacterSet Intersect(const CharacterSet& other) const;
  CharacterSet Difference(const CharacterSet& other) const;
  // Everything not in this set.
  CharacterSet Complement() const;

  // Removes the intersection from *both* sets and returns it, leaving three
  // disjoint pieces: the returned overlap, what remains here, and what remains
  // in `other`. This is the primitive that splits overlapping NFA transitions
  // into the disjoint transitions a DFA state needs.
  CharacterSet RemoveIntersection(CharacterSet& other);

  bool operator==(const CharacterSet&) const = default;
  auto operator<=>(const CharacterSet&) const = default;

  // Human-readable form for diagnostics, e.g. "[a-z0-9_]".
  std::string ToString() const;

 private:
  // Restores the invariant: sorted, non-overlapping, non-adjacent, no empties.
  void Normalize();

  std::vector<Range> ranges_;
};

}  // namespace ts_ref

#endif
