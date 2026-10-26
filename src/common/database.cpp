#include "database.hpp"
#include "logger.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// printf into a string of the exact length: a long statement is never cut short and run truncated.
// False when the format itself fails.
bool vformat_sql(std::string& out, const char* fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n < 0) return false;
    out.assign(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(&out[0], out.size(), fmt, args);
    out.resize(static_cast<size_t>(n));
    return true;
}

} // namespace

Database::Database()
{
    mysql_ = mysql_init(nullptr);
    if (!mysql_)
        throw DBException("mysql_init failed");
}

Database::~Database()
{
    disconnect();
}

bool Database::connect(const std::string& host,
                        const std::string& user,
                        const std::string& pass,
                        const std::string& dbname,
                        uint16_t port)
{
    // Enable auto-reconnect
    bool reconnect = true;
    mysql_options(mysql_, MYSQL_OPT_RECONNECT, &reconnect);
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    
    // Disable SSL if connection fails with SSL errors
    enum mysql_ssl_mode ssl_mode = SSL_MODE_DISABLED;
    mysql_options(mysql_, MYSQL_OPT_SSL_MODE, &ssl_mode);

    if (!mysql_real_connect(mysql_,
                            host.c_str(), user.c_str(), pass.c_str(),
                            dbname.c_str(), port,
                            nullptr, 0))
    {
        LOG_ERROR("DB connect failed: %s", mysql_error(mysql_));
        return false;
    }

    LOG_INFO("DB connected to %s:%u/%s", host.c_str(), port, dbname.c_str());
    return true;
}

void Database::disconnect()
{
    if (mysql_) {
        mysql_close(mysql_);
        mysql_ = nullptr;
    }
}

bool Database::ping()
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (!mysql_) return false;
    return mysql_ping(mysql_) == 0;
}

DBResult Database::query(const char* fmt, ...)
{
    std::string sql;
    va_list args;
    va_start(args, fmt);
    const bool ok = vformat_sql(sql, fmt, args);
    va_end(args);
    if (!ok) throw DBException("SQL format error");
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return query_internal(sql);
}

uint64_t Database::execute(const char* fmt, ...)
{
    std::string sql;
    va_list args;
    va_start(args, fmt);
    const bool ok = vformat_sql(sql, fmt, args);
    va_end(args);
    if (!ok) throw DBException("SQL format error");
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return execute_internal(sql);
}

uint64_t Database::insert(const char* fmt, ...)
{
    std::string sql;
    va_list args;
    va_start(args, fmt);
    const bool ok = vformat_sql(sql, fmt, args);
    va_end(args);
    if (!ok) throw DBException("SQL format error");
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    execute_internal(sql);
    return mysql_insert_id(mysql_);
}

uint64_t Database::last_insert_id() const
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return mysql_ ? mysql_insert_id(mysql_) : 0;
}

std::string Database::escape(const std::string& s)
{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::string out(s.size() * 2 + 1, '\0');
    unsigned long len = mysql_real_escape_string(mysql_, out.data(), s.data(), s.size());
    out.resize(len);
    return out;
}

DBResult Database::query_internal(const std::string& sql)
{
    if (mysql_query(mysql_, sql.c_str())) {
        LOG_ERROR("DB query error: %s\nSQL: %s", mysql_error(mysql_), sql.c_str());
        throw DBException(mysql_error(mysql_));
    }

    MYSQL_RES* res = mysql_store_result(mysql_);
    if (!res) {
        if (mysql_field_count(mysql_) == 0)
            return {};  // was a DML statement
        LOG_ERROR("DB store_result error: %s", mysql_error(mysql_));
        throw DBException(mysql_error(mysql_));
    }

    DBResult rows;
    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long* lengths = mysql_fetch_lengths(res);
        DBRow r;
        for (int i = 0; i < num_fields; ++i) {
            r[fields[i].name] = row[i] ? std::string(row[i], lengths[i]) : "";
        }
        rows.push_back(std::move(r));
    }

    mysql_free_result(res);
    return rows;
}

uint64_t Database::execute_internal(const std::string& sql)
{
    if (mysql_query(mysql_, sql.c_str())) {
        LOG_ERROR("DB execute error: %s\nSQL: %s", mysql_error(mysql_), sql.c_str());
        throw DBException(mysql_error(mysql_));
    }
    return mysql_affected_rows(mysql_);
}
