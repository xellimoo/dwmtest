/* systray.c - XEmbed system tray embedded in edwm's bar.
 *
 * Docking protocol (see the XEmbed and system tray specifications):
 * an app sends _NET_SYSTEM_TRAY_OPCODE/SYSTEM_TRAY_REQUEST_DOCK to the
 * manager window owning the _NET_SYSTEM_TRAY_S<screen> selection; we
 * reparent the icon into our tray container, acknowledge with
 * XEMBED_EMBEDDED_NOTIFY, and take over the icon's window management
 * (deny its ConfigureRequests with a synthetic ConfigureNotify, honor the
 * _XEMBED_INFO mapped flag). Icons sit in the save set, so they are
 * reparented back to root if edwm dies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "systray.h"
#include "util.h"

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_VERSION           0
#define XEMBED_MAPPED            (1L << 0)
#define TRAYGAP                  2 /* px of left margin kept for every icon */

/* XEmbed message opcodes */
#define XEMBED_EMBEDDED_NOTIFY   0
#define XEMBED_WINDOW_ACTIVATE   1
#define XEMBED_WINDOW_DEACTIVATE 2
#define XEMBED_REQUEST_FOCUS     3
#define XEMBED_FOCUS_IN          4
#define XEMBED_FOCUS_OUT         5
#define XEMBED_FOCUS_CURRENT     0

typedef struct TrayIcon {
	Window win;
	unsigned long version;
	unsigned long flags;             /* XEMBED_MAPPED when the icon wants to show */
	struct TrayIcon *next;
} TrayIcon;

static Display *dpy;
static Window traywin;              /* container, child of the primary bar */
static int traybh, scr, barw;       /* bar height, screen number, bar width */
static TrayIcon *icons;
static int trayw;
static int statw;                   /* status width from the last layout */
static int trayx = -1, traycurw, lastn = -1; /* cache: skip no-op X calls */
static Atom net_system_tray, net_system_tray_opcode, xembed, xembed_info, manager;

static TrayIcon *
iconfind(Window w)
{
	TrayIcon *ic;

	for (ic = icons; ic && ic->win != w; ic = ic->next);
	return ic;
}

static void
xembed_send(Window w, long opcode, long detail, long data1, long data2)
{
	XEvent e;

	memset(&e, 0, sizeof e);
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = xembed;
	e.xclient.format = 32;
	e.xclient.data.l[0] = CurrentTime;
	e.xclient.data.l[1] = opcode;
	e.xclient.data.l[2] = detail;
	e.xclient.data.l[3] = data1;
	e.xclient.data.l[4] = data2;
	XSendEvent(dpy, w, False, NoEventMask, &e);
}

/* read _XEMBED_INFO (2 cardinals: version, flags); missing property means
 * version 0 with the mapped flag set (apps that never wrote it) */
static int
read_xembed_info(Window w, unsigned long *version, unsigned long *flags)
{
	unsigned char *data = NULL;
	Atom type;
	int format, ret = 0;
	unsigned long n, left;

	*version = XEMBED_VERSION;
	*flags = XEMBED_MAPPED;
	if (XGetWindowProperty(dpy, w, xembed_info, 0L, 2L, False, XA_CARDINAL,
	                       &type, &format, &n, &left, &data) == Success && data) {
		if (n >= 2) {
			unsigned long *v = (unsigned long *)data;
			*version = v[0];
			*flags = v[1] & XEMBED_MAPPED;
			ret = 1;
		}
		XFree(data);
	}
	return ret;
}

static int
dock(Window w)
{
	TrayIcon *ic;
	XWindowAttributes wa;
	unsigned long version, flags;

	if (!w || iconfind(w))
		return 0;
	if (!XGetWindowAttributes(dpy, w, &wa)) /* gone between message and now */
		return 0;
	ic = ecalloc(1, sizeof(TrayIcon));
	ic->win = w;
	XSelectInput(dpy, w, StructureNotifyMask|PropertyChangeMask);
	XAddToSaveSet(dpy, w); /* icons survive an edwm crash */
	XReparentWindow(dpy, w, traywin, 0, 0);
	read_xembed_info(w, &version, &flags);
	ic->version = version;
	ic->flags = flags;
	xembed_send(w, XEMBED_EMBEDDED_NOTIFY, 0, traywin, MIN(XEMBED_VERSION, version));
	ic->next = icons;
	icons = ic;
	systray_layout(barw, statw);
	return 1;
}

