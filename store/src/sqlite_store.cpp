#include "gw/sqlite_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>

namespace gw {
namespace {

// points 的落库格式：[u16 count][count × (u16 addr, u16 value)]，全部大端。
// 与 MQTT 的 RecordCodec 无关（那是 gw_mqtt 的载荷格式，gw_common 保持零依赖）；
// 这里是持久化自己的格式，读写都在本文件内，配套校验。
constexpr std::size_t kPointsHeader = 2;
constexpr std::size_t kPointBytes = 4;
constexpr std::size_t kMaxPoints = 1024;

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::vector<std::uint8_t> encode_points(const std::vector<Point>& pts) {
    std::vector<std::uint8_t> v;
    v.reserve(kPointsHeader + pts.size() * kPointBytes);
    put_u16(v, static_cast<std::uint16_t>(pts.size()));
    for (const Point& p : pts) {
        put_u16(v, p.addr);
        put_u16(v, p.value);
    }
    return v;
}

bool decode_points(const std::uint8_t* p, std::size_t n, std::vector<Point>& out) {
    if (n < kPointsHeader) {
        return false;
    }
    const std::uint16_t count = get_u16(p);
    if (count == 0 || count > kMaxPoints ||
        n != kPointsHeader + static_cast<std::size_t>(count) * kPointBytes) {
        return false;
    }
    out.clear();
    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* q = p + kPointsHeader + static_cast<std::size_t>(i) * kPointBytes;
        out.push_back(Point{get_u16(q), get_u16(q + 2)});
    }
    return true;
}

// 每条 SQL 出错即抛：落库路径上的静默失败比崩溃更危险（会伪装成丢数据）
void check_rc(int rc, sqlite3* db, const char* what) {
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("sqlite ") + what + " failed: " +
                                 (db != nullptr ? sqlite3_errmsg(db) : "no db"));
    }
}

// INSERT/DELETE/UPDATE 的 step 以 SQLITE_DONE 结束（不是 SQLITE_OK）
void step_done(sqlite3_stmt* st, sqlite3* db, const char* what) {
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("sqlite ") + what + " failed: " +
                                 (db != nullptr ? sqlite3_errmsg(db) : "no db"));
    }
}

void bind_u64(sqlite3_stmt* st, int idx, std::uint64_t v) {
    // seq/ts 超出 int64 的场景不存在（毫秒时间戳 / 单调计数），按 int64 绑定即可
    check_rc(sqlite3_bind_int64(st, idx, static_cast<sqlite3_int64>(v)), nullptr, "bind_int64");
}

}  // namespace

// PIMPL：sqlite3.h 与预编译语句全部挡在这里，公开头文件零依赖
struct SqliteRecordStore::Impl {
    explicit Impl(const std::string& db_path, std::size_t cap, std::uint64_t ttl)
        : capacity(cap == 0 ? 1 : cap), ttl_ms(ttl) {
        check_rc(sqlite3_open_v2(db_path.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr),
                 db, "open");
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
        exec("CREATE TABLE IF NOT EXISTS records("
             "  seq    INTEGER PRIMARY KEY,"
             "  ts_ms  INTEGER NOT NULL,"
             "  points BLOB NOT NULL);");
        exec("CREATE TABLE IF NOT EXISTS meta("
             "  key   TEXT PRIMARY KEY,"
             "  value INTEGER NOT NULL);");

        prepare(stmt_insert, "INSERT OR REPLACE INTO records(seq,ts_ms,points) VALUES(?,?,?);");
        prepare(stmt_del_ack, "DELETE FROM records WHERE seq<=?;");
        prepare(stmt_del_expire, "DELETE FROM records WHERE ?1 > ts_ms + ?2;");
        prepare(stmt_front, "SELECT seq,ts_ms,points FROM records ORDER BY seq LIMIT 1;");
        prepare(stmt_meta_set, "INSERT INTO meta(key,value) VALUES(?1,?2) "
                               "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");

        // 恢复：行数、已落库最高 seq、持久化水位线（取两者较大者兜底旧库）
        size_count = scalar("SELECT COUNT(*) FROM records;");
        highest_pushed = scalar("SELECT COALESCE(MAX(seq),0) FROM records;");
        const std::uint64_t wm = meta_get("seq_watermark");
        last_hint = std::max(wm, highest_pushed);
    }

