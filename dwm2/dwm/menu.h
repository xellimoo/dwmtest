/* See LICENSE file for copyright and license details. */
#ifndef EDWM_MENU_H
#define EDWM_MENU_H

/* A popup menu.
 *
 * Deliberately knows nothing about D-Bus: it is handed a tree of items and a
 * callback, and reports which item was chosen. dbusmenu.c builds the tree from
 * an application's com.canonical.dbusmenu object, but anything else could
 * build one too.
 *
 * While a menu is up it holds the pointer and keyboard, so the window manager
 * offers it every button, motion, key and expose event first (menu_event).
 */

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "img.h"

enum { MenuToggleNone, MenuToggleCheck, MenuToggleRadio };

typedef struct MenuItem {
	int id;                  /* opaque to us; handed back on activation */
	char label[192];
	int separator;
	int enabled;
	int toggletype;          /* MenuToggle* */
	int togglestate;         /* 1 = on, -1 = indeterminate */
	Img icon;                /* optional, already square */
	struct MenuItem *child;  /* submenu, or NULL */
	int nchild;
} MenuItem;

/* Free a tree built by the caller (items, their icons and their children). */
void menu_freeitems(MenuItem *items, int n);

/* Colours and metrics. `scm` is indexed [normal|selected|disabled][fg|bg] and
 * borrowed, so it must outlive the menu; the caller re-supplies it whenever
 * the theme changes. */
void menu_setstyle(Clr *normal, Clr *selected, Clr *disabled, unsigned long border);

/* Pop up `items` with its top-left near (x, y), nudged on screen. `activate`
 * is called with the chosen item's id, then the menu closes. `closed` is
 * called when it goes away for any reason. Both may be NULL. */
void menu_show(Drw *shared, MenuItem *items, int n, int x, int y,
               void (*activate)(int id, void *ud),
               void (*closed)(void *ud), void *ud);

void menu_hide(void);
int  menu_active(void);

/* Offer an event to the menu. Returns 1 when the menu consumed it. */
int menu_event(XEvent *e);

#endif /* EDWM_MENU_H */
