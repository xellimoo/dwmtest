/* See LICENSE file for copyright and license details. */
/* XEmbed system tray. See systray.h for the model and the interface. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "systray.h"

/* System Tray Protocol */
#define SYSTEM_TRAY_REQUEST_DOCK    0

/* XEMBED, version 0 is all we implement (and all anything sends) */
#define XEMBED_VERSION              0
#define XEMBED_MAPPED               (1 << 0)
#define XEMBED_EMBEDDED_NOTIFY      0
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_WINDOW_DEACTIVATE    2
#define XEMBED_FOCUS_IN             4
#define XEMBED_FOCUS_OUT            5
#define XEMBED_REQUEST_FOCUS        3
#define XEMBED_FOCUS_CURRENT        0

#define MAXICONS 32

typedef struct {
	Window win;
	unsigned long version;
	unsigned long flags;   /* only XEMBED_MAPPED is meaningful */
	int hasinfo;           /* did the icon publish _XEMBED_INFO? */
} Icon;

static Display *dpy;
static int screen;
static Window root;
static Window traywin;
static Window parentbar;       /* bar we are reparented into, or None */
static Visual *trayvisual;
static int traydepth;
static Colormap traycmap;
static unsigned long traybg;
static int iconsize = 16, spacing = 2;
static int running;
static Time timestamp;
static void (*onchange)(void);

static Icon icons[MAXICONS];
static int nicons;

static Atom trayatom;          /* _NET_SYSTEM_TRAY_S<screen> */
static Atom opcodeatom;        /* _NET_SYSTEM_TRAY_OPCODE */
static Atom orientatom;
static Atom visualatom;
static Atom xembedatom;
static Atom xembedinfoatom;
static Atom manageratom;
static Atom kdedockatom;

/* ------------------------------------------------------------- utilities */

static Icon *
iconbywin(Window w)
{
	int i;

	for (i = 0; i < nicons; i++)
		if (icons[i].win == w)
			return &icons[i];
	return NULL;
}

static void
iconremove(int i)
{
	memmove(&icons[i], &icons[i + 1], (nicons - i - 1) * sizeof icons[0]);
	nicons--;
}

static int
visibleicons(void)
{
	int i, n = 0;

	for (i = 0; i < nicons; i++)
		if (icons[i].flags & XEMBED_MAPPED)
			n++;
	return n;
}

static void
changed(void)
{
	if (onchange)
		onchange();
}

/* ICCCM says not to use CurrentTime when taking a selection, and there is no
 * way to just ask for the server's clock, so append zero bytes to a property
 * and read the timestamp off the resulting PropertyNotify. */
static Time
servertime(void)
{
	Window w;
	XEvent ev;
	XSetWindowAttributes wa;

	wa.override_redirect = True;
	wa.event_mask = PropertyChangeMask;
	w = XCreateWindow(dpy, root, -100, -100, 1, 1, 0, CopyFromParent,
	                  InputOnly, CopyFromParent,
	                  CWOverrideRedirect | CWEventMask, &wa);
	XChangeProperty(dpy, w, XA_WM_NAME, XA_STRING, 8, PropModeAppend,
	                (unsigned char *)"", 0);
	for (;;) {
		XWindowEvent(dpy, w, PropertyChangeMask, &ev);
		if (ev.type == PropertyNotify) {
			XDestroyWindow(dpy, w);
			return ev.xproperty.time;
		}
	}
}

static void
xembedsend(Window w, long message, long d1, long d2, long d3)
{
	XEvent ev;

	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = xembedatom;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = timestamp;
	ev.xclient.data.l[1] = message;
	ev.xclient.data.l[2] = d1;
	ev.xclient.data.l[3] = d2;
	ev.xclient.data.l[4] = d3;
	XSendEvent(dpy, w, False, NoEventMask, &ev);
}

/* Read _XEMBED_INFO: two CARDINALs, [version, flags]. Icons that omit it
 * (notably Qt's) are treated as wanting to be mapped. */
