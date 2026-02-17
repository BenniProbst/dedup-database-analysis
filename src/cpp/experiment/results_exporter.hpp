// =============================================================================
// ResultsExporter -- Export experiment data and push to GitLab
//
// Workflow:
//   1. Consume metrics + events from Kafka topics
//   2. Write CSV/JSON tables to results/ directory
//   3. Git add + commit + push to GitLab
//   4. Signal that cleanup is safe (data is persistent in GitLab)
//
// CRITICAL: Export MUST complete BEFORE lab schemas are dropped!
//           Once lab data is deleted, Kafka topics are gone too.
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include "../config.hpp"
#include "metrics_trace.hpp"

namespace dedup {

class ResultsExporter {
public:
    ResultsExporter(const GitExportConfig& git_config,
                    const MetricsTraceConfig& trace_config,
                    const std::string& results_dir);

    // Consume all messages from dedup-lab-metrics topic and write to CSV
    // Returns number of metric points exported, or -1 on error
    int64_t export_metrics_csv(const std::string& filename = "metrics_trace.csv");

    // Consume all messages from dedup-lab-events topic and write to CSV
    // Returns number of events exported, or -1 on error
    int64_t export_events_csv(const std::string& filename = "events_trace.csv");

    // Git: add results/ + commit + push to GitLab
    // Returns true if push succeeded
    bool git_commit_and_push(const std::string& commit_message = "");

    // Full export pipeline: metrics CSV + events CSV + git push
    // Returns true if ALL steps succeeded
    bool export_all();

private:
    GitExportConfig git_config_;
    MetricsTraceConfig trace_config_;
    std::string results_dir_;

    // Kafka consumer helper: consume all messages from a topic
    // Returns vector of JSON strings (one per message)
    std::vector<std::string> consume_topic(const std::string& topic);

    // Write CSV file from JSON metric points
    int64_t write_metrics_csv(const std::vector<std::string>& json_messages,
                               const std::string& filepath);

    // Write CSV file from JSON events
    int64_t write_events_csv(const std::vector<std::string>& json_messages,
                              const std::string& filepath);

    // Run a shell command and return exit code
    int run_cmd(const std::string& cmd);
};

} // namespace dedup
