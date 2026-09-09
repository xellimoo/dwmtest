/* edwm_dbus.c - session-bus guarantee + org.edwm control service.
 *
 * The dispatch model is the one proven by awesome (dbus.c): keep one shared
 * libdbus connection, watch its unix fd in the WM's select() loop, and pump
 * messages with dbus_connection_read_write(conn, 0) + pop_message. There is
 * no watch/timeout registration and no glib main context: with a single
 * connection the fd is stable for its whole lifetime, which we re-derive on
 * every (re)connect.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dbus/dbus.h>

#include "edwm_dbus.h"
#include "theme.h"

#define EDBUS_NAME       "org.edwm"
#define EDBUS_PATH       "/org/edwm"
#define EDBUS_IFACE      "org.edwm.Theme"
#define EDBUS_RETRY_MS   5000

static DBusConnection *conn;
static int busfd = -1;
static long retryat;             /* monotonic ms when a reconnect is due; 0 = none */

static long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- session bus bootstrap ------------------------------------------------ */

static int
env_from_runtime_dir(void)
{
	const char *xr = getenv("XDG_RUNTIME_DIR");
	char path[256], addr[300];
	struct stat st;

	if (!xr || !*xr || strlen(xr) > sizeof path - 8)
		return -1;
	snprintf(path, sizeof path, "%s/bus", xr);
	if (stat(path, &st) < 0 || !S_ISSOCK(st.st_mode))
		return -1;
	snprintf(addr, sizeof addr, "unix:path=%s", path);
	return setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1) == 0 ? 0 : -1;
}

/* spawn dbus-daemon --session --fork and import the printed address/pid;
 * blocks at most ~3 s waiting for the two output lines */
static int
env_from_own_daemon(int xfd)
{
	int pfd[2], n, nl = 0;
	size_t len = 0;
	char buf[512], addr[512], *eol, pid[32];
	struct pollfd pf;

	if (pipe(pfd) < 0)
		return -1;
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
	if (fork() == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[1]);
		if (xfd >= 0)
			close(xfd);
		signal(SIGCHLD, SIG_DFL);
		execlp("dbus-daemon", "dbus-daemon", "--session", "--fork",
		       "--print-address=1", "--print-pid=1", (char *)NULL);
		_exit(127); /* exec failed: auto-reaped by SA_NOCLDWAIT */
	}
	close(pfd[1]);
	buf[0] = '\0';
	while (len + 1 < sizeof buf && nl < 2) {
		pf.fd = pfd[0];
		pf.events = POLLIN;
		if (poll(&pf, 1, 3000) <= 0)
			break;
		if ((n = read(pfd[0], buf + len, sizeof buf - 1 - len)) <= 0)
			break;
		len += n;
		buf[len] = '\0';
		for (n = 0; n < (int)len; n++)
			if (buf[n] == '\n')
				nl++;
	}
	close(pfd[0]);
	if (!(eol = strchr(buf, '\n')) || eol - buf <= 0)
		return -1;
	*eol = '\0';
	snprintf(addr, sizeof addr, "%s", buf);
	if (setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1) != 0)
		return -1;
	/* second line is the pid (best effort) */
	if ((eol = strchr(eol + 1, '\n')) && eol[1]) {
		char *end = strchr(eol + 1, '\n');
		if (end)
			*end = '\0';
		snprintf(pid, sizeof pid, "%s", eol + 1);
		setenv("DBUS_SESSION_BUS_PID", pid, 1);
	}
	return 0;
}

/* best-effort: push DISPLAY/XAUTHORITY into the activation environment so
 * services the bus activates later (notifications, redshift, ...) see them */
static void
spawn_update_activation_env(int xfd)
{
	if (fork() == 0) {
		if (xfd >= 0)
			close(xfd);
		signal(SIGCHLD, SIG_DFL);
		execlp("dbus-update-activation-environment",
		       "dbus-update-activation-environment", "DISPLAY", "XAUTHORITY",
		       (char *)NULL);
		_exit(127);
	}
}

/* --- connection ------------------------------------------------------------ */

static int
bus_connect(void)
{
	DBusError err;
	int ret;

	dbus_error_init(&err);
	conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!conn) {
		dbus_error_free(&err);
		return -1;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE); /* bus death must not _exit() us */
	busfd = -1;
	if (!dbus_connection_get_unix_fd(conn, &busfd) || busfd < 0) {
		dbus_connection_unref(conn);
		conn = NULL;
		dbus_error_free(&err);
		return -1;
	}
	fcntl(busfd, F_SETFD, FD_CLOEXEC); /* spawn() only closes the X fd */
	ret = dbus_bus_request_name(conn, EDBUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
	&& ret != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
		if (dbus_error_is_set(&err))
			fprintf(stderr, "edwm: cannot own %s: %s\n", EDBUS_NAME, err.message);
		dbus_error_free(&err);
		dbus_connection_unref(conn); /* shared connection: unref only, never close */
		conn = NULL;
		busfd = -1;
		return -1;
	}
	dbus_error_free(&err);
	return 0;
}

