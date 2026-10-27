// ProjectPAULA - tools/janus-launcher/janus_launcher.cpp (JanusLauncher.exe, the player-side
// launcher with a window)
//
// Starts the Trickster Online KR 2014 client (Trickster.bin) and points it at a ProjectPAULA
// server. The client takes its login server from a table of 18 IP slots baked into its read-only
// data (VA 0x00DACF00, 16 NUL-padded bytes each; command-line keywords pick the slot, the KR
// default is slot 14) and dials it on the built-in port 9980. The first command-line field is the
// retail web member number, never an address, so the IP has to be changed in the client's memory:
//   1. Every value is checked first; a host name is resolved to IPv4; the build is identified by
//      the MD5 of the file.
//   2. CreateProcessW(CREATE_SUSPENDED) with the client's folder as working directory (the client
//      opens data\... relative to it before it sets its own directory).
//   3. While none of the client's code has run, every copy of each Replace IP inside its main image
//      is rewritten with the server's IPv4 (plan_ip_patch: past the old string only NUL padding is
//      overwritten). The table's only reader runs inside WinMain and every later copy is made from
//      the slot, so there is no race (PaulaLauncher patches 3 s after the start instead). The
//      client has no TLS callbacks and no integrity check (RE 2026-10-27, live-verified).
//   4. Nothing replaced, or a copy that did not fit: the client is terminated without ever running,
//      so it cannot send the account name to the third-party IP baked into it (exit code 4).
//   5. ResumeThread; 3 s later all of the client's memory is scanned once more (expected: 0).
// The core (is_ipv4, is_valid_account, plan_ip_patch, the scanner, the command line, the exit
// codes) is ported from tools/launcher/launcher.cpp (PaulaLauncher). Settings: JanusLauncher.ini
// next to the exe; status lines: the window, JanusLauncher.log and, with --play, the console.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Windows 7, the oldest system in the manifest
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <process.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "resource.h"
#include "janus_launcher_version.h"

namespace {

const wchar_t kIniName[] = L"JanusLauncher.ini";
const wchar_t kLogName[] = L"JanusLauncher.log";

// Defaults (also documented in JanusLauncher.ini and README.md).
const wchar_t kDefaultAddress[] = L"127.0.0.1";
const wchar_t kDefaultClient[]  = L"Trickster.bin";
// The login server IP baked into each known build (all 18 slots of the IP table hold it):
//   51.38.50.68      Client_Janus build (the client folder the owner shares with players)
//   217.160.132.225  reference build (pristine Client_live)
//   211.44.251.24    KR January 2014 client (packed, so it is not found in the suspended image
//                    and the launcher refuses it: not supported)
const wchar_t kDefaultReplaceIPs[] = L"51.38.50.68,217.160.132.225,211.44.251.24";
// The login arguments after the account, live-tested with PaulaLauncher: session key "0" (the
// client also sends it as the password text; ProjectPAULA ignores both), game id, age "0" (only
// the Korean youth curfew reads it, and ProjectPAULA never sends that packet), extras.
const wchar_t kDefaultKey[]    = L"0";
const wchar_t kDefaultAge[]    = L"0";
const wchar_t kDefaultExtras[] = L"blankblank0|0";
const char    kGameId[]        = "Trickster";   // strcmp in the client: exact, case-sensitive

const size_t kMaxIpLen       = 15;    // "255.255.255.255"
const size_t kMaxAccountLen  = 18;    // the 19-byte username slot of the login packet 0x2CED
const size_t kMaxKeyLen      = 18;    // the 19-byte password slot (the key is sent as its text)
const size_t kMaxAgeLen      = 3;
const size_t kMaxExtrasLen   = 200;   // the client copies it unbounded into a 256-byte buffer
const size_t kMaxExtrasWebId = 127;   // sub-field [1]: sprintf into a 128-byte buffer
const size_t kMaxHostLen     = 253;
const size_t kMaxReplaceLen  = 255;
const size_t kMaxClientLen   = 1024;
const int    kMaxPatterns    = 8;
const UINT   kClientPort     = 9980;  // an immediate in the client (sub_401050), not data
const DWORD  kRescanDelayMs  = 3000;
const DWORD  kKillWaitMs     = 5000;

// The login IP table of both known builds: 18 slots x 16 bytes at RVA 0x9ACF00.
const ULONG_PTR kIpTableRva  = 0x9ACF00;
const int       kIpTableSlots = 18;
const size_t    kIpSlotSize  = 16;

// Exit codes, as PaulaLauncher.
const int kExitOk          = 0;
const int kExitInvalid     = 1;   // a setting is not valid
const int kExitNoClient    = 2;   // the client file was not found
const int kExitStartFailed = 3;   // the client could not start, or it closed at once
const int kExitNotPatched  = 4;   // the server IP could not be put in: the client never ran

const UINT WM_APP_LINE     = WM_APP + 1;   // lParam: new std::wstring, one or more status lines
const UINT WM_APP_DONE     = WM_APP + 2;   // wParam: the exit code of a start
const UINT WM_APP_RELAYOUT = WM_APP + 3;   // after a DPI change

struct KnownBuild {
    const char*    md5;
    const wchar_t* name;
    int            max_clients;   // the byte at 0x127380E: MYOWNTO_CLIENT_1.._N mutexes
};

const KnownBuild kKnownBuilds[] = {
    {"18e38cfd42abbaa19ccf37d8f518e426", L"Client_Janus build", 5},
    {"46ee494cb3042cdf93c1f9b42f1e16a4", L"reference build", 3},
};

// Words the client looks for in each command-line field (case-insensitive). One of them as an
// account name would switch the client to another service or server slot.
const char* const kClientKeywords[] = {
    "loadinifromdb", "gsp", "taiwanese", "chinese", "japanese", "thailand",
    "ts", "testserver", "devsite", "inter", "devserver",
};

struct Settings {
    std::wstring address     = kDefaultAddress;
    std::wstring account;
    std::wstring client      = kDefaultClient;
    std::wstring replace_ips = kDefaultReplaceIPs;
    std::wstring key         = kDefaultKey;
    std::wstring age         = kDefaultAge;
    std::wstring extras      = kDefaultExtras;
    bool close_on_start      = false;
};

struct Pattern {
    char   ip[kMaxIpLen + 1];
    size_t len;
};

// The values of Settings in the form the client gets them (validated ASCII).
struct Checked {
    std::string account, key, age, extras;
    std::vector<Pattern> pats;
};

struct PatchStats {
    int patched = 0;   // copies rewritten with the server IP
    int already = 0;   // copies that already hold it
    int skipped = 0;   // copies left unchanged: the IP does not fit there, or the write failed
    int       pat_patched[kMaxPatterns] = {};
    ULONG_PTR pat_first[kMaxPatterns] = {};
    ULONG_PTR pat_last[kMaxPatterns] = {};
};

enum class Hit { NotThisIp, Patched, Already, Skipped };

struct PeInfo {
    WORD  machine = 0;
    WORD  magic = 0;
    DWORD timestamp = 0;
    DWORD image_size = 0;
};

// ---- status output: the window, the log file and (--play, --selftest, --help) the console ----

struct Output {
    CRITICAL_SECTION lock;
    HANDLE       log = INVALID_HANDLE_VALUE;
    std::wstring log_path;
    HANDLE       out = nullptr;     // console, or stdout redirected to a file or pipe
    bool         out_is_console = false;
    HWND         window = nullptr;  // the dialog, once it exists
    std::wstring early;             // lines written before the dialog existed
};
Output g_out;

std::string to_utf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

// One or more lines ('\n' between them). Called from the UI thread and the start thread.
void emit(const std::wstring& text)
{
    std::wstring crlf;
    crlf.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        if (ch == L'\n')
            crlf += L"\r\n";
        else
            crlf += ch;
    }
    crlf += L"\r\n";
    EnterCriticalSection(&g_out.lock);
    if (g_out.log != INVALID_HANDLE_VALUE) {
        const std::string u = to_utf8(crlf);
        DWORD written = 0;
        WriteFile(g_out.log, u.data(), static_cast<DWORD>(u.size()), &written, nullptr);
    }
    if (g_out.out) {
        DWORD written = 0;
        if (g_out.out_is_console) {
            WriteConsoleW(g_out.out, crlf.data(), static_cast<DWORD>(crlf.size()), &written, nullptr);
        } else {
            const std::string u = to_utf8(crlf);
            WriteFile(g_out.out, u.data(), static_cast<DWORD>(u.size()), &written, nullptr);
        }
    }
    if (g_out.window) {
        std::wstring* line = new std::wstring(crlf);
        if (!PostMessageW(g_out.window, WM_APP_LINE, 0, reinterpret_cast<LPARAM>(line)))
            delete line;
    } else {
        g_out.early += crlf;
    }
    LeaveCriticalSection(&g_out.lock);
}

void say(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    emit(buf);
}

// Started from a console (--play, --selftest, --help): write there too. A GUI program gets no
// console of its own, so it attaches to its parent's; output redirected to a file or a pipe is
// used as it is.
void attach_output()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        g_out.out = h;
        g_out.out_is_console = false;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (con != INVALID_HANDLE_VALUE) {
            g_out.out = con;
            g_out.out_is_console = true;
            emit(L"");   // the console prompt is already printed: start on a new line
        }
    }
}

// A new log file at path; the previous one is kept as <path>.1, so reopening the launcher after
// a failed start does not lose the log of that failure.
HANDLE create_log(const std::wstring& path)
{
    MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// JanusLauncher.log next to the exe, a new one at each start of the launcher (the last one is
// JanusLauncher.log.1). A read-only folder (or a second launcher holding the file) moves it to %TEMP%.
void open_log(const std::wstring& exe_dir)
{
    std::wstring path = exe_dir + kLogName;
    HANDLE h = create_log(path);
    if (h == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n > 0 && n <= MAX_PATH) {
            path = std::wstring(tmp) + kLogName;
            h = create_log(path);
        }
    }
    if (h == INVALID_HANDLE_VALUE)
        return;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    WriteFile(h, bom, sizeof(bom), &written, nullptr);
    g_out.log = h;
    g_out.log_path = path;
}

// ---- small helpers ----

std::wstring trim(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t'))
        ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t'))
        --e;
    return s.substr(b, e - b);
}

std::wstring widen(const std::string& ascii)
{
    std::wstring w;
    w.reserve(ascii.size());
    for (char c : ascii)
        w += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return w;
}

// The client's arguments are ASCII: anything else is refused before it can reach the client.
bool to_ascii(const std::wstring& w, std::string& out)
{
    out.clear();
    for (wchar_t c : w) {
        if (c < 0x20 || c > 0x7E)
            return false;
        out += static_cast<char>(c);
    }
    return true;
}

