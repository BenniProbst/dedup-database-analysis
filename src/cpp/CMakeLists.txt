cmake_minimum_required(VERSION 3.20)
project(dedup-integration-test
    VERSION 1.1.0
    LANGUAGES CXX
    DESCRIPTION "Deduplication Database Integration Test - TU Dresden Research"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Dependencies (installed via apt on K8s runner: gcc:14-bookworm image)
# =============================================================================

# libpq (PostgreSQL C client) -- works for PostgreSQL AND CockroachDB (PG wire protocol)
find_package(PostgreSQL REQUIRED)

# libcurl -- for MinIO S3 API, ClickHouse HTTP API, Prometheus queries, Grafana push
find_package(CURL REQUIRED)

# hiredis (Redis C client) -- optional, fallback to raw TCP
find_library(HIREDIS_LIB hiredis)
find_path(HIREDIS_INCLUDE hiredis/hiredis.h)

# librdkafka (Kafka C client) -- optional
find_library(RDKAFKA_LIB rdkafka)
find_path(RDKAFKA_INCLUDE librdkafka/rdkafka.h)

# libmysqlclient (MariaDB/MySQL C client) -- optional
find_library(MYSQL_LIB mysqlclient mariadbclient)
find_path(MYSQL_INCLUDE mysql/mysql.h mariadb/mysql.h)

# nlohmann/json (header-only JSON)
# Prefer system-installed (apt: nlohmann-json3-dev), fallback to FetchContent
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package, trying FetchContent")
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(json)
endif()

# =============================================================================
# Common source files
# =============================================================================

set(DEDUP_SOURCES
    main.cpp
    connectors/postgres_connector.cpp
    connectors/redis_connector.cpp
    connectors/kafka_connector.cpp
    connectors/minio_connector.cpp
    connectors/mariadb_connector.cpp
    connectors/clickhouse_connector.cpp
    experiment/schema_manager.cpp
    experiment/data_loader.cpp
    experiment/metrics_collector.cpp
    experiment/dataset_generator.cpp
)

# =============================================================================
# Helper function: configure a target with all deps
# =============================================================================

function(configure_dedup_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PostgreSQL_INCLUDE_DIRS}
        ${CURL_INCLUDE_DIRS}
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        PostgreSQL::PostgreSQL
        CURL::libcurl
        nlohmann_json::nlohmann_json
    )

    # Optional: hiredis (Redis)
    if(HIREDIS_LIB AND HIREDIS_INCLUDE)
        target_compile_definitions(${TARGET_NAME} PRIVATE HAS_HIREDIS=1)
        target_include_directories(${TARGET_NAME} PRIVATE ${HIREDIS_INCLUDE})
        target_link_libraries(${TARGET_NAME} PRIVATE ${HIREDIS_LIB})
    endif()

    # Optional: librdkafka (Kafka)
    if(RDKAFKA_LIB AND RDKAFKA_INCLUDE)
        target_compile_definitions(${TARGET_NAME} PRIVATE HAS_RDKAFKA=1)
        target_include_directories(${TARGET_NAME} PRIVATE ${RDKAFKA_INCLUDE})
        target_link_libraries(${TARGET_NAME} PRIVATE ${RDKAFKA_LIB})
    endif()

    # Optional: libmysqlclient (MariaDB)
    if(MYSQL_LIB AND MYSQL_INCLUDE)
        target_compile_definitions(${TARGET_NAME} PRIVATE HAS_MYSQL=1)
        target_include_directories(${TARGET_NAME} PRIVATE ${MYSQL_INCLUDE})
        target_link_libraries(${TARGET_NAME} PRIVATE ${MYSQL_LIB})
    endif()

    # Optimization for N97 (Alder Lake-N, SSE4.2 + AVX2)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${TARGET_NAME} PRIVATE -O2)
    endif()
endfunction()

# =============================================================================
# Status messages for optional deps
# =============================================================================

if(HIREDIS_LIB AND HIREDIS_INCLUDE)
    message(STATUS "hiredis found: ${HIREDIS_LIB}")
else()
    message(STATUS "hiredis not found -- Redis connector will use raw TCP")
endif()

if(RDKAFKA_LIB AND RDKAFKA_INCLUDE)
    message(STATUS "librdkafka found: ${RDKAFKA_LIB}")
else()
    message(STATUS "librdkafka not found -- Kafka connector disabled")
endif()

if(MYSQL_LIB AND MYSQL_INCLUDE)
    message(STATUS "libmysqlclient found: ${MYSQL_LIB}")
else()
    message(STATUS "libmysqlclient not found -- MariaDB connector disabled")
endif()

message(STATUS "ClickHouse connector uses HTTP API via libcurl (no extra deps)")

# =============================================================================
# Main executable
# =============================================================================

add_executable(dedup-test ${DEDUP_SOURCES})
configure_dedup_target(dedup-test)

# =============================================================================
# Dry-run smoke test target
# =============================================================================

add_executable(dedup-smoke-test ${DEDUP_SOURCES})
target_compile_definitions(dedup-smoke-test PRIVATE DEDUP_DRY_RUN=1)
configure_dedup_target(dedup-smoke-test)

install(TARGETS dedup-test dedup-smoke-test RUNTIME DESTINATION bin)