static void
readxembedinfo(Icon *ic)
{
	Atom type;
	int format;
	unsigned long nitems, after;
	unsigned char *data = NULL;

	ic->hasinfo = 0;
	if (XGetWindowProperty(dpy, ic->win, xembedinfoatom, 0, 2, False,
	                       AnyPropertyType, &type, &format, &nitems, &after,
	                       &data) == Success && data) {
		if (nitems >= 2 && format == 32) {
			ic->version = ((unsigned long *)data)[0];
			ic->flags = ((unsigned long *)data)[1] & XEMBED_MAPPED;
			ic->hasinfo = 1;
		}
		XFree(data);
	}
	if (!ic->hasinfo) {
		ic->version = XEMBED_VERSION;
		ic->flags = XEMBED_MAPPED;
	}
}

/* --------------------------------------------------------------- layout */

int
systray_width(void)
{
	int n = visibleicons();

	if (!running || !n)
		return 0;
	return n * iconsize + (n - 1) * spacing;
}

static void
layouticons(void)
{
	int i, x = 0;

	for (i = 0; i < nicons; i++) {
		if (!(icons[i].flags & XEMBED_MAPPED)) {
			XUnmapWindow(dpy, icons[i].win);
			continue;
		}
		XMoveResizeWindow(dpy, icons[i].win, x, 0, iconsize, iconsize);
		XMapWindow(dpy, icons[i].win);
		/* make the icon repaint against our background */
		XClearArea(dpy, icons[i].win, 0, 0, 0, 0, True);
		x += iconsize + spacing;
	}
}

void
systray_setmetrics(int size, int space)
{
	if (size > 0)
		iconsize = size;
	if (space >= 0)
		spacing = space;
	if (running)
		layouticons();
}

void
systray_place(Window barwin, int x, int y, int h, unsigned long bg)
{
	int w = systray_width();

	if (!running)
		return;
	if (bg != traybg) {
		traybg = bg;
		XSetWindowBackground(dpy, traywin, bg);
		XClearArea(dpy, traywin, 0, 0, 0, 0, True);
	}
	if (!w) {
		XUnmapWindow(dpy, traywin);
		return;
	}
	if (barwin != parentbar) {
		XReparentWindow(dpy, traywin, barwin, x, y);
		parentbar = barwin;
	}
	/* centre the icons vertically in the bar */
	XMoveResizeWindow(dpy, traywin, x, y + (h - iconsize) / 2, w, iconsize);
	XMapRaised(dpy, traywin);
	layouticons();
}

/* ----------------------------------------------------------- docking */

static void
dockicon(Window w)
{
	Icon *ic;
	XWindowAttributes wa;

	if (nicons >= MAXICONS || iconbywin(w))
		return; /* a client may send REQUEST_DOCK more than once */
	if (!XGetWindowAttributes(dpy, w, &wa))
		return;

	ic = &icons[nicons];
	memset(ic, 0, sizeof *ic);
	ic->win = w;

	/* Ask for the info before we start moving the window around. */
	readxembedinfo(ic);

	XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask | EnterWindowMask);
	/* If we die, the server hands the icon back to the root window instead of
	 * destroying it along with the tray. */
	XAddToSaveSet(dpy, w);
	XReparentWindow(dpy, w, traywin, 0, 0);
	XResizeWindow(dpy, w, iconsize, iconsize);

	xembedsend(w, XEMBED_EMBEDDED_NOTIFY, 0, traywin,
	           ic->version < XEMBED_VERSION ? ic->version : XEMBED_VERSION);

	nicons++;
	changed();
}

static void
undock(int i, int alive)
{
	Window w = icons[i].win;

	iconremove(i);
	if (alive) {
		/* The window still exists, so take it back out of the save set. */
		XSelectInput(dpy, w, NoEventMask);
		XChangeSaveSet(dpy, w, SetModeDelete);
	}
	changed();
}

int
systray_owns(Window w)
{
	return running && (w == traywin || iconbywin(w) != NULL);
}

/* ------------------------------------------------------------- events */

