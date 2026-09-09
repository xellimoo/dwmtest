/* systray.h - XEmbed system tray embedded in edwm's bar.
 *
 * The tray container is a child of the primary monitor's bar window; tray
 * icons are real client windows reparented into it, so icon clicks go
 * straight to the owning application. Implementation follows awesome's
 * systray.c + common/xembed.c (manager selection, dock request handling,
 * XEmbed lifecycle) adapted to plain Xlib and dwm's fixed-height bar.
 *
 * Every systray_handle_* hook returns 1 when it consumed the event and the
 * caller should redraw the bar (tray width may have changed). */
#ifndef SYSTRAY_H
#define SYSTRAY_H

#include <X11/Xlib.h>

void systray_init(Display *dpy, Window parentbar, int barheight,
                  unsigned long bgpixel, int scr);
void systray_layout(int barwidth); /* reposition container + icons */
void systray_theme(unsigned long bgpixel);
int  systray_width(void);          /* bar space reserved on the primary monitor */

int  systray_handle_clientmessage(XClientMessageEvent *ev);
int  systray_handle_propertynotify(XPropertyEvent *ev);
int  systray_handle_configurerequest(XConfigureRequestEvent *ev);
int  systray_handle_maprequest(Window w);
int  systray_handle_destroynotify(Window w);
int  systray_handle_reparentnotify(Window w, Window newparent);
int  systray_adopt(Window w); /* dock an orphaned tray icon that mapped as a toplevel */

void systray_cleanup(void);

#endif
