#include "redis_connector.hpp"
#include "../utils/logger.hpp"
#include "../utils/timer.hpp"
#include <filesystem>

#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#endif

namespace dedup {
namespace fs = std::filesystem;

bool RedisConnector::connect(const DbConnection& conn) {
#ifdef DEDUP_DRY_RUN
    LOG_INF("[redis] DRY RUN: simulating connection to %s:%u", conn.host.c_str(), conn.port);
    connected_ = true;
    return true;
#endif

#ifdef HAS_HIREDIS
    struct timeval timeout = {10, 0};
    auto* c = redisConnectWithTimeout(conn.host.c_str(), conn.port, timeout);
    if (!c || c->err) {
        LOG_ERR("[redis] Connection failed: %s", c ? c->errstr : "null context");
        if (c) redisFree(c);
        return false;
    }
    ctx_ = c;

    // Authenticate if credentials are provided (Redis 6+ ACL)
    if (!conn.user.empty() && !conn.password.empty()) {
        auto* auth = static_cast<redisReply*>(
            redisCommand(c, "AUTH %s %s", conn.user.c_str(), conn.password.c_str()));
        if (!auth || auth->type == REDIS_REPLY_ERROR) {
            LOG_ERR("[redis] ACL AUTH failed: %s", auth ? auth->str : "null");
            if (auth) freeReplyObject(auth);
            redisFree(c);
            ctx_ = nullptr;
            return false;
        }
        freeReplyObject(auth);
        LOG_INF("[redis] Authenticated as user '%s'", conn.user.c_str());
    } else if (!conn.password.empty()) {
        auto* auth = static_cast<redisReply*>(
            redisCommand(c, "AUTH %s", conn.password.c_str()));
        if (!auth || auth->type == REDIS_REPLY_ERROR) {
            LOG_ERR("[redis] AUTH failed: %s", auth ? auth->str : "null");
            if (auth) freeReplyObject(auth);
            redisFree(c);
            ctx_ = nullptr;
            return false;
        }
        freeReplyObject(auth);
    }

    // Cluster mode: no SELECT, use key-prefix "dedup:" for lab isolation
    // Verify connectivity with PING
    auto* reply = static_cast<redisReply*>(redisCommand(c, "PING"));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        LOG_ERR("[redis] PING failed: %s", reply ? reply->str : "null");
        if (reply) freeReplyObject(reply);
        redisFree(c);
        ctx_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    LOG_INF("[redis] Connected to %s:%u (cluster mode, key-prefix: %s*)",
            conn.host.c_str(), conn.port, KEY_PREFIX);
    connected_ = true;
    return true;
#else
    LOG_ERR("[redis] hiredis not available, Redis connector disabled");
    (void)conn;
    return false;
#endif
}

void RedisConnector::disconnect() {
#ifdef HAS_HIREDIS
    if (ctx_) {
        redisFree(static_cast<redisContext*>(ctx_));
        ctx_ = nullptr;
    }
#endif
    connected_ = false;
}

bool RedisConnector::is_connected() const { return connected_; }

bool RedisConnector::create_lab_schema(const std::string&) {
    // Cluster mode: no schema to create, keys are prefixed with "dedup:"
    LOG_INF("[redis] Lab isolation via key-prefix \"%s*\" (cluster mode)", KEY_PREFIX);
    return true;
}

bool RedisConnector::drop_lab_schema(const std::string&) {
    LOG_WRN("[redis] Deleting all lab keys with prefix \"%s*\"", KEY_PREFIX);
#ifdef DEDUP_DRY_RUN
    return true;
#endif
    return delete_all_lab_keys() >= 0;
}

bool RedisConnector::reset_lab_schema(const std::string& s) {
    return drop_lab_schema(s);
}

// SCAN + DEL pattern for cluster-safe key deletion (no FLUSHDB in cluster!)
int64_t RedisConnector::delete_all_lab_keys() {
#ifdef HAS_HIREDIS
    if (!ctx_) return -1;
    auto* c = static_cast<redisContext*>(ctx_);

    int64_t total_deleted = 0;
    std::string cursor = "0";
    std::string pattern = std::string(KEY_PREFIX) + "*";

    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(c, "SCAN %s MATCH %s COUNT 1000",
                         cursor.c_str(), pattern.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            LOG_ERR("[redis] SCAN failed");
            return -1;
        }

        cursor = reply->element[0]->str;
        auto* keys = reply->element[1];

        if (keys->elements > 0) {
            // Build DEL command with all found keys
            std::vector<const char*> argv;
            std::vector<size_t> argvlen;
            argv.push_back("DEL");
            argvlen.push_back(3);

            for (size_t i = 0; i < keys->elements; i++) {
                argv.push_back(keys->element[i]->str);
                argvlen.push_back(keys->element[i]->len);
            }

            auto* del_reply = static_cast<redisReply*>(
                redisCommandArgv(c, static_cast<int>(argv.size()),
                                 argv.data(), argvlen.data()));
            if (del_reply && del_reply->type == REDIS_REPLY_INTEGER) {
                total_deleted += del_reply->integer;
            }
            if (del_reply) freeReplyObject(del_reply);
        }

        freeReplyObject(reply);
    } while (cursor != "0");

