/* Vector map test for the Skypine 6982E head unit (Windows CE 6, ARM): the on-car half of the
 * vector map pilot. It draws the vector map file (\SD\VMap\*.vmap, tools/vmap/) with
 * dashboard/src/vmap.c and measures how fast that is on this processor
 * (docs/vehicle-data-research.md, "Vector map pilot").
 *
 * Version 0.2 draws frames from map pictures (dashboard/src/vtiles.c): the map is drawn once into
 * 256 x 256 pictures kept in memory, and a frame only moves, turns and scales them, with the
 * labels drawn on top from kept letter pictures. Missing pictures are drawn while nothing else
 * happens: those on the screen, a ring around it, then the next zoom in and out. Once a turned
 * view stops moving for 0.3 s, it is drawn again in full for the sharpest picture.
 *
 * Started from Total Commander; it is not the navigation program (its window has its own title),
 * reads nothing from the car, the bus or the GPS, and writes only its log next to the exe.
 *
 * On start it logs the processor, the screen and four ways of copying a frame to the screen
 * (it keeps the fastest), then runs a timing series, then hands over:
 *   drag the map with a finger; buttons: - and + zoom by half a level, Turn L and Turn R turn the
 *   map by 15 degrees, Spin turns it once round, Drive simulates driving heading up (15 s),
 *   Night switches the dark and light map, Labels switches the names, Smooth/Fast picks how
 *   turned pictures are mixed, Pictures/Direct switches between frames from pictures and drawing
 *   every frame in full (as 0.1), Bench runs the series again, Exit closes.
 * Every timing goes into vmaptest-log.txt; the log has no positions.
 *
 * Functions the dashboard does not import are looked up at run time (GetProcAddressW), so a
 * missing one cannot stop the exe from loading. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "vfont.h"
#include "vmap.h"
#include "vtiles.h"

#define VERSION "0.5.1"
#define WINDOW_CLASS L"MonteroVMapTest"
#define WINDOW_TITLE L"Vector map test"
#define CACHE_TILES 128   /* map data tiles kept decoded: the whole Dasmarinas file */
#define PICTURES 480      /* map pictures kept, 128 KB each (61 MB) */
#define TIMER_START 1
#define TIMER_WORK 2      /* draws missing pictures while nothing else happens */
#define TIMER_SHARP 3     /* 0.3 s after the last move of a turned view: draw it in full */
#define SHARP_MS 300

/* ---------------------------------------------------------------- log */

static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_dir[MAX_PATH]; /* folder of this exe, with a trailing backslash */

static void log_line(const char *fmt, ...)
{
    char text[480], line[540];
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
    WriteFile(g_log, line, (DWORD)len, &written, NULL);
    FlushFileBuffers(g_log);
}

static void open_log(void)
{
    WCHAR path[MAX_PATH];
    DWORD size;
    _snwprintf(path, MAX_PATH, L"%svmaptest-log.txt", g_dir);
    path[MAX_PATH - 1] = 0;
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    size = GetFileSize(g_log, NULL);
    if (size != 0xFFFFFFFF && size > 512 * 1024) {
        CloseHandle(g_log);
        g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        SetFilePointer(g_log, 0, NULL, FILE_END);
    }
}

static void narrow(const WCHAR *w, char *out, int size)
{
    int i;
    for (i = 0; i < size - 1 && w[i]; i++)
        out[i] = w[i] < 128 ? (char)w[i] : '?';
    out[i] = 0;
}

/* ---------------------------------------------------------------- functions looked up at run time */

#if defined(__ARM_ARCH) && __ARM_ARCH >= 7
#define BUILT_FOR "ARMv7 (Cortex-A9)"
#else
#define BUILT_FOR "ARMv4T"
#endif

typedef BOOL(WINAPI *qpc_fn)(LARGE_INTEGER *);
typedef void(WINAPI *memory_status_fn)(MEMORYSTATUS *);
typedef void(WINAPI *system_info_fn)(SYSTEM_INFO *);
typedef BOOL(WINAPI *kernel_io_fn)(DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD);
typedef int(WINAPI *device_caps_fn)(HDC, int);
typedef int(WINAPI *dibits_fn)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void *, const BITMAPINFO *,
                               UINT);
typedef HBITMAP(WINAPI *dib_section_fn)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);


static HMODULE g_core;
static qpc_fn g_qpc;
static LARGE_INTEGER g_qpf;

static FARPROC core_fn(const WCHAR *name)
{
    return g_core ? GetProcAddressW(g_core, name) : NULL;
}

static unsigned clock_us(void)
{
    if (g_qpc && g_qpf.QuadPart > 0) {
        LARGE_INTEGER now;
        g_qpc(&now);
        return (unsigned)(now.QuadPart * 1000000 / g_qpf.QuadPart);
    }
    return GetTickCount() * 1000u;
}

static void log_memory(const char *when)
{
    memory_status_fn status = (memory_status_fn)core_fn(L"GlobalMemoryStatus");
    MEMORYSTATUS ms;
    if (status == NULL)
        return;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    status(&ms);
    log_line("[memory] %s: %lu KB of %lu KB free", when, ms.dwAvailPhys / 1024, ms.dwTotalPhys / 1024);
}

