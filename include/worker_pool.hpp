#pragma once

#include "bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace habib {

enum class SubmitPolicy { Block, Reject };

enum class ShutdownMode {
  /// Reject new work; finish queued and in-flight tasks; block until idle.
  Drain,
  /// Reject new work; discard queued tasks; in-flight tasks still run cooperatively.
  Cancel,
};

struct WorkerPoolStats {
  std::size_t submitted{0};
  std::size_t completed{0};
  std::size_t failed{0};
  std::size_t rejected{0};
};

class WorkerPool {
 public:
  WorkerPool(std::size_t worker_count, std::size_t queue_capacity,
             SubmitPolicy submit_policy = SubmitPolicy::Block)
      : worker_count_(worker_count),
        submit_policy_(submit_policy),
        queue_(queue_capacity) {
    workers_.reserve(worker_count_);
    for (std::size_t i = 0; i < worker_count_; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~WorkerPool() { shutdown(ShutdownMode::Drain); }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  template <typename F, typename... Args>
  auto submit(F&& f, Args&&... args)
      -> std::future<std::invoke_result_t<F, Args...>>;

  /// Idempotent: concurrent calls are safe; only the first call runs shutdown logic.
  void shutdown(ShutdownMode mode) {
    std::call_once(shutdown_once_, [this, mode] { shutdown_impl(mode); });
  }

  [[nodiscard]] bool is_shutdown() const {
    return shutdown_initiated_.load(std::memory_order_acquire);
  }

  [[nodiscard]] WorkerPoolStats stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
  }

  [[nodiscard]] std::stop_token stop_token() const { return stop_source_.get_token(); }

 private:
  struct TaskItem {
    std::function<void()> run;
    std::function<void()> on_discard;
  };

  void worker_loop() {
    for (;;) {
      const auto item = queue_.pop();
      if (!item.has_value()) {
        if (queue_.is_closed()) {
          return;
        }
        continue;
      }
      ++in_flight_;
      item->run();
      --in_flight_;
      notify_idle_if_needed();
    }
  }

  bool enqueue_task(TaskItem item) {
    if (shutdown_initiated_.load(std::memory_order_acquire)) {
      record_rejected();
      return false;
    }
    if (submit_policy_ == SubmitPolicy::Reject) {
      if (!queue_.try_push(std::move(item))) {
        record_rejected();
        return false;
      }
    } else if (!queue_.push(std::move(item))) {
      record_rejected();
      return false;
    }
    {
      std::lock_guard lock(stats_mutex_);
      ++stats_.submitted;
    }
    return true;
  }

  void shutdown_impl(ShutdownMode mode) {
    shutdown_initiated_.store(true, std::memory_order_release);

    if (mode == ShutdownMode::Cancel) {
      discard_queued_tasks();
      queue_.close_discard();
      stop_source_.request_stop();
    } else {
      wait_idle();
      stop_source_.request_stop();
      queue_.close();
    }

    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void wait_idle() {
    std::unique_lock lock(idle_mutex_);
    const bool idle = idle_cv_.wait_for(lock, std::chrono::seconds(30), [this] {
      return queue_.size() == 0 && in_flight_.load(std::memory_order_acquire) == 0;
    });
    if (!idle) {
      throw std::runtime_error("shutdown timed out waiting for idle");
    }
  }

  void discard_queued_tasks() {
    for (;;) {
      const auto task = queue_.try_pop();
      if (!task.has_value()) {
        break;
      }
      if (task->on_discard) {
        task->on_discard();
      }
    }
  }

  void notify_idle_if_needed() {
    if (queue_.size() == 0 && in_flight_.load(std::memory_order_acquire) == 0) {
      std::lock_guard lock(idle_mutex_);
      idle_cv_.notify_all();
    }
  }

  void record_completed() {
    std::lock_guard lock(stats_mutex_);
    ++stats_.completed;
  }

  void record_failed() {
    std::lock_guard lock(stats_mutex_);
    ++stats_.failed;
  }

  void record_rejected() {
    std::lock_guard lock(stats_mutex_);
    ++stats_.rejected;
  }

  const std::size_t worker_count_;
  const SubmitPolicy submit_policy_;
  BoundedQueue<TaskItem> queue_;
  std::vector<std::thread> workers_;
  std::stop_source stop_source_;
  mutable std::mutex stats_mutex_;
  WorkerPoolStats stats_;
  std::once_flag shutdown_once_;
  std::atomic<bool> shutdown_initiated_{false};
  std::atomic<std::size_t> in_flight_{0};
  mutable std::mutex idle_mutex_;
  std::condition_variable idle_cv_;
};

template <typename F, typename... Args>
auto WorkerPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {
  using R = std::invoke_result_t<F, Args...>;

  auto promise = std::make_shared<std::promise<R>>();
  auto future = promise->get_future();

  TaskItem item;
  item.on_discard = [promise]() {
    promise->set_exception(
        std::make_exception_ptr(std::runtime_error("task discarded during cancel shutdown")));
  };
  item.run = [promise, func = std::decay_t<F>(std::forward<F>(f)),
              caps = std::make_tuple(std::decay_t<Args>(std::forward<Args>(args))...),
              this]() mutable {
    try {
      if constexpr (std::is_void_v<R>) {
        std::apply([&](auto&... ps) { std::invoke(func, ps...); }, caps);
        promise->set_value();
      } else {
        R result = std::apply([&](auto&... ps) { return std::invoke(func, ps...); }, caps);
        promise->set_value(std::move(result));
      }
      record_completed();
    } catch (...) {
      record_failed();
      promise->set_exception(std::current_exception());
    }
  };

  if (!enqueue_task(std::move(item))) {
    std::promise<R> rejected;
    rejected.set_exception(
        std::make_exception_ptr(std::runtime_error("task rejected or pool shut down")));
    return rejected.get_future();
  }

  return future;
}

}  // namespace habib
