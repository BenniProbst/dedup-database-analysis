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
    // Kafka topics are auto-created on first produce
    LOG_INF("[kafka] Lab topics: %s-U0, %s-U50, %s-U90 (auto-create on produce)",
        topic_prefix_.c_str(), topic_prefix_.c_str(), topic_prefix_.c_str());
    return true;
}

bool KafkaConnector::drop_lab_schema(const std::string&) {
    LOG_WRN("[kafka] Topic deletion requires admin API -- use kafka-topics CLI");
    // TODO: AdminClient API to delete topics matching prefix
    return true;
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
    return bulk_insert(data_dir, grade);  // Same for Kafka
}

MeasureResult KafkaConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[kafka] Per-file delete = topic deletion (requires admin API)");
    return result;
}

MeasureResult KafkaConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[kafka] Maintenance = log compaction + retention (background, Strimzi-managed)");
    return result;
}

int64_t KafkaConnector::get_logical_size_bytes() {
    // Would need admin API to query topic sizes
    return -1;
}

} // namespace dedup