/* The processor: GetSystemInfo, and the OEM's description when KernelIoControl answers. */
static void log_processor(void)
{
    typedef struct {
        WORD wVersion;
        WCHAR szProcessorCore[40];
        WORD wCoreRevision;
        WCHAR szProcessorName[40];
        WORD wProcessorRevision;
        WCHAR szCatalogNumber[100];
        WCHAR szVendor[100];
        DWORD dwInstructionSet;
        DWORD dwClockSpeed;
    } ProcessorInfo;
    system_info_fn info = (system_info_fn)core_fn(L"GetSystemInfo");
    kernel_io_fn kio = (kernel_io_fn)core_fn(L"KernelIoControl");
    if (info) {
        SYSTEM_INFO si;
        memset(&si, 0, sizeof si);
        info(&si);
        log_line("[cpu] GetSystemInfo: type %lu, level %u, revision 0x%04x, %lu processor(s), page %lu",
                 si.dwProcessorType, si.wProcessorLevel, si.wProcessorRevision, si.dwNumberOfProcessors, si.dwPageSize);
    }
    if (kio) {
        ProcessorInfo pi;
        DWORD got = 0;
        memset(&pi, 0, sizeof pi);
        /* IOCTL_PROCESSOR_INFORMATION = CTL_CODE(FILE_DEVICE_HAL, 25, METHOD_BUFFERED, FILE_ANY_ACCESS) */
        if (kio(0x01010064, NULL, 0, &pi, sizeof pi, &got)) {
            char core[41], name[41], vendor[101];
            narrow(pi.szProcessorCore, core, sizeof core);
            narrow(pi.szProcessorName, name, sizeof name);
            narrow(pi.szVendor, vendor, sizeof vendor);
            log_line("[cpu] core \"%s\" rev %u, name \"%s\" rev %u, vendor \"%s\", instruction set 0x%lx, clock %lu Hz",
                     core, pi.wCoreRevision, name, pi.wProcessorRevision, vendor, pi.dwInstructionSet, pi.dwClockSpeed);
        } else {
            log_line("[cpu] KernelIoControl(IOCTL_PROCESSOR_INFORMATION) refused (error %lu)", GetLastError());
        }
    }
}

/* ---------------------------------------------------------------- files */

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

static unsigned char *g_ttf;
static VFont g_font;
static VMap g_map;
static VTiles g_tiles;
static HANDLE g_map_file = INVALID_HANDLE_VALUE;
static int g_map_ok, g_tiles_ok;

static int load_font(void)
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
        g_ttf = (unsigned char *)malloc(size);
        if (g_ttf && (!ReadFile(h, g_ttf, size, &got, NULL) || got != size)) {
            free(g_ttf);
            g_ttf = NULL;
        }
    }
    CloseHandle(h);
    return g_ttf != NULL && vf_load(&g_font, g_ttf, size);
}

static int open_map_in(const WCHAR *folder)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    char name[64];
    int ok = 0;
    _snwprintf(path, MAX_PATH, L"%s*.vmap", folder);
    path[MAX_PATH - 1] = 0;
    find = FindFirstFileW(path, &found);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (found.cFileName[0] == L'.')
            continue;
        _snwprintf(path, MAX_PATH, L"%s%s", folder, found.cFileName);
        path[MAX_PATH - 1] = 0;
        g_map_file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_map_file == INVALID_HANDLE_VALUE)
            continue;
        narrow(found.cFileName, name, sizeof name);
        if (vmap_open(&g_map, g_map_file, file_read_at, CACHE_TILES)) {
            log_line("[map] %s: %lu bytes, %d layers, %d data zooms", name, GetFileSize(g_map_file, NULL),
                     g_map.nlayers, g_map.nlevels);
            ok = 1;
            break;
        }
        log_line("[map] %s is not a usable vector map", name);
        CloseHandle(g_map_file);
        g_map_file = INVALID_HANDLE_VALUE;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return ok;
}

/* The VMap folder at the SD card root ("\SD\VMapTest\" -> "\SD\VMap\"), then this exe's folder. */
static int open_map(void)
{
    WCHAR folder[MAX_PATH], *slash;
    wcscpy(folder, g_dir);
    folder[wcslen(folder) - 1] = 0;
    slash = wcsrchr(folder, L'\\');
    if (slash)
        slash[1] = 0;
    else
        folder[0] = 0;
    _snwprintf(folder + wcslen(folder), MAX_PATH - wcslen(folder), L"VMap\\");
    folder[MAX_PATH - 1] = 0;
    return open_map_in(folder) || open_map_in(g_dir);
}

/* ---------------------------------------------------------------- screen and copying */

#define SCREEN_W 800
#define SCREEN_H 480

/* Ways of putting a frame on the screen. The screen has 32 bits per pixel (0.2's log): a 16-bit
 * picture is converted by the display driver while it is copied (about 40 ms), a 32-bit one is
 * not. Screen memory (GETRAWFRAMEBUFFER) is not offered by this unit's driver. */
enum { COPY_UP16, COPY_DOWN16, COPY_DIBITS16, COPY_DOWN32, COPY_DIBITS32, COPY_WAYS };
static const char *const g_copy_name[COPY_WAYS] = {
    "BitBlt from a 16-bit bottom-up DIB", "BitBlt from a 16-bit top-down DIB", "SetDIBitsToDevice 16-bit",
    "BitBlt from a 32-bit top-down DIB", "SetDIBitsToDevice 32-bit"};
static unsigned g_copy_took[COPY_WAYS]; /* microseconds, from the copy test */

typedef struct {
    BITMAPINFOHEADER h;
    DWORD masks[3];
} DibInfo;

