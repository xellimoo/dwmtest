/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for SubstructureRedirectMask on the root window, to receive
 * events about window (dis-)appearance. Only one X connection at a time is
 * allowed to select for this event mask.
 *
 * The event handlers of dwm are organized in an array which is accessed
 * whenever a new event has been fetched. This allows event dispatching
 * in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Keys and tagging rules are organized as arrays and defined in config.h.
 *
 * To understand everything else, start reading main().
 */
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/select.h>
/* The theme file watch is the one thing with no portable spelling: Linux has
 * inotify, the BSDs have kqueue. Everything else in this file is POSIX.
 *
 * The mechanism is picked from the compiler's own platform macros, so no build
 * system configuration is needed, but either can be forced with -D so the
 * other branch can be compiled and tested on one machine. */
#if !defined(EDWM_INOTIFY) && !defined(EDWM_KQUEUE)
# if defined(__linux__)
#  define EDWM_INOTIFY 1
# elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) \
    || defined(__DragonFly__) || defined(__APPLE__)
#  define EDWM_KQUEUE 1
# endif
#endif

#if defined(EDWM_INOTIFY)
# include <sys/inotify.h>
#elif defined(EDWM_KQUEUE)
# include <sys/event.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/Xft/Xft.h>
#include <X11/extensions/XInput2.h>

#include "dbusif.h"
#include "drw.h"
#include "dbusmenu.h"
#include "menu.h"
#include "session.h"
#include "sni.h"
#include "systray.h"
#include "theme.h"
#include "util.h"

/* macros */
#define BUTTONMASK              (ButtonPressMask|ButtonReleaseMask)
#define CLEANMASK(mask)         (mask & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) \
                               * MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))
#define ISVISIBLE(C)            ((C->tags & C->mon->tagset[C->mon->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define TEXTW(X)                (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
/* color schemes come from theme.h, which owns the runtime theme model */
/* Control surface usable without D-Bus: writing a command string to _EDWM_CMD
 * on the root window is picked up by propertynotify. edwmctl falls back to
 * this, so theming is fully scriptable even in a build without libdbus. */
enum { EdwmCommand, EdwmTheme, EdwmLast };
enum { NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetWMFullscreen, NetActiveWindow, NetWMWindowType,
       NetWMWindowTypeDialog, NetClientList, NetLast }; /* EWMH atoms */
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast }; /* default atoms */
enum { ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
       ClkClientWin, ClkRootWin, ClkLast }; /* clicks */

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;

/* A record of what drawbar() actually drew, and where.
 *
 * buttonpress() used to re-derive the bar layout with its own arithmetic,
 * which had already drifted: the client-count and snap badges were drawn but
 * not accounted for, so clicks on them were reported as title clicks, and the
 * status hit region was wider than the drawn text. Recording the segments as
 * they are drawn keeps hit-testing and rendering from ever disagreeing, and is
 * what makes per-window title buttons possible at all. */
typedef enum {
	SegNone, SegTag, SegLayout, SegCount, SegSnap, SegTask, SegStatus,
	SegSystray, SegSni
} SegType;

typedef struct {
	int x, w;
	SegType type;
	unsigned int ui;	/* tag index, for SegTag */
	Client *c;		/* the window, for SegTask */
} BarSeg;

#define BARSEGMAX 64
struct Client {
	char name[256];
	float mina, maxa;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
    int x_before_snap, y_before_snap, w_before_snap, h_before_snap;
    int x_before_staylow, y_before_staylow, w_before_staylow, h_before_staylow;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;
	int bw, oldbw;
	unsigned int tags;
	int isalwaysontop, isfixed, isfloating, isurgent, neverfocus, oldstate, isstaylow, issetfullscreen, isfullscreen, isinsnap, isterm;
	Client *next;
	Client *snext;
    Client* switch_next;
	Monitor *mon;
	Window win;
};

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	char ltsymbol[16];
	float mfact;
	int nmaster;
	int num;
	int by;               /* bar geometry */
	int mx, my, mw, mh;   /* screen size */
	int wx, wy, ww, wh;   /* window area  */
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	int showbar;
	int topbar;
    BarSeg segs[BARSEGMAX];	/* what drawbar() drew, for hit-testing */
    int nsegs;
    int istaskview;
    int isinsnap;
    int issnapinitstate;
    int snap_tag;
    int taskview_tag;
    int cur_tag;
	Client *clients;
	Client *sel;
	Client *stack;
	Monitor *next;
	Window barwin;
	const Layout *lt[2];
};

typedef struct {
	const char *class;
	const char *instance;
	const char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
} Rule;

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void arrange(Monitor *m);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void drawtasks(Monitor *m, int x, int avail, int boxs, int boxw, unsigned int n);
static void barsegadd(Monitor *m, int x, int w, SegType t, unsigned int ui, Client *c);
static BarSeg *barsegat(Monitor *m, int x);
static void focusclient(const Arg *arg);
static void taskraise(const Arg *arg);
static void taskkill(const Arg *arg);
static void drawbars(void);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusin(XEvent *e);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void grabbuttons(Client *c, int focused);
static void grabkeys(void);
static void incnmaster(const Arg *arg);
static void keypress(XEvent *e);
static void killclient(const Arg *arg);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(Monitor *m);
static void motionnotify(XEvent *e);
static void movemouse(const Arg *arg);
static Client *nexttiled(Client *c);
static void pop(Client *c);
static void propertynotify(XEvent *e);
static void quit(const Arg *arg);
static Monitor *recttomon(int x, int y, int w, int h);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemouse(const Arg *arg);
static void restack(Monitor *m);
static void run(void);
static void evsrc_add(int fd, void (*cb)(void));
static void evsrc_del(int fd);
static void setupsignals(void);
static void handlesignals(void);
static void handlexfd(void);
static void scan(void);
static int sendevent(Client *c, Atom proto);
static void sendmon(Client *c, Monitor *m);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setup(void);
static void setupvisual(void);
static int applytheme(const Theme *t);
static void inittheme(void);
static void reloadtheme(void);
static void runcommand(void);
static void settone(const char *spec);
static void docommand(char *cmd);
static void handledbus(void);
static void dbus_docommand(const char *cmd);
static const char *dbus_gettheme(void);
static void dbus_listclients(DbusClientSink sink, void *sinkdata);
static void dbus_focuswin(unsigned long win);
static void dbus_closewin(unsigned long win);
static void dbus_viewtags(unsigned int tagmask);
static void dbus_setstatus(const char *text);
static void dbus_doquit(void);
static void setthemebyname(const char *name);
static void setupthemewatch(void);
static void reparentnotify(XEvent *e);
static void selectionclear(XEvent *e);
static Monitor *systraymon(void);
static void systraychanged(void);
static void handlethemewatch(void);
static void seturgent(Client *c, int urg);
static void showhide(Client *c);
static void spawn(const Arg *arg);
static void childsignals(void);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglebar(const Arg *arg);
static void togglefloating(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(void);
static void updatenumlockmask(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);
static Client *wintoclient(Window w);
static Monitor *wintomon(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);
static void zoom(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void doswitchclient(const Arg *arg);
static void raisetotop(const Arg *arg);
static void staylow(const Arg *arg);
static void clear_client_size_pos_data(Client* c);
static void save_client_size_pos_data(Client* c);
static void entertaskview();
static void leavetaskview();
static void snapsidebyside();
static void swapsidebyside();
static void snap2left();
static void snap2right();
static void adjustwidth(const Arg* arg);
static void keymove(int dx, int dy);
static void keyresize(int dw, int dh);
static void movex(const Arg* arg);
static void movey(const Arg* arg);
static void resizew(const Arg* arg);
static void resizeh(const Arg* arg);
static void enterstaylow(Client* c);
static void leavestaylow(Client* c);
static void relayoutstaylow(void);
static void spawnxterm(const Arg* arg);
static void gevent(XEvent *e);
static void chordreplay(void);
static void chorddrag(Client *c);
static long nowms(void);
static int xerrorxi(Display *dpy, XErrorEvent *ee);
static int lowh = 100;
static int loww = 300;
static int offsetx = 3; /* px right margin of the staylow stack */
static int offsety = 3; /* px bottom margin below the lowest staylow window */
static int offsetoverlap = 8; /* px gap between stacked staylow windows */
static int staylowcount = 0;
static int offsetoverlapxterm = 20;
static int xtermcount = 0;

/* variables */
static const char broken[] = "broken";
static char stext[1024];
static int screen;
static int sw, sh;           /* X display screen geometry width, height */
static Visual *visual;       /* visual used for the bar and drw's pixmap */
static int depth;            /* its depth: 32 when a compositor-backed ARGB
                              * visual was found and useargb is on, else the
                              * screen default */
static Colormap cmap;
static Theme theme;          /* the live theme; reloaded without restarting */
static unsigned int cborderpx; /* theme.borderpx, seeded from config.h */
static int themewd = -1;     /* inotify watch descriptor, or the watched dir fd */
#ifdef EDWM_KQUEUE
static int themefilefd = -1; /* kqueue watches descriptors, so the file needs one */
#endif
/* Deadline, not a flag: when edwmctl makes us rewrite theme.conf we suppress
 * the watch event our own write causes. A plain boolean would go stale and
 * swallow the user's next real edit if that event never arrived -- which is
 * exactly what happens wherever the watch could not be set up. */
static long themeselfwrite;
static int inotifyfd = -1;
static int dbusfd = -1;      /* changes across a reconnect, so it is tracked */
static int bh;               /* bar height */
static int lrpad;            /* sum of left and right padding for text */
static int (*xerrorxlib)(Display *, XErrorEvent *);
static unsigned int numlockmask = 0;
static void (*handler[LASTEvent]) (XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify] = configurenotify,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[Expose] = expose,
	[FocusIn] = focusin,
	[KeyPress] = keypress,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify,
	[PropertyNotify] = propertynotify,
	[ReparentNotify] = reparentnotify,
	[SelectionClear] = selectionclear,
	[UnmapNotify] = unmapnotify,
	[GenericEvent] = gevent
};
static Atom wmatom[WMLast], netatom[NetLast], edwmatom[EdwmLast];
static int running = 1;
static Cur *cursor[CurLast];
static Clr **scheme;
static Display *dpy;
static Drw *drw;
static Monitor *mons, *selmon;
static Window root, wmcheckwin;
static int xi_opcode = -1;
static int xi_sel_err = 0;
static int xi_ok = 0;			/* XI2 raw events available for chord detection */
/* Extra descriptors multiplexed into the main loop next to the X connection.
 * Later subsystems (theme file watch, D-Bus) register themselves here rather
 * than each growing its own branch inside run(). */
#define MAXEVFD 8
static struct pollfd evpfd[MAXEVFD];
static void (*evcb[MAXEVFD])(void);
static int nevfd = 0;
/* Signals reach the loop through a pipe: the handler writes the number and
 * returns, and the real work happens in normal context next time round. That
 * is the portable spelling of Linux's signalfd, and needs no second
 * implementation for the BSDs. */
static int sigpipe[2] = { -1, -1 };

static Window chordwin = None;		/* client whose button press is on hold */
static unsigned int chordbtn;		/* the held button (Button1 or Button3) */
static int chordx, chordy;		/* root coordinates of the held press */
static long chorddeadline;		/* monotonic ms when the hold gives up */

/* configuration, allows nested code to access above variables */
#include "config.h"

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* function implementations */
void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule *r;
	Monitor *m;
	XClassHint ch = { NULL, NULL };

	/* rule matching */
    // let's make every new window opened as floating
	c->isfloating = 1;
	c->issetfullscreen = 0;
    c->isinsnap = 0;
	c->tags = 0;
	XGetClassHint(dpy, c->win, &ch);
	class    = ch.res_class ? ch.res_class : broken;
	instance = ch.res_name  ? ch.res_name  : broken;
	c->isterm = (strstr(class, "Alacritty") || strstr(class, "XTerm")) ? 1 : 0;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->title || strstr(c->name, r->title))
		&& (!r->class || strstr(class, r->class))
		&& (!r->instance || strstr(instance, r->instance)))
		{
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			for (m = mons; m && m->num != r->monitor; m = m->next);
			if (m)
				c->mon = m;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
	c->tags = c->tags & TAGMASK ? c->tags & TAGMASK : c->mon->tagset[c->mon->seltags];
}

