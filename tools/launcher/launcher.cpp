// ProjectPAULA - tools/launcher/launcher.cpp (PaulaLauncher.exe, the player-side launcher)
//
// Starts the KR 2014 client (Trickster.bin) with the command line the retail web launcher gave
// it, then points it at a ProjectPAULA server: every copy of the server IP baked into the client
// ([Launcher] OriginalIP) is rewritten in the client's memory with the server's IP
// ([Server] LoginIP). The client then dials that IP on its built-in login port 9980.
//   1. Command line "<LoginIP>,<Account>,0,Trickster,0,blankblank0|0" (the format of the
//      janus-emulator launcher, live-tested).
//   2. CreateProcess (not suspended), working directory = the client's folder.
//   3. After 3 s, scan the client's readable memory and rewrite each copy in place, but only
//      where the new IP fits (plan_ip_patch): nothing is written past the old string except
//      over the NUL padding that already follows it.
// Settings: launcher.ini next to PaulaLauncher.exe (template and README.md in this folder).
//
// Ported from janus-emulator/tools/launcher/launcher.cpp. Dropped: the writes to 0x132E584 /
// 0x132E604 (globals of an older client build; they lie outside the image of the supported
// build, so the writes failed there and could corrupt memory on another build) and the unused
// hook.dll injection setting.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cstring>

#ifndef PAULA_LAUNCHER_VERSION
#define PAULA_LAUNCHER_VERSION "0.1.0"
#endif

#define PL_TAG "[PaulaLauncher] "

namespace {

// The server IP baked into each known client build (the "MainServer" IP table of 16-byte,
// NUL-padded slots):
//   217.160.132.225  pristine Client_live build (the build ProjectPAULA is tested with)
//   51.38.50.68      Client_Janus build patched on 2026-10-21
//   211.44.251.24    KR January 2014 client (not supported: it crashes in-world)
const char kDefaultOriginalIPs[] = "217.160.132.225,51.38.50.68,211.44.251.24";

// The rest of the client command line, unchanged from the janus launcher (live-tested):
// group "0", auth "Trickster", subscript "0", then "blank" + token.
const char kGroupId[] = "0";
const char kToken[]   = "blank0|0";

const size_t kMaxIpLen      = 15;     // "255.255.255.255"
const size_t kMaxAccountLen = 18;     // the client's account field is 19 bytes with the NUL
const int    kMaxPatterns   = 8;
const UINT   kClientPort    = 9980;   // the supported clients always dial this login port
const DWORD  kStartDelayMs  = 3000;   // janus launcher value: let the client initialize first
const int    kScanAttempts  = 3;      // nothing found yet: rescan after 1 s (slow PCs)

struct Pattern {
    char   ip[kMaxIpLen + 1];
    size_t len;
};

struct PatchStats {
    int patched = 0;   // copies rewritten with LoginIP
    int already = 0;   // copies that already hold LoginIP
    int skipped = 0;   // copies left unchanged: LoginIP does not fit there, or the write failed
};

enum class Hit { NotThisIp, Patched, Already, Skipped };

// Started from Explorer, the launcher is alone on its console, which closes as soon as it
// exits: keep it open until a key is pressed so the player can read the messages.
void wait_for_key_if_own_console()
{
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) != 1)
        return;
    printf("\nPress any key to close this window...");
    fflush(stdout);
    _getch();
    printf("\n");
}

int fail(int exit_code)
{
    wait_for_key_if_own_console();
    return exit_code;
}

bool is_ip_char(char c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

// Dotted-quad IPv4, digits and dots only, at most 15 characters. Host names are refused:
// the client's IP slots hold the literal address.
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

// Same rule as tools/add-account.bat: 1-18 characters from A-Z a-z 0-9 _. (The name is also a
// field of the comma-separated client command line, so no commas, quotes or spaces.)
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

void trim(char* s)
{
    const char* b = s;
    while (*b == ' ' || *b == '\t')
        ++b;
    const char* e = b + strlen(b);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        --e;
    memmove(s, b, (size_t)(e - b));
    s[e - b] = '\0';
}

// Splits the comma-separated OriginalIP list. Returns the number of addresses, or -1 after
// printing an error.
int parse_original_ips(const char* list, Pattern* out, int max_out)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    int count = 0;
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
        trim(tok);
        if (tok[0] == '\0')
            continue;
        if (!is_ipv4(tok)) {
            printf(PL_TAG "ERROR: [Launcher] OriginalIP: \"%s\" is not an IPv4 address.\n", tok);
            return -1;
        }
        bool duplicate = false;
        for (int k = 0; k < count; ++k)
            duplicate = duplicate || strcmp(out[k].ip, tok) == 0;
        if (duplicate)
            continue;
        if (count == max_out) {
            printf(PL_TAG "ERROR: [Launcher] OriginalIP: at most %d addresses.\n", max_out);
            return -1;
        }
        snprintf(out[count].ip, sizeof(out[count].ip), "%s", tok);
        out[count].len = strlen(tok);
        ++count;
    }
    if (count == 0)
        printf(PL_TAG "ERROR: [Launcher] OriginalIP holds no address.\n");
    return count == 0 ? -1 : count;
}

