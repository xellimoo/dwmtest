/* See LICENSE file for copyright and license details. */
#ifndef EDWM_SYSTRAY_H
#define EDWM_SYSTRAY_H

/* XEmbed system tray (freedesktop System Tray Protocol).
 *
 * Self-contained: it owns its icon list and touches none of dwm's Client or
 * Monitor state, so the interface below is everything dwm.c needs. Tray icons
 * are deliberately NOT dwm Clients -- the widely copied dwm systray patch
 * reuses Client and consequently leaks tray windows into wintoclient(), the
 * focus stack and _NET_CLIENT_LIST, which is where most of its bugs come from.
 *
 * Modern apps (Electron, Telegram, ...) publish StatusNotifierItem over D-Bus
 * instead of XEmbed and will not appear here; run snixembed to bridge them.
 */

#include <X11/Xlib.h>

/* Create the tray window and take the _NET_SYSTEM_TRAY_Sn selection.
 * `changed` is called whenever the icon list or its size changes, so the bar
 * can be re-laid out and redrawn. Returns 0 if the selection is already owned
 * by another tray, in which case edwm simply runs without one. */
int systray_init(Display *dpy, int screen, Window root, void (*changed)(void));

/* Release the selection and hand the icons back to the root window. */
void systray_cleanup(void);

/* Width in pixels the bar must reserve on the right, 0 when no icon is
 * mapped. */
int systray_width(void);

/* Put the tray at the right-hand end of `barwin`, `h` tall, with its origin at
 * (x, y). Also sets the background the icons composite against. */
void systray_place(Window barwin, int x, int y, int h, unsigned long bg);

/* Icon geometry, from the theme. */
void systray_setmetrics(int iconsize, int spacing);

/* Is `w` the tray window or one of its icons? */
int systray_owns(Window w);

/* Event hooks. Each returns 1 when it consumed the event, so dwm.c can stop
 * processing it. They are cheap no-ops when the tray is not running. */
int systray_clientmessage(XClientMessageEvent *e);
int systray_destroynotify(XDestroyWindowEvent *e);
int systray_reparentnotify(XReparentEvent *e);
int systray_configurerequest(XConfigureRequestEvent *e);
int systray_maprequest(XMapRequestEvent *e);
int systray_propertynotify(XPropertyEvent *e);
int systray_selectionclear(XSelectionClearEvent *e);

/* Old KDE applets never send a dock request; they map an ordinary toplevel
 * carrying _KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR. Called from manage()/scan() to
 * claim those before they become normal windows. Returns 1 if it took `w`. */
int systray_trykdedock(Window w);

#endif /* EDWM_SYSTRAY_H */