int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	int baseismin;
	Monitor *m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (resizehints || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
		if (!c->hintsvalid)
			updatesizehints(c);
		/* see last two sentences in ICCCM 4.1.2.3 */
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) { /* temporarily remove base dimensions */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for aspect limits */
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float)*w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float)*h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) { /* increment calculation requires this */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for increment value */
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		/* restore base dimensions */
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void
arrange(Monitor *m)
{
	if (m)
		showhide(m->stack);
	else for (m = mons; m; m = m->next)
		showhide(m->stack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else for (m = mons; m; m = m->next)
		arrangemon(m);
}

void
arrangemon(Monitor *m)
{
    // show task view symbol
    strncpy(m->ltsymbol, m->istaskview == 1 ? layouts[1].symbol : layouts[0].symbol, sizeof m->ltsymbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void
attach(Client *c)
{
	c->next = c->mon->clients;
	c->mon->clients = c;
}

void
attachstack(Client *c)
{
	c->snext = c->mon->stack;
    c->mon->stack = c;

    if (c->snext && ISVISIBLE(c->snext))
        c->switch_next = c->snext;
}

void
buttonpress(XEvent *e)
{
	if (menu_event(e))
		return;

	unsigned int i, click;
	Arg arg = {0};
	Client *c;
	Monitor *m;
	XButtonPressedEvent *ev = &e->xbutton;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->window)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if (ev->window == selmon->barwin) {
		/* hit-test against what drawbar() recorded, so the two can never
		 * disagree about where anything is */
		BarSeg *seg = barsegat(selmon, ev->x);

		if (seg)
			switch (seg->type) {
			case SegTag:
				click = ClkTagBar;
				arg.ui = 1 << seg->ui;
				break;
			case SegLayout:
				click = ClkLtSymbol;
				break;
			case SegStatus:
				click = ClkStatusText;
				break;
			case SegTask:
				click = ClkWinTitle;
				arg.v = seg->c;
				break;
			case SegSni:
				/* forwarded straight to the application, which owns the
				 * meaning of each button */
				sni_click(sni_indexat(ev->x, seg->x,
				                      theme.systrayiconsize,
				                      theme.systrayspacing),
				          ev->button, ev->x_root, ev->y_root);
				return;
			default:
				break; /* count and snap badges are indicators, not buttons */
			}
	} else if ((c = wintoclient(ev->window))) {
        if (selmon->istaskview == 1) {
            Client *cli;
            selmon->istaskview = 0;
            for (cli = selmon->clients; cli; cli = cli->next) {
                if (cli->tags == selmon->tagset[selmon->seltags]) {
                    // don't want any fixed windows
                    // i want them all floating, the resize call
                    // will make sure they go back to theri original
                    // size and potision
                    cli->isfloating = 1;
                    resizeclient(cli, cli->oldx, cli->oldy, cli->oldw, cli->oldh);
                }
            }
            arrange(selmon);
        }
        // focus the client clicked and exit task view
        focus(c);
        restack(selmon);
        selmon->nmaster = 1;
		if (xi_ok && !chordwin && !c->isfullscreen
		&& (ev->button == Button1 || ev->button == Button3)
		&& CLEANMASK(ev->state) == 0) {
			/* hold the press for chord detection; it stays frozen in the
			 * server, so the client receives nothing until gevent() replays
			 * it (plain click) or starts the chord drag */
			chordwin = c->win;
			chordbtn = ev->button;
			chordx = ev->x_root;
			chordy = ev->y_root;
			chorddeadline = nowms() + chordwait;
			return;
		}
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        click = ClkClientWin;
	}
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func && buttons[i].button == ev->button
		&& CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func((click == ClkTagBar || click == ClkWinTitle)
			                && buttons[i].arg.i == 0 ? &arg : &buttons[i].arg);
}

/* GenericEvent handler: XI2 raw events are the only pointer activity we can
 * observe while the server has the pointer frozen on a held press (see the
 * chord hold in buttonpress()). Raw events are delivered regardless of grabs
 * and freezes, which is what makes chord detection possible at all. */
void
gevent(XEvent *e)
{
	XGenericEventCookie *cookie = &e->xcookie;
	XIRawEvent *rev;
	Client *c;
	Window dummy, child;
	int rx, ry, wx, wy;
	unsigned int mask;

	if (cookie->extension != xi_opcode)
		return;
	if (cookie->evtype == XI_RawMotion && !chordwin)
		return; /* most frequent event; bail before XGetEventData */
	if (!XGetEventData(dpy, cookie))
		return;
	rev = cookie->data;

	if (!chordwin)
		; /* nothing held */
	else if (cookie->evtype == XI_RawButtonPress
	&& (rev->detail == Button1 || rev->detail == Button2 || rev->detail == Button3)
	&& rev->detail != chordbtn) {
		/* second press of a chord (L+R, L+M, R+M): drag the window */
		if ((c = wintoclient(chordwin)) && !c->isfullscreen)
			chorddrag(c);
		else
			chordreplay();
	} else if (cookie->evtype == XI_RawButtonPress) {
		chordreplay(); /* scroll wheel or extra button: not a chord */
	} else if (cookie->evtype == XI_RawButtonRelease
	&& (rev->detail == Button1 || rev->detail == Button2 || rev->detail == Button3)) {
		chordreplay(); /* held button released (or missed partner): plain click */
	} else if (cookie->evtype == XI_RawMotion
	&& XQueryPointer(dpy, root, &dummy, &child, &rx, &ry, &wx, &wy, &mask)
	&& (abs(rx - chordx) > chorddist || abs(ry - chordy) > chorddist)) {
		chordreplay(); /* plain drag intent */
	}
	XFreeEventData(dpy, cookie);
}

/* hand the held press, and everything queued behind it, back to the client
 * exactly as if the grab had never existed */
void
chordreplay(void)
{
	if (!chordwin)
		return;
	chordwin = None;
	XAllowEvents(dpy, ReplayPointer, CurrentTime);
}

/* chord confirmed: drag the window. The client never sees the chord - the
 * held press is swallowed with the freeze, the partner press and all motions
 * are routed to dwm by our grab, and the partner's trailing release is
 * swallowed by the tail grab below. */
void
chorddrag(Client *c)
{
	Window dummy, child;
	int rx, ry, wx, wy, i;
	unsigned int mask;

	chordwin = None;
	/* unfreeze but keep the grab: the queued partner press is delivered
	 * to dwm (the grab owner), never to the client */
	XAllowEvents(dpy, AsyncPointer, CurrentTime);
	if (selmon->sel != c)
		focus(c); /* keyboard focus may have moved during the hold */
	movemouse(NULL);
	/* swallow the tail: the partner button is still logically down and its
	 * trailing motions/release must not leak into the client */
	if (XGrabPointer(dpy, root, False, BUTTONMASK|PointerMotionMask,
	GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) {
		for (i = 0; i < 100; i++) { /* at most ~500 ms */
			if (!XQueryPointer(dpy, root, &dummy, &child, &rx, &ry, &wx, &wy, &mask)
			|| !(mask & (Button1Mask|Button2Mask|Button3Mask)))
				break;
			usleep(5000);
		}
		XUngrabPointer(dpy, CurrentTime);
	}
}

long
nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

void
cleanup(void)
{
	Arg a = {.ui = ~0};
	Layout foo = { "", NULL };
	Monitor *m;
	size_t i;

	view(&a);
	selmon->lt[selmon->sellt] = &foo;
	for (m = mons; m; m = m->next)
		while (m->stack)
			unmanage(m->stack, 0);
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	dbusif_cleanup();
	systray_cleanup();
	while (mons)
		cleanupmon(mons);
	for (i = 0; i < CurLast; i++)
		drw_cur_free(drw, cursor[i]);
	for (i = 0; i < SchemeLast; i++)
		drw_scm_free(drw, scheme[i], THEME_NCOLOR);
	free(scheme);
	XDestroyWindow(dpy, wmcheckwin);
	drw_free(drw);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

void
cleanupmon(Monitor *mon)
{
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next);
		m->next = mon->next;
	}
	XUnmapWindow(dpy, mon->barwin);
	XDestroyWindow(dpy, mon->barwin);
	free(mon);
}

void
clientmessage(XEvent *e)
{
	/* Dock requests are addressed to the tray window, so this has to come
	 * before the "not one of our clients, ignore" bail below. */
	if (systray_clientmessage(&e->xclient))
		return;

	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);

	if (!c)
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if (cme->data.l[1] == netatom[NetWMFullscreen]
		|| cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c, (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD    */
				|| (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ && !c->isfullscreen)));
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		if (c != selmon->sel && !c->isurgent)
			seturgent(c, 1);
	}
}

void
configure(Client *c)
{
	XConfigureEvent ce;

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void
configurenotify(XEvent *e)
{
	Monitor *m;
	Client *c;
	XConfigureEvent *ev = &e->xconfigure;
	int dirty;

	/* TODO: updategeom handling sucks, needs to be simplified */
	if (ev->window == root) {
		dirty = (sw != ev->width || sh != ev->height);
		sw = ev->width;
		sh = ev->height;
		if (updategeom() || dirty) {
			drw_resize(drw, sw, bh);
			updatebars();
			for (m = mons; m; m = m->next) {
				for (c = m->clients; c; c = c->next)
					if (c->isfullscreen)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, m->ww, bh);
			}
			focus(NULL);
			arrange(NULL);
		}
	}
}

void
configurerequest(XEvent *e)
{
	if (systray_configurerequest(&e->xconfigurerequest))
		return;

	Client *c;
	Monitor *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (ev->value_mask & CWX) {
				c->oldx = c->x;
				c->x = m->mx + ev->x;
			}
			if (ev->value_mask & CWY) {
				c->oldy = c->y;
				c->y = m->my + ev->y;
			}
			if (ev->value_mask & CWWidth) {
				c->oldw = c->w;
				c->w = ev->width;
			}
			if (ev->value_mask & CWHeight) {
				c->oldh = c->h;
				c->h = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx + (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my + (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		} else
			configure(c);
	} else {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

Monitor *
createmon(void)
{
	Monitor *m;

	m = ecalloc(1, sizeof(Monitor));
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact = mfact;
	m->nmaster = nmaster;
	m->showbar = showbar;
	m->topbar = topbar;
	m->lt[0] = &layouts[0];
	m->lt[1] = &layouts[1];
    m->cur_tag = 0x1;
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
	return m;
}

void
destroynotify(XEvent *e)
{
	if (systray_destroynotify(&e->xdestroywindow))
		return;

	Client *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
}

void
detach(Client *c)
{
	Client **tc;

	for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next);
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc, *t;

	for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext);
	*tc = c->snext;
    c->switch_next = NULL;

	if (c == c->mon->sel) {
		for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext);
		c->mon->sel = t;
	}
}

Monitor *
dirtomon(int dir)
{
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons)
		for (m = mons; m->next; m = m->next);
	else
		for (m = mons; m->next != selmon; m = m->next);
	return m;
}

void
barsegadd(Monitor *m, int x, int w, SegType t, unsigned int ui, Client *c)
{
	if (m->nsegs >= BARSEGMAX || w <= 0)
		return;
	m->segs[m->nsegs].x = x;
	m->segs[m->nsegs].w = w;
	m->segs[m->nsegs].type = t;
	m->segs[m->nsegs].ui = ui;
	m->segs[m->nsegs].c = c;
	m->nsegs++;
}