    ~Impl() {
        for (sqlite3_stmt* st : {stmt_insert, stmt_del_ack, stmt_del_expire, stmt_front,
                                 stmt_meta_set}) {
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = (err != nullptr) ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error(std::string("sqlite exec failed: ") + msg);
        }
    }

    void prepare(sqlite3_stmt*& st, const char* sql) {
        check_rc(sqlite3_prepare_v2(db, sql, -1, &st, nullptr), db, "prepare");
    }

    std::uint64_t scalar(const char* sql) {
        sqlite3_stmt* st = nullptr;
        prepare(st, sql);
        const int rc = sqlite3_step(st);
        const std::uint64_t v =
            (rc == SQLITE_ROW) ? static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)) : 0;
        sqlite3_finalize(st);
        return v;
    }

    std::uint64_t meta_get(const char* key) {
        sqlite3_stmt* st = nullptr;
        prepare(st, "SELECT value FROM meta WHERE key=?;");
        check_rc(sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC), db, "bind_text");
        std::uint64_t v = 0;
        if (sqlite3_step(st) == SQLITE_ROW) {
            v = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);
        return v;
    }

    void meta_set(const char* key, std::uint64_t value) {
        check_rc(sqlite3_reset(stmt_meta_set), db, "reset");
        check_rc(sqlite3_clear_bindings(stmt_meta_set), db, "clear");
        check_rc(sqlite3_bind_text(stmt_meta_set, 1, key, -1, SQLITE_STATIC), db, "bind_text");
        bind_u64(stmt_meta_set, 2, value);
        step_done(stmt_meta_set, db, "step");
        check_rc(sqlite3_reset(stmt_meta_set), db, "reset");
    }

    // front 缓存：接口返回 const Record&，落库版把首行物化在内存里
    void ensure_front() const {
        if (!front_dirty) {
            return;
        }
        front_cache = Record{};
        front_valid = false;
        if (size_count > 0) {
            check_rc(sqlite3_reset(stmt_front), db, "reset");
            if (sqlite3_step(stmt_front) == SQLITE_ROW) {
                front_cache.seq =
                    static_cast<std::uint64_t>(sqlite3_column_int64(stmt_front, 0));
                front_cache.ts_ms =
                    static_cast<std::uint64_t>(sqlite3_column_int64(stmt_front, 1));
                const auto* blob =
                    reinterpret_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_front, 2));
                const int bytes = sqlite3_column_bytes(stmt_front, 2);
                if (!decode_points(blob, static_cast<std::size_t>(bytes), front_cache.points)) {
                    throw std::runtime_error("sqlite store: front 行的 points 载荷损坏");
                }
                front_valid = true;
            }
            front_dirty = false;
        }
    }

    sqlite3* db = nullptr;
    sqlite3_stmt* stmt_insert = nullptr;
    sqlite3_stmt* stmt_del_ack = nullptr;
    sqlite3_stmt* stmt_del_expire = nullptr;
    sqlite3_stmt* stmt_front = nullptr;
    sqlite3_stmt* stmt_meta_set = nullptr;

    std::size_t capacity;
    std::uint64_t ttl_ms;

    // 进程本地统计（重启归零）与内存版对齐的观测面
    std::size_t size_count = 0;
    std::uint64_t dropped = 0;
    std::uint64_t expired = 0;
    std::uint64_t acked_count = 0;
    std::uint64_t highest_pushed = 0;
    std::uint64_t last_hint = 0;

    mutable Record front_cache;
    mutable bool front_dirty = true;
    mutable bool front_valid = false;
};

SqliteRecordStore::SqliteRecordStore(const std::string& db_path, std::size_t capacity,
                                     std::uint64_t ttl_ms)
    : impl_(std::make_unique<Impl>(db_path, capacity, ttl_ms)) {}

