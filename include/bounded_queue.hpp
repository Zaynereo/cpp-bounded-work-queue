#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace habib {

/// Blocking bounded multi-producer multi-consumer queue (mutex + condition variables).
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  [[nodiscard]] std::size_t capacity() const {
    std::lock_guard lock(mutex_);
    return capacity_;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return items_.size();
  }

  [[nodiscard]] bool is_closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  /// Reject immediately when full, closed, or capacity is zero.
  [[nodiscard]] bool try_push(T value) {
    std::lock_guard lock(mutex_);
    if (closed_ || capacity_ == 0 || items_.size() >= capacity_) {
      return false;
    }
    items_.push_back(std::move(value));
    cv_not_empty_.notify_one();
    return true;
  }

  /// Block until space, close, or capacity zero (returns false without blocking when capacity is zero).
  [[nodiscard]] bool push(T value) {
    std::unique_lock lock(mutex_);
    if (capacity_ == 0) {
      return false;
    }
    cv_not_full_.wait(lock, [&] {
      return closed_ || items_.size() < capacity_;
    });
    if (closed_) {
      return false;
    }
    items_.push_back(std::move(value));
    cv_not_empty_.notify_one();
    return true;
  }

  /// Non-blocking pop;nullopt when empty (even if closed).
  [[nodiscard]] std::optional<T> try_pop() {
    std::lock_guard lock(mutex_);
    if (items_.empty()) {
      return std::nullopt;
    }
    T value = std::move(items_.front());
    items_.pop_front();
    cv_not_full_.notify_one();
    return value;
  }

  /// Block until an item is available or the queue is closed and drained.
  [[nodiscard]] std::optional<T> pop() {
    std::unique_lock lock(mutex_);
    cv_not_empty_.wait(lock, [&] { return closed_ || !items_.empty(); });
    if (items_.empty()) {
      return std::nullopt;
    }
    T value = std::move(items_.front());
    items_.pop_front();
    cv_not_full_.notify_one();
    return value;
  }

  /// Wake blocked producers and consumers; further pushes are rejected.
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
  }

  /// Close and drop pending items without delivering them to consumers.
  void close_discard() {
    std::lock_guard lock(mutex_);
    closed_ = true;
    items_.clear();
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
  }

 private:
  const std::size_t capacity_;
  std::deque<T> items_;
  bool closed_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
};

}  // namespace habib
