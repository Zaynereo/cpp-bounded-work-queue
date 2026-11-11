#pragma once

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

inline void expect_within(std::chrono::milliseconds budget, auto&& action) {
  std::promise<void> done;
  auto synced = done.get_future();
  std::thread worker([&] {
    try {
      std::forward<decltype(action)>(action)();
      done.set_value();
    } catch (...) {
      done.set_exception(std::current_exception());
    }
  });
  if (synced.wait_for(budget) != std::future_status::ready) {
    worker.detach();
    throw std::runtime_error("test timed out");
  }
  worker.join();
  synced.get();
}