    LOG_INF("[redis] Deleted %lld lab keys", total_deleted);
    return total_deleted;
#else
    return -1;
#endif
}

MeasureResult RedisConnector::bulk_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[redis] Bulk insert from %s", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[redis] DRY RUN: would bulk-insert files");
    result.rows_affected = 42;
    return result;
#endif

#ifdef HAS_HIREDIS
    Timer timer;
    timer.start();
    auto* c = static_cast<redisContext*>(ctx_);

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string key = std::string(KEY_PREFIX) + entry.path().filename().string();
        auto* reply = static_cast<redisReply*>(
            redisCommand(c, "SET %s %b", key.c_str(), buf.data(), fsize));
        if (reply) {
            result.rows_affected++;
            freeReplyObject(reply);
        }
        result.bytes_logical += fsize;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[redis] Bulk insert: %lld keys, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, timer.elapsed_ms());
#endif
    return result;
}

MeasureResult RedisConnector::perfile_insert(const std::string& data_dir, DupGrade grade) {
    MeasureResult result{};
    const std::string dir = data_dir + "/" + dup_grade_str(grade);
    LOG_INF("[redis] Per-file insert from %s (with per-key latency)", dir.c_str());

#ifdef DEDUP_DRY_RUN
    LOG_INF("[redis] DRY RUN: would per-file-insert keys");
    result.rows_affected = 42;
    return result;
#endif

#ifdef HAS_HIREDIS
    Timer total_timer;
    total_timer.start();
    auto* c = static_cast<redisContext*>(ctx_);

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fsize = entry.file_size();
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> buf(fsize);
        f.read(buf.data(), static_cast<std::streamsize>(fsize));

        std::string key = std::string(KEY_PREFIX) + entry.path().filename().string();

        int64_t set_ns = 0;
        {
            ScopedTimer st(set_ns);
            auto* reply = static_cast<redisReply*>(
                redisCommand(c, "SET %s %b", key.c_str(), buf.data(), fsize));
            if (reply) {
                result.rows_affected++;
                freeReplyObject(reply);
            }
        }
        result.per_file_latencies_ns.push_back(set_ns);
        result.bytes_logical += fsize;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[redis] Per-file insert: %lld keys, %lld bytes, %lld ms",
        result.rows_affected, result.bytes_logical, total_timer.elapsed_ms());
#endif
    return result;
}

MeasureResult RedisConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[redis] Deleting all lab keys individually (prefix: %s*)", KEY_PREFIX);
#ifdef DEDUP_DRY_RUN
    LOG_INF("[redis] DRY RUN: would delete lab keys individually");
    return result;