BarSeg *
barsegat(Monitor *m, int x)
{
	int i;

	for (i = 0; i < m->nsegs; i++)
		if (x >= m->segs[i].x && x < m->segs[i].x + m->segs[i].w)
			return &m->segs[i];
	return NULL;
}

/* One clickable button per window visible on this monitor, filling the region
 * the single focused title used to occupy. Seeing what else is open on the
 * current screen, and being able to click straight to it, is the point. */
void
drawtasks(Monitor *m, int x, int avail, int boxs, int boxw, unsigned int n)
{
	Client *c, *vis[BARSEGMAX];
	int w, left = avail, scm, nv = 0, k;

	if (avail <= 0)
		return;
	if (!n) { /* nothing open: blank the region */
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x, 0, avail, bh, 1, 1);
		return;
	}

	/* Equal shares, clamped to the theme's bounds. When even the minimum will
	 * not fit, fall back to an equal split so every window still gets a
	 * button rather than some being dropped entirely. */
	w = avail / (int)n;
	if (w > theme.taskmaxw)
		w = theme.taskmaxw;
	if (w < theme.taskminw)
		w = (theme.taskminw * (int)n <= avail) ? theme.taskminw : avail / (int)n;

	/* dwm attaches new clients at the head, so walking m->clients directly
	 * would put the newest window first and shift every button along whenever
	 * one opens. Collect and reverse, giving a stable oldest-first order where
	 * new windows simply appear on the right. */
	for (c = m->clients; c && nv < BARSEGMAX; c = c->next)
		if (ISVISIBLE(c))
			vis[nv++] = c;

	for (k = nv - 1; k >= 0 && left > 0; k--) {
		c = vis[k];
		if (w > left)
			w = left;
		if (c->isurgent)
			scm = SchemeTaskUrg;
		else if (c == m->sel && m == selmon)
			scm = SchemeTaskSel;
		else if (c->isstaylow)
			scm = SchemeTaskMin;
		else
			scm = SchemeTaskNorm;
		drw_setscheme(drw, scheme[scm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, c->name, 0);
		if (c->isfloating)
			drw_rect(drw, x + boxs, boxs, boxw, boxw, c->isfixed, 0);
		if (c->isalwaysontop)
			drw_rect(drw, x + boxs, bh - boxw - boxs, boxw, boxw, 0, 0);
		barsegadd(m, x, w, SegTask, 0, c);
		x += w;
		left -= w;
	}
	if (left > 0) { /* trailing gap */
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x, 0, left, bh, 1, 1);
	}
}

void
drawbar(Monitor *m)
{
	int x, w, tw = 0, stw = 0, snw = 0, trayw = 0, trayx, statusx;
	int boxs = drw->fonts->h / 9;
	int boxw = drw->fonts->h / 6 + 2;
	unsigned int i, occ = 0, urg = 0, n = 0;
	char buf[16];
	const char *ltsym;
	Client *c;

	if (!m->showbar)
		return;

	m->nsegs = 0;

	/* The right end of the bar, laid out from the edge inwards:
	 *
	 *     [ ...taskbar... ][ SNI icons ][ XEmbed icons ][ status text ]
	 *
	 * Status sits hard against the screen edge and the tray sits to its left,
	 * so a status line of system readings reads as one block at the end. */
	tw = TEXTW(stext) - lrpad + 2; /* 2px right padding */
	statusx = m->ww - tw;

	if (m == systraymon()) {
		stw = systray_width();
		snw = sni_width(theme.systrayiconsize, theme.systrayspacing);
		/* Each icon carries its own left margin, so the two kinds need no
		 * extra gap between them; only a trailing one, so the last icon does
		 * not touch the status text. */
		trayw = (stw || snw) ? stw + snw + theme.systrayspacing : 0;
	}
	trayx = statusx - trayw;

	/* status first, so the tags can overdraw it if they must */
	drw_setscheme(drw, scheme[SchemeStatus]);
	drw_text(drw, statusx, 0, tw, bh, 0, stext, 0);
	barsegadd(m, statusx, tw, SegStatus, 0, NULL);
	if (trayw) {
		/* paint under both trays so their icons composite against the theme */
		drw_setscheme(drw, scheme[SchemeSystray]);
		drw_rect(drw, trayx, 0, trayw, bh, 1, 1);
		if (snw)
			barsegadd(m, trayx, snw, SegSni, 0, NULL);
		if (stw)
			barsegadd(m, trayx + snw, stw, SegSystray, 0, NULL);
	}

	for (c = m->clients; c; c = c->next) {
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
		if (ISVISIBLE(c))
			n++; /* bitmask overlap, so a window on several tags counts once */
	}

	x = 0;
	for (i = 0; i < LENGTH(tags); i++) {
		w = TEXTW(tags[i]);
		drw_setscheme(drw, scheme[m->tagset[m->seltags] & 1 << i ? SchemeTagSel : SchemeTagNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, tags[i], urg & 1 << i);
		if (occ & 1 << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
				m == selmon && selmon->sel && selmon->sel->tags & 1 << i,
				urg & 1 << i);
		barsegadd(m, x, w, SegTag, i, NULL);
		x += w;
	}

	/* layout symbol: read the monitor being drawn, not selmon, or a second
	 * monitor shows the selected one's state */
	ltsym = (m->istaskview && m->cur_tag == m->taskview_tag) ? "[T]" : "><>";
	w = TEXTW(ltsym);
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, x, 0, w, bh, lrpad / 2, ltsym, 0);
	barsegadd(m, x, w, SegLayout, 0, NULL);
	x += w;

	/* how many windows are on the current tag */
	snprintf(buf, sizeof buf, "#%d", n);
	w = TEXTW(buf) + (m->isinsnap ? 0 : 10);
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, x, 0, w, bh, lrpad / 2, buf, 0);
	barsegadd(m, x, w, SegCount, 0, NULL);
	x += w;

	if (m->isinsnap && m->cur_tag == m->snap_tag) {
		w = TEXTW("[S]") + 10;
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, "[S]", 0);
		barsegadd(m, x, w, SegSnap, 0, NULL);
		x += w;
	}

	drawtasks(m, x, trayx - x, boxs, boxw, n);

	/* SNI icons are painted, not embedded, so they go onto the pixmap before
	 * it is copied to the bar. */
	if (snw)
		sni_draw(drw, trayx, (bh - theme.systrayiconsize) / 2,
		         theme.systrayiconsize, theme.systrayspacing,
		         scheme[SchemeSystray][ColBg].pixel);

	drw_map(drw, m->barwin, 0, 0, m->ww, bh);
	/* after drw_map: the XEmbed tray is a real child window, so it is placed
	 * once the bar beneath it has been painted */
	if (m == systraymon())
		systray_place(m->barwin, trayx + snw, 0, bh,
		              scheme[SchemeSystray][ColBg].pixel);
}

/* Reach a window from its taskbar button. Switches to one of its tags if it is
 * off-screen, leaves task view, and un-minimises it, so the button works from
 * any state. Clicking the window that already has focus minimises it, which is
 * what a taskbar button is expected to do. */
void
focusclient(const Arg *arg)
{
	Client *c = (Client *)arg->v;
	Arg a;

	if (!c)
		return;
	if (c == selmon->sel && !c->isstaylow) {
		enterstaylow(c); /* toggle down */
		return;
	}
	if (c->mon != selmon) {
		unfocus(selmon->sel, 0);
		selmon = c->mon;
	}
	if (!ISVISIBLE(c)) {
		a.ui = c->tags & TAGMASK;
		if (a.ui)
			view(&a);
	}
	if (selmon->istaskview)
		leavetaskview();
	if (c->isstaylow)
		leavestaylow(c);
	focus(c);
	restack(selmon);
	XRaiseWindow(dpy, c->win);
	drawbars();
}

/* The always-on-top and close actions already exist and operate on the focused
 * window, so focus the clicked one first and reuse them rather than
 * duplicating their logic. */
void
taskraise(const Arg *arg)
{
	Client *c = (Client *)arg->v;

	if (!c)
		return;
	focusclient(arg);
	raisetotop(NULL);
}

void
taskkill(const Arg *arg)
{
	Client *c = (Client *)arg->v;

	if (!c)
		return;
	if (c->mon != selmon) {
		unfocus(selmon->sel, 0);
		selmon = c->mon;
	}
	focus(c);
	killclient(NULL);
}

/* The tray lives on one monitor only. Pinning it to the first keeps it from
 * hopping between screens as focus moves. */
Monitor *
systraymon(void)
{
	return mons;
}

/* The tray's contents changed, so the bar needs re-laying out around it. */
void
systraychanged(void)
{
	drawbars();
}

void
reparentnotify(XEvent *e)
{
	systray_reparentnotify(&e->xreparent);
}

void
selectionclear(XEvent *e)
{
	systray_selectionclear(&e->xselectionclear);
}

void
drawbars(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		drawbar(m);
}

void
enternotify(XEvent *e)
{
	Client *c;
	Monitor *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	c = wintoclient(ev->window);
	m = c ? c->mon : wintomon(ev->window);
	if (m != selmon) {
        unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
    // focus(c);
}

void
expose(XEvent *e)
{
	if (menu_event(e))
		return;

	Monitor *m;
	XExposeEvent *ev = &e->xexpose;

	if (ev->count == 0 && (m = wintomon(ev->window)))
		drawbar(m);
}

void
focus(Client *c)
{
	if (!c || !ISVISIBLE(c))
		for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext);
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);
		setfocus(c);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	dbusif_emit_focus(c ? (unsigned long)c->win : 0UL);
	drawbars();
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (!mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
}

void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next);
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next);
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
	}
}

Atom
getatomprop(Client *c, Atom prop)
{
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, XA_ATOM,
		&da, &di, &dl, &dl, &p) == Success && p) {
		atom = *(Atom *)p;
		XFree(p);
	}
	return atom;
}

int
getrootptr(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long
getstate(Window w)
{
	int format;
	long result = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False, wmatom[WMState],
		&real, &format, &n, &extra, (unsigned char **)&p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	char **list = NULL;
	int n;
	XTextProperty name;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
		return 0;
	if (name.encoding == XA_STRING) {
		strncpy(text, (char *)name.value, size - 1);
	} else if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success && n > 0 && *list) {
		strncpy(text, *list, size - 1);
		XFreeStringList(list);
	}
	text[size - 1] = '\0';
	XFree(name.value);
	return 1;
}

void
grabbuttons(Client *c, int focused)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		if (!focused)
			XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
				BUTTONMASK, GrabModeSync, GrabModeSync, None, None);
		for (i = 0; i < LENGTH(buttons); i++)
			if (buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabButton(dpy, buttons[i].button,
						buttons[i].mask | modifiers[j],
						c->win, False, BUTTONMASK,
						GrabModeAsync, GrabModeSync, None, None);
		if (focused)
			/* plain left/right presses are grabbed synchronously so
			 * buttonpress() can hold them for chord detection: the press
			 * stays frozen server-side and reaches the client only when
			 * dwm replays it (XAllowEvents ReplayPointer). Unfocused
			 * windows already freeze presses via the AnyButton grab. */
			for (j = 0; j < LENGTH(modifiers); j++) {
				XGrabButton(dpy, Button1, modifiers[j], c->win, False,
					BUTTONMASK, GrabModeSync, GrabModeAsync, None, None);
				XGrabButton(dpy, Button3, modifiers[j], c->win, False,
					BUTTONMASK, GrabModeSync, GrabModeAsync, None, None);
			}
	}
}

