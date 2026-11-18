/* Montero Probe: data-discovery tool for the Skypine 6982E head unit (Windows CE 6, ARM).
 *
 * Buttons (nothing runs until one is pressed):
 *   Survey     read-only: system info, windows, drivers, USB driver registry, folder listings
 *   Bus        joins Main.exe's message bus as "TerminalA" (an id nothing else uses), reads the
 *              shared-memory ring of ALL bus messages (backlog first, then live), logs each message
 *              type when its content changes, decodes the CarInfo replies Main.exe sends to the
 *              resident Car Info screen, and sends the factory screen's read requests. Main.exe
 *              routes requests by the sender's app id, so they are posted with sender 0x14.
 *   GPS        reads NMEA from COM1: (38400) and shows speed, course, altitude, satellites;
 *              position is never logged
 *   Ports      opens spare COM ports read-only, reports NMEA sentence types or a hex sample
 *   Next step  guided test: shows the next action and writes a marker into the log
 *   Exit       quits; it does NOT send EndCommunication (the resident Car Info screen owns that)
 * Always on: logs when Main.exe's climate popup (AirControlBar) appears and saves a few
 * screenshots of it, because fan speed never reaches the message bus. Reads the fan level and the
 * device status bytes (parking brake, lights, reverse) from Main.exe's memory, read-only. Maps
 * Main.exe's system status table (shared memory SkSysInfoShareMem) read-only, logs all of it at
 * start and exit and every change in between.
 *
 * Everything is appended to probe-log.txt next to the exe, flushed after every line.
 * Bus details come from SkCom.dll; see docs/vehicle-data-research.md.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "busring.h"
#include "carinfo.h"
#include "nmea.h"
#include "sysinfo.h"

#define PROBE_VERSION "0.5.0"

/* Message bus constants (decoded from SkCom.dll / Main.exe) */
#define APP_MAIN 0x00       /* bus window "NavigRecive" */
#define APP_CARINFO 0x14    /* bus window "CarInfo"; Main.exe routes 0x4A requests by arg1 == 0x14 */
#define APP_PROBE 0x1A      /* bus window "TerminalA" (third-party slot, unused on this unit) */
#define MSG_CARINFO 0x4A
#define BUS_SHM_SIZE 0x40000

/* SkCom.dll layout of firmware cne-6982e-a (477,696 bytes); verified before raw access */
#define RVA_MSGSESSION_INIT 0x1ee88
#define RVA_MSGSESSION_CONFIG 0x1ccbc
#define RVA_FN_GETDESTNAME 0x69a38
#define RVA_SHM_BASE_PTR 0x69a34

#define GPS_PORT L"COM1:"
#define GPS_BAUD 38400

#define ID_SURVEY 101
#define ID_BUS 102
#define ID_GPS 103
#define ID_PORTS 104
#define ID_EXIT 105
#define ID_STEP 106
#define TIMER_RING 1
#define TIMER_BURST 2
#define TIMER_REQUESTS 3
#define TIMER_SUMMARY 4
#define TIMER_WATCH 5
#define WM_PROBE_LOG (WM_APP + 1)
#define WM_PROBE_REFRESH (WM_APP + 2)

/* ---------------------------------------------------------------- logging */

static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;
static HWND g_main, g_edit, g_values;
static volatile LONG g_busy;

static void log_line(const char *fmt, ...)
{
    char text[600], line[700];
    SYSTEMTIME now;
    va_list args;
    DWORD written;
    int len;

    va_start(args, fmt);
    len = _vsnprintf(text, sizeof text - 1, fmt, args);
    va_end(args);
    text[len < 0 ? (int)sizeof text - 1 : len] = '\0';
    GetLocalTime(&now);
    len = _snprintf(line, sizeof line - 3, "%02d:%02d:%02d.%03lu %s", now.wHour, now.wMinute,
                    now.wSecond, GetTickCount() % 1000, text);
    if (len < 0)
        len = (int)sizeof line - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    line[len] = '\0';

    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        WriteFile(g_log_file, line, (DWORD)len, &written, NULL);
        FlushFileBuffers(g_log_file);
    }
    LeaveCriticalSection(&g_log_lock);

    if (g_main) {
        char *copy = (char *)LocalAlloc(LMEM_FIXED, (UINT)len + 1);
        if (copy) {
            memcpy(copy, line, (size_t)len + 1);
            if (!PostMessageW(g_main, WM_PROBE_LOG, 0, (LPARAM)copy))
                LocalFree(copy);
        }
    }
}

static void log_hex(const char *label, const unsigned char *data, int length, int max)
{
    char buf[3 * 32 + 1];
    int shown = length < max ? length : max;
    int i, row;

    log_line("%s (%d bytes)%s", label, length, shown < length ? ", truncated" : "");
    for (row = 0; row < shown; row += 32) {
        int n = 0;
        for (i = row; i < shown && i < row + 32; i++)
            n += _snprintf(buf + n, sizeof buf - (size_t)n, "%02x ", data[i]);
        buf[n] = '\0';
        log_line("  %04x: %s", row, buf);
    }
}

static void narrow(const WCHAR *in, char *out, int size)
{
    if (!WideCharToMultiByte(CP_ACP, 0, in, -1, out, size, NULL, NULL))
        out[0] = '\0';
    out[size - 1] = '\0';
}

static void append_to_edit(char *line)
{
    WCHAR wide[720];
    int len = GetWindowTextLengthW(g_edit);
    MultiByteToWideChar(CP_ACP, 0, line, -1, wide, 720);
    wide[719] = 0;
    if (len > 24000) { /* keep the on-screen log small; the file has everything */
        SendMessageW(g_edit, EM_SETSEL, 0, 12000);
        SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(g_edit);
    }
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)wide);
}

/* ---------------------------------------------------------------- survey (read-only) */

#define MAX_PORTS 24
static WCHAR g_ports[MAX_PORTS][16];
static int g_port_count;

static void log_reg_values(HKEY key, const char *indent)
{
    WCHAR name[128];
    BYTE data[512];
    char a[160], b[400];
    DWORD i, name_len, data_len, type;

    for (i = 0; i < 64; i++) {
        name_len = 128;
        data_len = sizeof data - 4;
        if (RegEnumValueW(key, i, name, &name_len, NULL, &type, data, &data_len) != ERROR_SUCCESS)
            break;
        narrow(name, a, sizeof a);
        if (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ) {
            WCHAR *w = (WCHAR *)data;
            DWORD k;
            data[data_len] = 0;
            data[data_len + 1] = 0;
            for (k = 0; type == REG_MULTI_SZ && k + 1 < data_len / 2; k++)
                if (w[k] == 0)
                    w[k] = L';';
            narrow(w, b, sizeof b);
            log_line("%s%s = \"%s\"", indent, a, b);
        } else if (type == REG_DWORD && data_len == 4) {
            log_line("%s%s = 0x%lx", indent, a, *(DWORD *)data);
        } else {
            log_line("%s%s = <type %lu, %lu bytes>", indent, a, type, data_len);
        }
    }
}