static HWND g_main;
static HDC g_dc_up, g_dc_down, g_dc_32;
static VSurface g_up, g_down, g_32, g_surface; /* g_surface: the one in use */
static int g_screen_ok, g_copy = COPY_UP16;
static dibits_fn g_dibits;
static DibInfo g_info_down, g_info_32;

/* A top-down (or bottom-up) DIB section of 16 or 32 bits per pixel, selected into a new DC. */
static HDC make_dib(HDC screen, int top_down, int bits32, VSurface *s, DibInfo *keep)
{
    DibInfo info;
    dib_section_fn create = (dib_section_fn)core_fn(L"CreateDIBSection");
    void *bits = NULL;
    int stride = bits32 ? SCREEN_W : ((SCREEN_W * 2 + 3) & ~3) / 2;
    HDC dc;
    HBITMAP bitmap;
    if (create == NULL)
        return NULL;
    memset(&info, 0, sizeof info);
    info.h.biSize = sizeof info.h;
    info.h.biWidth = SCREEN_W;
    info.h.biHeight = top_down ? -SCREEN_H : SCREEN_H;
    info.h.biPlanes = 1;
    info.h.biBitCount = bits32 ? 32 : 16;
    info.h.biCompression = bits32 ? BI_RGB : BI_BITFIELDS; /* 32-bit BI_RGB: 0x00RRGGBB */
    info.masks[0] = 0xF800;
    info.masks[1] = 0x07E0;
    info.masks[2] = 0x001F;
    if (keep)
        memcpy(keep, &info, sizeof info);
    dc = CreateCompatibleDC(screen);
    bitmap = create(dc, (const BITMAPINFO *)&info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bitmap == NULL || bits == NULL) {
        log_line("[screen] cannot create a %s %d-bit DIB (error %lu)", top_down ? "top-down" : "bottom-up",
                 bits32 ? 32 : 16, GetLastError());
        return NULL;
    }
    SelectObject(dc, bitmap);
    memset(s, 0, sizeof *s);
    s->w = SCREEN_W;
    s->h = SCREEN_H;
    if (bits32) {
        s->px32 = (unsigned *)bits;
        s->stride = stride;
    } else if (top_down) {
        s->px = (unsigned short *)bits;
        s->stride = stride;
    } else {
        s->px = (unsigned short *)bits + (long)(SCREEN_H - 1) * stride; /* the top row is the last in memory */
        s->stride = -stride;
    }
    return dc;
}

static void copy_way(HDC dc, int way)
{
    switch (way) {
    case COPY_UP16:
        BitBlt(dc, 0, 0, SCREEN_W, SCREEN_H, g_dc_up, 0, 0, SRCCOPY);
        break;
    case COPY_DOWN16:
        BitBlt(dc, 0, 0, SCREEN_W, SCREEN_H, g_dc_down, 0, 0, SRCCOPY);
        break;
    case COPY_DIBITS16:
        g_dibits(dc, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, SCREEN_H, g_down.px, (const BITMAPINFO *)&g_info_down,
                 DIB_RGB_COLORS);
        break;
    case COPY_DOWN32:
        BitBlt(dc, 0, 0, SCREEN_W, SCREEN_H, g_dc_32, 0, 0, SRCCOPY);
        break;
    case COPY_DIBITS32:
        g_dibits(dc, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, SCREEN_H, g_32.px32, (const BITMAPINFO *)&g_info_32,
                 DIB_RGB_COLORS);
        break;
    }
}

static void copy_to_screen(HDC dc)
{
    copy_way(dc, g_copy);
}

static void use_surface(int way)
{
    g_copy = way;
    g_surface = way == COPY_UP16 ? g_up : (way == COPY_DOWN32 || way == COPY_DIBITS32) ? g_32 : g_down;
}

/* Checks the screen, then times each copying way that works and keeps the fastest. */
static void choose_copy(void)
{
    device_caps_fn caps = (device_caps_fn)core_fn(L"GetDeviceCaps");
    HDC dc = GetDC(g_main);
    unsigned best_us = 0xFFFFFFFF, took[COPY_WAYS];
    int way, k, x, y, best = COPY_UP16;

    g_dibits = (dibits_fn)core_fn(L"SetDIBitsToDevice");
    if (caps)
        log_line("[screen] %d bits per pixel, %d planes, %d x %d", caps(dc, BITSPIXEL), caps(dc, PLANES),
                 caps(dc, HORZRES), caps(dc, VERTRES));
    /* the same frame in every bitmap */
    for (y = 0; y < SCREEN_H; y++) {
        const unsigned short *src = g_up.px + (long)y * g_up.stride;
        if (g_dc_down)
            memcpy(g_down.px + (long)y * g_down.stride, src, SCREEN_W * 2);
        if (g_dc_32)
            for (x = 0; x < SCREEN_W; x++)
                g_32.px32[(long)y * g_32.stride + x] = VR_565_TO_32(src[x]);
    }
    for (way = 0; way < COPY_WAYS; way++) {
        unsigned start;
        took[way] = 0;
        if (((way == COPY_DOWN16 || way == COPY_DIBITS16) && g_dc_down == NULL) ||
            ((way == COPY_DOWN32 || way == COPY_DIBITS32) && g_dc_32 == NULL) ||
            ((way == COPY_DIBITS16 || way == COPY_DIBITS32) && g_dibits == NULL))
            continue;
        copy_way(dc, way);
        start = clock_us();
        for (k = 0; k < 10; k++)
            copy_way(dc, way);
        took[way] = (clock_us() - start) / 10;
        g_copy_took[way] = took[way];
        if (took[way] < best_us) {
            best_us = took[way];
            best = way;
        }
    }
    ReleaseDC(g_main, dc);
    log_line("[copy] 16-bit: bottom-up BitBlt %u us, top-down BitBlt %u us, SetDIBitsToDevice %u us; 32-bit: "
             "BitBlt %u us, SetDIBitsToDevice %u us (0: not available); using %s",
             took[COPY_UP16], took[COPY_DOWN16], took[COPY_DIBITS16], took[COPY_DOWN32], took[COPY_DIBITS32],
             g_copy_name[best]);
    use_surface(best);
}

