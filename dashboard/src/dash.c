/* Montero dashboard: replaces \SD\Navigation\Navigation.exe on the Skypine 6982E head unit
 * (Windows CE 6, ARM). Three full-screen pages: the dashboard; the car info page, opened with the
 * car button at the right end of its header; and the map page, opened by tapping the car on the
 * map, where a finger drags the map (mapview.c). Back returns from either. Layout in render.c,
 * logic in model.c.
 *
 * Data sources (all read-only, see docs/vehicle-data-research.md):
 *   GPS       NMEA from COM1: at 38400 baud, opened for reading only
 *   CarInfo   joins Main.exe's message bus as "TerminalA" (0x1A), reads the shared-memory ring and
 *             posts the factory Car Info screen's read requests with sender 0x14 every 5 s
 *   lights    window message 0x52A index 9 broadcast by Main.exe, and CarInfo frame 0x33
 *   map       the *.vmap file in the VMap folder at the SD card root (tools/vmap/), opened read-only;
 *             drawn by the program itself (vmap.c), turned with the car's heading
 *   address   the *.addr file in the same folder (tools/address/), read-only, looked up on its own
 *             thread; shown on screen only, never logged
 *   holidays  holidays.txt next to this exe (tools/holidays/), read-only: today's national holidays,
 *             and local ones for the town nearest the GPS position (the list's own town centres,
 *             not the map); a notification at the top of the map shows them (island.c), then
 *             shrinks into a round button left of the map that opens it again; the town and the
 *             local holidays are shown on screen only, never logged
 * Main.exe shows and hides this program by its window title "GPS - Navigation" (SP_MAIN.ini).
 * Hold the clock for 2 seconds for the menu (file manager, close).
 * Never sends EndCommunication (81 01 00), 0x6D1 or 0x6D4; never writes to vehicle ports.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "busring.h"
#include "carinfo.h"
#include "addr.h"
#include "holiday.h"
#include "island.h"
#include "mapview.h"
#include "model.h"
#include "nmea.h"
#include "render.h"
#include "sun.h"
#include "vmap.h"
#include "vtiles.h"

#define DASH_VERSION "0.9.0"
#define WINDOW_CLASS L"MonteroDashboard"
#define WINDOW_TITLE L"GPS - Navigation" /* SP_MAIN.ini [NAVIGATION] TITLE */

/* Message bus constants (decoded from SkCom.dll / Main.exe, same as the probe) */
#define APP_MAIN 0x00
#define APP_CARINFO 0x14
#define APP_TERMINAL 0x1A
#define MSG_CARINFO 0x4A
#define MSG_SYSINFO 0x52A   /* Main.exe broadcast: wParam index, lParam value; index 9 = lights */
#define SYSINFO_LIGHTS 9
#define BUS_SHM_SIZE 0x40000

/* SkCom.dll layout of firmware cne-6982e-a; checked before any raw access */
#define RVA_MSGSESSION_INIT 0x1ee88
#define RVA_MSGSESSION_CONFIG 0x1ccbc
#define RVA_FN_GETDESTNAME 0x69a38
#define RVA_SHM_BASE_PTR 0x69a34

#define GPS_PORT L"COM1:"
#define GPS_BAUD 38400

#define TIMER_TICK 1        /* 1 s: update the model and repaint */
#define TIMER_RING 2        /* 100 ms: read new bus messages */
#define TIMER_FIRST 3       /* one shot, 1 s after StartCommunication */
#define TIMER_REQUESTS 4    /* 5 s: CarInfo read requests */
#define TIMER_HOLD 5        /* long press on the clock */
#define TIMER_STATS 6       /* 10 min: one summary line in the log */
#define TIMER_BUS_RETRY 7   /* 30 s: try to join the bus again */
#define TIMER_ISLAND 8      /* 15 ms while the holiday notification moves */
#define TIMER_ANIM 9        /* about 100 ms while the map glides to a new GPS position and heading */
#define TIMER_MAPWORK 10    /* 1 ms while map pictures are missing: one is drawn when nothing else waits */

#ifndef ANTIALIASED_QUALITY
#define ANTIALIASED_QUALITY 4
#endif

/* ---------------------------------------------------------------- log (no GPS position, ever) */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;
static WCHAR g_dir[MAX_PATH]; /* folder of this exe, with a trailing backslash */

static void log_line(const char *fmt, ...)
{
    char text[400], line[460];
    SYSTEMTIME now;
    va_list args;
    DWORD written;
    int len;

    if (g_log == INVALID_HANDLE_VALUE)
        return;
    va_start(args, fmt);
    len = _vsnprintf(text, sizeof text - 1, fmt, args);
    va_end(args);
    text[len < 0 ? (int)sizeof text - 1 : len] = '\0';
    GetLocalTime(&now);
    len = _snprintf(line, sizeof line - 3, "%04d-%02d-%02d %02d:%02d:%02d %s", now.wYear, now.wMonth, now.wDay,
                    now.wHour, now.wMinute, now.wSecond, text);
    if (len < 0)
        len = (int)sizeof line - 3;
    line[len++] = '\r';
    line[len++] = '\n';
    EnterCriticalSection(&g_log_lock);
    WriteFile(g_log, line, (DWORD)len, &written, NULL);
    FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_log_lock);
}

static void open_log(void)
{
    WCHAR path[MAX_PATH];
    DWORD size;
    _snwprintf(path, MAX_PATH, L"%sdashboard-log.txt", g_dir);
    path[MAX_PATH - 1] = 0;
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    size = GetFileSize(g_log, NULL);
    if (size != 0xFFFFFFFF && size > 256 * 1024) { /* keep the log small: start over */
        CloseHandle(g_log);
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        SetFilePointer(g_log, 0, NULL, FILE_END);
    }
}

/* ---------------------------------------------------------------- shared state */

static HWND g_main;
static CRITICAL_SECTION g_gps_lock;
static NmeaState g_gps;            /* guarded by g_gps_lock */
static DWORD g_gps_tick;           /* when the last RMC sentence arrived (guarded) */
static volatile LONG g_quit;

static CarInfoState g_car;         /* UI thread only */
static DWORD g_car_tick;           /* last CarInfo reply */
static int g_lights;               /* last lights state seen on the bus */
static DashModel g_model;
static int g_menu, g_has_files;
enum { PAGE_DASHBOARD, PAGE_CARINFO, PAGE_MAP };
static int g_page;                /* the page shown */
static int g_carinfo_cat;         /* the car info page's category (CARINFO_GENERAL ...), kept while the program runs */
static MapView g_view;            /* the map page's view (never logged: its middle is a position) */
static WCHAR g_files_path[MAX_PATH];
#define DEFAULT_UTC_OFFSET_MIN 480 /* UTC+8 until another time zone is chosen in the menu */
static int g_tz_set = 1, g_tz_min = DEFAULT_UTC_OFFSET_MIN; /* time zone in use, minutes from UTC */
static double g_saved_zoom = -1;  /* the dashboard map's zoom from dashboard-settings.txt (-1: none) */

/* dashboard-settings.txt next to the exe: utc_offset_minutes=<n>, and map_zoom_tenths=<n> (the
 * dashboard map's zoom x 10, the last one chosen with + and -). */
static void settings_path(WCHAR *path)
{
    _snwprintf(path, MAX_PATH, L"%sdashboard-settings.txt", g_dir);
    path[MAX_PATH - 1] = 0;
}

/* First place where key (len characters) appears in text, or NULL. Written out
   instead of strstr, which no program has imported from COREDLL on the car yet. */
static const char *find_text(const char *text, const char *key, int len)
{
    int i;
    for (; *text; text++) {
        for (i = 0; i < len && text[i] == key[i]; i++)
            ;
        if (i == len)
            return text;
    }
    return NULL;
}

/* The whole number after key in text, or `missing`. */
static int setting(const char *text, const char *key, int key_len, int missing)
{
    const char *p = find_text(text, key, key_len);
    int sign = 1, v = 0, digits = 0;
    if (p == NULL)
        return missing;
    p += key_len;
    if (*p == '-' || *p == '+')
        sign = *p++ == '-' ? -1 : 1;
    for (; *p >= '0' && *p <= '9' && digits < 4; p++, digits++)
        v = v * 10 + (*p - '0');
    return digits ? v * sign : missing;
}

static void load_settings(void)
{
    static const char tz_key[] = "utc_offset_minutes=", zoom_key[] = "map_zoom_tenths=";
    WCHAR path[MAX_PATH];
    char buf[160];
    DWORD got = 0;
    HANDLE h;
    int v;

    settings_path(path);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("[clock] time zone %+d min (the default; change it in the menu)", g_tz_min);
        return;
    }
    if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL))
        got = 0;
    CloseHandle(h);
    buf[got] = '\0';
    v = setting(buf, tz_key, (int)sizeof tz_key - 1, 100000);
    if (v >= -720 && v <= 840) {
        g_tz_set = 1;
        g_tz_min = v;
        log_line("[clock] time zone from dashboard-settings.txt: %+d min", v);
    }
    v = setting(buf, zoom_key, (int)sizeof zoom_key - 1, 0);
    if (v >= 20 && v <= 200)
        g_saved_zoom = v / 10.0;
}

static void save_settings(void)
{
    WCHAR path[MAX_PATH];
    char line[96];
    DWORD written;
    HANDLE h;
    int len = _snprintf(line, sizeof line, "utc_offset_minutes=%d\r\nmap_zoom_tenths=%d\r\n", g_tz_min,
                        (int)(g_model.map_zoom * 10.0 + 0.5));

    settings_path(path);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("[clock] cannot save dashboard-settings.txt (error %lu)", GetLastError());
        return;
    }
    WriteFile(h, line, (DWORD)len, &written, NULL);
    CloseHandle(h);
}

/* ---------------------------------------------------------------- GPS (read-only) */