/* Adopt a toplevel that looks like an orphaned tray icon: an app that found
 * no tray manager when it created its icon (login order races, Wine's first
 * dock attempt failing) may fall back to mapping it as a plain window.
 * Real toplevels never carry _XEMBED_INFO, so its presence is a safe tell;
 * pull such windows into the tray instead of managing them as clients. */
int
systray_adopt(Window w)
{
	Atom type;
	int format, ret;
	unsigned long n, left;
	unsigned char *data = NULL;

	if (!w || iconfind(w))
		return 0;
	if (XGetWindowProperty(dpy, w, xembed_info, 0L, 2L, False, XA_CARDINAL,
	                       &type, &format, &n, &left, &data) != Success || !data || n < 2) {
		if (data)
			XFree(data);
		return 0;
	}
	XFree(data);
	ret = dock(w);
	return ret;
}

static int
removeicon(Window w, int reparented)
{
	TrayIcon **tp, *ic;

	for (tp = &icons; *tp && (*tp)->win != w; tp = &(*tp)->next);
	if (!*tp)
		return 0;
	ic = *tp;
	*tp = ic->next;
	if (reparented) /* moved out of the tray: undo the save-set entry */
		XRemoveFromSaveSet(dpy, w);
	free(ic);
	systray_layout(barw, statw);
	return 1;
}

/* --- public API ------------------------------------------------------------- */

void
systray_init(Display *display, Window parentbar, int barheight,
             unsigned long bgpixel, int screen)
{
	XSetWindowAttributes wa;
	XClassHint ch = {"edwm-tray", "edwm"};
	char atomname[32];
	XEvent e;

	dpy = display;
	traybh = barheight;
	scr = screen;
	barw = 1;
	snprintf(atomname, sizeof atomname, "_NET_SYSTEM_TRAY_S%d", scr);
	net_system_tray = XInternAtom(dpy, atomname, False);
	net_system_tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	xembed = XInternAtom(dpy, "_XEMBED", False);
	xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
	manager = XInternAtom(dpy, "MANAGER", False);

	/* container for the icons: SubstructureRedirect routes the icons' map
	 * and configure requests to us; ButtonPress swallows clicks on padding
	 * so they are not misread as bar clicks */
	traywin = XCreateSimpleWindow(dpy, parentbar, 0, 0, 1, traybh, 0, 0, bgpixel);
	wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
	                |ButtonPressMask|ExposureMask;
	wa.override_redirect = True;
	XChangeWindowAttributes(dpy, traywin, CWEventMask|CWOverrideRedirect, &wa);
	XSetClassHint(dpy, traywin, &ch);
	XSetSelectionOwner(dpy, net_system_tray, traywin, CurrentTime);
	/* broadcast the new manager, like awesome's systray.c does */
	memset(&e, 0, sizeof e);
	e.xclient.type = ClientMessage;
	e.xclient.window = RootWindow(dpy, scr);
	e.xclient.message_type = manager;
	e.xclient.format = 32;
	e.xclient.data.l[0] = CurrentTime;
	e.xclient.data.l[1] = net_system_tray;
	e.xclient.data.l[2] = traywin;
	XSendEvent(dpy, RootWindow(dpy, scr), False, 0xFFFFFF, &e);
}

void
systray_layout(int barwidth, int statuswidth)
{
	TrayIcon *ic;
	int n = 0, ix = 0, x;

	barw = barwidth;
	statw = statuswidth;
	for (ic = icons; ic; ic = ic->next)
		if (ic->flags & XEMBED_MAPPED)
			n++;
	trayw = n * (traybh + TRAYGAP); /* every icon slot carries its margin */
	if (!n) {
		XUnmapWindow(dpy, traywin);
		traycurw = 0; /* force a reposition when an icon returns */
		lastn = 0;
		return;
	}
	/* directly left of the status text; never off-screen */
	x = barw - statw - trayw;
	if (x < 0)
		x = 0;
	/* the status script changes the text width all the time: move only
	 * when something actually changed, so recurring layout calls from
	 * updatestatus() cost nothing and never make icons redraw */
	if (x != trayx || trayw != traycurw) {
		XMoveResizeWindow(dpy, traywin, x, 0, trayw, traybh);
		trayx = x;
		traycurw = trayw;
	}
	if (n != lastn) {
		for (ic = icons; ic; ic = ic->next) {
			if (!(ic->flags & XEMBED_MAPPED)) {
				XUnmapWindow(dpy, ic->win);
				continue;
			}
			XMoveResizeWindow(dpy, ic->win, ix + TRAYGAP, 0, traybh, traybh);
			XMapWindow(dpy, ic->win);
			ix += traybh + TRAYGAP;
		}
		XMapRaised(dpy, traywin);
		lastn = n;
	}
}

