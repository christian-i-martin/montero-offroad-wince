/* Montero dashboard: replaces \SD\Navigation\Navigation.exe on the Skypine 6982E head unit
 * (Windows CE 6, ARM). Two full-screen pages: the dashboard, and the car info page opened with the
 * car button at the right end of its header (Back returns). Layout in render.c, logic in model.c.
 *
 * Data sources (all read-only, see docs/vehicle-data-research.md):
 *   GPS       NMEA from COM1: at 38400 baud, opened for reading only
 *   CarInfo   joins Main.exe's message bus as "TerminalA" (0x1A), reads the shared-memory ring and
 *             posts the factory Car Info screen's read requests with sender 0x14 every 5 s
 *   lights    window message 0x52A index 9 broadcast by Main.exe, and CarInfo frame 0x33
 *   map       *.pack files in the Map folder at the SD card root (tools/map/), opened read-only
 *   address   the *.addr file in the same folder (tools/address/), read-only, looked up on its own
 *             thread; shown on screen only, never logged
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
#include "map.h"
#include "model.h"
#include "nmea.h"
#include "render.h"
#include "sun.h"

#define DASH_VERSION "0.5.2"
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
static int g_carinfo;             /* the car info page is shown instead of the dashboard */
static WCHAR g_files_path[MAX_PATH];
#define DEFAULT_UTC_OFFSET_MIN 480 /* UTC+8 until another time zone is chosen in the menu */
static int g_tz_set = 1, g_tz_min = DEFAULT_UTC_OFFSET_MIN; /* time zone in use, minutes from UTC */

/* dashboard-settings.txt next to the exe, one line: utc_offset_minutes=<n> */
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

static void load_settings(void)
{
    static const char key[] = "utc_offset_minutes=";
    WCHAR path[MAX_PATH];
    char buf[128];
    const char *p;
    DWORD got = 0;
    HANDLE h;
    int sign = 1, v = 0, digits = 0;

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
    p = find_text(buf, key, (int)sizeof key - 1);
    if (p == NULL)
        return;
    p += sizeof key - 1;
    if (*p == '-' || *p == '+')
        sign = *p++ == '-' ? -1 : 1;
    for (; *p >= '0' && *p <= '9' && digits < 4; p++, digits++)
        v = v * 10 + (*p - '0');
    v *= sign;
    if (digits && v >= -720 && v <= 840) {
        g_tz_set = 1;
        g_tz_min = v;
        log_line("[clock] time zone from dashboard-settings.txt: %+d min", v);
    }
}

static void save_settings(void)
{
    WCHAR path[MAX_PATH];
    char line[48];
    DWORD written;
    HANDLE h;
    int len = _snprintf(line, sizeof line, "utc_offset_minutes=%d\r\n", g_tz_min);

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

/* ---------------------------------------------------------------- offline map (read-only) */

/* The map files sit in a Map folder at the SD card root, beside the Navigation folder, so that
 * installing a new dashboard does not mean copying the map again: the dark map in Map, the light
 * one for the day in Map\Day. map.c composes the picture straight into the back buffer (a 16-bit
 * DIB section, see create_back_buffer), where the panels are then tinted over it. The full screen
 * needs up to 15 tiles at once (5 across, 3 down), so 20 are kept (128 KB each). */
#define MAP_CACHE_TILES 20
static MapSet g_map;
static int g_map_ok;
static int g_map_style = -1;        /* style asked for last: MAP_DARK at night, MAP_LIGHT by day */
static int g_zoom_touched;          /* the zoom buttons were used: keep that zoom, else the highest level */
static DWORD g_map_slowest_ms;

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
 * "._name" side file next to each one (macOS keeps file details there), which matches "*.pack" and
 * "*.addr" too but is not a map or address file. */
static int hidden_name(const WCHAR *name)
{
    return name[0] == L'.';
}

/* The Map folder at the SD card root, with a trailing backslash: "\SD\Navigation\" -> "\SD\Map\". */
static void sd_map_folder(WCHAR *folder)
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
    _snwprintf(folder, MAX_PATH, L"%sMap\\", root);
    folder[MAX_PATH - 1] = 0;
}