void
grabkeys(void)
{
	updatenumlockmask();
	{
		unsigned int i, j, k;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		int start, end, skip;
		KeySym *syms;

		XUngrabKey(dpy, AnyKey, AnyModifier, root);
		XDisplayKeycodes(dpy, &start, &end);
		syms = XGetKeyboardMapping(dpy, start, end - start + 1, &skip);
		if (!syms)
			return;
		for (k = start; k <= end; k++)
			for (i = 0; i < LENGTH(keys); i++)
				/* skip modifier codes, we do that ourselves */
				if (keys[i].keysym == syms[(k - start) * skip])
					for (j = 0; j < LENGTH(modifiers); j++)
						XGrabKey(dpy, k,
							 keys[i].mod | modifiers[j],
							 root, True,
							 GrabModeAsync, GrabModeAsync);
		XFree(syms);
	}
}

void
incnmaster(const Arg *arg)
{
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

#ifdef XINERAMA
static int
isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
		&& unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

void
keypress(XEvent *e)
{
	if (menu_event(e))
		return;

	unsigned int i;
	KeySym keysym;
	XKeyEvent *ev;

	ev = &e->xkey;
	keysym = XKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0);
	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym
		&& CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)
		&& keys[i].func)
			keys[i].func(&(keys[i].arg));
}

void
killclient(const Arg *arg)
{
    Client *c;
	if (!(c = selmon->sel))
		return;

	if (!sendevent(selmon->sel, wmatom[WMDelete])) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, selmon->sel->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void
manage(Window w, XWindowAttributes *wa)
{
	Client *c, *t = NULL;
	Window trans = None;
	XWindowChanges wc;

	if (systray_trykdedock(w))
		return; /* a legacy KDE applet belongs in the tray, not the stack */

	c = ecalloc(1, sizeof(Client));
	c->win = w;
	/* geometry */
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh = wa->height;
	c->oldbw = wa->border_width;

	updatetitle(c);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = selmon;
		applyrules(c);
	}

	if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww)
		c->x = c->mon->wx + c->mon->ww - WIDTH(c);
	if (c->y + HEIGHT(c) > c->mon->wy + c->mon->wh)
		c->y = c->mon->wy + c->mon->wh - HEIGHT(c);
	c->x = MAX(c->x, c->mon->wx);
	c->y = MAX(c->y, c->mon->wy);
	c->bw = cborderpx;

	wc.border_width = c->bw;
	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = trans != None || c->isfixed;
	if (c->isfloating)
		XRaiseWindow(dpy, c->win);
	attach(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32, PropModeAppend,
		(unsigned char *) &(c->win), 1);
	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w, c->h); /* some windows require this */
	setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	c->mon->sel = c;
	arrange(c->mon);
	XMapWindow(dpy, c->win);
	focus(NULL);
}

void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

void
maprequest(XEvent *e)
{
	if (systray_maprequest(&e->xmaprequest))
		return;

	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;

	if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
		return;
	if (!wintoclient(ev->window))
		manage(ev->window, &wa);
}

void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client *c;

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		resize(c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 0);
}

void
motionnotify(XEvent *e)
{
	if (menu_event(e))
		return;

	static Monitor *mon = NULL;
	Monitor *m;
	XMotionEvent *ev = &e->xmotion;

	if (ev->window != root)
		return;
	if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void
movemouse(const Arg *arg)
{
	int x, y, ocx, ocy, nx, ny, x_restore_from_snap, y_restore_from_snap, restore_from_snap = 0;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support moving fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;

    int c_right_edge = c->x + c->w;
    int screen_right_edge = selmon->wx + selmon->ww - cborderpx * 2;
    // selmon->wx is the left edge of the screen, not necessarily 0
    // selmon->wx + selmon->ww is the right edge of the screen
    // the - 4, don't understand what happens, when snapped to the right, if
    // whatever window is created, the right edge of the snapped window will
    // be 4px less than screen right edge... I guess universe is expanding
    if ((c->x == selmon->wx || c_right_edge == screen_right_edge || c_right_edge == screen_right_edge - 4) && c->y_before_snap != 0)
    {
        x_restore_from_snap = x - c->w_before_snap / 2;
        y_restore_from_snap = y - c->h_before_snap / 2;
        resizeclient(c, x_restore_from_snap, y_restore_from_snap, c->w_before_snap, c->h_before_snap);
        clear_client_size_pos_data(c);
        restore_from_snap = 1;
        c->isinsnap = 0;
    }

	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

            if (restore_from_snap)
            {
                nx = x_restore_from_snap + (ev.xmotion.x - x);
                ny = y_restore_from_snap + (ev.xmotion.y - y);
            }
            else
            {
                nx = ocx + (ev.xmotion.x - x);
                ny = ocy + (ev.xmotion.y - y);
            }
			if (abs(selmon->wx - nx) < snap)
				nx = selmon->wx;
			else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snap)
				nx = selmon->wx + selmon->ww - WIDTH(c);
			if (abs(selmon->wy - ny) < snap)
				ny = selmon->wy;
			else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snap)
				ny = selmon->wy + selmon->wh - HEIGHT(c);
			if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
			&& (abs(nx - c->x) > snap || abs(ny - c->y) > snap))
				togglefloating(NULL);
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, c->w, c->h, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}

	if (c->x < selmon->wx - 20 && c->x > selmon->wx -100)
        snap2left();

    c_right_edge = c->x + c->w;
	if (c_right_edge > selmon->wx + selmon->ww + 20 && c_right_edge < selmon->wx + selmon->ww + 100)
        snap2right();
}

Client *
nexttiled(Client *c)
{
	for (; c && (c->isfloating || !ISVISIBLE(c)); c = c->next);
	return c;
}

void
pop(Client *c)
{
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

void
propertynotify(XEvent *e)
{
	if (systray_propertynotify(&e->xproperty))
		return;

	Client *c;
	Window trans;
	XPropertyEvent *ev = &e->xproperty;

	if ((ev->window == root) && (ev->atom == edwmatom[EdwmCommand]))
		runcommand();
	else if ((ev->window == root) && (ev->atom == XA_WM_NAME))
		updatestatus();
	else if (ev->state == PropertyDelete)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch(ev->atom) {
		default: break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans)) &&
				(c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XA_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			drawbars();
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				drawbar(c->mon);
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
	}
}

void
quit(const Arg *arg)
{
	running = 0;
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r = selmon;
	int a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r = m;
		}
	return r;
}

void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	c->oldx = c->x; c->x = wc.x = x;
	c->oldy = c->y; c->y = wc.y = y;
	c->oldw = c->w; c->w = wc.width = w;
	c->oldh = c->h; c->h = wc.height = h;
	wc.border_width = c->bw;
	XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
}

void
resizemouse(const Arg *arg)
{
	int ocx, ocy, ow, oh, nw, nh, nx, ny, px, py, left, top;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support resizing fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	ow = c->w;
	oh = c->h;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&px, &py)) {
		XUngrabPointer(dpy, CurrentTime);
		return;
	}
	/* pointer quadrant picks the dragged corner; the opposite corner anchors */
	left = px < ocx + WIDTH(c) / 2;
	top = py < ocy + HEIGHT(c) / 2;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			if (left) {
				nx = ocx + (ev.xmotion.x - px);
				nw = ow - (ev.xmotion.x - px);
				if (nw < 1) {
					nw = 1;
					nx = ocx + ow - nw;          /* pin to anchored right edge */
				}
			} else {
				nx = ocx;
				nw = MAX(ow + (ev.xmotion.x - px), 1);
			}
			if (top) {
				ny = ocy + (ev.xmotion.y - py);
				nh = oh - (ev.xmotion.y - py);
				if (nh < 1) {
					nh = 1;
					ny = ocy + oh - nh;          /* pin to anchored bottom edge */
				}
			} else {
				ny = ocy;
				nh = MAX(oh + (ev.xmotion.y - py), 1);
			}
			if (c->mon->wx + nw >= selmon->wx && c->mon->wx + nw <= selmon->wx + selmon->ww
			&& c->mon->wy + nh >= selmon->wy && c->mon->wy + nh <= selmon->wy + selmon->wh)
			{
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
				&& (abs(nw - c->w) > snap || abs(nh - c->h) > snap))
					togglefloating(NULL);
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, nw, nh, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

void
restack(Monitor *m)
{
	Client *c;
	XEvent ev;
	XWindowChanges wc;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange)
		XRaiseWindow(dpy, m->sel->win);

    // raise the aot window and the staylow stack (task view on top)
    for (c = selmon->clients; c; c = c->next) {
        if (c->tags == selmon->tagset[selmon->seltags] && (c->isalwaysontop || c->isstaylow))
            XRaiseWindow(dpy, c->win);
    }

	if (m->lt[m->sellt]->arrange) {
		wc.stack_mode = Below;
		wc.sibling = m->barwin;
		for (c = m->stack; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c)) {
				XConfigureWindow(dpy, c->win, CWSibling|CWStackMode, &wc);
				wc.sibling = c->win;
			}
	}
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
}

void
evsrc_add(int fd, void (*cb)(void))
{
	if (fd < 0)
		return;
	if (nevfd >= MAXEVFD) {
		fprintf(stderr, "edwm: too many event sources, ignoring fd %d\n", fd);
		return;
	}
	evpfd[nevfd].fd = fd;
	evpfd[nevfd].events = POLLIN;
	evpfd[nevfd].revents = 0;
	evcb[nevfd] = cb;
	nevfd++;
}

void
evsrc_del(int fd)
{
	int i;

	for (i = 0; i < nevfd; i++)
		if (evpfd[i].fd == fd) {
			memmove(&evpfd[i], &evpfd[i + 1], (nevfd - i - 1) * sizeof evpfd[0]);
			memmove(&evcb[i], &evcb[i + 1], (nevfd - i - 1) * sizeof evcb[0]);
			nevfd--;
			return;
		}
}

/* Deliver signals through a descriptor instead of a handler, so they are
 * dispatched in normal context alongside every other event source and cannot
 * interrupt Xlib mid-request. */
/* Runs in signal context, so it does the one thing that is safe there. */
static void
sigwriter(int sig)
{
	unsigned char c = (unsigned char)sig;
	int saved = errno;

	if (write(sigpipe[1], &c, 1) < 0)
		; /* a full pipe already means a wakeup is pending */
	errno = saved;
}

void
setupsignals(void)
{
	struct sigaction sa;
	int i;

	if (pipe(sigpipe) < 0) {
		fprintf(stderr, "edwm: warning: pipe failed, signals not handled\n");
		return;
	}
	for (i = 0; i < 2; i++) {
		/* non-blocking so neither end can ever stall the loop or the handler,
		 * close-on-exec so children do not inherit them */
		fcntl(sigpipe[i], F_SETFL, fcntl(sigpipe[i], F_GETFL) | O_NONBLOCK);
		fcntl(sigpipe[i], F_SETFD, FD_CLOEXEC);
	}

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sa.sa_handler = sigwriter;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	evsrc_add(sigpipe[0], handlesignals);
}

void
handlesignals(void)
{
	unsigned char c;

	while (read(sigpipe[0], &c, 1) == 1) {
		switch (c) {
		case SIGHUP:
			reloadtheme();
			break;
		case SIGINT:
		case SIGTERM:
			running = 0;
			break;
		}
	}
}

/* The X connection becoming readable needs no work of its own: the drain at
 * the top of run() picks the events up. */
void
handlexfd(void)
{
}

