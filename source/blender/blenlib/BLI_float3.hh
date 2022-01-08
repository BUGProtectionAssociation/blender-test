/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

#include <iostream>

#include "BLI_float2.hh"
#include "BLI_math_base_safe.h"
#include "BLI_math_vector.hh"

namespace blender {

struct float3 {
  float x, y, z;

  float3() = default;

  float3(const float *ptr) : x{ptr[0]}, y{ptr[1]}, z{ptr[2]}
  {
  }

  float3(const float (*ptr)[3]) : float3(static_cast<const float *>(ptr[0]))
  {
  }

  explicit float3(float value) : x(value), y(value), z(value)
  {
  }

  explicit float3(int value) : x(value), y(value), z(value)
  {
  }

  float3(float x, float y, float z) : x{x}, y{y}, z{z}
  {
  }

  float3(float2 xy, float z) : x(xy.x), y(xy.y), z(z)
  {
  }

  float3(float x, float2 yz) : x(x), y(yz.x), z(yz.y)
  {
  }

  /** Conversions. */

  explicit operator float2() const
  {
    return float2(x, y);
  }

  operator const float *() const
  {
    return &x;
  }

  operator float *()
  {
    return &x;
  }

  /** Array access. */

  const float &operator[](int64_t index) const
  {
    BLI_assert(index < 3);
    return (&x)[index];
  }

  float &operator[](int64_t index)
  {
    BLI_assert(index < 3);
    return (&x)[index];
  }

  /** Arithmetic. */

  friend float3 operator+(const float3 &a, const float3 &b)
  {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }

  friend float3 operator+(const float3 &a, const float &b)
  {
    return {a.x + b, a.y + b, a.z + b};
  }

  friend float3 operator+(const float &a, const float3 &b)
  {
    return b + a;
  }

  float3 &operator+=(const float3 &b)
  {
    x += b.x;
    y += b.y;
    z += b.z;
    return *this;
  }

  float3 &operator+=(const float &b)
  {
    x += b;
    y += b;
    z += b;
    return *this;
  }

  friend float3 operator-(const float3 &a)
  {
    return {-a.x, -a.y, -a.z};
  }

  friend float3 operator-(const float3 &a, const float3 &b)
  {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
  }

  friend float3 operator-(const float3 &a, const float &b)
  {
    return {a.x - b, a.y - b, a.z - b};
  }

  friend float3 operator-(const float &a, const float3 &b)
  {
    return {a - b.x, a - b.y, a - b.z};
  }

  float3 &operator-=(const float3 &b)
  {
    x -= b.x;
    y -= b.y;
    z -= b.z;
    return *this;
  }

  float3 &operator-=(const float &b)
  {
    x -= b;
    y -= b;
    z -= b;
    return *this;
  }

  friend float3 operator*(const float3 &a, const float3 &b)
  {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
  }

  friend float3 operator*(const float3 &a, float b)
  {
    return {a.x * b, a.y * b, a.z * b};
  }

  friend float3 operator*(float a, const float3 &b)
  {
    return b * a;
  }

  float3 &operator*=(float b)
  {
    x *= b;
    y *= b;
    z *= b;
    return *this;
  }

  float3 &operator*=(const float3 &b)
  {
    x *= b.x;
    y *= b.y;
    z *= b.z;
    return *this;
  }

  friend float3 operator/(const float3 &a, const float3 &b)
  {
    BLI_assert(b.x != 0.0f && b.y != 0.0f && b.z != 0.0f);
    return {a.x / b.x, a.y / b.y, a.z / b.z};
  }

  friend float3 operator/(const float3 &a, float b)
  {
    BLI_assert(b != 0.0f);
    return {a.x / b, a.y / b, a.z / b};
  }

  friend float3 operator/(float a, const float3 &b)
  {
    BLI_assert(b.x != 0.0f && b.y != 0.0f && b.z != 0.0f);
    return {a / b.x, a / b.y, a / b.z};
  }

  float3 &operator/=(float b)
  {
    BLI_assert(b != 0.0f);
    x /= b;
    y /= b;
    z /= b;
    return *this;
  }

  float3 &operator/=(const float3 &b)
  {
    BLI_assert(b.x != 0.0f && b.y != 0.0f && b.z != 0.0f);
    x /= b.x;
    y /= b.y;
    z /= b.z;
    return *this;
  }

  /** Compare. */

  friend bool operator==(const float3 &a, const float3 &b)
  {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  }

  friend bool operator!=(const float3 &a, const float3 &b)
  {
    return !(a == b);
  }

  /** Print. */

  friend std::ostream &operator<<(std::ostream &stream, const float3 &v)
  {
    stream << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return stream;
  }

  float normalize_and_get_length()
  {
    return normalize_v3(*this);
  }

  static float3 normalize_and_get_length(const float3 &vec, float &out_len)
  {
    float3 result;
    out_len = normalize_v3_v3(result, vec);
    return result;
  }

  /**
   * Normalizes the vector in place.
   */
  void normalize()
  {
    normalize_v3(*this);
  }

  /**
   * Returns a normalized vector. The original vector is not changed.
   */
  float3 normalized() const
  {
    float3 result;
    normalize_v3_v3(result, *this);
    return result;
  }

