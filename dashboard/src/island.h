/* The holiday notification's movement: a small round button at the top left of the map (the
 * north mark's place at the top right, mirrored) that grows into a panel across the top of the
 * map, shows today's holidays for a few seconds and shrinks back into the button, like the
 * "island" on a phone. Opening, closing, a row added or removed while open, and the button
 * appearing or going away are all animated; every move is worked out from the time it began, so
 * it ends on time however few frames the head unit manages. render_island (render.c) draws a
 * frame; the program asks for frames while island_step says it moves.
 *
 * Portable C, no drawing: the shapes are in screen pixels (800 x 480). */
#ifndef ISLAND_H
#define ISLAND_H

#include "holiday.h"

#define ISLAND_DOT_X 153        /* the button: 10 px right of the left column (render.c) */
#define ISLAND_DOT_Y 48         /* level with the panels' tops */
#define ISLAND_DOT_D 44         /* the north mark's size */
#define ISLAND_PANEL_W 380
#define ISLAND_PANEL_X ((800 - ISLAND_PANEL_W) / 2)
#define ISLAND_PANEL_Y 48
#define ISLAND_PAD 4            /* above the first row and below the last */
#define ISLAND_ROW_H 72
#define ISLAND_RADIUS 22
#define ISLAND_ICON 24          /* the calendar icon, drawn at the size it is designed for */

/* Every frame stays inside this box (the panel's bounce included), so the program can keep a copy
 * of the screen under it and redraw only there while the panel moves. */
#define ISLAND_AREA_X 148
#define ISLAND_AREA_Y 44
#define ISLAND_AREA_W 460
#define ISLAND_AREA_H 240

#define ISLAND_HOLD_MS 6000     /* open this long before closing by itself */

enum { ISLAND_HIDDEN, ISLAND_DOT, ISLAND_OPEN };                       /* where it rests */
enum { ISLAND_STILL, ISLAND_OPENING, ISLAND_CLOSING, ISLAND_RESIZING, ISLAND_APPEARING, ISLAND_LEAVING };
enum { ISLAND_HIT_NONE, ISLAND_HIT_DOT, ISLAND_HIT_PANEL };

typedef struct { double x, y, w, h, r; } IslandBox;

typedef struct {
    int rest;                   /* ISLAND_HIDDEN, ISLAND_DOT or ISLAND_OPEN: where it is or is going */
    int move;                   /* ISLAND_STILL, or the move under way */
    unsigned t0;                /* when the move began (tick count, ms) */
    IslandBox from;             /* the shape then */
    double from_icon_x, from_icon_y;
    int rows;                   /* rows the panel holds */
    int kept_rows;              /* while resizing: rows whose text stays shown */
    int from_alpha[HOLIDAY_ROWS]; /* each row's text when the move began, so a move that interrupts
                                   * another goes on from what was shown */
    unsigned hold_until;        /* when an open panel closes by itself */
} Island;

/* One frame. Nothing is drawn while w or h is below 1. */
typedef struct {
    int x, y, w, h, radius;
    int icon;                   /* 1 when the first row's icon is drawn, at (icon_x, icon_y) */
    int icon_x, icon_y;
    int panel;                  /* 1 when the box is wide enough for the rows */
    int rows;                   /* rows laid out in the panel */
    int alpha[HOLIDAY_ROWS];    /* each row's text: 0 hidden to 256 shown */
} IslandFrame;

void island_init(Island *s);
/* Today's holidays changed to `rows` (0: none). Open, it grows or shrinks to them; without any it
 * goes away; at rest it shows the button. */
void island_set_rows(Island *s, int rows, unsigned now);
/* Grows the panel out of the button (or out of nothing), then closes after ISLAND_HOLD_MS. */
void island_open(Island *s, unsigned now);
/* Shrinks the panel back into the button. */
void island_close(Island *s, unsigned now);
/* Ends any move at once; an open panel becomes the button (the page was left). */
void island_snap(Island *s);
/* Moves on to now; closes an open panel whose time is up. Returns 1 while it moves. */
int island_step(Island *s, unsigned now);
void island_frame(const Island *s, unsigned now, IslandFrame *f);
/* What a finger at (x, y) is on. */
int island_hit(const Island *s, unsigned now, int x, int y);
/* The box a frame covers, its border included. */
void island_bounds(const IslandFrame *f, int *x, int *y, int *w, int *h);

#endif