static void log_reg_tree(HKEY root, const WCHAR *path, int depth)
{
    HKEY key;
    WCHAR sub[128], child[320];
    char a[320];
    DWORD i, len;

    narrow(path, a, sizeof a);
    if (RegOpenKeyExW(root, path, 0, 0, &key) != ERROR_SUCCESS) {
        log_line("[reg] %s: not found", a);
        return;
    }
    log_line("[reg] %s", a);
    log_reg_values(key, "    ");
    for (i = 0; depth > 0 && i < 64; i++) {
        len = 128;
        if (RegEnumKeyExW(key, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        _snwprintf(child, 320, L"%s\\%s", path, sub);
        child[319] = 0;
        log_reg_tree(root, child, depth - 1);
    }
    RegCloseKey(key);
}

static int reg_string(HKEY key, const WCHAR *value, WCHAR *out, DWORD chars)
{
    DWORD type, size = chars * sizeof(WCHAR);
    if (RegQueryValueExW(key, value, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS || type != REG_SZ)
        return 0;
    out[chars - 1] = 0;
    return 1;
}

static void survey_drivers(int verbose)
{
    HKEY active, item, driver;
    WCHAR sub[64], path[96], name[64], key_path[200];
    char a[64], b[200];
    DWORD i, len;

    g_port_count = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Drivers\\Active", 0, 0, &active) != ERROR_SUCCESS) {
        log_line("[drivers] Drivers\\Active not found");
        return;
    }
    for (i = 0; i < 128; i++) {
        len = 64;
        if (RegEnumKeyExW(active, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        _snwprintf(path, 96, L"Drivers\\Active\\%s", sub);
        path[95] = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, 0, &item) != ERROR_SUCCESS)
            continue;
        name[0] = 0;
        key_path[0] = 0;
        reg_string(item, L"Name", name, 64);
        reg_string(item, L"Key", key_path, 200);
        RegCloseKey(item);
        if (verbose) {
            narrow(name, a, sizeof a);
            narrow(key_path, b, sizeof b);
            log_line("[drivers] %-8s %s", a, b);
            if (key_path[0] && RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, 0, &driver) == ERROR_SUCCESS) {
                log_reg_values(driver, "    ");
                RegCloseKey(driver);
            }
        }
        if (g_port_count < MAX_PORTS && wcsncmp(name, L"COM", 3) == 0) {
            wcsncpy(g_ports[g_port_count], name, 15);
            g_ports[g_port_count++][15] = 0;
        }
    }
    RegCloseKey(active);
}

/* Network adapters and their TCP/IP settings (for the USB tethering / USB network test). */
static void survey_network(void)
{
    HKEY comm, tcpip;
    WCHAR sub[64], path[128];
    char a[64];
    DWORD i, len;
    int found = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Comm", 0, 0, &comm) != ERROR_SUCCESS) {
        log_line("[network] HKLM\\Comm not found");
        return;
    }
    for (i = 0; i < 128; i++) {
        len = 64;
        if (RegEnumKeyExW(comm, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        _snwprintf(path, 128, L"Comm\\%s\\Parms\\TcpIp", sub);
        path[127] = 0;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, 0, &tcpip) != ERROR_SUCCESS)
            continue;
        narrow(sub, a, sizeof a);
        log_line("[network] adapter %s", a);
        log_reg_values(tcpip, "    ");
        RegCloseKey(tcpip);
        found++;
    }
    RegCloseKey(comm);
    log_line("[network] %d adapters with TCP/IP settings", found);
}

static int contains_nocase(const WCHAR *text, const char *needle)
{
    char a[MAX_PATH];
    size_t i, n = strlen(needle);
    narrow(text, a, sizeof a);
    for (i = 0; a[i]; i++)
        if (a[i] >= 'A' && a[i] <= 'Z')
            a[i] = (char)(a[i] - 'A' + 'a');
    for (i = 0; a[i]; i++)
        if (strncmp(a + i, needle, n) == 0)
            return 1;
    return 0;
}

static void list_dir(const WCHAR *dir, int depth, const char *const *filters)
{
    WIN32_FIND_DATAW found;
    WCHAR pattern[300], child[300];
    char a[300], b[300];
    HANDLE h;
    int count = 0;
    const WCHAR *sep = dir[wcslen(dir) - 1] == L'\\' ? L"" : L"\\";

    _snwprintf(pattern, 300, L"%s%s*", dir, sep);
    pattern[299] = 0;
    narrow(dir, a, sizeof a);
    h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("[dir] %s: cannot list (error %lu)", a, GetLastError());
        return;
    }
    log_line("[dir] %s%s", a, filters ? " (filtered)" : "");
    do {
        int show = filters == NULL, k;
        for (k = 0; filters && filters[k] && !show; k++)
            show = contains_nocase(found.cFileName, filters[k]);
        if (!show)
            continue;
        narrow(found.cFileName, b, sizeof b);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            log_line("    <dir>      %s", b);
            if (depth > 0 && count < 200) {
                _snwprintf(child, 300, L"%s%s%s", dir, sep, found.cFileName);
                child[299] = 0;
                list_dir(child, depth - 1, NULL);
            }
        } else {
            log_line("    %10lu %s", found.nFileSizeLow, b);
        }
    } while (++count < 1000 && FindNextFileW(h, &found));
    FindClose(h);
}

static BOOL CALLBACK log_window(HWND hwnd, LPARAM unused)
{
    WCHAR title[128], cls[128];
    char a[128], b[128];
    RECT r;
    DWORD pid = 0;

    title[0] = cls[0] = 0;
    GetWindowTextW(hwnd, title, 128);
    GetClassNameW(hwnd, cls, 128);
    GetWindowRect(hwnd, &r);
    GetWindowThreadProcessId(hwnd, &pid);
    narrow(title, a, sizeof a);
    narrow(cls, b, sizeof b);
    log_line("[window] %08lx pid %08lx %s (%ld,%ld %ldx%ld) class \"%s\" title \"%s\"",
             (unsigned long)hwnd, pid, IsWindowVisible(hwnd) ? "visible" : "hidden ",
             r.left, r.top, r.right - r.left, r.bottom - r.top, b, a);
    return TRUE;
}

