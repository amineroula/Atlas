#include <AtlasJobs/JobQueue.h>

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace atlas {

struct JobQueue::Job {
  JobSnapshot snapshot;
  JobWork work;
  std::stop_source stop;
};

JobQueue::JobQueue(std::size_t workers) {
  if (workers == 0) workers = std::max(1u, std::thread::hardware_concurrency() / 2);
  workers = std::min<std::size_t>(workers, 8);
  workers_.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    workers_.emplace_back([this](std::stop_token token) { workerLoop(token); });
  }
}

JobQueue::~JobQueue() {
  for (auto& worker : workers_) worker.request_stop();
  ready_.notify_all();
}

JobId JobQueue::submit(std::string name, JobWork work) {
  if (!work) throw std::invalid_argument("job work is required");
  auto job = std::make_shared<Job>();
  {
    std::lock_guard lock(mutex_);
    job->snapshot.id = nextId_++;
    job->snapshot.name = std::move(name);
    job->work = std::move(work);
    jobs_.emplace(job->snapshot.id, job);
    pending_.push(job);
  }
  notify(job);
  ready_.notify_one();
  return job->snapshot.id;
}

bool JobQueue::cancel(JobId id) {
  std::shared_ptr<Job> job;
  {
    std::lock_guard lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) return false;
    job = found->second;
    if (job->snapshot.state == JobState::Completed || job->snapshot.state == JobState::Failed ||
        job->snapshot.state == JobState::Cancelled) return false;
    job->stop.request_stop();
    if (job->snapshot.state == JobState::Queued) job->snapshot.state = JobState::Cancelled;
  }
  notify(job);
  ready_.notify_all();
  return true;
}

void JobQueue::pause() {
  std::vector<std::shared_ptr<Job>> changed;
  {
    std::lock_guard lock(mutex_);
    paused_ = true;
    for (const auto& [id, job] : jobs_) {
      if (job->snapshot.state == JobState::Queued) {
        job->snapshot.state = JobState::Paused;
        changed.push_back(job);
      }
    }
  }
  for (const auto& job : changed) notify(job);
}

void JobQueue::resume() {
  std::vector<std::shared_ptr<Job>> changed;
  {
    std::lock_guard lock(mutex_);
    paused_ = false;
    for (const auto& [id, job] : jobs_) {
      if (job->snapshot.state == JobState::Paused) {
        job->snapshot.state = JobState::Queued;
        changed.push_back(job);
      }
    }
  }
  for (const auto& job : changed) notify(job);
  ready_.notify_all();
}

bool JobQueue::waitForIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  return idle_.wait_for(lock, timeout, [this] {
    if (running_ != 0) return false;
    for (const auto& [id, job] : jobs_) {
      if (job->snapshot.state == JobState::Queued || job->snapshot.state == JobState::Paused ||
          job->snapshot.state == JobState::Running) return false;
    }
    return true;
  });
}

std::vector<JobSnapshot> JobQueue::snapshots() const {
  std::lock_guard lock(mutex_);
  std::vector<JobSnapshot> result;
  result.reserve(jobs_.size());
  for (const auto& [id, job] : jobs_) result.push_back(job->snapshot);
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.id < right.id;
  });
  return result;
}

void JobQueue::setObserver(JobObserver observer) {
  std::lock_guard lock(mutex_);
  observer_ = std::move(observer);
}

void JobQueue::workerLoop(std::stop_token token) {
  while (!token.stop_requested()) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, token, [this] { return !paused_ && !pending_.empty(); });
      if (token.stop_requested()) return;
      while (!pending_.empty()) {
        job = pending_.front();
        pending_.pop();
        if (job->snapshot.state == JobState::Cancelled) job.reset();
        else break;
      }
      if (!job) {
        idle_.notify_all();
        continue;
      }
      job->snapshot.state = JobState::Running;
      ++running_;
    }
    notify(job);
    try {
      job->work(job->stop.get_token(), [this, job](double fraction, std::string detail) {
        {
          std::lock_guard lock(mutex_);
          job->snapshot.progress.fraction = std::clamp(fraction, 0.0, 1.0);
          job->snapshot.progress.detail = std::move(detail);
        }
        notify(job);
      });
      {
        std::lock_guard lock(mutex_);
        job->snapshot.state = job->stop.stop_requested() ? JobState::Cancelled : JobState::Completed;
        if (job->snapshot.state == JobState::Completed) job->snapshot.progress.fraction = 1.0;
      }
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      job->snapshot.state = JobState::Failed;
      job->snapshot.error = error.what();
    } catch (...) {
      std::lock_guard lock(mutex_);
      job->snapshot.state = JobState::Failed;
      job->snapshot.error = "unknown error";
    }
    {
      std::lock_guard lock(mutex_);
      --running_;
    }
    notify(job);
    idle_.notify_all();
  }
}

void JobQueue::notify(const std::shared_ptr<Job>& job) {
  JobObserver observer;
  JobSnapshot snapshot;
  {
    std::lock_guard lock(mutex_);
    observer = observer_;
    snapshot = job->snapshot;
  }
  if (observer) observer(snapshot);
}

const char* jobStateName(JobState state) noexcept {
  switch (state) {
    case JobState::Queued: return "Queued";
    case JobState::Running: return "Running";
    case JobState::Paused: return "Paused";
    case JobState::Completed: return "Completed";
    case JobState::Cancelled: return "Cancelled";
    case JobState::Failed: return "Failed";
  }
  return "Unknown";
}

}  // namespace atlas