  static float3 normalize(const float3 &vec)
  {
    float3 result;
    normalize_v3_v3(result, vec);
    return result;
  }

  float length() const
  {
    return len_v3(*this);
  }

  static float length(const float3 &vec)
  {
    return len_v3(vec);
  }

  float length_squared() const
  {
    return len_squared_v3(*this);
  }

  static float length_squared(const float3 &vec)
  {
    return len_squared_v3(vec);
  }

  bool is_zero() const
  {
    return this->x == 0.0f && this->y == 0.0f && this->z == 0.0f;
  }

  static float3 reflect(const float3 &incident, const float3 &normal)
  {
    float3 result;
    reflect_v3_v3v3(result, incident, normal);
    return result;
  }

  static float3 refract(const float3 &incident, const float3 &normal, const float eta)
  {
    float3 result;
    float k = 1.0f - eta * eta * (1.0f - dot(normal, incident) * dot(normal, incident));
    if (k < 0.0f) {
      result = float3(0.0f);
    }
    else {
      result = eta * incident - (eta * dot(normal, incident) + sqrt(k)) * normal;
    }
    return result;
  }

  static float3 faceforward(const float3 &vector, const float3 &incident, const float3 &reference)
  {
    return dot(reference, incident) < 0.0f ? vector : -vector;
  }

  static float3 safe_divide(const float3 &a, const float3 &b)
  {
    float3 result;
    result.x = (b.x == 0.0f) ? 0.0f : a.x / b.x;
    result.y = (b.y == 0.0f) ? 0.0f : a.y / b.y;
    result.z = (b.z == 0.0f) ? 0.0f : a.z / b.z;
    return result;
  }

  static float3 min(const float3 &a, const float3 &b)
  {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
  }

  static float3 max(const float3 &a, const float3 &b)
  {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
  }

  static void min_max(const float3 &vector, float3 &min, float3 &max)
  {
    min = float3::min(vector, min);
    max = float3::max(vector, max);
  }

  static float3 safe_divide(const float3 &a, const float b)
  {
    return (b != 0.0f) ? a / b : float3(0.0f);
  }

  static float3 floor(const float3 &a)
  {
    return float3(floorf(a.x), floorf(a.y), floorf(a.z));
  }

  static float3 ceil(const float3 &a)
  {
    return float3(ceilf(a.x), ceilf(a.y), ceilf(a.z));
  }

  uint64_t hash() const
  {
    uint64_t x1 = *reinterpret_cast<const uint32_t *>(&x);
    uint64_t x2 = *reinterpret_cast<const uint32_t *>(&y);
    uint64_t x3 = *reinterpret_cast<const uint32_t *>(&z);
    return (x1 * 435109) ^ (x2 * 380867) ^ (x3 * 1059217);
  }

  static uint64_t hash(const float3 &vec)
  {
    uint64_t x1 = *reinterpret_cast<const uint32_t *>(&vec.x);
    uint64_t x2 = *reinterpret_cast<const uint32_t *>(&vec.y);
    uint64_t x3 = *reinterpret_cast<const uint32_t *>(&vec.z);
    return (x1 * 435109) ^ (x2 * 380867) ^ (x3 * 1059217);
  }

  static float dot(const float3 &a, const float3 &b)
  {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  }

  static float3 cross_high_precision(const float3 &a, const float3 &b)
  {
    float3 result;
    cross_v3_v3v3_hi_prec(result, a, b);
    return result;
  }

  static float3 cross(const float3 &a, const float3 &b)
  {
    float3 result;
    cross_v3_v3v3(result, a, b);
    return result;
  }

  static float3 project(const float3 &a, const float3 &b)
  {
    float3 result;
    project_v3_v3v3(result, a, b);
    return result;
  }

  static float distance(const float3 &a, const float3 &b)
  {
    return (a - b).length();
  }

  static float distance_squared(const float3 &a, const float3 &b)
  {
    float3 diff = a - b;
    return float3::dot(diff, diff);
  }

  static float3 interpolate(const float3 &a, const float3 &b, float t)
  {
    return a * (1 - t) + b * t;
  }

  static float3 abs(const float3 &a)
  {
    return float3(fabsf(a.x), fabsf(a.y), fabsf(a.z));
  }

  static float3 mod(const float3 &a, const float3 &b)
  {
    return float3(safe_modf(a.x, b.x), safe_modf(a.y, b.y), safe_modf(a.z, b.z));
  }

  static float3 fract(const float3 &a)
  {
    return a - float3(floorf(a.x), floorf(a.y), floorf(a.z));
  }
};

}  // namespace blender

/** Declare functions outside of namespace to avoid them taking predecence over
 *  the standard functions. */

#define float3 blender::float3

inline float3 abs(const float3 &a)
{
  return blender::abs_impl<float3, 3>(a);
}

inline float3 floor(const float3 &a)
{
  return blender::floor_impl<float3, 3>(a);
}

inline float dot(const float3 &a, const float3 &b)
{
  return blender::dot_impl<float3, 3>(a, b);
}

inline float3 reflect(const float3 &incident, const float3 &normal)
{
  return blender::reflect_impl<float3, 3>(incident, normal);
}

inline float3 refract(const float3 &incident, const float3 &normal, const float eta)
{
  return blender::refract_impl<float3, 3>(incident, normal, eta);
}

#undef float3