std::wstring win_error(DWORD err)
{
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                                   reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring msg = (n > 0 && text) ? std::wstring(text, n) : std::wstring();
    if (text)
        LocalFree(text);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ' || msg.back() == L'.'))
        msg.pop_back();
    wchar_t head[40];
    swprintf_s(head, L"Windows error %lu", err);
    return msg.empty() ? std::wstring(head) : std::wstring(head) + L": " + msg;
}

// ---- validation (pure) ----

bool is_ip_char(char c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

// Dotted-quad IPv4, digits and dots only, at most 15 characters.
bool is_ipv4(const char* s)
{
    if (strlen(s) > kMaxIpLen)
        return false;
    int parts = 0;
    for (;;) {
        int digits = 0, value = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (*s - '0');
            if (++digits > 3)
                return false;
            ++s;
        }
        if (digits == 0 || value > 255)
            return false;
        ++parts;
        if (*s == '\0')
            return parts == 4;
        if (*s != '.' || parts == 4)
            return false;
        ++s;
    }
}

// The client converts the address with inet_addr, which reads "010" as octal 8: the address it
// gets is always written without leading zeros.
bool canonical_ipv4(const char* s, std::string& out)
{
    if (!is_ipv4(s))
        return false;
    unsigned v[4] = {};
    int k = 0;
    for (const char* p = s; *p; ++p) {
        if (*p == '.')
            ++k;
        else
            v[k] = v[k] * 10 + static_cast<unsigned>(*p - '0');
    }
    char buf[kMaxIpLen + 1];
    sprintf_s(buf, "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    out = buf;
    return true;
}

bool only_digits_and_dots(const std::string& s)
{
    for (char c : s)
        if (!is_ip_char(c))
            return false;
    return true;
}

// Letters, digits and '-' in labels of 1-63 characters separated by dots (a final dot is allowed).
bool is_host_name(const std::string& s)
{
    if (s.empty() || s.size() > kMaxHostLen)
        return false;
    size_t label = 0;
    for (char c : s) {
        if (c == '.') {
            if (label == 0)
                return false;
            label = 0;
            continue;
        }
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok || ++label > 63)
            return false;
    }
    return true;
}

