/* See LICENSE file for copyright and license details. */
/* edwm's D-Bus service. See dbusif.h for the split with dwm.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbusif.h"
#include "dbusmenu.h"
#include "sni.h"

#ifndef EDWM_DBUS

/* Built without libdbus: everything is a no-op and the WM keeps working. */
int  dbusif_init(const DbusOps *ops) { (void)ops; return 0; }
int  dbusif_fd(void) { return -1; }
void dbusif_pump(void) {}
int  dbusif_timeout(void) { return -1; }
void dbusif_retry(void) {}
void dbusif_cleanup(void) {}
void dbusif_emit_theme(const char *n) { (void)n; }
void dbusif_emit_focus(unsigned long w) { (void)w; }
void dbusif_emit_clients(void) {}
void dbusif_emit_tags(unsigned int s, unsigned int o) { (void)s; (void)o; }

#else /* EDWM_DBUS */

#include <fcntl.h>
#include <time.h>
#include <dbus/dbus.h>

#define BUS_NAME  "org.edwm.WM"
#define BUS_PATH  "/org/edwm/WM"
#define BUS_IFACE "org.edwm.WM"

#define RETRY_MIN_MS   1000
#define RETRY_MAX_MS  30000

static DBusConnection *conn;
static const DbusOps *ops;
static int fd = -1;
static long retryat;      /* monotonic ms of the next attempt, 0 = none */
static int retrydelay = RETRY_MIN_MS;

static const char *introspect_xml =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
"  </interface>\n"
"  <interface name=\"" BUS_IFACE "\">\n"
"    <method name=\"Command\"><arg name=\"cmd\" type=\"s\" direction=\"in\"/></method>\n"
"    <method name=\"ReloadTheme\"/>\n"
"    <method name=\"SetTheme\"><arg name=\"name\" type=\"s\" direction=\"in\"/></method>\n"
"    <method name=\"GetTheme\"><arg name=\"name\" type=\"s\" direction=\"out\"/></method>\n"
"    <method name=\"SetTone\"><arg name=\"spec\" type=\"s\" direction=\"in\"/></method>\n"
"    <method name=\"ListClients\">\n"
"      <arg name=\"clients\" type=\"a(usubbb)\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"FocusClient\"><arg name=\"window\" type=\"u\" direction=\"in\"/></method>\n"
"    <method name=\"CloseClient\"><arg name=\"window\" type=\"u\" direction=\"in\"/></method>\n"
"    <method name=\"View\"><arg name=\"tags\" type=\"u\" direction=\"in\"/></method>\n"
"    <method name=\"SetStatus\"><arg name=\"text\" type=\"s\" direction=\"in\"/></method>\n"
"    <method name=\"Quit\"/>\n"
"    <signal name=\"ThemeChanged\"><arg name=\"name\" type=\"s\"/></signal>\n"
"    <signal name=\"FocusChanged\"><arg name=\"window\" type=\"u\"/></signal>\n"
"    <signal name=\"ClientListChanged\"/>\n"
"    <signal name=\"TagsChanged\">\n"
"      <arg name=\"selected\" type=\"u\"/><arg name=\"occupied\" type=\"u\"/>\n"
"    </signal>\n"
"  </interface>\n"
"</node>\n";

static long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ----------------------------------------------------------- connection */

static void
teardown(void)
{
	if (!conn)
		return;
	/* dbus_bus_get hands back a connection libdbus owns, so it must only be
	 * unreffed, never closed. */
	dbus_connection_unref(conn);
	conn = NULL;
	fd = -1;
}

static void
armretry(void)
{
	retryat = nowms() + retrydelay;
	retrydelay *= 2;
	if (retrydelay > RETRY_MAX_MS)
		retrydelay = RETRY_MAX_MS;
}

static int
connect_bus(void)
{
	DBusError err;
	int ret;

	dbus_error_init(&err);
	if (!(conn = dbus_bus_get(DBUS_BUS_SESSION, &err)) || dbus_error_is_set(&err)) {
		if (dbus_error_is_set(&err)) {
			fprintf(stderr, "edwm: no session D-Bus: %s\n", err.message);
			dbus_error_free(&err);
		}
		conn = NULL;
		return 0;
	}

	/* Without this libdbus calls _exit() when the bus goes away, which would
	 * take the window manager down with a dbus-daemon restart. */
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	if (!dbus_connection_get_unix_fd(conn, &fd)) {
		fprintf(stderr, "edwm: cannot get the D-Bus file descriptor\n");
		teardown();
		return 0;
	}
	/* Spawned applications must not inherit the WM's bus socket. */
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	ret = dbus_bus_request_name(conn, BUS_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "edwm: cannot take the name %s: %s\n", BUS_NAME, err.message);
		dbus_error_free(&err);
		teardown();
		return 0;
	}
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
	&& ret != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
		fprintf(stderr, "edwm: %s is owned by another process\n", BUS_NAME);
		teardown();
		return 0;
	}

	retrydelay = RETRY_MIN_MS;
	retryat = 0;
	sni_connected();
	return 1;
}

