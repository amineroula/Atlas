#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace atlas {

using JobId = std::uint64_t;
enum class JobState { Queued, Running, Paused, Completed, Cancelled, Failed };

struct JobProgress {
  double fraction{};
  std::string detail;
};

struct JobSnapshot {
  JobId id{};
  std::string name;
  JobState state{JobState::Queued};
  JobProgress progress;
  std::string error;
};

using JobReporter = std::function<void(double, std::string)>;
using JobWork = std::function<void(std::stop_token, const JobReporter&)>;
using JobObserver = std::function<void(const JobSnapshot&)>;

class JobQueue {
 public:
  explicit JobQueue(std::size_t workers = 0);
  ~JobQueue();
  JobQueue(const JobQueue&) = delete;
  JobQueue& operator=(const JobQueue&) = delete;

  JobId submit(std::string name, JobWork work);
  bool cancel(JobId id);
  void pause();
  void resume();
  [[nodiscard]] bool waitForIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] std::vector<JobSnapshot> snapshots() const;
  void setObserver(JobObserver observer);

 private:
  struct Job;
  void workerLoop(std::stop_token token);
  void notify(const std::shared_ptr<Job>& job);

  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::condition_variable idle_;
  std::queue<std::shared_ptr<Job>> pending_;
  std::unordered_map<JobId, std::shared_ptr<Job>> jobs_;
  std::vector<std::jthread> workers_;
  JobObserver observer_;
  JobId nextId_{1};
  std::size_t running_{};
  bool paused_{};
};

[[nodiscard]] const char* jobStateName(JobState state) noexcept;

}  // namespace atlas
