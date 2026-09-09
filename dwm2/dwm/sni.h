/* See LICENSE file for copyright and license details. */
#ifndef EDWM_SNI_H
#define EDWM_SNI_H

/* StatusNotifierItem host.
 *
 * The XEmbed tray in systray.c only sees applications that dock an X window.
 * Most current applets do not: they publish an item on the session bus and
 * wait for a host to display it. nm-applet is one, which is why it never
 * appeared in the XEmbed tray.
 *
 * This file provides the two services such an applet looks for -- the
 * org.kde.StatusNotifierWatcher, and a StatusNotifierHost registered with it
 * -- then tracks each item and draws its icon into the bar. Unlike XEmbed
 * icons these are not windows we embed; we fetch the pixels and paint them
 * ourselves, so they are ordinary bar content.
 *
 * Scope: icons, status, tooltips-as-titles and the three click actions. Items
 * also advertise a DBusMenu object for their right-click menu; that is a
 * separate protocol and is not implemented, so ContextMenu is forwarded to the
 * application and whether anything appears is up to it.
 *
 * Everything here is a no-op unless built with -DEDWM_DBUS.
 */

/* drw.h is not self-contained (stock dwm style), so pull in what it needs. */
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "drw.h"

/* `changed` is invoked whenever the visible item set or an icon changes, so
 * the bar can be re-laid out and redrawn. */
void sni_init(void (*changed)(void), Drw *drw);

/* Called by dbusif when the bus connection comes and goes. sni_connected()
 * takes the watcher and host names and re-queries every item. */
void sni_connected(void);
void sni_disconnected(void);

/* Offered every message dbusif does not own. Returns 1 when consumed.
 * `msg` is a DBusMessage *, kept opaque so dwm.c need not see libdbus. */
int sni_handle_message(void *msg);

/* Number of items that should currently be shown. */
int sni_count(void);

/* Width these icons need in the bar, 0 when there are none. */
int sni_width(int iconsize, int spacing);

/* Paint the icons into drw's drawable at (x, y), compositing against `bg`
 * since we have no alpha channel to hand off to. */
void sni_draw(Drw *drw, int x, int y, int iconsize, int spacing, unsigned long bg);

/* Which icon is under `x`, given the run starts at `xstart`; -1 for none. */
int sni_indexat(int x, int xstart, int iconsize, int spacing);

/* Activate (1), SecondaryActivate (2) or ContextMenu (3) on an icon. The
 * coordinates are passed to the application so it can place a menu. */
void sni_click(int index, int button, int xroot, int yroot);

/* Icon size to fetch and cache at. */
void sni_setmetrics(int iconsize);

void sni_cleanup(void);

#endif /* EDWM_SNI_H */