void
run(void)
{
	XEvent ev;
	int i, n, timeout;
	long left;

	/* main event loop */
	XSync(dpy, False);
	while (running) {
		/* Xlib queues events in user space and poll() reports readability of
		 * the socket only, so anything already queued would never wake us.
		 * Drain the queue before sleeping. */
		while (running && XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (handler[ev.type])
				handler[ev.type](&ev); /* call handler */
		}
		if (!running)
			break;

		/* libdbus buffers messages in user space too, so drain it here rather
		 * than only when poll reports the socket readable. */
		dbusif_pump();
		dbusif_retry();
		if (dbusfd != dbusif_fd()) { /* the fd changes across a reconnect */
			if (dbusfd >= 0)
				evsrc_del(dbusfd);
			if ((dbusfd = dbusif_fd()) >= 0)
				evsrc_add(dbusfd, handledbus);
		}

		timeout = -1; /* nothing pending: sleep until something happens */
		if ((n = dbusif_timeout()) >= 0)
			timeout = n; /* wake to retry the bus */
		if (chordwin) {
			/* a press is on hold: never block past its deadline */
			left = chorddeadline - nowms();
			if (left <= 0) {
				chordreplay();
				continue;
			}
			if (timeout < 0 || (int)left < timeout)
				timeout = (int)left;
		}

		XFlush(dpy); /* nothing reaches the server while we are asleep */
		if ((n = poll(evpfd, nevfd, timeout)) < 0) {
			if (errno == EINTR)
				continue;
			die("edwm: poll:");
		}
		if (n == 0 && chordwin)
			chordreplay(); /* the chord deadline expired */
		for (i = 0; i < nevfd; i++)
			if (evpfd[i].revents && evcb[i])
				evcb[i]();
	}
}

void
scan(void)
{
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa)
			|| wa.override_redirect || XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (!XGetWindowAttributes(dpy, wins[i], &wa))
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1)
			&& (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

void
sendmon(Client *c, Monitor *m)
{
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon = m;
	c->tags = m->tagset[m->seltags]; /* assign tags of target monitor */
	attach(c);
	attachstack(c);
	focus(NULL);
	arrange(NULL);
}

void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
		PropModeReplace, (unsigned char *)data, 2);
}

int
sendevent(Client *c, Atom proto)
{
	int n;
	Atom *protocols;
	int exists = 0;
	XEvent ev;

	if (XGetWMProtocols(dpy, c->win, &protocols, &n)) {
		while (!exists && n--)
			exists = protocols[n] == proto;
		XFree(protocols);
	}
	if (exists) {
		ev.type = ClientMessage;
		ev.xclient.window = c->win;
		ev.xclient.message_type = wmatom[WMProtocols];
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = proto;
		ev.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	}
	return exists;
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow],
			XA_WINDOW, 32, PropModeReplace,
			(unsigned char *) &(c->win), 1);
	}
	sendevent(c, wmatom[WMTakeFocus]);
}

void
setfullscreen(Client *c, int fullscreen)
{
	if (fullscreen && !c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)&netatom[NetWMFullscreen], 1);
		c->isfullscreen = 1;
		c->oldstate = c->isfloating;
		c->oldbw = c->bw;
		c->bw = 0;
		c->isfloating = 1;
		resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
		XRaiseWindow(dpy, c->win);
	} else if (!fullscreen && c->isfullscreen){
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)0, 0);
		c->isfullscreen = 0;
		c->isfloating = c->oldstate;
		c->bw = c->oldbw;
		c->x = c->oldx;
		c->y = c->oldy;
		c->w = c->oldw;
		c->h = c->oldh;
		resizeclient(c, c->x, c->y, c->w, c->h);
		arrange(c->mon);
	}
}

void
entertaskview()
{
    // only 1 or no client, no need to enter task view
    if (!selmon->sel) return;

    Client *c;
    int client_count = 0x0;
    for (c = selmon->clients; c; c = c->next) {
        if (c->tags == selmon->tagset[selmon->seltags])
            client_count += 1;
    }

    if (client_count == 0x1) return;

    if (selmon->isinsnap)
        snapsidebyside();

    selmon->istaskview = 0x1;
    selmon->taskview_tag = selmon->sel->tags;
    for (c = selmon->clients; c; c = c->next) {
        if (c->tags == selmon->tagset[selmon->seltags]) {
            // deal with more than 5 windows
            // which means by default, when master area only has 1 window
            // slave area will be stacked by 4 windows, that's the maximum
            // number of windows I want for slave area, because too many
            // is going to make it hard to see. So when client count exceeds
            // 6, let's increase master area by number of client count - default
            // maximum client windows (4).

            // leavestaylow for staylow windows first
            if (c->isstaylow)
                leavestaylow(c);

            c->oldstate = c->isfloating;
            c->isfloating = 0x0;
        }
    }

    if (client_count > 0x3) {
        Arg arg;
        if (client_count % 2 == 0x0) {
            // even number
            arg.i = client_count / 2 - 1;
        } else {
            // odd number
            arg.i = (client_count - 1) / 2 - 1;
        }
        incnmaster(&arg);
    }
    arrange(selmon);
}

void
leavetaskview()
{
    if (!selmon->istaskview) return;
    Client *c;
    selmon->istaskview = 0x0;
    selmon->taskview_tag = 0x0;
    for (c = selmon->clients; c; c = c->next) {
        if (c->tags == selmon->tagset[selmon->seltags]) {
            c->isfloating = 1;
            
            resizeclient(c, c->oldx, c->oldy, c->oldw, c->oldh);
            if (c == selmon->sel)
                focus(c);
        }
    }
    selmon->nmaster = 1;
    arrange(selmon);
}

void
clear_client_size_pos_data(Client *c)
{
    if (!c) return;

    c->x_before_snap = 0x0;
    c->y_before_snap = 0x0;
    c->w_before_snap = 0x0;
    c->h_before_snap = 0x0;
}

void
save_client_size_pos_data(Client* c)
{
    if (!c) return;

    c->x_before_snap = c->x;
    c->y_before_snap = c->y;
    c->w_before_snap = c->w;
    c->h_before_snap = c->h;
}

void
snapsidebyside()
{
    Client* c;
    Client* csn;
    c = selmon->sel;
    if (!c) return;
    csn = c->snext;
    // safe harness
    // if only one client, or any of the window is in full screen mode, return
    if (!c || !csn || csn->tags != c->tags) {
        // exit snap mode
        selmon->isinsnap = 0;
        drawbar(selmon);
        return;
    }

    if (selmon->istaskview)
        // if current in task view
        // exit task view
        leavetaskview();

    if (selmon->isinsnap) {
        selmon->isinsnap = 0;
        selmon->snap_tag = 0x0;
        if (c->w_before_snap != 0 && c->h_before_snap != 0)
            resizeclient(c, c->x_before_snap, c->y_before_snap, c->w_before_snap, c->h_before_snap);
        if (csn->w_before_snap != 0 && csn->h_before_snap != 0)
            resizeclient(csn, csn->x_before_snap, csn->y_before_snap, csn->w_before_snap, csn->h_before_snap);
        clear_client_size_pos_data(c);
        clear_client_size_pos_data(csn);
        c->isinsnap = 0;
        csn->isinsnap = 0;
    } else if (!selmon->isinsnap) {
        selmon->isinsnap = 1;
        selmon->snap_tag = c->tags;

        save_client_size_pos_data(c);
        save_client_size_pos_data(csn);

        int w = (selmon->mw - cborderpx * 2) / 2;
        int h = selmon->mh - bh - cborderpx * 2;
        resizeclient(c, selmon->mx, selmon->my + bh, w, h);
        resizeclient(csn, selmon->mx + w, selmon->my + bh, w, h);
        c->isinsnap = 1;
        csn->isinsnap = 1;
    }
    drawbar(selmon);
}

void
swapsidebyside()
{
    if (!selmon) return;
    Client *c = selmon->sel;

    // only works in snap mode
    if (!c || !c->snext || !selmon->isinsnap) return;
    // swap two clients side by side
    Client *csn = c->snext;

    int w = (selmon->mw - 2 * cborderpx) / 2;
    int h = selmon->mh - bh - 2 * cborderpx;

    if (selmon->issnapinitstate) {
        selmon->issnapinitstate = 0;
        resizeclient(c, selmon->mx, selmon->my + bh, w, h);
        resizeclient(csn, selmon->mx + w, selmon->my + bh, w, h);
    } else {
        selmon->issnapinitstate = 1;
        resizeclient(c, selmon->mx + w, selmon->my + bh, w, h);
        resizeclient(csn, selmon->mx, selmon->my + bh, w, h);
    }
}

void
snap2left()
{
    Client *c = selmon->sel;
    if (!c) return;
    if (selmon->isinsnap) return;

    save_client_size_pos_data(c);

    int w = (selmon->mw - 2 * cborderpx) / 2 - cborderpx * 2;
    int h = selmon->mh - bh - 2 * cborderpx;
    resizeclient(c, selmon->mx, selmon->my + bh, w, h);
    c->isinsnap = 1;
}

void
snap2right()
{
    Client *c = selmon->sel;
    if (!c) return;
    if (selmon->isinsnap) return;

    save_client_size_pos_data(c);

    int w = (selmon->mw - 2 * cborderpx) / 2;
    int h = selmon->mh - bh - 2 * cborderpx;
    resizeclient(c, selmon->mx + w, selmon->my + bh, w, h);
    c->isinsnap = 1;
}

void
adjustwidth(const Arg* arg)
{
    Client* c = selmon->sel;
    if (!c) return;
    Client* cn = c->snext;
    if (!cn) return;
    if (!c->isinsnap || !cn->isinsnap) return;

    int offset = arg->i;

    if (c->x == selmon->wx)
    {
        resizeclient(c, c->x, c->y, c->w + offset, c->h);
        resizeclient(cn, cn->x + offset, cn->y, cn->w + (~offset + 1), cn->h);
    }
    else
    {
        resizeclient(c, c->x + ~offset + 1, c->y, c->w + offset, c->h);
        resizeclient(cn, cn->x, cn->y, cn->w + ~offset + 1, cn->h);
    }
}

/* keyboard move by dx/dy; unsnaps at current geometry (drops pre-snap memory,
 * unlike mouse drag which restores it), auto-floats if tiled, clamps to the
 * monitor work area */
void
keymove(int dx, int dy)
{
	Client *c = selmon->sel;
	Monitor *m;
	int nx, ny;

	if (!c || c->isfullscreen)
		return;
	if (c->isinsnap) {
		clear_client_size_pos_data(c);
		c->isinsnap = 0;
	}
	if (!c->isfloating && selmon->lt[selmon->sellt]->arrange)
		togglefloating(NULL);
	if (!selmon->lt[selmon->sellt]->arrange && !c->isfloating)
		return;
	m = c->mon;
	nx = MAX(m->wx, MIN(c->x + dx, m->wx + m->ww - WIDTH(c)));
	ny = MAX(m->wy, MIN(c->y + dy, m->wy + m->wh - HEIGHT(c)));
	resizeclient(c, nx, ny, c->w, c->h);
}

/* keyboard resize by dw/dh, top-left corner anchored, clamped to the monitor
 * work area; resize() applies size hints */
void
keyresize(int dw, int dh)
{
	Client *c = selmon->sel;
	Monitor *m;
	int nw, nh;

	if (!c || c->isfullscreen)
		return;
	if (c->isinsnap) {
		clear_client_size_pos_data(c);
		c->isinsnap = 0;
	}
	if (!c->isfloating && selmon->lt[selmon->sellt]->arrange)
		togglefloating(NULL);
	if (!selmon->lt[selmon->sellt]->arrange && !c->isfloating)
		return;
	m = c->mon;
	nw = MAX(MIN(c->w + dw, m->wx + m->ww - c->x - 2 * c->bw), 1);
	nh = MAX(MIN(c->h + dh, m->wy + m->wh - c->y - 2 * c->bw), 1);
	resize(c, c->x, c->y, nw, nh, 1);
}

/* wrappers for keys[]: only the sign of arg->i is used, so the j/k bindings
 * can carry +/-15 through to the adjustwidth fallback */
void
movex(const Arg* arg)
{
	keymove(arg->i < 0 ? -movespeed : movespeed, 0);
}

void
movey(const Arg* arg)
{
	Client *c = selmon->sel;

	/* two windows snapped side-by-side (Alt+e state): j/k adjust the pair */
	if (c && c->snext && c->isinsnap && c->snext->isinsnap) {
		adjustwidth(arg);
		return;
	}
	keymove(0, arg->i < 0 ? -movespeed : movespeed);
}

