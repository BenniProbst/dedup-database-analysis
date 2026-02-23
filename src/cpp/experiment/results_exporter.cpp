// =============================================================================
// ResultsExporter implementation
//
// Kafka consumer reads ALL messages from metrics/events topics,
// writes them as CSV, then commits + pushes to GitLab.
// =============================================================================

#include "results_exporter.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <filesystem>
#include <nlohmann/json.hpp>

#ifdef HAS_RDKAFKA
#include <librdkafka/rdkafka.h>
#endif

namespace fs = std::filesystem;

namespace dedup {

ResultsExporter::ResultsExporter(const GitExportConfig& git_config,
                                  const MetricsTraceConfig& trace_config,
                                  const std::string& results_dir)
    : git_config_(git_config)
    , trace_config_(trace_config)
    , results_dir_(results_dir)
{
    fs::create_directories(results_dir_);
}

// ---------------------------------------------------------------------------
// Kafka consumer: read all messages from a topic (from beginning to end)
// ---------------------------------------------------------------------------

std::vector<std::string> ResultsExporter::consume_topic(const std::string& topic) {
    std::vector<std::string> messages;

#ifdef HAS_RDKAFKA
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    char errstr[512];

    rd_kafka_conf_set(conf, "bootstrap.servers",
        trace_config_.kafka_bootstrap.c_str(), errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "group.id", "dedup-exporter", errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "auto.offset.reset", "earliest", errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "enable.auto.commit", "false", errstr, sizeof(errstr));

    rd_kafka_t* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!consumer) {
        LOG_ERR("[exporter] Kafka consumer create failed: %s", errstr);
        return messages;
    }

    rd_kafka_poll_set_consumer(consumer);

    rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(topics, topic.c_str(), RD_KAFKA_PARTITION_UA);

    rd_kafka_resp_err_t err = rd_kafka_subscribe(consumer, topics);
    rd_kafka_topic_partition_list_destroy(topics);

    if (err) {
        LOG_ERR("[exporter] Kafka subscribe failed: %s", rd_kafka_err2str(err));
        rd_kafka_destroy(consumer);
        return messages;
    }

    LOG_INF("[exporter] Consuming from %s ...", topic.c_str());

    // Read until no new messages for 5 seconds (topic fully consumed)
    int empty_polls = 0;
    while (empty_polls < 5) {
        rd_kafka_message_t* msg = rd_kafka_consumer_poll(consumer, 1000);
        if (!msg) { empty_polls++; continue; }

        if (msg->err == RD_KAFKA_RESP_ERR_NO_ERROR) {
            messages.emplace_back(static_cast<char*>(msg->payload), msg->len);
            empty_polls = 0;
        } else if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
            empty_polls++;
        } else {
            LOG_ERR("[exporter] Kafka error: %s", rd_kafka_message_errstr(msg));
            empty_polls++;
        }

        rd_kafka_message_destroy(msg);
    }

    rd_kafka_consumer_close(consumer);
    rd_kafka_destroy(consumer);

    LOG_INF("[exporter] Consumed %zu messages from %s", messages.size(), topic.c_str());
#else
    LOG_WRN("[exporter] No librdkafka -- cannot consume from %s", topic.c_str());
#endif

    return messages;
}

// ---------------------------------------------------------------------------
// CSV writers
// ---------------------------------------------------------------------------

int64_t ResultsExporter::write_metrics_csv(
        const std::vector<std::string>& json_messages,
        const std::string& filepath) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        LOG_ERR("[exporter] Cannot open %s for writing", filepath.c_str());
        return -1;
    }

    // CSV header
    out << "timestamp_ms,system,metric,value,unit\n";

    int64_t count = 0;
    for (const auto& msg : json_messages) {
        try {
            auto j = nlohmann::json::parse(msg);
            out << j.value("ts", 0LL) << ","
                << j.value("system", "") << ","
                << j.value("metric", "") << ","
                << j.value("value", 0.0) << ","
                << j.value("unit", "") << "\n";
            count++;
        } catch (const std::exception& e) {
            LOG_DBG("[exporter] Skip malformed JSON: %s", e.what());
        }
    }

    out.close();
    LOG_INF("[exporter] Wrote %lld metrics to %s",
        static_cast<long long>(count), filepath.c_str());
    return count;
}