SqliteRecordStore::~SqliteRecordStore() = default;

bool SqliteRecordStore::push(const Record& rec) {
    if (impl_->size_count >= impl_->capacity) {
        // 容量满丢「最新」：与内存版同款取舍 —— 队列头部 seq 保持连续，
        // 接收端看到的是尾部缺口而不是中间空洞
        ++impl_->dropped;
        return false;
    }
    const std::vector<std::uint8_t> blob = encode_points(rec.points);
    check_rc(sqlite3_reset(impl_->stmt_insert), impl_->db, "reset");
    bind_u64(impl_->stmt_insert, 1, rec.seq);
    bind_u64(impl_->stmt_insert, 2, rec.ts_ms);
    check_rc(sqlite3_bind_blob(impl_->stmt_insert, 3, blob.data(),
                               static_cast<int>(blob.size()), SQLITE_TRANSIENT),
             impl_->db, "bind_blob");
    step_done(impl_->stmt_insert, impl_->db, "step");
    check_rc(sqlite3_reset(impl_->stmt_insert), impl_->db, "reset");

    ++impl_->size_count;
    impl_->highest_pushed = std::max(impl_->highest_pushed, rec.seq);
    impl_->front_dirty = true;
    return true;
}

void SqliteRecordStore::ack(std::uint64_t seq) {
    check_rc(sqlite3_reset(impl_->stmt_del_ack), impl_->db, "reset");
    bind_u64(impl_->stmt_del_ack, 1, seq);
    step_done(impl_->stmt_del_ack, impl_->db, "step");
    const int n = sqlite3_changes(impl_->db);
    check_rc(sqlite3_reset(impl_->stmt_del_ack), impl_->db, "reset");
    if (n > 0) {
        impl_->size_count -= static_cast<std::size_t>(n);
        impl_->acked_count += static_cast<std::uint64_t>(n);
        impl_->front_dirty = true;
    }
}

void SqliteRecordStore::expire(std::uint64_t now_ms) {
    check_rc(sqlite3_reset(impl_->stmt_del_expire), impl_->db, "reset");
    bind_u64(impl_->stmt_del_expire, 1, now_ms);
    bind_u64(impl_->stmt_del_expire, 2, impl_->ttl_ms);
    step_done(impl_->stmt_del_expire, impl_->db, "step");
    const int n = sqlite3_changes(impl_->db);
    check_rc(sqlite3_reset(impl_->stmt_del_expire), impl_->db, "reset");
    if (n > 0) {
        impl_->size_count -= static_cast<std::size_t>(n);
        impl_->expired += static_cast<std::uint64_t>(n);
        impl_->front_dirty = true;
    }
}

std::size_t SqliteRecordStore::size() const {
    return impl_->size_count;
}

bool SqliteRecordStore::empty() const {
    return impl_->size_count == 0;
}

const Record& SqliteRecordStore::front() const {
    impl_->ensure_front();
    return impl_->front_cache;
}

std::uint64_t SqliteRecordStore::dropped() const {
    return impl_->dropped;
}

std::uint64_t SqliteRecordStore::expired() const {
    return impl_->expired;
}

std::uint64_t SqliteRecordStore::highest_seq() const {
    return impl_->highest_pushed;
}

std::uint64_t SqliteRecordStore::lowest_seq() const {
    impl_->ensure_front();
    return impl_->front_valid ? impl_->front_cache.seq : 0;
}

std::uint64_t SqliteRecordStore::acked_count() const {
    return impl_->acked_count;
}

void SqliteRecordStore::note_seq(std::uint64_t seq) {
    // 只前进不回退：note_seq 的调用方（控制器）保证单调递增
    if (seq > impl_->last_hint) {
        impl_->last_hint = seq;
        impl_->meta_set("seq_watermark", seq);
    }
}

std::uint64_t SqliteRecordStore::last_seq_hint() const {
    return impl_->last_hint;
}

}  // namespace gw