// Same rule as tools/add-account.bat and PaulaLauncher: 1-18 characters from A-Z a-z 0-9 _. (The
// name is also a field of the comma-separated client command line and part of a registry key.)
bool is_valid_account(const char* s)
{
    const size_t n = strlen(s);
    if (n == 0 || n > kMaxAccountLen)
        return false;
    for (; *s; ++s) {
        const char c = *s;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

bool is_client_keyword(const std::string& s)
{
    for (const char* k : kClientKeywords)
        if (_stricmp(s.c_str(), k) == 0)
            return true;
    return false;
}

bool all_digits(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

// Each check returns "" when the value is fine, otherwise what the player has to fix.
std::wstring check_account(const std::wstring& typed, std::string& account)
{
    const std::wstring t = trim(typed);
    if (t.empty())
        return L"Type your account name (the server operator created it for you).";
    if (!to_ascii(t, account) || !is_valid_account(account.c_str()))
        return L"The account name \"" + t + L"\" is not valid: 1-18 characters, letters A-Z a-z, digits and _ only.";
    if (is_client_keyword(account))
        return L"\"" + t + L"\" cannot be an account name: the client reads it as one of its command-line "
               L"keywords. Ask the server operator for another name.";
    return std::wstring();
}

std::wstring check_key(const std::wstring& typed, std::string& key)
{
    if (!to_ascii(trim(typed), key) || !all_digits(key) || key.size() > kMaxKeyLen)
        return L"Session key: 1-18 digits (default 0). It is not a password; the server ignores it.";
    return std::wstring();
}

std::wstring check_age(const std::wstring& typed, std::string& age)
{
    if (!to_ascii(trim(typed), age) || !all_digits(age) || age.size() > kMaxAgeLen || atoi(age.c_str()) > 255)
        return L"Age: a number from 0 to 255 (default 0).";
    return std::wstring();
}

// The last field: '|'-separated values the client splits with strtok (empty ones are skipped).
// [0] is only tested for the prefix "devsit" (QA mode, which then wants Game ID TricksterTest);
// [1] goes into the cash-shop web parameters (128-byte buffer); [3], when not 0, is an hour at
// which the client warns and closes itself.
std::wstring check_extras(const std::wstring& typed, std::string& extras)
{
    const std::wstring t = trim(typed);
    if (t.empty())
        return L"Extras cannot be empty (default blankblank0|0).";
    if (t.size() > kMaxExtrasLen)
        return L"Extras: at most 200 characters (default blankblank0|0).";
    bool ok = to_ascii(t, extras);
    for (size_t i = 0; ok && i < extras.size(); ++i) {
        const char c = extras[i];
        ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             c == '_' || c == '.' || c == '-' || c == '|';
    }
    if (!ok)
        return L"Extras: only letters, digits and _ . - | are allowed (no spaces, quotes or commas).";
    if (_strnicmp(extras.c_str(), "devsit", 6) == 0)
        return L"Extras must not start with \"devsit\": the client would switch to its test mode and refuse to start.";
    if (is_client_keyword(extras))
        return L"Extras cannot be one of the client's command-line keywords (default blankblank0|0).";
    std::vector<std::string> sub;
    size_t start = 0;
    while (start <= extras.size()) {
        size_t bar = extras.find('|', start);
        if (bar == std::string::npos)
            bar = extras.size();
        if (bar > start)
            sub.push_back(extras.substr(start, bar - start));
        start = bar + 1;
    }
    if (sub.size() > 1 && sub[1].size() > kMaxExtrasWebId)
        return L"Extras: the second |-separated value must be shorter than 128 characters.";
    if (sub.size() > 3) {
        errno = 0;
        const long hour = strtol(sub[3].c_str(), nullptr, 10);
        if (hour != 0 || errno == ERANGE)
            return L"Extras: the fourth |-separated value must be 0 or left out (otherwise the client closes "
                   L"itself at that hour).";
    }
    return std::wstring();
}

// Splits the comma-separated Replace IPs list. Each entry is matched literally in memory.
std::wstring parse_replace_ips(const std::wstring& list, std::vector<Pattern>& out)
{
    out.clear();
    std::string a;
    if (!to_ascii(list, a))
        return L"Replace IPs: IPv4 addresses separated by commas (default " + std::wstring(kDefaultReplaceIPs) + L").";
    size_t start = 0;
    while (start <= a.size()) {
        size_t comma = a.find(',', start);
        if (comma == std::string::npos)
            comma = a.size();
        std::string tok = a.substr(start, comma - start);
        start = comma + 1;
        const size_t b = tok.find_first_not_of(" \t");
        const size_t e = tok.find_last_not_of(" \t");
        tok = (b == std::string::npos) ? std::string() : tok.substr(b, e - b + 1);
        if (tok.empty())
            continue;
        if (!is_ipv4(tok.c_str()))
            return L"Replace IPs: \"" + widen(tok) + L"\" is not an IPv4 address.";
        bool duplicate = false;
        for (const Pattern& p : out)
            duplicate = duplicate || tok == p.ip;
        if (duplicate)
            continue;
        if (out.size() == static_cast<size_t>(kMaxPatterns))
            return L"Replace IPs: at most 8 addresses.";
        Pattern p = {};
        strcpy_s(p.ip, tok.c_str());
        p.len = tok.size();
        out.push_back(p);
    }
    if (out.empty())
        return L"Replace IPs is empty (default " + std::wstring(kDefaultReplaceIPs) + L").";
    return std::wstring();
}

// Everything except the server address (which may need a DNS lookup).
std::wstring check_values(const Settings& s, Checked& c)
{
    std::wstring problem = check_account(s.account, c.account);
    if (problem.empty())
        problem = check_key(s.key, c.key);
    if (problem.empty())
        problem = check_age(s.age, c.age);
    if (problem.empty())
        problem = check_extras(s.extras, c.extras);
    if (problem.empty())
        problem = parse_replace_ips(s.replace_ips, c.pats);
    return problem;
}

// What the player typed as server address: an IPv4 address (ip is set) or a host name (host is
// set, resolved later by lookup_ipv4).
std::wstring parse_address(const std::wstring& typed, std::string& ip, std::string& host)
{
    ip.clear();
    host.clear();
    const std::wstring t = trim(typed);
    if (t.empty())
        return L"Type the server address (127.0.0.1 is a server on this PC).";
    const std::wstring bad = L"The server address \"" + t + L"\" is not valid: use an IPv4 address such as "
                             L"127.0.0.1, or a host name (letters, digits, - and .).";
    std::string a;
    if (!to_ascii(t, a))
        return bad;
    if (only_digits_and_dots(a)) {
        if (!canonical_ipv4(a.c_str(), ip))
            return L"The server address \"" + t + L"\" is not a valid IPv4 address: four numbers 0-255 "
                   L"separated by dots, such as 127.0.0.1.";
        if (ip == "0.0.0.0") {
            ip.clear();
            return L"0.0.0.0 is not a server address (a server listens on it). Use 127.0.0.1 for a server "
                   L"on this PC, or the address the server operator gave you.";
        }
        return std::wstring();
    }
    if (!is_host_name(a))
        return bad;
    host = a;
    return std::wstring();
}

// DNS lookup of a host name (IPv4 only: the client's slots hold a dotted IPv4). Blocks, so it
// runs on the start thread, never on the UI thread.
std::wstring lookup_ipv4(const std::string& host, std::string& ip)
{
    const std::wstring whost = widen(host);
    WSADATA wsa;
    const int started = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (started != 0)
        return L"Cannot look up \"" + whost + L"\" (" + win_error(static_cast<DWORD>(started)) + L").";
    ADDRINFOW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    const int rc = GetAddrInfoW(whost.c_str(), nullptr, &hints, &res);
    std::wstring problem;
    if (rc != 0 || !res || !res->ai_addr) {
        problem = L"Cannot find the server \"" + whost + L"\" (" + win_error(static_cast<DWORD>(rc)) +
                  L"). Check the name and the internet connection, or ask the server operator for the "
                  L"server's IPv4 address.";
    } else {
        const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&sin->sin_addr);
        char buf[kMaxIpLen + 1];
        sprintf_s(buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        ip = buf;
    }
    if (res)
        FreeAddrInfoW(res);
    WSACleanup();
    return problem;
}

// ---- command line and paths (pure) ----

// "<ip>,<account>,<key>,Trickster,<age>,<extras>": the retail web launcher's format, as
// PaulaLauncher sends it ("127.0.0.1,acc,0,Trickster,0,blankblank0|0", live-tested). Field 0 is
// the web member number to the client; the server IP goes there only for the record.
std::string build_client_args(const std::string& ip, const Checked& c)
{
    return ip + "," + c.account + "," + c.key + "," + kGameId + "," + c.age + "," + c.extras;
}

std::wstring build_command_line(const std::wstring& client_path, const std::string& args)
{
    return L"\"" + client_path + L"\" \"" + widen(args) + L"\"";
}

bool is_relative_path(const std::wstring& p)
{
    if (p.empty())
        return true;
    if (p[0] == L'\\' || p[0] == L'/')
        return false;
    return !(p.size() >= 2 && p[1] == L':' && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')));
}

// A relative client path is relative to the launcher's folder (not to the current directory).
std::wstring full_client_path(const std::wstring& setting, const std::wstring& exe_dir)
{
    const std::wstring t = trim(setting);
    if (t.empty())
        return std::wstring();
    const std::wstring joined = is_relative_path(t) ? exe_dir + t : t;
    const DWORD need = GetFullPathNameW(joined.c_str(), 0, nullptr, nullptr);
    if (need == 0)
        return std::wstring();
    std::vector<wchar_t> buf(need + 1);
    const DWORD n = GetFullPathNameW(joined.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n == 0 || n >= buf.size())
        return std::wstring();
    return std::wstring(buf.data(), n);
}

std::wstring folder_of(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return std::wstring();
    if (slash == 2 && path.size() > 1 && path[1] == L':')
        return path.substr(0, 3);   // keep "C:\" for a drive root
    return path.substr(0, slash);
}

std::wstring launcher_folder()
{
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    const std::wstring path(buf.data(), (n > 0 && n < buf.size()) ? n : 0);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

// ---- JanusLauncher.ini ----
// The profile functions read both ANSI and UTF-16LE (with BOM) files and write in the file's own
// encoding. A new file is created as UTF-16, and an ANSI file is converted to UTF-16 (comments
// kept) before a value is saved that the ANSI code page cannot hold, such as a Korean folder name
// on a Western Windows.

std::wstring ini_read(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
{
    std::vector<wchar_t> buf(4096);
    GetPrivateProfileStringW(section, key, L"", buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    return trim(buf.data());
}

bool ini_has_key(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
{
    wchar_t buf[8];
    GetPrivateProfileStringW(section, key, L"\x01", buf, _countof(buf), ini.c_str());
    return !(buf[0] == 1 && buf[1] == 0);
}

void load_settings(const std::wstring& ini, Settings& s)
{
    auto take = [&](const wchar_t* section, const wchar_t* key, std::wstring& field) {
        const std::wstring v = ini_read(ini, section, key);
        if (!v.empty())   // a missing or empty key keeps its default
            field = v;
    };
    take(L"Server", L"Address", s.address);
    take(L"Server", L"Account", s.account);
    take(L"Launcher", L"ClientPath", s.client);
    take(L"Launcher", L"ReplaceIPs", s.replace_ips);
    take(L"Launcher", L"SessionKey", s.key);
    take(L"Launcher", L"Age", s.age);
    take(L"Launcher", L"Extras", s.extras);
    const std::wstring close = ini_read(ini, L"Launcher", L"CloseOnStart");
    if (!close.empty())
        s.close_on_start = close == L"1";
}

bool fits_ansi(const std::wstring& w)
{
    if (GetACP() == CP_UTF8)
        return true;
    BOOL lossy = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &lossy);
    return lossy == FALSE;
}

// Returns 0, or the Windows error.
DWORD make_ini_unicode_if_needed(const std::wstring& ini, bool needed)
{
    std::string bytes;
    HANDLE f = CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const bool exists = f != INVALID_HANDLE_VALUE;
    if (!exists) {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND)
            return err;
    } else {
        char buf[16384];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
            bytes.append(buf, got);
        CloseHandle(f);
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
            return 0;   // already UTF-16
        if (!needed)
            return 0;   // ANSI, and every value fits
    }
    std::wstring text;
    if (!bytes.empty()) {
        const int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (n > 0) {
            text.resize(static_cast<size_t>(n));
            MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], n);
        }
    }
    HANDLE w = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE)
        return GetLastError();
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    BOOL ok = WriteFile(w, &bom, sizeof(bom), &written, nullptr);
    if (ok && !text.empty())
        ok = WriteFile(w, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(w);
    return err;
}

// Returns "" or why the settings could not be saved.
std::wstring save_settings(const std::wstring& ini, const Settings& s)
{
    struct Item {
        const wchar_t* section;
        const wchar_t* key;
        std::wstring   value;
        const wchar_t* def;   // nullptr: always written
    };
    const Item items[] = {
        {L"Server", L"Address", trim(s.address), nullptr},
        {L"Server", L"Account", trim(s.account), nullptr},
        {L"Launcher", L"ClientPath", trim(s.client), kDefaultClient},
        {L"Launcher", L"ReplaceIPs", trim(s.replace_ips), kDefaultReplaceIPs},
        {L"Launcher", L"SessionKey", trim(s.key), kDefaultKey},
        {L"Launcher", L"Age", trim(s.age), kDefaultAge},
        {L"Launcher", L"Extras", trim(s.extras), kDefaultExtras},
        {L"Launcher", L"CloseOnStart", s.close_on_start ? L"1" : L"0", L"0"},
    };
    bool all_fit = true;
    for (const Item& it : items)
        all_fit = all_fit && fits_ansi(it.value);
    const DWORD err = make_ini_unicode_if_needed(ini, !all_fit);
    if (err != 0)
        return win_error(err);
    for (const Item& it : items) {
        // A default value is only written over a key that is already there, so that a later
        // launcher version can change the default.
        if (it.def && it.value == it.def && !ini_has_key(ini, it.section, it.key))
            continue;
        if (!WritePrivateProfileStringW(it.section, it.key, it.value.c_str(), ini.c_str()))
            return win_error(GetLastError());
    }
    return std::wstring();
}

// ---- client file: PE header and MD5 ----

bool parse_pe_header(const unsigned char* buf, size_t len, PeInfo& pe)
{
    if (len < 0x40 || buf[0] != 'M' || buf[1] != 'Z')
        return false;
    LONG lfanew = 0;
    memcpy(&lfanew, buf + 0x3C, sizeof(lfanew));
    if (lfanew < 0x40 || static_cast<size_t>(lfanew) + sizeof(IMAGE_NT_HEADERS32) > len)
        return false;
    IMAGE_NT_HEADERS32 nt;
    memcpy(&nt, buf + lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    pe.machine = nt.FileHeader.Machine;
    pe.timestamp = nt.FileHeader.TimeDateStamp;
    pe.magic = nt.OptionalHeader.Magic;   // same offset in PE32 and PE32+
    pe.image_size = nt.OptionalHeader.SizeOfImage;
    return true;
}

bool read_file_pe(const std::wstring& path, PeInfo& pe)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    unsigned char head[4096];
    DWORD got = 0;
    const BOOL ok = ReadFile(f, head, sizeof(head), &got, nullptr);
    CloseHandle(f);
    return ok != FALSE && parse_pe_header(head, got, pe);
}

bool file_md5(const std::wstring& path, std::string& hex, DWORD& err)
{
    hex.clear();
    err = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buf(256 * 1024);
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            err = GetLastError();
            ok = false;
        } else if (got == 0) {
            break;
        } else {
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
        }
    }
    unsigned char digest[16] = {};
    if (ok)
        ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) {
        if (err == 0)
            err = ERROR_GEN_FAILURE;
        return false;
    }
    char out[33];
    for (int i = 0; i < 16; ++i)
        sprintf_s(out + 2 * i, 3, "%02x", digest[i]);
    hex = out;
    return true;
}

const KnownBuild* find_build(const std::string& md5)
{
    for (const KnownBuild& b : kKnownBuilds)
        if (md5 == b.md5)
            return &b;
    return nullptr;
}

// ---- the client's memory ----

// How one copy of the old IP is rewritten. old_len = length of the old IP; after = the bytes
// that follow it in memory (after_len of them could be read). The new IP is written with its
// NUL and, when it is shorter, NULs over the rest of the old string: max(new_len + 1, old_len)
// bytes from the start of the copy. Bytes past the old string are only overwritten when they
// already are NUL padding (the 16-byte slots of the client's IP table), so a longer IP never runs
// into the next value. Returns the number of bytes to write (out holds them), or 0 when the new
// IP does not fit there.
size_t plan_ip_patch(size_t old_len, const char* new_ip, const unsigned char* after, size_t after_len,
                     char* out, size_t out_size)
{
    const size_t new_len = strlen(new_ip);
    const size_t write_len = (new_len + 1 > old_len) ? new_len + 1 : old_len;
    if (write_len > out_size)
        return 0;
    const size_t past_old = write_len - old_len;   // bytes written after the old string
    if (past_old > after_len)
        return 0;
    for (size_t i = 0; i < past_old; ++i)
        if (after[i] != 0)
            return 0;
    memset(out, 0, write_len);
    memcpy(out, new_ip, new_len);
    return write_len;
}

bool is_scannable(DWORD prot)
{
    // Write-copy is how a suspended client's writable sections look before their first write.
    // Guard, no-cache and write-combine pages are left alone.
    switch (prot) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// One occurrence of pat at addr in the client; prot = the protection of its memory region.
Hit patch_hit(HANDLE proc, ULONG_PTR addr, DWORD prot, const Pattern& pat, const char* new_ip)
{
    // The bytes after the old string, up to the first unreadable one.
    unsigned char after[kMaxIpLen + 1] = {};
    size_t after_len = 0;
    while (after_len < sizeof(after)) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr + pat.len + after_len), &after[after_len], 1, &got) ||
            got != 1)
            break;
        ++after_len;
    }
    if (after_len > 0 && is_ip_char(static_cast<char>(after[0])))
        return Hit::NotThisIp;   // the start of a longer number, e.g. "51.38.50.681"
    if (strcmp(pat.ip, new_ip) == 0)
        return Hit::Already;

    char bytes[kMaxIpLen + 1];
    const size_t n = plan_ip_patch(pat.len, new_ip, after, after_len, bytes, sizeof(bytes));
    if (n == 0) {
        say(L"ERROR: %hs does not fit in the client's copy of %hs at %p: it needs %u NUL bytes after the\n"
            L"       old address and fewer are there. That copy was left unchanged.",
            new_ip, pat.ip, reinterpret_cast<void*>(addr), static_cast<unsigned>(strlen(new_ip) + 1 - pat.len));
        return Hit::Skipped;
    }
    const bool exec = prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    LPVOID where = reinterpret_cast<LPVOID>(addr);
    DWORD old_prot = 0;
    const BOOL reprotected = VirtualProtectEx(proc, where, n, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old_prot);
    SIZE_T written = 0;
    const BOOL ok = WriteProcessMemory(proc, where, bytes, n, &written);
    const DWORD err = GetLastError();
    if (reprotected)
        VirtualProtectEx(proc, where, n, old_prot, &old_prot);
    if (!ok || written != n) {
        say(L"ERROR: could not write the server IP at %p (%ls). That copy was left unchanged.",
            reinterpret_cast<void*>(addr), win_error(err).c_str());
        return Hit::Skipped;
    }
    return Hit::Patched;
}