int64_t ResultsExporter::write_events_csv(
        const std::vector<std::string>& json_messages,
        const std::string& filepath) {
    std::ofstream out(filepath);
    if (!out.is_open()) {
        LOG_ERR("[exporter] Cannot open %s for writing", filepath.c_str());
        return -1;
    }

    // CSV header
    out << "timestamp_ms,event_type,system,dup_grade,stage,detail\n";

    int64_t count = 0;
    for (const auto& msg : json_messages) {
        try {
            auto j = nlohmann::json::parse(msg);
            // Escape detail field (may contain commas/quotes)
            std::string detail = j.value("detail", "");
            // Simple CSV escaping: wrap in quotes, double internal quotes
            std::string escaped;
            escaped.reserve(detail.size() + 2);
            escaped += '"';
            for (char c : detail) {
                if (c == '"') escaped += '"';
                escaped += c;
            }
            escaped += '"';

            out << j.value("ts", 0LL) << ","
                << j.value("event", "") << ","
                << j.value("system", "") << ","
                << j.value("dup_grade", "") << ","
                << j.value("stage", "") << ","
                << escaped << "\n";
            count++;
        } catch (const std::exception& e) {
            LOG_DBG("[exporter] Skip malformed event JSON: %s", e.what());
        }
    }

    out.close();
    LOG_INF("[exporter] Wrote %lld events to %s",
        static_cast<long long>(count), filepath.c_str());
    return count;
}

// ---------------------------------------------------------------------------
// Public export methods
// ---------------------------------------------------------------------------

int64_t ResultsExporter::export_metrics_csv(const std::string& filename) {
    auto messages = consume_topic(trace_config_.metrics_topic);
    if (messages.empty()) {
        LOG_WRN("[exporter] No metrics to export from %s",
            trace_config_.metrics_topic.c_str());
        return 0;
    }
    return write_metrics_csv(messages, results_dir_ + "/" + filename);
}

int64_t ResultsExporter::export_events_csv(const std::string& filename) {
    auto messages = consume_topic(trace_config_.events_topic);
    if (messages.empty()) {
        LOG_WRN("[exporter] No events to export from %s",
            trace_config_.events_topic.c_str());
        return 0;
    }
    return write_events_csv(messages, results_dir_ + "/" + filename);
}

// ---------------------------------------------------------------------------
// Git commit + push
// ---------------------------------------------------------------------------

int ResultsExporter::run_cmd(const std::string& cmd) {
    LOG_DBG("[exporter] Running: %s", cmd.c_str());
    return std::system(cmd.c_str());
}

bool ResultsExporter::git_commit_and_push(const std::string& commit_message) {
    // Generate timestamp for commit message
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::gmtime(&t));

    std::string msg = commit_message.empty()
        ? std::string("Experiment results ") + ts
        : commit_message;

    // Configure git for GitLab self-signed cert
    if (!git_config_.ssl_verify) {
        run_cmd("git config http.sslVerify false");
    }

    // Stage results directory
    int rc = run_cmd("git add " + results_dir_ + "/");
    if (rc != 0) {
        LOG_ERR("[exporter] git add failed (rc=%d)", rc);
        return false;
    }

    // Check if there are changes to commit
    rc = run_cmd("git diff --cached --quiet");
    if (rc == 0) {
        LOG_INF("[exporter] No changes to commit");
        return true;
    }

    // Commit
    rc = run_cmd("git commit -m \"" + msg + "\"");
    if (rc != 0) {
        LOG_ERR("[exporter] git commit failed (rc=%d)", rc);
        return false;
    }

    LOG_INF("[exporter] Committed: %s", msg.c_str());

    // Push
    if (git_config_.auto_push) {
        std::string push_cmd = "git push " + git_config_.remote_name
                               + " " + git_config_.branch;
        rc = run_cmd(push_cmd);
        if (rc != 0) {
            LOG_ERR("[exporter] git push failed (rc=%d)", rc);
            return false;
        }
        LOG_INF("[exporter] Pushed to %s/%s",
            git_config_.remote_name.c_str(), git_config_.branch.c_str());
    }

    return true;
}

