#include "telemetry.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// libpq — PostgreSQL C client library.
#include <libpq-fe.h>

namespace bench {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Format a system_clock time_point as an ISO-8601 timestamp string suitable
// for passing to Postgres as a TIMESTAMPTZ literal.
std::string tp_to_iso(std::chrono::system_clock::time_point tp) {
  const auto t = std::chrono::system_clock::to_time_t(tp);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      tp.time_since_epoch())
                      .count() %
                  1'000'000;
  std::ostringstream oss;
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
  oss << "." << std::setw(6) << std::setfill('0') << us << "Z";
  return oss.str();
}

// Escape a string for use in a Postgres dollar-quoted or literal context.
// We use parameterised queries so this is only needed for identifiers.
std::string escape_literal(PGconn* conn, const std::string& s) {
  char* escaped = PQescapeLiteral(conn, s.c_str(), s.size());
  std::string result(escaped ? escaped : "''");
  PQfreemem(escaped);
  return result;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TelemetryWriter
// ─────────────────────────────────────────────────────────────────────────────
/* static */ std::string TelemetryWriter::normalise_url(const std::string& url) {
  // Strip SQLAlchemy driver suffix: postgresql+psycopg2:// → postgresql://
  const std::string prefix = "postgresql+";
  if (url.rfind(prefix, 0) == 0) {
    const auto plus_pos = url.find('+');
    const auto colon_pos = url.find(':', plus_pos);
    if (colon_pos != std::string::npos) {
      return "postgresql" + url.substr(colon_pos);
    }
  }
  return url;
}

TelemetryWriter::TelemetryWriter(Config cfg, MetricQueue& queue)
    : config_(std::move(cfg)), queue_(queue) {
  config_.db_url = normalise_url(config_.db_url);
  thread_ = std::thread([this] { run(); });
}

TelemetryWriter::~TelemetryWriter() {
  queue_.close();
  if (thread_.joinable()) thread_.join();
  std::cerr << "[telemetry] shutdown — written=" << written_count_.load()
            << " errors=" << error_count_.load()
            << " queue_overflow=" << queue_.overflow_count() << "\n";
}

void TelemetryWriter::run() {
  const std::size_t batch_size = config_.batch_size;

  while (true) {
    // Block until at least one event is available or the queue is closed.
    auto first = queue_.dequeue();
    if (!first) {
      // Queue is closed and empty — we're done.
      break;
    }

    // Collect remaining events up to batch_size - 1.
    std::vector<MetricEvent> batch;
    batch.reserve(batch_size);
    batch.push_back(std::move(*first));

    auto additional = queue_.drain_batch(batch_size - 1);
    for (auto& e : additional) batch.push_back(std::move(e));

    flush_batch(batch);
  }

  // Drain any remaining events that arrived after the last dequeue().
  while (true) {
    auto tail = queue_.drain_batch(config_.batch_size);
    if (tail.empty()) break;
    flush_batch(tail);
  }
}

bool TelemetryWriter::flush_batch(const std::vector<MetricEvent>& batch) {
  if (batch.empty()) return true;

  PGconn* conn = PQconnectdb(config_.db_url.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "[telemetry] DB connection failed: " << PQerrorMessage(conn) << "\n";
    PQfinish(conn);
    ++error_count_;
    return false;
  }

  // Build a multi-row INSERT.
  std::ostringstream sql;
  sql << "INSERT INTO raw_metrics_table "
         "(run_id, arm, stage, start_ts, end_ts, duration_us) VALUES ";

  const auto run_id_lit = escape_literal(conn, config_.run_id);

  for (std::size_t i = 0; i < batch.size(); ++i) {
    const auto& e = batch[i];
    const auto end_tp   = e.end_tp;
    const auto start_tp = end_tp - std::chrono::microseconds(e.duration_us);

    sql << "(" << run_id_lit << ","
        << escape_literal(conn, e.arm) << ","
        << escape_literal(conn, to_string(e.stage)) << ","
        << "'" << tp_to_iso(start_tp) << "',"
        << "'" << tp_to_iso(end_tp) << "',"
        << e.duration_us << ")";

    if (i + 1 < batch.size()) sql << ",";
  }

  const std::string query = sql.str();
  PGresult* res = PQexec(conn, query.c_str());

  bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
  if (!ok) {
    std::cerr << "[telemetry] INSERT failed: " << PQresultErrorMessage(res) << "\n";
    ++error_count_;
  } else {
    written_count_ += batch.size();
  }

  PQclear(res);
  PQfinish(conn);
  return ok;
}

}  // namespace bench
