/* See LICENSE file for copyright and license details. */
/* Popup menu widget. See menu.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include "menu.h"

#define MAXLEVEL   6     /* nesting depth we are willing to open */
#define PADX       6     /* left/right padding inside an item */
#define PADY       3     /* above and below the label */
#define SEPH       5     /* height of a separator row */
#define GUTTER     20    /* space reserved for the check mark or icon */
#define ARROWW     14    /* space reserved for the submenu arrow */
#define BORDER     1

typedef struct {
	Window win;
	Drw *drw;
	int x, y, w, h;
	MenuItem *items;
	int n;
	int sel;                 /* highlighted row, -1 for none */
	int *rowy;               /* top of each row, n+1 entries */
} Level;

static Level levels[MAXLEVEL];
static int nlevels;
static int active;
static Display *dpy;
static int screen;
static Window root;
static Drw *shared;          /* fonts, visual and colormap come from here */
static Clr *scmnorm, *scmsel, *scmdis;
static unsigned long bordercolor;
static void (*on_activate)(int, void *);
static void (*on_closed)(void *);
static void *userdata;

/* ------------------------------------------------------------- helpers */

void
menu_setstyle(Clr *normal, Clr *selected, Clr *disabled, unsigned long border)
{
	scmnorm = normal;
	scmsel = selected;
	scmdis = disabled;
	bordercolor = border;
}

void
menu_freeitems(MenuItem *items, int n)
{
	int i;

	if (!items)
		return;
	for (i = 0; i < n; i++) {
		img_free(&items[i].icon);
		menu_freeitems(items[i].child, items[i].nchild);
	}
	free(items);
}

static int
rowheight(void)
{
	return shared->fonts->h + 2 * PADY;
}

/* ------------------------------------------------------------ geometry */

static int
measure(Level *lv)
{
	int i, w = 0, tw, h = 0, rh = rowheight();
	int wantarrow = 0, wantgutter = 0;

	for (i = 0; i < lv->n; i++) {
		if (lv->items[i].child)
			wantarrow = 1;
		if (lv->items[i].toggletype != MenuToggleNone || lv->items[i].icon.rgba)
			wantgutter = 1;
	}
	for (i = 0; i < lv->n; i++) {
		if (lv->items[i].separator)
			continue;
		tw = (int)drw_fontset_getwidth(shared, lv->items[i].label);
		if (tw > w)
			w = tw;
	}
	w += 2 * PADX + (wantgutter ? GUTTER : 0) + (wantarrow ? ARROWW : 0);
	if (w < 80)
		w = 80;

	if (!(lv->rowy = calloc((size_t)lv->n + 1, sizeof *lv->rowy)))
		return 0;
	for (i = 0; i < lv->n; i++) {
		lv->rowy[i] = h;
		h += lv->items[i].separator ? SEPH : rh;
	}
	lv->rowy[lv->n] = h;
	lv->w = w;
	lv->h = h ? h : rh;
	return 1;
}

static int
rowat(Level *lv, int y)
{
	int i;

	for (i = 0; i < lv->n; i++)
		if (y >= lv->rowy[i] && y < lv->rowy[i + 1])
			return i;
	return -1;
}

/* --------------------------------------------------------------- draw */

/* A check mark and a submenu arrow, drawn from rectangles so the menu needs no
 * glyphs beyond the configured font. */
static void
drawcheck(Drw *d, int x, int y, int h, int on)
{
	int s = h / 3, cx = x + h / 3, cy = y + h / 2;

	if (!on)
		return;
	drw_rect(d, cx - s / 2, cy, 2, 2, 1, 0);
	drw_rect(d, cx - s / 2 + 1, cy + 1, 2, 2, 1, 0);
	drw_rect(d, cx - s / 2 + 2, cy, 2, 2, 1, 0);
	drw_rect(d, cx - s / 2 + 3, cy - 2, 2, 2, 1, 0);
	drw_rect(d, cx - s / 2 + 4, cy - 4, 2, 2, 1, 0);
	drw_rect(d, cx - s / 2 + 5, cy - 6, 2, 2, 1, 0);
}

static void
drawarrow(Drw *d, int x, int y, int h)
{
	int i, cy = y + h / 2, n = 4;

	for (i = 0; i < n; i++)
		drw_rect(d, x + i, cy - (n - i), 1, 2 * (n - i), 1, 0);
}

