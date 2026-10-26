#pragma once
#include <cstdint>
#include <ctime>

// FILETIMEs (100 ns since 1601-01-01) of the server's LOCAL time: the retail servers turn local SYSTEMTIMEs
// into FILETIMEs with no zone change — the DB's dates (the Master's sub_4620F0) and the GameServer's clock
// (sub_508680: GetLocalTime + SystemTimeToFileTime, the global S2C 508 carries). The client sets them against
// its own local time.
inline uint64_t local_filetime(int y, int mo, int d, int h, int mi, int s)
{
    // the days from 1970-01-01 of the civil date (H. Hinnant's days_from_civil), + 134774 from 1601
    const int64_t yy = static_cast<int64_t>(y) - (mo <= 2 ? 1 : 0);
    const int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned m = static_cast<unsigned>(mo);
    const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468 + 134774;
    const int64_t secs = days * 86400 + h * 3600 + mi * 60 + s;
    return secs < 0 ? 0 : static_cast<uint64_t>(secs) * 10000000ULL;
}

inline uint64_t local_filetime_now()
{
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return local_filetime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}
