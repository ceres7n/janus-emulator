#include "client_text.hpp"
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

std::string to_client_text(const std::string& utf8)
{
    bool ascii = true;
    for (unsigned char c : utf8) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return utf8;
#ifdef _WIN32
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) return utf8;
    std::vector<wchar_t> w(static_cast<size_t>(wn));
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wn);
    const int n = WideCharToMultiByte(949, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return utf8;
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(949, 0, w.data(), wn, &out[0], n, nullptr, nullptr);
    return out;
#else
    return utf8;
#endif
}

std::string from_client_text(const std::string& cp949)
{
    bool ascii = true;
    for (unsigned char c : cp949) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return cp949;
#ifdef _WIN32
    const int wn = MultiByteToWideChar(949, 0, cp949.data(), static_cast<int>(cp949.size()), nullptr, 0);
    if (wn <= 0) return cp949;
    std::vector<wchar_t> w(static_cast<size_t>(wn));
    MultiByteToWideChar(949, 0, cp949.data(), static_cast<int>(cp949.size()), w.data(), wn);
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return cp949;
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], n, nullptr, nullptr);
    return out;
#else
    return cp949;
#endif
}