static void
drawlevel(Level *lv)
{
	int i, rh = rowheight(), ty, iconsz = shared->fonts->h;
	int gutter = 0, j;
	Clr *scm;

	for (j = 0; j < lv->n; j++)
		if (lv->items[j].toggletype != MenuToggleNone || lv->items[j].icon.rgba) {
			gutter = GUTTER;
			break;
		}

	drw_setscheme(lv->drw, scmnorm);
	drw_rect(lv->drw, 0, 0, lv->w, lv->h, 1, 1);

	for (i = 0; i < lv->n; i++) {
		MenuItem *it = &lv->items[i];

		if (it->separator) {
			drw_setscheme(lv->drw, scmdis);
			drw_rect(lv->drw, PADX, lv->rowy[i] + SEPH / 2, lv->w - 2 * PADX, 1, 1, 0);
			continue;
		}
		scm = !it->enabled ? scmdis : (i == lv->sel ? scmsel : scmnorm);
		drw_setscheme(lv->drw, scm);
		ty = lv->rowy[i];
		drw_rect(lv->drw, 0, ty, lv->w, rh, 1, 1);
		drw_text(lv->drw, PADX + gutter, ty, lv->w - 2 * PADX - gutter, rh, 0,
		         it->label, 0);
		if (it->icon.rgba)
			img_draw(lv->drw, &it->icon, PADX, ty + (rh - iconsz) / 2,
			         scm[ColBg].pixel);
		else if (it->toggletype != MenuToggleNone)
			drawcheck(lv->drw, PADX, ty, rh, it->togglestate);
		if (it->child)
			drawarrow(lv->drw, lv->w - PADX - 6, ty, rh);
	}
	drw_map(lv->drw, lv->win, 0, 0, lv->w, lv->h);
}

/* ------------------------------------------------------- open and close */

static void
closelevel(int i)
{
	if (levels[i].drw) {
		levels[i].drw->fonts = NULL; /* borrowed from the shared Drw */
		drw_free(levels[i].drw);
	}
	if (levels[i].win)
		XDestroyWindow(dpy, levels[i].win);
	free(levels[i].rowy);
	memset(&levels[i], 0, sizeof levels[i]);
}

static void
closeto(int depth)
{
	while (nlevels > depth)
		closelevel(--nlevels);
}

/* `altx` is where to put the menu when it will not fit at `x` -- for a submenu
 * that is the far side of its parent, so it flips instead of overlapping it.
 * Pass altx < 0 to just clamp. */
static int
openlevel(MenuItem *items, int n, int x, int altx, int y)
{
	Level *lv;
	XSetWindowAttributes wa;
	int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);

	if (nlevels >= MAXLEVEL)
		return 0;
	lv = &levels[nlevels];
	memset(lv, 0, sizeof *lv);
	lv->items = items;
	lv->n = n;
	lv->sel = -1;
	if (!measure(lv))
		return 0;

	/* keep it on screen: flip to the other side if we were given one, and
	 * clamp only as a last resort */
	if (x + lv->w > sw) {
		if (altx >= 0 && altx - lv->w >= 0)
			x = altx - lv->w;
		else
			x = sw - lv->w;
	}
	if (x < 0)
		x = 0;
	if (y + lv->h > sh)
		y = sh - lv->h;
	if (y < 0)
		y = 0;
	lv->x = x;
	lv->y = y;

	memset(&wa, 0, sizeof wa);
	wa.override_redirect = True;
	wa.background_pixel = scmnorm[ColBg].pixel;
	wa.border_pixel = bordercolor;
	wa.colormap = shared->cmap;
	wa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask
	              | ExposureMask | LeaveWindowMask;
	lv->win = XCreateWindow(dpy, root, x, y, lv->w, lv->h, BORDER,
	                        shared->depth, InputOutput, shared->visual,
	                        CWOverrideRedirect | CWBackPixel | CWBorderPixel
	                        | CWColormap | CWEventMask, &wa);
	lv->drw = drw_create(dpy, screen, root, lv->w, lv->h,
	                     shared->visual, shared->depth, shared->cmap);
	if (!lv->drw) {
		XDestroyWindow(dpy, lv->win);
		return 0;
	}
	lv->drw->fonts = shared->fonts; /* borrowed; cleared before drw_free */
	XMapRaised(dpy, lv->win);
	nlevels++;
	drawlevel(lv);
	return 1;
}

int
menu_active(void)
{
	return active;
}

void
menu_hide(void)
{
	if (!active)
		return;
	active = 0;
	closeto(0);
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	if (on_closed)
		on_closed(userdata);
}