void
systray_theme(unsigned long bgpixel)
{
	if (!traywin)
		return;
	XSetWindowBackground(dpy, traywin, bgpixel);
	XClearWindow(dpy, traywin); /* icons own their pixels, only padding retints */
}

int
systray_width(void)
{
	return trayw;
}

int
systray_handle_clientmessage(XClientMessageEvent *ev)
{
	if (ev->format != 32)
		return 0;
	if (ev->message_type == net_system_tray_opcode
	&& ev->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK)
		return dock((Window)ev->data.l[2]);
	if (ev->message_type == xembed
	&& ev->data.l[1] == XEMBED_REQUEST_FOCUS && iconfind(ev->window)) {
		xembed_send(ev->window, XEMBED_FOCUS_IN, XEMBED_FOCUS_CURRENT, 0, 0);
		return 1;
	}
	return 0;
}

int
systray_handle_propertynotify(XPropertyEvent *ev)
{
	TrayIcon *ic;
	unsigned long version, flags;

	if (ev->atom != xembed_info || !(ic = iconfind(ev->window)))
		return 0;
	if (!read_xembed_info(ic->win, &version, &flags))
		return 0;
	if ((flags & XEMBED_MAPPED) && !(ic->flags & XEMBED_MAPPED)) {
		XMapWindow(dpy, ic->win);
		xembed_send(ic->win, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
	} else if (!(flags & XEMBED_MAPPED) && (ic->flags & XEMBED_MAPPED)) {
		XUnmapWindow(dpy, ic->win);
		xembed_send(ic->win, XEMBED_WINDOW_DEACTIVATE, 0, 0, 0);
		xembed_send(ic->win, XEMBED_FOCUS_OUT, 0, 0, 0);
	}
	ic->flags = flags;
	systray_layout(barw, statw);
	return 1;
}

int
systray_handle_configurerequest(XConfigureRequestEvent *ev)
{
	TrayIcon *ic = iconfind(ev->window);
	XWindowAttributes wa;
	XConfigureEvent ce;
	int rx, ry;
	Window child;

	if (!ic)
		return 0;
	/* the tray decides the icon geometry; as the XEmbed embedder we act as
	 * the icon's window manager and answer with the current geometry */
	if (XGetWindowAttributes(dpy, ev->window, &wa)) {
		XTranslateCoordinates(dpy, ev->window, RootWindow(dpy, scr), 0, 0, &rx, &ry, &child);
		memset(&ce, 0, sizeof ce);
		ce.type = ConfigureNotify;
		ce.event = ev->window;
		ce.window = ev->window;
		ce.x = rx;
		ce.y = ry;
		ce.width = wa.width;
		ce.height = wa.height;
		ce.border_width = wa.border_width;
		ce.above = None;
		ce.override_redirect = False;
		XSendEvent(dpy, ev->window, False, StructureNotifyMask, (XEvent *)&ce);
	}
	return 1;
}

int
systray_handle_maprequest(Window w)
{
	TrayIcon *ic = iconfind(w);

	if (!ic)
		return 0;
	/* Qt icons map without ever writing _XEMBED_INFO: treat as mapped */
	XMapWindow(dpy, w);
	xembed_send(w, XEMBED_WINDOW_ACTIVATE, 0, 0, 0);
	if (!(ic->flags & XEMBED_MAPPED)) {
		ic->flags |= XEMBED_MAPPED;
		systray_layout(barw, statw);
	}
	return 1;
}

int
systray_handle_destroynotify(Window w)
{
	return removeicon(w, 0); /* save-set entry disappears with the window */
}

int
systray_handle_reparentnotify(Window w, Window newparent)
{
	if (newparent == traywin)
		return 0; /* that is our own dock() reparent in progress */
	return removeicon(w, 1);
}

void
systray_cleanup(void)
{
	TrayIcon *ic, *next;

	if (!traywin)
		return;
	XSetSelectionOwner(dpy, None, net_system_tray, CurrentTime);
	for (ic = icons; ic; ic = next) {
		next = ic->next;
		XRemoveFromSaveSet(dpy, ic->win);
		XReparentWindow(dpy, ic->win, RootWindow(dpy, scr), 0, 0); /* icon lives on */
		free(ic);
	}
	icons = NULL;
	trayw = 0;
	XUnmapWindow(dpy, traywin);
	XDestroyWindow(dpy, traywin);
	traywin = None;
}
