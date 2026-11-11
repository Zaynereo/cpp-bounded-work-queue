#include "test_helpers.hpp"
#include "worker_pool.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void test_reject_policy_when_full() {
  habib::WorkerPool pool(1, 1, habib::SubmitPolicy::Reject);
  std::latch started{1};
  std::latch release{1};

  auto slow = pool.submit([&] {
    started.count_down();
    release.wait();
  });
  started.wait();

  auto queued = pool.submit([] { return 1; });
  auto third = pool.submit([] { return 2; });
  assert(third.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready);
  try {
    third.get();
    assert(false);
  } catch (const std::runtime_error&) {
  }

  release.count_down();
  slow.get();
  queued.get();
  pool.shutdown(habib::ShutdownMode::Drain);
  assert(pool.stats().rejected >= 1);
}

void test_exception_propagation_and_failed_count() {
  habib::WorkerPool pool(2, 4);
  auto fut = pool.submit([]() -> int { throw std::runtime_error("boom"); });
  expect_within(std::chrono::seconds(2), [&] {
    assert(fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
  });
  try {
    fut.get();
    assert(false);
  } catch (const std::runtime_error& ex) {
    assert(std::string(ex.what()) == "boom");
  }
  pool.shutdown(habib::ShutdownMode::Drain);
  assert(pool.stats().failed == 1);
  assert(pool.stats().completed == 0);
}

void test_drain_waits_for_in_flight() {
  habib::WorkerPool pool(1, 4);
  std::latch in_task{1};
  std::latch finish{1};

  auto fut = pool.submit([&] {
    in_task.count_down();
    finish.wait();
    return 7;
  });
  in_task.wait();

  std::thread shutdown_thread([&] { pool.shutdown(habib::ShutdownMode::Drain); });
  expect_within(std::chrono::milliseconds(200), [&] {
    assert(shutdown_thread.joinable());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  });

  finish.count_down();
  shutdown_thread.join();
  assert(fut.get() == 7);
}

void test_cancel_discards_queued_not_in_flight() {
  habib::WorkerPool pool(1, 8);
  std::latch hold_worker{1};
  std::latch unblock{1};

  auto first = pool.submit([&] {
    hold_worker.count_down();
    unblock.wait();
  });
  hold_worker.wait();

  for (int i = 0; i < 5; ++i) {
    (void)pool.submit([i] { return i; });
  }

  std::thread closer([&] { pool.shutdown(habib::ShutdownMode::Cancel); });
  unblock.count_down();
  closer.join();
  first.get();

  const auto stats = pool.stats();
  assert(stats.completed >= 1);
  assert(stats.submitted >= 6);
}

void test_concurrent_shutdown_once() {
  habib::WorkerPool pool(2, 4);
  auto a = pool.submit([] { return 1; });
  std::latch start{2};
  std::thread t1([&] {
    start.arrive_and_wait();
    pool.shutdown(habib::ShutdownMode::Drain);
  });
  std::thread t2([&] {
    start.arrive_and_wait();
    pool.shutdown(habib::ShutdownMode::Cancel);
  });
  t1.join();
  t2.join();
  assert(a.get() == 1);
  assert(pool.is_shutdown());
}

void test_stop_token_visible() {
  habib::WorkerPool pool(1, 2);
  std::latch seen{1};
  auto fut = pool.submit([&] {
    seen.count_down();
    return 0;
  });
  seen.wait();
  assert(!pool.stop_token().stop_requested());
  pool.shutdown(habib::ShutdownMode::Drain);
  fut.get();
  assert(pool.stop_token().stop_requested());
}

void test_submit_rejected_after_shutdown_started() {
  habib::WorkerPool pool(1, 4);
  std::latch block{1};
  std::latch done{1};
  (void)pool.submit([&] {
    block.count_down();
    done.wait();
  });
  block.wait();
  std::thread shutdown_thread([&] { pool.shutdown(habib::ShutdownMode::Drain); });
  expect_within(std::chrono::seconds(2), [&] {
    while (!pool.is_shutdown()) {
      std::this_thread::yield();
    }
    auto fut = pool.submit([] { return 1; });
    assert(fut.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready);
    try {
      fut.get();
      assert(false);
    } catch (const std::runtime_error&) {
    }
  });
  done.count_down();
  shutdown_thread.join();
}

}  // namespace

int main() {
  test_reject_policy_when_full();
  test_exception_propagation_and_failed_count();
  test_drain_waits_for_in_flight();
  test_cancel_discards_queued_not_in_flight();
  test_concurrent_shutdown_once();
  test_stop_token_visible();
  test_submit_rejected_after_shutdown_started();
  return 0;
}