/* ---------------------------------------------------------------- the view and the buttons */

static VMapView g_view;
static VMapStats g_stats;
static int g_pictures = 1, g_smooth = 1, g_sharp;
static int g_moving; /* frames while the map moves: labels kept from the last placement */
static unsigned g_map_us, g_labels_us, g_copy_us, g_frame_us;

enum { B_OUT, B_IN, B_LEFT, B_RIGHT, B_SPIN, B_DRIVE, B_NIGHT, B_LABELS, B_SMOOTH, B_MODE, B_BENCH, B_EXIT, BUTTONS };
static const char *const g_button_text[BUTTONS] = {"-",   "+",      "Turn L", "Turn R",   "Spin",  "Drive",
                                                   "Day", "Labels", "Smooth", "Pictures", "Bench", "Exit"};
#define BUTTON_W 60
#define BUTTON_H 56
#define BUTTON_GAP 6
#define BUTTON_X 8
#define BUTTON_Y (SCREEN_H - BUTTON_H - 8)

static int button_at(int x, int y)
{
    int k = (x - BUTTON_X) / (BUTTON_W + BUTTON_GAP);
    if (y < BUTTON_Y || y >= BUTTON_Y + BUTTON_H || x < BUTTON_X || k >= BUTTONS)
        return -1;
    return (x - BUTTON_X) - k * (BUTTON_W + BUTTON_GAP) < BUTTON_W ? k : -1;
}

static int deg(double bearing)
{
    return (int)(bearing + 0.5) % 360;
}

/* The button bar is drawn once (on an opaque strip) and kept; a frame copies it in. */
#define BAR_Y (BUTTON_Y - 4)
#define BAR_H (SCREEN_H - BAR_Y)
static unsigned g_bar32[SCREEN_W * BAR_H];
static unsigned short g_bar16[SCREEN_W * BAR_H];
static int g_bar_key = -1;

static void draw_bar(int light)
{
    unsigned panel = light ? 0xF4F2EA : 0x0B0E11, text = light ? 0x2B3036 : 0xF1F4F6,
             button = light ? 0xE3DFD3 : 0x1E242A;
    int k, y;
    for (y = BAR_Y; y < SCREEN_H; y++) {
        if (g_surface.px32) {
            unsigned *row = g_surface.px32 + (long)y * g_surface.stride;
            for (k = 0; k < SCREEN_W; k++)
                row[k] = panel;
        } else {
            unsigned short *row = g_surface.px + (long)y * g_surface.stride, c = vr_rgb565(panel, 0);
            for (k = 0; k < SCREEN_W; k++)
                row[k] = c;
        }
    }
    for (k = 0; k < BUTTONS; k++) {
        int x = BUTTON_X + k * (BUTTON_W + BUTTON_GAP), w;
        const char *label = g_button_text[k];
        /* toggles show what is on now */
        if (k == B_NIGHT)
            label = light ? "Day" : "Night";
        if (k == B_SMOOTH)
            label = g_smooth ? "Smooth" : "Fast";
        if (k == B_MODE)
            label = g_pictures ? "Pictures" : "Direct";
        if (k == B_LABELS)
            label = g_view.labels ? "Labels" : "Hidden";
        vmap_box(&g_map, &g_surface, x, BUTTON_Y, BUTTON_W, BUTTON_H, 8, button, 256);
        w = vmap_text_width(&g_map, label, 16);
        vmap_text(&g_map, &g_surface, label, x + (BUTTON_W - w) / 2, BUTTON_Y + 34, 16, text, button, 0);
    }
}

static void bar(int light)
{
    int key = light | g_smooth << 1 | g_pictures << 2 | g_view.labels << 3 | (g_surface.px32 != NULL) << 4, y;
    if (key != g_bar_key) {
        draw_bar(light);
        for (y = 0; y < BAR_H; y++) {
            if (g_surface.px32)
                memcpy(g_bar32 + y * SCREEN_W, g_surface.px32 + (long)(BAR_Y + y) * g_surface.stride, SCREEN_W * 4);
            else
                memcpy(g_bar16 + y * SCREEN_W, g_surface.px + (long)(BAR_Y + y) * g_surface.stride, SCREEN_W * 2);
        }
        g_bar_key = key;
        return;
    }
    for (y = 0; y < BAR_H; y++) {
        if (g_surface.px32)
            memcpy(g_surface.px32 + (long)(BAR_Y + y) * g_surface.stride, g_bar32 + y * SCREEN_W, SCREEN_W * 4);
        else
            memcpy(g_surface.px + (long)(BAR_Y + y) * g_surface.stride, g_bar16 + y * SCREEN_W, SCREEN_W * 2);
    }
}