static DWORD WINAPI survey_thread(LPVOID unused)
{
    static const char *const driver_files[] = {
        "usb", "ser", "ftdi", "cp210", "pl23", "acm", "cdc", "hid", "gps", "can", "obd",
        "sensor", "accel", "gyro", "mma", "bma", "lis3", "mpu", NULL};
    OSVERSIONINFOW os;
    WCHAR exe[MAX_PATH];
    char a[MAX_PATH];

    log_line("=== Survey (read-only)");
    os.dwOSVersionInfoSize = sizeof os;
    if (GetVersionExW(&os))
        log_line("[system] Windows CE %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion,
                 os.dwBuildNumber);
    log_line("[system] screen %dx%d", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    narrow(exe, a, sizeof a);
    log_line("[system] probe %s at %s", PROBE_VERSION, a);
    EnumWindows(log_window, 0);
    survey_drivers(1);
    survey_network();
    log_reg_tree(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\ClientDrivers", 2);
    log_reg_tree(HKEY_LOCAL_MACHINE, L"Drivers\\USB\\LoadClients", 4);
    list_dir(L"\\Windows", 0, driver_files);
    list_dir(L"\\NandFlash\\Application", 1, NULL);
    log_line("=== Survey done");
    InterlockedExchange((LONG *)&g_busy, 0);
    return 0;
}

/* ---------------------------------------------------------------- serial ports */

static int sample_port(HANDLE h, DWORD ms, int *nmea)
{
    unsigned char buf[1024];
    DWORD start = GetTickCount(), got, total = 0;
    int i;

    *nmea = 0;
    PurgeComm(h, PURGE_RXCLEAR);
    while (GetTickCount() - start < ms && total < sizeof buf - 1) {
        if (!ReadFile(h, buf + total, sizeof buf - 1 - total, &got, NULL))
            break;
        total += got;
    }
    for (i = 0; i + 1 < (int)total; i++)
        if (buf[i] == '$' && (buf[i + 1] == 'G' || buf[i + 1] == 'P'))
            *nmea = 1;
    if (*nmea) {
        /* Report only which sentences appear: the raw text contains the car's position. */
        NmeaState st;
        char line[128];
        int used = 0;
        memset(&st, 0, sizeof st);
        nmea_feed(&st, line, &used, sizeof line, (const char *)buf, (int)total);
        log_line("    %lu bytes, NMEA sentence types: %s", total, st.seen);
    } else if (total) {
        log_hex("    sample", buf, (int)total, 96);
    } else {
        log_line("    no data");
    }
    return (int)total;
}

static DWORD WINAPI port_scan_thread(LPVOID unused)
{
    static const DWORD rates[] = {4800, 9600, 19200, 38400, 57600, 115200};
    int i, r;

    log_line("=== Port scan (read-only opens; COM2/COM3/COM5 are never touched)");
    survey_drivers(0);
    for (i = 0; i < g_port_count; i++) {
        HANDLE h;
        DCB original, dcb;
        COMMTIMEOUTS timeouts = {50, 0, 200, 0, 0};
        char a[16];
        int have_dcb, nmea;

        narrow(g_ports[i], a, sizeof a);
        if (!wcscmp(g_ports[i], L"COM2:") || !wcscmp(g_ports[i], L"COM3:") || !wcscmp(g_ports[i], L"COM5:")) {
            log_line("[port] %s skipped (MCU, Bluetooth or disc)", a);
            continue;
        }
        h = CreateFileW(g_ports[i], GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            log_line("[port] %s cannot open (error %lu)", a, GetLastError());
            continue;
        }
        original.DCBlength = sizeof original;
        have_dcb = GetCommState(h, &original);
        SetCommTimeouts(h, &timeouts);
        log_line("[port] %s opened, current speed %lu", a, have_dcb ? original.BaudRate : 0);
        sample_port(h, 1500, &nmea);
        for (r = 0; have_dcb && !nmea && r < (int)(sizeof rates / sizeof rates[0]); r++) {
            dcb = original;
            dcb.BaudRate = rates[r];
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            if (!SetCommState(h, &dcb))
                continue;
            log_line("[port] %s at %lu", a, rates[r]);
            sample_port(h, 1500, &nmea);
        }
        if (have_dcb)
            SetCommState(h, &original);
        CloseHandle(h);
    }
    log_line("=== Port scan done");
    InterlockedExchange((LONG *)&g_busy, 0);
    return 0;
}

/* ---------------------------------------------------------------- GPS live */

static NmeaState g_gps;
static volatile LONG g_gps_running;
static long g_fan_level = -1, g_fan_max = -1; /* from the climate reader; -1 = unknown */
static HANDLE g_gps_handle = INVALID_HANDLE_VALUE;

static DWORD WINAPI gps_thread(LPVOID unused)
{
    char raw[256], line[128];
    int used = 0;
    DWORD got, last_log = 0;
    DCB dcb;
    COMMTIMEOUTS timeouts = {50, 0, 200, 0, 0};
    HANDLE h = CreateFileW(GPS_PORT, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (h == INVALID_HANDLE_VALUE) {
        log_line("[gps] cannot open COM1: (error %lu)", GetLastError());
        InterlockedExchange((LONG *)&g_gps_running, 0);
        return 0;
    }
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(h, &dcb) && dcb.BaudRate != GPS_BAUD) {
        dcb.BaudRate = GPS_BAUD;
        SetCommState(h, &dcb);
    }
    SetCommTimeouts(h, &timeouts);
    g_gps_handle = h;
    log_line("[gps] reading COM1: at %d (position is not logged)", GPS_BAUD);
    while (g_gps_running) {
        if (!ReadFile(h, raw, sizeof raw, &got, NULL))
            break;
        if (got) {
            nmea_feed(&g_gps, line, &used, sizeof line, raw, (int)got);
            PostMessageW(g_main, WM_PROBE_REFRESH, 0, 0);
        }
        if (GetTickCount() - last_log > 5000) {
            last_log = GetTickCount();
            log_line("[gps] fix %s quality %d sats %d | %d.%d km/h | course %d.%d | alt %d.%d m | %u sentences, %u bad",
                     g_gps.valid ? "valid" : "none", g_gps.fix_quality, g_gps.satellites,
                     g_gps.speed_dkmh / 10, g_gps.speed_dkmh % 10, g_gps.course_ddeg / 10, g_gps.course_ddeg % 10,
                     g_gps.altitude_dm / 10, (g_gps.altitude_dm < 0 ? -g_gps.altitude_dm : g_gps.altitude_dm) % 10,
                     g_gps.sentences, g_gps.bad_checksums);
        }
    }
    CloseHandle(h);
    g_gps_handle = INVALID_HANDLE_VALUE;
    log_line("[gps] stopped; sentence types seen: %s", g_gps.seen);
    return 0;
}

/* ---------------------------------------------------------------- message bus */

typedef void (*config_fn)(int sender, const char *(*dest_name)(int));
typedef void *(*session_ctor_fn)(void *self);
typedef int (*session_init_fn)(void *self, void *proc);
typedef int (*post_fn)(int dest, int what, int arg1, int arg2, void *parcelable);
typedef void *(*obtain_fn)(void **result);
typedef void (*bundle_put_fn)(void *bundle, void *const *key, const void *data, int size);
typedef void *(*string_ctor_fn)(void **self, const char *text);
typedef int (*dec_ref_fn)(void *object);

#define MAX_PAIRS 128
#define MAX_OWN 64

static struct {
    int active;
    unsigned char *skcom_base;
    unsigned char *shm;
    config_fn config;
    const char *(*dest_name)(int);
    session_ctor_fn session_ctor;
    session_init_fn session_init;
    post_fn post;
    obtain_fn bundle_obtain;
    bundle_put_fn bundle_put;
    string_ctor_fn string_ctor;
    dec_ref_fn dec_ref;
    void *key_data; /* juce::String "data" (one pointer) */
    void *session;
    BusRing ring;
    CarInfoState state;
    unsigned records, carinfo_replies, tx_count, tx_failed, own_seen, window_msgs;
    struct { int what, sender; unsigned count, hash; } pairs[MAX_PAIRS];
    int pair_count;
    struct { unsigned start, end; } own[MAX_OWN]; /* ring ranges written by our own posts */
    int own_next;
} g_bus;

/* Guided test: the user presses "Next step" when starting each action; the log gets a marker. */
static const char *const g_steps[] = {
    "Baseline: touch nothing for 10 seconds",
    "Parking (small) lights ON 5 s, then OFF",
    "Headlights ON 5 s, then OFF",
    "Hazard lights ON 5 s, then OFF",
    "Foot firmly on the brake: shift to R, wait 5 s, back to P",
    "Foot on the brake: release the parking brake 5 s, apply it again",
    "Driver's seatbelt: unbuckle 5 s, then buckle again",
    "Driver's door: open 5 s, then close",
    "Get out: front passenger, rear left, rear right door, then tailgate, 5 s each",
    "Key cars only: key to ACC 10 s (engine off), key to ON 10 s, start the engine",
    "Done: if you can, leave this running 10 minutes, then press Exit",
};
static int g_step = -1;
static unsigned g_sysinfo_changes; /* from the status table reader */

static void update_values(void)
{
    char line[520];
    WCHAR wide[520];
    int n = carinfo_format(&g_bus.state, line, 200);
    n += _snprintf(line + n, sizeof line - (size_t)n,
                   "\r\nGPS %s, %d sats | %d.%d km/h | course %d | alt %d m",
                   !g_gps.have_rmc ? "--" : (g_gps.valid ? "fix" : "no fix"), g_gps.satellites,
                   g_gps.speed_dkmh / 10, g_gps.speed_dkmh % 10, g_gps.course_ddeg / 10, g_gps.altitude_dm / 10);
    if (g_fan_level >= 0)
        n += _snprintf(line + n, sizeof line - (size_t)n, " | Fan %ld/%ld", g_fan_level, g_fan_max);
    n += _snprintf(line + n, sizeof line - (size_t)n,
                   "\r\nbus records %u, CarInfo %u, sent %u (%u failed), status changes %u",
                   g_bus.records, g_bus.carinfo_replies, g_bus.tx_count, g_bus.tx_failed, g_sysinfo_changes);
    _snprintf(line + n, sizeof line - (size_t)n, "\r\n%s%s",
              g_step < 0 ? "Press Next step to start the guided test" : "STEP: ",
              g_step < 0 ? "" : g_steps[g_step]);
    line[sizeof line - 1] = '\0';
    MultiByteToWideChar(CP_ACP, 0, line, -1, wide, 520);
    wide[519] = 0;
    SetWindowTextW(g_values, wide);
}

static void next_step(void)
{
    if (g_step + 1 < (int)(sizeof g_steps / sizeof g_steps[0]))
        g_step++;
    log_line("=== STEP %d: %s", g_step + 1, g_steps[g_step]);
    update_values();
}

static void rc_release(void *object)
{
    if (object != NULL && (g_bus.dec_ref(object) & 0xff))
        ((void (*)(void *, unsigned))(*(void ***)object)[0])(object, 1);
}

static unsigned fnv1a(const unsigned char *p, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Returns 1 when this (what, sender) pair is new or its content changed since the last one. */
static int pair_changed(const BusHeader *h, const unsigned char *m, int length)
{
    unsigned hash = fnv1a(m + 8, length - 8); /* skip magic and id, keep args, sender and bundle */
    int i;
    for (i = 0; i < g_bus.pair_count; i++)
        if (g_bus.pairs[i].what == h->what && g_bus.pairs[i].sender == h->sender) {
            g_bus.pairs[i].count++;
            if (g_bus.pairs[i].hash == hash)
                return 0;
            g_bus.pairs[i].hash = hash;
            return 1;
        }
    if (g_bus.pair_count < MAX_PAIRS) {
        g_bus.pairs[g_bus.pair_count].what = h->what;
        g_bus.pairs[g_bus.pair_count].sender = h->sender;
        g_bus.pairs[g_bus.pair_count].hash = hash;
        g_bus.pairs[g_bus.pair_count++].count = 1;
    }
    return 1;
}

static void log_pair_summary(void)
{
    int i;
    log_line("[bus] message summary (what / sender: count)");
    for (i = 0; i < g_bus.pair_count; i++)
        log_line("    0x%04x / 0x%02x: %u", g_bus.pairs[i].what, g_bus.pairs[i].sender, g_bus.pairs[i].count);
}

static int is_own_record(unsigned offset)
{
    int i;
    for (i = 0; i < MAX_OWN; i++)
        if (g_bus.own[i].end > g_bus.own[i].start && offset >= g_bus.own[i].start && offset < g_bus.own[i].end)
            return 1;
    return 0;
}

static void on_ring_record(void *context, unsigned offset, const unsigned char *message, int length)
{
    static unsigned char copy[4096];
    char text[400], hex[3 * 24 + 1];
    const unsigned char *data;
    BusHeader h;
    int size, i;

    g_bus.records++;
    if (is_own_record(offset)) {
        g_bus.own_seen++;
        return;
    }
    if (length <= 0 || length > (int)sizeof copy) {
        log_line("[ring] 0x%05x: %d-byte message not decoded (size)", offset, length);
        return;
    }
    memcpy(copy, message, (size_t)length); /* other processes may overwrite the ring meanwhile */
    if (!busring_message_header(copy, length, &h)) {
        log_line("[ring] 0x%05x: %d bytes without RMSG header", offset, length);
        return;
    }
    if (h.what == MSG_CARINFO) {
        size = busring_bundle_bytes(copy, length, "data", &data);
        if (size < 1) {
            log_line("[ring] CarInfo message without data, sender 0x%02x", h.sender);
            return;
        }
        for (i = 0; i < size && i < 24; i++)
            _snprintf(hex + 3 * i, 4, "%02x ", data[i]);
        hex[3 * (size < 24 ? size : 24) - 1] = '\0';
        if (h.sender == APP_MAIN) {
            int updated = carinfo_apply(&g_bus.state, data, size);
            g_bus.carinfo_replies++;
            if (updated < 0) {
                log_line("[carinfo] %s (not decoded)", hex);
            } else {
                carinfo_format(&g_bus.state, text, sizeof text);
                log_line("[carinfo] %s -> %s", hex, text);
            }
        } else {
            log_line("[carinfo] request %s from sender 0x%02x", hex, h.sender);
        }
        return;
    }
    if (!pair_changed(&h, copy, length))
        return;
    busring_bundle_format(copy, length, text, sizeof text);
    log_line("[ring] what 0x%04x arg1 0x%x arg2 0x%x sender 0x%02x %s", h.what, h.arg1, h.arg2, h.sender, text);
}

static void bus_poll_ring(void)
{
    int n;
    do {
        n = busring_poll(&g_bus.ring, g_bus.shm, BUS_SHM_SIZE, 64, on_ring_record, NULL);
    } while (n == 64);
    if (n < 0) {
        log_line("[ring] shared-memory header no longer valid; stopping the monitor");
        KillTimer(g_main, TIMER_RING);
    }
    update_values();
}

/* Fake IMsgProc: vtable slot 0 destructor, 1 procOwnerMsg, 2 procUnknownMsg. */
static void *proc_destructor(void *self, unsigned flags) { return self; }

static void proc_owner_msg(void *self, void **message_ref)
{
    unsigned char *m = message_ref ? (unsigned char *)*message_ref : NULL;
    if (m != NULL && *(int *)(m + 0x08) != 0x6D3) /* 0x6D3 is Main.exe's heartbeat */
        log_line("[rx] TerminalA got what 0x%x arg1 0x%x arg2 0x%x sender 0x%x", *(int *)(m + 0x08),
                 *(int *)(m + 0x0c), *(int *)(m + 0x10), *(int *)(m + 0x18));
}

static void proc_unknown_msg(void *self, int msg, int wparam, int lparam)
{
    static int last_msg, last_w, last_l;
    if (msg == 0xC000 && ++g_bus.window_msgs > 3)
        return; /* periodic system broadcast */
    if (msg == last_msg && wparam == last_w && lparam == last_l)
        return;
    last_msg = msg;
    last_w = wparam;
    last_l = lparam;
    log_line("[rx] TerminalA window message 0x%x wParam 0x%x lParam 0x%x", msg, wparam, lparam);
}

static void *proc_spare(void *self)
{
    log_line("[rx] unexpected IMsgProc slot called");
    return NULL;
}

static void *g_proc_vtable[8] = {
    (void *)proc_destructor, (void *)proc_owner_msg, (void *)proc_unknown_msg, (void *)proc_spare,
    (void *)proc_spare, (void *)proc_spare, (void *)proc_spare, (void *)proc_spare,
};
static void *g_proc = g_proc_vtable; /* object whose first word is the vtable pointer */

/* Post one CarInfo request. Main.exe routes requests by the sender's app id, so the sender is
 * switched to AppCarInfo (0x14) for the duration of the post, exactly as the factory screen sends
 * it; the probe's own window stays "TerminalA". */
static int bus_send(const unsigned char *bytes, int size)
{
    void *bundle = NULL;
    unsigned before, after;
    int ok;

    g_bus.bundle_obtain(&bundle);
    if (bundle == NULL)
        return 0;
    g_bus.bundle_put(bundle, &g_bus.key_data, bytes, size);
    before = *(volatile unsigned *)(g_bus.shm + 8);
    g_bus.config(APP_CARINFO, g_bus.dest_name);
    ok = g_bus.post(APP_MAIN, MSG_CARINFO, APP_CARINFO, 0, bundle) & 0xff;
    g_bus.config(APP_PROBE, g_bus.dest_name);
    after = *(volatile unsigned *)(g_bus.shm + 8);
    if (after > before) { /* remember our record so the ring log can skip it */
        g_bus.own[g_bus.own_next].start = before;
        g_bus.own[g_bus.own_next].end = after;
        g_bus.own_next = (g_bus.own_next + 1) % MAX_OWN;
    }
    /* same order as CarInfoApi::copyDataToHost: recycle (vtable slot 4), then drop our reference */
    ((void (*)(void *))(*(void ***)bundle)[4])(bundle);
    rc_release(bundle);
    g_bus.tx_count++;
    if (!ok)
        g_bus.tx_failed++;
    return ok;
}

static void send_requests(const unsigned char *const *frames, const int *sizes, int count, const char *label)
{
    char text[160];
    int i, n = 0, ok = 0;
    for (i = 0; i < count; i++) {
        int k;
        ok += bus_send(frames[i], sizes[i]);
        for (k = 0; k < sizes[i] && n < (int)sizeof text - 4; k++)
            n += _snprintf(text + n, sizeof text - (size_t)n, "%02x%s", frames[i][k], k + 1 < sizes[i] ? " " : "");
        if (i + 1 < count && n < (int)sizeof text - 3)
            n += _snprintf(text + n, sizeof text - (size_t)n, ", ");
    }
    text[sizeof text - 1] = '\0';
    log_line("[tx] %s: %s (%d of %d posted)", label, text, ok, count);
}

static FARPROC need(HMODULE module, const WCHAR *name, int *missing)
{
    FARPROC p = GetProcAddressW(module, name);
    if (p == NULL) {
        char a[200];
        narrow(name, a, sizeof a);
        log_line("[bus] missing export %s", a);
        (*missing)++;
    }
    return p;
}

static void bus_start(void)
{
    static const unsigned char start_comm[] = {0x81, 0x01, 0x01};
    static const unsigned char *const start_frames[] = {start_comm};
    static const int start_sizes[] = {3};
    HMODULE skcom, skgui;
    unsigned char *fn_slot;
    unsigned start;
    int missing = 0, ok, n;

    if (g_bus.active) {
        log_line("[bus] already running");
        return;
    }
    log_line("=== Bus monitor");
    log_line("[bus] CarInfo window %s, Main window %s", FindWindowW(L"CarInfo", L"CarInfo") ? "present" : "absent",
             FindWindowW(L"NavigRecive", L"NavigRecive") ? "present" : "absent");
    if (FindWindowW(L"TerminalA", L"TerminalA")) {
        log_line("[bus] a TerminalA window already exists; not joining");
        return;
    }
    skcom = LoadLibraryW(L"SkCom.dll");
    if (skcom == NULL)
        skcom = LoadLibraryW(L"\\NandFlash\\Application\\Bin\\SkCom.dll");
    skgui = LoadLibraryW(L"SkGUI.dll");
    if (skgui == NULL)
        skgui = LoadLibraryW(L"\\NandFlash\\Application\\Bin\\SkGUI.dll");
    if (skcom == NULL || skgui == NULL) {
        log_line("[bus] cannot load SkCom.dll / SkGUI.dll (error %lu)", GetLastError());
        return;
    }

    g_bus.config = (config_fn)need(skcom, L"?config@MsgSession@@SAXHP6APBDH@Z@Z", &missing);
    g_bus.dest_name = (const char *(*)(int))need(skcom, L"getDestName", &missing);
    g_bus.session_ctor = (session_ctor_fn)need(skcom, L"??0MsgSession@@QAA@XZ", &missing);
    g_bus.session_init = (session_init_fn)need(skcom, L"?init@MsgSession@@QAA_NPAVIMsgProc@@@Z", &missing);
    g_bus.post = (post_fn)need(skcom, L"?postMessage@MessageHelper@IPCMsg@@SA_NHHHHPAVParcelable@@@Z", &missing);
    g_bus.bundle_obtain = (obtain_fn)need(
        skcom, L"?obtain@Bundle@@SA?AV?$ReferenceCountedObjectPtr@VBundle@@@juce@@XZ", &missing);
    g_bus.bundle_put = (bundle_put_fn)need(skcom, L"?putByteArray@Bundle@@QAAXABVString@juce@@PBXH@Z", &missing);
    g_bus.string_ctor = (string_ctor_fn)need(skgui, L"??0String@juce@@QAA@PBD@Z", &missing);
    g_bus.dec_ref = (dec_ref_fn)need(
        skgui, L"?decReferenceCountWithoutDeleting@ReferenceCountedObject@juce@@QAA_NXZ", &missing);
    fn_slot = (unsigned char *)need(skcom, L"?m_fnGetDestName@MsgSession@@0P6APBDH@ZA", &missing);
    if (missing) {
        log_line("[bus] %d exports missing; not starting", missing);
        return;
    }
    g_bus.skcom_base = (unsigned char *)g_bus.session_init - RVA_MSGSESSION_INIT;
    if ((unsigned char *)g_bus.config - g_bus.skcom_base != RVA_MSGSESSION_CONFIG ||
        fn_slot - g_bus.skcom_base != RVA_FN_GETDESTNAME) {
        log_line("[bus] SkCom.dll is a different build than cne-6982e-a; not starting");
        return;
    }

    g_bus.string_ctor(&g_bus.key_data, "data");
    g_bus.config(APP_PROBE, g_bus.dest_name);
    g_bus.session = LocalAlloc(LPTR, 32);
    g_bus.session_ctor(g_bus.session);
    log_line("[bus] MsgSession::init as TerminalA (0x1A) ...");
    ok = g_bus.session_init(g_bus.session, &g_proc) & 0xff;
    g_bus.shm = *(unsigned char **)(g_bus.skcom_base + RVA_SHM_BASE_PTR);
    log_line("[bus] init returned %d, window %08lx, shared memory %08lx", ok,
             (unsigned long)FindWindowW(L"TerminalA", L"TerminalA"), (unsigned long)g_bus.shm);
    if (!ok || g_bus.shm == NULL || !busring_header_ok(g_bus.shm, BUS_SHM_SIZE)) {
        log_line("[bus] cannot use the shared-memory ring; not starting");
        return;
    }
    g_bus.active = 1;

    /* backlog: everything in the current lap of the ring, oldest first */
    start = *(unsigned *)(g_bus.shm + 4);
    g_bus.ring.cursor = start;
    log_line("[ring] reading backlog from 0x%x to 0x%x", start, *(unsigned *)(g_bus.shm + 8));
    do {
        n = busring_poll(&g_bus.ring, g_bus.shm, BUS_SHM_SIZE, 256, on_ring_record, NULL);
    } while (n == 256);
    log_line("[ring] backlog done: %u records, %u CarInfo replies", g_bus.records, g_bus.carinfo_replies);
    log_pair_summary();
    SetTimer(g_main, TIMER_RING, 100, NULL);
    SetTimer(g_main, TIMER_SUMMARY, 60000, NULL);

    /* same order as the factory screen: start, then requests one second later */
    send_requests(start_frames, start_sizes, 1, "start");
    SetTimer(g_main, TIMER_BURST, 1000, NULL);
    update_values();
}

static void bus_timer(UINT id)
{
    static const unsigned char compass[] = {0x11, 0x00}, pressure[] = {0x21, 0x00},
                               maintain[] = {0xA0, 0x02, 0x40}, rv_meter[] = {0x00, 0x00},
                               outside[] = {0x31, 0x00}, can[] = {0x41, 0x00};
    static const unsigned char *const once[] = {compass, pressure, maintain, rv_meter, outside, can};
    static const int once_sizes[] = {2, 2, 3, 2, 2, 2};
    static const unsigned char *const repeat[] = {compass, pressure, maintain, outside};
    static const int repeat_sizes[] = {2, 2, 3, 2};
    static unsigned rounds;

    if (!g_bus.active)
        return;
    switch (id) {
    case TIMER_RING:
        bus_poll_ring();
        break;
    case TIMER_SUMMARY:
        log_pair_summary();
        break;
    case TIMER_BURST: /* first round one second after StartCommunication, then every 5 seconds */
        KillTimer(g_main, TIMER_BURST);
        send_requests(once, once_sizes, 6, "first requests");
        SetTimer(g_main, TIMER_REQUESTS, 5000, NULL);
        break;
    case TIMER_REQUESTS:
        rounds++;
        if (rounds <= 3 || g_bus.tx_failed)
            send_requests(repeat, repeat_sizes, 4, "requests");
        else { /* stay quiet in the log once it works */
            int i;
            for (i = 0; i < 4; i++)
                bus_send(repeat[i], repeat_sizes[i]);
        }
        break;
    }
}

/* ---------------------------------------------------------------- climate popup watcher */

#define MAX_SHOTS 12
static int g_shots;

/* Save a screen rectangle as a 24-bit BMP next to the exe. Read-only on the system. */
static void save_screen_rect(const RECT *r, int index)
{
    WCHAR path[MAX_PATH], *slash;
    BITMAPINFO bi;
    HDC screen, mem;
    HBITMAP bmp, old;
    void *bits = NULL;
    unsigned char header[14];
    int w = r->right - r->left, h = r->bottom - r->top;
    DWORD stride, size, written;
    HANDLE f;

    if (w <= 0 || h <= 0 || w > 1024 || h > 768)
        return;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    _snwprintf(slash ? slash + 1 : path, 40, L"aircontrol_%02d.bmp", index);
    path[MAX_PATH - 1] = 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (mem == NULL || bmp == NULL || bits == NULL) {
        log_line("[climate] screenshot failed (error %lu)", GetLastError());
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return;
    }
    old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r->left, r->top, SRCCOPY);
    stride = ((DWORD)w * 3 + 3) & ~3u;
    size = stride * (DWORD)h;
    memset(header, 0, sizeof header);
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char)((54 + size) & 0xFF);
    header[3] = (unsigned char)(((54 + size) >> 8) & 0xFF);
    header[4] = (unsigned char)(((54 + size) >> 16) & 0xFF);
    header[5] = (unsigned char)(((54 + size) >> 24) & 0xFF);
    header[10] = 54;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, header, 14, &written, NULL);
        WriteFile(f, &bi.bmiHeader, 40, &written, NULL);
        WriteFile(f, bits, size, &written, NULL);
        CloseHandle(f);
        log_line("[climate] saved aircontrol_%02d.bmp (%dx%d at %ld,%ld)", index, w, h, r->left, r->top);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

/* Main.exe shows its AirControlBar popup when the climate settings change. Fan speed never
 * reaches the message bus, so log when the popup appears and keep a few screenshots of it. */
static void watch_climate_popup(void)
{
    static HWND popup;
    static int was_visible;
    static DWORD last_shot;
    int visible;
    RECT r;

    if (popup == NULL || !IsWindow(popup))
        popup = FindWindowW(NULL, L"AirControlBar");
    if (popup == NULL)
        return;
    visible = IsWindowVisible(popup) ? 1 : 0;
    if (visible != was_visible) {
        GetWindowRect(popup, &r);
        log_line("[climate] AirControlBar %s", visible ? "shown" : "hidden");
        was_visible = visible;
        last_shot = 0;
    }
    if (visible && g_shots < MAX_SHOTS && GetTickCount() - last_shot > 1000) {
        Sleep(150); /* let Main.exe finish drawing the new value */
        GetWindowRect(popup, &r);
        save_screen_rect(&r, ++g_shots);
        last_shot = GetTickCount();
    }
}

/* ---------------------------------------------------------------- climate reader (read-only) */

/* Main.exe (cne-6982e-a, 791,040 bytes) never puts climate data on the bus; it calls its own
 * AirControlBar popup directly (message 0x53B). The popup object keeps the fan level in its
 * CSPProgressBar, so read it from Main.exe's memory:
 *   [0xbe4b8] -> AirControlBar object, +0xe4 -> CSPProgressBar, bar +0x74 min, +0x78 max, +0x7c value
 *   object +0x100 / +0x108: last temperature value (kinds 0 and 1, raw)
 * The object only exists after the first climate change since the head unit started. */
#define MAIN_EXE_SIZE 791040
#define VA_AIRCONTROL_PTR 0xbe4b8
#define OFF_FAN_BAR 0xe4
#define OFF_TEMP_A 0x100
#define OFF_TEMP_B 0x108

static struct {
    int state; /* 0 not started, 1 reading, -1 unavailable */
    HANDLE process;
    DWORD bar_vtable, object;
    long fan, fan_max, temp_a, temp_b;
    int logged_none, logged_vtable, logged_range;
} g_climate = {0, NULL, 0, 0, -1, -1, -1, -1, 0, 0, 0};

/* Looked up at run time so the probe still starts if this COREDLL lacks them. */
typedef HANDLE (WINAPI *open_process_fn)(DWORD, BOOL, DWORD);
typedef BOOL (WINAPI *read_memory_fn)(HANDLE, LPCVOID, LPVOID, DWORD, LPDWORD);
static open_process_fn g_open_process;
static read_memory_fn g_read_memory;

static int remote_u32(DWORD address, DWORD *out)
{
    DWORD got = 0;
    return g_read_memory(g_climate.process, (LPCVOID)address, out, 4, &got) && got == 4;
}

static void climate_open(void)
{
    WIN32_FIND_DATAW found;
    HANDLE f = FindFirstFileW(L"\\NandFlash\\Application\\Bin\\Main.exe", &found);
    HWND main_window = FindWindowW(L"NavigRecive", L"NavigRecive");
    HMODULE gui;
    DWORD pid = 0;

    g_climate.state = -1;
    if (f == INVALID_HANDLE_VALUE) {
        log_line("[climate] Main.exe not found; fan reading off");
        return;
    }
    FindClose(f);
    if (found.nFileSizeLow != MAIN_EXE_SIZE) {
        log_line("[climate] Main.exe is %lu bytes, not the analysed build; fan reading off", found.nFileSizeLow);
        return;
    }
    if (main_window == NULL || !GetWindowThreadProcessId(main_window, &pid) || pid == 0) {
        log_line("[climate] Main.exe window not found; fan reading off");
        return;
    }
    {
        HMODULE core = GetModuleHandleW(L"coredll.dll");
        g_open_process = core ? (open_process_fn)GetProcAddressW(core, L"OpenProcess") : NULL;
        g_read_memory = core ? (read_memory_fn)GetProcAddressW(core, L"ReadProcessMemory") : NULL;
    }
    if (g_open_process == NULL || g_read_memory == NULL) {
        log_line("[climate] OpenProcess/ReadProcessMemory not available; fan reading off");
        return;
    }
    g_climate.process = g_open_process(PROCESS_VM_READ, FALSE, pid);
    if (g_climate.process == NULL)
        g_climate.process = g_open_process(0, FALSE, pid);
    if (g_climate.process == NULL) {
        log_line("[climate] OpenProcess failed (error %lu); fan reading off", GetLastError());
        return;
    }
    gui = LoadLibraryW(L"SkGUI.dll");
    if (gui == NULL)
        gui = LoadLibraryW(L"\\NandFlash\\Application\\Bin\\SkGUI.dll");
    if (gui != NULL)
        g_climate.bar_vtable = (DWORD)GetProcAddressW(gui, L"??_7CSPProgressBar@@6B@");
    g_climate.state = 1;
    log_line("[climate] reading Main.exe memory read-only (pid %08lx)", pid);
}

static void climate_read(void)
{
    DWORD object, bar, vtable, lo, hi, value, temp_a, temp_b;

    if (g_climate.state == 0)
        climate_open();
    if (g_climate.state != 1)
        return;
    if (!remote_u32(VA_AIRCONTROL_PTR, &object)) {
        log_line("[climate] ReadProcessMemory failed (error %lu); fan reading off", GetLastError());
        g_climate.state = -1;
        return;
    }
    if (object == 0) {
        if (!g_climate.logged_none++)
            log_line("[climate] popup object not created yet; it appears after the first fan or A/C change");
        return;
    }
    if (object != g_climate.object) {
        g_climate.object = object;
        log_line("[climate] AirControlBar object at %08lx", object);
    }
    if (!remote_u32(object + OFF_FAN_BAR, &bar) || bar == 0 || !remote_u32(bar, &vtable) ||
        !remote_u32(bar + 0x74, &lo) || !remote_u32(bar + 0x78, &hi) || !remote_u32(bar + 0x7c, &value) ||
        !remote_u32(object + OFF_TEMP_A, &temp_a) || !remote_u32(object + OFF_TEMP_B, &temp_b))
        return;
    if (vtable != g_climate.bar_vtable && !g_climate.logged_vtable++)
        log_line("[climate] fan bar vtable %08lx, ours %08lx (DLLs may load at different addresses; "
                 "relying on the range check)", vtable, g_climate.bar_vtable);
    if (lo != 0 || hi == 0 || hi > 20 || value > hi) {
        if (!g_climate.logged_range++)
            log_line("[climate] fan bar values out of range (min %lu max %lu value %lu); not trusted", lo, hi, value);
        return;
    }
    if ((long)value != g_climate.fan || (long)hi != g_climate.fan_max) {
        g_climate.fan = g_fan_level = (long)value;
        g_climate.fan_max = g_fan_max = (long)hi;
        log_line("[climate] fan %lu of %lu", value, hi);
        update_values();
    }
    if ((long)temp_a != g_climate.temp_a || (long)temp_b != g_climate.temp_b) {
        g_climate.temp_a = (long)temp_a;
        g_climate.temp_b = (long)temp_b;
        log_line("[climate] temperature fields +0x100 = %ld, +0x108 = %ld (raw)", (long)temp_a, (long)temp_b);
    }
}

/* Main.exe's DeviceStatusManager splits the MCU's 16-bit device status word (GET_DEVICE_STATUS_RESP,
 * incoming command 0x03) into bytes at object +0x4C (25 bytes; the previous copy follows at +0x65).
 * Only three are broadcast: +0x4D lights (system info 9), +0x4E reverse (8), +0x4F unknown (5).
 * +0x4C (status bit 0x0100) is never broadcast and is probably the parking brake. Decoded from
 * Main.exe: singleton getter RVA 0x21700 keeps the object at [0xbe4cc], the constructor (0x213ac)
 * writes vtable 0x9bce8, the change handler is 0x6a94c. Read-only, with the same process handle and
 * Main.exe size check as the fan reader. */
#define VA_DEVSTATUS_PTR 0xbe4cc
#define DEVSTATUS_VTABLE 0x9bce8
#define OFF_DEVSTATUS 0x4c
#define DEVSTATUS_SIZE 25

static struct {
    int state; /* 0 no baseline yet, 1 reading, -1 off */
    DWORD object;
    unsigned char last[DEVSTATUS_SIZE];
    int logged_none;
} g_devstatus;

static void devstatus_read(void)
{
    unsigned char now[DEVSTATUS_SIZE];
    DWORD object, vtable = 0, got = 0;
    int i;

    if (g_climate.state != 1 || g_devstatus.state < 0)
        return;
    if (!remote_u32(VA_DEVSTATUS_PTR, &object))
        return;
    if (object == 0) {
        if (!g_devstatus.logged_none++)
            log_line("[devstatus] DeviceStatusManager not created yet");
        return;
    }
    if (object != g_devstatus.object) {
        if (!remote_u32(object, &vtable) || vtable != DEVSTATUS_VTABLE) {
            log_line("[devstatus] object at %08lx is not the DeviceStatusManager (vtable %08lx); reading off",
                     object, vtable);
            g_devstatus.state = -1;
            return;
        }
        g_devstatus.object = object;
        g_devstatus.state = 0;
    }
    if (!g_read_memory(g_climate.process, (LPCVOID)(object + OFF_DEVSTATUS), now, DEVSTATUS_SIZE, &got) ||
        got != DEVSTATUS_SIZE)
        return;
    if (g_devstatus.state == 0) {
        log_hex("[devstatus] DeviceStatusManager +0x4c..+0x64 at start (+0x4c probably parking brake, "
                "+0x4d lights, +0x4e reverse, +0x4f system info 5)", now, DEVSTATUS_SIZE, DEVSTATUS_SIZE);
        memcpy(g_devstatus.last, now, DEVSTATUS_SIZE);
        g_devstatus.state = 1;
        return;
    }
    for (i = 0; i < DEVSTATUS_SIZE; i++)
        if (now[i] != g_devstatus.last[i])
            log_line("[devstatus] +0x%02x: %u -> %u", OFF_DEVSTATUS + i, g_devstatus.last[i], now[i]);
    memcpy(g_devstatus.last, now, DEVSTATUS_SIZE);
}

/* ---------------------------------------------------------------- system status table (read-only) */

/* Main.exe keeps its system state (Bluetooth, reverse, lights, current page and more) in the
 * 72-byte shared memory block SkSysInfoShareMem (layout in sysinfo.h). Main.exe broadcasts its
 * own changes as window message 0x52A, but values that never change were never logged, and other
 * programs can write the block through SkShareMem.dll without a broadcast. So the probe maps the
 * block read-only and logs all of it at start and exit, and each change in between. A mapping
 * size of 0 can only open an existing block, never create one (SkShareMem.dll relies on the same
 * rule to tell whether the block exists); no SkShareMem.dll code runs in the probe. */
#define SKSHAREMEM_SIZE 147456

/* Looked up at run time like OpenProcess above: the probe never imported them on the car before
 * (this unit's SkGUI.dll imports them as COREDLL ordinals 548 and 549). */
typedef HANDLE (WINAPI *create_mapping_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
typedef LPVOID (WINAPI *map_view_fn)(HANDLE, DWORD, DWORD, DWORD, DWORD);

static struct {
    int state; /* 0 not started, 1 reading, -1 unavailable */
    int layout_ok;
    HANDLE mapping;
    const volatile unsigned char *view;
    unsigned char last[SYSINFO_SIZE];
} g_sys;

static void log_sysinfo_change(void *context, int index, int offset, long before, long after)
{
    g_sysinfo_changes++;
    if (index >= 0)
        log_line("[sysinfo] index 0x%02x: %ld -> %ld", index, before, after);
    else
        log_line("[sysinfo] offset 0x%02x (no index): %ld -> %ld", offset, before, after);
}

static void sysinfo_log_all(const char *when)
{
    char text[200];
    int first;

    log_line("[sysinfo] all values at %s (hex index = value)%s", when,
             g_sys.layout_ok ? "" : ": layout not verified, raw bytes only");
    for (first = 0; g_sys.layout_ok && first < SYSINFO_FIELDS; first += 16) {
        sysinfo_format(g_sys.last, first, 16, text, sizeof text);
        log_line("    %s", text);
    }
    log_hex("[sysinfo] raw block", g_sys.last, SYSINFO_SIZE, SYSINFO_SIZE);
}

static void sysinfo_copy(unsigned char *out)
{
    int i;
    for (i = 0; i < SYSINFO_SIZE; i++)
        out[i] = g_sys.view[i];
}

static void sysinfo_open(void)
{
    WIN32_FIND_DATAW found;
    HANDLE f = FindFirstFileW(L"\\NandFlash\\Application\\Bin\\SkShareMem.dll", &found);
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    create_mapping_fn create_mapping = core ? (create_mapping_fn)GetProcAddressW(core, L"CreateFileMappingW") : NULL;
    map_view_fn map_view = core ? (map_view_fn)GetProcAddressW(core, L"MapViewOfFile") : NULL;
    DWORD error;

    g_sys.state = -1;
    if (f != INVALID_HANDLE_VALUE) {
        FindClose(f);
        g_sys.layout_ok = found.nFileSizeLow == SKSHAREMEM_SIZE;
        if (!g_sys.layout_ok)
            log_line("[sysinfo] SkShareMem.dll is %lu bytes, not the analysed build", found.nFileSizeLow);
    } else {
        log_line("[sysinfo] SkShareMem.dll not found");
    }
    if (create_mapping == NULL || map_view == NULL) {
        log_line("[sysinfo] CreateFileMappingW/MapViewOfFile not available; status table off");
        return;
    }
    g_sys.mapping = create_mapping(INVALID_HANDLE_VALUE, NULL, PAGE_READONLY, 0, 0, L"SkSysInfoShareMem");
    error = GetLastError();
    if (g_sys.mapping == NULL) {
        log_line("[sysinfo] SkSysInfoShareMem cannot be opened (error %lu); status table off", error);
        return;
    }
    g_sys.view = (const volatile unsigned char *)map_view(g_sys.mapping, FILE_MAP_READ, 0, 0, SYSINFO_SIZE);
    if (g_sys.view == NULL) {
        log_line("[sysinfo] MapViewOfFile failed (error %lu); status table off", GetLastError());
        CloseHandle(g_sys.mapping);
        return;
    }
    log_line("[sysinfo] SkSysInfoShareMem mapped read-only (%s)",
             error == ERROR_ALREADY_EXISTS ? "existing block" : "not reported as existing");
    sysinfo_copy(g_sys.last);
    g_sys.state = 1;
    sysinfo_log_all("start");
}

static void sysinfo_poll(void)
{
    unsigned char now[SYSINFO_SIZE];

    if (g_sys.state == 0)
        sysinfo_open();
    if (g_sys.state != 1)
        return;
    sysinfo_copy(now);
    if (sysinfo_diff(g_sys.last, now, g_sys.layout_ok, log_sysinfo_change, NULL)) {
        memcpy(g_sys.last, now, SYSINFO_SIZE);
        update_values();
    }
}

/* ---------------------------------------------------------------- window */

static void run_worker(LPTHREAD_START_ROUTINE fn)
{
    HANDLE t;
    if (InterlockedExchange((LONG *)&g_busy, 1)) {
        log_line("busy, wait for the current task to finish");
        return;
    }
    t = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange((LONG *)&g_busy, 0);
}

static void gps_toggle(void)
{
    HANDLE t;
    if (InterlockedExchange((LONG *)&g_gps_running, 1)) {
        InterlockedExchange((LONG *)&g_gps_running, 0);
        log_line("[gps] stopping");
        return;
    }
    memset(&g_gps, 0, sizeof g_gps);
    t = CreateThread(NULL, 0, gps_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        InterlockedExchange((LONG *)&g_gps_running, 0);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_PROBE_LOG:
        append_to_edit((char *)lparam);
        LocalFree((HLOCAL)lparam);
        return 0;
    case WM_PROBE_REFRESH:
        update_values();
        return 0;
    case WM_ACTIVATE:
        log_line("[probe] window %s", LOWORD(wparam) == WA_INACTIVE ? "went to the background" : "is in front");
        break;
    case WM_TIMER:
        if (wparam == TIMER_WATCH) {
            static unsigned ticks;
            watch_climate_popup();
            if (++ticks % 3 == 0) {
                climate_read();
                devstatus_read();
            }
            if (ticks % 2 == 0)
                sysinfo_poll();
        } else
            bus_timer((UINT)wparam);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_SURVEY: run_worker(survey_thread); break;
        case ID_PORTS: run_worker(port_scan_thread); break;
        case ID_BUS: bus_start(); break;
        case ID_GPS: gps_toggle(); break;
        case ID_STEP: next_step(); break;
        case ID_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_CLOSE:
        if (g_bus.active) {
            bus_poll_ring();
            log_pair_summary();
            log_line("[bus] %u records, %u CarInfo replies, %u requests sent (%u failed), %u own records seen",
                     g_bus.records, g_bus.carinfo_replies, g_bus.tx_count, g_bus.tx_failed, g_bus.own_seen);
        }
        if (g_sys.state == 1) {
            sysinfo_poll();
            sysinfo_log_all("exit");
            log_line("[sysinfo] %u changes", g_sysinfo_changes);
        }
        InterlockedExchange((LONG *)&g_gps_running, 0);
        log_line("=== Exit");
        Sleep(400);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void open_log(void)
{
    WCHAR path[MAX_PATH];
    WCHAR *slash;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    slash = wcsrchr(path, L'\\');
    if (slash)
        wcscpy(slash + 1, L"probe-log.txt");
    else
        wcscpy(path, L"\\probe-log.txt");
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log_file != INVALID_HANDLE_VALUE)
        SetFilePointer(g_log_file, 0, NULL, FILE_END);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show)
{
    static const struct { int id; const WCHAR *text; } buttons[] = {
        {ID_SURVEY, L"Survey"}, {ID_BUS, L"Bus"}, {ID_GPS, L"GPS"},
        {ID_PORTS, L"Ports"}, {ID_STEP, L"Next step"}, {ID_EXIT, L"Exit"},
    };
    WNDCLASSW wc;
    LOGFONTW lf;
    HFONT big;
    MSG msg;
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
    int bw, i;
    HWND running = FindWindowW(L"MonteroProbe", NULL);

    if (running != NULL) { /* already running in the background: bring it back instead */
        ShowWindow(running, SW_SHOW);
        SetForegroundWindow(running);
        return 0;
    }
    InitializeCriticalSection(&g_log_lock);
    open_log();
    log_line("=== Montero Probe %s started", PROBE_VERSION);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = instance;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"MonteroProbe";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, L"MonteroProbe", L"Montero Probe", WS_VISIBLE | WS_POPUP, 0, 0, cx, cy,
                             NULL, NULL, instance, NULL);
    if (g_main == NULL)
        return 1;

    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -18;
    lf.lfWeight = FW_BOLD;
    big = CreateFontIndirectW(&lf);
    g_values = CreateWindowExW(0, L"STATIC", L"No data yet", WS_CHILD | WS_VISIBLE | SS_LEFT, 6, 2, cx - 12,
                               96, g_main, NULL, instance, NULL);
    SendMessageW(g_values, WM_SETFONT, (WPARAM)big, TRUE);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                             ES_READONLY | ES_AUTOVSCROLL, 4, 100, cx - 8, cy - 100 - 74, g_main, NULL, instance, NULL);
    bw = cx / (int)(sizeof buttons / sizeof buttons[0]);
    for (i = 0; i < (int)(sizeof buttons / sizeof buttons[0]); i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", buttons[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 i * bw + 3, cy - 70, bw - 6, 64, g_main, (HMENU)(INT_PTR)buttons[i].id, instance,
                                 NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)big, TRUE);
    }
    update_values();
    SetTimer(g_main, TIMER_WATCH, 100, NULL);
    log_line("Ready. Survey, GPS and Ports only read. Bus sends read requests only.");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_log_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_log_file);
    return 0;
}