static DWORD WINAPI gps_thread(LPVOID unused)
{
    char raw[256], line[128];
    int used = 0, reported = 0;
    DWORD got;
    unsigned last_rmc = 0;
    NmeaState local;

    memset(&local, 0, sizeof local);
    while (!g_quit) {
        DCB dcb;
        COMMTIMEOUTS timeouts = {50, 0, 200, 0, 0};
        HANDLE h = CreateFileW(GPS_PORT, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (!reported++)
                log_line("[gps] cannot open COM1: (error %lu); retrying every 10 s", GetLastError());
            Sleep(10000);
            continue;
        }
        dcb.DCBlength = sizeof dcb;
        if (GetCommState(h, &dcb) && dcb.BaudRate != GPS_BAUD) {
            dcb.BaudRate = GPS_BAUD;
            SetCommState(h, &dcb);
        }
        SetCommTimeouts(h, &timeouts);
        log_line("[gps] reading COM1: at %d", GPS_BAUD);
        reported = 0;
        while (!g_quit && ReadFile(h, raw, sizeof raw, &got, NULL)) {
            if (!got)
                continue;
            nmea_feed(&local, line, &used, sizeof line, raw, (int)got);
            EnterCriticalSection(&g_gps_lock);
            g_gps = local;
            if (local.rmc_count != last_rmc) {
                last_rmc = local.rmc_count;
                g_gps_tick = GetTickCount();
            }
            LeaveCriticalSection(&g_gps_lock);
        }
        CloseHandle(h);
        if (!g_quit) {
            log_line("[gps] read failed (error %lu); reopening", GetLastError());
            Sleep(2000);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- message bus (from the probe) */

typedef void (*config_fn)(int sender, const char *(*dest_name)(int));
typedef void *(*session_ctor_fn)(void *self);
typedef int (*session_init_fn)(void *self, void *proc);
typedef int (*post_fn)(int dest, int what, int arg1, int arg2, void *parcelable);
typedef void *(*obtain_fn)(void **result);
typedef void (*bundle_put_fn)(void *bundle, void *const *key, const void *data, int size);
typedef void *(*string_ctor_fn)(void **self, const char *text);
typedef int (*dec_ref_fn)(void *object);

static struct {
    int active, resolved, give_up;
    unsigned char *skcom_base, *shm;
    config_fn config;
    const char *(*dest_name)(int);
    session_ctor_fn session_ctor;
    session_init_fn session_init;
    post_fn post;
    obtain_fn bundle_obtain;
    bundle_put_fn bundle_put;
    string_ctor_fn string_ctor;
    dec_ref_fn dec_ref;
    void *key_data; /* juce::String "data" */
    void *session;
    BusRing ring;
    unsigned records, replies, tx, tx_failed;
} g_bus;

static void rc_release(void *object)
{
    if (object != NULL && (g_bus.dec_ref(object) & 0xff))
        ((void (*)(void *, unsigned))(*(void ***)object)[0])(object, 1);
}

static void set_lights(int on, const char *source)
{
    on = on ? 1 : 0;
    if (on != g_lights)
        log_line("[lights] %s (%s)", on ? "on" : "off", source);
    g_lights = on;
}

static void on_ring_record(void *context, unsigned offset, const unsigned char *message, int length)
{
    static unsigned char copy[4096];
    const unsigned char *data;
    BusHeader h;
    int size;

    g_bus.records++;
    if (length <= 0 || length > (int)sizeof copy)
        return;
    memcpy(copy, message, (size_t)length); /* other processes may overwrite the ring meanwhile */
    if (!busring_message_header(copy, length, &h) || h.what != MSG_CARINFO || h.sender != APP_MAIN)
        return; /* only Main.exe's CarInfo replies; requests come from 0x14 */
    size = busring_bundle_bytes(copy, length, "data", &data);
    if (size < 1 || carinfo_apply(&g_car, data, size) < 0)
        return;
    if (g_bus.replies++ == 0)
        log_line("[bus] first CarInfo reply (command %02x)", data[0]);
    g_car_tick = GetTickCount();
    if (data[0] == 0x33 && g_car.have_lights)
        set_lights((g_car.lights & 0x04) != 0, "CarInfo 0x33");
}

static void bus_poll(void)
{
    int n;
    if (!g_bus.active)
        return;
    do {
        n = busring_poll(&g_bus.ring, g_bus.shm, BUS_SHM_SIZE, 64, on_ring_record, NULL);
    } while (n == 64);
    if (n < 0) {
        log_line("[bus] shared-memory header no longer valid; car data stops");
        g_bus.active = 0;
        KillTimer(g_main, TIMER_RING);
        KillTimer(g_main, TIMER_REQUESTS);
    }
}

/* Fake IMsgProc for MsgSession: slot 0 destructor, 1 procOwnerMsg, 2 procUnknownMsg. */
static void *proc_destructor(void *self, unsigned flags) { return self; }
static void proc_owner_msg(void *self, void **message_ref) {}
static void proc_unknown_msg(void *self, int msg, int wparam, int lparam)
{
    if (msg == MSG_SYSINFO && wparam == SYSINFO_LIGHTS)
        set_lights(lparam != 0, "0x52A");
}
static void *proc_spare(void *self) { return NULL; }
static void *g_proc_vtable[8] = {
    (void *)proc_destructor, (void *)proc_owner_msg, (void *)proc_unknown_msg, (void *)proc_spare,
    (void *)proc_spare, (void *)proc_spare, (void *)proc_spare, (void *)proc_spare,
};
static void *g_proc = g_proc_vtable;

/* One CarInfo read request. Main.exe routes requests by the sender's app id, so the sender is
 * AppCarInfo (0x14) for the post only, exactly as the factory screen sends it. */
static int bus_send(const unsigned char *bytes, int size)
{
    void *bundle = NULL;
    int ok;

    g_bus.bundle_obtain(&bundle);
    if (bundle == NULL)
        return 0;
    g_bus.bundle_put(bundle, &g_bus.key_data, bytes, size);
    g_bus.config(APP_CARINFO, g_bus.dest_name);
    ok = g_bus.post(APP_MAIN, MSG_CARINFO, APP_CARINFO, 0, bundle) & 0xff;
    g_bus.config(APP_TERMINAL, g_bus.dest_name);
    ((void (*)(void *))(*(void ***)bundle)[4])(bundle); /* recycle, as CarInfoApi does */
    rc_release(bundle);
    g_bus.tx++;
    if (!ok)
        g_bus.tx_failed++;
    return ok;
}

/* Read requests only: compass, pressure and elevation, service and battery, outside temperature. */
static void send_requests(void)
{
    static const unsigned char compass[] = {0x11, 0x00}, pressure[] = {0x21, 0x00}, maintain[] = {0xA0, 0x02, 0x40},
                               outside[] = {0x31, 0x00};
    bus_send(compass, 2);
    bus_send(pressure, 2);
    bus_send(maintain, 3);
    bus_send(outside, 2);
}

static FARPROC need(HMODULE module, const WCHAR *name, int *missing)
{
    FARPROC p = GetProcAddressW(module, name);
    if (p == NULL)
        (*missing)++;
    return p;
}

static int bus_resolve(void)
{
    HMODULE skcom, skgui;
    unsigned char *fn_slot;
    int missing = 0;

    skcom = LoadLibraryW(L"SkCom.dll");
    if (skcom == NULL)
        skcom = LoadLibraryW(L"\\NandFlash\\Application\\Bin\\SkCom.dll");
    skgui = LoadLibraryW(L"SkGUI.dll");
    if (skgui == NULL)
        skgui = LoadLibraryW(L"\\NandFlash\\Application\\Bin\\SkGUI.dll");
    if (skcom == NULL || skgui == NULL) {
        log_line("[bus] cannot load SkCom.dll / SkGUI.dll (error %lu)", GetLastError());
        return 0;
    }
    g_bus.config = (config_fn)need(skcom, L"?config@MsgSession@@SAXHP6APBDH@Z@Z", &missing);
    g_bus.dest_name = (const char *(*)(int))need(skcom, L"getDestName", &missing);
    g_bus.session_ctor = (session_ctor_fn)need(skcom, L"??0MsgSession@@QAA@XZ", &missing);
    g_bus.session_init = (session_init_fn)need(skcom, L"?init@MsgSession@@QAA_NPAVIMsgProc@@@Z", &missing);
    g_bus.post = (post_fn)need(skcom, L"?postMessage@MessageHelper@IPCMsg@@SA_NHHHHPAVParcelable@@@Z", &missing);
    g_bus.bundle_obtain = (obtain_fn)need(skcom, L"?obtain@Bundle@@SA?AV?$ReferenceCountedObjectPtr@VBundle@@@juce@@XZ",
                                          &missing);
    g_bus.bundle_put = (bundle_put_fn)need(skcom, L"?putByteArray@Bundle@@QAAXABVString@juce@@PBXH@Z", &missing);
    g_bus.string_ctor = (string_ctor_fn)need(skgui, L"??0String@juce@@QAA@PBD@Z", &missing);
    g_bus.dec_ref = (dec_ref_fn)need(skgui, L"?decReferenceCountWithoutDeleting@ReferenceCountedObject@juce@@QAA_NXZ",
                                     &missing);
    fn_slot = (unsigned char *)need(skcom, L"?m_fnGetDestName@MsgSession@@0P6APBDH@ZA", &missing);
    if (missing) {
        log_line("[bus] %d SkCom/SkGUI exports missing; no car data", missing);
        return 0;
    }
    g_bus.skcom_base = (unsigned char *)g_bus.session_init - RVA_MSGSESSION_INIT;
    if ((unsigned char *)g_bus.config - g_bus.skcom_base != RVA_MSGSESSION_CONFIG ||
        fn_slot - g_bus.skcom_base != RVA_FN_GETDESTNAME) {
        log_line("[bus] SkCom.dll is a different build than cne-6982e-a; no car data");
        return 0;
    }
    g_bus.string_ctor(&g_bus.key_data, "data");
    return 1;
}

static void bus_start(void)
{
    static const unsigned char start_comm[] = {0x81, 0x01, 0x01};
    int ok;

    if (g_bus.active || g_bus.give_up)
        return;
    if (FindWindowW(L"TerminalA", L"TerminalA")) { /* the probe, most likely: try again later */
        static int said;
        if (!said++)
            log_line("[bus] a TerminalA window already exists (is the probe running?); retrying every 30 s");
        SetTimer(g_main, TIMER_BUS_RETRY, 30000, NULL);
        return;
    }
    if (!g_bus.resolved) {
        if (!bus_resolve()) {
            g_bus.give_up = 1;
            return;
        }
        g_bus.resolved = 1;
    }
    g_bus.config(APP_TERMINAL, g_bus.dest_name);
    g_bus.session = LocalAlloc(LPTR, 32);
    g_bus.session_ctor(g_bus.session);
    ok = g_bus.session_init(g_bus.session, &g_proc) & 0xff;
    g_bus.shm = *(unsigned char **)(g_bus.skcom_base + RVA_SHM_BASE_PTR);
    if (!ok || g_bus.shm == NULL || !busring_header_ok(g_bus.shm, BUS_SHM_SIZE)) {
        log_line("[bus] cannot use the bus (init %d); no car data", ok);
        g_bus.give_up = 1;
        return;
    }
    KillTimer(g_main, TIMER_BUS_RETRY);
    g_bus.active = 1;
    g_bus.ring.cursor = *(unsigned *)(g_bus.shm + 4); /* read the backlog first: recent values at once */
    bus_poll();
    log_line("[bus] joined as TerminalA; backlog %u records, %u CarInfo replies", g_bus.records, g_bus.replies);
    SetTimer(g_main, TIMER_RING, 100, NULL);
    bus_send(start_comm, 3); /* as the factory screen: start, then requests one second later */
    SetTimer(g_main, TIMER_FIRST, 1000, NULL);
}

/* ---------------------------------------------------------------- fonts and drawing (GDI) */

/* Shape and font-name functions are looked up at run time: the probe that ran on the car never
 * imported them, and a missing import would stop the exe from loading at all. Each has a
 * plainer fallback. */
typedef BOOL (WINAPI *ellipse_fn)(HDC, int, int, int, int);
typedef BOOL (WINAPI *round_rect_fn)(HDC, int, int, int, int, int, int);
typedef BOOL (WINAPI *poly_fn)(HDC, const POINT *, int);
typedef int (WINAPI *text_face_fn)(HDC, int, LPWSTR);
static struct {
    ellipse_fn ellipse;
    round_rect_fn round_rect;
    poly_fn polygon, polyline;
    text_face_fn text_face;
} g_gdi;

static void resolve_gdi(void)
{
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    if (core == NULL)
        return;
    g_gdi.ellipse = (ellipse_fn)GetProcAddressW(core, L"Ellipse");
    g_gdi.round_rect = (round_rect_fn)GetProcAddressW(core, L"RoundRect");
    g_gdi.polygon = (poly_fn)GetProcAddressW(core, L"Polygon");
    g_gdi.polyline = (poly_fn)GetProcAddressW(core, L"Polyline");
    g_gdi.text_face = (text_face_fn)GetProcAddressW(core, L"GetTextFaceW");
    log_line("[gdi] Ellipse %s, RoundRect %s, Polygon %s, Polyline %s, GetTextFaceW %s",
             g_gdi.ellipse ? "yes" : "NO", g_gdi.round_rect ? "yes" : "NO", g_gdi.polygon ? "yes" : "NO",
             g_gdi.polyline ? "yes" : "NO", g_gdi.text_face ? "yes" : "NO");
}

static const WCHAR *const g_face_name[FACE_COUNT] = {L"Barlow Medium", L"Barlow SemiBold", L"Barlow Condensed SemiBold"};
static const WCHAR *const g_face_file[FACE_COUNT] = {L"Barlow-Medium.ttf", L"Barlow-SemiBold.ttf",
                                                     L"BarlowCondensed-SemiBold.ttf"};
static const int g_face_weight[FACE_COUNT] = {500, 600, 600};
static int g_face_loaded[FACE_COUNT];
static HFONT g_font[FONT_COUNT];
static int g_ascent[FONT_COUNT];

typedef int (WINAPI *font_resource_fn)(LPCWSTR);

static font_resource_fn coredll_fn(const WCHAR *name)
{
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    return core ? (font_resource_fn)GetProcAddressW(core, name) : NULL;
}

static void load_fonts(HDC dc)
{
    font_resource_fn add = coredll_fn(L"AddFontResourceW");
    WCHAR path[MAX_PATH], face[LF_FACESIZE];
    int i;

    for (i = 0; i < FACE_COUNT; i++) {
        _snwprintf(path, MAX_PATH, L"%sfonts\\%s", g_dir, g_face_file[i]);
        path[MAX_PATH - 1] = 0;
        g_face_loaded[i] = add != NULL && add(path) > 0;
    }
    for (i = 0; i < FONT_COUNT; i++) {
        int f = render_fonts[i].face, tries;
        for (tries = 0; tries < 2; tries++) {
            int custom = g_face_loaded[f] && tries == 0;
            LOGFONTW lf;
            TEXTMETRICW tm;
            HGDIOBJ old;
            memset(&lf, 0, sizeof lf);
            lf.lfHeight = -render_fonts[i].px;
            lf.lfWeight = custom ? g_face_weight[f] : (f == FACE_MEDIUM ? FW_NORMAL : FW_BOLD);
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = ANTIALIASED_QUALITY;
            wcsncpy(lf.lfFaceName, custom ? g_face_name[f] : L"Tahoma", LF_FACESIZE - 1);
            g_font[i] = CreateFontIndirectW(&lf);
            old = SelectObject(dc, g_font[i]);
            face[0] = 0;
            if (g_gdi.text_face)
                g_gdi.text_face(dc, LF_FACESIZE, face);
            GetTextMetricsW(dc, &tm);
            g_ascent[i] = tm.tmAscent;
            SelectObject(dc, old);
            if (!custom || !g_gdi.text_face || _wcsicmp(face, g_face_name[f]) == 0)
                break;
            DeleteObject(g_font[i]); /* the file loaded but the face did not map: use Tahoma */
            g_face_loaded[f] = 0;
        }
    }
    log_line("[fonts] Barlow Medium %s, SemiBold %s, Condensed %s (otherwise Tahoma)",
             g_face_loaded[0] ? "loaded" : "missing", g_face_loaded[1] ? "loaded" : "missing",
             g_face_loaded[2] ? "loaded" : "missing");
}

static void unload_fonts(void)
{
    font_resource_fn remove = coredll_fn(L"RemoveFontResourceW");
    WCHAR path[MAX_PATH];
    int i;
    for (i = 0; i < FONT_COUNT; i++)
        if (g_font[i])
            DeleteObject(g_font[i]);
    for (i = 0; i < FACE_COUNT; i++)
        if (g_face_loaded[i] && remove) {
            _snwprintf(path, MAX_PATH, L"%sfonts\\%s", g_dir, g_face_file[i]);
            path[MAX_PATH - 1] = 0;
            remove(path);
        }
}

/* UTF-8 (render.c's strings) to UTF-16. */
static int widen(const char *s, WCHAR *out, int max)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max - 1) {
        unsigned c = *p++;
        if (c >= 0xE0 && p[0] && p[1]) {
            c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && p[0]) {
            c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
            p += 1;
        }
        out[n++] = (WCHAR)c;
    }
    out[n] = 0;
    return n;
}

static COLORREF colour(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

static void gdi_fill_rect(void *ctx, int x, int y, int w, int h, unsigned rgb)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(colour(rgb));
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect((HDC)ctx, &r, b);
    DeleteObject(b);
}

static void gdi_round_rect(void *ctx, int x, int y, int w, int h, int radius, unsigned fill, unsigned border)
{
    HDC dc = (HDC)ctx;
    HBRUSH b;
    HPEN p;
    HGDIOBJ ob, op;

    if (!g_gdi.round_rect) { /* square corners */
        if (fill != NO_FILL)
            gdi_fill_rect(ctx, x, y, w, h, fill);
        gdi_fill_rect(ctx, x, y, w, 1, border);
        gdi_fill_rect(ctx, x, y + h - 1, w, 1, border);
        gdi_fill_rect(ctx, x, y, 1, h, border);
        gdi_fill_rect(ctx, x + w - 1, y, 1, h, border);
        return;
    }
    b = fill == NO_FILL ? NULL : CreateSolidBrush(colour(fill));
    p = CreatePen(PS_SOLID, 1, colour(border));
    ob = SelectObject(dc, b ? (HGDIOBJ)b : GetStockObject(NULL_BRUSH));
    op = SelectObject(dc, p);
    g_gdi.round_rect(dc, x, y, x + w, y + h, 2 * radius, 2 * radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (b)
        DeleteObject(b);
    DeleteObject(p);
}

static void gdi_polyline(void *ctx, const DPoint *pts, int n, int width, unsigned rgb)
{
    HDC dc = (HDC)ctx;
    POINT p[SERIES_MAX + 4];
    HPEN pen;
    HGDIOBJ old;
    int i;
    if (n < 2 || !g_gdi.polyline)
        return;
    for (i = 0; i < n && i < SERIES_MAX + 4; i++) {
        p[i].x = pts[i].x;
        p[i].y = pts[i].y;
    }
    pen = CreatePen(PS_SOLID, width, colour(rgb));
    old = SelectObject(dc, pen);
    g_gdi.polyline(dc, p, i);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void gdi_polygon(void *ctx, const DPoint *pts, int n, unsigned fill)
{
    HDC dc = (HDC)ctx;
    POINT p[SERIES_MAX + 4];
    HBRUSH b;
    HGDIOBJ ob, op;
    int i;
    if (n < 3)
        return;
    if (!g_gdi.polygon) { /* outline only */
        gdi_polyline(ctx, pts, n, 1, fill);
        return;
    }
    for (i = 0; i < n && i < SERIES_MAX + 4; i++) {
        p[i].x = pts[i].x;
        p[i].y = pts[i].y;
    }
    b = CreateSolidBrush(colour(fill));
    ob = SelectObject(dc, b);
    op = SelectObject(dc, GetStockObject(NULL_PEN));
    g_gdi.polygon(dc, p, i);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void gdi_circle(void *ctx, int cx, int cy, int r, unsigned fill, unsigned border, int border_width)
{
    HDC dc = (HDC)ctx;
    HBRUSH b;
    HPEN p;
    HGDIOBJ ob, op;

    if (!g_gdi.ellipse) { /* a 40-sided polygon instead */
        static const short s40[11] = {0, 1564, 3090, 4540, 5878, 7071, 8090, 8910, 9511, 9877, 10000}; /* sin x 10^4 */
        DPoint q[41];
        int k;
        for (k = 0; k < 40; k++) {
            int a = k % 10, quarter = k / 10;
            int sx = s40[a], cy4 = s40[10 - a];
            int dx = quarter == 0 ? sx : quarter == 1 ? cy4 : quarter == 2 ? -sx : -cy4;
            int dy = quarter == 0 ? -cy4 : quarter == 1 ? sx : quarter == 2 ? cy4 : -sx;
            q[k].x = cx + dx * r / 10000;
            q[k].y = cy + dy * r / 10000;
        }
        q[40] = q[0];
        if (fill != NO_FILL)
            gdi_polygon(ctx, q, 40, fill);
        gdi_polyline(ctx, q, 41, border_width, border);
        return;
    }
    b = fill == NO_FILL ? NULL : CreateSolidBrush(colour(fill));
    p = CreatePen(PS_SOLID, border_width, colour(border));
    ob = SelectObject(dc, b ? (HGDIOBJ)b : GetStockObject(NULL_BRUSH));
    op = SelectObject(dc, p);
    g_gdi.ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    if (b)
        DeleteObject(b);
    DeleteObject(p);
}

static int gdi_text_width(void *ctx, int font, const char *s)
{
    HDC dc = (HDC)ctx;
    WCHAR w[128];
    SIZE size;
    int n = widen(s, w, 128);
    HGDIOBJ old = SelectObject(dc, g_font[font]);
    size.cx = 0;
    GetTextExtentPoint32W(dc, w, n, &size);
    SelectObject(dc, old);
    return size.cx;
}

static void gdi_text(void *ctx, int x, int baseline, int font, unsigned rgb, const char *s)
{
    HDC dc = (HDC)ctx;
    WCHAR w[128];
    int n = widen(s, w, 128);
    HGDIOBJ old = SelectObject(dc, g_font[font]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour(rgb));
    ExtTextOutW(dc, x, baseline - g_ascent[font], 0, NULL, w, (UINT)n, NULL);
    SelectObject(dc, old);
}

static HDC g_back;
static HBITMAP g_back_bitmap;

/* ---------------------------------------------------------------- vector map (read-only) */

/* The map is the vector map (*.vmap, tools/vmap/) in a VMap folder at the SD card root, beside the
 * Navigation folder: the head unit draws it itself (vmap.c), at any zoom and turned with the car.
 * Frames come from map pictures kept in memory (vtiles.c: 256 x 256 pieces of the map, drawn once
 * while nothing else happens, then moved and turned per frame) while the map moves; once it rests,
 * it is drawn in full, sharp, once, and that picture is reused until the view changes. Labels and
 * place markers are placed afresh at rest and moved with the map while it moves. The address file
 * stays in the Map folder (sd_folder). */
#define MAP_PICTURES 400            /* map pictures kept, 129 KB each (52 MB; 348 MB were free on the unit) */
#define MAP_PICTURES_LOW 120
#define MAP_DATA_TILES 128          /* map file tiles kept decoded */
#define MAP_TOP_ZOOM 19.0           /* the closest zoom offered */
#define MAP_ZOOM_STEP 0.5           /* each press of + or - */
#define GLIDE_MS 1000               /* the map glides to each new GPS position and heading in a second ... */
#define ANIM_MS 100                 /* ... at most ten frames a second, fewer when a frame takes long */
static VMap g_vmap;
static VTiles g_tiles;
static VFont g_mapfont;
static unsigned char *g_mapfont_data;
static int g_map_ok;                /* a map file is open: the map is drawn */
static MapBox g_map_box;            /* the box it covers (Web Mercator) */
/* the glide: the map moves from where it is shown to the newest GPS position and heading */
static int g_glide, g_glide_known, g_anim_timer;
static double g_glide_mx0, g_glide_my0, g_glide_b0, g_glide_mx1, g_glide_my1, g_glide_b1;
static DWORD g_glide_start, g_anim_ms = ANIM_MS;
/* the sharp map at rest, kept while the view stays */
static unsigned *g_rest;
static int g_rest_ok;
static VMapView g_rest_view;
/* for the log: frames while the map moves and their slowest, sharp drawings and their slowest */
static unsigned g_moving_frames, g_moving_sum_ms, g_sharp_count, g_sharp_sum_ms;
static DWORD g_moving_slowest_ms, g_sharp_slowest_ms;
/* the map page, for the log: times opened, frames drawn while dragging, the slowest of them
 * (map, panels and copy to the screen) */
static unsigned g_page_opened, g_drag_frames;
static DWORD g_drag_slowest_ms, g_drag_slowest_all_ms; /* this time the page is open; since the start */

static int file_read_at(void *file, unsigned offset, void *buffer, unsigned size)
{
    LONG high = 0;
    DWORD got = 0;
    if (SetFilePointer((HANDLE)file, (LONG)offset, &high, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR)
        return -1;
    if (!ReadFile((HANDLE)file, buffer, size, &got, NULL))
        return -1;
    return (int)got;
}

/* Names starting with a dot are skipped. When a Mac copies files to the FAT card, it adds a small
 * "._name" side file next to each one (macOS keeps file details there), which matches "*.vmap" and
 * "*.addr" too but is not a map or address file. */
static int hidden_name(const WCHAR *name)
{
    return name[0] == L'.';
}

/* A folder at the SD card root, with a trailing backslash: "\SD\Navigation\" -> "\SD\<name>\". */
static void sd_folder(WCHAR *folder, const WCHAR *name)
{
    WCHAR root[MAX_PATH], *slash;
    wcscpy(root, g_dir);
    if (wcslen(root) > 1)
        root[wcslen(root) - 1] = 0;
    slash = wcsrchr(root, L'\\');
    if (slash)
        slash[1] = 0;
    else
        wcscpy(root, L"\\");
    _snwprintf(folder, MAX_PATH, L"%s%s\\", root, name);
    folder[MAX_PATH - 1] = 0;
}

static unsigned clock_us(void)
{
    return GetTickCount() * 1000u;
}

/* The labels' font (the Barlow file the dashboard ships), read into memory for vfont.c. */
static int load_map_font(void)
{
    WCHAR path[MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    _snwprintf(path, MAX_PATH, L"%sfonts\\Barlow-Medium.ttf", g_dir);
    path[MAX_PATH - 1] = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    size = GetFileSize(h, NULL);
    if (size != 0xFFFFFFFF && size < (4u << 20)) {
        g_mapfont_data = (unsigned char *)malloc(size);
        if (g_mapfont_data && (!ReadFile(h, g_mapfont_data, size, &got, NULL) || got != size)) {
            free(g_mapfont_data);
            g_mapfont_data = NULL;
        }
    }
    CloseHandle(h);
    return g_mapfont_data != NULL && vf_load(&g_mapfont, g_mapfont_data, size);
}

static void map_open(void)
{
    WCHAR folder[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find, h = INVALID_HANDLE_VALUE;
    int hidden = 0, bad = 0;
    double lowest, highest;

    sd_folder(folder, L"VMap");
    _snwprintf(path, MAX_PATH, L"%s*.vmap", folder);
    path[MAX_PATH - 1] = 0;
    find = FindFirstFileW(path, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (hidden_name(found.cFileName)) {
                hidden++;
                continue;
            }
            _snwprintf(path, MAX_PATH, L"%s%s", folder, found.cFileName);
            path[MAX_PATH - 1] = 0;
            h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE && vmap_open(&g_vmap, h, file_read_at, MAP_DATA_TILES))
                break;
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
            bad++;
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (h == INVALID_HANDLE_VALUE) {
        log_line("[map] no vector map in the VMap folder at the SD card root (%d unreadable, %d ._ files skipped)", bad,
                 hidden);
        return;
    }
    g_vmap.clock = clock_us;
    if (load_map_font())
        vmap_set_font(&g_vmap, &g_mapfont);
    else
        log_line("[map] cannot read fonts\\Barlow-Medium.ttf: the map has no names");
    if (!vt_init(&g_tiles, &g_vmap, MAP_PICTURES) && !vt_init(&g_tiles, &g_vmap, MAP_PICTURES_LOW)) {
        log_line("[map] not enough memory for the map pictures");
        vmap_close(&g_vmap);
        CloseHandle(h);
        return;
    }
    g_rest = (unsigned *)malloc(SCREEN_W * SCREEN_H * sizeof(unsigned));
    vmap_mercator(g_vmap.north, g_vmap.west, &g_map_box.left, &g_map_box.top);
    vmap_mercator(g_vmap.south, g_vmap.east, &g_map_box.right, &g_map_box.bottom);
    vmap_zoom_range(&g_vmap, &lowest, &highest);
    g_model.map_min_zoom = lowest;
    g_model.map_max_zoom = highest < MAP_TOP_ZOOM ? highest : MAP_TOP_ZOOM;
    g_model.map_zoom = g_saved_zoom > 0 ? g_saved_zoom : MAP_DEFAULT_ZOOM;
    if (g_model.map_zoom < g_model.map_min_zoom)
        g_model.map_zoom = g_model.map_min_zoom;
    if (g_model.map_zoom > g_model.map_max_zoom)
        g_model.map_zoom = g_model.map_max_zoom;
    g_map_ok = 1;
    log_line("[map] vector map: %d rules, %d data zooms, zoom %.1f to %.1f, showing %.1f%s; %d pictures at most (%d ._ "
             "files skipped)", g_vmap.nlayers, g_vmap.nlevels, g_model.map_min_zoom, g_model.map_max_zoom,
             g_model.map_zoom, g_saved_zoom > 0 ? " (remembered)" : "", g_tiles.count, hidden);
}

/* One step in or out from zoom, kept inside the map's range. */
static double zoom_step(double zoom, int direction)
{
    double z = zoom + direction * MAP_ZOOM_STEP;
    if (z < g_model.map_min_zoom)
        z = g_model.map_min_zoom;
    if (z > g_model.map_max_zoom)
        z = g_model.map_max_zoom;
    return z;
}

static double turn_between(double from, double to)
{
    double d = to - from;
    while (d > 180)
        d -= 360;
    while (d < -180)
        d += 360;
    return d;
}

/* Where the glide is now: the model's map view (the position drawn at the car, the heading up). */
static void glide_now(DWORD now)
{
    double f = g_glide ? (double)(now - g_glide_start) / GLIDE_MS : 1.0, b;
    if (f >= 1.0) {
        f = 1.0;
        g_glide = 0;
    }
    g_model.map_mx = g_glide_mx0 + (g_glide_mx1 - g_glide_mx0) * f;
    g_model.map_my = g_glide_my0 + (g_glide_my1 - g_glide_my0) * f;
    b = g_glide_b0 + turn_between(g_glide_b0, g_glide_b1) * f;
    while (b < 0)
        b += 360;
    while (b >= 360)
        b -= 360;
    g_model.map_bearing = b;
}

/* After each model update: the newest GPS position, and the heading while it comes from GPS (when
 * the car stops, the map keeps its last heading rather than the car's 8-point compass). The map
 * glides there unless the change is too small to see. */
static void glide_to_model(void)
{
    DWORD now = GetTickCount();
    double mx, my, b = g_glide_b1, scale, still;
    if (!g_model.have_position)
        return;
    vmap_mercator(g_model.lat, g_model.lon, &mx, &my);
    if (g_model.heading_src == SRC_GPS)
        b = g_model.heading_deg;
    if (!g_glide_known) { /* the first position: no glide */
        g_glide_known = 1;
        g_glide_mx0 = g_glide_mx1 = mx;
        g_glide_my0 = g_glide_my1 = my;
        g_glide_b0 = g_glide_b1 = g_model.heading_src == SRC_GPS ? b : 0.0;
        glide_now(now);
        return;
    }
    scale = 256.0 * vmap_exp2(g_model.map_zoom);
    /* too small to see, or, parked (under 2 km/h), the GPS position wandering by a few metres: the
     * map stays (each glide ends in a sharp drawing of the whole map) */
    still = g_model.have_speed && g_model.speed_kmh < 2.0f ? 4.0 : 0.3;
    if ((mx - g_glide_mx1) * scale < still && (g_glide_mx1 - mx) * scale < still && (my - g_glide_my1) * scale < still &&
        (g_glide_my1 - my) * scale < still && turn_between(g_glide_b1, b) < 0.5 && turn_between(g_glide_b1, b) > -0.5)
        return;
    glide_now(now);
    g_glide_mx0 = g_model.map_mx;
    g_glide_my0 = g_model.map_my;
    g_glide_b0 = g_model.map_bearing;
    g_glide_mx1 = mx;
    g_glide_my1 = my;
    g_glide_b1 = b;
    g_glide_start = now;
    g_glide = 1;
    if (!g_anim_timer) {
        SetTimer(g_main, TIMER_ANIM, g_anim_ms, NULL);
        g_anim_timer = 1;
    }
}

/* The view a frame shows (the map page's or the dashboard's), for drawing missing pictures. */
static void map_view_now(VMapView *v)
{
    memset(v, 0, sizeof *v);
    v->style = g_model.night ? 0 : 1;
    v->dim = g_model.night;
    v->labels = 1;
    if (g_page == PAGE_MAP) {
        mapview_centre(&g_view, &g_model, &v->mx, &v->my);
        v->zoom = g_view.zoom;
        v->bearing = mapview_bearing(&g_view, &g_model);
        v->cx = SCREEN_W / 2;
        v->cy = SCREEN_H / 2;
    } else {
        v->mx = g_model.map_mx;
        v->my = g_model.map_my;
        v->zoom = g_model.map_zoom;
        v->bearing = g_model.map_bearing;
        v->cx = CAR_X;
        v->cy = CAR_Y;
    }
}

static void map_work_start(void)
{
    SetTimer(g_main, TIMER_MAPWORK, 1, NULL);
}

/* One missing picture, nearest the screen first: those on it (then a frame shows it), a ring
 * around it, the next zoom in and out, and the other look (day or night) for the switch. */
static void map_work(void)
{
    VMapView v, other;
    if (!g_map_ok || (g_page != PAGE_DASHBOARD && g_page != PAGE_MAP) || !g_model.have_position ||
        !IsWindowVisible(g_main)) {
        KillTimer(g_main, TIMER_MAPWORK);
        return;
    }
    map_view_now(&v);
    other = v;
    other.style = !v.style;
    other.dim = !v.dim;
    if (vt_fill(&g_tiles, &v, SCREEN_W, SCREEN_H, 0, 0.0)) {
        if (!g_glide)
            InvalidateRect(g_main, NULL, FALSE);
        return;
    }
    if (vt_fill(&g_tiles, &v, SCREEN_W, SCREEN_H, 2, 0.0) || vt_fill(&g_tiles, &v, SCREEN_W, SCREEN_H, 0, MAP_ZOOM_STEP) ||
        vt_fill(&g_tiles, &v, SCREEN_W, SCREEN_H, 0, -MAP_ZOOM_STEP) || vt_fill(&g_tiles, &other, SCREEN_W, SCREEN_H, 0, 0.0))
        return;
    KillTimer(g_main, TIMER_MAPWORK);
}

/* ---------------------------------------------------------------- address (read-only) */

/* The address file (*.addr, tools/address/) sits in the same Map folder. Lookups run on their
 * own thread, since one can read a few hundred kilobytes from the SD card; the result goes to the
 * screen only and is never logged (an address is as revealing as the position). */
static AddrFile g_addr;
static int g_addr_ok;
static CRITICAL_SECTION g_addr_lock;
static AddrResult g_addr_result;   /* guarded by g_addr_lock */
static int g_addr_state;           /* guarded: ADDRESS_WAITING, ADDRESS_OUTSIDE or ADDRESS_OK */
static DWORD g_addr_slowest_ms;

/* The first *.addr file in the Map folder that opens; "._" side files are skipped. */
static void address_open(void)
{
    WCHAR folder[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find, h;
    int bad = 0, hidden = 0;
    DWORD error = 0;

    sd_folder(folder, L"Map");
    _snwprintf(path, MAX_PATH, L"%s*.addr", folder);
    path[MAX_PATH - 1] = 0;
    find = FindFirstFileW(path, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (hidden_name(found.cFileName)) {
                hidden++;
                continue;
            }
            _snwprintf(path, MAX_PATH, L"%s%s", folder, found.cFileName);
            path[MAX_PATH - 1] = 0;
            h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                error = GetLastError();
                bad++;
            } else if (!addr_open(&g_addr, h, file_read_at)) {
                CloseHandle(h);
                bad++;
            } else {
                g_addr_ok = 1;
                break;
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (!g_addr_ok) {
        if (bad) {
            g_model.address_state = ADDRESS_UNREADABLE;
            log_line("[address] %d address files cannot be read (error %lu, %d ._ files skipped)", bad, error, hidden);
        } else {
            log_line("[address] no address file in the Map folder at the SD card root (%d ._ files skipped)", hidden);
        }
        return;
    }
    g_addr_state = ADDRESS_WAITING;
    log_line("[address] address file: %u x %u cells, %u items (%d unreadable, %d ._ files skipped)", g_addr.cols,
             g_addr.rows, g_addr.item_count, bad, hidden);
}

static DWORD WINAPI address_thread(LPVOID unused)
{
    int have_last = 0, last_lon = 0, last_lat = 0;
    DWORD last_ms = 0;

    while (!g_quit) {
        NmeaState gps;
        AddrResult r;
        DWORD now, took;
        int fresh, inside, dlat, dlon;

        Sleep(1000);
        now = GetTickCount();
        EnterCriticalSection(&g_gps_lock);
        gps = g_gps;
        fresh = gps.have_rmc && gps.valid && gps.have_position && now - g_gps_tick < 3000;
        LeaveCriticalSection(&g_gps_lock);
        if (!fresh)
            continue;
        dlat = gps.lat_e6 - last_lat;
        dlon = gps.lon_e6 - last_lon;
        if (dlat < 0)
            dlat = -dlat;
        if (dlon < 0)
            dlon = -dlon;
        if (have_last && dlat < 135 && dlon < 140 && now - last_ms < 30000)
            continue; /* again after about 15 m or 30 s */
        inside = addr_lookup(&g_addr, gps.lon_e6, gps.lat_e6, &r);
        took = GetTickCount() - now;
        if (took > g_addr_slowest_ms)
            g_addr_slowest_ms = took;
        EnterCriticalSection(&g_addr_lock);
        g_addr_result = r;
        g_addr_state = inside ? ADDRESS_OK : ADDRESS_OUTSIDE;
        LeaveCriticalSection(&g_addr_lock);
        have_last = 1;
        last_lat = gps.lat_e6;
        last_lon = gps.lon_e6;
        last_ms = now;
    }
    return 0;
}

static void copy_text(char *dst, const char *src, int size)
{
    int i;
    for (i = 0; i < size - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

/* The latest address into the model (UI thread). */
static void address_to_model(void)
{
    if (!g_addr_ok)
        return;
    EnterCriticalSection(&g_addr_lock);
    g_model.address_state = g_addr_state;
    copy_text(g_model.addr_road, g_addr_result.road, ADDRESS_TEXT);
    copy_text(g_model.addr_barangay, g_addr_result.barangay, ADDRESS_TEXT);
    copy_text(g_model.addr_city, g_addr_result.city, ADDRESS_TEXT);
    copy_text(g_model.addr_province, g_addr_result.province, ADDRESS_TEXT);
    copy_text(g_model.addr_place, g_addr_result.place, ADDRESS_TEXT);
    g_model.addr_road_m = g_addr_result.road_m;
    g_model.addr_place_m = g_addr_result.place_m;
    LeaveCriticalSection(&g_addr_lock);
}

/* The back buffer: an 800 x 480 32-bit DIB section (0x00RRGGBB, top row first) when
 * CreateDIBSection is there (looked up at run time, like the other optional GDI functions): the
 * screen has 32 bits per pixel, so copying it to the screen needs no conversion (about 9 ms against
 * 40 for a 16-bit one, measured with VMapTest). The map is drawn and the panels tinted straight
 * into its pixels, and GDI draws the text and shapes on the same bitmap. Otherwise an ordinary
 * bitmap: no map, and solid panels. */
typedef HBITMAP (WINAPI *dib_section_fn)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
static unsigned *g_back_bits;

static void create_back_buffer(HDC screen)
{
    BITMAPINFOHEADER info;
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    dib_section_fn create = core ? (dib_section_fn)GetProcAddressW(core, L"CreateDIBSection") : NULL;
    void *bits = NULL;

    g_back = CreateCompatibleDC(screen);
    if (create) {
        memset(&info, 0, sizeof info);
        info.biSize = sizeof info;
        info.biWidth = SCREEN_W;
        info.biHeight = -SCREEN_H; /* top-down */
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        g_back_bitmap = create(g_back, (const BITMAPINFO *)&info, DIB_RGB_COLORS, &bits, NULL, 0);
        if (g_back_bitmap == NULL || bits == NULL) {
            log_line("[screen] cannot create the 32-bit back buffer (error %lu): no map, solid panels", GetLastError());
            if (g_back_bitmap)
                DeleteObject(g_back_bitmap);
            g_back_bitmap = NULL;
            bits = NULL;
        }
    } else {
        log_line("[screen] CreateDIBSection missing: no map, solid panels");
    }
    if (g_back_bitmap == NULL)
        g_back_bitmap = CreateCompatibleBitmap(screen, SCREEN_W, SCREEN_H);
    SelectObject(g_back, g_back_bitmap);
    g_back_bits = (unsigned *)bits;
}

/* Row y (0 = top) of the back buffer. */
static unsigned *back_row(int y)
{
    return g_back_bits + (long)y * SCREEN_W;
}

static int same_view(const VMapView *a, const VMapView *b)
{
    double scale = 256.0 * vmap_exp2(a->zoom), dx = (a->mx - b->mx) * scale, dy = (a->my - b->my) * scale;
    double db = a->bearing - b->bearing;
    return a->zoom == b->zoom && a->style == b->style && a->dim == b->dim && a->cx == b->cx && a->cy == b->cy &&
           dx < 0.4 && dx > -0.4 && dy < 0.4 && dy > -0.4 && db < 0.3 && db > -0.3;
}

/* The map into the back buffer. While it moves (the glide, or a finger dragging the map page): a
 * frame from the map pictures, smooth, with the labels moved from their last placement. At rest:
 * drawn in full once (sharp, labels placed afresh) and kept, then reused while the view stays. */
static int gdi_map(void *ctx, int x, int y, int w, int h, double mx, double my, double zoom, double bearing, int cx,
                   int cy, int night)
{
    VSurface s;
    VMapView v;
    DWORD start = GetTickCount(), took;
    int moving = g_glide || (g_page == PAGE_MAP && g_view.dragging);
    if (!g_map_ok)
        return MAPDRAW_NO_FILES;
    if (g_back_bits == NULL || x != 0 || y != 0 || w != SCREEN_W || h != SCREEN_H) {
        gdi_fill_rect(ctx, x, y, w, h, g_vmap.land[night ? 0 : 1]);
        return MAPDRAW_OUTSIDE;
    }
    memset(&s, 0, sizeof s);
    s.px32 = g_back_bits;
    s.w = SCREEN_W;
    s.h = SCREEN_H;
    s.stride = SCREEN_W;
    memset(&v, 0, sizeof v);
    v.mx = mx;
    v.my = my;
    v.zoom = zoom;
    v.bearing = bearing;
    v.style = night ? 0 : 1; /* the dark map at half brightness at night, the light one by day */
    v.dim = night;
    v.cx = cx;
    v.cy = cy;
    v.labels = 1;
    if (moving) {
        vt_compose(&g_tiles, &s, &v, 1);
        vmap_draw(&g_vmap, &s, &v, VM_DRAW_LABELS_KEPT, NULL);
        g_rest_ok = 0;
        took = GetTickCount() - start;
        g_moving_frames++;
        g_moving_sum_ms += took;
        if (took > g_moving_slowest_ms)
            g_moving_slowest_ms = took;
    } else if (g_rest_ok && g_rest && same_view(&v, &g_rest_view)) {
        memcpy(g_back_bits, g_rest, SCREEN_W * SCREEN_H * sizeof(unsigned));
    } else {
        vmap_render(&g_vmap, &s, &v, NULL);
        if (g_rest) {
            memcpy(g_rest, g_back_bits, SCREEN_W * SCREEN_H * sizeof(unsigned));
            g_rest_view = v;
            g_rest_ok = 1;
        }
        took = GetTickCount() - start;
        g_sharp_count++;
        g_sharp_sum_ms += took;
        if (took > g_sharp_slowest_ms)
            g_sharp_slowest_ms = took;
    }
    if (g_tiles.missing > 0 || moving)
        map_work_start(); /* pictures for the frames to come */
    return mx >= g_map_box.left && mx <= g_map_box.right && my >= g_map_box.top && my <= g_map_box.bottom
               ? MAPDRAW_SHOWN
               : MAPDRAW_OUTSIDE;
}

/* Semi-transparent panel, mixed into the back buffer's pixels (GDI on Windows CE draws at once,
 * so the pixels are up to date). */
static void gdi_glass(void *ctx, int x, int y, int w, int h, int radius, unsigned rgb, int alpha)
{
    VSurface s;
    memset(&s, 0, sizeof s);
    s.px32 = g_back_bits;
    s.w = SCREEN_W;
    s.h = SCREEN_H;
    s.stride = SCREEN_W;
    vr_tint(&s, x, y, w, h, radius, rgb, alpha);
}

static void make_canvas(Canvas *c)
{
    c->ctx = g_back;
    c->fill_rect = gdi_fill_rect;
    c->round_rect = gdi_round_rect;
    c->polyline = gdi_polyline;
    c->polygon = gdi_polygon;
    c->circle = gdi_circle;
    c->text_width = gdi_text_width;
    c->text = gdi_text;
    c->map = gdi_map;
    c->glass = g_back_bits ? gdi_glass : NULL;
}

/* ---------------------------------------------------------------- holidays (read-only) */

/* holidays.txt next to the exe (tools/holidays/build_holidays.py): national holidays by the date,
 * local ones by the town nearest the GPS position, worked out from the list's own town centres
 * (holiday.c), never from the map or the address file. The log gets the national holidays; the
 * town and the local holidays found are never logged (as revealing as the position). */
#define HOLIDAY_FILE_MAX (128 * 1024)
#define TOWN_MOVED_E5 100           /* the town is looked up again after about 110 m ... */
#define TOWN_EVERY_MS 10000u        /* ... at most this often */
#define SHOWN_MAX 8
static HolidayList g_holidays;
static char g_holiday_text[HOLIDAY_FILE_MAX + 1];
static int g_holidays_ok;
static int g_town = -1, g_town_known, g_town_lat, g_town_lon; /* never logged */
static DWORD g_town_ms;
static long g_holiday_day = -1;     /* the day g_shown is for */
static char g_shown_name[SHOWN_MAX][HOLIDAY_NAME_MAX], g_shown_place[SHOWN_MAX][HOLIDAY_NAME_MAX];
static int g_shown_count;           /* holidays the notification has shown today */

static void holidays_open(void)
{
    WCHAR path[MAX_PATH];
    HANDLE h;
    DWORD got = 0;
    int y1, m1, d1, y2, m2, d2;

    _snwprintf(path, MAX_PATH, L"%sholidays.txt", g_dir);
    path[MAX_PATH - 1] = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("[holidays] no holidays.txt next to the program: no holidays shown");
        return;
    }
    if (!ReadFile(h, g_holiday_text, HOLIDAY_FILE_MAX, &got, NULL))
        got = 0;
    CloseHandle(h);
    if (got >= HOLIDAY_FILE_MAX)
        log_line("[holidays] holidays.txt is larger than %d KB: its end is left out", HOLIDAY_FILE_MAX / 1024);
    g_holiday_text[got] = 0;
    holiday_parse(&g_holidays, g_holiday_text, (int)got);
    g_holidays_ok = g_holidays.count > 0 && g_holidays.first_day <= g_holidays.last_day;
    sun_civil_from_days(g_holidays.first_day, &y1, &m1, &d1);
    sun_civil_from_days(g_holidays.last_day, &y2, &m2, &d2);
    log_line("[holidays] %d holidays, %d provinces, %d towns, for %04d-%02d-%02d to %04d-%02d-%02d (%d lines unreadable)",
             g_holidays.count, g_holidays.province_count, g_holidays.town_count, y1, m1, d1, y2, m2, d2,
             g_holidays.bad_lines);
}

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ---------------------------------------------------------------- holiday notification */

/* The notification moves (island.c) while the dashboard page is in front: then a 15 ms timer asks
 * for frames, and each frame redraws only the island's box. The screen under that box is copied
 * after every full repaint (g_under), so a frame puts back the part the last frame covered and
 * draws the new one there, and only that part goes to the screen; a full repaint (map, panels,
 * 800 x 480 copy) is far too slow for a smooth movement on this unit. The 1 s repaint and the map's
 * glide frames wait while it moves (a move lasts half a second). Without the 32-bit back buffer
 * every frame is a full
 * repaint. */
#define ISLAND_FRAME_MS 15
static Island g_island;
static int g_island_timer;          /* TIMER_ISLAND runs */
static int g_island_waiting;        /* a holiday not shown yet waits for the dashboard to be in front */
static unsigned g_under[ISLAND_AREA_W * ISLAND_AREA_H];
static int g_under_ok;
static int g_island_partial;        /* the next WM_PAINT is for the island alone */
static int g_tick_waiting;          /* the 1 s repaint waits until the island stops */
static unsigned g_island_frames;    /* frames that redrew the island alone, and the slowest (log) */
static DWORD g_island_slowest_ms;
static RECT g_island_drawn;         /* where the last frame was drawn (empty: nowhere) */

static int island_in_front(void)
{
    return g_page == PAGE_DASHBOARD && !g_menu && IsWindowVisible(g_main);
}

/* Starts the frame timer when the island moves. */
static void island_kick(void)
{
    if (island_step(&g_island, GetTickCount()) && !g_island_timer) {
        SetTimer(g_main, TIMER_ISLAND, ISLAND_FRAME_MS, NULL);
        g_island_timer = 1;
    }
}

/* The page was left, the menu opened or the window hidden: an open notification becomes the
 * button at once. */
static void island_leave(void)
{
    island_snap(&g_island);
    g_under_ok = 0;
}

static void island_announce(void)
{
    if (island_in_front()) {
        island_open(&g_island, GetTickCount());
        g_island_waiting = 0;
        island_kick();
    } else {
        g_island_waiting = 1;
    }
}

static void island_set_drawn(const IslandFrame *f)
{
    int x, y, w, h;
    island_bounds(f, &x, &y, &w, &h);
    if (f->w < 1 || f->h < 1)
        w = h = 0;
    g_island_drawn.left = x;
    g_island_drawn.top = y;
    g_island_drawn.right = x + w;
    g_island_drawn.bottom = y + h;
}

/* The dashboard under the island's area, from the back buffer (before the island is drawn). */
static void island_save_under(void)
{
    int x, y;
    if (g_back_bits == NULL)
        return;
    for (y = 0; y < ISLAND_AREA_H; y++) {
        const unsigned *src = back_row(ISLAND_AREA_Y + y) + ISLAND_AREA_X;
        unsigned *dst = g_under + y * ISLAND_AREA_W;
        for (x = 0; x < ISLAND_AREA_W; x++)
            dst[x] = src[x];
    }
    g_under_ok = 1;
}

/* Asks for a frame: only the part the last frame and this one cover, or the whole screen when
 * there is no copy of what lies under the island. */
static void island_invalidate(void)
{
    IslandFrame f;
    RECT r;
    int x, y, w, h;
    if (!g_under_ok || g_back_bits == NULL) {
        InvalidateRect(g_main, NULL, FALSE);
        return;
    }
    island_frame(&g_island, GetTickCount(), &f);
    island_bounds(&f, &x, &y, &w, &h);
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    if (g_island_drawn.right > g_island_drawn.left) {
        if (g_island_drawn.left < r.left) r.left = g_island_drawn.left;
        if (g_island_drawn.top < r.top) r.top = g_island_drawn.top;
        if (g_island_drawn.right > r.right) r.right = g_island_drawn.right;
        if (g_island_drawn.bottom > r.bottom) r.bottom = g_island_drawn.bottom;
    }
    if (r.left < ISLAND_AREA_X) r.left = ISLAND_AREA_X;
    if (r.top < ISLAND_AREA_Y) r.top = ISLAND_AREA_Y;
    if (r.right > ISLAND_AREA_X + ISLAND_AREA_W) r.right = ISLAND_AREA_X + ISLAND_AREA_W;
    if (r.bottom > ISLAND_AREA_Y + ISLAND_AREA_H) r.bottom = ISLAND_AREA_Y + ISLAND_AREA_H;
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    g_island_partial = 1;
    InvalidateRect(g_main, &r, FALSE);
}

static void island_timer(void)
{
    if (island_step(&g_island, GetTickCount())) {
        if (island_in_front())
            island_invalidate();
        return;
    }
    KillTimer(g_main, TIMER_ISLAND);
    g_island_timer = 0;
    if (island_in_front())
        island_invalidate(); /* the last frame, where the move ends */
    if (g_tick_waiting) {
        g_tick_waiting = 0;
        InvalidateRect(g_main, NULL, FALSE);
    }
}

/* 1 when a WM_PAINT for rc can be served by redrawing the island alone. */
static int island_partial_ok(const RECT *rc)
{
    return g_island_partial && g_under_ok && g_back_bits != NULL && island_in_front() && rc->left >= ISLAND_AREA_X &&
           rc->top >= ISLAND_AREA_Y && rc->right <= ISLAND_AREA_X + ISLAND_AREA_W &&
           rc->bottom <= ISLAND_AREA_Y + ISLAND_AREA_H && rc->right > rc->left && rc->bottom > rc->top;
}

/* A frame of the island: the dashboard under rc put back from the copy, the island drawn, rc copied
 * to the screen. */
static void paint_island(HDC target, const RECT *rc)
{
    Canvas c;
    IslandFrame f;
    int x, y;
    for (y = rc->top; y < rc->bottom; y++) {
        const unsigned *src = g_under + (y - ISLAND_AREA_Y) * ISLAND_AREA_W + (rc->left - ISLAND_AREA_X);
        unsigned *dst = back_row(y) + rc->left;
        for (x = 0; x < rc->right - rc->left; x++)
            dst[x] = src[x];
    }
    make_canvas(&c);
    island_frame(&g_island, GetTickCount(), &f);
    render_island(&c, &g_model, &f);
    island_set_drawn(&f);
    BitBlt(target, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, g_back, rc->left, rc->top, SRCCOPY);
}

/* Today's holidays into the model (once a second): national ones by the date, local ones for the
 * town the car is in. The notification opens for a holiday it has not shown yet today. */
static void holidays_update(void)
{
    HolidayRow rows[HOLIDAY_ROWS];
    DWORD now = GetTickCount();
    long day;
    int n = 0, i, j, fresh = 0, covered;

    if (!g_holidays_ok || !g_model.clock_valid)
        return;
    covered = holiday_covers(&g_holidays, g_model.year, g_model.month, g_model.day);
    day = sun_days_from_civil(g_model.year, g_model.month, g_model.day);
    if (day != g_holiday_day) { /* a new day: the national holidays go to the log, never local ones */
        g_holiday_day = day;
        g_shown_count = 0;
        if (!covered) {
            log_line("[holidays] %04d-%02d-%02d is outside the holiday list's dates: no holidays shown", g_model.year,
                     g_model.month, g_model.day);
        } else {
            n = holiday_today(&g_holidays, g_model.year, g_model.month, g_model.day, -1, rows, HOLIDAY_ROWS);
            if (n == 0)
                log_line("[holidays] %04d-%02d-%02d: no national holiday", g_model.year, g_model.month, g_model.day);
            for (i = 0; i < n; i++)
                log_line("[holidays] %04d-%02d-%02d: %s (%s)", g_model.year, g_model.month, g_model.day, rows[i].name,
                         rows[i].kind == HOLIDAY_REGULAR ? "regular holiday"
                         : rows[i].kind == HOLIDAY_SPECIAL ? "special non-working day" : "special working day");
        }
    }
    n = 0;
    if (covered) {
        if (g_model.have_position) {
            double la = g_model.lat * 1e5, lo = g_model.lon * 1e5;
            int lat = (int)(la < 0 ? la - 0.5 : la + 0.5), lon = (int)(lo < 0 ? lo - 0.5 : lo + 0.5);
            int dlat = lat > g_town_lat ? lat - g_town_lat : g_town_lat - lat;
            int dlon = lon > g_town_lon ? lon - g_town_lon : g_town_lon - lon;
            if (!g_town_known || ((dlat > TOWN_MOVED_E5 || dlon > TOWN_MOVED_E5) && now - g_town_ms >= TOWN_EVERY_MS)) {
                g_town = holiday_locate(&g_holidays, lat, lon);
                g_town_known = 1;
                g_town_lat = lat;
                g_town_lon = lon;
                g_town_ms = now;
            }
        }
        n = holiday_today(&g_holidays, g_model.year, g_model.month, g_model.day, g_town_known ? g_town : -1, rows,
                          HOLIDAY_ROWS);
    }
    for (i = 0; i < n; i++) {
        for (j = 0; j < g_shown_count; j++)
            if (same_text(g_shown_name[j], rows[i].name) && same_text(g_shown_place[j], rows[i].place))
                break;
        if (j < g_shown_count)
            continue;
        fresh = 1;
        if (g_shown_count < SHOWN_MAX) {
            copy_text(g_shown_name[g_shown_count], rows[i].name, HOLIDAY_NAME_MAX);
            copy_text(g_shown_place[g_shown_count], rows[i].place, HOLIDAY_NAME_MAX);
            g_shown_count++;
        }
    }
    for (i = 0; i < n; i++)
        g_model.holidays[i] = rows[i];
    g_model.holiday_count = n;
    island_set_rows(&g_island, n, now);
    if (n == 0)
        g_island_waiting = 0;
    else if (fresh || (g_island_waiting && island_in_front()))
        island_announce();
    island_kick();
}

static void paint(HDC target)
{
    Canvas c;
    make_canvas(&c);
    if (g_page == PAGE_CARINFO) {
        render_carinfo(&c, &g_model, g_carinfo_cat);
        g_under_ok = 0;
    } else if (g_page == PAGE_MAP) {
        render_mappage(&c, &g_model, &g_view);
        g_under_ok = 0;
    } else {
        IslandFrame f;
        render_dashboard(&c, &g_model);
        island_save_under();
        island_frame(&g_island, GetTickCount(), &f);
        render_island(&c, &g_model, &f);
        island_set_drawn(&f);
        if (g_menu)
            render_menu(&c, &g_model, g_has_files, DASH_VERSION);
    }
    BitBlt(target, 0, 0, SCREEN_W, SCREEN_H, g_back, 0, 0, SRCCOPY);
}

/* ---------------------------------------------------------------- model tick */

static void tick(void)
{
    DashInputs in;
    SYSTEMTIME local, utc;
    DWORD now = GetTickCount();

    memset(&in, 0, sizeof in);
    in.now_ms = now;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    in.clock_valid = local.wYear >= 2020;
    in.year = local.wYear;
    in.month = local.wMonth;
    in.day = local.wDay;
    in.hour = local.wHour;
    in.minute = local.wMinute;
    in.second = local.wSecond;
    in.system_utc_offset_min =
        (int)((sun_days_from_civil(local.wYear, local.wMonth, local.wDay) - sun_days_from_civil(utc.wYear, utc.wMonth, utc.wDay)) * 1440 +
              (local.wHour * 60 + local.wMinute) - (utc.wHour * 60 + utc.wMinute));
    EnterCriticalSection(&g_gps_lock);
    in.gps = g_gps;
    in.gps_fresh = g_gps.have_rmc && now - g_gps_tick < 3000;
    LeaveCriticalSection(&g_gps_lock);
    in.car = g_car;
    in.car_fresh = g_car_tick != 0 && now - g_car_tick < 30000;
    in.lights_on = g_lights;
    in.tz_override = g_tz_set;
    in.tz_override_min = g_tz_min;
    dash_update(&g_model, &in);
    {
        static int last_night = -1, last_src = -1;
        if (g_model.night != last_night || g_model.night_src != last_src)
            log_line("[colours] %s, from %s", g_model.night ? "night" : "day",
                     g_model.night_src == SRC_GPS ? "sunset and sunrise" : "the headlights (no GPS time and position yet)");
        last_night = g_model.night;
        last_src = g_model.night_src;
    }
    glide_to_model();
    address_to_model();
    holidays_update();
    if (g_model.clock_src == SRC_GPS) { /* once: what the GPS clock gave against the unit's clock */
        static int logged;
        if (!logged++)
            log_line("[clock] GPS time: %04d-%02d-%02d %02d:%02d local, UTC%+d min (time-zone setting %+d min)",
                     g_model.year, g_model.month, g_model.day, g_model.hour, g_model.minute,
                     g_model.utc_offset_min, in.system_utc_offset_min);
    }
}

static void log_stats(void)
{
    NmeaState gps;
    EnterCriticalSection(&g_gps_lock);
    gps = g_gps;
    LeaveCriticalSection(&g_gps_lock);
    log_line("[stats] gps %s, %d sats, %u sentences (%u bad) | bus %s, %u records, %u replies, %u requests (%u failed), "
             "%u resyncs | elevation from %s, heading from %s",
             g_model.gps_ok ? "fix" : "no fix", gps.satellites, gps.sentences, gps.bad_checksums,
             g_bus.active ? "on" : "off", g_bus.records, g_bus.replies, g_bus.tx, g_bus.tx_failed, g_bus.ring.resyncs,
             g_model.elev_src == SRC_GPS ? "GPS" : g_model.elev_src == SRC_CAR ? "car" : "none",
             g_model.heading_src == SRC_GPS ? "GPS" : g_model.heading_src == SRC_CAR ? "car" : "none");
    if (g_map_ok)
        log_line("[map] zoom %.1f | moving: %u frames, map %u ms on average, slowest %lu ms; at rest: %u sharp drawings, "
                 "%u ms on average, slowest %lu ms | %d pictures drawn, %u ms each on average, %d kept | map page "
                 "opened %u times, slowest frame while dragging %lu ms", g_model.map_zoom, g_moving_frames,
                 g_moving_frames ? g_moving_sum_ms / g_moving_frames : 0, g_moving_slowest_ms, g_sharp_count,
                 g_sharp_count ? g_sharp_sum_ms / g_sharp_count : 0, g_sharp_slowest_ms, g_tiles.drawn,
                 g_tiles.drawn ? g_tiles.us_drawing / g_tiles.drawn / 1000 : 0, g_tiles.allocated, g_page_opened,
                 g_drag_slowest_all_ms);
    if (g_holidays_ok) /* counts only: never the town */
        log_line("[holidays] notification: %u frames drawn alone, slowest %lu ms", g_island_frames, g_island_slowest_ms);
    if (g_addr_ok) /* counts only: never the address */
        log_line("[address] %u lookups, %u read errors, %u outlines skipped, slowest lookup %lu ms", g_addr.lookups,
                 g_addr.read_errors, g_addr.skipped, g_addr_slowest_ms);
}

/* ---------------------------------------------------------------- map page */

/* Opened by tapping the car on the dashboard's map, at the dashboard's zoom, following the car.
 * The log gets the zoom and, when it closes, how dragging went: never a position. */
static void map_page_open(void)
{
    g_page = PAGE_MAP;
    mapview_open(&g_view, g_model.map_zoom, GetTickCount());
    g_page_opened++;
    g_drag_frames = 0;
    g_drag_slowest_ms = 0;
    g_rest_ok = 0;
    log_line("[page] map, zoom %.1f", g_view.zoom);
}

static void map_page_close(void)
{
    g_page = PAGE_DASHBOARD;
    g_rest_ok = 0;
    log_line("[page] dashboard (map page: %u frames while dragging, slowest %lu ms)", g_drag_frames, g_drag_slowest_ms);
}

/* A finger down on the map page: a button, or the start of a drag. Returns 1 when the page needs
 * drawing again. */
static int map_page_press(int x, int y)
{
    DWORD now = GetTickCount();
    int hit = render_mappage_hit(x, y, &g_view);
    double z;

    switch (hit) {
    case MAPPAGE_BACK:
        map_page_close();
        return 1;
    case MAPPAGE_ZOOM_IN:
    case MAPPAGE_ZOOM_OUT:
        z = zoom_step(g_view.zoom, hit == MAPPAGE_ZOOM_IN ? 1 : -1);
        if (z == g_view.zoom)
            return 0;
        mapview_set_zoom(&g_view, z, now);
        return 1;
    case MAPPAGE_CENTRE:
        mapview_follow(&g_view);
        return 1;
    }
    mapview_press(&g_view, &g_model, x, y, now);
    return 0;
}

/* The finger moved on the map page (x, y); returns 1 when the map moved. */
static int map_page_move(int x, int y)
{
    if (!g_view.pressed)
        return 0;
    return mapview_move(&g_view, x, y, g_map_ok ? &g_map_box : NULL, GetTickCount());
}

/* A frame of the glide (TIMER_ANIM): the model's map view moves on; the screen is drawn again
 * where the map shows (the dashboard, or the map page while it follows the car). Frames come at
 * most every ANIM_MS, and further apart when they take long, so the unit keeps time for the rest;
 * they wait while the holiday notification moves. */
static void glide_frame(void)
{
    DWORD now = GetTickCount();
    glide_now(now);
    if (!g_glide) {
        KillTimer(g_main, TIMER_ANIM);
        g_anim_timer = 0;
    }
    if (g_island_timer && island_in_front())
        return;
    if (IsWindowVisible(g_main) && !g_menu && (g_page == PAGE_DASHBOARD || (g_page == PAGE_MAP && g_view.follow)))
        InvalidateRect(g_main, NULL, FALSE); /* the last one, at rest, is drawn sharp */
}

/* After a frame of the glide: frames further apart when they take long (a third of the time left
 * for the rest of the unit). */
static void glide_pace(DWORD took)
{
    DWORD want = took * 3 / 2 > ANIM_MS ? took * 3 / 2 : ANIM_MS;
    if (!g_anim_timer || (want < g_anim_ms + 15 && want + 15 > g_anim_ms))
        return;
    g_anim_ms = want;
    SetTimer(g_main, TIMER_ANIM, g_anim_ms, NULL);
}

/* ---------------------------------------------------------------- window */

static void open_file_manager(void)
{
    PROCESS_INFORMATION pi;
    if (CreateProcessW(g_files_path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        log_line("[menu] file manager started");
    } else {
        log_line("[menu] cannot start the file manager (error %lu)", GetLastError());
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_TIMER:
        switch (wparam) {
        case TIMER_TICK:
            tick();
            if (g_page == PAGE_MAP)
                mapview_idle(&g_view, GetTickCount()); /* follows the car again after 30 s untouched */
            island_kick(); /* an open notification closes by itself after a while */
            if (g_island_timer && island_in_front())
                g_tick_waiting = 1; /* repaint once the notification stops moving */
            else if (IsWindowVisible(hwnd))
                InvalidateRect(hwnd, NULL, FALSE);
            break;
        case TIMER_ISLAND: island_timer(); break;
        case TIMER_ANIM: glide_frame(); break;
        case TIMER_MAPWORK: map_work(); break;
        case TIMER_RING: bus_poll(); break;
        case TIMER_FIRST:
            KillTimer(hwnd, TIMER_FIRST);
            send_requests();
            log_line("[bus] requests sent: %u of %u posted", g_bus.tx - g_bus.tx_failed, g_bus.tx);
            SetTimer(hwnd, TIMER_REQUESTS, 5000, NULL);
            break;
        case TIMER_REQUESTS:
            if (g_bus.active)
                send_requests();
            break;
        case TIMER_HOLD:
            KillTimer(hwnd, TIMER_HOLD);
            g_menu = 1;
            island_leave();
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case TIMER_STATS: log_stats(); break;
        case TIMER_BUS_RETRY: bus_start(); break;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
        if (g_menu) {
            int hit = render_menu_hit(x, y, g_has_files);
            switch (hit) {
            case MENU_FILES: g_menu = 0; open_file_manager(); break;
            case MENU_CLOSE: log_line("[menu] close"); DestroyWindow(hwnd); return 0;
            case MENU_BACK: g_menu = 0; break;
            case MENU_TZ_MINUS:
            case MENU_TZ_PLUS: { /* 30-minute steps from the zone in use, UTC-12 to UTC+14 */
                int v = g_model.utc_offset_min + (hit == MENU_TZ_PLUS ? 30 : -30);
                v = v < -720 ? -720 : v > 840 ? 840 : v;
                g_tz_set = 1;
                g_tz_min = v;
                save_settings();
                g_model.sun_valid = 0; /* work the sunset out again at once */
                tick();
                log_line("[menu] time zone %+d min", v);
                break;
            }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_page == PAGE_MAP) {
            if (map_page_press(x, y))
                InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_page == PAGE_CARINFO) {
            int cat = render_carinfo_category_hit(x, y);
            if (render_carinfo_back_hit(x, y)) {
                g_page = PAGE_DASHBOARD;
                log_line("[page] dashboard");
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (cat >= 0 && cat != g_carinfo_cat) {
                g_carinfo_cat = cat;
                log_line("[page] car info, category %d", cat);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (island_hit(&g_island, GetTickCount(), x, y) != ISLAND_HIT_NONE) {
            if (island_hit(&g_island, GetTickCount(), x, y) == ISLAND_HIT_DOT)
                island_open(&g_island, GetTickCount()); /* the button: the notification again */
            else
                island_close(&g_island, GetTickCount());
            island_kick();
        } else if (render_carinfo_button_hit(x, y)) {
            g_page = PAGE_CARINFO;
            island_leave();
            log_line("[page] car info");
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (render_map_hit(x, y) != MAPHIT_NONE && g_map_ok && g_model.have_position) {
            double z = zoom_step(g_model.map_zoom, render_map_hit(x, y) == MAPHIT_ZOOM_IN ? 1 : -1);
            if (z != g_model.map_zoom) {
                g_model.map_zoom = z;
                save_settings(); /* remembered: the dashboard starts at this zoom next time */
                log_line("[map] zoom %.1f", z);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (render_car_hit(x, y) && g_map_ok && g_back_bits != NULL && g_model.have_position) {
            map_page_open(); /* the car on the map: only where the map picture is drawn */
            island_leave();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (render_clock_hit(x, y)) {
            SetTimer(hwnd, TIMER_HOLD, 2000, NULL);
        }
        return 0;
    }
    case WM_MOUSEMOVE: /* Windows CE sends it only while the finger is down */
        if (g_page == PAGE_MAP && map_page_move((short)LOWORD(lparam), (short)HIWORD(lparam)))
            InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        KillTimer(hwnd, TIMER_HOLD);
        if (g_page == PAGE_MAP && g_view.pressed) {
            map_page_move((short)LOWORD(lparam), (short)HIWORD(lparam));
            mapview_release(&g_view, GetTickCount());
            InvalidateRect(hwnd, NULL, FALSE); /* at rest: drawn sharp */
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wparam)
            InvalidateRect(hwnd, NULL, FALSE);
        else
            island_leave();
        if (!wparam && g_view.pressed) /* hidden with the finger down: no WM_LBUTTONUP will come */
            mapview_release(&g_view, GetTickCount());
        break;
    case WM_ERASEBKGND:
        return 1; /* paint() covers everything */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        DWORD start = GetTickCount(), took;
        int dragging = g_page == PAGE_MAP && g_view.dragging, gliding = g_glide;
        HDC dc = BeginPaint(hwnd, &ps);
        if (island_partial_ok(&ps.rcPaint)) {
            paint_island(dc, &ps.rcPaint);
            took = GetTickCount() - start;
            g_island_frames++;
            if (took > g_island_slowest_ms)
                g_island_slowest_ms = took;
        } else {
            paint(dc);
        }
        g_island_partial = 0;
        EndPaint(hwnd, &ps);
        took = GetTickCount() - start;
        if (dragging) {
            g_drag_frames++;
            if (took > g_drag_slowest_ms)
                g_drag_slowest_ms = took;
            if (took > g_drag_slowest_all_ms)
                g_drag_slowest_all_ms = took;
        }
        if (gliding)
            glide_pace(took);
        return 0;
    }
    case MSG_SYSINFO:
        if (wparam == SYSINFO_LIGHTS)
            set_lights(lparam != 0, "0x52A");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HDC screen;
    HANDLE thread;
    WCHAR *slash;
    int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
    HWND running = FindWindowW(WINDOW_CLASS, NULL);

    if (running != NULL) { /* already running (Main.exe normally finds it by title): bring it back */
        ShowWindow(running, SW_SHOW);
        SetForegroundWindow(running);
        return 0;
    }
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    slash = wcsrchr(g_dir, L'\\');
    if (slash)
        slash[1] = 0;
    else
        wcscpy(g_dir, L"\\");
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_gps_lock);
    InitializeCriticalSection(&g_addr_lock);
    open_log();
    log_line("=== Montero dashboard %s started (screen %dx%d)", DASH_VERSION, cx, cy);

    _snwprintf(g_files_path, MAX_PATH, L"%sTotalCommander\\cecmd.exe", g_dir);
    g_files_path[MAX_PATH - 1] = 0;
    g_has_files = GetFileAttributesW(g_files_path) != 0xFFFFFFFF;
    {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        if (lt.wYear < 2020)
            log_line("[clock] the head unit's clock is not set (year %d): the clock waits for GPS time", lt.wYear);
    }
    load_settings();
    dash_init(&g_model);
    map_open();
    address_open();
    holidays_open();
    island_init(&g_island);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = instance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, WS_VISIBLE | WS_POPUP, 0, 0, cx, cy, NULL, NULL, instance,
                             NULL);
    if (g_main == NULL) {
        log_line("cannot create the window (error %lu)", GetLastError());
        return 1;
    }
    screen = GetDC(g_main);
    create_back_buffer(screen);
    resolve_gdi();
    load_fonts(g_back);
    ReleaseDC(g_main, screen);

    thread = CreateThread(NULL, 0, gps_thread, NULL, 0, NULL);
    if (thread)
        CloseHandle(thread);
    if (g_addr_ok) {
        thread = CreateThread(NULL, 0, address_thread, NULL, 0, NULL);
        if (thread)
            CloseHandle(thread);
    }
    bus_start();
    tick();
    SetTimer(g_main, TIMER_TICK, 1000, NULL);
    SetTimer(g_main, TIMER_STATS, 600000, NULL);
    InvalidateRect(g_main, NULL, FALSE);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    InterlockedExchange((LONG *)&g_quit, 1);
    log_stats();
    log_line("=== exit");
    Sleep(300); /* let the GPS thread leave its read */
    unload_fonts();
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    return 0;
}
