/* See LICENSE file for copyright and license details. */
#ifndef EDWM_DBUSMENU_H
#define EDWM_DBUSMENU_H

/* com.canonical.dbusmenu client.
 *
 * StatusNotifierItem applets do not draw their own right-click menu; they
 * publish it as a D-Bus object and expect the host to render it. This fetches
 * that description, hands it to menu.c, and reports the chosen item back with
 * an Event call.
 *
 * Supported: labels (with the "_" mnemonic markers stripped), separators,
 * disabled and hidden items, checkmark and radio toggles, per-item PNG icons
 * from icon-data, and nested submenus. Not supported: shortcut display,
 * icon-name lookup for menu items, and live LayoutUpdated refresh while the
 * menu is open -- it is re-fetched on each opening instead.
 *
 * A no-op unless built with -DEDWM_DBUS.
 */

/* drw.h is not self-contained (stock dwm style), so pull in what it needs. */
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "drw.h"

/* Fetch the menu at `service`/`path` and pop it up near (x, y). The fetch is
 * asynchronous, so this returns immediately and the menu appears when the
 * application answers. */
void dbusmenu_open(const char *service, const char *path, Drw *drw, int x, int y);

/* Offered every message dbusif does not own; returns 1 when consumed. */
int dbusmenu_handle_message(void *msg);

/* Drop any pending fetch, e.g. when the bus goes away. */
void dbusmenu_reset(void);

#endif /* EDWM_DBUSMENU_H */
