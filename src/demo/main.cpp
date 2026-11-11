#include "worker_pool.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <stop_token>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
  std::cerr
      << "Usage: demo <workers> <capacity> <tasks> [block|reject]\n"
      << "  Local CLI demo for habib::WorkerPool — no network I/O.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    print_usage();
    return 1;
  }

  const std::size_t workers = static_cast<std::size_t>(std::stoul(argv[1]));
  const std::size_t capacity = static_cast<std::size_t>(std::stoul(argv[2]));
  const std::size_t tasks = static_cast<std::size_t>(std::stoul(argv[3]));
  const std::string_view policy_arg = argc == 5 ? argv[4] : "block";

  habib::SubmitPolicy policy = habib::SubmitPolicy::Block;
  if (policy_arg == "reject") {
    policy = habib::SubmitPolicy::Reject;
  } else if (policy_arg != "block") {
    print_usage();
    return 1;
  }

  habib::WorkerPool pool(workers, capacity, policy);
  std::vector<std::future<int>> futures;
  futures.reserve(tasks);

  for (std::size_t i = 0; i < tasks; ++i) {
    auto fut = pool.submit([i, token = pool.stop_token()] {
      if (token.stop_requested()) {
        return -1;
      }
      return static_cast<int>(i * i);
    });
    futures.push_back(std::move(fut));
  }

  long long sum = 0;
  for (auto& fut : futures) {
    sum += fut.get();
  }

  pool.shutdown(habib::ShutdownMode::Drain);
  const auto stats = pool.stats();

  std::cout << "workers=" << workers << " capacity=" << capacity << " policy="
            << (policy == habib::SubmitPolicy::Block ? "block" : "reject") << '\n';
  std::cout << "sum=" << sum << " submitted=" << stats.submitted
            << " completed=" << stats.completed << " failed=" << stats.failed
            << " rejected=" << stats.rejected << '\n';
  return 0;
}