int
dbusif_init(const DbusOps *o)
{
	ops = o;
	if (connect_bus())
		return 1;
	armretry(); /* the bus may simply not be up yet */
	return 0;
}

int
dbusif_fd(void)
{
	return conn ? fd : -1;
}

DBusConnection *
dbusif_conn(void)
{
	return conn;
}

int
dbusif_timeout(void)
{
	long left;

	if (conn || !retryat)
		return -1;
	left = retryat - nowms();
	return left < 0 ? 0 : (int)left;
}

void
dbusif_retry(void)
{
	if (conn || !retryat || nowms() < retryat)
		return;
	if (connect_bus())
		fprintf(stderr, "edwm: reconnected to the session bus\n");
	else
		armretry();
}

/* -------------------------------------------------------------- replies */

static void
send_and_unref(DBusMessage *m)
{
	if (!m)
		return;
	dbus_connection_send(conn, m, NULL);
	dbus_message_unref(m);
}

static void
reply_empty(DBusMessage *msg)
{
	if (!dbus_message_get_no_reply(msg))
		send_and_unref(dbus_message_new_method_return(msg));
}

static void
reply_string(DBusMessage *msg, const char *s)
{
	DBusMessage *r;

	if (dbus_message_get_no_reply(msg))
		return;
	if (!(r = dbus_message_new_method_return(msg)))
		return;
	if (!s)
		s = "";
	dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
	send_and_unref(r);
}

static void
reply_error(DBusMessage *msg, const char *name, const char *text)
{
	/* Always answer. A caller that gets no reply blocks until its own
	 * timeout, which is a miserable way to discover a typo. */
	if (!dbus_message_get_no_reply(msg))
		send_and_unref(dbus_message_new_error(msg, name, text));
}

/* Collects clients into the reply as it is built. */
struct clientsink {
	DBusMessageIter *array;
};

static void
sink_client(void *sinkdata, unsigned long win, const char *title,
            unsigned int tags, int focused, int urgent, int minimized)
{
	struct clientsink *cs = sinkdata;
	DBusMessageIter st;
	dbus_uint32_t w = (dbus_uint32_t)win, t = tags;
	dbus_bool_t f = focused ? TRUE : FALSE;
	dbus_bool_t u = urgent ? TRUE : FALSE;
	dbus_bool_t m = minimized ? TRUE : FALSE;

	if (!title)
		title = "";
	dbus_message_iter_open_container(cs->array, DBUS_TYPE_STRUCT, NULL, &st);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &w);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &title);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32, &t);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &f);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &u);
	dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &m);
	dbus_message_iter_close_container(cs->array, &st);
}

static void
reply_clients(DBusMessage *msg)
{
	DBusMessage *r;
	DBusMessageIter iter, array;
	struct clientsink cs;

	if (dbus_message_get_no_reply(msg))
		return;
	if (!(r = dbus_message_new_method_return(msg)))
		return;
	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(usubbb)", &array);
	cs.array = &array;
	if (ops && ops->listclients)
		ops->listclients(sink_client, &cs);
	dbus_message_iter_close_container(&iter, &array);
	send_and_unref(r);
}

/* ------------------------------------------------------------ dispatch */

static int
getstring(DBusMessage *msg, const char **out)
{
	DBusError err;

	dbus_error_init(&err);
	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, out, DBUS_TYPE_INVALID)) {
		dbus_error_free(&err);
		return 0;
	}
	return 1;
}

static int
getuint(DBusMessage *msg, dbus_uint32_t *out)
{
	DBusError err;

	dbus_error_init(&err);
	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, out, DBUS_TYPE_INVALID)) {
		dbus_error_free(&err);
		return 0;
	}
	return 1;
}

