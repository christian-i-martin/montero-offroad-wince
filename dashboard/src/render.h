/* Draws the dashboard (800 x 480) through a small drawing interface, so the same layout code
 * runs with GDI on the head unit and writes an SVG preview on the build machine.
 *
 * The offline map fills the whole screen; every widget sits on its own semi-transparent panel
 * (tinted, not blurred, to spare the processor): a 40 px header across the top (clock, GPS,
 * headlights, sunset or sunrise), side columns 1/6 of the screen wide (speed and elevation on the
 * left; outside temperature, pressure and battery on the right), the heading at the bottom left
 * and the address at the bottom right, each 2/6 wide. Icons are drawn on a pixel grid for the
 * low-resolution screen; graphs are lines through every sample. Day and night colours follow the
 * sun. A button at the right end of the header opens the car info page (fixed facts about the
 * car: a side bar of categories, up to 9 tiles each), whose Back button returns to the dashboard.
 * The map turns with the car: the direction of travel points up (m->map_bearing), and a compass
 * needle shows north. Tapping the car on the map opens the map page: the map alone, which a finger
 * can drag.
 * Layout and colours follow the design canvas; text is UTF-8. Portable C. */
#ifndef RENDER_H
#define RENDER_H

#include "island.h"
#include "mapview.h"
#include "model.h"

#define SCREEN_W 800
#define SCREEN_H 480
#define HEADER_H 40
#define NO_FILL 0xFFFFFFFFu

enum { FACE_MEDIUM, FACE_SEMIBOLD, FACE_CONDENSED, FACE_COUNT };
enum {
    FONT_CLOCK, FONT_HEADER, FONT_LABEL, FONT_SMALL, FONT_TREND, FONT_UNIT, FONT_VALUE, FONT_TILE,
    FONT_SPEED, FONT_SPEED3, FONT_STAT, FONT_HEADING, FONT_TAPE, FONT_MENU, FONT_ADDR, FONT_COUNT
};

typedef struct { int px; int face; } FontSpec;   /* px = em size, as CSS font-size */
extern const FontSpec render_fonts[FONT_COUNT];

typedef struct { int x, y; } DPoint;

enum { MAPDRAW_NO_FILES = -1, MAPDRAW_OUTSIDE = 0, MAPDRAW_SHOWN = 1 };

typedef struct {
    void *ctx;
    void (*fill_rect)(void *ctx, int x, int y, int w, int h, unsigned rgb);
    void (*round_rect)(void *ctx, int x, int y, int w, int h, int radius, unsigned fill, unsigned border);
    void (*polyline)(void *ctx, const DPoint *p, int n, int width, unsigned rgb);
    void (*polygon)(void *ctx, const DPoint *p, int n, unsigned fill);
    void (*circle)(void *ctx, int cx, int cy, int r, unsigned fill, unsigned border, int border_width);
    int (*text_width)(void *ctx, int font, const char *utf8);
    void (*text)(void *ctx, int x, int baseline, int font, unsigned rgb, const char *utf8);
    /* The vector map (vmap.h) across the box: position (mx, my) (Web Mercator, 0 to 1) drawn at
     * screen point (cx, cy), at `zoom` (any fraction), with `bearing` (degrees) up the screen; the
     * dark map at half brightness at night, the light map by day. Returns MAPDRAW_SHOWN,
     * MAPDRAW_OUTSIDE (the box is then land colour) or MAPDRAW_NO_FILES. NULL when the platform
     * has no map. */
    int (*map)(void *ctx, int x, int y, int w, int h, double mx, double my, double zoom, double bearing, int cx, int cy,
               int night);
    /* Semi-transparent panel: mixes rgb at alpha (0 clear to 256 solid) into a box with corners
     * of the given radius. NULL when the platform cannot: panels are then drawn solid. */
    void (*glass)(void *ctx, int x, int y, int w, int h, int radius, unsigned rgb, int alpha);
} Canvas;

/* Where the car is drawn: the middle of the screen below the header. */
#define CAR_X (SCREEN_W / 2)
#define CAR_Y ((SCREEN_H + HEADER_H) / 2)

void render_dashboard(const Canvas *c, const DashModel *m);

/* The holiday notification over the dashboard (island.h): a frame of the panel with today's
 * holidays (m->holidays), or the round button it collapses into. Drawn after render_dashboard,
 * on its own so the program can redraw only its area while it moves. Each row: the calendar icon,
 * the holiday's name, the long date ("Thursday, November 26, 2026"), and what kind of day it is,
 * with a "DAY OFF" badge before it for a regular holiday or a special non-working day. */
void render_island(const Canvas *c, const DashModel *m, const IslandFrame *f);

/* Car info page: fixed facts about the car. A side bar on the left picks the category; its tiles
 * fill the rest (General: plate, fuel, engine, gearbox, tyres, weight; Size and clearance:
 * clearances, angles, size; Fluids: types, amounts and change intervals). */
enum { CARINFO_GENERAL, CARINFO_SIZE, CARINFO_FLUIDS, CARINFO_CATEGORIES };
void render_carinfo(const Canvas *c, const DashModel *m, int category);
/* The category whose side bar button is at (x, y), or -1. */
int render_carinfo_category_hit(int x, int y);
/* 1 when (x, y) is on the car info button in the dashboard's header. */
int render_carinfo_button_hit(int x, int y);
/* 1 when (x, y) is on the car info page's Back button. */
int render_carinfo_back_hit(int x, int y);

/* Maintenance menu, opened by holding the clock. The time-zone row shows m->utc_offset_min; the
 * program changes its setting when MENU_TZ_MINUS or MENU_TZ_PLUS is hit. */
enum { MENU_NONE, MENU_FILES, MENU_CLOSE, MENU_BACK, MENU_TZ_MINUS, MENU_TZ_PLUS };
void render_menu(const Canvas *c, const DashModel *m, int has_file_manager, const char *version);
int render_menu_hit(int x, int y, int has_file_manager);
/* 1 when (x, y) is on the clock, where a long press opens the menu. */
int render_clock_hit(int x, int y);

/* The map's zoom buttons (shown while the map has a position to show). */
enum { MAPHIT_NONE, MAPHIT_ZOOM_IN, MAPHIT_ZOOM_OUT };
int render_map_hit(int x, int y);
/* 1 when (x, y) is on the car on the dashboard's map: a box of CAR_HIT x CAR_HIT pixels around
 * it, larger than the arrow, so a finger finds it in a moving car. */
#define CAR_HIT 100
int render_car_hit(int x, int y);

/* The map page: the map across the whole screen with the car, Back at the top left, the compass
 * needle at the top right, the zoom buttons at the bottom right, "Centre on car" beside them while
 * the view does not follow the car, the scale bar at the bottom left and the map credit at the
 * bottom middle. While following, the car is in the middle of the screen and the map turns as on
 * the dashboard. */
void render_mappage(const Canvas *c, const DashModel *m, const MapView *v);
enum { MAPPAGE_NONE, MAPPAGE_BACK, MAPPAGE_ZOOM_IN, MAPPAGE_ZOOM_OUT, MAPPAGE_CENTRE };
/* The button at (x, y) on the map page, or MAPPAGE_NONE (the map itself). */
int render_mappage_hit(int x, int y, const MapView *v);

#endif
