/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file uint128_type.hpp Minimal 128 bit unsigned integer type, used as bit set storage where more than 64 bits are needed. */

#ifndef UINT128_TYPE_HPP
#define UINT128_TYPE_HPP

#include <compare>
#include <cstdint>
#include <limits>

/**
 * Unsigned 128 bit integer, big enough to be used as the storage of a bit set
 * which needs to address more than 64 values.
 *
 * Only the operations required by BaseBitSet/EnumBitSet are provided; this is
 * not a general purpose integer type.
 */
struct Uint128 {
	uint64_t lo = 0; ///< Least significant 64 bits.
	uint64_t hi = 0; ///< Most significant 64 bits.

	constexpr Uint128() = default;
	constexpr Uint128(uint64_t value) : lo(value) {} // NOLINT: implicit conversion is intended, so that literals can be used.
	constexpr Uint128(uint64_t lo, uint64_t hi) : lo(lo), hi(hi) {}

	constexpr bool operator==(const Uint128 &other) const { return this->lo == other.lo && this->hi == other.hi; }
	constexpr std::strong_ordering operator<=>(const Uint128 &other) const
	{
		if (this->hi != other.hi) return this->hi <=> other.hi;
		return this->lo <=> other.lo;
	}

	constexpr Uint128 operator~() const { return Uint128{~this->lo, ~this->hi}; }
	constexpr Uint128 operator|(const Uint128 &other) const { return Uint128{this->lo | other.lo, this->hi | other.hi}; }
	constexpr Uint128 operator&(const Uint128 &other) const { return Uint128{this->lo & other.lo, this->hi & other.hi}; }
	constexpr Uint128 operator^(const Uint128 &other) const { return Uint128{this->lo ^ other.lo, this->hi ^ other.hi}; }

	constexpr Uint128 &operator|=(const Uint128 &other) { this->lo |= other.lo; this->hi |= other.hi; return *this; }
	constexpr Uint128 &operator&=(const Uint128 &other) { this->lo &= other.lo; this->hi &= other.hi; return *this; }
	constexpr Uint128 &operator^=(const Uint128 &other) { this->lo ^= other.lo; this->hi ^= other.hi; return *this; }

	constexpr Uint128 operator-(const Uint128 &other) const
	{
		Uint128 result{this->lo - other.lo, this->hi - other.hi};
		if (this->lo < other.lo) result.hi--;
		return result;
	}

	constexpr Uint128 operator<<(uint shift) const
	{
		if (shift >= 128) return Uint128{};
		if (shift >= 64) return Uint128{0, this->lo << (shift - 64)};
		if (shift == 0) return *this;
		return Uint128{this->lo << shift, (this->hi << shift) | (this->lo >> (64 - shift))};
	}

	constexpr Uint128 operator>>(uint shift) const
	{
		if (shift >= 128) return Uint128{};
		if (shift >= 64) return Uint128{this->hi >> (shift - 64), 0};
		if (shift == 0) return *this;
		return Uint128{(this->lo >> shift) | (this->hi << (64 - shift)), this->hi >> shift};
	}
};

namespace std {

template <> struct numeric_limits<Uint128> {
	static constexpr bool is_specialized = true;
	static constexpr Uint128 max() noexcept { return Uint128{UINT64_MAX, UINT64_MAX}; }
	static constexpr int digits = 128;
};

} // namespace std

#endif /* UINT128_TYPE_HPP */
