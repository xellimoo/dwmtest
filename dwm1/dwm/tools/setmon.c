/* setmon.c <name> <WxH+X+Y> - define a RandR monitor region directly,
 * bypassing xrandr(1) (which refuses when the output has no gamma table) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

typedef struct {
	Atom name;
	Bool primary;
	Bool automatic;
	int noutput;
	int x, y, width, height;
	int mwidth, mheight;
} XRRMonitorInfo;

extern void XRRSetMonitor(Display *, Window, int, XRRMonitorInfo *);

int main(int argc, char **argv)
{
	Display *d;
	XRRMonitorInfo mi;
	char *s;
	int v[4] = {0, 0, 0, 0}, i = 0;

	if (argc != 3 || !(d = XOpenDisplay(NULL)))
		return 1;
	for (s = argv[2]; *s && i < 4; ) {
		if (*s >= '0' && *s <= '9') {
			v[i] = strtol(s, &s, 10);
			i++;
		} else
			s++;
	}
	if (i < 4)
		return 2;
	memset(&mi, 0, sizeof mi);
	mi.name = XInternAtom(d, argv[1], False);
	mi.primary = 1;
	mi.noutput = 0;
	mi.x = v[2]; mi.y = 0;
	mi.width = v[0]; mi.height = v[1];
	mi.mwidth = v[0] * 254 / 96; mi.mheight = v[1] * 254 / 96;
	XRRSetMonitor(d, RootWindow(d, DefaultScreen(d)), 1, &mi);
	XFlush(d);
	printf("monitor %s %dx%d+%d+%d\n", argv[1], v[0], v[1], v[2], v[3]);
	return 0;
}