void
resizew(const Arg* arg)
{
	keyresize(arg->i < 0 ? -resizespeed : resizespeed, 0);
}

void
resizeh(const Arg* arg)
{
	keyresize(0, arg->i < 0 ? -resizespeed : resizespeed);
}

void
setlayout(const Arg *arg)
{
    if (!selmon->sel) return;

    if (!selmon->istaskview)
        entertaskview();
    else
        leavetaskview();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

/* Choose the visual everything we draw on will use. A 32-bit TrueColor visual
 * lets the bar carry an alpha channel, but only a running compositor gives that
 * alpha any meaning -- without one an ARGB bar renders as garbage. So it is
 * opt-in via useargb in config.h, and we fall back silently when the server has
 * no such visual. */
void
setupvisual(void)
{
	XVisualInfo vi;

	if (useargb && XMatchVisualInfo(dpy, screen, 32, TrueColor, &vi)) {
		visual = vi.visual;
		depth = vi.depth;
		cmap = XCreateColormap(dpy, root, visual, AllocNone);
		return;
	}
	visual = DefaultVisual(dpy, screen);
	depth = DefaultDepth(dpy, screen);
	cmap = DefaultColormap(dpy, screen);
}

/* Install `t` into the running WM. Colours are allocated into a fresh set
 * first, so a theme that fails to allocate leaves the previous one running
 * untouched -- reloading a broken file must never leave the bar unreadable or
 * take the WM down. */
int
applytheme(const Theme *t)
{
	Clr **new, **old;
	XRenderColor rc;
	ThemeColor tc;
	Fnt *oldfonts = NULL;
	const char *fspec[2];
	Client *c;
	Monitor *m;
	int i, j, nf = 0, oldbh = bh, built = 0;

	new = ecalloc(SchemeLast, sizeof(Clr *));
	for (i = 0; i < SchemeLast; i++) {
		new[i] = ecalloc(THEME_NCOLOR, sizeof(Clr));
		for (j = 0; j < THEME_NCOLOR; j++) {
			tc = t->scm[i][j];
			/* XRenderColor channels are premultiplied by alpha */
			rc.red   = (unsigned short)((unsigned long)tc.r * tc.a / 65535);
			rc.green = (unsigned short)((unsigned long)tc.g * tc.a / 65535);
			rc.blue  = (unsigned short)((unsigned long)tc.b * tc.a / 65535);
			rc.alpha = tc.a;
			if (!drw_clr_create_value(drw, &new[i][j], rc)) {
				/* roll back everything allocated so far */
				drw_scm_free(drw, new[i], j);
				new[i] = NULL;
				for (i = 0; i < built; i++)
					drw_scm_free(drw, new[i], THEME_NCOLOR);
				free(new);
				return 0;
			}
		}
		built = i + 1;
	}

	/* Fonts. drw_fontset_create assigns drw->fonts itself, so the old head has
	 * to be captured first, and restored if the new set will not load. */
	if (t->font[0]) {
		fspec[nf++] = t->font;
		if (t->fontfallback[0])
			fspec[nf++] = t->fontfallback;
		oldfonts = drw->fonts;
		if (!drw_fontset_create(drw, fspec, nf)) {
			fprintf(stderr, "edwm: theme fonts unusable, keeping the current ones\n");
			drw->fonts = oldfonts;
			oldfonts = NULL;
		} else {
			drw_fontcache_reset(drw);
		}
	}

	/* metrics */
	cborderpx = t->borderpx;
	systray_setmetrics(t->systrayiconsize, t->systrayspacing);
	sni_setmetrics(t->systrayiconsize);
	lrpad = drw->fonts->h;
	bh = drw->fonts->h + 2 + 2 * t->barpadding;

	/* An open menu borrows Clr pointers from the scheme we are about to free,
	 * so it cannot outlive the swap. */
	menu_hide();

	/* swap the schemes in, then release the old ones. drw holds a borrowed
	 * pointer into the scheme it drew with last, so repoint it first. */
	old = scheme;
	scheme = new;
	drw_setscheme(drw, scheme[SchemeNorm]);
	if (old) {
		for (i = 0; i < SchemeLast; i++)
			drw_scm_free(drw, old[i], THEME_NCOLOR);
		free(old);
	}
	if (oldfonts)
		drw_fontset_free(oldfonts);

	menu_setstyle(scheme[SchemeMenu], scheme[SchemeMenuSel], scheme[SchemeMenuDim],
	              scheme[SchemeMenu][ColBorder].pixel);

	theme = *t;
	/* keep dmenu in step with the bar */
	theme_colorstr_rgb(t->scm[SchemeNorm][ColBg], dmenu_nb, sizeof dmenu_nb);
	theme_colorstr_rgb(t->scm[SchemeNorm][ColFg], dmenu_nf, sizeof dmenu_nf);
	theme_colorstr_rgb(t->scm[SchemeSel][ColBg],  dmenu_sb, sizeof dmenu_sb);
	theme_colorstr_rgb(t->scm[SchemeSel][ColFg],  dmenu_sf, sizeof dmenu_sf);
	snprintf(dmenu_fn, sizeof dmenu_fn, "%s", t->font[0] ? t->font : dmenufont);
	dbusif_emit_theme(theme.name);

	/* Everything below needs the monitors and bars to exist, which they do not
	 * during the first call from setup(). */
	if (!mons || !mons->barwin)
		return 1;

	if (bh != oldbh) {
		drw_resize(drw, sw, bh);
		for (m = mons; m; m = m->next) {
			updatebarpos(m);
			XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, m->ww, bh);
		}
	}
	/* Borders are pushed to windows imperatively, so a redraw alone would not
	 * recolour them. */
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next) {
			if (c->bw != (int)cborderpx) {
				c->bw = cborderpx;
				XSetWindowBorderWidth(dpy, c->win, c->bw);
				configure(c);
			}
			XSetWindowBorder(dpy, c->win,
				scheme[c == selmon->sel ? SchemeSel : SchemeNorm][ColBorder].pixel);
		}
	arrange(NULL);
	drawbars();
	return 1;
}

/* Seed the theme from config.h, then let the user's file override it. */
void
inittheme(void)
{
	Theme t;

	theme_seed("bg",     colors[SchemeNorm][ColBg]);
	theme_seed("fg",     colors[SchemeNorm][ColFg]);
	theme_seed("border", colors[SchemeNorm][ColBorder]);
	theme_seed("bright", colors[SchemeSel][ColFg]);
	theme_seed("accent", colors[SchemeSel][ColBg]);
	theme_seedmetrics((int)borderpx);

	theme_defaults(&t);
	theme_parse(theme_configpath(), &t);
	if (!applytheme(&t))
		die("edwm: cannot allocate the initial theme colors");
}

void
reloadtheme(void)
{
	Theme t;

	theme_defaults(&t);
	if (!theme_parse(theme_configpath(), &t))
		return;
	if (!applytheme(&t))
		fprintf(stderr, "edwm: theme reload failed, keeping the previous theme\n");
}

/* Switch to themes/<name>.theme and remember the choice. */
void
setthemebyname(const char *name)
{
	char path[1024];
	Theme t;

	if (!theme_findnamed(name, path, sizeof path)) {
		fprintf(stderr, "edwm: no theme named '%s'\n", name);
		return;
	}
	if (!theme_persistname(name))
		return;
	/* We are about to rewrite theme.conf ourselves; skip the watch event that
	 * causes, or the theme is parsed and applied twice for one switch. The
	 * window is short so a later, genuine edit is never swallowed. */
	themeselfwrite = nowms() + 1000;
	theme_defaults(&t);
	if (!theme_parse(theme_configpath(), &t))
		return;
	if (!applytheme(&t))
		fprintf(stderr, "edwm: theme '%s' failed to apply, keeping the previous one\n", name);
}

/* Something under the config directory changed. Any change re-reads the theme:
 * parsing is cheap, and it saves us tracking which editor wrote which
 * temporary file. */
static void
themechanged(void)
{
	if (themeselfwrite && nowms() < themeselfwrite) {
		themeselfwrite = 0; /* our own edwmctl write; already applied */
		return;
	}
	themeselfwrite = 0;
	reloadtheme();
}

#if defined(EDWM_INOTIFY)

/* Watch the config *directory*, not just the file: editors replace theme.conf
 * by rename, which would leave a watch pointing at the old inode. */
