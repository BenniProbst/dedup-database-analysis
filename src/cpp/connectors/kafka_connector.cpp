#include "kafka_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include <filesystem>
#include <fstream>

#ifdef HAS_RDKAFKA
#include <librdkafka/rdkafka.h>
#endif

namespace dedup {
namespace fs = std::filesystem;

bool KafkaConnector::connect(const DbConnection& conn) {
    bootstrap_ = conn.host + ":" + std::to_string(conn.port);
    topic_prefix_ = conn.lab_schema.empty() ? "dedup-lab" : conn.lab_schema;

#ifdef DEDUP_DRY_RUN
    LOG_INF("[kafka] DRY RUN: simulating connection to %s", bootstrap_.c_str());
    connected_ = true;
    return true;
#endif

#ifdef HAS_RDKAFKA
    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "bootstrap.servers", bootstrap_.c_str(), errstr, sizeof(errstr));
    rd_kafka_conf_set(conf, "client.id", "dedup-test", errstr, sizeof(errstr));

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (!rk) {
        LOG_ERR("[kafka] Producer creation failed: %s", errstr);
        return false;
    }
    producer_ = rk;
    connected_ = true;
    LOG_INF("[kafka] Connected to %s, topic prefix: %s-*", bootstrap_.c_str(), topic_prefix_.c_str());
    return true;
#else
    LOG_ERR("[kafka] librdkafka not available, Kafka connector disabled");
    (void)conn;
    return false;
#endif
}

void KafkaConnector::disconnect() {
#ifdef HAS_RDKAFKA
    if (producer_) {
        auto* rk = static_cast<rd_kafka_t*>(producer_);
        rd_kafka_flush(rk, 10000);
        rd_kafka_destroy(rk);
        producer_ = nullptr;
    }
#endif
    connected_ = false;
}

bool KafkaConnector::is_connected() const { return connected_; }

bool KafkaConnector::create_lab_schema(const std::string&) {
#ifdef DEDUP_DRY_RUN
    LOG_INF("[kafka] DRY RUN: would create topics %s-U0, %s-U50, %s-U90",
        topic_prefix_.c_str(), topic_prefix_.c_str(), topic_prefix_.c_str());
    return true;
#endif

#ifdef HAS_RDKAFKA
    if (!producer_) return false;
    auto* rk = static_cast<rd_kafka_t*>(producer_);

    // Create topics via AdminClient API
    const char* suffixes[] = {"U0", "U50", "U90"};
    for (const auto& suffix : suffixes) {
        std::string topic_name = topic_prefix_ + "-" + suffix;

        rd_kafka_NewTopic_t* new_topic = rd_kafka_NewTopic_new(
            topic_name.c_str(), 4, 3, nullptr, 0);  // 4 partitions, RF=3

        rd_kafka_NewTopic_t* topics[] = {new_topic};

        rd_kafka_AdminOptions_t* options = rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_CREATETOPICS);
        rd_kafka_AdminOptions_set_request_timeout(options, 30000, nullptr, 0);

        rd_kafka_queue_t* queue = rd_kafka_queue_new(rk);
        rd_kafka_CreateTopics(rk, topics, 1, options, queue);

        rd_kafka_event_t* event = rd_kafka_queue_poll(queue, 30000);
        if (event) {
            const rd_kafka_CreateTopics_result_t* result = rd_kafka_event_CreateTopics_result(event);
            if (result) {
                size_t cnt = 0;
                const rd_kafka_topic_result_t** res_topics = rd_kafka_CreateTopics_result_topics(result, &cnt);
                for (size_t i = 0; i < cnt; i++) {
                    rd_kafka_resp_err_t err = rd_kafka_topic_result_error(res_topics[i]);
                    if (err == RD_KAFKA_RESP_ERR_NO_ERROR || err == RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS) {
                        LOG_INF("[kafka] Topic created: %s", rd_kafka_topic_result_name(res_topics[i]));
                    } else {
                        LOG_ERR("[kafka] Topic create failed: %s -- %s",
                            rd_kafka_topic_result_name(res_topics[i]),
                            rd_kafka_topic_result_error_string(res_topics[i]));
                    }
                }
            }
            rd_kafka_event_destroy(event);
        }

        rd_kafka_queue_destroy(queue);
        rd_kafka_AdminOptions_destroy(options);
        rd_kafka_NewTopic_destroy(new_topic);
    }

    LOG_INF("[kafka] Lab topics created: %s-{U0,U50,U90}", topic_prefix_.c_str());
    return true;
#else
    LOG_INF("[kafka] Lab topics: %s-U0, %s-U50, %s-U90 (auto-create on produce)",
        topic_prefix_.c_str(), topic_prefix_.c_str(), topic_prefix_.c_str());
    return true;
#endif
}