int
systray_clientmessage(XClientMessageEvent *e)
{
	if (!running)
		return 0;

	if (e->message_type == opcodeatom) {
		if (e->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK && e->data.l[2])
			dockicon((Window)e->data.l[2]);
		return 1;
	}
	if (e->message_type == xembedatom) {
		if (e->data.l[1] == XEMBED_REQUEST_FOCUS && iconbywin(e->window))
			xembedsend(e->window, XEMBED_FOCUS_IN, XEMBED_FOCUS_CURRENT, 0, 0);
		return iconbywin(e->window) != NULL;
	}
	return 0;
}

int
systray_destroynotify(XDestroyWindowEvent *e)
{
	Icon *ic;

	if (!running || !(ic = iconbywin(e->window)))
		return 0;
	/* Already destroyed: touching the save set now would be a BadWindow. */
	undock((int)(ic - icons), 0);
	return 1;
}

int
systray_reparentnotify(XReparentEvent *e)
{
	Icon *ic;

	if (!running || !(ic = iconbywin(e->window)))
		return 0;
	if (e->parent == traywin)
		return 1; /* our own reparent-in */
	/* Someone took the icon away from us; stop tracking it. */
	undock((int)(ic - icons), 1);
	return 1;
}

int
systray_configurerequest(XConfigureRequestEvent *e)
{
	Icon *ic;
	XConfigureEvent ce;

	if (!running || !(ic = iconbywin(e->window)))
		return 0;

	/* Icons do not get to choose their size -- the tray does. Refusing
	 * silently makes some toolkits spin, so acknowledge with a synthetic
	 * ConfigureNotify carrying the geometry we actually gave them, as a window
	 * manager is required to. */
	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = ic->win;
	ce.window = ic->win;
	ce.x = 0;
	ce.y = 0;
	ce.width = iconsize;
	ce.height = iconsize;
	ce.border_width = 0;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, ic->win, False, StructureNotifyMask, (XEvent *)&ce);
	return 1;
}

