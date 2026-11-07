// The plumbing crd-webrtc-fec-peer shims: the two Abseil container algorithms its files call.
#pragma once

#include <algorithm>
#include <iterator>
#include <utility>

namespace absl
{
template <class Range, class Output>
Output c_copy(const Range& range, Output output)
{
    return std::copy(std::begin(range), std::end(range), output);
}

template <class Range, class Value, class Less>
auto c_lower_bound(Range& range, Value&& value, Less&& less)
{
    return std::lower_bound(std::begin(range), std::end(range), std::forward<Value>(value), std::forward<Less>(less));
}
} // namespace absl
