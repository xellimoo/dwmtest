/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "session.h"

#define PATHLEN 1024

/* Variables a D-Bus-activated service needs in order to talk to our X session
 * and pick the right portal backend. */
static const char *exported[] = {
	"DISPLAY",
	"XAUTHORITY",
	"XDG_CURRENT_DESKTOP",
	"XDG_SESSION_TYPE",
	"XDG_SESSION_DESKTOP",
	"DESKTOP_SESSION",
	NULL
};

static void
setifunset(const char *name, const char *value)
{
	const char *cur = getenv(name);

	if (!cur || !*cur)
		setenv(name, value, 1);
}

/* Run a command detached, without blocking the WM and without caring about the
 * result beyond a diagnostic. */
static void
runquiet(char *const argv[])
{
	pid_t pid;
	int status;

	if ((pid = fork()) == 0) {
		setsid();
		/* these helpers are chatty on success; only errors are interesting */
		if (!freopen("/dev/null", "w", stdout))
			_exit(127);
		execvp(argv[0], argv);
		_exit(127);
	} else if (pid > 0) {
		/* short-lived helper: reap it so it does not linger, but never block
		 * the WM's startup on it for long */
		waitpid(pid, &status, 0);
	}
}

void
session_init(void)
{
	char path[PATHLEN], addr[PATHLEN + 16];
	struct stat st;

	/* 1. Find the session bus.
	 *
	 * If the address is already set, trust it -- the session manager, or
	 * dbus-run-session, put it there.
	 *
	 * Otherwise look for a bus socket in the runtime directory. $XDG_RUNTIME_DIR
	 * is the portable spelling; /run/user/$UID is where systemd puts it and is
	 * tried as a fallback for logind sessions that did not export the variable.
	 * On systems with neither -- FreeBSD without a session manager, say --
	 * neither exists and we fall through to the warning.
	 *
	 * We deliberately do NOT run dbus-launch here. Starting a bus would create
	 * a *second* one that nothing else in the session knows about, which is
	 * precisely the failure people mean when they say dwm has D-Bus problems:
	 * half the session talks to one bus and half to the other. Say so instead,
	 * and point at the supported way to start edwm. */
	if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		int found = 0;

		if (rt && *rt) {
			snprintf(path, sizeof path, "%s/bus", rt);
			found = stat(path, &st) == 0;
		}
		if (!found) {
			snprintf(path, sizeof path, "/run/user/%lu/bus",
			         (unsigned long)getuid());
			found = stat(path, &st) == 0;
		}
		if (found) {
			snprintf(addr, sizeof addr, "unix:path=%s", path);
			setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
		} else {
			fputs("edwm: warning: no session D-Bus found.\n"
			      "      Portals, notifications and keyring prompts will not work.\n"
			      "      Start edwm through edwm-session, or with:\n"
			      "          dbus-run-session -- dwm\n", stderr);
		}
	}

	/* 2. Declare the desktop identity. Portals key their backend choice off
	 * XDG_CURRENT_DESKTOP, and several toolkits look at the rest. Only set
	 * what is missing, so a session manager that already described the session
	 * keeps the last word. */
	setifunset("XDG_CURRENT_DESKTOP", "edwm");
	setifunset("XDG_SESSION_TYPE", "x11");
	setifunset("XDG_SESSION_DESKTOP", "edwm");
	setifunset("DESKTOP_SESSION", "edwm");
}

void
session_exportenv(void)
{
	char *argv[16];
	struct stat st;
	int i, n = 0;

	if (!getenv("DBUS_SESSION_BUS_ADDRESS"))
		return;

	/* The single most useful thing this file does. Without it, anything D-Bus
	 * activates later (xdg-desktop-portal, gnome-keyring, the notification
	 * daemon) starts with no DISPLAY and either hangs or dies, and the user
	 * sees it as "the file picker never opens". */
	argv[n++] = (char *)"dbus-update-activation-environment";
	/* --systemd additionally pushes the variables into the systemd --user
	 * activation environment. Ask for it only where systemd is actually
	 * running, so the BSDs do not take a pointless failed round trip. */
	if (stat("/run/systemd/system", &st) == 0)
		argv[n++] = (char *)"--systemd";
	for (i = 0; exported[i] && n < 14; i++)
		if (getenv(exported[i]))
			argv[n++] = (char *)exported[i];
	argv[n] = NULL;
	if (n > 2)
		runquiet(argv);
}

void
session_autostart(void)
{
	char path[PATHLEN];
	const char *xdg, *home;
	struct stat st;
	char *argv[3];

	if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg)
		snprintf(path, sizeof path, "%s/edwm/autostart.sh", xdg);
	else if ((home = getenv("HOME")) && *home)
		snprintf(path, sizeof path, "%s/.config/edwm/autostart.sh", home);
	else
		return;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return;

	/* Run it through sh so it needs no execute bit, and detached so a script
	 * that blocks cannot hold up the window manager. */
	argv[0] = (char *)"/bin/sh";
	argv[1] = path;
	argv[2] = NULL;
	if (fork() == 0) {
		setsid();
		execv(argv[0], argv);
		_exit(127);
	}
}
