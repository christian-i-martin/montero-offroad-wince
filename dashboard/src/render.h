/* Draws the dashboard (800 x 480) through a small drawing interface, so the same layout code
 * runs with GDI on the head unit and writes an SVG preview on the build machine.
 * Layout and colours follow the design canvas; text is UTF-8. Portable C. */
#ifndef RENDER_H
#define RENDER_H

#include "model.h"

#define SCREEN_W 800
#define SCREEN_H 480
#define HEADER_H 40
#define NO_FILL 0xFFFFFFFFu

enum { FACE_MEDIUM, FACE_SEMIBOLD, FACE_CONDENSED, FACE_COUNT };
enum {
    FONT_CLOCK, FONT_HEADER, FONT_LABEL, FONT_SMALL, FONT_TREND, FONT_UNIT, FONT_UNIT_BIG, FONT_VALUE,
    FONT_SPEED, FONT_STAT, FONT_HEADING, FONT_DIR, FONT_DIR_SMALL, FONT_ROSE, FONT_MENU, FONT_COUNT
};

typedef struct { int px; int face; } FontSpec;   /* px = em size, as CSS font-size */
extern const FontSpec render_fonts[FONT_COUNT];

typedef struct { int x, y; } DPoint;

typedef struct {
    void *ctx;
    void (*fill_rect)(void *ctx, int x, int y, int w, int h, unsigned rgb);
    void (*round_rect)(void *ctx, int x, int y, int w, int h, int radius, unsigned fill, unsigned border);
    void (*polyline)(void *ctx, const DPoint *p, int n, int width, unsigned rgb);
    void (*polygon)(void *ctx, const DPoint *p, int n, unsigned fill);
    void (*circle)(void *ctx, int cx, int cy, int r, unsigned fill, unsigned border, int border_width);
    int (*text_width)(void *ctx, int font, const char *utf8);
    void (*text)(void *ctx, int x, int baseline, int font, unsigned rgb, const char *utf8);
} Canvas;

void render_dashboard(const Canvas *c, const DashModel *m);

/* Maintenance menu, opened by holding the clock. */
enum { MENU_NONE, MENU_FILES, MENU_CLOSE, MENU_BACK };
void render_menu(const Canvas *c, const DashModel *m, int has_file_manager, const char *version);
int render_menu_hit(int x, int y, int has_file_manager);
/* 1 when (x, y) is on the clock, where a long press opens the menu. */
int render_clock_hit(int x, int y);

#endif