void
edwm_dbus_init(int xfd)
{
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");

	if (addr && *addr && bus_connect() == 0)
		goto up;
	if (env_from_runtime_dir() == 0 && bus_connect() == 0)
		goto up;
	if (env_from_own_daemon(xfd) == 0 && bus_connect() == 0)
		goto up;
	retryat = nowms() + EDBUS_RETRY_MS;
	return;
up:
	spawn_update_activation_env(xfd);
}

/* --- org.edwm.Theme method dispatch ---------------------------------------- */

static void
reply_string(DBusConnection *c, DBusMessage *msg, const char *s)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);

	if (reply && dbus_message_append_args(reply, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID))
		dbus_connection_send(c, reply, NULL);
	if (reply)
		dbus_message_unref(reply);
}

static void
reply_bool(DBusConnection *c, DBusMessage *msg, int b)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_bool_t v = b;

	if (reply && dbus_message_append_args(reply, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID))
		dbus_connection_send(c, reply, NULL);
	if (reply)
		dbus_message_unref(reply);
}

static void
reply_theme_list(DBusConnection *c, DBusMessage *msg)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);
	DBusMessageIter iter, sub;
	const char *name;
	int i;

	if (!reply)
		return;
	dbus_message_iter_init_append(reply, &iter);
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
	                                      DBUS_TYPE_STRING_AS_STRING, &sub)) {
		dbus_message_unref(reply);
		return;
	}
	for (i = 0; i < theme_count(); i++) {
		name = theme_name(i);
		dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &name);
	}
	dbus_message_iter_close_container(&iter, &sub);
	dbus_connection_send(c, reply, NULL);
	dbus_message_unref(reply);
}

static void
edwm_dbus_handle(DBusConnection *c, DBusMessage *msg)
{
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	const char *path = dbus_message_get_path(msg);
	const char *name = NULL;
	DBusMessage *err;

	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return;
	if (!path || strcmp(path, EDBUS_PATH))
		return;
	if (iface && strcmp(iface, EDBUS_IFACE))
		return;
	if (!member)
		return;

	if (!strcmp(member, "Next")) {
		theme_action(+1, NULL);
		reply_string(c, msg, theme_current());
	} else if (!strcmp(member, "Prev")) {
		theme_action(-1, NULL);
		reply_string(c, msg, theme_current());
	} else if (!strcmp(member, "SetTheme")) {
		if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
			err = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "SetTheme expects one string");
			if (err) {
				dbus_connection_send(c, err, NULL);
				dbus_message_unref(err);
			}
			return;
		}
		reply_bool(c, msg, theme_action(0, name) == 0);
	} else if (!strcmp(member, "Current")) {
		reply_string(c, msg, theme_current());
	} else if (!strcmp(member, "List")) {
		reply_theme_list(c, msg);
	} else {
		err = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_METHOD, "Unknown member");
		if (err) {
			dbus_connection_send(c, err, NULL);
			dbus_message_unref(err);
		}
	}
}

/* --- public loop-facing API ------------------------------------------------- */

void
edwm_dbus_dispatch(void)
{
	DBusMessage *msg;

	if (!conn)
		return;
	for (;;) {
		if (!dbus_connection_read_write(conn, 0)) /* non-blocking pump */
			goto disconnected;
		if (!(msg = dbus_connection_pop_message(conn)))
			break;
		if (dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected")) {
			dbus_message_unref(msg);
			goto disconnected;
		}
		edwm_dbus_handle(conn, msg);
		dbus_message_unref(msg);
	}
	dbus_connection_flush(conn);
	return;
disconnected:
	dbus_connection_unref(conn);
	conn = NULL;
	busfd = -1;
	retryat = nowms() + EDBUS_RETRY_MS;
}

void
edwm_dbus_tick(void)
{
	if (conn || retryat == 0 || nowms() < retryat)
		return;
	retryat = 0;
	/* retry without spawning another daemon: whatever killed the bus (or an
	 * external dbus-launch) fixes the env, we just try to get back on it */
	if (bus_connect() < 0)
		retryat = nowms() + EDBUS_RETRY_MS;
}

int
edwm_dbus_fd(void)
{
	return conn ? busfd : -1;
}

long
edwm_dbus_timeout(void)
{
	if (conn || retryat == 0)
		return -1;
	return retryat > nowms() ? retryat - nowms() : 0;
}

void
edwm_dbus_announce(const char *themename)
{
	DBusMessage *msg;

	if (!conn || !themename)
		return;
	msg = dbus_message_new_signal(EDBUS_PATH, EDBUS_IFACE, "ThemeChanged");
	if (msg && dbus_message_append_args(msg, DBUS_TYPE_STRING, &themename, DBUS_TYPE_INVALID)) {
		dbus_connection_send(conn, msg, NULL);
		dbus_connection_flush(conn);
	}
	if (msg)
		dbus_message_unref(msg);
}

void
edwm_dbus_cleanup(void)
{
	if (conn)
		dbus_connection_unref(conn);
	conn = NULL;
	busfd = -1;
	retryat = 0;
}