#endif
#ifdef HAS_HIREDIS
    if (!ctx_) return result;
    auto* c = static_cast<redisContext*>(ctx_);

    Timer total_timer;
    total_timer.start();

    // Collect all lab keys first
    std::vector<std::string> all_keys;
    std::string cursor = "0";
    std::string pattern = std::string(KEY_PREFIX) + "*";

    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(c, "SCAN %s MATCH %s COUNT 1000",
                         cursor.c_str(), pattern.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            break;
        }
        cursor = reply->element[0]->str;
        auto* keys = reply->element[1];
        for (size_t i = 0; i < keys->elements; i++) {
            all_keys.emplace_back(keys->element[i]->str, keys->element[i]->len);
        }
        freeReplyObject(reply);
    } while (cursor != "0");

    LOG_INF("[redis] Deleting %zu keys individually", all_keys.size());

    // Delete each key individually with latency tracking
    for (const auto& key : all_keys) {
        int64_t del_ns = 0;
        {
            ScopedTimer st(del_ns);
            auto* reply = static_cast<redisReply*>(
                redisCommand(c, "DEL %s", key.c_str()));
            if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0) {
                result.rows_affected++;
            }
            if (reply) freeReplyObject(reply);
        }
        result.per_file_latencies_ns.push_back(del_ns);
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
    LOG_INF("[redis] Per-key delete: %lld keys, %lld ms",
        result.rows_affected, total_timer.elapsed_ms());
#endif
    return result;
}

MeasureResult RedisConnector::run_maintenance() {
    MeasureResult result{};
    LOG_INF("[redis] No maintenance needed (in-memory store)");
    return result;
}

int64_t RedisConnector::get_logical_size_bytes() {
#ifdef DEDUP_DRY_RUN
    return 0;
#endif
#ifdef HAS_HIREDIS
    if (!ctx_) return -1;
    auto* c = static_cast<redisContext*>(ctx_);

    // Count lab keys using SCAN (cluster-safe) and sum their sizes
    int64_t total_bytes = 0;
    int64_t key_count = 0;
    std::string cursor = "0";
    std::string pattern = std::string(KEY_PREFIX) + "*";

    do {
        auto* reply = static_cast<redisReply*>(
            redisCommand(c, "SCAN %s MATCH %s COUNT 1000",
                         cursor.c_str(), pattern.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply) freeReplyObject(reply);
            return -1;
        }

        cursor = reply->element[0]->str;
        auto* keys = reply->element[1];

        for (size_t i = 0; i < keys->elements; i++) {
            key_count++;
            // Get string length for each key
            auto* len_reply = static_cast<redisReply*>(
                redisCommand(c, "STRLEN %s", keys->element[i]->str));
            if (len_reply && len_reply->type == REDIS_REPLY_INTEGER) {
                total_bytes += len_reply->integer;
            }
            if (len_reply) freeReplyObject(len_reply);
        }

        freeReplyObject(reply);
    } while (cursor != "0");

    LOG_INF("[redis] Lab key count: %lld, total value bytes: %lld", key_count, total_bytes);
    return total_bytes;
#else
    return -1;
#endif
}


// ============================================================================
// Native insertion mode (Stage 1) -- Redis HSET for structured data
// ============================================================================

bool RedisConnector::create_native_schema(const std::string& schema_name, PayloadType type) {
    // Redis is schemaless -- nothing to create
    LOG_INF("[redis] Native schema for %s: no-op (schemaless)", payload_type_str(type));
    return true;
}

bool RedisConnector::drop_native_schema(const std::string& schema_name, PayloadType type) {
    // Delete all keys with the native prefix
    return drop_lab_schema(schema_name);
}

MeasureResult RedisConnector::native_bulk_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
    LOG_INF("[redis] Native bulk insert: %zu records (type: %s)",
        records.size(), payload_type_str(type));

#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

