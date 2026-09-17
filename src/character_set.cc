#include "ts_ref/character_set.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"

namespace ts_ref {
namespace {

// Renders one code point the way it would appear inside a character class.
std::string DescribeChar(std::uint32_t character) {
  switch (character) {
    case '\t':
      return "\\t";
    case '\n':
      return "\\n";
    case '\r':
      return "\\r";
    case ' ':
      return "' '";
    case '\\':
      return "\\\\";
    case ']':
      return "\\]";
    case '-':
      return "\\-";
    default:
      break;
  }
  if (character >= 0x21 && character <= 0x7E) {
    return std::string(1, static_cast<char>(character));
  }
  return absl::StrCat("\\u{", absl::Hex(character), "}");
}

}  // namespace

CharacterSet CharacterSet::FromChar(std::uint32_t character) {
  CharacterSet set;
  set.ranges_.push_back(Range{character, character + 1});
  return set;
}

CharacterSet CharacterSet::FromRange(std::uint32_t first, std::uint32_t last) {
  if (first > last) std::swap(first, last);
  CharacterSet set;
  set.ranges_.push_back(Range{first, last + 1});
  return set;
}

CharacterSet CharacterSet::Any() { return FromRange(0, kMaxCodePoint); }

bool CharacterSet::Contains(std::uint32_t character) const {
  // The ranges are sorted and disjoint, so the first range whose end exceeds
  // the character is the only one that could contain it.
  auto iter = std::upper_bound(ranges_.begin(), ranges_.end(), character,
                               [](std::uint32_t value, const Range& range) {
                                 return value < range.end;
                               });
  return iter != ranges_.end() && character >= iter->start;
}

CharacterSet& CharacterSet::AddChar(std::uint32_t character) {
  ranges_.push_back(Range{character, character + 1});
  Normalize();
  return *this;
}

CharacterSet& CharacterSet::AddRange(std::uint32_t first, std::uint32_t last) {
  if (first > last) std::swap(first, last);
  ranges_.push_back(Range{first, last + 1});
  Normalize();
  return *this;
}

CharacterSet& CharacterSet::Add(const CharacterSet& other) {
  ranges_.insert(ranges_.end(), other.ranges_.begin(), other.ranges_.end());
  Normalize();
  return *this;
}

void CharacterSet::Normalize() {
  std::sort(ranges_.begin(), ranges_.end());
  std::vector<Range> merged;
  merged.reserve(ranges_.size());
  for (const Range& range : ranges_) {
    if (range.start >= range.end) continue;
    // Merge with the previous range when they overlap *or* merely touch, so
    // that [a-m][n-z] collapses to one range rather than two.
    if (!merged.empty() && range.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  ranges_ = std::move(merged);
}

CharacterSet CharacterSet::Intersect(const CharacterSet& other) const {
  CharacterSet result;
  std::size_t left = 0;
  std::size_t right = 0;
  while (left < ranges_.size() && right < other.ranges_.size()) {
    const std::uint32_t start =
        std::max(ranges_[left].start, other.ranges_[right].start);
    const std::uint32_t end =
        std::min(ranges_[left].end, other.ranges_[right].end);
    if (start < end) result.ranges_.push_back(Range{start, end});
    // Advance whichever range ends first; the other may still overlap the next.
    if (ranges_[left].end < other.ranges_[right].end) {
      ++left;
    } else {
      ++right;
    }
  }
  return result;
}

CharacterSet CharacterSet::Difference(const CharacterSet& other) const {
  CharacterSet result;
  std::size_t right = 0;
  for (const Range& range : ranges_) {
    std::uint32_t cursor = range.start;
    // Skip subtrahend ranges that end before this one begins.
    while (right < other.ranges_.size() &&
           other.ranges_[right].end <= range.start) {
      ++right;
    }
    // Walk the subtrahend ranges that overlap, emitting the gaps between them.
    for (std::size_t i = right;
         i < other.ranges_.size() && other.ranges_[i].start < range.end; ++i) {
      if (other.ranges_[i].start > cursor) {
        result.ranges_.push_back(
            Range{cursor, std::min(other.ranges_[i].start, range.end)});
      }
      cursor = std::max(cursor, other.ranges_[i].end);
    }
    if (cursor < range.end) result.ranges_.push_back(Range{cursor, range.end});
  }
  return result;
}

CharacterSet CharacterSet::Complement() const {
  return CharacterSet::Any().Difference(*this);
}

CharacterSet CharacterSet::RemoveIntersection(CharacterSet& other) {
  CharacterSet intersection = Intersect(other);
  if (intersection.IsEmpty()) return intersection;
  *this = Difference(intersection);
  other = other.Difference(intersection);
  return intersection;
}

std::string CharacterSet::ToString() const {
  if (ranges_.empty()) return "[]";
  std::string out = "[";
  for (const Range& range : ranges_) {
    absl::StrAppend(&out, DescribeChar(range.start));
    if (range.end > range.start + 1) {
      absl::StrAppend(&out, "-", DescribeChar(range.end - 1));
    }
  }
  out += "]";
  return out;
}

}  // namespace ts_ref
