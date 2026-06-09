#pragma once
// Minimal compatibility shim for folly/Overload.h — builds an overloaded
// callable from several lambdas (used to visit Centroid-vs-double in merge).
// Authored for BitLSM; NOT Folly code. See ../../NOTICE.
#include <utility>

namespace folly {
namespace overload_detail {
template <class... Fs>
struct OverloadSet : Fs... {
  using Fs::operator()...;
};
template <class... Fs>
OverloadSet(Fs...) -> OverloadSet<Fs...>;
}  // namespace overload_detail

template <class... Fs>
constexpr auto overload(Fs&&... fs) {
  return overload_detail::OverloadSet<std::decay_t<Fs>...>{
      std::forward<Fs>(fs)...};
}
}  // namespace folly
