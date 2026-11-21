// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace ClipperIntegerRounding {
// Preserve Clipper's floor(value + 0.5) rule, including its explicit exception
// immediately below +0.5. The caller must ensure that the input is finite and
// the rounded result fits Integer, just as for the original floating cast.
// Truncation removes the fractional part; only a negative fraction needs the
// one-unit floor correction. No floating-point rounding mode is changed.
template<class Integer>
inline Integer round_up(double value)
{
    if (value == 0.49999999999999994)
        return 0;
    const double shifted = value + 0.5;
    const Integer truncated = static_cast<Integer>(shifted);
    return truncated - Integer(static_cast<double>(truncated) > shifted);
}
} // namespace ClipperIntegerRounding
