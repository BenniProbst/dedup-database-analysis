#pragma once
#include "db_connector.hpp"

namespace dedup {

// Kafka connector -- uses topic prefix "dedup-lab-*" for isolation
// Production topics are NEVER touched
class KafkaConnector : public DbConnector {
public:
    ~KafkaConnector() override { disconnect(); }

    bool connect(const DbConnection& conn) override;
    void disconnect() override;
    [[nodiscard]] bool is_connected() const override;

    bool create_lab_schema(const std::string& schema_name) override;
    bool drop_lab_schema(const std::string& schema_name) override;
    bool reset_lab_schema(const std::string& schema_name) override;

    MeasureResult bulk_insert(const std::string& data_dir, DupGrade grade) override;
    MeasureResult perfile_insert(const std::string& data_dir, DupGrade grade) override;
    MeasureResult perfile_delete() override;
    MeasureResult run_maintenance() override;

    int64_t get_logical_size_bytes() override;

    [[nodiscard]] DbSystem system() const override { return DbSystem::KAFKA; }
    [[nodiscard]] const char* system_name() const override { return "kafka"; }

private:
    std::string bootstrap_;
    std::string topic_prefix_ = "dedup-lab";
    void* producer_ = nullptr;  // rd_kafka_t*
    bool connected_ = false;
};

} // namespace dedup
