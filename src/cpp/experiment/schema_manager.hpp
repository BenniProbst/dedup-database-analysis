#pragma once
// Manages lab schema lifecycle across all database connectors
// CRITICAL: ensures production data isolation
#include <memory>
#include <vector>
#include "../connectors/db_connector.hpp"

namespace dedup {

class SchemaManager {
public:
    // Register a connector to be managed
    void add_connector(std::shared_ptr<DbConnector> connector);

    // Create lab schemas on all registered connectors
    bool create_all_lab_schemas(const std::string& schema_name);

    // Reset (drop + create) lab schemas on all registered connectors
    // Called AFTER EACH experiment run
    bool reset_all_lab_schemas(const std::string& schema_name);

    // Drop all lab schemas (final cleanup)
    bool drop_all_lab_schemas(const std::string& schema_name);

    [[nodiscard]] const std::vector<std::shared_ptr<DbConnector>>& connectors() const {
        return connectors_;
    }

private:
    std::vector<std::shared_ptr<DbConnector>> connectors_;
};

} // namespace dedup
