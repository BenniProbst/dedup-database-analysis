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

    // Select lab database (DB 15) -- NEVER touch DB 0 (production!)
    auto* reply = static_cast<redisReply*>(redisCommand(c, "SELECT %d", LAB_DB));
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        LOG_ERR("[redis] Failed to SELECT DB %d", LAB_DB);
        freeReplyObject(reply);
        redisFree(c);
        ctx_ = nullptr;
        return false;
    }
    freeReplyObject(reply);

    LOG_INF("[redis] Connected to %s:%u, selected DB %d (lab)", conn.host.c_str(), conn.port, LAB_DB);
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
    // Redis DB 15 already selected on connect -- no schema to create
    LOG_INF("[redis] Lab schema = DB %d (already selected)", LAB_DB);
    return true;
}

bool RedisConnector::drop_lab_schema(const std::string&) {
    LOG_WRN("[redis] FLUSHING DB %d (lab data)", LAB_DB);
#ifdef DEDUP_DRY_RUN
    return true;
#endif
#ifdef HAS_HIREDIS
    if (!ctx_) return false;
    auto* reply = static_cast<redisReply*>(
        redisCommand(static_cast<redisContext*>(ctx_), "FLUSHDB"));
    bool ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    return ok;
#else
    return false;
#endif
}

bool RedisConnector::reset_lab_schema(const std::string& s) {
    return drop_lab_schema(s);
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

        std::string key = "dedup:" + entry.path().filename().string();
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
    LOG_INF("[redis] Deleting all lab keys (FLUSHDB on DB %d)", LAB_DB);
#ifdef DEDUP_DRY_RUN
    return result;
#endif
#ifdef HAS_HIREDIS
    Timer timer;
    timer.start();
    auto* reply = static_cast<redisReply*>(
        redisCommand(static_cast<redisContext*>(ctx_), "FLUSHDB"));
    if (reply) freeReplyObject(reply);
    timer.stop();
    result.duration_ns = timer.elapsed_ns();
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
    auto* reply = static_cast<redisReply*>(
        redisCommand(static_cast<redisContext*>(ctx_), "DBSIZE"));
    int64_t count = 0;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        count = reply->integer;
    }
    if (reply) freeReplyObject(reply);
    // Approximate: DBSIZE * average value size
    return count;
#else
    return -1;
#endif
}

} // namespace dedup