static void
handle(DBusMessage *msg)
{
	const char *member = dbus_message_get_member(msg);
	const char *iface = dbus_message_get_interface(msg);
	const char *s;
	char buf[512];
	dbus_uint32_t u;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return;

	if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")
	&& member && !strcmp(member, "Introspect")) {
		reply_string(msg, introspect_xml);
		return;
	}
	if (!iface || strcmp(iface, BUS_IFACE)) {
		reply_error(msg, DBUS_ERROR_UNKNOWN_INTERFACE, "no such interface");
		return;
	}
	if (!member || !ops) {
		reply_error(msg, DBUS_ERROR_UNKNOWN_METHOD, "no such method");
		return;
	}

	if (!strcmp(member, "Command")) {
		if (!getstring(msg, &s))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a string");
		else {
			ops->command(s);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "ReloadTheme")) {
		ops->command("reload");
		reply_empty(msg);
	} else if (!strcmp(member, "SetTheme")) {
		if (!getstring(msg, &s))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a theme name");
		else {
			snprintf(buf, sizeof buf, "theme %s", s);
			ops->command(buf);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "SetTone")) {
		if (!getstring(msg, &s))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a tone spec");
		else {
			snprintf(buf, sizeof buf, "tone %s", s);
			ops->command(buf);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "GetTheme")) {
		reply_string(msg, ops->gettheme ? ops->gettheme() : "");
	} else if (!strcmp(member, "ListClients")) {
		reply_clients(msg);
	} else if (!strcmp(member, "FocusClient")) {
		if (!getuint(msg, &u))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a window id");
		else {
			ops->focusclient((unsigned long)u);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "CloseClient")) {
		if (!getuint(msg, &u))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a window id");
		else {
			ops->closeclient((unsigned long)u);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "View")) {
		if (!getuint(msg, &u))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a tag mask");
		else {
			ops->view((unsigned int)u);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "SetStatus")) {
		if (!getstring(msg, &s))
			reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected a string");
		else {
			ops->setstatus(s);
			reply_empty(msg);
		}
	} else if (!strcmp(member, "Quit")) {
		reply_empty(msg);
		dbus_connection_flush(conn);
		ops->quit();
	} else {
		reply_error(msg, DBUS_ERROR_UNKNOWN_METHOD, "no such method");
	}
}

void
dbusif_pump(void)
{
	DBusMessage *msg;
	int n = 0;

	if (!conn)
		return;
	for (;;) {
		/* timeout 0: never block the window manager */
		dbus_connection_read_write(conn, 0);
		if (!(msg = dbus_connection_pop_message(conn)))
			break;
		if (dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected")) {
			dbus_message_unref(msg);
			fprintf(stderr, "edwm: session bus went away, will reconnect\n");
			sni_disconnected();
			dbusmenu_reset();
			teardown();
			armretry();
			return;
		}
		if (!sni_handle_message(msg) && !dbusmenu_handle_message(msg))
			handle(msg);
		dbus_message_unref(msg);
		n++;
	}
	if (n)
		dbus_connection_flush(conn);
}

void
dbusif_cleanup(void)
{
	if (!conn)
		return;
	sni_cleanup();
	dbus_bus_release_name(conn, BUS_NAME, NULL);
	dbus_connection_flush(conn);
	teardown();
}

/* -------------------------------------------------------------- signals */

static void
emit(const char *name, int type, const void *arg, int type2, const void *arg2)
{
	DBusMessage *m;

	if (!conn)
		return;
	if (!(m = dbus_message_new_signal(BUS_PATH, BUS_IFACE, name)))
		return;
	if (type != DBUS_TYPE_INVALID)
		dbus_message_append_args(m, type, arg, type2, arg2, DBUS_TYPE_INVALID);
	dbus_connection_send(conn, m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(conn);
}

void
dbusif_emit_theme(const char *name)
{
	if (!name)
		name = "";
	emit("ThemeChanged", DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID, NULL);
}

void
dbusif_emit_focus(unsigned long win)
{
	dbus_uint32_t w = (dbus_uint32_t)win;

	emit("FocusChanged", DBUS_TYPE_UINT32, &w, DBUS_TYPE_INVALID, NULL);
}

void
dbusif_emit_clients(void)
{
	emit("ClientListChanged", DBUS_TYPE_INVALID, NULL, DBUS_TYPE_INVALID, NULL);
}

void
dbusif_emit_tags(unsigned int selected, unsigned int occupied)
{
	dbus_uint32_t s = selected, o = occupied;

	emit("TagsChanged", DBUS_TYPE_UINT32, &s, DBUS_TYPE_UINT32, &o);
}

#endif /* EDWM_DBUS */