// ---------------------------------------------------------------------------
// Full export pipeline
// ---------------------------------------------------------------------------

bool ResultsExporter::export_all() {
    LOG_INF("[exporter] === EXPORT PIPELINE START ===");

    int64_t metrics = export_metrics_csv();
    int64_t events  = export_events_csv();

    LOG_INF("[exporter] Exported %lld metrics + %lld events to %s/",
        static_cast<long long>(metrics), static_cast<long long>(events),
        results_dir_.c_str());

    if (metrics <= 0 && events <= 0) {
        LOG_WRN("[exporter] Nothing to export -- skipping git push");
        return true;
    }

    bool pushed = git_commit_and_push();

    LOG_INF("[exporter] === EXPORT PIPELINE %s ===",
        pushed ? "COMPLETE" : "FAILED");

    return pushed;
}


// ---------------------------------------------------------------------------
// ExperimentResult CSV export (per-stage aggregated results)
// ---------------------------------------------------------------------------

void ResultsExporter::export_results_csv(
        const std::vector<ExperimentResult>& results,
        const std::string& output_dir) {
    std::string filepath = output_dir + "/experiment_results.csv";
    std::ofstream csv(filepath);
    if (!csv.is_open()) {
        LOG_ERR("[exporter] Cannot open %s", filepath.c_str());
        return;
    }

    // Header (25 columns)
    csv << "system,payload_type,dup_grade,insertion_mode,stage,"
        << "duration_ms,rows_affected,bytes_logical,"
        << "logical_size_before,logical_size_after,"
        << "phys_size_before,phys_size_after,phys_delta,"
        << "edr,throughput_mbps,"
        << "latency_p50_us,latency_p95_us,latency_p99_us,"
        << "latency_min_us,latency_max_us,latency_mean_us,"
        << "replica_count,volume_name,timestamp,error" << std::endl;

    for (const auto& r : results) {
        // Escape error field for CSV
        std::string safe_error = r.error;
        if (safe_error.find(',') != std::string::npos ||
            safe_error.find('"') != std::string::npos) {
            std::string escaped;
            escaped += '"';
            for (char c : safe_error) {
                if (c == '"') escaped += '"';
                escaped += c;
            }
            escaped += '"';
            safe_error = escaped;
        }

        csv << r.system << ","
            << r.payload_type << ","
            << r.dup_grade << ","
            << r.insertion_mode << ","
            << r.stage << ","
            << (r.duration_ns / 1000000) << ","
            << r.rows_affected << ","
            << r.bytes_logical << ","
            << r.logical_size_before << ","
            << r.logical_size_after << ","
            << r.phys_size_before << ","
            << r.phys_size_after << ","
            << r.phys_delta << ",";

        // Fixed precision for float fields
        csv << std::fixed;
        csv << std::setprecision(4) << r.edr << ",";
        csv << std::setprecision(2) << (r.throughput_bytes_per_sec / 1048576.0) << ",";

        // Latency in microseconds
        csv << (r.latency_p50_ns / 1000) << ","
            << (r.latency_p95_ns / 1000) << ","
            << (r.latency_p99_ns / 1000) << ","
            << (r.latency_min_ns / 1000) << ","
            << (r.latency_max_ns / 1000) << ",";
        csv << std::setprecision(1) << (r.latency_mean_ns / 1000.0) << ",";

        csv << r.replica_count << ","
            << r.volume_name << ","
            << r.timestamp << ","
            << safe_error << std::endl;
    }

    csv.close();
    LOG_INF("[exporter] Wrote %zu experiment results to %s",
        results.size(), filepath.c_str());
}

} // namespace dedup