static void draw_overlay(void)
{
    char note[220];
    int light = g_view.style == 1;
    unsigned panel = light ? 0xF4F2EA : 0x0B0E11, text = light ? 0x2B3036 : 0xF1F4F6;
    bar(light);
    _snprintf(note, sizeof note, "%s  zoom %d.%d  %d deg  %s %s  frame %u ms: map %u, labels %u, copy %u  %d pictures, %d missing",
              VERSION, (int)g_view.zoom, (int)(g_view.zoom * 10) % 10, deg(g_view.bearing),
              g_pictures ? (g_sharp ? "full (sharp)" : "pictures") : "direct", g_smooth ? "smooth" : "fast",
              g_frame_us / 1000, g_map_us / 1000, g_labels_us / 1000, g_copy_us / 1000, g_tiles.allocated,
              g_tiles.missing);
    note[sizeof note - 1] = 0;
    vmap_box(&g_map, &g_surface, 8, 8, vmap_text_width(&g_map, note, 15) + 20, 28, 6, panel, 230);
    vmap_text(&g_map, &g_surface, note, 18, 27, 15, text, panel, 0);
    vmap_box(&g_map, &g_surface, g_view.cx - 6, g_view.cy - 6, 12, 12, 6, light ? 0xC2410C : 0xF6A623, 256);
}

/* One frame: the map (from pictures, or in full), the labels, the buttons, then the copy. While
 * the map moves, the labels of the last placement move and turn with it (placed afresh after 20
 * degrees or 120 pixels); a frame at rest places them afresh. */
static unsigned frame(HDC dc)
{
    unsigned start = clock_us(), t;
    int labels = g_moving ? VM_DRAW_LABELS_KEPT : VM_DRAW_LABELS;
    VMapStats shapes;
    memset(&g_stats, 0, sizeof g_stats);
    memset(&shapes, 0, sizeof shapes);
    if (g_pictures && !g_sharp && g_tiles_ok)
        vt_compose(&g_tiles, &g_surface, &g_view, g_smooth);
    else
        vmap_draw(&g_map, &g_surface, &g_view, VM_DRAW_SHAPES, &shapes);
    t = clock_us();
    g_map_us = t - start;
    if (g_view.labels)
        vmap_draw(&g_map, &g_surface, &g_view, labels, &g_stats);
    g_labels_us = clock_us() - t;
    g_stats.features = shapes.features;
    draw_overlay();
    t = clock_us();
    copy_to_screen(dc);
    g_copy_us = clock_us() - t;
    g_frame_us = clock_us() - start;
    return g_frame_us;
}

static void frame_now(void)
{
    HDC dc = GetDC(g_main);
    frame(dc);
    ReleaseDC(g_main, dc);
}

static int turned(void)
{
    return deg(g_view.bearing) != 0;
}

/* The view changed: frames come from pictures again, missing ones get drawn, and a turned view is
 * drawn sharp once it rests. */
static void view_changed(void)
{
    g_sharp = 0;
    if (g_pictures && g_tiles_ok) {
        SetTimer(g_main, TIMER_WORK, 1, NULL);
        if (turned())
            SetTimer(g_main, TIMER_SHARP, SHARP_MS, NULL);
    }
}

/* One step of the background work: a missing picture, nearest first. Returns 0 when all is done. */
static int work_step(void)
{
    VMapView other = g_view; /* the other look (day or night), ready for the switch */
    other.style = !g_view.style;
    other.dim = !other.style;
    if (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0))
        return 2; /* one on the screen: show it */
    if (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 2, 0.0) || vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.5) ||
        vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, -0.5) || vt_fill(&g_tiles, &other, SCREEN_W, SCREEN_H, 0, 0.0))
        return 1;
    return 0;
}

/* Frames while the view moves: missing pictures on the screen get drawn one per frame. */
typedef struct {
    int frames, drawn, kept;
    unsigned sum, slowest, map, labels, copy, drawn_us;
} Motion;

static void log_motion(const char *what, const Motion *mo)
{
    if (mo->frames == 0)
        return;
    log_line("%s zoom %d.%d %s %s: %d frames, %u ms on average (map %u, labels %u, copy %u), slowest %u ms, "
             "%.1f frames a second; labels kept in %d frames; %d pictures drawn in between (%u ms)",
             what, (int)g_view.zoom, (int)(g_view.zoom * 10) % 10, g_pictures ? "pictures" : "direct",
             g_smooth ? "smooth" : "fast", mo->frames, mo->sum / mo->frames / 1000, mo->map / mo->frames / 1000,
             mo->labels / mo->frames / 1000, mo->copy / mo->frames / 1000, mo->slowest / 1000,
             mo->sum ? mo->frames * 1000000.0 / mo->sum : 0.0, mo->kept, mo->drawn, mo->drawn_us / 1000);
}

static void motion_frame(HDC dc, Motion *mo)
{
    unsigned took, before;
    int drawn = g_tiles.drawn;
    g_moving = 1;
    before = clock_us();
    if (g_pictures && g_tiles_ok && g_tiles.missing > 0)
        vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0);
    frame(dc);
    took = clock_us() - before; /* the frame and the picture drawn for it */
    g_moving = 0;
    mo->kept += g_stats.labels_kept;
    if (g_tiles.drawn != drawn) {
        mo->drawn++;
        mo->drawn_us += g_tiles.us_last;
    }
    mo->frames++;
    mo->sum += took;
    mo->map += g_map_us;
    mo->labels += g_labels_us;
    mo->copy += g_copy_us;
    if (took > mo->slowest)
        mo->slowest = took;
}

