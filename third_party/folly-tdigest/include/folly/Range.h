#pragma once
// Minimal compatibility shim for folly/Range.h — a non-owning [begin, end)
// view, providing only the surface the vendored folly TDigest sources use.
// Authored for BitLSM; NOT Folly code. See ../../NOTICE.
#include <cstddef>
#include <type_traits>
#include <utility>

#include <folly/Portability.h>

namespace folly {

template <class Iter>
class Range {
 public:
  using iterator = Iter;
  using const_iterator = Iter;
  using size_type = std::size_t;
  using reference = decltype(*std::declval<Iter>());
  using value_type =
      typename std::remove_cv<typename std::remove_reference<reference>::type>::type;

  constexpr Range() = default;
  constexpr Range(Iter begin, Iter end) : b_(begin), e_(end) {}
  constexpr Range(Iter begin, size_type size) : b_(begin), e_(begin + size) {}

  // Construct from a contiguous container (e.g. std::vector): used by the
  // multi-digest merge path (cursors.emplace_back(centroids_)).
  template <
      class Container,
      class = decltype(static_cast<Iter>(std::declval<Container&>().data()))>
  /* implicit */ Range(Container&& c)
      : b_(c.data()), e_(c.data() + c.size()) {}

  constexpr Iter begin() const { return b_; }
  constexpr Iter end() const { return e_; }
  constexpr Iter data() const { return b_; }
  constexpr size_type size() const { return static_cast<size_type>(e_ - b_); }
  constexpr bool empty() const { return b_ == e_; }

  reference front() const { return *b_; }
  reference back() const { return *(e_ - 1); }
  reference operator[](size_type i) const { return b_[i]; }

  void pop_front() { ++b_; }
  void pop_back() { --e_; }

 private:
  Iter b_ = Iter();
  Iter e_ = Iter();
};

}  // namespace folly