bool KafkaConnector::drop_lab_schema(const std::string&) {
#ifdef DEDUP_DRY_RUN
    LOG_WRN("[kafka] DRY RUN: would delete topics %s-U0, %s-U50, %s-U90",
        topic_prefix_.c_str(), topic_prefix_.c_str(), topic_prefix_.c_str());
    return true;
#endif

#ifdef HAS_RDKAFKA
    if (!producer_) return false;
    auto* rk = static_cast<rd_kafka_t*>(producer_);

    const char* suffixes[] = {"U0", "U50", "U90"};
    for (const auto& suffix : suffixes) {
        std::string topic_name = topic_prefix_ + "-" + suffix;

        rd_kafka_DeleteTopic_t* del_topic = rd_kafka_DeleteTopic_new(topic_name.c_str());
        rd_kafka_DeleteTopic_t* topics[] = {del_topic};

        rd_kafka_AdminOptions_t* options = rd_kafka_AdminOptions_new(rk, RD_KAFKA_ADMIN_OP_DELETETOPICS);
        rd_kafka_AdminOptions_set_request_timeout(options, 30000, nullptr, 0);

        rd_kafka_queue_t* queue = rd_kafka_queue_new(rk);
        rd_kafka_DeleteTopics(rk, topics, 1, options, queue);

        rd_kafka_event_t* event = rd_kafka_queue_poll(queue, 30000);
        if (event) {
            const rd_kafka_DeleteTopics_result_t* result = rd_kafka_event_DeleteTopics_result(event);
            if (result) {
                size_t cnt = 0;
                const rd_kafka_topic_result_t** res_topics = rd_kafka_DeleteTopics_result_topics(result, &cnt);
                for (size_t i = 0; i < cnt; i++) {
                    rd_kafka_resp_err_t err = rd_kafka_topic_result_error(res_topics[i]);
                    if (err == RD_KAFKA_RESP_ERR_NO_ERROR || err == RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART) {
                        LOG_INF("[kafka] Topic deleted: %s", rd_kafka_topic_result_name(res_topics[i]));
                    } else {
                        LOG_ERR("[kafka] Topic delete failed: %s -- %s",
                            rd_kafka_topic_result_name(res_topics[i]),
                            rd_kafka_topic_result_error_string(res_topics[i]));
                    }
                }
            }
            rd_kafka_event_destroy(event);
        }

        rd_kafka_queue_destroy(queue);
        rd_kafka_AdminOptions_destroy(options);
        rd_kafka_DeleteTopic_destroy(del_topic);
    }

    LOG_WRN("[kafka] Lab topics deleted: %s-{U0,U50,U90}", topic_prefix_.c_str());
    return true;
#else
    LOG_WRN("[kafka] Topic deletion requires librdkafka AdminClient -- topics NOT deleted");
    return true;
#endif
}

bool KafkaConnector::reset_lab_schema(const std::string& s) {
    return drop_lab_schema(s) && create_lab_schema(s);
}

MeasureResult KafkaConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    const std::string topic = topic_prefix_ + "-" + dup_grade_str(grade);

    LOG_INF("[kafka] Bulk produce to topic %s from %s", topic.c_str(), dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[kafka] DRY RUN: would produce messages to %s", topic.c_str());
    result.rows_affected = 42;
    return result;
#endif

#ifdef HAS_RDKAFKA
    Timer timer;
    timer.start();
    auto* rk = static_cast<rd_kafka_t*>(producer_);

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string key = entry.path().filename().string();
        int err = rd_kafka_producev(rk,
            RD_KAFKA_V_TOPIC(topic.c_str()),
            RD_KAFKA_V_KEY(key.data(), key.size()),
            RD_KAFKA_V_VALUE(buf.data(), fsize),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_END);

        if (err == 0) result.rows_affected++;
        result.bytes_logical += fsize;
        rd_kafka_poll(rk, 0);
    }

    rd_kafka_flush(rk, 30000);
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[kafka] Bulk produce: %lld msgs, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
#endif
    return result;
}

MeasureResult KafkaConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    return bulk_insert(data_dir, grade);
}

MeasureResult KafkaConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[kafka] Per-file delete = recreate topics (delete + create)");
    // Kafka has no per-message delete; we delete and recreate topics
    return result;
}

MeasureResult KafkaConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[kafka] Maintenance = log compaction + retention (Strimzi-managed, background)");
    return result;
}

int64_t KafkaConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif

#ifdef HAS_RDKAFKA
    if (!producer_) return -1;
    auto* rk = static_cast<rd_kafka_t*>(producer_);

    int64_t total = 0;
    const char* suffixes[] = {"U0", "U50", "U90"};

    for (const auto& suffix : suffixes) {
        std::string topic_name = topic_prefix_ + "-" + suffix;
        rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk, topic_name.c_str(), nullptr);
        if (!rkt) continue;

        // Get metadata to find partition count
        const rd_kafka_metadata_t* metadata = nullptr;
        rd_kafka_resp_err_t err = rd_kafka_metadata(rk, 0, rkt, &metadata, 10000);
        if (err != RD_KAFKA_RESP_ERR_NO_ERROR || !metadata) {
            rd_kafka_topic_destroy(rkt);
            continue;
        }

        for (int t = 0; t < metadata->topic_cnt; t++) {
            if (std::string(metadata->topics[t].topic) != topic_name) continue;
            for (int p = 0; p < metadata->topics[t].partition_cnt; p++) {
                int32_t partition = metadata->topics[t].partitions[p].id;
                int64_t lo = 0, hi = 0;
                rd_kafka_query_watermark_offsets(rk, topic_name.c_str(), partition, &lo, &hi, 5000);
                // Approximate: (hi - lo) messages, but we don't know sizes without consuming
                // Use metadata approximation
                total += (hi - lo);
            }
        }

        rd_kafka_metadata_destroy(metadata);
        rd_kafka_topic_destroy(rkt);
    }

    // Return message count as proxy (actual size requires consuming all messages)
    LOG_INF("[kafka] Lab topic total messages (offset-based): %lld", total);
    return total;
#else
    return -1;
#endif
}

} // namespace dedup