// How one copy of the old IP is rewritten. old_len = length of the old IP; after = the bytes
// that follow it in memory (after_len of them could be read). The new IP is written with its
// NUL and, when it is shorter, NULs over the rest of the old string (as the janus launcher
// did): max(new_len + 1, old_len) bytes from the start of the copy. Bytes past the old string
// are only overwritten when they already are NUL padding (the 16-byte slots of the client's IP
// table), so a longer IP never runs into the next value. Returns the number of bytes to write
// (out holds them), or 0 when the new IP does not fit there.
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

// One occurrence of pat at addr in the client; prot = the protection of its memory region.
Hit patch_hit(HANDLE proc, char* addr, DWORD prot, const Pattern& pat, const char* new_ip)
{
    // The bytes after the old string, up to the first unreadable one.
    unsigned char after[kMaxIpLen + 1] = {};
    size_t after_len = 0;
    while (after_len < sizeof(after)) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, addr + pat.len + after_len, &after[after_len], 1, &got) || got != 1)
            break;
        ++after_len;
    }
    if (after_len > 0 && is_ip_char((char)after[0]))
        return Hit::NotThisIp;   // the start of a longer number, e.g. "51.38.50.681"
    if (strcmp(pat.ip, new_ip) == 0)
        return Hit::Already;

    char bytes[kMaxIpLen + 1];
    const size_t n = plan_ip_patch(pat.len, new_ip, after, after_len, bytes, sizeof(bytes));
    if (n == 0) {
        const size_t need = strlen(new_ip) + 1 - pat.len;
        printf(PL_TAG "ERROR: %s does not fit in the client's copy of %s at %p: it needs %u NUL\n"
               PL_TAG "       bytes after the old address and fewer are there. That copy was left unchanged.\n",
               new_ip, pat.ip, (void*)addr, (unsigned)need);
        return Hit::Skipped;
    }
    const bool exec = prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE;
    DWORD old_prot = 0;
    const BOOL reprotected = VirtualProtectEx(proc, addr, n, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                                              &old_prot);
    SIZE_T written = 0;
    const BOOL ok = WriteProcessMemory(proc, addr, bytes, n, &written);
    const DWORD err = GetLastError();
    if (reprotected)
        VirtualProtectEx(proc, addr, n, old_prot, &old_prot);
    if (!ok || written != n) {
        printf(PL_TAG "ERROR: could not write the server IP at %p (Windows error %lu). That copy was left\n"
               PL_TAG "       unchanged.\n", (void*)addr, err);
        return Hit::Skipped;
    }
    printf(PL_TAG "  %s -> %s at %p\n", pat.ip, new_ip, (void*)addr);
    return Hit::Patched;
}