#ifdef HAS_HIREDIS
    auto ns = get_native_schema(type);
    std::string prefix = "dedup:" + ns.table_name + ":";

    Timer timer;
    timer.start();

    int64_t idx = 0;
    for (const auto& rec : records) {
        // Build HSET command: HSET prefix:idx field1 val1 field2 val2 ...
        std::vector<std::string> args = {"HSET", prefix + std::to_string(idx)};

        for (const auto& [col_name, val] : rec.columns) {
            args.push_back(col_name);
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) args.push_back("");
                else if constexpr (std::is_same_v<T, bool>) args.push_back(v ? "1" : "0");
                else if constexpr (std::is_same_v<T, int64_t>) args.push_back(std::to_string(v));
                else if constexpr (std::is_same_v<T, double>) args.push_back(std::to_string(v));
                else if constexpr (std::is_same_v<T, std::string>) args.push_back(v);
                else if constexpr (std::is_same_v<T, std::vector<char>>) {
                    args.push_back(std::string(v.begin(), v.end()));
                }
            }, val);
        }

        // Execute HSET via hiredis
        std::vector<const char*> argv;
        std::vector<size_t> argvlen;
        for (const auto& a : args) {
            argv.push_back(a.c_str());
            argvlen.push_back(a.size());
        }

        redisReply* reply = static_cast<redisReply*>(
            redisCommandArgv(static_cast<redisContext*>(ctx_),
                static_cast<int>(argv.size()), argv.data(), argvlen.data()));
        if (reply) {
            result.rows_affected++;
            freeReplyObject(reply);
        }
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
        idx++;
    }

    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    LOG_INF("[redis] Native bulk insert: %lld rows, %lld ms",
        result.rows_affected, timer.elapsed_ms());
#else
    result.error = "Redis not compiled (HAS_HIREDIS not defined)";
#endif
    return result;
}

MeasureResult RedisConnector::native_perfile_insert(
    const std::vector<NativeRecord>& records, PayloadType type) {
    MeasureResult result{};
#ifdef DEDUP_DRY_RUN
    result.rows_affected = static_cast<int64_t>(records.size());
    return result;
#endif

#ifdef HAS_HIREDIS
    auto ns = get_native_schema(type);
    std::string prefix = "dedup:" + ns.table_name + ":";

    Timer total_timer;
    total_timer.start();

    int64_t idx = 0;
    for (const auto& rec : records) {
        std::vector<std::string> args = {"HSET", prefix + std::to_string(idx)};
        for (const auto& [col_name, val] : rec.columns) {
            args.push_back(col_name);
            std::visit([&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) args.push_back("");
                else if constexpr (std::is_same_v<T, bool>) args.push_back(v ? "1" : "0");
                else if constexpr (std::is_same_v<T, int64_t>) args.push_back(std::to_string(v));
                else if constexpr (std::is_same_v<T, double>) args.push_back(std::to_string(v));
                else if constexpr (std::is_same_v<T, std::string>) args.push_back(v);
                else if constexpr (std::is_same_v<T, std::vector<char>>) {
                    args.push_back(std::string(v.begin(), v.end()));
                }
            }, val);
        }

        std::vector<const char*> argv;
        std::vector<size_t> argvlen;
        for (const auto& a : args) {
            argv.push_back(a.c_str());
            argvlen.push_back(a.size());
        }

        int64_t insert_ns = 0;
        {
            ScopedTimer st(insert_ns);
            redisReply* reply = static_cast<redisReply*>(
                redisCommandArgv(static_cast<redisContext*>(ctx_),
                    static_cast<int>(argv.size()), argv.data(), argvlen.data()));
            if (reply) {
                result.rows_affected++;
                freeReplyObject(reply);
            }
        }
        result.per_file_latencies_ns.push_back(insert_ns);
        result.bytes_logical += static_cast<int64_t>(rec.estimated_size_bytes());
        idx++;
    }

    total_timer.stop();
    result.duration_ns = total_timer.elapsed_ns();
#else
    result.error = "Redis not compiled";
#endif
    return result;
}

MeasureResult RedisConnector::native_perfile_delete(PayloadType type) {
    // Same as BLOB delete -- scan and delete all dedup: keys
    return perfile_delete();
}

int64_t RedisConnector::get_native_logical_size_bytes(PayloadType type) {
    return get_logical_size_bytes();
}

} // namespace dedup