void
setupthemewatch(void)
{
	if ((inotifyfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) < 0) {
		fprintf(stderr, "edwm: warning: inotify unavailable, live theme reload off\n");
		return;
	}
	themewd = inotify_add_watch(inotifyfd, theme_configdir(),
	                            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (themewd < 0) {
		close(inotifyfd);
		inotifyfd = -1;
		return; /* no config dir yet; SIGHUP and the command property still work */
	}
	evsrc_add(inotifyfd, handlethemewatch);
}

void
handlethemewatch(void)
{
	/* The union gives the buffer the alignment inotify_event needs. */
	union {
		struct inotify_event e;
		char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	} u;
	int changed = 0;

	while (read(inotifyfd, &u, sizeof u) > 0)
		changed = 1;
	if (changed)
		themechanged();
}

#elif defined(EDWM_KQUEUE)

/* kqueue watches open descriptors rather than paths, so the theme file needs
 * one of its own: a directory change is not reported for a file written in
 * place, and a file change is not reported when it is replaced by rename.
 * Watching both covers either kind of editor. */
static void
watchthemefile(void)
{
	struct kevent kev;

	if (themefilefd >= 0) {
		close(themefilefd);
		themefilefd = -1;
	}
	if ((themefilefd = open(theme_configpath(), O_RDONLY | O_CLOEXEC)) < 0)
		return; /* no theme file yet; the directory watch spots it appearing */
	EV_SET(&kev, themefilefd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
	       NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME,
	       0, NULL);
	kevent(inotifyfd, &kev, 1, NULL, 0, NULL);
}

void
setupthemewatch(void)
{
	struct kevent kev;

	if ((inotifyfd = kqueue()) < 0) {
		fprintf(stderr, "edwm: warning: kqueue unavailable, live theme reload off\n");
		return;
	}
	fcntl(inotifyfd, F_SETFD, FD_CLOEXEC);

	if ((themewd = open(theme_configdir(), O_RDONLY | O_CLOEXEC)) < 0) {
		close(inotifyfd);
		inotifyfd = -1;
		return; /* no config dir yet; SIGHUP and the command property still work */
	}
	EV_SET(&kev, themewd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
	       NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME, 0, NULL);
	kevent(inotifyfd, &kev, 1, NULL, 0, NULL);

	watchthemefile();
	evsrc_add(inotifyfd, handlethemewatch);
}

void
handlethemewatch(void)
{
	struct kevent ev[8];
	struct timespec zero = { 0, 0 };
	int n, i, changed = 0, refile = 0;

	while ((n = kevent(inotifyfd, NULL, 0, ev, 8, &zero)) > 0) {
		changed = 1;
		for (i = 0; i < n; i++) {
			/* the watched file was replaced or removed, so its descriptor now
			 * refers to something nobody will write to again */
			if ((int)ev[i].ident == themefilefd
			&& (ev[i].fflags & (NOTE_DELETE | NOTE_RENAME)))
				refile = 1;
			else if ((int)ev[i].ident == themewd)
				refile = 1; /* something appeared in the directory */
		}
		if (n < 8)
			break;
	}
	if (refile)
		watchthemefile();
	if (changed)
		themechanged();
}

#else /* no file-change notification on this platform */

void
setupthemewatch(void)
{
	fprintf(stderr, "edwm: no file watch on this platform; reload with "
	                "edwmctl reload or SIGHUP\n");
}

void
handlethemewatch(void)
{
}

#endif

/* Execute a command written to the _EDWM_CMD root property, then clear it so a
 * repeat of the same command still registers as a change. The vocabulary is
 * deliberately tiny and mirrors what the D-Bus interface will expose. */
void
runcommand(void)
{
	char cmd[512];
	char *arg;

	if (!gettextprop(root, edwmatom[EdwmCommand], cmd, sizeof cmd))
		return;
	XDeleteProperty(dpy, root, edwmatom[EdwmCommand]);
	docommand(cmd);
	(void)arg;
}

/* The one command vocabulary, shared by the _EDWM_CMD property and D-Bus so
 * the two transports cannot drift apart. Modifies `cmd` in place. */
void
docommand(char *cmd)
{
	char *arg;

	if ((arg = strchr(cmd, ' ')))
		*arg++ = '\0';
	else
		arg = cmd + strlen(cmd);

	if (!strcmp(cmd, "reload"))
		reloadtheme();
	else if (!strcmp(cmd, "theme"))
		setthemebyname(arg);
	else if (!strcmp(cmd, "tone"))
		settone(arg);
	else if (!strcmp(cmd, "quit"))
		running = 0;
	else
		fprintf(stderr, "edwm: unknown command '%s'\n", cmd);
}

/* ---- the D-Bus service's view of the WM. dbusif.c never sees a Client. ---- */

void
dbus_docommand(const char *cmd)
{
	char buf[512];

	snprintf(buf, sizeof buf, "%s", cmd);
	docommand(buf);
}

const char *
dbus_gettheme(void)
{
	return theme.name;
}

void
dbus_listclients(DbusClientSink sink, void *sinkdata)
{
	Monitor *m;
	Client *c;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			sink(sinkdata, (unsigned long)c->win, c->name, c->tags,
			     c == selmon->sel, c->isurgent, c->isstaylow);
}

void
dbus_focuswin(unsigned long win)
{
	Client *c;
	Arg a;

	if (!(c = wintoclient((Window)win)))
		return;
	if (c == selmon->sel) {
		/* focusclient() minimises a window that already has focus, which is
		 * right for a taskbar click but wrong for a programmatic request:
		 * "focus this" should never hide it. */
		restack(selmon);
		XRaiseWindow(dpy, c->win);
		return;
	}
	a.v = c;
	focusclient(&a);
}

void
dbus_closewin(unsigned long win)
{
	Client *c;
	Arg a;

	if (!(c = wintoclient((Window)win)))
		return;
	a.v = c;
	taskkill(&a);
}

void
dbus_viewtags(unsigned int tagmask)
{
	Arg a;

	a.ui = tagmask & TAGMASK;
	if (a.ui)
		view(&a);
}

void
dbus_setstatus(const char *text)
{
	snprintf(stext, sizeof stext, "%s", text);
	drawbars();
}

void
dbus_doquit(void)
{
	running = 0;
}

static const DbusOps dbusops = {
	.command     = dbus_docommand,
	.gettheme    = dbus_gettheme,
	.listclients = dbus_listclients,
	.focusclient = dbus_focuswin,
	.closeclient = dbus_closewin,
	.view        = dbus_viewtags,
	.setstatus   = dbus_setstatus,
	.quit        = dbus_doquit,
};

/* The connection becoming readable needs no work here: run() pumps it. */
void
handledbus(void)
{
}

/* "hue=+20 saturation=1.2 opacity=0.9" -- adjust the live tone without
 * touching the theme file, so it can be swept interactively. */
void
settone(const char *spec)
{
	Theme t = theme;
	char buf[256], *p, *tok, *eq;
	double v;

	snprintf(buf, sizeof buf, "%s", spec);
	for (p = buf; (tok = strtok(p, " \t")); p = NULL) {
		if (!(eq = strchr(tok, '=')))
			continue;
		*eq++ = '\0';
		v = atof(eq);
		if (!strcmp(tok, "hue"))             t.tone.hue = v;
		else if (!strcmp(tok, "saturation")) t.tone.saturation = v;
		else if (!strcmp(tok, "lightness"))  t.tone.lightness = v;
		else if (!strcmp(tok, "contrast"))   t.tone.contrast = v;
		else if (!strcmp(tok, "opacity"))    t.tone.opacity = v;
		else {
			fprintf(stderr, "edwm: unknown tone key '%s'\n", tok);
			continue;
		}
	}
	/* Re-resolve from the theme file with the new tone, so tone changes
	 * compose rather than stacking on already-toned colours. */
	if (!theme_retone(theme_configpath(), &t.tone, &t) || !applytheme(&t))
		fprintf(stderr, "edwm: could not apply tone\n");
}

void
setup(void)
{
	XSetWindowAttributes wa;
	Atom utf8string;
	struct sigaction sa;

	/* do not transform children into zombies when they terminate */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	/* clean up any zombies (inherited from .xinitrc etc) immediately */
	while (waitpid(-1, NULL, WNOHANG) > 0);

	/* init screen */
	screen = DefaultScreen(dpy);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	root = RootWindow(dpy, screen);
	setupvisual();
	drw = drw_create(dpy, screen, root, sw, sh, visual, depth, cmap);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	bh = drw->fonts->h + 2;
	inittheme(); /* may replace the fontset and so change bh/lrpad */
	updategeom();
	/* init atoms */
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	edwmatom[EdwmCommand] = XInternAtom(dpy, "_EDWM_CMD", False);
	edwmatom[EdwmTheme] = XInternAtom(dpy, "_EDWM_THEME", False);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_sizing);
	cursor[CurMove] = drw_cur_create(drw, XC_fleur);
	/* appearance is installed by inittheme() above */
	/* init bars */
	updatebars();
	updatestatus();
	if (systray_init(dpy, screen, root, systraychanged))
		systray_setmetrics(theme.systrayiconsize, theme.systrayspacing);
	sni_init(systraychanged, drw);
	dbusif_init(&dbusops);
	if ((dbusfd = dbusif_fd()) >= 0)
		evsrc_add(dbusfd, handledbus);
	/* supporting window for NetWMCheck */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
		PropModeReplace, (unsigned char *) "dwm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	/* EWMH support per view */
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
		PropModeReplace, (unsigned char *) netatom, NetLast);
	XDeleteProperty(dpy, root, netatom[NetClientList]);
	/* select events */
	wa.cursor = cursor[CurNormal]->cursor;
	wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
		|ButtonPressMask|PointerMotionMask|EnterWindowMask
		|LeaveWindowMask|StructureNotifyMask|PropertyChangeMask;
	XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa);
	XSelectInput(dpy, root, wa.event_mask);
	grabkeys();
	evsrc_add(ConnectionNumber(dpy), handlexfd);
	setupsignals();
	setupthemewatch();
	/* XInput2 raw events for the chord drag. Raw events are delivered
	 * regardless of pointer grabs, which the drag needs: the app under the
	 * pointer holds X's automatic grab from the first button press. Raw
	 * events may only be selected on the root window. */
	{
		int xiev, xierr, ximaj = 2, ximin = 0;
		unsigned char mask[XIMaskLen(XI_RawMotion)] = {0};
		XIEventMask em;

		if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &xiev, &xierr)) {
			xi_opcode = -1;
		} else {
			XIQueryVersion(dpy, &ximaj, &ximin);
			XISetMask(mask, XI_RawButtonPress);
			XISetMask(mask, XI_RawButtonRelease);
			XISetMask(mask, XI_RawMotion);
			em.deviceid = XIAllDevices;
			em.mask_len = sizeof(mask);
			em.mask = mask;
			XSync(dpy, False);
			xi_sel_err = 0;
			XSetErrorHandler(xerrorxi);
			XISelectEvents(dpy, root, &em, 1);
			XSync(dpy, False);
			XSetErrorHandler(xerror);
			if (xi_sel_err) {
				xi_opcode = -1;
				fprintf(stderr, "dwm: warning: XI2 raw event selection failed, chord drag disabled\n");
			} else
				xi_ok = 1; /* chord hold may be enabled (see buttonpress) */
		}
	}
	focus(NULL);
}

void
seturgent(Client *c, int urg)
{
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		/* show clients top down */
		XMoveWindow(dpy, c->win, c->x, c->y);
		if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) && !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		/* hide clients bottom up */
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

/* Undo the signal setup a child inherits from the WM. execvp() resets caught
 * signals to their default itself, so only SIG_IGN needs undoing -- but a
 * child that is forked and not yet exec'd would otherwise still ignore
 * SIGCHLD, and anything it spawns in turn would inherit that. */
void
childsignals(void)
{
	struct sigaction sa;
	sigset_t mask;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* nothing blocks these any more, but clear the mask regardless so a child
	 * is never at the mercy of what we happened to be doing */
	sigemptyset(&mask);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

void
spawn(const Arg *arg)
{
	if (arg->v == dmenucmd)
		dmenumon[0] = '0' + selmon->num;
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		childsignals();

		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwm: execvp '%s' failed:", ((char **)arg->v)[0]);
	}
}

void
tag(const Arg *arg)
{
	if (selmon->sel && arg->ui & TAGMASK) {
		selmon->sel->tags = arg->ui & TAGMASK;
		focus(NULL);
		arrange(selmon);
	}
}

void
tagmon(const Arg *arg)
{
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
		mw = m->ww;
	for (i = my = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i);
			resize(c, m->wx, m->wy + my, mw - (2*c->bw), h - (2*c->bw), 0);
			if (my + HEIGHT(c) < m->wh)
				my += HEIGHT(c);
		} else {
			h = (m->wh - ty) / (n - i);
			resize(c, m->wx + mw, m->wy + ty, m->ww - mw - (2*c->bw), h - (2*c->bw), 0);
			if (ty + HEIGHT(c) < m->wh)
				ty += HEIGHT(c);
		}
}

void
togglebar(const Arg *arg)
{
	selmon->showbar = !selmon->showbar;
	updatebarpos(selmon);
	XMoveResizeWindow(dpy, selmon->barwin, selmon->wx, selmon->by, selmon->ww, bh);
	arrange(selmon);
}

void
togglefloating(const Arg *arg)
{
	if (!selmon->sel)
		return;
	if (selmon->sel->isfullscreen) /* no support for fullscreen windows */
		return;
	selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
    if (selmon->sel->isfloating)
        resize(selmon->sel, selmon->sel->x, selmon->sel->y,
            selmon->sel->w, selmon->sel->h, 0);
    else
        selmon->sel->isalwaysontop = 0;
    arrange(selmon);
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		focus(NULL);
		arrange(selmon);
	}
}

void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focus(NULL);
		arrange(selmon);
	}
}

void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = c->mon;
	XWindowChanges wc;

	if (c->isterm)
		xtermcount = MAX(xtermcount - 1, 0);
	if (c->win == chordwin)
		chordwin = None; /* held press client went away */
	detach(c);
	detachstack(c);
	if (c->isstaylow)
		relayoutstaylow(); /* close the gap left by the closed window */
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy); /* avoid race conditions */
		XSetErrorHandler(xerrordummy);
		XSelectInput(dpy, c->win, NoEventMask);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc); /* restore border */
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);
	focus(NULL);
	updateclientlist();
	arrange(m);
}

void
unmapnotify(XEvent *e)
{
	Client *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	}
}

void
updatebars(void)
{
	Monitor *m;
	XSetWindowAttributes wa = {
		.override_redirect = True,
		/* ParentRelative is invalid when our depth differs from the root's,
		 * so an ARGB bar paints its own background instead. */
		.background_pixel = 0,
		.border_pixel = 0,
		.colormap = cmap,
		.event_mask = ButtonPressMask|ExposureMask
	};
	unsigned long valmask = CWOverrideRedirect|CWEventMask
		|(depth == DefaultDepth(dpy, screen) ? CWBackPixmap : CWBackPixel|CWBorderPixel|CWColormap);

	if (depth == DefaultDepth(dpy, screen))
		wa.background_pixmap = ParentRelative;
	XClassHint ch = {"dwm", "dwm"};
	for (m = mons; m; m = m->next) {
		if (m->barwin)
			continue;
		m->barwin = XCreateWindow(dpy, root, m->wx, m->by, m->ww, bh, 0, depth,
				InputOutput, visual, valmask, &wa);
		XDefineCursor(dpy, m->barwin, cursor[CurNormal]->cursor);
		XMapRaised(dpy, m->barwin);
		XSetClassHint(dpy, m->barwin, &ch);
	}
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + bh : m->wy;
	} else
		m->by = -bh;
}