static void spin(void)
{
    HDC dc = GetDC(g_main);
    Motion mo;
    double start = g_view.bearing;
    int k;
    memset(&mo, 0, sizeof mo);
    g_sharp = 0;
    for (k = 1; k <= 24; k++) {
        g_view.bearing = start + 15 * k;
        while (g_view.bearing >= 360)
            g_view.bearing -= 360;
        motion_frame(dc, &mo);
    }
    ReleaseDC(g_main, dc);
    log_motion("[spin]", &mo);
}

/* 15 seconds of driving at 50 km/h, heading up, turning 6 degrees a second: what the dashboard's
 * map would do while following the GPS (as fast as frames come, no waiting). */
static void drive(unsigned ms)
{
    HDC dc = GetDC(g_main);
    Motion mo;
    double lat = (g_map.south + g_map.north) / 2, s, c, metres_per_unit;
    unsigned start = clock_us(), last = start, now;
    memset(&mo, 0, sizeof mo);
    vmap_sincos(lat, &s, &c);
    metres_per_unit = 40075016.686 * c; /* Mercator units are a whole circumference at this latitude */
    g_sharp = 0;
    while ((now = clock_us()) - start < ms * 1000u) {
        double dt = (now - last) / 1e6, bs, bc, step = 50.0 / 3.6 * dt / metres_per_unit;
        last = now;
        g_view.bearing += 6.0 * dt;
        while (g_view.bearing >= 360)
            g_view.bearing -= 360;
        vmap_sincos(g_view.bearing, &bs, &bc);
        g_view.mx += bs * step;
        g_view.my -= bc * step;
        motion_frame(dc, &mo);
    }
    ReleaseDC(g_main, dc);
    log_motion("[drive]", &mo);
}

static void drag_steps(int dx_total, int steps)
{
    HDC dc = GetDC(g_main);
    Motion mo;
    double scale = 256.0 * vmap_exp2(g_view.zoom), s, c;
    int k;
    memset(&mo, 0, sizeof mo);
    vmap_sincos(g_view.bearing, &s, &c);
    g_sharp = 0;
    for (k = 0; k < steps; k++) {
        double dx = (double)dx_total / steps;
        g_view.mx += c * dx / scale;
        g_view.my += s * dx / scale;
        motion_frame(dc, &mo);
    }
    ReleaseDC(g_main, dc);
    log_motion("[drag test]", &mo);
}

/* Turned frames are composed in chunks so the picture rows they cross stay in the processor's
 * cache; which chunk size is quickest depends on that cache, so try a few and keep the best. */
static void choose_chunks(void)
{
    static const int sizes[][2] = {{0, 0}, {64, 16}, {32, 32}, {128, 8}, {32, 16}};
    unsigned took[5], best = 0xFFFFFFFF;
    int k, n, pick = 1;
    for (k = 0; k < 5; k++) {
        unsigned start;
        g_tiles.chunk_w = sizes[k][0];
        g_tiles.chunk_h = sizes[k][1];
        vt_compose(&g_tiles, &g_surface, &g_view, 1);
        start = clock_us();
        for (n = 0; n < 3; n++)
            vt_compose(&g_tiles, &g_surface, &g_view, 1);
        took[k] = (clock_us() - start) / 3;
        if (took[k] < best) {
            best = took[k];
            pick = k;
        }
    }
    g_tiles.chunk_w = sizes[pick][0];
    g_tiles.chunk_h = sizes[pick][1];
    log_line("[chunks] a smooth turned frame: row by row %u us, 64 x 16 %u us, 32 x 32 %u us, 128 x 8 %u us, "
             "32 x 16 %u us; using %s",
             took[0], took[1], took[2], took[3], took[4], pick == 0 ? "rows" : pick == 1 ? "64 x 16" : pick == 2 ? "32 x 32"
                                                                         : pick == 3 ? "128 x 8"
                                                                                     : "32 x 16");
}

/* ---------------------------------------------------------------- the timing series */

