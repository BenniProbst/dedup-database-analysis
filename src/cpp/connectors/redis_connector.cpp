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
    // Same as bulk for Redis (each SET is atomic)
    return bulk_insert(data_dir, grade);
}

MeasureResult RedisConnector::perfile_delete() {
    MeasureResult result{};
    LOG_INF("[redis] Deleting all lab keys (prefix: %s*)", KEY_PREFIX);
#ifdef DEDUP_DRY_RUN
    return result;
#endif
#ifdef HAS_HIREDIS
    Timer timer;
    timer.start();
    int64_t deleted = delete_all_lab_keys();
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
    result.rows_affected = deleted;
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

} // namespace dedup
