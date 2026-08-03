#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

#if __cplusplus < 201703L

namespace std {

typedef unsigned char byte;

struct nullopt_t {
  explicit constexpr nullopt_t(int) {}
};

constexpr nullopt_t nullopt{0};

template <typename T>
class optional {
public:
  optional() : engaged_(false) {}
  optional(nullopt_t) : engaged_(false) {}
  optional(const T& value) : engaged_(true) { new (&storage_) T(value); }
  optional(T&& value) : engaged_(true) { new (&storage_) T(std::move(value)); }

  optional(const optional& other) : engaged_(other.engaged_) {
    if (engaged_) new (&storage_) T(*other);
  }

  optional(optional&& other) : engaged_(other.engaged_) {
    if (engaged_) new (&storage_) T(std::move(*other));
  }

  ~optional() { reset(); }

  optional& operator=(nullopt_t) {
    reset();
    return *this;
  }

  optional& operator=(optional&& other) {
    if (this != &other) {
      reset();
      if (other.engaged_) {
        new (&storage_) T(std::move(*other));
        engaged_ = true;
      }
    }
    return *this;
  }

  bool has_value() const { return engaged_; }
  explicit operator bool() const { return engaged_; }

  T& operator*() { return *ptr(); }
  const T& operator*() const { return *ptr(); }
  T* operator->() { return ptr(); }
  const T* operator->() const { return ptr(); }

private:
  typedef typename aligned_storage<sizeof(T), alignof(T)>::type Storage;

  T* ptr() { return reinterpret_cast<T*>(&storage_); }
  const T* ptr() const { return reinterpret_cast<const T*>(&storage_); }

  void reset() {
    if (engaged_) {
      ptr()->~T();
      engaged_ = false;
    }
  }

  bool engaged_;
  Storage storage_;
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

  iterator begin() const { return data_; }
  iterator end() const { return data_ + size_; }
  reverse_iterator rbegin() const { return reverse_iterator(end()); }
  reverse_iterator rend() const { return reverse_iterator(begin()); }

  T& operator[](size_t index) const { return data_[index]; }

private:
  T* data_;
  size_t size_;
};

template <typename T>
constexpr int bit_width(T value) {
  int width = 0;
  while (value != 0) {
    ++width;
    value >>= 1;
  }
  return width;
}

template <typename T>
constexpr T bit_ceil(T value) {
  if (value <= 1) return 1;
  --value;
  for (size_t shift = 1; shift < sizeof(T) * 8; shift <<= 1) {
    value |= static_cast<T>(value >> shift);
  }
  return static_cast<T>(value + 1);
}

template <typename InputIt, typename T, typename BinaryOp, typename UnaryOp>
T transform_reduce(InputIt first, InputIt last, T init, BinaryOp binaryOp, UnaryOp unaryOp) {
  for (; first != last; ++first) {
    init = binaryOp(init, unaryOp(*first));
  }
  return init;
}

namespace numbers {
constexpr double ln2 = 0.69314718055994530942;
}  // namespace numbers

}  // namespace std

namespace gemini_bloom {

class string_view {
public:
  string_view() : data_(nullptr), size_(0) {}
  string_view(const char* data, size_t size) : data_(data), size_(size) {}
  string_view(const char* data) : data_(data), size_(std::char_traits<char>::length(data)) {}

  const char* data() const { return data_; }
  size_t size() const { return size_; }

private:
  const char* data_;
  size_t size_;
};

}  // namespace gemini_bloom

#else
#include <bit>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>

namespace gemini_bloom {
using std::string_view;
}  // namespace gemini_bloom
#endif