static void bench(void)
{
    static const double zooms[] = {12, 14, 15, 16, 17, 18};
    VMapView saved = g_view;
    int saved_pictures = g_pictures, saved_smooth = g_smooth, k;
    HDC dc = GetDC(g_main);

    KillTimer(g_main, TIMER_WORK);
    KillTimer(g_main, TIMER_SHARP);
    g_sharp = 0;
    log_line("[bench] start (copy: %s)", g_copy_name[g_copy]);
    for (k = 0; k < (int)(sizeof zooms / sizeof zooms[0]); k++) {
        unsigned full, fill_start, fill_us, north, turned_fast, turned_smooth, labels_cold, labels_warm;
        int filled = 0, i, features, letters, markers;
        vmap_middle(&g_map, &g_view.mx, &g_view.my);
        g_view.zoom = zooms[k];
        g_view.bearing = 0;
        g_view.style = 1;
        g_view.dim = 0;
        g_view.labels = 1;

        /* drawn in full every frame, as in 0.1 (the first draw reads the tiles) */
        g_pictures = 0;
        frame(dc);
        frame(dc);
        full = g_map_us + g_labels_us;
        features = g_stats.features;

        /* the pictures for this view, from none */
        g_pictures = 1;
        vt_clear(&g_tiles);
        fill_start = clock_us();
        while (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0))
            filled++;
        fill_us = clock_us() - fill_start;

        /* labels from an empty letter cache, then a kept one */
        if (g_map.glyphs)
            for (i = 0; i < VM_GLYPH_SLOTS; i++) {
                free(g_map.glyphs[i].bits);
                g_map.glyphs[i].bits = NULL;
                g_map.glyphs[i].key = g_map.glyphs[i].key2 = 0;
            }
        frame(dc);
        labels_cold = g_labels_us;
        letters = g_stats.glyphs_new;
        markers = g_stats.markers;
        frame(dc);
        north = g_map_us;
        labels_warm = g_labels_us;

        /* turned 30 degrees: the pictures it needs, then frames */
        g_view.bearing = 30;
        while (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0))
            filled++;
        g_smooth = 0;
        frame(dc);
        turned_fast = g_map_us;
        g_smooth = 1;
        frame(dc);
        turned_smooth = g_map_us;

        log_line("[bench] zoom %d: full draw %u ms (%d features); pictures: %d drawn in %u ms (%u each); frame from "
                 "pictures %u ms north up, turned %u ms fast, %u ms smooth; labels and %d place markers %u ms "
                 "(empty letter cache %u ms, %d letters made); copy %u ms",
                 (int)zooms[k], full / 1000, features, filled, fill_us / 1000,
                 filled ? fill_us / filled / 1000 : 0, north / 1000, turned_fast / 1000, turned_smooth / 1000,
                 markers, labels_warm / 1000, labels_cold / 1000, letters,
                 g_copy_us / 1000);
    }

    /* motion at zoom 16, every picture of the view ready, then into undrawn ground */
    vmap_middle(&g_map, &g_view.mx, &g_view.my);
    g_view.zoom = 16;
    g_view.bearing = 0;
    g_pictures = 1;
    while (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 1, 0.0))
        ;
    g_view.bearing = 30;
    while (vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0))
        ;
    choose_chunks();
    g_view.bearing = 0;
    g_smooth = 1;
    spin();
    g_smooth = 0;
    spin();
    g_view.bearing = 0;
    drag_steps(-256, 16);
    drag_steps(1200, 40);
    vmap_middle(&g_map, &g_view.mx, &g_view.my);
    g_view.zoom = 17;
    g_smooth = 1;
    drive(10000);
    log_line("[bench] done: %d pictures kept, %d drawn since the start (%u ms each on average)", g_tiles.allocated,
             g_tiles.drawn, g_tiles.drawn ? g_tiles.us_drawing / g_tiles.drawn / 1000 : 0);
    log_memory("after the series");
    ReleaseDC(g_main, dc);
    g_view = saved;
    g_pictures = saved_pictures;
    g_smooth = saved_smooth;
    view_changed();
    frame_now();
}

/* ---------------------------------------------------------------- touch */

static int g_pressed, g_dragging, g_press_x, g_press_y;
static double g_start_mx, g_start_my;
static Motion g_drag;

static void drag_to(int x, int y)
{
    double s, c, scale = 256.0 * vmap_exp2(g_view.zoom), dx = x - g_press_x, dy = y - g_press_y;
    vmap_sincos(g_view.bearing, &s, &c);
    /* the finger's move on the screen, turned back to north up, in Mercator units */
    g_view.mx = g_start_mx - (c * dx - s * dy) / scale;
    g_view.my = g_start_my - (s * dx + c * dy) / scale;
}