// Scans the committed, readable memory of the client between lo and hi for the old IPs and
// rewrites every copy. Never touches any process but proc.
void scan_and_patch(HANDLE proc, ULONG_PTR lo, ULONG_PTR hi, const std::vector<Pattern>& pats, const char* new_ip,
                    PatchStats& st)
{
    size_t maxlen = 0;
    for (const Pattern& p : pats)
        if (p.len > maxlen)
            maxlen = p.len;
    if (maxlen == 0)
        return;
    std::vector<char> buf(64 * 1024);

    ULONG_PTR region = lo;
    MEMORY_BASIC_INFORMATION mbi;
    while (region < hi && VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(region), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const ULONG_PTR rbase = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        ULONG_PTR end = rbase + mbi.RegionSize;
        if (end <= region)
            break;   // wrapped around the top of the address space
        if (end > hi)
            end = hi;
        if (mbi.State == MEM_COMMIT && is_scannable(mbi.Protect)) {
            char prev = 0;   // the byte before buf[0] (none at the start of a region)
            ULONG_PTR p = region > rbase ? region : rbase;
            while (p < end) {
                size_t want = static_cast<size_t>(end - p);
                if (want > buf.size())
                    want = buf.size();
                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(p), buf.data(), want, &got) && got == 0)
                    break;   // no longer readable: next region
                const bool last = p + got >= end || got < want || got < maxlen;
                // Each start position is examined once: the next read starts maxlen-1 bytes back,
                // so a copy that straddles two reads is still found.
                const size_t examine = last ? got : got - (maxlen - 1);
                for (size_t i = 0; i < examine; ++i) {
                    if (is_ip_char(i ? buf[i - 1] : prev))
                        continue;   // the tail of a longer number, e.g. "151.38.50.68"
                    for (size_t k = 0; k < pats.size(); ++k) {
                        if (i + pats[k].len > got || memcmp(buf.data() + i, pats[k].ip, pats[k].len) != 0)
                            continue;
                        const ULONG_PTR at = p + i;
                        const Hit hit = patch_hit(proc, at, mbi.Protect, pats[k], new_ip);
                        if (hit == Hit::NotThisIp)
                            continue;
                        if (hit == Hit::Patched) {
                            ++st.patched;
                            if (st.pat_patched[k]++ == 0)
                                st.pat_first[k] = at;
                            st.pat_last[k] = at;
                        } else if (hit == Hit::Already) {
                            ++st.already;
                        } else {
                            ++st.skipped;
                        }
                        break;
                    }
                }
                if (last)
                    break;
                prev = buf[examine - 1];
                p += examine;
            }
        }
        region = end;
    }
}

void report_patch(const std::vector<Pattern>& pats, const PatchStats& st, const std::string& ip)
{
    for (size_t k = 0; k < pats.size(); ++k)
        if (st.pat_patched[k] > 0)
            say(L"  %hs -> %hs: %d %ls (%p..%p)", pats[k].ip, ip.c_str(), st.pat_patched[k],
                st.pat_patched[k] == 1 ? L"copy" : L"copies", reinterpret_cast<void*>(st.pat_first[k]),
                reinterpret_cast<void*>(st.pat_last[k]));
}

bool image_matches(HANDLE proc, ULONG_PTR base, const PeInfo& file_pe)
{
    unsigned char head[4096];
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(base), head, sizeof(head), &got) && got == 0)
        return false;
    PeInfo mem;
    return parse_pe_header(head, got, mem) && mem.machine == file_pe.machine && mem.magic == file_pe.magic &&
           mem.timestamp == file_pe.timestamp && mem.image_size == file_pe.image_size;
}

// PEB.ImageBaseAddress: the kernel sets it when it maps the exe, before any of its code runs.
bool read_peb_image_base(HANDLE proc, ULONG_PTR& base)
{
    struct BasicInformation {
        LONG      exit_status;
        PVOID     peb;
        ULONG_PTR affinity_mask;
        LONG      base_priority;
        ULONG_PTR process_id;
        ULONG_PTR parent_process_id;
    };
    using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const QueryFn query = ntdll ? reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
    if (!query)
        return false;
    BasicInformation info = {};
    ULONG len = 0;
    if (query(proc, 0 /* ProcessBasicInformation */, &info, sizeof(info), &len) < 0 || !info.peb)
        return false;
    const size_t image_base_offset = sizeof(void*) * 2;   // PEB: 4 flag bytes, Mutant, ImageBaseAddress
    PVOID value = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, static_cast<const char*>(info.peb) + image_base_offset, &value, sizeof(value), &got) ||
        got != sizeof(value) || !value)
        return false;
    base = reinterpret_cast<ULONG_PTR>(value);
    return true;
}

void full_address_range(ULONG_PTR& lo, ULONG_PTR& hi)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    lo = reinterpret_cast<ULONG_PTR>(si.lpMinimumApplicationAddress);
    hi = reinterpret_cast<ULONG_PTR>(si.lpMaximumApplicationAddress);
}

// The client's main image, never assumed to be at 0x400000: the PEB's image base when its PE
// header matches the file, else the MEM_IMAGE allocation whose header matches.
bool find_client_image(HANDLE proc, const PeInfo& file_pe, ULONG_PTR& base, const wchar_t*& how)
{
    ULONG_PTR peb_base = 0;
    if (read_peb_image_base(proc, peb_base) && image_matches(proc, peb_base, file_pe)) {
        base = peb_base;
        how = L"PEB";
        return true;
    }
    how = L"memory map";
    ULONG_PTR region = 0, top = 0;
    full_address_range(region, top);
    MEMORY_BASIC_INFORMATION mbi;
    while (region < top && VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(region), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const ULONG_PTR rbase = reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        const ULONG_PTR end = rbase + mbi.RegionSize;
        if (end <= region)
            break;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && mbi.BaseAddress == mbi.AllocationBase &&
            image_matches(proc, rbase, file_pe)) {
            base = rbase;
            return true;
        }
        region = end;
    }
    return false;
}

// How many of the 18 IP-table slots hold exactly ip (+ NUL padding); -1 when unreadable.
int count_table_slots(HANDLE proc, ULONG_PTR base, const std::string& ip)
{
    unsigned char table[kIpTableSlots * kIpSlotSize];
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(base + kIpTableRva), table, sizeof(table), &got) ||
        got != sizeof(table))
        return -1;
    unsigned char want[kIpSlotSize] = {};
    memcpy(want, ip.data(), ip.size() < kIpSlotSize ? ip.size() : kIpSlotSize - 1);
    int good = 0;
    for (int i = 0; i < kIpTableSlots; ++i)
        if (memcmp(table + i * kIpSlotSize, want, kIpSlotSize) == 0)
            ++good;
    return good;
}

// Ends a client that must not run. It is still suspended, so it has not connected anywhere.
void discard_client(PROCESS_INFORMATION& pi)
{
    if (TerminateProcess(pi.hProcess, 1))
        WaitForSingleObject(pi.hProcess, kKillWaitMs);
    else
        say(L"WARNING: the suspended client (PID %lu) could not be closed (%ls). Close Trickster.bin in Task Manager.",
            pi.dwProcessId, win_error(GetLastError()).c_str());
    if (pi.hThread)
        CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    pi.hThread = pi.hProcess = nullptr;
    say(L"The client was closed before it ran, so it did not connect anywhere.");
}

const wchar_t* start_error_hint(DWORD err)
{
    switch (err) {
    case ERROR_ELEVATION_REQUIRED:
        return L"\n       The client asks for administrator rights: start JanusLauncher.exe the same way.";
    case ERROR_ACCESS_DENIED:
        return L"\n       An antivirus may be blocking it, or the client folder needs administrator rights.";
    case ERROR_BAD_EXE_FORMAT:
        return L"\n       The file is not a valid Windows program (damaged copy?). Copy the client folder again.";
    default:
        return L"";
    }
}

// Why the client may have closed right after the start (RE of its early exits).
void explain_exit(DWORD code, const KnownBuild* build)
{
    switch (code) {
    case 0:
        if (build)
            say(L"       Most likely %d clients are already running on this PC: the %ls allows at most %d at a\n"
                L"       time and quietly closes the next one. Close one and press Play again.",
                build->max_clients, build->name, build->max_clients);
        else
            say(L"       Most likely the most clients this build allows are already running on this PC.\n"
                L"       Close one and try again.");
        break;
    case 1:
        say(L"       The client could not set up its window or Direct3D. Update the graphics driver, close\n"
            L"       other full-screen programs and try again.");
        break;
    case 0xC0000135:
        say(L"       A DLL is missing: ijl15.dll, LIBEXPAT.dll, MSS32.DLL and dbghelp.dll must be in the\n"
            L"       client folder, next to Trickster.bin.");
        break;
    case 0xC000007B:
        say(L"       A DLL in the client folder is damaged or of the wrong kind. Copy the client folder again.");
        break;
    case 0xC0000005:
        say(L"       The client crashed. Its folder may be incomplete (data\\libconfig.dat, data\\libconfig.idx)\n"
            L"       or the login arguments wrong (Advanced, Restore defaults). Send exception.log from the\n"
            L"       client folder and JanusLauncher.log to the server operator.");
        break;
    default:
        say(L"       Send JanusLauncher.log to the server operator.");
        break;
    }
}

