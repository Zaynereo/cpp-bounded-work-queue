#include "bounded_queue.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <latch>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

void test_capacity_zero_rejects() {
  habib::BoundedQueue<int> q(0);
  assert(!q.try_push(1));
  assert(!q.push(1));
  assert(q.try_pop() == std::nullopt);
}

void test_try_push_backpressure() {
  habib::BoundedQueue<int> q(2);
  assert(q.try_push(1));
  assert(q.try_push(2));
  assert(!q.try_push(3));
  assert(q.capacity() == 2);
  assert(q.size() == 2);
}

void test_blocking_push_and_wakeup() {
  habib::BoundedQueue<int> q(1);
  assert(q.try_push(1));

  std::latch producer_ready{1};
  std::atomic<bool> pushed{false};

  std::thread producer([&] {
    producer_ready.count_down();
    assert(q.push(2));
    pushed.store(true);
  });

  producer_ready.wait();
  assert(q.try_pop().value() == 1);
  expect_within(std::chrono::seconds(2), [&] {
    while (!pushed.load()) {
      std::this_thread::yield();
    }
  });
  producer.join();
  assert(q.try_pop().value() == 2);
}

void test_close_wakes_blocked_pop() {
  habib::BoundedQueue<int> q(1);
  std::atomic<bool> woke{false};

  std::thread consumer([&] {
    const auto v = q.pop();
    assert(!v.has_value());
    woke.store(true);
  });

  expect_within(std::chrono::seconds(2), [&] {
    while (q.size() != 0) {
      std::this_thread::yield();
    }
    q.close();
    while (!woke.load()) {
      std::this_thread::yield();
    }
  });
  consumer.join();
}

void test_close_wakes_blocked_push() {
  habib::BoundedQueue<int> q(1);
  assert(q.try_push(1));

  std::atomic<bool> returned{false};
  std::thread producer([&] {
    const bool ok = q.push(2);
    assert(!ok);
    returned.store(true);
  });

  expect_within(std::chrono::seconds(2), [&] {
    q.close();
    while (!returned.load()) {
      std::this_thread::yield();
    }
  });
  producer.join();
}

void test_mpmc_exactly_once() {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 250;
  constexpr int kTotal = kProducers * kPerProducer;

  habib::BoundedQueue<int> q(16);
  std::mutex seen_mutex;
  std::set<int> seen;
  std::vector<std::thread> producers;
  std::vector<std::thread> consumers;

  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        const int value = p * kPerProducer + i;
        expect_within(std::chrono::seconds(5), [&] { assert(q.push(value)); });
      }
    });
  }

  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      for (;;) {
        const auto v = q.pop();
        if (!v.has_value()) {
          break;
        }
        std::lock_guard lock(seen_mutex);
        assert(seen.insert(v.value()).second);
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }
  q.close();
  for (auto& t : consumers) {
    t.join();
  }

  assert(static_cast<int>(seen.size()) == kTotal);
  for (int i = 0; i < kTotal; ++i) {
    assert(seen.count(i) == 1);
  }
}

}  // namespace

int main() {
  test_capacity_zero_rejects();
  test_try_push_backpressure();
  test_blocking_push_and_wakeup();
  test_close_wakes_blocked_pop();
  test_close_wakes_blocked_push();
  test_mpmc_exactly_once();
  return 0;
}