// Scans every committed, readable region of the client for the old IPs and rewrites them.
// Guard pages and write-copy pages are left alone, as the janus launcher did (the IP table is
// in the client's read-only data).
void scan_and_patch(HANDLE proc, const Pattern* pats, int npats, const char* new_ip, PatchStats& st)
{
    size_t maxlen = 0;
    for (int k = 0; k < npats; ++k)
        if (pats[k].len > maxlen)
            maxlen = pats[k].len;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const ULONG_PTR top = (ULONG_PTR)si.lpMaximumApplicationAddress;
    ULONG_PTR region = (ULONG_PTR)si.lpMinimumApplicationAddress;
    static char buf[64 * 1024];

    MEMORY_BASIC_INFORMATION mbi;
    while (region < top && VirtualQueryEx(proc, (LPCVOID)region, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress;
        const ULONG_PTR end = base + mbi.RegionSize;
        if (end <= region)
            break;   // wrapped around the top of the address space
        const DWORD prot = mbi.Protect;
        if (mbi.State == MEM_COMMIT && (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                                        prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE)) {
            char prev = 0;   // the byte before buf[0] (none at the start of a region)
            ULONG_PTR p = base;
            while (p < end) {
                size_t want = (size_t)(end - p);
                if (want > sizeof(buf))
                    want = sizeof(buf);
                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, (LPCVOID)p, buf, want, &got) && got == 0)
                    break;   // no longer readable: next region
                const bool last = p + got >= end || got < want || got < maxlen;
                // Each start position is examined once: the next read starts maxlen-1 bytes back,
                // so a copy that straddles two reads is still found.
                const size_t examine = last ? got : got - (maxlen - 1);
                for (size_t i = 0; i < examine; ++i) {
                    if (is_ip_char(i ? buf[i - 1] : prev))
                        continue;   // the tail of a longer number, e.g. "151.38.50.68"
                    for (int k = 0; k < npats; ++k) {
                        if (i + pats[k].len > got || memcmp(buf + i, pats[k].ip, pats[k].len) != 0)
                            continue;
                        const Hit hit = patch_hit(proc, (char*)(p + i), prot, pats[k], new_ip);
                        if (hit == Hit::NotThisIp)
                            continue;
                        if (hit == Hit::Patched)
                            ++st.patched;
                        else if (hit == Hit::Already)
                            ++st.already;
                        else
                            ++st.skipped;
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

bool is_relative_path(const char* p)
{
    if (p[0] == '\\' || p[0] == '/')
        return false;
    return !(((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

}  // namespace

int main(int argc, char** argv)
{
    printf(PL_TAG "ProjectPAULA launcher v" PAULA_LAUNCHER_VERSION "\n");

    // launcher.ini, and a relative TricksterPath, are taken from the launcher's own folder.
    char exe_dir[MAX_PATH];
    const DWORD exe_len = GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        printf(PL_TAG "ERROR: cannot read the launcher's own path (Windows error %lu).\n", GetLastError());
        return fail(1);
    }
    char* slash = strrchr(exe_dir, '\\');
    if (slash)
        slash[1] = '\0';
    char ini[MAX_PATH + 16];
    snprintf(ini, sizeof(ini), "%slauncher.ini", exe_dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
        printf(PL_TAG "launcher.ini not found next to PaulaLauncher.exe: using the defaults.\n");

    char client_setting[MAX_PATH], original_list[512], login_ip[64], account[64];
    GetPrivateProfileStringA("Launcher", "TricksterPath", "", client_setting, sizeof(client_setting), ini);
    GetPrivateProfileStringA("Launcher", "OriginalIP", "", original_list, sizeof(original_list), ini);
    GetPrivateProfileStringA("Server", "LoginIP", "", login_ip, sizeof(login_ip), ini);
    const UINT login_port = GetPrivateProfileIntA("Server", "LoginPort", kClientPort, ini);
    GetPrivateProfileStringA("Server", "Account", "", account, sizeof(account), ini);
    if (argc > 1 && argv[1][0])
        snprintf(account, sizeof(account), "%s", argv[1]);
    trim(client_setting);
    trim(original_list);
    trim(login_ip);
    trim(account);
    // A missing or empty key takes its default.
    if (!client_setting[0])
        snprintf(client_setting, sizeof(client_setting), "Trickster.bin");
    if (!original_list[0])
        snprintf(original_list, sizeof(original_list), "%s", kDefaultOriginalIPs);
    if (!login_ip[0])
        snprintf(login_ip, sizeof(login_ip), "127.0.0.1");

    if (!is_ipv4(login_ip)) {
        printf(PL_TAG "ERROR: [Server] LoginIP \"%s\" is not an IPv4 address (digits and dots, at most\n"
               PL_TAG "       15 characters; host names are not supported).\n", login_ip);
        return fail(1);
    }
    if (login_port == 0 || login_port > 65535) {
        printf(PL_TAG "ERROR: [Server] LoginPort must be 1-65535.\n");
        return fail(1);
    }
    if (!account[0]) {
        printf(PL_TAG "ERROR: no account name. Set [Server] Account in launcher.ini, or start\n"
               PL_TAG "       PaulaLauncher.exe <account>.\n");
        return fail(1);
    }
    if (!is_valid_account(account)) {
        printf(PL_TAG "ERROR: the account name \"%s\" is not valid: 1-18 characters, letters A-Z a-z,\n"
               PL_TAG "       digits and _ only.\n", account);
        return fail(1);
    }
    Pattern pats[kMaxPatterns];
    const int npats = parse_original_ips(original_list, pats, kMaxPatterns);
    if (npats < 0)
        return fail(1);

    char joined[MAX_PATH * 2];
    snprintf(joined, sizeof(joined), "%s%s", is_relative_path(client_setting) ? exe_dir : "", client_setting);
    char client[MAX_PATH];
    const DWORD client_len = GetFullPathNameA(joined, MAX_PATH, client, nullptr);
    if (client_len == 0 || client_len >= MAX_PATH) {
        printf(PL_TAG "ERROR: [Launcher] TricksterPath is not a valid path: %s\n", client_setting);
        return fail(2);
    }
    const DWORD attr = GetFileAttributesA(client);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        printf(PL_TAG "ERROR: the client was not found: %s\n"
               PL_TAG "       Put PaulaLauncher.exe and launcher.ini in your client folder, next to\n"
               PL_TAG "       Trickster.bin, or set [Launcher] TricksterPath in launcher.ini.\n", client);
        return fail(2);
    }
    char workdir[MAX_PATH];
    snprintf(workdir, sizeof(workdir), "%s", client);
    slash = strrchr(workdir, '\\');
    if (slash)
        slash[(slash == workdir + 2 && workdir[1] == ':') ? 1 : 0] = '\0';   // keep "C:\" for a drive root

    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s,%s,%s,Trickster,0,blank%s\"",
             client, login_ip, account, kGroupId, kToken);

    char ip_list[512] = "";
    for (int k = 0; k < npats; ++k) {
        const size_t used = strlen(ip_list);
        snprintf(ip_list + used, sizeof(ip_list) - used, "%s%s", k ? ", " : "", pats[k].ip);
    }
    printf(PL_TAG "Client:      %s\n", client);
    printf(PL_TAG "Server:      %s:%u\n", login_ip, login_port);
    printf(PL_TAG "Account:     %s\n", account);
    printf(PL_TAG "Replaces:    %s\n", ip_list);
    if (login_port != kClientPort)
        printf(PL_TAG "Note: LoginPort %u is not used - the supported clients always connect to port %u,\n"
               PL_TAG "      so the server's AuthServer must listen on %u.\n", login_port, kClientPort, kClientPort);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(client, cmdline, nullptr, nullptr, FALSE, 0, nullptr, workdir, &si, &pi)) {
        printf(PL_TAG "ERROR: cannot start the client (Windows error %lu).\n", GetLastError());
        return fail(3);
    }
    printf(PL_TAG "Client started (PID %lu). Waiting for it to initialize...\n", pi.dwProcessId);
    Sleep(kStartDelayMs);

    PatchStats st;
    for (int attempt = 1;; ++attempt) {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
            printf(PL_TAG "ERROR: the client closed right after it started (exit code %lu).\n", code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return fail(3);
        }
        printf(PL_TAG "Scanning the client's memory for the server IP...\n");
        scan_and_patch(pi.hProcess, pats, npats, login_ip, st);
        if (st.patched + st.already + st.skipped > 0 || attempt == kScanAttempts)
            break;
        Sleep(1000);
    }
    printf(PL_TAG "IP copies replaced: %d, already right: %d, left unchanged: %d\n",
           st.patched, st.already, st.skipped);

    if (st.patched + st.already == 0) {
        if (st.skipped == 0)
            printf(PL_TAG "ERROR: none of these addresses is in the client's memory: %s\n"
                   PL_TAG "       Set [Launcher] OriginalIP in launcher.ini to the server IP baked into your\n"
                   PL_TAG "       Trickster.bin.\n", ip_list);
        else
            printf(PL_TAG "ERROR: the server IP could not be put into the client (see above).\n");
        printf(PL_TAG "The client was closed, so that it does not connect to another server.\n");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return fail(4);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (st.skipped > 0) {
        printf(PL_TAG "WARNING: %d copies of the old address could not be replaced (see above). If the\n"
               PL_TAG "         client cannot reach the server, send this text to the server operator.\n",
               st.skipped);
        wait_for_key_if_own_console();
        return 0;
    }
    printf(PL_TAG "Done: the client connects to %s.\n", login_ip);
    return 0;
}
