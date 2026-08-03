#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#if __cplusplus >= 201703L
#include <optional>
#include <string_view>
#endif

#if __cplusplus >= 202002L
#include <bit>
#include <numbers>
#include <span>
#endif

#if __cplusplus < 201703L
namespace std {

enum class byte : unsigned char {};

struct nullopt_t {
  explicit constexpr nullopt_t(int) {}
};

static constexpr nullopt_t nullopt{0};

template <typename T>
class optional {
public:
  optional() : has_(false) {}
  optional(nullopt_t) : has_(false) {}
  optional(const T& value) : has_(true) { new (&storage_) T(value); }
  optional(T&& value) : has_(true) { new (&storage_) T(std::move(value)); }

  optional(const optional& other) : has_(false) {
    if (other.has_) {
      new (&storage_) T(*other);
      has_ = true;
    }
  }

  optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
      : has_(false) {
    if (other.has_) {
      new (&storage_) T(std::move(*other));
      has_ = true;
    }
  }

  ~optional() { reset(); }

  optional& operator=(nullopt_t) {
    reset();
    return *this;
  }

  optional& operator=(optional&& other) noexcept(
      std::is_nothrow_move_constructible<T>::value &&
      std::is_nothrow_move_assignable<T>::value) {
    if (this == &other) return *this;
    if (has_ && other.has_) {
      **this = std::move(*other);
    } else if (other.has_) {
      new (&storage_) T(std::move(*other));
      has_ = true;
    } else {
      reset();
    }
    return *this;
  }

  bool has_value() const { return has_; }
  explicit operator bool() const { return has_; }

  T& operator*() { return *ptr(); }
  const T& operator*() const { return *ptr(); }
  T* operator->() { return ptr(); }
  const T* operator->() const { return ptr(); }

private:
  void reset() {
    if (has_) {
      ptr()->~T();
      has_ = false;
    }
  }

  T* ptr() { return reinterpret_cast<T*>(&storage_); }
  const T* ptr() const { return reinterpret_cast<const T*>(&storage_); }

  bool has_;
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
};

template <typename T>
class span {
public:
  typedef T element_type;
  typedef T* iterator;
  typedef const T* const_iterator;
  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

  span() : data_(nullptr), size_(0) {}
  span(T* data, size_t size) : data_(data), size_(size) {}

  T* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  iterator begin() const { return data_; }
  iterator end() const { return data_ + size_; }
  reverse_iterator rbegin() const { return reverse_iterator(end()); }
  reverse_iterator rend() const { return reverse_iterator(begin()); }

  T& operator[](size_t index) const { return data_[index]; }

private:
  T* data_;
  size_t size_;
};

}  // namespace std
#endif

namespace bloom_compat {
#if __cplusplus >= 201703L
using StringView = std::string_view;
#else
class StringView {
public:
  StringView() : data_(nullptr), size_(0) {}
  StringView(const char* str) : data_(str), size_(str ? std::strlen(str) : 0) {}
  StringView(const char* str, size_t len) : data_(str), size_(len) {}

  const char* data() const { return data_; }
  size_t size() const { return size_; }

private:
  const char* data_;
  size_t size_;
};
#endif
}  // namespace bloom_compat

#if __cplusplus < 202002L
namespace std {
namespace numbers {
static constexpr double ln2 = 0.693147180559945309417232121458176568;
}  // namespace numbers

template <typename T>
typename std::enable_if<std::is_unsigned<T>::value, int>::type bit_width(T value) {
  int width = 0;
  while (value != 0) {
    ++width;
    value >>= 1;
  }
  return width;
}

template <typename T>
typename std::enable_if<std::is_unsigned<T>::value, T>::type bit_ceil(T value) {
  if (value <= 1) return 1;
  --value;
  for (size_t shift = 1; shift < std::numeric_limits<T>::digits; shift <<= 1) {
    value |= static_cast<T>(value >> shift);
  }
  return static_cast<T>(value + 1);
}

}  // namespace std
#endif
