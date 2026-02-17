cmake_minimum_required(VERSION 3.20)
project(dedup-integration-test
    VERSION 1.0.0
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

# libcurl -- for MinIO S3 API, Prometheus queries, Grafana push
find_package(CURL REQUIRED)

# hiredis (Redis C client) -- optional, fallback to raw TCP
find_library(HIREDIS_LIB hiredis)
find_path(HIREDIS_INCLUDE hiredis/hiredis.h)

# librdkafka (Kafka C client) -- optional
find_library(RDKAFKA_LIB rdkafka)
find_path(RDKAFKA_INCLUDE librdkafka/rdkafka.h)

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
    experiment/schema_manager.cpp
    experiment/data_loader.cpp
    experiment/metrics_collector.cpp
    experiment/dataset_generator.cpp
)

# =============================================================================
# Main executable
# =============================================================================

add_executable(dedup-test ${DEDUP_SOURCES})

target_include_directories(dedup-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PostgreSQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(dedup-test PRIVATE
    PostgreSQL::PostgreSQL
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# Optional: hiredis
if(HIREDIS_LIB AND HIREDIS_INCLUDE)
    target_compile_definitions(dedup-test PRIVATE HAS_HIREDIS=1)
    target_include_directories(dedup-test PRIVATE ${HIREDIS_INCLUDE})
    target_link_libraries(dedup-test PRIVATE ${HIREDIS_LIB})
    message(STATUS "hiredis found: ${HIREDIS_LIB}")
else()
    message(STATUS "hiredis not found -- Redis connector will use raw TCP")
endif()

# Optional: librdkafka
if(RDKAFKA_LIB AND RDKAFKA_INCLUDE)
    target_compile_definitions(dedup-test PRIVATE HAS_RDKAFKA=1)
    target_include_directories(dedup-test PRIVATE ${RDKAFKA_INCLUDE})
    target_link_libraries(dedup-test PRIVATE ${RDKAFKA_LIB})
    message(STATUS "librdkafka found: ${RDKAFKA_LIB}")
else()
    message(STATUS "librdkafka not found -- Kafka connector disabled")
endif()

# Optimization for N97 (Alder Lake-N, SSE4.2 + AVX2)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(dedup-test PRIVATE -O2)
endif()

# =============================================================================
# Dry-run smoke test target
# =============================================================================

add_executable(dedup-smoke-test ${DEDUP_SOURCES})

target_compile_definitions(dedup-smoke-test PRIVATE DEDUP_DRY_RUN=1)

target_include_directories(dedup-smoke-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PostgreSQL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(dedup-smoke-test PRIVATE
    PostgreSQL::PostgreSQL
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

if(HIREDIS_LIB AND HIREDIS_INCLUDE)
    target_compile_definitions(dedup-smoke-test PRIVATE HAS_HIREDIS=1)
    target_include_directories(dedup-smoke-test PRIVATE ${HIREDIS_INCLUDE})
    target_link_libraries(dedup-smoke-test PRIVATE ${HIREDIS_LIB})
endif()

if(RDKAFKA_LIB AND RDKAFKA_INCLUDE)
    target_compile_definitions(dedup-smoke-test PRIVATE HAS_RDKAFKA=1)
    target_include_directories(dedup-smoke-test PRIVATE ${RDKAFKA_INCLUDE})
    target_link_libraries(dedup-smoke-test PRIVATE ${RDKAFKA_LIB})
endif()

install(TARGETS dedup-test dedup-smoke-test RUNTIME DESTINATION bin)
