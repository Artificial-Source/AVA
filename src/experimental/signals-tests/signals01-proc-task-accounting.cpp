#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <thread>

namespace {

constexpr unsigned max_iterations = 20000;
constexpr unsigned max_reports = 8;
constexpr auto maximum_runtime = std::chrono::seconds(180);

// Read the procfs task-directory link count used by the former Application
// single-thread check. A value of three conventionally represents '.', '..',
// and the calling thread, but procfs may still expose a joined thread briefly.
bool read_task_link_count(nlink_t& link_count)
{
  struct stat task_directory{};
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (::stat("/proc/self/task", &task_directory) != 0)
    return false;
  link_count = task_directory.st_nlink;
  return true;
}

}  // namespace

// Demonstrate that a successful std::thread::join() does not guarantee that
// an immediate stat of /proc/self/task reports only the calling thread.
//
// The experiment reports timing-dependent mismatches but succeeds when none
// are observed, because procfs task-accounting lag is not guaranteed on every
// kernel or run. A stat failure is the only test failure.
int main()
{
  using Clock = std::chrono::steady_clock;
  auto const start = Clock::now();
  auto const deadline = start + maximum_runtime;
  unsigned iterations = 0;
  unsigned mismatches = 0;

  while (iterations < max_iterations && Clock::now() < deadline)
  {
    std::thread worker([] {});
    worker.join();
    ++iterations;

    nlink_t link_count = 0;
    if (!read_task_link_count(link_count))
    {
      std::perror("stat /proc/self/task");
      return 2;
    }
    if (link_count == 3)
      continue;

    ++mismatches;
    if (mismatches <= max_reports)
    {
      nlink_t immediate_restat = 0;
      if (!read_task_link_count(immediate_restat))
      {
        std::perror("re-stat /proc/self/task");
        return 2;
      }
      std::printf("mismatch iteration=%u st_nlink=%llu immediate_restat=%llu\n", iterations,
                  static_cast<unsigned long long>(link_count), static_cast<unsigned long long>(immediate_restat));
    }
  }

  double const elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("iterations=%u mismatches=%u elapsed_seconds=%.6f stop=%s\n", iterations, mismatches, elapsed,
              iterations == max_iterations ? "iteration_limit" : "time_limit");
  return 0;
}