// The whole start. Returns the exit code; every step is reported with say().
int start_client(const Settings& s, const std::wstring& exe_dir)
{
    // 1. Every value is checked before anything is started.
    Checked c;
    std::wstring problem = check_values(s, c);
    std::string ip, host;
    if (problem.empty())
        problem = parse_address(s.address, ip, host);
    if (!problem.empty()) {
        say(L"ERROR: %ls", problem.c_str());
        return kExitInvalid;
    }
    if (!host.empty()) {
        say(L"Looking up %hs...", host.c_str());
        problem = lookup_ipv4(host, ip);
        if (!problem.empty()) {
            say(L"ERROR: %ls", problem.c_str());
            return kExitInvalid;
        }
        say(L"%hs is %hs.", host.c_str(), ip.c_str());
    }
    const std::wstring client = full_client_path(s.client, exe_dir);
    const DWORD attr = client.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(client.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        say(L"ERROR: the client was not found: %ls\n"
            L"       Put JanusLauncher.exe in the client folder, next to Trickster.bin, or choose the client\n"
            L"       under Advanced (--client on the command line).", client.empty() ? trim(s.client).c_str() : client.c_str());
        return kExitNoClient;
    }

    // 2. Which client is it?
    PeInfo pe;
    if (!read_file_pe(client, pe)) {
        say(L"ERROR: %ls cannot be read or is not a Windows program.", client.c_str());
        return kExitStartFailed;
    }
    if (pe.machine != IMAGE_FILE_MACHINE_I386 || pe.magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        say(L"ERROR: %ls is not a 32-bit program. Only the 32-bit Trickster Online KR 2014 client is\n"
            L"       supported; it was not started.", client.c_str());
        return kExitNotPatched;
    }
    std::string md5;
    DWORD md5_err = 0;
    const KnownBuild* build = file_md5(client, md5, md5_err) ? find_build(md5) : nullptr;
    say(L"Server:   %hs, port %u (built into the client)", ip.c_str(), kClientPort);
    say(L"Account:  %hs", c.account.c_str());
    say(L"Client:   %ls", client.c_str());
    if (build)
        say(L"Build:    %ls (MD5 %hs)", build->name, md5.c_str());
    else if (!md5.empty())
        say(L"Build:    unknown (MD5 %hs). Note: the launcher looks for the Replace IPs in it; if none is\n"
            L"          found, the client is closed before it runs.", md5.c_str());
    else
        say(L"Build:    unknown (the file could not be read to identify it: %ls).", win_error(md5_err).c_str());
    const std::wstring cmdline = build_command_line(client, build_client_args(ip, c));
    say(L"Command line: %ls", cmdline.c_str());

    // 3. Start it suspended, in its own folder.
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(L'\0');
    const std::wstring workdir = folder_of(client);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(client.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        workdir.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        say(L"ERROR: the client could not be started (%ls).%ls", win_error(err).c_str(), start_error_hint(err));
        return kExitStartFailed;
    }
    say(L"Client created suspended (PID %lu): none of its code has run yet.", pi.dwProcessId);

    // 4. Point it at the server while it is suspended.
    ULONG_PTR base = 0;
    const wchar_t* how = L"";
    if (!find_client_image(pi.hProcess, pe, base, how)) {
        say(L"ERROR: the client's program image was not found in its memory.");
        discard_client(pi);
        return kExitNotPatched;
    }
    PatchStats st;
    scan_and_patch(pi.hProcess, base, base + pe.image_size, c.pats, ip.c_str(), st);
    report_patch(c.pats, st, ip);
    say(L"IP copies in the client's image at %p (found via %ls): %d replaced, %d already right, %d did not fit.",
        reinterpret_cast<void*>(base), how, st.patched, st.already, st.skipped);
    if (st.skipped > 0) {
        say(L"ERROR: the server address could not be put into every copy (see above), so the client could\n"
            L"       still connect to the address baked into it. Use a shorter server address.");
        discard_client(pi);
        return kExitNotPatched;
    }
    if (st.patched + st.already == 0) {
        say(L"ERROR: none of the Replace IPs (%ls) is in this client.\n"
            L"       Unknown client build: add its baked server IP to Replace IPs (Advanced). Packed\n"
            L"       builds such as the KR January 2014 client are not supported.", trim(s.replace_ips).c_str());
        discard_client(pi);
        return kExitNotPatched;
    }
    if (build) {
        const int good = count_table_slots(pi.hProcess, base, ip);
        if (good != kIpTableSlots) {
            say(L"ERROR: only %d of the 18 server slots of the %ls hold %hs. Restore the default Replace IPs\n"
                L"       (%ls).", good < 0 ? 0 : good, build->name, ip.c_str(), kDefaultReplaceIPs);
            discard_client(pi);
            return kExitNotPatched;
        }
        say(L"IP table: all 18 slots hold %hs.", ip.c_str());
    }

    // 5. Let it run.
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        say(L"ERROR: the client could not be resumed (%ls).", win_error(GetLastError()).c_str());
        discard_client(pi);
        return kExitStartFailed;
    }
    CloseHandle(pi.hThread);
    pi.hThread = nullptr;
    say(L"Client running (PID %lu). Checking it again in 3 seconds...", pi.dwProcessId);
    if (WaitForSingleObject(pi.hProcess, kRescanDelayMs) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        say(L"ERROR: the client closed right after it started (exit code %lu, 0x%08lX).", code, code);
        explain_exit(code, build);
        return kExitStartFailed;
    }

    // 6. Once more over all of its memory. Every runtime copy of the address is made from the
    //    patched table, so none is expected.
    PatchStats late;
    ULONG_PTR lo = 0, hi = 0;
    full_address_range(lo, hi);
    scan_and_patch(pi.hProcess, lo, hi, c.pats, ip.c_str(), late);
    report_patch(c.pats, late, ip);
    CloseHandle(pi.hProcess);
    say(L"Old IP copies found after 3 s: %d (expected 0); replaced: %d.", late.patched + late.skipped, late.patched);
    if (late.skipped > 0)
        say(L"WARNING: %d copies could not be replaced (see above). If the client cannot reach the server,\n"
            L"         send JanusLauncher.log to the server operator.", late.skipped);
    say(L"Done: the client (PID %lu) connects to %hs:%u.", pi.dwProcessId, ip.c_str(), kClientPort);
    return kExitOk;
}

// ---- the window ----

struct Ui {
    HWND         dlg = nullptr;
    bool         advanced = false;
    bool         busy = false;
    bool         close_requested = false;
    int          starts = 0;
    HANDLE       worker = nullptr;
    Settings     settings;   // loaded at start
    Settings     running;    // the values of the start in progress (saved when it succeeds)
    std::wstring exe_dir, ini;
};
Ui g_ui;

const int kAdvancedIds[] = {
    IDC_ADV_GROUP, IDC_CLIENT_LABEL, IDC_CLIENT, IDC_BROWSE, IDC_REPLACE_LABEL, IDC_REPLACE_IPS,
    IDC_KEY_LABEL, IDC_KEY, IDC_AGE_LABEL, IDC_AGE, IDC_GAMEID_LABEL, IDC_GAMEID, IDC_DEFAULTS,
    IDC_EXTRAS_LABEL, IDC_EXTRAS, IDC_PORT_INFO, IDC_PREVIEW_LABEL, IDC_PREVIEW,
};

std::wstring get_text(HWND dlg, int id)
{
    HWND h = GetDlgItem(dlg, id);
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n > 0 ? n : 0) + 1, L'\0');
    const int got = GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(got > 0 ? got : 0));
    return s;
}

Settings read_dialog(HWND dlg)
{
    Settings s;
    s.address = get_text(dlg, IDC_ADDRESS);
    s.account = get_text(dlg, IDC_ACCOUNT);
    s.client = get_text(dlg, IDC_CLIENT);
    s.replace_ips = get_text(dlg, IDC_REPLACE_IPS);
    s.key = get_text(dlg, IDC_KEY);
    s.age = get_text(dlg, IDC_AGE);
    s.extras = get_text(dlg, IDC_EXTRAS);
    s.close_on_start = IsDlgButtonChecked(dlg, IDC_CLOSE_ON_START) == BST_CHECKED;
    return s;
}

std::wstring preview_text(const Settings& s, const std::wstring& exe_dir)
{
    Checked c;
    std::wstring problem = check_values(s, c);
    std::string ip, host;
    if (problem.empty())
        problem = parse_address(s.address, ip, host);
    if (!problem.empty())
        return L"(not ready: " + problem + L")";
    if (!host.empty())
        ip = "<IPv4 of " + host + ">";
    std::wstring client = full_client_path(s.client, exe_dir);
    if (client.empty())
        client = trim(s.client);
    return build_command_line(client, build_client_args(ip, c));
}

void update_preview(HWND dlg)
{
    SetDlgItemTextW(dlg, IDC_PREVIEW, preview_text(read_dialog(dlg), g_ui.exe_dir).c_str());
}