static void press_button(int b)
{
    double lowest, highest;
    vmap_zoom_range(&g_map, &lowest, &highest);
    switch (b) {
    case B_OUT:
        if (g_view.zoom - 0.5 >= lowest)
            g_view.zoom -= 0.5;
        break;
    case B_IN:
        if (g_view.zoom + 0.5 <= 18.5)
            g_view.zoom += 0.5;
        break;
    case B_LEFT:
        g_view.bearing -= 15;
        if (g_view.bearing < 0)
            g_view.bearing += 360;
        break;
    case B_RIGHT:
        g_view.bearing += 15;
        if (g_view.bearing >= 360)
            g_view.bearing -= 360;
        break;
    case B_SPIN:
        spin();
        break;
    case B_DRIVE:
        drive(15000);
        break;
    case B_NIGHT:
        g_view.style = !g_view.style;
        g_view.dim = !g_view.style;
        break;
    case B_LABELS:
        g_view.labels = !g_view.labels;
        break;
    case B_SMOOTH:
        g_smooth = !g_smooth;
        break;
    case B_MODE:
        g_pictures = !g_pictures;
        break;
    case B_BENCH:
        bench();
        return;
    case B_EXIT:
        DestroyWindow(g_main);
        return;
    }
    view_changed();
    frame_now();
    if (b != B_SPIN && b != B_DRIVE)
        log_line("[view] zoom %d.%d, %d deg, %s, %s %s: frame %u ms (map %u, labels %u, copy %u), %d missing",
                 (int)g_view.zoom, (int)(g_view.zoom * 10) % 10, deg(g_view.bearing), g_view.style ? "light" : "dark",
                 g_pictures ? "pictures" : "direct", g_smooth ? "smooth" : "fast", g_frame_us / 1000, g_map_us / 1000,
                 g_labels_us / 1000, g_copy_us / 1000, g_tiles.missing);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_TIMER:
        if (wparam == TIMER_START) {
            KillTimer(hwnd, TIMER_START);
            if (g_screen_ok && g_map_ok) {
                frame_now();
                choose_copy();
                bench();
            }
        } else if (wparam == TIMER_WORK) {
            int step = work_step();
            if (step == 0)
                KillTimer(hwnd, TIMER_WORK);
            else if (step == 2 && !g_sharp)
                InvalidateRect(hwnd, NULL, FALSE);
        } else if (wparam == TIMER_SHARP) {
            KillTimer(hwnd, TIMER_SHARP);
            if (!g_pressed && turned() && g_pictures) {
                g_sharp = 1;
                frame_now();
                log_line("[sharp] zoom %d.%d, %d deg: drawn in full in %u ms (map %u, labels %u)", (int)g_view.zoom,
                         (int)(g_view.zoom * 10) % 10, deg(g_view.bearing), g_frame_us / 1000, g_map_us / 1000,
                         g_labels_us / 1000);
            }
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam), b = button_at(x, y);
        if (!g_screen_ok || !g_map_ok) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (b >= 0) {
            press_button(b);
            return 0;
        }
        g_pressed = 1;
        g_dragging = 0;
        g_press_x = x;
        g_press_y = y;
        g_start_mx = g_view.mx;
        g_start_my = g_view.my;
        memset(&g_drag, 0, sizeof g_drag);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
        if (!g_pressed)
            return 0;
        if (!g_dragging && x - g_press_x < 10 && g_press_x - x < 10 && y - g_press_y < 10 && g_press_y - y < 10)
            return 0;
        g_dragging = 1;
        drag_to(x, y);
        g_sharp = 0;
        KillTimer(hwnd, TIMER_SHARP);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging)
            log_motion("[drag]", &g_drag);
        if (g_dragging) {
            view_changed();
            InvalidateRect(hwnd, NULL, FALSE); /* at rest: the labels placed afresh */
        }
        g_pressed = g_dragging = 0;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_screen_ok && g_map_ok) {
            if (g_dragging)
                motion_frame(dc, &g_drag);
            else
                frame(dc);
            /* pictures still missing on the screen: draw them before anything else in the
             * background (WM_PAINT comes when no other message waits, so a finger still gets in) */
            if (g_pictures && !g_sharp && g_tiles.missing > 0) {
                if (!g_dragging && vt_fill(&g_tiles, &g_view, SCREEN_W, SCREEN_H, 0, 0.0))
                    InvalidateRect(hwnd, NULL, FALSE);
                else if (g_dragging)
                    InvalidateRect(hwnd, NULL, FALSE);
            }
        } else {
            static const WCHAR note[] = L"Vector map test: no map file or no 16-bit screen. "
                                        L"See vmaptest-log.txt. Tap to close.";
            RECT r = {0, 0, SCREEN_W, SCREEN_H};
            FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            ExtTextOutW(dc, 40, SCREEN_H / 2, 0, NULL, note, (UINT)wcslen(note), NULL);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
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
    WCHAR *slash;
    qpc_fn qpf;

    if (FindWindowW(WINDOW_CLASS, NULL) != NULL)
        return 0;
    g_core = GetModuleHandleW(L"coredll.dll");
    g_qpc = (qpc_fn)core_fn(L"QueryPerformanceCounter");
    qpf = (qpc_fn)core_fn(L"QueryPerformanceFrequency");
    if (qpf == NULL || !qpf(&g_qpf))
        g_qpf.QuadPart = 0;
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    slash = wcsrchr(g_dir, L'\\');
    if (slash)
        slash[1] = 0;
    else
        wcscpy(g_dir, L"\\");
    open_log();
    log_line("=== Montero vector map test %s for %s started (screen %dx%d, timer %s)", VERSION, BUILT_FOR,
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
             g_qpf.QuadPart > 0 ? "high resolution" : "1 ms ticks");
    log_memory("at start");
    log_processor();
    if (!load_font())
        log_line("[font] cannot load fonts\\Barlow-Medium.ttf: no labels");
    g_map_ok = open_map();
    if (!g_map_ok)
        log_line("[map] no vector map: copy dist/vmap/VMap to the SD card root as \\SD\\VMap");
    else if (g_ttf)
        vmap_set_font(&g_map, &g_font);
    g_map.clock = clock_us;
    if (g_map_ok) {
        g_tiles_ok = vt_init(&g_tiles, &g_map, PICTURES);
        if (!g_tiles_ok)
            log_line("[pictures] no memory for the picture table: frames are drawn in full");
    }

    memset(&g_view, 0, sizeof g_view);
    if (g_map_ok)
        vmap_middle(&g_map, &g_view.mx, &g_view.my);
    g_view.zoom = 16;
    g_view.style = 1;
    g_view.labels = 1;
    g_view.cx = SCREEN_W / 2;
    g_view.cy = SCREEN_H / 2 + 20;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = instance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, WS_VISIBLE | WS_POPUP, 0, 0, SCREEN_W, SCREEN_H, NULL, NULL,
                             instance, NULL);
    if (g_main == NULL) {
        log_line("cannot create the window (error %lu)", GetLastError());
        return 1;
    }
    screen = GetDC(g_main);
    g_dc_up = make_dib(screen, 0, 0, &g_up, NULL);
    g_dc_down = make_dib(screen, 1, 0, &g_down, &g_info_down);
    g_dc_32 = make_dib(screen, 1, 1, &g_32, &g_info_32);

    ReleaseDC(g_main, screen);
    g_screen_ok = g_dc_up != NULL;
    if (g_screen_ok)
        use_surface(COPY_UP16);
    SetTimer(g_main, TIMER_START, 500, NULL);
    InvalidateRect(g_main, NULL, FALSE);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    log_line("=== exit (%d pictures drawn)", g_tiles.drawn);
    if (g_tiles_ok)
        vt_free(&g_tiles);
    if (g_map_ok)
        vmap_close(&g_map);
    if (g_map_file != INVALID_HANDLE_VALUE)
        CloseHandle(g_map_file);
    vf_free(&g_font);
    free(g_ttf);
    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);
    return 0;
}
