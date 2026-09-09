/* See LICENSE file for copyright and license details. */
#ifndef EDWM_DBUSIF_H
#define EDWM_DBUSIF_H

/* edwm's own D-Bus service: org.edwm.WM on /org/edwm/WM.
 *
 * Optional. Build without -DEDWM_DBUS (see config.mk) and every entry point
 * below becomes a no-op, so the WM still builds and runs with no libdbus and
 * loses only this control surface -- theming still works over the _EDWM_CMD
 * root property.
 *
 * dwm.c supplies the behaviour through DbusOps and this file never sees a
 * Client or Monitor; in return dwm.c never sees a DBusMessage. The two halves
 * meet only at the plain types below.
 */

/* Called once per window while answering ListClients. */
typedef void (*DbusClientSink)(void *sink, unsigned long win, const char *title,
                               unsigned int tags, int focused, int urgent,
                               int minimized);

typedef struct {
	/* The same one-line vocabulary the _EDWM_CMD root property accepts, so
	 * both transports behave identically: "reload", "theme <name>",
	 * "tone k=v ...", "quit". */
	void (*command)(const char *cmd);
	const char *(*gettheme)(void);
	void (*listclients)(DbusClientSink sink, void *sinkdata);
	void (*focusclient)(unsigned long win);
	void (*closeclient)(unsigned long win);
	void (*view)(unsigned int tagmask);
	void (*setstatus)(const char *text);
	void (*quit)(void);
} DbusOps;

/* Connect and request the name. Safe to call when no bus exists: it simply
 * reports failure and arms a retry. */
int dbusif_init(const DbusOps *ops);

/* The connection's descriptor, or -1 while disconnected. It changes across a
 * reconnect, so the caller re-registers it when it differs. */
int dbusif_fd(void);

/* Drain and dispatch everything pending. Must drain fully: libdbus buffers
 * messages in user space, so a level-triggered poll can go quiet with several
 * still queued. */
void dbusif_pump(void);

/* Milliseconds until the next reconnect attempt, or -1 when connected or when
 * no retry is pending. Feed this into the main loop's poll timeout. */
int dbusif_timeout(void);

/* Try to reconnect if the backoff has elapsed. Cheap when connected. */
void dbusif_retry(void);

void dbusif_cleanup(void);

#ifdef EDWM_DBUS
#include <dbus/dbus.h>
/* The live connection, or NULL while disconnected. The SNI host shares it
 * rather than opening a second one, so there is one fd and one reconnect
 * path. */
DBusConnection *dbusif_conn(void);
#endif

/* Signals. No-ops while disconnected. */
void dbusif_emit_theme(const char *name);
void dbusif_emit_focus(unsigned long win);
void dbusif_emit_clients(void);
void dbusif_emit_tags(unsigned int selected, unsigned int occupied);

#endif /* EDWM_DBUSIF_H */