void append_status(HWND dlg, const std::wstring& text)
{
    HWND st = GetDlgItem(dlg, IDC_STATUS);
    const int len = GetWindowTextLengthW(st);
    SendMessageW(st, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(st, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(st, EM_SCROLLCARET, 0, 0);
}

int bottom_of(HWND dlg, int id)
{
    RECT r;
    GetWindowRect(GetDlgItem(dlg, id), &r);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
    return r.bottom;
}

// Shows or hides the Advanced part. Hidden controls are also disabled, so Tab and the Alt keys
// cannot reach them. The height comes from the controls' current positions (the dialog manager
// moves them when the window changes to a screen with other scaling), and the window is kept
// inside the screen's work area.
void set_advanced(HWND dlg, bool show)
{
    g_ui.advanced = show;
    const HWND focus = GetFocus();
    bool focus_inside = false;
    for (int id : kAdvancedIds)
        focus_inside = focus_inside || GetDlgItem(dlg, id) == focus;
    if (!show && focus_inside)
        SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_ADVANCED)), TRUE);
    for (int id : kAdvancedIds) {
        HWND ctl = GetDlgItem(dlg, id);
        EnableWindow(ctl, show ? TRUE : FALSE);
        ShowWindow(ctl, show ? SW_SHOW : SW_HIDE);
    }
    SetDlgItemTextW(dlg, IDC_ADVANCED, show ? L"<< A&dvanced" : L"A&dvanced >>");

    RECT margin = {0, 0, 0, 7};   // dialog units under the last control
    MapDialogRect(dlg, &margin);
    const int client_h = bottom_of(dlg, show ? IDC_ADV_GROUP : IDC_ADVANCED) + margin.bottom;
    RECT wr, cr;
    GetWindowRect(dlg, &wr);
    GetClientRect(dlg, &cr);
    const int width = wr.right - wr.left;
    const int height = client_h + (wr.bottom - wr.top) - (cr.bottom - cr.top);
    int x = wr.left, y = wr.top;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& wa = mi.rcWork;
        if (y + height > wa.bottom)
            y = wa.bottom - height;
        if (y < wa.top)
            y = wa.top;
        if (x + width > wa.right)
            x = wa.right - width;
        if (x < wa.left)
            x = wa.left;
    }
    SetWindowPos(dlg, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void center_in_work_area(HWND dlg)
{
    RECT wr;
    GetWindowRect(dlg, &wr);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTOPRIMARY), &mi))
        return;
    const RECT& wa = mi.rcWork;
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    SetWindowPos(dlg, nullptr, wa.left + ((wa.right - wa.left) - w) / 2, wa.top + ((wa.bottom - wa.top) - h) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void init_dialog(HWND dlg)
{
    g_ui.dlg = dlg;
    const Settings& s = g_ui.settings;
    SendDlgItemMessageW(dlg, IDC_ADDRESS, EM_SETLIMITTEXT, kMaxHostLen, 0);
    SendDlgItemMessageW(dlg, IDC_ACCOUNT, EM_SETLIMITTEXT, kMaxAccountLen, 0);
    SendDlgItemMessageW(dlg, IDC_CLIENT, EM_SETLIMITTEXT, kMaxClientLen, 0);
    SendDlgItemMessageW(dlg, IDC_REPLACE_IPS, EM_SETLIMITTEXT, kMaxReplaceLen, 0);
    SendDlgItemMessageW(dlg, IDC_KEY, EM_SETLIMITTEXT, kMaxKeyLen, 0);
    SendDlgItemMessageW(dlg, IDC_AGE, EM_SETLIMITTEXT, kMaxAgeLen, 0);
    SendDlgItemMessageW(dlg, IDC_EXTRAS, EM_SETLIMITTEXT, kMaxExtrasLen, 0);
    SendDlgItemMessageW(dlg, IDC_STATUS, EM_SETLIMITTEXT, 0, 0);   // no 32K limit
    SetDlgItemTextW(dlg, IDC_ADDRESS, s.address.c_str());
    SetDlgItemTextW(dlg, IDC_ACCOUNT, s.account.c_str());
    SetDlgItemTextW(dlg, IDC_CLIENT, s.client.c_str());
    SetDlgItemTextW(dlg, IDC_REPLACE_IPS, s.replace_ips.c_str());
    SetDlgItemTextW(dlg, IDC_KEY, s.key.c_str());
    SetDlgItemTextW(dlg, IDC_AGE, s.age.c_str());
    SetDlgItemTextW(dlg, IDC_EXTRAS, s.extras.c_str());
    SetDlgItemTextW(dlg, IDC_GAMEID, widen(kGameId).c_str());
    CheckDlgButton(dlg, IDC_CLOSE_ON_START, s.close_on_start ? BST_CHECKED : BST_UNCHECKED);

    EnterCriticalSection(&g_out.lock);
    g_out.window = dlg;
    const std::wstring early = g_out.early;
    g_out.early.clear();
    LeaveCriticalSection(&g_out.lock);
    append_status(dlg, early);

    set_advanced(dlg, false);
    center_in_work_area(dlg);
    update_preview(dlg);
    const int first = trim(s.account).empty() ? IDC_ACCOUNT : IDOK;
    SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, first)), TRUE);
}

void end_dialog(HWND dlg)
{
    EnterCriticalSection(&g_out.lock);
    g_out.window = nullptr;
    LeaveCriticalSection(&g_out.lock);
    EndDialog(dlg, 0);
}

unsigned __stdcall start_thread(void* arg)
{
    std::unique_ptr<Settings> s(static_cast<Settings*>(arg));
    const int code = start_client(*s, g_ui.exe_dir);
    PostMessageW(g_ui.dlg, WM_APP_DONE, static_cast<WPARAM>(code), 0);
    return 0;
}

void start_finished(HWND dlg, int code)
{
    if (g_ui.worker) {
        WaitForSingleObject(g_ui.worker, INFINITE);   // it posted WM_APP_DONE as its last step
        CloseHandle(g_ui.worker);
        g_ui.worker = nullptr;
    }
    g_ui.busy = false;
    EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
    if (code == kExitOk) {
        const std::wstring problem = save_settings(g_ui.ini, g_ui.running);
        if (!problem.empty())
            say(L"Note: the settings were not saved in %ls (%ls).\n"
                L"      The folder may be read-only; the launcher works without it.", g_ui.ini.c_str(), problem.c_str());
        if (g_ui.running.close_on_start) {
            end_dialog(dlg);
            return;
        }
        if (!g_ui.close_requested)
            say(L"Press Play again to start another client (for example with a second account).");
    }
    if (g_ui.close_requested) {
        end_dialog(dlg);
        return;
    }
    SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDOK)), TRUE);
}

void start_from_dialog(HWND dlg)
{
    if (g_ui.busy)
        return;
    g_ui.running = read_dialog(dlg);
    if (g_ui.starts++ > 0)
        say(L"--------");
    if (GetFocus() == GetDlgItem(dlg, IDOK))
        SendMessageW(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_STATUS)), TRUE);
    g_ui.busy = true;
    EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
    // The start (DNS lookup, MD5, 3 s wait) runs on its own thread, so the window never freezes.
    Settings* job = new Settings(g_ui.running);
    const uintptr_t t = _beginthreadex(nullptr, 0, start_thread, job, 0, nullptr);
    if (t == 0) {
        delete job;
        say(L"ERROR: the start could not begin (no thread).");
        start_finished(dlg, kExitStartFailed);
        return;
    }
    g_ui.worker = reinterpret_cast<HANDLE>(t);
}

void browse_client(HWND dlg)
{
    const std::wstring current = full_client_path(get_text(dlg, IDC_CLIENT), g_ui.exe_dir);
    const std::wstring dir = folder_of(current);
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = L"Trickster client (Trickster.bin)\0Trickster.bin\0Programs (*.bin, *.exe)\0*.bin;*.exe\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Choose the client (Trickster.bin)";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn))
        return;
    std::wstring chosen = file.data();
    // Inside the launcher's folder: keep it relative, so the folder can be moved or shared.
    const std::wstring& base = g_ui.exe_dir;
    if (!base.empty() && chosen.size() > base.size() && _wcsnicmp(chosen.c_str(), base.c_str(), base.size()) == 0)
        chosen = chosen.substr(base.size());
    SetDlgItemTextW(dlg, IDC_CLIENT, chosen.c_str());
}

void close_dialog(HWND dlg)
{
    if (g_ui.busy) {
        // Never leave while the client may still be suspended: close when the start has finished.
        g_ui.close_requested = true;
        say(L"The window closes as soon as the start in progress has finished.");
        return;
    }
    end_dialog(dlg);
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        init_dialog(dlg);
        return FALSE;   // the focus is set
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            start_from_dialog(dlg);
            return TRUE;
        case IDCANCEL:
            close_dialog(dlg);
            return TRUE;
        case IDC_ADVANCED:
            set_advanced(dlg, !g_ui.advanced);
            return TRUE;
        case IDC_BROWSE:
            browse_client(dlg);
            return TRUE;
        case IDC_DEFAULTS:
            SetDlgItemTextW(dlg, IDC_KEY, kDefaultKey);
            SetDlgItemTextW(dlg, IDC_AGE, kDefaultAge);
            SetDlgItemTextW(dlg, IDC_EXTRAS, kDefaultExtras);
            return TRUE;
        case IDC_ADDRESS:
        case IDC_ACCOUNT:
        case IDC_CLIENT:
        case IDC_REPLACE_IPS:
        case IDC_KEY:
        case IDC_AGE:
        case IDC_EXTRAS:
            if (HIWORD(wp) == EN_CHANGE)
                update_preview(dlg);
            return TRUE;
        default:
            break;
        }
        break;
    case WM_APP_LINE: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        append_status(dlg, *text);
        return TRUE;
    }
    case WM_APP_DONE:
        start_finished(dlg, static_cast<int>(wp));
        return TRUE;
    case WM_APP_RELAYOUT:
        set_advanced(dlg, g_ui.advanced);
        return TRUE;
    case WM_DPICHANGED:
        // The dialog manager rescales the window and its controls; then the height is redone.
        PostMessageW(dlg, WM_APP_RELAYOUT, 0, 0);
        return FALSE;
    default:
        break;
    }
    return FALSE;
}

// ---- command line ----

struct Override {
    bool         set = false;
    std::wstring value;
};

struct Cli {
    bool play = false, selftest = false, help = false;
    std::wstring error;
    Override address, account, client, replace_ips, key, age, extras;
};

Cli parse_cli(int argc, wchar_t** argv)
{
    Cli c;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i], value;
        bool inline_value = false;
        if (a.size() > 2 && a[0] == L'-' && a[1] == L'-') {
            const size_t eq = a.find(L'=');
            if (eq != std::wstring::npos) {
                value = a.substr(eq + 1);
                a = a.substr(0, eq);
                inline_value = true;
            }
        }
        Override* target = nullptr;
        if (a == L"--play")
            c.play = true;
        else if (a == L"--selftest")
            c.selftest = true;
        else if (a == L"--help" || a == L"-h" || a == L"-?" || a == L"/?")
            c.help = true;
        else if (a == L"--ip" || a == L"--address")
            target = &c.address;
        else if (a == L"--account")
            target = &c.account;
        else if (a == L"--client")
            target = &c.client;
        else if (a == L"--replace-ips")
            target = &c.replace_ips;
        else if (a == L"--key" || a == L"--session-key")
            target = &c.key;
        else if (a == L"--age")
            target = &c.age;
        else if (a == L"--extras")
            target = &c.extras;
        else if (!a.empty() && a[0] != L'-' && a[0] != L'/' && !c.account.set) {
            c.account.set = true;   // JanusLauncher.exe <account>
            c.account.value = a;
            continue;
        } else {
            c.error = L"Unknown option: " + a;
            return c;
        }
        if (!target) {
            if (inline_value) {
                c.error = a + L" takes no value.";
                return c;
            }
            continue;
        }
        if (!inline_value) {
            if (i + 1 >= argc) {
                c.error = L"Missing value after " + a + L".";
                return c;
            }
            value = argv[++i];
        }
        target->set = true;
        target->value = value;
    }
    return c;
}

void apply(const Override& o, std::wstring& field)
{
    if (o.set)
        field = o.value;
}

