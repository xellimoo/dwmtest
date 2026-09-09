/* docktest.c [seconds] - dock a red 24x24 XEmbed tray icon, then exit */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
	Display *d = XOpenDisplay(NULL);
	Window root, self, owner;
	XEvent e;
	char name[64];
	Atom tray, opcode, info;
	long xembed_info[2] = {0, 1}; /* version 0, XEMBED_MAPPED */
	int scr, secs = argc > 1 ? atoi(argv[1]) : 10;
	if (!d) return 1;
	scr = DefaultScreen(d);
	root = RootWindow(d, scr);
	self = XCreateSimpleWindow(d, root, 0, 0, 24, 24, 0, 0,
	                           WhitePixel(d, scr) /* bright: visible in the bar */);
	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", scr);
	tray = XInternAtom(d, name, False);
	opcode = XInternAtom(d, "_NET_SYSTEM_TRAY_OPCODE", False);
	info = XInternAtom(d, "_XEMBED_INFO", False);
	XChangeProperty(d, self, info, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)xembed_info, 2);
	XFlush(d);
	owner = XGetSelectionOwner(d, tray);
	printf("tray selection owner: 0x%lx\n", owner);
	if (!owner) return 2;
	memset(&e, 0, sizeof e);
	e.xclient.type = ClientMessage;
	e.xclient.window = owner;
	e.xclient.message_type = opcode;
	e.xclient.format = 32;
	e.xclient.data.l[0] = CurrentTime;
	e.xclient.data.l[1] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
	e.xclient.data.l[2] = self;
	XSendEvent(d, owner, False, NoEventMask, &e);
	XFlush(d);
	sleep(secs);
	return 0;
}
