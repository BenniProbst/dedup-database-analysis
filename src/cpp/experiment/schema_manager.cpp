#include "schema_manager.hpp"
#include "../utils/logger.hpp"

namespace dedup {

void SchemaManager::add_connector(std::shared_ptr<DbConnector> connector) {
    connectors_.push_back(std::move(connector));
}

bool SchemaManager::create_all_lab_schemas(const std::string& schema_name) {
    LOG_INF("[schema] Creating lab schemas on %zu connectors", connectors_.size());
    bool all_ok = true;
    for (auto& conn : connectors_) {
        if (!conn->is_connected()) {
            LOG_WRN("[schema] %s not connected, skipping", conn->system_name());
            continue;
        }
        if (!conn->create_lab_schema(schema_name)) {
            LOG_ERR("[schema] Failed to create lab schema on %s", conn->system_name());
            all_ok = false;
        }
    }
    return all_ok;
}

bool SchemaManager::reset_all_lab_schemas(const std::string& schema_name) {
    LOG_INF("[schema] RESETTING lab schemas on %zu connectors (post-run cleanup)",
        connectors_.size());
    bool all_ok = true;
    for (auto& conn : connectors_) {
        if (!conn->is_connected()) continue;
        if (!conn->reset_lab_schema(schema_name)) {
            LOG_ERR("[schema] Failed to reset lab schema on %s", conn->system_name());
            all_ok = false;
        }
    }
    return all_ok;
}

bool SchemaManager::drop_all_lab_schemas(const std::string& schema_name) {
    LOG_WRN("[schema] DROPPING all lab schemas (final cleanup)");
    bool all_ok = true;
    for (auto& conn : connectors_) {
        if (!conn->is_connected()) continue;
        if (!conn->drop_lab_schema(schema_name)) {
            LOG_ERR("[schema] Failed to drop lab schema on %s", conn->system_name());
            all_ok = false;
        }
    }
    return all_ok;
}

} // namespace dedup
