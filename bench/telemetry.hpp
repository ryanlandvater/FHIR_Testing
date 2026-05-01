#pragma once

// Non-blocking metric telemetry for the benchmark harness.
//
// Architecture:
//   - Timed benchmark code paths call MetricQueue::enqueue() ONLY.
//     No SQL, no network I/O, no filesystem writes inside timed sections.
//   - A background TelemetryWriter thread drains the queue and issues
//     batched INSERTs against the local Postgres instance via libpq.
//   - On destruction, TelemetryWriter signals the queue closed and joins
//     the background thread, guaranteeing all enqueued events are flushed
//     before the process exits.
//
// Queue policy:
//   - Bounded by kDefaultCapacity.  When full, enqueue() drops the event
//     and increments overflow_count().  In strict benchmark mode you should
//     assert overflow_count() == 0 after the run.
//
// Upgrade path:
//   - Replace the std::mutex + std::deque with a lock-free ring buffer if
//     profiling shows mutex contention from the benchmark hot path.

#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bench {

// ─────────────────────────────────────────────────────────────────────────────
// MetricQueue
// ─────────────────────────────────────────────────────────────────────────────
class MetricQueue {
 public:
  static constexpr std::size_t kDefaultCapacity = 65'536;

  explicit MetricQueue(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity) {}

  // Called from timed code paths. Non-blocking; drops on overflow.
  void enqueue(MetricEvent event) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (queue_.size() >= capacity_) {
      ++overflow_count_;
      return;
    }
    queue_.push_back(std::move(event));
    lk.unlock();
    cv_.notify_one();
  }

  // Called from writer thread. Blocks until an event is available or closed.
  std::optional<MetricEvent> dequeue() {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return std::nullopt;
    MetricEvent e = std::move(queue_.front());
    queue_.pop_front();
    return e;
  }

  // Drain up to max_count events without blocking. Used for batch flushing.
  std::vector<MetricEvent> drain_batch(std::size_t max_count) {
    std::vector<MetricEvent> batch;
    batch.reserve(max_count);
    std::unique_lock<std::mutex> lk(mutex_);
    while (!queue_.empty() && batch.size() < max_count) {
      batch.push_back(std::move(queue_.front()));
      queue_.pop_front();
    }
    return batch;
  }

  // Signal that no more events will be produced; writer will drain then exit.
  void close() {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  bool is_closed() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
  }

  std::size_t overflow_count() const { return overflow_count_.load(); }

 private:
  const std::size_t capacity_;
  std::deque<MetricEvent> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_{false};
  std::atomic<std::size_t> overflow_count_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// TelemetryWriter
// ─────────────────────────────────────────────────────────────────────────────
class TelemetryWriter {
 public:
  struct Config {
    std::string db_url;    // postgresql://user:pass@host:port/db  (no +driver suffix)
    std::string run_id;
    std::size_t batch_size = 256;
  };

  // Spawns the background writer thread immediately.
  explicit TelemetryWriter(Config cfg, MetricQueue& queue);

  // Closes the queue, joins the background thread, and prints final diagnostics.
  ~TelemetryWriter();

  TelemetryWriter(const TelemetryWriter&)            = delete;
  TelemetryWriter& operator=(const TelemetryWriter&) = delete;
  TelemetryWriter(TelemetryWriter&&)                 = delete;
  TelemetryWriter& operator=(TelemetryWriter&&)      = delete;

  // Number of events successfully written to the DB.
  std::size_t written_count() const { return written_count_.load(); }

  // Number of DB write errors (batches that failed and were dropped).
  std::size_t error_count() const { return error_count_.load(); }

 private:
  void run();
  bool flush_batch(const std::vector<MetricEvent>& batch);
  // Normalise SQLAlchemy-style URLs (postgresql+psycopg2://...) to plain PG URLs.
  static std::string normalise_url(const std::string& url);

  Config config_;
  MetricQueue& queue_;
  std::thread thread_;
  std::atomic<std::size_t> written_count_{0};
  std::atomic<std::size_t> error_count_{0};
};

}  // namespace bench
