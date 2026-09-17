#ifndef TS_REF_TYPES_H_
#define TS_REF_TYPES_H_
#include <cstdint>

namespace ts_ref {

struct Point {
  Point() = default;
  Point(std::uint32_t r, std::uint32_t c) : row{r}, column{c} {}

  bool operator==(const Point& other) const = default;

  // Handles logic of row/column addition
  Point Add(Point new_point) const {
    if (new_point.row > 0) {
      return Point(row + new_point.row, new_point.column);
    } else {
      return Point(row, column + new_point.column);
    }
  }

  std::uint32_t row = 0;
  std::uint32_t column = 0;
};

struct Length {
  Length() = default;
  Length(std::uint32_t byte_len, std::uint32_t row, std::uint32_t column)
      : bytes{byte_len}, coords(row, column) {}
  Length(std::uint32_t byte_len, Point new_point)
      : bytes{byte_len}, coords{new_point} {}

  bool operator==(const Length& other) const = default;

  Length Add(Length new_length) const {
    return Length(bytes + new_length.bytes, coords.Add(new_length.coords));
  }

  std::uint32_t bytes = 0;
  Point coords;
};

using Symbol = std::uint16_t;
using StateId = std::uint32_t;
using StackVersion = std::uint32_t;
using DynamicPrecedenceType = std::int32_t;

// range represented by half-open interval [x, y)
struct ByteRange {
  std::uint32_t start;
  std::uint32_t end;

  std::uint32_t Length() const { return end - start; }
  bool Empty() const { return start == end; }
};

// Reserved Symbol value for end-of-input marker for lookahead:
inline constexpr Symbol kEndOfInput = 0;

// Not needed if we don't include error recovery:
// inline constexpr Symbol kError = 1;
}  // namespace ts_ref
#endif
