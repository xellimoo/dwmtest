/* A minimal system tray client, for testing edwm's tray.
 *
 * Does exactly what the freedesktop System Tray Protocol asks of an icon:
 * finds the _NET_SYSTEM_TRAY_Sn owner, sets _XEMBED_INFO, sends
 * SYSTEM_TRAY_REQUEST_DOCK, and paints a solid colour so it is obvious on
 * screen. With -t it toggles its XEMBED_MAPPED flag on a timer, which is how
 * real applets hide and show themselves and the case naive trays get wrong.
 *
 *   cc -o traytest traytest.c -lX11
 *   ./traytest '#ff0000'          dock a red icon
 *   ./traytest '#00ff00' -t       dock, then blink via _XEMBED_INFO
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_MAPPED (1 << 0)

int
main(int argc, char **argv)
{
	Display *dpy;
	Window win, tray;
	Atom trayatom, opcode, xembedinfo;
	XEvent ev;
	char name[64];
	const char *color = argc > 1 ? argv[1] : "#ff0000";
	int toggle = (argc > 2 && !strcmp(argv[2], "-t"));
	unsigned long info[2] = { 0, XEMBED_MAPPED };
	XColor c;
	int screen;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "traytest: cannot open display\n");
		return 1;
	}
	screen = DefaultScreen(dpy);

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", screen);
	trayatom = XInternAtom(dpy, name, False);
	opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	xembedinfo = XInternAtom(dpy, "_XEMBED_INFO", False);

	if ((tray = XGetSelectionOwner(dpy, trayatom)) == None) {
		fprintf(stderr, "traytest: no system tray is running on %s\n", name);
		return 1;
	}
	printf("traytest: tray owner is 0x%lx\n", tray);

	XParseColor(dpy, DefaultColormap(dpy, screen), color, &c);
	XAllocColor(dpy, DefaultColormap(dpy, screen), &c);

	win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 16, 16, 0, 0, c.pixel);
	XSelectInput(dpy, win, StructureNotifyMask | ExposureMask);
	XStoreName(dpy, win, "traytest");

	/* Publish before docking: the tray reads this as part of embedding. */
	XChangeProperty(dpy, win, xembedinfo, xembedinfo, 32, PropModeReplace,
	                (unsigned char *)info, 2);

	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = tray;
	ev.xclient.message_type = opcode;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
	ev.xclient.data.l[2] = win;
	XSendEvent(dpy, tray, False, NoEventMask, &ev);
	XSync(dpy, False);
	printf("traytest: sent dock request for 0x%lx\n", win);

	for (;;) {
		if (toggle) {
			sleep(3);
			info[1] ^= XEMBED_MAPPED;
			printf("traytest: XEMBED_MAPPED -> %lu\n", info[1]);
			XChangeProperty(dpy, win, xembedinfo, xembedinfo, 32,
			                PropModeReplace, (unsigned char *)info, 2);
			XSync(dpy, False);
		} else {
			while (XPending(dpy))
				XNextEvent(dpy, &ev);
			usleep(200000);
		}
	}
	return 0;
}