std::wstring usage_text()
{
    return std::wstring(
        L"Janus Launcher " JL_VERSION_STR L" - starts the Trickster Online KR 2014 client and points it at a server.\n"
        L"\n"
        L"  JanusLauncher.exe                  open the window\n"
        L"  JanusLauncher.exe <account>        open the window with this account filled in\n"
        L"  JanusLauncher.exe --play [options] start one client without a window; values not given come\n"
        L"                                     from JanusLauncher.ini, then the defaults:\n"
        L"      --ip <address>        server IPv4 address or host name (default 127.0.0.1)\n"
        L"      --account <name>      account name: 1-18 letters, digits or _\n"
        L"      --client <path>       the client (default Trickster.bin next to the launcher)\n"
        L"      --replace-ips <list>  baked IPs to replace (default 51.38.50.68,217.160.132.225,211.44.251.24)\n"
        L"      --key <digits>        session key, not a password (default 0)\n"
        L"      --age <0-255>         (default 0)\n"
        L"      --extras <text>       (default blankblank0|0)\n"
        L"  JanusLauncher.exe --selftest       run the built-in tests (exit code 0 = all passed)\n"
        L"  JanusLauncher.exe --help           this text\n"
        L"\n"
        L"Exit codes of --play: 0 the client runs and points at the server; 1 invalid setting; 2 client\n"
        L"not found; 3 the client could not start or closed at once; 4 the server IP could not be put\n"
        L"into the client (it is closed before it runs). The status lines also go to JanusLauncher.log\n"
        L"next to the launcher (or in %TEMP% when that folder is read-only).");
}

// ---- --selftest ----

int g_checks = 0;
int g_failed = 0;

void expect(bool ok, const wchar_t* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        say(L"FAIL: %ls", what);
    }
}

bool write_bytes(const std::wstring& path, const void* data, size_t len)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
    return ok != FALSE && written == len;
}

std::string read_bytes(const std::wstring& path)
{
    std::string bytes;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
        bytes.append(buf, got);
    CloseHandle(f);
    return bytes;
}

bool is_utf16_file(const std::string& bytes)
{
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
}

void selftest_validation()
{
    expect(is_ipv4("127.0.0.1"), L"is_ipv4 127.0.0.1");
    expect(is_ipv4("255.255.255.255"), L"is_ipv4 255.255.255.255");
    expect(is_ipv4("127.000.000.001"), L"is_ipv4 127.000.000.001");
    expect(!is_ipv4("256.1.1.1"), L"is_ipv4 256.1.1.1");
    expect(!is_ipv4("1.2.3"), L"is_ipv4 1.2.3");
    expect(!is_ipv4("1.2.3.4.5"), L"is_ipv4 1.2.3.4.5");
    expect(!is_ipv4("1..2.3"), L"is_ipv4 1..2.3");
    expect(!is_ipv4(""), L"is_ipv4 empty");
    expect(!is_ipv4("1.2.3.4 "), L"is_ipv4 trailing space");
    expect(!is_ipv4("0001.2.3.4"), L"is_ipv4 4 digits");
    std::string ip, host;
    expect(canonical_ipv4("127.000.000.001", ip) && ip == "127.0.0.1", L"canonical 127.000.000.001");
    expect(canonical_ipv4("010.1.1.1", ip) && ip == "10.1.1.1", L"canonical 010.1.1.1 (no octal)");

    // parse_address never does a DNS lookup itself.
    expect(parse_address(L" 127.0.0.2 ", ip, host).empty() && ip == "127.0.0.2" && host.empty(), L"address 127.0.0.2");
    expect(parse_address(L"play.example.com", ip, host).empty() && ip.empty() && host == "play.example.com",
           L"address host name");
    expect(parse_address(L"localhost", ip, host).empty() && host == "localhost", L"address localhost");
    expect(!parse_address(L"0.0.0.0", ip, host).empty(), L"address 0.0.0.0 refused");
    expect(!parse_address(L"1.2.3.256", ip, host).empty() && host.empty(), L"address 1.2.3.256 refused, no lookup");
    expect(!parse_address(L"", ip, host).empty(), L"address empty");
    expect(!parse_address(L"bad host", ip, host).empty(), L"address with a space");
    expect(!parse_address(L"a..b", ip, host).empty(), L"address a..b");
    expect(!parse_address(L"\"1.2.3.4\"", ip, host).empty(), L"address with quotes");
    expect(!parse_address(L"Jos\u00e9.com", ip, host).empty(), L"address non-ASCII");
    expect(!is_host_name(std::string(254, 'a')), L"host name over 253 characters");
    expect(!is_host_name(std::string(64, 'a') + ".com"), L"host label over 63 characters");

    std::string s;
    expect(check_account(L"player1", s).empty() && s == "player1", L"account player1");
    expect(check_account(L" A_b_9 ", s).empty() && s == "A_b_9", L"account trimmed");
    expect(check_account(L"abcdefghijklmnopqr", s).empty(), L"account 18 characters");
    expect(!check_account(L"abcdefghijklmnopqrs", s).empty(), L"account 19 characters");
    expect(!check_account(L"", s).empty(), L"account empty");
    expect(!check_account(L"bad name", s).empty(), L"account with space");
    expect(!check_account(L"a,b", s).empty(), L"account with comma");
    expect(!check_account(L"a|b", s).empty(), L"account with |");
    expect(!check_account(L"Jos\u00e9", s).empty(), L"account non-ASCII");
    expect(!check_account(L"ts", s).empty(), L"account keyword ts");
    expect(!check_account(L"TestServer", s).empty(), L"account keyword TestServer");
    expect(check_account(L"tsx", s).empty(), L"account tsx");

    expect(check_key(L"0", s).empty() && s == "0", L"key 0");
    expect(check_key(L"12345678901", s).empty(), L"key 11 digits");
    expect(check_key(L"123456789012345678", s).empty(), L"key 18 digits");
    expect(!check_key(L"1234567890123456789", s).empty(), L"key 19 digits");
    expect(!check_key(L"", s).empty(), L"key empty");
    expect(!check_key(L"12a", s).empty(), L"key with a letter");
    expect(!check_key(L"-1", s).empty(), L"key negative");
    expect(check_age(L"0", s).empty() && check_age(L"255", s).empty(), L"age 0 and 255");
    expect(!check_age(L"256", s).empty() && !check_age(L"", s).empty() && !check_age(L"1234", s).empty() &&
           !check_age(L"abc", s).empty(), L"age refused values");

    expect(check_extras(kDefaultExtras, s).empty() && s == "blankblank0|0", L"extras default");
    expect(!check_extras(L"", s).empty(), L"extras empty");
    expect(!check_extras(L"a,b", s).empty(), L"extras with comma");
    expect(!check_extras(L"a b", s).empty(), L"extras with space");
    expect(!check_extras(L"a\"b", s).empty(), L"extras with quote");
    expect(!check_extras(L"devsite|0", s).empty() && !check_extras(L"DEVSITx", s).empty(), L"extras devsit prefix");
    expect(!check_extras(L"blankblank0|0|0|23", s).empty(), L"extras shutdown hour");
    expect(check_extras(L"blankblank0|0|0|0", s).empty(), L"extras fourth value 0");
    expect(!check_extras(L"x|" + std::wstring(128, L'a'), s).empty(), L"extras web id 128");
    expect(check_extras(L"x|" + std::wstring(127, L'a'), s).empty(), L"extras web id 127");
    expect(!check_extras(std::wstring(201, L'a'), s).empty(), L"extras 201 characters");
    expect(!check_extras(L"ts", s).empty(), L"extras keyword");

    std::vector<Pattern> pats;
    expect(parse_replace_ips(kDefaultReplaceIPs, pats).empty() && pats.size() == 3 && pats[0].len == 11,
           L"replace IPs default");
    expect(parse_replace_ips(L"1.2.3.4,1.2.3.4", pats).empty() && pats.size() == 1, L"replace IPs duplicate");
    expect(parse_replace_ips(L" 1.2.3.4 , 5.6.7.8 ,", pats).empty() && pats.size() == 2, L"replace IPs trimmed");
    expect(!parse_replace_ips(L"1.2.3", pats).empty(), L"replace IPs invalid");
    expect(!parse_replace_ips(L" , ", pats).empty(), L"replace IPs empty");
    expect(!parse_replace_ips(L"1.1.1.1,1.1.1.2,1.1.1.3,1.1.1.4,1.1.1.5,1.1.1.6,1.1.1.7,1.1.1.8,1.1.1.9", pats).empty(),
           L"replace IPs more than 8");
}

void selftest_plan()
{
    char out[kMaxIpLen + 1];
    const unsigned char pad[16] = {};
    // Same length: the NUL after the new IP lands on the first padding byte.
    size_t n = plan_ip_patch(11, "127.0.0.114", pad, 5, out, sizeof(out));
    expect(n == 12 && memcmp(out, "127.0.0.114\0", 12) == 0, L"plan: same length");
    // Shorter: the rest of the old string becomes NUL.
    n = plan_ip_patch(15, "127.0.0.1", pad, 1, out, sizeof(out));
    expect(n == 15 && memcmp(out, "127.0.0.1\0\0\0\0\0\0", 15) == 0, L"plan: shorter IP");
    // Longer, with enough padding (the 16-byte slots).
    n = plan_ip_patch(11, "255.255.255.255", pad, 5, out, sizeof(out));
    expect(n == 16 && memcmp(out, "255.255.255.255\0", 16) == 0, L"plan: longer IP into padding");
    // Longer, padding missing or too short.
    const unsigned char short_pad[5] = {0, 0, 'x', 0, 0};
    expect(plan_ip_patch(11, "255.255.255.255", short_pad, 5, out, sizeof(out)) == 0, L"plan: padding interrupted");
    expect(plan_ip_patch(11, "255.255.255.255", pad, 2, out, sizeof(out)) == 0, L"plan: padding unreadable");
    const unsigned char no_pad[1] = {'A'};
    expect(plan_ip_patch(11, "127.0.0.114", no_pad, 1, out, sizeof(out)) == 0, L"plan: no padding at all");
    expect(plan_ip_patch(11, "127.0.0.114", pad, 5, out, 4) == 0, L"plan: output buffer too small");
}

void selftest_command_line()
{
    Checked c;
    Settings s;
    s.account = L"player1";
    expect(check_values(s, c).empty(), L"default values valid");
    const std::string args = build_client_args("127.0.0.1", c);
    expect(args == "127.0.0.1,player1,0,Trickster,0,blankblank0|0", L"client arguments");
    expect(build_command_line(L"C:\\Users\\Jos\u00e9\\\u00c1rea de Trabalho\\Trickster.bin", args) ==
           L"\"C:\\Users\\Jos\u00e9\\\u00c1rea de Trabalho\\Trickster.bin\" \"127.0.0.1,player1,0,Trickster,0,blankblank0|0\"",
           L"command line");
    expect(full_client_path(L"Trickster.bin", L"C:\\Jogos\\\u00c1rea\\") == L"C:\\Jogos\\\u00c1rea\\Trickster.bin",
           L"relative client path");
    expect(full_client_path(L"D:\\TO\\Trickster.bin", L"C:\\X\\") == L"D:\\TO\\Trickster.bin", L"absolute client path");
    expect(full_client_path(L"..\\TO\\Trickster.bin", L"C:\\A\\B\\") == L"C:\\A\\TO\\Trickster.bin", L"client path ..");
    expect(folder_of(L"C:\\Trickster.bin") == L"C:\\" && folder_of(L"C:\\A\\Trickster.bin") == L"C:\\A", L"folder_of");
    expect(preview_text(s, L"C:\\T\\").find(L"\"127.0.0.1,player1,0,Trickster,0,blankblank0|0\"") != std::wstring::npos,
           L"preview");
    s.account.clear();
    expect(preview_text(s, L"C:\\T\\").compare(0, 11, L"(not ready:") == 0, L"preview of an invalid value");
}

