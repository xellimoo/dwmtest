/* See LICENSE file for copyright and license details. */
#ifndef EDWM_SESSION_H
#define EDWM_SESSION_H

#include <X11/Xlib.h>

/* Session and D-Bus bootstrap.
 *
 * This is the part that actually fixes the "dwm has D-Bus problems" class of
 * bug. Applications do not break because the window manager cannot speak
 * D-Bus; they break because the session was never described to D-Bus and
 * systemd. Portals then start with no DISPLAY and hang, file choosers time
 * out, notifications go nowhere and keyring prompts never appear.
 *
 * None of this needs libdbus. It is environment plumbing, done once at
 * startup, and every process spawned by the WM inherits the result.
 */

/* Before XOpenDisplay: locate the session bus and declare the desktop
 * identity in our own environment. Deliberately does NOT start a bus -- see
 * the comment in session.c for why launching one here is the classic way to
 * end up with two. */
void session_init(void);

/* After the display is open: publish DISPLAY, XAUTHORITY and the desktop
 * identity into the D-Bus and systemd --user activation environments, so
 * services activated later inherit them. */
void session_exportenv(void);

/* Run ~/.config/edwm/autostart.sh once, if it exists and is executable. The
 * natural home for a compositor, notification daemon, polkit agent and
 * snixembed. */
void session_autostart(void);

#endif /* EDWM_SESSION_H */