void
updateclientlist(void)
{
	Client *c;
	dbusif_emit_clients();
	Monitor *m;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			XChangeProperty(dpy, root, netatom[NetClientList],
				XA_WINDOW, 32, PropModeAppend,
				(unsigned char *) &(c->win), 1);
}

int
updategeom(void)
{
	int dirty = 0;

#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int i, j, n, nn;
		Client *c;
		Monitor *m;
		XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
		XineramaScreenInfo *unique = NULL;

		for (n = 0, m = mons; m; m = m->next, n++);
		/* only consider unique geometries as separate screens */
		unique = ecalloc(nn, sizeof(XineramaScreenInfo));
		for (i = 0, j = 0; i < nn; i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
		XFree(info);
		nn = j;

		/* new monitors if nn > n */
		for (i = n; i < nn; i++) {
            for (m = mons; m && m->next; m = m->next);
            if (m)
                m->next = createmon();
			else
				mons = createmon();
		}
		for (i = 0, m = mons; i < nn && m; m = m->next, i++)
			if (i >= n
			|| unique[i].x_org != m->mx || unique[i].y_org != m->my
			|| unique[i].width != m->mw || unique[i].height != m->mh)
			{
				dirty = 1;
				m->num = i;
				m->mx = m->wx = unique[i].x_org;
				m->my = m->wy = unique[i].y_org;
				m->mw = m->ww = unique[i].width;
				m->mh = m->wh = unique[i].height;
				updatebarpos(m);
			}
		/* removed monitors if n > nn */
		for (i = nn; i < n; i++) {
			for (m = mons; m && m->next; m = m->next);
			while ((c = m->clients)) {
				dirty = 1;
				m->clients = c->next;
				detachstack(c);
				c->mon = mons;
				attach(c);
				attachstack(c);
			}
			if (m == selmon)
				selmon = mons;
			cleanupmon(m);
		}
		free(unique);
	} else
#endif /* XINERAMA */
	{ /* default monitor setup */
		if (!mons)
			mons = createmon();
		if (mons->mw != sw || mons->mh != sh) {
			dirty = 1;
			mons->mw = mons->ww = sw;
			mons->mh = mons->wh = sh;
			updatebarpos(mons);
		}
	}
	if (dirty) {
		selmon = mons;
		selmon = wintomon(root);
	}
	return dirty;
}

void
updatenumlockmask(void)
{
	unsigned int i, j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap = XGetModifierMapping(dpy);
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j]
				== XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1 << i);
	XFreeModifiermap(modmap);
}

void
updatesizehints(Client *c)
{
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		/* size is uninitialized, ensure that size.flags aren't used */
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float)size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
	c->hintsvalid = 1;
}

void
updatestatus(void)
{
	if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
		strcpy(stext, "dwm-"VERSION);
	/* drawbar(selmon); */
    /* now we drawbars on all monitors */
    drawbars();
}

void
updatetitle(Client *c)
{
	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0') /* hack to mark broken clients */
		strcpy(c->name, broken);
}

void
updatewindowtype(Client *c)
{
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog])
		c->isfloating = 1;
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void
view(const Arg *arg)
{
    unsigned cur_tag = arg->ui & TAGMASK;
	if (cur_tag == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (cur_tag)
    {
        selmon->tagset[selmon->seltags] = cur_tag;
        selmon->cur_tag = cur_tag;
    }
	focus(NULL);
	arrange(selmon);
}

void
doswitchclient(const Arg *arg)
{
    Client *c = selmon->sel;
    if (!c) return;
    if (!c->switch_next) return;
    focus(c->switch_next);
    restack(selmon);
}

Client *
wintoclient(Window w)
{
	Client *c;
	Monitor *m;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->win == w)
				return c;
	return NULL;
}

Monitor *
wintomon(Window w)
{
	int x, y;
	Client *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return selmon;
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's). Other types of errors call Xlibs
 * default error handler, which may call exit. */
int
xerror(Display *dpy, XErrorEvent *ee)
{
	if (ee->error_code == BadWindow
	|| (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	|| (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
	|| (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	|| (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	|| (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	|| (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "dwm: fatal error: request code=%d, error code=%d\n",
		ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee); /* may call exit */
}

int
xerrordummy(Display *dpy, XErrorEvent *ee)
{
	return 0;
}

/* error handler for the one-time XI2 raw event selection in setup() */
int
xerrorxi(Display *dpy, XErrorEvent *ee)
{
	xi_sel_err = 1;
	return 0;
}

/* Startup Error handler to check if another window manager
 * is already running. */
int
xerrorstart(Display *dpy, XErrorEvent *ee)
{
	die("dwm: another window manager is already running");
	return -1;
}

void
zoom(const Arg *arg)
{
	Client *c = selmon->sel;

	if (!selmon->lt[selmon->sellt]->arrange || !c || c->isfloating)
		return;
	if (c == nexttiled(selmon->clients) && !(c = nexttiled(c->next)))
		return;
	pop(c);
}

void leavestaylow(Client *c)
{
    resizeclient(c, c->x_before_staylow, c->y_before_staylow,
            c->w_before_staylow, c->h_before_staylow);
    c->isstaylow = 0;
    relayoutstaylow(); /* slide the windows above into the gap */
    restack(selmon); /* re-raise the remaining staylow stack */
}

void
enterstaylow(Client *c)
{
    Client *cc;

    /* recount so the new window lands on top of the real stack */
    staylowcount = 0;
    for (cc = selmon->clients; cc; cc = cc->next)
        if (cc->isstaylow)
            staylowcount++;
    c->x_before_staylow = c->x;
    c->y_before_staylow = c->y;
    c->w_before_staylow = c->w;
    c->h_before_staylow = c->h;
    resizeclient(c,
            selmon->wx + (selmon->ww - loww - 2 * cborderpx - offsetx),
            selmon->wy + (selmon->wh - lowh - 2 * cborderpx - offsety - staylowcount * (lowh + offsetoverlap)),
            loww,
            lowh);
    focus(c->snext);
    c->isstaylow = 1;
    staylowcount += 1;
    restack(selmon); /* staylow windows stay on top of everything */
}

/* recompact the staylow stack: the bottom-most window sits at the initial
 * staylow position (bottom right), the others stack on top of it in their
 * current order, so removing one window lets the ones above slide down
 * into the gap with no empty space in between */
void
relayoutstaylow(void)
{
    Client *stack[64], *cc, *tmp;
    int n = 0, i, j;

    for (cc = selmon->clients; cc && n < 64; cc = cc->next)
        if (cc->isstaylow)
            stack[n++] = cc;
    staylowcount = n; /* keep the count in sync with the real stack */
    if (n < 1)
        return;
    /* sort bottom-most first (largest y first) */
    for (i = 1; i < n; i++) {
        tmp = stack[i];
        for (j = i - 1; j >= 0 && stack[j]->y < tmp->y; j--)
            stack[j + 1] = stack[j];
        stack[j + 1] = tmp;
    }
    for (i = 0; i < n; i++)
        resizeclient(stack[i],
                selmon->wx + (selmon->ww - loww - 2 * cborderpx - offsetx),
                selmon->wy + (selmon->wh - lowh - 2 * cborderpx - offsety - i * (lowh + offsetoverlap)),
                loww,
                lowh);
}

void
raisetotop(const Arg *arg)
{
    Client *c = selmon->sel;
    if (!c)
        return;
    if (c->isfullscreen)
        return;

    if(c->isalwaysontop){
        c->isalwaysontop = 0;
    }else{
        // raise the aot window
        for (Client *cc = selmon->clients; cc; cc = cc->next) {
            if (cc->tags == selmon->tagset[selmon->seltags] && cc->isalwaysontop)
                cc->isalwaysontop = 0;
        }
        c->isalwaysontop = 1;
    }
    arrange(selmon);
}

/* minimize window to lower right corner */
void
staylow(const Arg *arg)
{
    Client *c = selmon->sel;
    if (!c)
        return;
    if (c->isstaylow)
        leavestaylow(c);
    else
        enterstaylow(c);
}

void
togglefullscreen(const Arg *arg)
{
    Client *c = selmon->sel;
    if (!c) return;
    if (!c->issetfullscreen) {
		c->issetfullscreen = 1;
		resizeclient(c, selmon->mx, selmon->my + bh, selmon->mw - 2 * cborderpx, selmon->mh - bh - 2 * cborderpx);
	} else {
		c->issetfullscreen = 0;
		resizeclient(c, c->oldx, c->oldy, c->oldw, c->oldh);
    }
}

void
spawnxterm(const Arg *arg)
{
    xtermcount += 1;

    if (fork() == 0) {
        if (dpy)
            close(ConnectionNumber(dpy));
        setsid();
        childsignals();

        /* int x = selmon->wx + (selmon->mw - 1230 + xtermcount * offsetoverlapxterm); */
        /* int y = selmon->wy + (selmon->mh - 1050 + xtermcount * offsetoverlapxterm); */
        /* char geobuf[16]; */
        /* snprintf(geobuf, sizeof(geobuf), "90x39+%d+%d", x, y); */
        /* char *cmd = "xterm"; */
        /* // char *args[] = { "xterm", "-xrm", "*allowSendEvents:true", "-geometry", geobuf, "-fa", "courier 10 pitch", "-fs", "13", "-bg", "#21222C", "-fg", "#1B7FDA", NULL }; */
        /* char *args[] = { "xterm", "-xrm", "*allowSendEvents:true", "-geometry", geobuf, NULL }; */
        /*  */
		/* execvp(cmd, args); */
		/* die("dwm: execvp '%s' failed:", ((char **)arg->v)[0]); */

        int x = selmon->wx + (selmon->mw - 1230 + xtermcount * offsetoverlapxterm);
        int y = selmon->wy + (selmon->mh - 1050 + xtermcount * offsetoverlapxterm);

        char pos_x[64];
        char pos_y[64];
        snprintf(pos_x, sizeof(pos_x), "window.position.x=%d", x);
        snprintf(pos_y, sizeof(pos_y), "window.position.y=%d", y);

        char *cmd = "alacritty";

        char *args[] = {
            "alacritty",
            "-o", "font.normal.family=\"courier 10 pitch\"",
            "-o", "font.size=10",
            "-o", "colors.primary.background=\"#21222C\"",
            "-o", "colors.primary.foreground=\"#1B7FDA\"",
            "-o", "window.dimensions.columns=85",
            "-o", "window.dimensions.lines=27",
            "-o", "window.padding.x=3",
            "-o", "window.padding.y=3",
            "-o", pos_x,
            "-o", pos_y,
            NULL
        };

        execvp(cmd, args);
        die("dwm: execvp '%s' failed:", cmd);
    }
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp("-v", argv[1]))
		die("dwm-"VERSION);
	else if (argc != 1)
		die("usage: dwm [-v]");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	/* Before the display is open: find the session bus and declare the desktop
	 * identity, so everything we later spawn inherits a correct environment. */
	session_init();
	if (!(dpy = XOpenDisplay(NULL)))
		die("dwm: cannot open display");
	checkotherwm();
	setup();
	/* Now that DISPLAY is known good, publish it to D-Bus and systemd --user
	 * so activated services (portals, keyring, notifications) can reach us. */
	session_exportenv();
	session_autostart();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == -1)
		die("pledge");
#endif /* __OpenBSD__ */
	scan();
	run();
	cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