// The scanner and patch_hit on a buffer of this process (never another process).
void selftest_scanner()
{
    const size_t size = 128 * 1024;
    char* mem = static_cast<char*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    expect(mem != nullptr, L"scanner: allocation");
    if (!mem)
        return;
    memcpy(mem + 0, "51.38.50.68\0\0\0\0\0", 16);           // a table slot
    memcpy(mem + 100, "151.38.50.68\0\0\0\0", 16);          // the tail of a longer number
    memcpy(mem + 200, "51.38.50.681\0\0\0\0", 16);          // the start of a longer number
    memcpy(mem + 300, "51.38.50.68ABC", 14);                // no padding after it
    memcpy(mem + 65536 - 5, "51.38.50.68\0\0\0\0\0", 16);   // across two 64 KB reads
    DWORD old = 0;
    VirtualProtect(mem, size, PAGE_READONLY, &old);         // like the client's .rdata
    std::vector<Pattern> pats;
    parse_replace_ips(kDefaultReplaceIPs, pats);
    PatchStats st;
    say(L"(the next line is an expected error of the scanner test)");
    scan_and_patch(GetCurrentProcess(), reinterpret_cast<ULONG_PTR>(mem), reinterpret_cast<ULONG_PTR>(mem) + size,
                   pats, "127.0.0.114", st);
    expect(st.patched == 2 && st.skipped == 1 && st.already == 0, L"scanner: counts");
    expect(memcmp(mem + 0, "127.0.0.114\0\0\0\0\0", 16) == 0, L"scanner: slot rewritten");
    expect(memcmp(mem + 65536 - 5, "127.0.0.114\0\0\0\0\0", 16) == 0, L"scanner: copy across reads rewritten");
    expect(memcmp(mem + 100, "151.38.50.68", 12) == 0 && memcmp(mem + 200, "51.38.50.681", 12) == 0 &&
           memcmp(mem + 300, "51.38.50.68ABC", 14) == 0, L"scanner: other bytes unchanged");
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(mem, &mbi, sizeof(mbi));
    expect(mbi.Protect == PAGE_READONLY, L"scanner: protection restored");
    VirtualFree(mem, 0, MEM_RELEASE);

    // The image of this process: found through the PEB, and its header matches the file.
    std::vector<wchar_t> self(32768);
    const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    PeInfo pe;
    expect(n > 0 && read_file_pe(std::wstring(self.data(), n), pe) && pe.machine == IMAGE_FILE_MACHINE_I386 &&
           pe.magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC, L"PE header of the launcher");
    ULONG_PTR base = 0;
    expect(read_peb_image_base(GetCurrentProcess(), base) &&
           base == reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)), L"PEB image base");
    base = 0;
    const wchar_t* how = L"";
    expect(find_client_image(GetCurrentProcess(), pe, base, how) &&
           base == reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr)), L"find_client_image");
}

void selftest_files()
{
    wchar_t tmp[MAX_PATH + 1];
    const DWORD tn = GetTempPathW(MAX_PATH + 1, tmp);
    expect(tn > 0 && tn <= MAX_PATH, L"temp folder");
    if (tn == 0 || tn > MAX_PATH)
        return;
    wchar_t tag[48];
    swprintf_s(tag, L"JanusLauncher_selftest_%lu_", GetCurrentProcessId());
    const std::wstring dir = std::wstring(tmp) + tag;
    const std::wstring ansi = dir + L"ansi.ini", wide = dir + L"utf16.ini", fresh = dir + L"new.ini",
                       data = dir + L"md5.txt";

    // MD5 (BCrypt).
    std::string md5;
    DWORD err = 0;
    expect(write_bytes(data, "abc", 3) && file_md5(data, md5, err) && md5 == "900150983cd24fb0d6963f7d28e17f72",
           L"MD5 of \"abc\"");

    // An ANSI ini.
    const char ansi_text[] = "; comment\r\n[Server]\r\nAddress=10.1.2.3\r\nAccount=tester_1\r\n"
                             "[Launcher]\r\nClientPath=D:\\Games\\TO\\Trickster.bin\r\nAge=7\r\n";
    Settings s;
    expect(write_bytes(ansi, ansi_text, sizeof(ansi_text) - 1), L"ini: write ANSI file");
    load_settings(ansi, s);
    expect(s.address == L"10.1.2.3" && s.account == L"tester_1" && s.client == L"D:\\Games\\TO\\Trickster.bin" &&
           s.age == L"7" && s.key == kDefaultKey && s.extras == kDefaultExtras && s.replace_ips == kDefaultReplaceIPs,
           L"ini: read ANSI");

    // A UTF-16 ini with a non-ASCII path.
    const std::wstring korean_path = L"C:\\Users\\Jos\u00e9\\\u00c1rea de Trabalho\\\uD55C\uAD6D\\Trickster.bin";
    const std::wstring wide_text = L"\xFEFF[Server]\r\nAddress=play.example.com\r\nAccount=abc\r\n[Launcher]\r\nClientPath=" +
                                   korean_path + L"\r\n";
    expect(write_bytes(wide, wide_text.data(), wide_text.size() * sizeof(wchar_t)), L"ini: write UTF-16 file");
    Settings w;
    load_settings(wide, w);
    expect(w.address == L"play.example.com" && w.account == L"abc" && w.client == korean_path, L"ini: read UTF-16");

    // Save into a new file: created as UTF-16, defaults not written, everything reads back.
    Settings out;
    out.address = L"127.0.0.5";
    out.account = L"saver";
    out.client = korean_path;
    out.extras = L"blankblank1|0";
    out.close_on_start = true;
    DeleteFileW(fresh.c_str());
    expect(save_settings(fresh, out).empty(), L"ini: save into a new file");
    Settings back;
    load_settings(fresh, back);
    expect(back.address == out.address && back.account == out.account && back.client == korean_path &&
           back.extras == out.extras && back.close_on_start && back.key == kDefaultKey, L"ini: round trip");
    expect(is_utf16_file(read_bytes(fresh)), L"ini: new file is UTF-16");
    expect(!ini_has_key(fresh, L"Launcher", L"ReplaceIPs"), L"ini: default value not written");

    // Save a value the ANSI code page cannot hold into the ANSI file: converted, comment kept.
    Settings conv = s;
    conv.client = korean_path;
    expect(save_settings(ansi, conv).empty(), L"ini: save into an ANSI file");
    Settings back2;
    load_settings(ansi, back2);
    const std::string bytes = read_bytes(ansi);
    expect(back2.client == korean_path && back2.address == L"10.1.2.3" && back2.age == L"7", L"ini: ANSI round trip");
    if (!fits_ansi(korean_path)) {
        const std::wstring comment = L"; comment";
        expect(is_utf16_file(bytes) &&
               std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2).find(comment) !=
                   std::wstring::npos, L"ini: ANSI file converted to UTF-16 with its comment");
    }
    DeleteFileW(ansi.c_str());
    DeleteFileW(wide.c_str());
    DeleteFileW(fresh.c_str());
    DeleteFileW(data.c_str());
}

int run_selftest()
{
    say(L"Janus Launcher " JL_VERSION_STR L" self-test");
    selftest_validation();
    selftest_plan();
    selftest_command_line();
    selftest_scanner();
    selftest_files();
    wchar_t summary[128];
    if (g_failed == 0)
        swprintf_s(summary, L"selftest: all %d checks passed.", g_checks);
    else
        swprintf_s(summary, L"selftest: %d of %d checks FAILED.", g_failed, g_checks);
    say(L"%ls", summary);
    if (!g_out.out)
        MessageBoxW(nullptr, summary, L"Janus Launcher", g_failed ? MB_ICONERROR : MB_ICONINFORMATION);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    InitializeCriticalSection(&g_out.lock);
    const std::wstring exe_dir = launcher_folder();

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Cli cli = argv ? parse_cli(argc, argv) : Cli();
    if (argv)
        LocalFree(argv);

    if (cli.play || cli.selftest || cli.help || !cli.error.empty())
        attach_output();
    if (!cli.error.empty() || cli.help) {
        const std::wstring text = cli.error.empty() ? usage_text() : cli.error + L"\n\n" + usage_text();
        if (g_out.out)
            emit(text);
        else
            MessageBoxW(nullptr, text.c_str(), L"Janus Launcher",
                        cli.error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return cli.error.empty() ? kExitOk : kExitInvalid;
    }
    if (cli.selftest)
        return run_selftest();

    open_log(exe_dir);
    SYSTEMTIME now;
    GetLocalTime(&now);
    say(L"Janus Launcher " JL_VERSION_STR L" (%04u-%02u-%02u %02u:%02u:%02u)", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond);
    if (g_out.log == INVALID_HANDLE_VALUE)
        say(L"Note: no log file could be written (the launcher's folder and %%TEMP%% are read-only).");
    else if (g_out.log_path != exe_dir + kLogName)
        say(L"Note: the launcher's folder is read-only or in use; the log is %ls.", g_out.log_path.c_str());

    Settings s;
    const std::wstring ini = exe_dir + kIniName;
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        say(L"%ls not found next to the launcher: using the defaults.", kIniName);
    else
        load_settings(ini, s);
    apply(cli.address, s.address);
    apply(cli.account, s.account);
    apply(cli.client, s.client);
    apply(cli.replace_ips, s.replace_ips);
    apply(cli.key, s.key);
    apply(cli.age, s.age);
    apply(cli.extras, s.extras);

    if (cli.play) {
        // No window: one start, the ini is not changed.
        const int code = start_client(s, exe_dir);
        say(L"Exit code %d.", code);
        if (g_out.log != INVALID_HANDLE_VALUE)
            CloseHandle(g_out.log);
        return code;
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   // file dialog
    say(L"Type the server address and your account name, then press Play.");
    g_ui.settings = s;
    g_ui.exe_dir = exe_dir;
    g_ui.ini = ini;
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialog_proc, 0);
    if (SUCCEEDED(com))
        CoUninitialize();
    if (g_out.log != INVALID_HANDLE_VALUE)
        CloseHandle(g_out.log);
    return kExitOk;
}
