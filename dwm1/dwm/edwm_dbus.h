/* edwm_dbus.h - session-bus guarantee + org.edwm control service.
 *
 * edwm_dbus_init() makes sure a session bus exists (existing env address,
 * then $XDG_RUNTIME_DIR/bus, then a dbus-daemon we spawn ourselves) and
 * exports it via setenv() so every child edwm spawns inherits it; it also
 * connects to the bus and owns the org.edwm name. The bus fd is served by
 * dwm's select() loop: no threads, no glib (pattern from awesome's dbus.c). */
#ifndef EDWM_DBUS_H
#define EDWM_DBUS_H

/* xfd: the X connection fd, closed in forked children (spawn() does the
 * same); pass -1 when the display is not open yet */
void edwm_dbus_init(int xfd);

/* call every run() iteration: reconnects once the cooldown expires */
void edwm_dbus_tick(void);

/* drain and dispatch pending bus messages (non-blocking) */
void edwm_dbus_dispatch(void);

/* bus fd for select(); -1 while disconnected */
int edwm_dbus_fd(void);

/* ms until the next reconnect attempt is due; -1 when nothing is pending */
long edwm_dbus_timeout(void);

/* emit org.edwm.Theme.ThemeChanged(name); no-op while disconnected */
void edwm_dbus_announce(const char *themename);

void edwm_dbus_cleanup(void);

#endif