void
menu_show(Drw *sh, MenuItem *items, int n, int x, int y,
          void (*activate)(int, void *), void (*closed)(void *), void *ud)
{
	if (active)
		menu_hide();
	if (!sh || !items || n <= 0 || !scmnorm)
		return;

	shared = sh;
	dpy = sh->dpy;
	screen = sh->screen;
	root = sh->root;
	on_activate = activate;
	on_closed = closed;
	userdata = ud;
	nlevels = 0;

	if (!openlevel(items, n, x, -1, y))
		return;
	active = 1;

	/* owner_events so our own windows still get their events normally, while
	 * anything outside lands here and dismisses the menu */
	if (XGrabPointer(dpy, levels[0].win, True,
	                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime)
	    != GrabSuccess) {
		menu_hide();
		return;
	}
	XGrabKeyboard(dpy, levels[0].win, True, GrabModeAsync, GrabModeAsync,
	              CurrentTime);
}

/* ------------------------------------------------------------- input */

static int
levelof(Window w)
{
	int i;

	for (i = 0; i < nlevels; i++)
		if (levels[i].win == w)
			return i;
	return -1;
}

static void
select_row(int li, int row)
{
	Level *lv = &levels[li];

	if (lv->sel == row)
		return;
	lv->sel = row;
	/* any deeper menu belonged to the previously selected row */
	closeto(li + 1);
	drawlevel(lv);

	if (row >= 0 && lv->items[row].child && lv->items[row].enabled) {
		/* to the right of this menu, or flipped to its left if there is no
		 * room, rather than sitting on top of it */
		openlevel(lv->items[row].child, lv->items[row].nchild,
		          lv->x + lv->w, lv->x, lv->y + lv->rowy[row]);
	}
}

static void
activate_row(int li, int row)
{
	MenuItem *it;

	if (row < 0 || row >= levels[li].n)
		return;
	it = &levels[li].items[row];
	if (it->separator || !it->enabled)
		return;
	if (it->child) { /* a submenu parent opens rather than activates */
		select_row(li, row);
		return;
	}
	if (on_activate)
		on_activate(it->id, userdata);
	menu_hide();
}

/* Move the highlight by `dir`, skipping separators and disabled rows. */
static void
step(int li, int dir)
{
	Level *lv = &levels[li];
	int i, row = lv->sel;

	for (i = 0; i < lv->n; i++) {
		row = (row + dir + lv->n) % lv->n;
		if (!lv->items[row].separator && lv->items[row].enabled) {
			select_row(li, row);
			return;
		}
	}
}

int
menu_event(XEvent *e)
{
	int li, row;
	KeySym ks;

	if (!active)
		return 0;

	switch (e->type) {
	case Expose:
		if ((li = levelof(e->xexpose.window)) >= 0) {
			if (!e->xexpose.count)
				drawlevel(&levels[li]);
			return 1;
		}
		return 0;

	case MotionNotify:
		if ((li = levelof(e->xmotion.window)) >= 0) {
			row = rowat(&levels[li], e->xmotion.y);
			if (row >= 0 && levels[li].items[row].separator)
				row = -1;
			select_row(li, row);
			return 1;
		}
		return 1; /* swallow motion elsewhere while we hold the grab */

	case ButtonPress:
		if ((li = levelof(e->xbutton.window)) >= 0) {
			row = rowat(&levels[li], e->xbutton.y);
			if (e->xbutton.button == Button1 || e->xbutton.button == Button3)
				activate_row(li, row);
			return 1;
		}
		menu_hide(); /* a click outside dismisses */
		return 1;

	case ButtonRelease:
		return 1;

	case KeyPress:
		ks = XkbKeycodeToKeysym(dpy, (KeyCode)e->xkey.keycode, 0, 0);
		li = nlevels - 1;
		switch (ks) {
		case XK_Escape:
			if (nlevels > 1)
				closeto(nlevels - 1);
			else
				menu_hide();
			break;
		case XK_Up:
			step(li, -1);
			break;
		case XK_Down:
			step(li, 1);
			break;
		case XK_Right:
			if (levels[li].sel >= 0 && levels[li].items[levels[li].sel].child) {
				select_row(li, levels[li].sel);
				if (nlevels > li + 1)
					step(li + 1, 1);
			}
			break;
		case XK_Left:
			if (nlevels > 1)
				closeto(nlevels - 1);
			break;
		case XK_Return:
		case XK_KP_Enter:
			activate_row(li, levels[li].sel);
			break;
		}
		return 1;
	}
	return 0;
}