/* Opens every *.pack in folder (read-only) as a pack of the given style. */
static void add_packs(const WCHAR *folder, int style, int *bad, int *hidden)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;

    _snwprintf(path, MAX_PATH, L"%s*.pack", folder);
    path[MAX_PATH - 1] = 0;
    find = FindFirstFileW(path, &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        HANDLE h;
        if (hidden_name(found.cFileName)) {
            (*hidden)++;
            continue;
        }
        _snwprintf(path, MAX_PATH, L"%s%s", folder, found.cFileName);
        path[MAX_PATH - 1] = 0;
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            (*bad)++;
        } else if (!map_add_pack_style(&g_map, h, file_read_at, style)) {
            CloseHandle(h);
            (*bad)++;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

/* The zoom range of the style in use; the highest level unless the zoom buttons were used. */
static void map_zoom_range(void)
{
    g_model.map_min_zoom = g_map.min_zoom;
    g_model.map_max_zoom = g_map.max_zoom;
    if (!g_zoom_touched || g_model.map_zoom > g_map.max_zoom)
        g_model.map_zoom = g_map.max_zoom;
    if (g_model.map_zoom < g_map.min_zoom)
        g_model.map_zoom = g_map.min_zoom;
    while (g_model.map_zoom > g_map.min_zoom && !map_has_zoom(&g_map, g_model.map_zoom))
        g_model.map_zoom--;
}

static void map_open(void)
{
    WCHAR folder[MAX_PATH], day[MAX_PATH];
    int bad = 0, hidden = 0;

    if (!map_init(&g_map, MAP_CACHE_TILES)) {
        log_line("[map] not enough memory for the map");
        return;
    }
    g_map_ok = 1;
    sd_map_folder(folder);
    _snwprintf(day, MAX_PATH, L"%sDay\\", folder);
    day[MAX_PATH - 1] = 0;
    add_packs(folder, MAP_DARK, &bad, &hidden);
    add_packs(day, MAP_LIGHT, &bad, &hidden);
    if (g_map.count == 0) {
        log_line("[map] no map files in the Map folder at the SD card root (%d unreadable, %d ._ files skipped)", bad,
                 hidden);
        return;
    }
    map_use_style(&g_map, MAP_DARK);
    map_zoom_range();
    log_line("[map] %d dark and %d light (Day folder) map files, zoom %d to %d, showing %d (%d unreadable, %d ._ "
             "files skipped)", g_map.style_packs[MAP_DARK], g_map.style_packs[MAP_LIGHT], g_map.min_zoom,
             g_map.max_zoom, g_model.map_zoom, bad, hidden);
}

/* Dark map at night, light map by day (the dark one when there is no light one). */
static void map_follow_colours(void)
{
    int want = g_model.night ? MAP_DARK : MAP_LIGHT, used;
    if (!g_map_ok || g_map.count == 0 || want == g_map_style)
        return;
    g_map_style = want;
    used = map_use_style(&g_map, want);
    map_zoom_range();
    log_line("[map] %s map, zoom %d to %d, showing %d", used == MAP_LIGHT ? "light" : "dark", g_map.min_zoom,
             g_map.max_zoom, g_model.map_zoom);
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

    sd_map_folder(folder);
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

/* One zoom level in or out, skipping levels the map files do not have. */
static void map_step(int direction)
{
    int z = g_model.map_zoom + direction;
    while (z >= g_map.min_zoom && z <= g_map.max_zoom && !map_has_zoom(&g_map, z))
        z += direction;
    if (z >= g_map.min_zoom && z <= g_map.max_zoom)
        g_model.map_zoom = z;
    g_zoom_touched = 1;
}

/* The back buffer: an 800 x 480 RGB565 DIB section when CreateDIBSection is there (looked up at
 * run time, like the other optional GDI functions), so the map can be composed and the panels
 * tinted straight into its pixels while GDI draws the text and shapes on the same bitmap.
 * Otherwise an ordinary bitmap: no map picture, and solid panels. */
typedef HBITMAP (WINAPI *dib_section_fn)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
static unsigned short *g_back_bits; /* bottom-up: the top row is the last one in memory */
static int g_back_stride;           /* pixels per row */

static void create_back_buffer(HDC screen)
{
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } info;
    HMODULE core = GetModuleHandleW(L"coredll.dll");
    dib_section_fn create = core ? (dib_section_fn)GetProcAddressW(core, L"CreateDIBSection") : NULL;
    void *bits = NULL;

    g_back = CreateCompatibleDC(screen);
    if (create) {
        memset(&info, 0, sizeof info);
        info.h.biSize = sizeof info.h;
        info.h.biWidth = SCREEN_W;
        info.h.biHeight = SCREEN_H; /* bottom-up, which every Windows CE version supports */
        info.h.biPlanes = 1;
        info.h.biBitCount = 16;
        info.h.biCompression = BI_BITFIELDS;
        info.masks[0] = 0xF800; /* RGB565 */
        info.masks[1] = 0x07E0;
        info.masks[2] = 0x001F;
        g_back_bitmap = create(g_back, (const BITMAPINFO *)&info, DIB_RGB_COLORS, &bits, NULL, 0);
        if (g_back_bitmap == NULL || bits == NULL) {
            log_line("[screen] cannot create the 16-bit back buffer (error %lu): no map picture, solid panels",
                     GetLastError());
            if (g_back_bitmap)
                DeleteObject(g_back_bitmap);
            g_back_bitmap = NULL;
            bits = NULL;
        }
    } else {
        log_line("[screen] CreateDIBSection missing: no map picture, solid panels");
    }
    if (g_back_bitmap == NULL)
        g_back_bitmap = CreateCompatibleBitmap(screen, SCREEN_W, SCREEN_H);
    SelectObject(g_back, g_back_bitmap);
    g_back_bits = (unsigned short *)bits;
    g_back_stride = ((SCREEN_W * 2 + 3) & ~3) / 2; /* rows are padded to 4 bytes */
}

/* Row y (0 = top) of the back buffer. */
static unsigned short *back_row(int y)
{
    return g_back_bits + (long)(SCREEN_H - 1 - y) * g_back_stride;
}

static int gdi_map(void *ctx, int x, int y, int w, int h, int zoom, double px, double py, int night)
{
    DWORD start = GetTickCount(), took;
    int result;
    if (!g_map_ok || g_map.count == 0)
        return MAP_NO_FILES;
    if (g_back_bits == NULL || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > SCREEN_W || y + h > SCREEN_H) {
        gdi_fill_rect(ctx, x, y, w, h, MAP_OUTSIDE_RGB);
        return MAP_OUTSIDE;
    }
    result = map_compose(&g_map, back_row(y) + x, w, h, -g_back_stride, zoom, px, py, night);
    took = GetTickCount() - start;
    if (took > g_map_slowest_ms)
        g_map_slowest_ms = took;
    return result;
}

/* Semi-transparent panel, mixed into the back buffer's pixels (GDI on Windows CE draws at once,
 * so the pixels are up to date). */
static void gdi_glass(void *ctx, int x, int y, int w, int h, int radius, unsigned rgb, int alpha)
{
    map_tint(back_row(0), SCREEN_W, SCREEN_H, -g_back_stride, x, y, w, h, radius, rgb, alpha);
}

static void paint(HDC target)
{
    Canvas c;
    c.ctx = g_back;
    c.fill_rect = gdi_fill_rect;
    c.round_rect = gdi_round_rect;
    c.polyline = gdi_polyline;
    c.polygon = gdi_polygon;
    c.circle = gdi_circle;
    c.text_width = gdi_text_width;
    c.text = gdi_text;
    c.map = gdi_map;
    c.glass = g_back_bits ? gdi_glass : NULL;
    if (g_carinfo) {
        render_carinfo(&c, &g_model);
    } else {
        render_dashboard(&c, &g_model);
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
    map_follow_colours();
    address_to_model();
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
    if (g_map.count > 0)
        log_line("[map] zoom %d, %u tiles decoded, %u failed, %u read errors, slowest map draw %lu ms",
                 g_model.map_zoom, g_map.decoded, g_map.failed, g_map.read_errors, g_map_slowest_ms);
    if (g_addr_ok) /* counts only: never the address */
        log_line("[address] %u lookups, %u read errors, %u outlines skipped, slowest lookup %lu ms", g_addr.lookups,
                 g_addr.read_errors, g_addr.skipped, g_addr_slowest_ms);
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
            if (IsWindowVisible(hwnd))
                InvalidateRect(hwnd, NULL, FALSE);
            break;
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
        } else if (g_carinfo) {
            if (render_carinfo_back_hit(x, y)) {
                g_carinfo = 0;
                log_line("[page] dashboard");
                InvalidateRect(hwnd, NULL, FALSE);
            }
        } else if (render_carinfo_button_hit(x, y)) {
            g_carinfo = 1;
            log_line("[page] car info");
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (render_map_hit(x, y) != MAPHIT_NONE && g_map.count > 0 && g_model.have_position) {
            map_step(render_map_hit(x, y) == MAPHIT_ZOOM_IN ? 1 : -1);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (render_clock_hit(x, y)) {
            SetTimer(hwnd, TIMER_HOLD, 2000, NULL);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        KillTimer(hwnd, TIMER_HOLD);
        return 0;
    case WM_SHOWWINDOW:
        if (wparam)
            InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1; /* paint() covers everything */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
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
