#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <mysql.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Database — thin wrapper around libmysqlclient
//
//  Usage:
//    Database db;
//    db.connect("127.0.0.1", "janus", "pass", "janus_db", 3306);
//
//    auto res = db.query("SELECT uid, name FROM characters WHERE uid = %d", uid);
//    for (auto& row : res) {
//        printf("%s %s\n", row["uid"].c_str(), row["name"].c_str());
//    }
// ─────────────────────────────────────────────────────────────────────────────

using DBRow    = std::unordered_map<std::string, std::string>;
using DBResult = std::vector<DBRow>;

class Database
{
public:
    Database();
    ~Database();

    // Not copyable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool connect(const std::string& host,
                 const std::string& user,
                 const std::string& pass,
                 const std::string& dbname,
                 uint16_t port = 3306);

    void disconnect();
    bool ping();    // reconnect if needed

    // Execute SELECT, returns rows
#if defined(__GNUC__) || defined(__clang__)
    DBResult query(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
    DBResult query(const char* fmt, ...);
#endif

    // Execute INSERT/UPDATE/DELETE, returns affected rows
#if defined(__GNUC__) || defined(__clang__)
    uint64_t execute(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
    uint64_t execute(const char* fmt, ...);
#endif

    // Execute an INSERT and return its AUTO_INCREMENT id, both under the connection lock (another
    // thread's INSERT cannot slip in between, as it can between execute() and last_insert_id()).
#if defined(__GNUC__) || defined(__clang__)
    uint64_t insert(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
    uint64_t insert(const char* fmt, ...);
#endif

    // Last insert ID
    uint64_t last_insert_id() const;

    // Escape a string value for safe insertion
    std::string escape(const std::string& s);

    bool is_connected() const { return mysql_ != nullptr; }

    // Serializes all access to the single MySQL connection (the server hits it from
    // multiple io threads + the autosave timer; libmysql on one MYSQL* is not thread-safe).
    // Recursive: a DBTransaction keeps it across its statements.
    std::recursive_mutex& lock() const { return mutex_; }

private:
    MYSQL* mysql_{ nullptr };
    mutable std::recursive_mutex mutex_;

    DBResult query_internal(const std::string& sql);
    uint64_t execute_internal(const std::string& sql);
};

// ─────────────────────────────────────────────────────────────────────────────
//  DBException
// ─────────────────────────────────────────────────────────────────────────────
class DBException : public std::runtime_error {
public:
    explicit DBException(const std::string& msg) : std::runtime_error(msg) {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  DBTransaction — a stored procedure's BEGIN TRAN ... COMMIT TRAN: holds the connection for its
//  statements (no other thread's statement can land inside it) and rolls back unless commit() ran.
// ─────────────────────────────────────────────────────────────────────────────
class DBTransaction {
public:
    explicit DBTransaction(Database& db) : db_(db), lk_(db.lock()) { db_.execute("START TRANSACTION"); }
    ~DBTransaction()
    {
        if (done_) return;
        try { db_.execute("ROLLBACK"); } catch (const DBException&) {}
    }
    DBTransaction(const DBTransaction&) = delete;
    DBTransaction& operator=(const DBTransaction&) = delete;

    void commit() { db_.execute("COMMIT"); done_ = true; }

private:
    Database& db_;
    std::lock_guard<std::recursive_mutex> lk_;
    bool done_{ false };
};