int
systray_maprequest(XMapRequestEvent *e)
{
	Icon *ic;

	if (!running || !(ic = iconbywin(e->window)))
		return 0;
	/* Qt does not set _XEMBED_INFO and just maps itself. Neither the XEMBED
	 * nor the tray spec covers that, but without honouring it Qt tray icons
	 * never appear, so treat the request as "mapped". */
	if (!(ic->flags & XEMBED_MAPPED)) {
		ic->flags |= XEMBED_MAPPED;
		changed();
	}
	XMapRaised(dpy, ic->win);
	xembedsend(ic->win, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
	return 1;
}

int
systray_propertynotify(XPropertyEvent *e)
{
	Icon *ic;
	unsigned long was;

	if (!running || !(ic = iconbywin(e->window)))
		return 0;
	if (e->atom != xembedinfoatom)
		return 1;

	was = ic->flags;
	readxembedinfo(ic);
	if ((was ^ ic->flags) & XEMBED_MAPPED) {
		if (ic->flags & XEMBED_MAPPED) {
			XMapRaised(dpy, ic->win);
			xembedsend(ic->win, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
		} else {
			/* An icon that hides itself stays docked -- unmapping is not
			 * undocking, and treating it as such is why some applets vanish
			 * for good in other implementations. */
			XUnmapWindow(dpy, ic->win);
			xembedsend(ic->win, XEMBED_WINDOW_DEACTIVATE, 0, 0, 0);
			xembedsend(ic->win, XEMBED_FOCUS_OUT, 0, 0, 0);
		}
		changed();
	}
	return 1;
}

int
systray_selectionclear(XSelectionClearEvent *e)
{
	if (!running || e->selection != trayatom)
		return 0;
	fprintf(stderr, "edwm: lost the system tray selection to another tray\n");
	systray_cleanup();
	changed();
	return 1;
}

int
systray_trykdedock(Window w)
{
	Atom type;
	int format;
	unsigned long nitems, after;
	unsigned char *data = NULL;
	int isdock = 0;

	if (!running)
		return 0;
	if (XGetWindowProperty(dpy, w, kdedockatom, 0, 1, False, XA_WINDOW,
	                       &type, &format, &nitems, &after, &data) == Success
	    && data) {
		isdock = (nitems == 1);
		XFree(data);
	}
	if (isdock)
		dockicon(w);
	return isdock;
}

/* --------------------------------------------------------------- setup */

int
systray_init(Display *d, int s, Window r, void (*cb)(void))
{
	char name[64];
	XSetWindowAttributes wa;
	XVisualInfo vi;
	unsigned long orient = 0; /* _NET_SYSTEM_TRAY_ORIENTATION_HORZ */
	long visualid;

	dpy = d;
	screen = s;
	root = r;
	onchange = cb;

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", screen);
	trayatom       = XInternAtom(dpy, name, False);
	opcodeatom     = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	orientatom     = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	visualatom     = XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
	xembedatom     = XInternAtom(dpy, "_XEMBED", False);
	xembedinfoatom = XInternAtom(dpy, "_XEMBED_INFO", False);
	manageratom    = XInternAtom(dpy, "MANAGER", False);
	kdedockatom    = XInternAtom(dpy, "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);

	if (XGetSelectionOwner(dpy, trayatom) != None) {
		fprintf(stderr, "edwm: another system tray is already running\n");
		return 0;
	}

	/* A 32-bit visual lets GTK/Qt icons draw with real alpha. Without one they
	 * fall back to compositing against an assumed background, which is what
	 * produces the familiar black boxes behind tray icons. */
	if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vi)) {
		trayvisual = vi.visual;
		traydepth = 32;
		traycmap = XCreateColormap(dpy, root, trayvisual, AllocNone);
	} else {
		trayvisual = DefaultVisual(dpy, screen);
		traydepth = DefaultDepth(dpy, screen);
		traycmap = DefaultColormap(dpy, screen);
	}

	memset(&wa, 0, sizeof wa);
	wa.override_redirect = True;
	wa.background_pixel = 0;
	wa.border_pixel = 0;
	wa.colormap = traycmap;
	/* SubstructureRedirect is what lets us intercept icons trying to resize
	 * themselves; SubstructureNotify tells us when they go away. */
	wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask | ExposureMask;
	traywin = XCreateWindow(dpy, root, -1, -1, 1, 1, 0, traydepth, InputOutput,
	                        trayvisual,
	                        CWOverrideRedirect | CWBackPixel | CWBorderPixel
	                        | CWColormap | CWEventMask, &wa);

	XChangeProperty(dpy, traywin, orientatom, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)&orient, 1);
	visualid = trayvisual->visualid;
	XChangeProperty(dpy, traywin, visualatom, XA_VISUALID, 32,
	                PropModeReplace, (unsigned char *)&visualid, 1);

	timestamp = servertime();
	XSetSelectionOwner(dpy, trayatom, traywin, timestamp);
	if (XGetSelectionOwner(dpy, trayatom) != traywin) {
		fprintf(stderr, "edwm: could not acquire the system tray selection\n");
		XDestroyWindow(dpy, traywin);
		traywin = None;
		return 0;
	}

	/* Tell everyone a tray exists, so applets started before us dock now
	 * rather than only on their next restart. */
	{
		XEvent ev;

		memset(&ev, 0, sizeof ev);
		ev.xclient.type = ClientMessage;
		ev.xclient.window = root;
		ev.xclient.message_type = manageratom;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = timestamp;
		ev.xclient.data.l[1] = trayatom;
		ev.xclient.data.l[2] = traywin;
		XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
	}

	running = 1;
	return 1;
}

void
systray_cleanup(void)
{
	int i;

	if (!running)
		return;
	running = 0;
	XSetSelectionOwner(dpy, trayatom, None, timestamp);
	/* Hand the icons back rather than destroying them with the tray. */
	for (i = nicons - 1; i >= 0; i--) {
		XSelectInput(dpy, icons[i].win, NoEventMask);
		XReparentWindow(dpy, icons[i].win, root, 0, 0);
		XChangeSaveSet(dpy, icons[i].win, SetModeDelete);
	}
	nicons = 0;
	XDestroyWindow(dpy, traywin);
	traywin = None;
	parentbar = None;
}
