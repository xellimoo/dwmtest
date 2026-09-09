/* See LICENSE file for copyright and license details. */
/* StatusNotifierItem host. See sni.h for scope. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sni.h"

#ifndef EDWM_DBUS

void sni_init(void (*c)(void), Drw *d) { (void)c; (void)d; }
void sni_connected(void) {}
void sni_disconnected(void) {}
int  sni_handle_message(void *m) { (void)m; return 0; }
int  sni_count(void) { return 0; }
int  sni_width(int i, int s) { (void)i; (void)s; return 0; }
void sni_draw(Drw *d, int x, int y, int i, int s, unsigned long b)
{ (void)d; (void)x; (void)y; (void)i; (void)s; (void)b; }
int  sni_indexat(int x, int xs, int i, int s) { (void)x; (void)xs; (void)i; (void)s; return -1; }
void sni_click(int i, int b, int x, int y) { (void)i; (void)b; (void)x; (void)y; }
void sni_setmetrics(int i) { (void)i; }
void sni_cleanup(void) {}

#else /* EDWM_DBUS */

#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dbus/dbus.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "dbusif.h"
#include "dbusmenu.h"
#include "img.h"

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE    "org.kde.StatusNotifierItem"
#define PROPS_IFACE   "org.freedesktop.DBus.Properties"

#define MAXITEMS   16
#define MAXPENDING 32
#define MAXCACHE   32
#define NAMELEN    128
#define PATHLEN    512

typedef struct {
	char service[NAMELEN];   /* unique bus name that owns the item */
	char path[PATHLEN];      /* its object path */
	char id[NAMELEN];
	char title[NAMELEN];
	char iconname[NAMELEN];
	char themepath[PATHLEN];
	char menupath[PATHLEN];  /* the item's com.canonical.dbusmenu object */
	int passive;             /* Status == "Passive": hidden */
	Img icon;              /* already scaled to iconsize */
	int used;
} Item;

typedef struct {
	dbus_uint32_t serial;
	int item;
	int used;
} Pending;

typedef struct {
	char key[NAMELEN + 16];
	char file[PATHLEN];
	int used;
} IconCache;

static Item items[MAXITEMS];
static Pending pending[MAXPENDING];
static IconCache iconcache[MAXCACHE];
static int nitems;
static int iconsize = 16;
static int haswatcher;       /* we own org.kde.StatusNotifierWatcher */
static void (*onchange)(void);
static Drw *menudrw;         /* borrowed, for rendering item menus */

static void refreshitem(int i);

/* ------------------------------------------------------------- helpers */

static void
changed(void)
{
	if (onchange)
		onchange();
}

static int
finditem(const char *service, const char *path)
{
	int i;

	for (i = 0; i < MAXITEMS; i++)
		if (items[i].used && !strcmp(items[i].service, service)
		&& (!path || !strcmp(items[i].path, path)))
			return i;
	return -1;
}

/* ------------------------------------------------------------- images */

/* Pull a pixel size out of a path component such as "22x22" or "22". */
static int
dirsize(const char *name)
{
	int n = 0;

	while (*name >= '0' && *name <= '9')
		n = n * 10 + (*name++ - '0');
	if (n && (*name == '\0' || *name == 'x'))
		return n;
	return 0;
}

/* Depth-limited search for <name>.png, preferring the size closest to `want`.
 * Only PNG is considered: most themes also ship SVG, but rendering that would
 * mean pulling librsvg into a window manager. */
static void
searchdir(const char *dir, const char *name, int want, int depth,
          char *best, size_t bestlen, int *bestscore, int cursize)
{
	DIR *d;
	struct dirent *e;
	char sub[PATHLEN];
	struct stat st;
	size_t nlen = strlen(name);
	int sz, score;

	if (depth > 4 || !(d = opendir(dir)))
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name) >= (int)sizeof sub)
			continue;
		if (stat(sub, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			sz = dirsize(e->d_name);
			searchdir(sub, name, want, depth + 1, best, bestlen, bestscore,
			          sz ? sz : cursize);
		} else if (!strncmp(e->d_name, name, nlen)
		&& !strcmp(e->d_name + nlen, ".png")) {
			/* prefer the closest size, and a larger one over a smaller when
			 * equally distant, since downscaling beats upscaling */
			sz = cursize ? cursize : want;
			score = sz >= want ? (sz - want) : (want - sz) * 2 + 1;
			if (*bestscore < 0 || score < *bestscore) {
				*bestscore = score;
				snprintf(best, bestlen, "%s", sub);
			}
		}
	}
	closedir(d);
}

static int
findiconfile(const char *name, const char *themepath, int want,
             char *out, size_t len)
{
	/* /usr/local first: that is where the BSDs and locally built software put
	 * icon themes, and where they should win over anything in /usr. */
	static const char *fallback[] = { "/usr/local/share/icons",
	                                  "/usr/local/share/pixmaps",
	                                  "/usr/share/icons",
	                                  "/usr/share/pixmaps", NULL };
	char key[NAMELEN + 16], root[PATHLEN];
	const char *home, *xdgdata, *xdgdirs, *p, *q;
	int best = -1, i;

	if (!name || !*name || strchr(name, '/'))
		return 0;

	snprintf(key, sizeof key, "%s@%d", name, want);
	for (i = 0; i < MAXCACHE; i++)
		if (iconcache[i].used && !strcmp(iconcache[i].key, key)) {
			snprintf(out, len, "%s", iconcache[i].file);
			return out[0] != '\0';
		}

	out[0] = '\0';

	/* An item may ship its own icons and point us at them. */
	if (themepath && *themepath)
		searchdir(themepath, name, want, 0, out, len, &best, 0);

	if (best < 0 && (home = getenv("HOME"))) {
		snprintf(root, sizeof root, "%s/.local/share/icons", home);
		searchdir(root, name, want, 0, out, len, &best, 0);
		if (best < 0) {
			snprintf(root, sizeof root, "%s/.icons", home);
			searchdir(root, name, want, 0, out, len, &best, 0);
		}
	}
	if (best < 0 && (xdgdata = getenv("XDG_DATA_HOME")) && *xdgdata) {
		snprintf(root, sizeof root, "%s/icons", xdgdata);
		searchdir(root, name, want, 0, out, len, &best, 0);
	}
	if (best < 0 && (xdgdirs = getenv("XDG_DATA_DIRS")) && *xdgdirs) {
		for (p = xdgdirs; *p && best < 0; p = *q ? q + 1 : q) {
			q = strchr(p, ':');
			if (!q)
				q = p + strlen(p);
			if (q - p > 0 && (size_t)(q - p) < sizeof root - 8) {
				memcpy(root, p, q - p);
				snprintf(root + (q - p), sizeof root - (q - p), "/icons");
				searchdir(root, name, want, 0, out, len, &best, 0);
			}
		}
	}
	for (i = 0; fallback[i] && best < 0; i++)
		searchdir(fallback[i], name, want, 0, out, len, &best, 0);

	/* Cache misses too: a missing icon must not re-walk the theme tree on
	 * every redraw. */
	for (i = 0; i < MAXCACHE; i++)
		if (!iconcache[i].used) {
			iconcache[i].used = 1;
			snprintf(iconcache[i].key, sizeof iconcache[i].key, "%s", key);
			snprintf(iconcache[i].file, sizeof iconcache[i].file, "%s", out);
			break;
		}
	return out[0] != '\0';
}

static void
loadiconbyname(Item *it)
{
	char file[PATHLEN];
	Img raw;

	img_free(&it->icon);
	if (!findiconfile(it->iconname, it->themepath, iconsize, file, sizeof file))
		return;
	memset(&raw, 0, sizeof raw);
	if (!img_loadfile(file, &raw))
		return;
	img_fit(&raw, iconsize);
	it->icon = raw;
}

/* IconPixmap is a(iiay): width, height and ARGB32 bytes in network order.
 * When an item offers it we use it and skip the theme entirely. */
static int
readiconpixmap(DBusMessageIter *variant, Item *it)
{
	DBusMessageIter arr, st, bytes;
	dbus_int32_t w, h;
	unsigned char *data;
	int n, best = -1, bw = 0, bh = 0;
	unsigned char *bdata = NULL;
	Img raw;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_ARRAY)
		return 0;
	dbus_message_iter_recurse(variant, &arr);
	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		dbus_message_iter_recurse(&arr, &st);
		if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
			break;
		dbus_message_iter_get_basic(&st, &w);
		dbus_message_iter_next(&st);
		dbus_message_iter_get_basic(&st, &h);
		dbus_message_iter_next(&st);
		if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&st, &bytes);
			dbus_message_iter_get_fixed_array(&bytes, &data, &n);
			if (w > 0 && h > 0 && n == w * h * 4) {
				int score = w >= iconsize ? w - iconsize : (iconsize - w) * 2 + 1;
				if (best < 0 || score < best) {
					best = score;
					bw = w; bh = h; bdata = data;
				}
			}
		}
		dbus_message_iter_next(&arr);
	}
	if (best < 0 || !bdata)
		return 0;

	memset(&raw, 0, sizeof raw);
	if (!img_fromargb32(bdata, bw, bh, &raw))
		return 0;
	img_fit(&raw, iconsize);
	img_free(&it->icon);
	it->icon = raw;
	return 1;
}

/* --------------------------------------------------------- D-Bus plumbing */

static DBusConnection *
conn(void)
{
	return dbusif_conn();
}

static void
addmatch(const char *rule)
{
	if (conn())
		/* NULL error: fire and forget. Passing a real DBusError would make
		 * this a blocking round trip. */
		dbus_bus_add_match(conn(), rule, NULL);
}

static void
trackpending(dbus_uint32_t serial, int item)
{
	int i;

	for (i = 0; i < MAXPENDING; i++)
		if (!pending[i].used) {
			pending[i].used = 1;
			pending[i].serial = serial;
			pending[i].item = item;
			return;
		}
}

/* Ask an item for everything at once. Sent without waiting: the reply is
 * matched by serial in sni_handle_message, so the WM never blocks on an
 * application that is slow or wedged. */
static void
refreshitem(int i)
{
	DBusMessage *m;
	dbus_uint32_t serial;
	const char *iface = ITEM_IFACE;

	if (!conn() || !items[i].used)
		return;
	m = dbus_message_new_method_call(items[i].service, items[i].path,
	                                 PROPS_IFACE, "GetAll");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
	if (dbus_connection_send(conn(), m, &serial))
		trackpending(serial, i);
	dbus_message_unref(m);
}

static void
emitwatcher(const char *signame, const char *arg)
{
	DBusMessage *m;

	if (!conn())
		return;
	if (!(m = dbus_message_new_signal(WATCHER_PATH, WATCHER_IFACE, signame)))
		return;
	if (arg)
		dbus_message_append_args(m, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
	dbus_connection_send(conn(), m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(conn());
}

static void
additem(const char *service, const char *path)
{
	char buf[NAMELEN + PATHLEN + 2];
	int i;

	if (finditem(service, path) >= 0)
		return;
	for (i = 0; i < MAXITEMS; i++)
		if (!items[i].used)
			break;
	if (i == MAXITEMS)
		return;

	memset(&items[i], 0, sizeof items[i]);
	items[i].used = 1;
	snprintf(items[i].service, sizeof items[i].service, "%s", service);
	snprintf(items[i].path, sizeof items[i].path, "%s", path);
	nitems++;

	snprintf(buf, sizeof buf, "%.*s%.*s", NAMELEN, service, PATHLEN, path);
	emitwatcher("StatusNotifierItemRegistered", buf);
	refreshitem(i);
}

static void
dropitem(int i)
{
	char buf[NAMELEN + PATHLEN + 2];

	if (!items[i].used)
		return;
	snprintf(buf, sizeof buf, "%.*s%.*s", NAMELEN, items[i].service,
			         PATHLEN, items[i].path);
	img_free(&items[i].icon);
	items[i].used = 0;
	nitems--;
	emitwatcher("StatusNotifierItemUnregistered", buf);
	changed();
}

/* --------------------------------------------------------- property reply */

static const char *
variantstring(DBusMessageIter *v)
{
	const char *s = NULL;

	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING
	 || dbus_message_iter_get_arg_type(v) == DBUS_TYPE_OBJECT_PATH)
		dbus_message_iter_get_basic(v, &s);
	return s;
}

static void
handleprops(int idx, DBusMessage *msg)
{
	DBusMessageIter iter, dict, entry, val;
	Item *it = &items[idx];
	const char *key, *s;
	int gotpixmap = 0, iconchanged = 0;
	char prevname[NAMELEN], prevtheme[PATHLEN];

	if (!it->used || !dbus_message_iter_init(msg, &iter))
		return;
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return;

	snprintf(prevname, sizeof prevname, "%s", it->iconname);
	snprintf(prevtheme, sizeof prevtheme, "%s", it->themepath);

	dbus_message_iter_recurse(&iter, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &val);

		if (!strcmp(key, "Id")) {
			if ((s = variantstring(&val)))
				snprintf(it->id, sizeof it->id, "%s", s);
		} else if (!strcmp(key, "Title")) {
			if ((s = variantstring(&val)))
				snprintf(it->title, sizeof it->title, "%s", s);
		} else if (!strcmp(key, "Status")) {
			if ((s = variantstring(&val)))
				it->passive = !strcmp(s, "Passive");
		} else if (!strcmp(key, "IconName")) {
			if ((s = variantstring(&val)))
				snprintf(it->iconname, sizeof it->iconname, "%s", s);
		} else if (!strcmp(key, "Menu")) {
			if ((s = variantstring(&val)))
				snprintf(it->menupath, sizeof it->menupath, "%s", s);
		} else if (!strcmp(key, "IconThemePath")) {
			if ((s = variantstring(&val)))
				snprintf(it->themepath, sizeof it->themepath, "%s", s);
		} else if (!strcmp(key, "IconPixmap")) {
			/* Pixels beat a name: no theme lookup, and exactly what the
			 * application intended. */
			if (readiconpixmap(&val, it)) {
				gotpixmap = 1;
				iconchanged = 1;
			}
		}
		dbus_message_iter_next(&dict);
	}

	if (!gotpixmap && it->iconname[0]
	&& (!it->icon.rgba || strcmp(prevname, it->iconname)
	    || strcmp(prevtheme, it->themepath))) {
		loadiconbyname(it);
		iconchanged = 1;
	}
	(void)iconchanged;
	changed();
}

/* ------------------------------------------------------------- watcher */

static const char *watcher_xml =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
"    <method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method>\n"
"  </interface>\n"
"  <interface name=\"org.freedesktop.DBus.Properties\">\n"
"    <method name=\"Get\">\n"
"      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"property\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
"    </method>\n"
"    <method name=\"GetAll\">\n"
"      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
"      <arg name=\"props\" type=\"a{sv}\" direction=\"out\"/>\n"
"    </method>\n"
"  </interface>\n"
"  <interface name=\"" WATCHER_IFACE "\">\n"
"    <method name=\"RegisterStatusNotifierItem\">\n"
"      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
"    </method>\n"
"    <method name=\"RegisterStatusNotifierHost\">\n"
"      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
"    </method>\n"
"    <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
"    <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
"    <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
"    <signal name=\"StatusNotifierItemRegistered\"><arg name=\"service\" type=\"s\"/></signal>\n"
"    <signal name=\"StatusNotifierItemUnregistered\"><arg name=\"service\" type=\"s\"/></signal>\n"
"    <signal name=\"StatusNotifierHostRegistered\"/>\n"
"    <signal name=\"StatusNotifierHostUnregistered\"/>\n"
"  </interface>\n"
"</node>\n";

static void
append_items_array(DBusMessageIter *it)
{
	DBusMessageIter arr;
	char buf[NAMELEN + PATHLEN + 2];
	const char *p = buf;
	int i;

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "s", &arr);
	for (i = 0; i < MAXITEMS; i++)
		if (items[i].used) {
			snprintf(buf, sizeof buf, "%.*s%.*s", NAMELEN, items[i].service,
			         PATHLEN, items[i].path);
			dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &p);
		}
	dbus_message_iter_close_container(it, &arr);
}

static void
reply_prop(DBusMessage *msg, const char *prop)
{
	DBusMessage *r;
	DBusMessageIter iter, var;
	dbus_bool_t t = TRUE;
	dbus_int32_t ver = 0;

	if (!(r = dbus_message_new_method_return(msg)))
		return;
	dbus_message_iter_init_append(r, &iter);
	if (!strcmp(prop, "RegisteredStatusNotifierItems")) {
		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "as", &var);
		append_items_array(&var);
		dbus_message_iter_close_container(&iter, &var);
	} else if (!strcmp(prop, "IsStatusNotifierHostRegistered")) {
		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &var);
		dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &t);
		dbus_message_iter_close_container(&iter, &var);
	} else if (!strcmp(prop, "ProtocolVersion")) {
		dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "i", &var);
		dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &ver);
		dbus_message_iter_close_container(&iter, &var);
	} else {
		dbus_message_unref(r);
		r = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, prop);
	}
	if (r) {
		dbus_connection_send(conn(), r, NULL);
		dbus_message_unref(r);
	}
}

static void
reply_allprops(DBusMessage *msg)
{
	DBusMessage *r;
	DBusMessageIter iter, dict, ent, var;
	const char *k;
	dbus_bool_t t = TRUE;
	dbus_int32_t ver = 0;

	if (!(r = dbus_message_new_method_return(msg)))
		return;
	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

	k = "RegisteredStatusNotifierItems";
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
	dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "as", &var);
	append_items_array(&var);
	dbus_message_iter_close_container(&ent, &var);
	dbus_message_iter_close_container(&dict, &ent);

	k = "IsStatusNotifierHostRegistered";
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
	dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "b", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &t);
	dbus_message_iter_close_container(&ent, &var);
	dbus_message_iter_close_container(&dict, &ent);

	k = "ProtocolVersion";
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
	dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "i", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &ver);
	dbus_message_iter_close_container(&ent, &var);
	dbus_message_iter_close_container(&dict, &ent);

	dbus_message_iter_close_container(&iter, &dict);
	dbus_connection_send(conn(), r, NULL);
	dbus_message_unref(r);
}

/* ------------------------------------------------------------ dispatch */

int
sni_handle_message(void *vmsg)
{
	DBusMessage *msg = vmsg;
	DBusMessage *r;
	const char *iface, *member, *sender, *arg, *s;
	char path[PATHLEN], service[NAMELEN];
	dbus_uint32_t rs;
	int i, type;

	if (!msg || !conn())
		return 0;

	type = dbus_message_get_type(msg);
	iface = dbus_message_get_interface(msg);
	member = dbus_message_get_member(msg);
	sender = dbus_message_get_sender(msg);

	/* 1. a property reply we asked for */
	if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) {
		rs = dbus_message_get_reply_serial(msg);
		for (i = 0; i < MAXPENDING; i++)
			if (pending[i].used && pending[i].serial == rs) {
				pending[i].used = 0;
				if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN)
					handleprops(pending[i].item, msg);
				return 1;
			}
		return 0;
	}

	/* 2. an item telling us something changed */
	if (type == DBUS_MESSAGE_TYPE_SIGNAL && iface && !strcmp(iface, ITEM_IFACE)) {
		if ((i = finditem(sender, NULL)) >= 0)
			refreshitem(i);
		return 1;
	}

	/* 3. the watcher name we queued for became ours */
	if (type == DBUS_MESSAGE_TYPE_SIGNAL && iface
	&& !strcmp(iface, "org.freedesktop.DBus")
	&& member && !strcmp(member, "NameAcquired")) {
		const char *name;

		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name,
		                          DBUS_TYPE_INVALID)
		&& name && !strcmp(name, WATCHER_NAME) && !haswatcher) {
			haswatcher = 1;
			fprintf(stderr, "edwm: took over the StatusNotifierWatcher name\n");
			emitwatcher("StatusNotifierHostRegistered", NULL);
		}
		return 0;
	}

	/* 4. an item's process went away */
	if (type == DBUS_MESSAGE_TYPE_SIGNAL && iface
	&& !strcmp(iface, "org.freedesktop.DBus")
	&& member && !strcmp(member, "NameOwnerChanged")) {
		const char *name, *old, *new;

		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name,
		                          DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &new,
		                          DBUS_TYPE_INVALID) && new && !*new) {
			for (i = 0; i < MAXITEMS; i++)
				if (items[i].used && !strcmp(items[i].service, name))
					dropitem(i);
		}
		return 0; /* others may care about this too */
	}

	if (type != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return 0;

	/* 5. calls addressed to our watcher object */
	if (!dbus_message_has_path(msg, WATCHER_PATH))
		return 0;

	if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable")
	&& member && !strcmp(member, "Introspect")) {
		if ((r = dbus_message_new_method_return(msg))) {
			dbus_message_append_args(r, DBUS_TYPE_STRING, &watcher_xml,
			                         DBUS_TYPE_INVALID);
			dbus_connection_send(conn(), r, NULL);
			dbus_message_unref(r);
		}
		return 1;
	}

	if (iface && !strcmp(iface, PROPS_IFACE)) {
		const char *wanted, *prop;

		if (member && !strcmp(member, "Get")
		&& dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &wanted,
		                         DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
			reply_prop(msg, prop);
			return 1;
		}
		if (member && !strcmp(member, "GetAll")) {
			reply_allprops(msg);
			return 1;
		}
		return 0;
	}

	if (!iface || strcmp(iface, WATCHER_IFACE))
		return 0;

	if (member && !strcmp(member, "RegisterStatusNotifierItem")) {
		if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &arg,
		                           DBUS_TYPE_INVALID)) {
			if ((r = dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS,
			                                "expected a service name")))
				{ dbus_connection_send(conn(), r, NULL); dbus_message_unref(r); }
			return 1;
		}
		/* The argument is either a bus name or an object path. libappindicator
		 * sends the path and expects the sender's own name to be used, which
		 * is why nm-applet shows up as ":1.N@/org/ayatana/...". */
		if (arg[0] == '/') {
			snprintf(service, sizeof service, "%s", sender ? sender : "");
			snprintf(path, sizeof path, "%s", arg);
		} else {
			snprintf(service, sizeof service, "%s", arg);
			snprintf(path, sizeof path, "%s", "/StatusNotifierItem");
		}
		if (*service)
			additem(service, path);
		if (!dbus_message_get_no_reply(msg)
		&& (r = dbus_message_new_method_return(msg))) {
			dbus_connection_send(conn(), r, NULL);
			dbus_message_unref(r);
		}
		return 1;
	}

	if (member && !strcmp(member, "RegisterStatusNotifierHost")) {
		if (!dbus_message_get_no_reply(msg)
		&& (r = dbus_message_new_method_return(msg))) {
			dbus_connection_send(conn(), r, NULL);
			dbus_message_unref(r);
		}
		emitwatcher("StatusNotifierHostRegistered", NULL);
		return 1;
	}

	if ((r = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_METHOD, member))) {
		dbus_connection_send(conn(), r, NULL);
		dbus_message_unref(r);
	}
	(void)s;
	return 1;
}

/* ------------------------------------------------------------ lifecycle */

void
sni_init(void (*cb)(void), Drw *drw)
{
	onchange = cb;
	menudrw = drw;
}

void
sni_connected(void)
{
	DBusError err;
	char hostname[64];
	int ret, i;

	if (!conn())
		return;

	dbus_error_init(&err);
	/* Queue for the name rather than giving up if it is taken. Another tray
	 * may legitimately own it (a desktop shell), in which case we simply show
	 * nothing and the bus hands the name over if that tray ever exits. It also
	 * covers restarting edwm, where the previous instance's connection has not
	 * been reaped yet and the name is briefly still held. Stealing it with
	 * REPLACE_EXISTING would break a real tray, so we wait instead. */
	ret = dbus_bus_request_name(conn(), WATCHER_NAME, 0, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "edwm: StatusNotifierWatcher: %s\n", err.message);
		dbus_error_free(&err);
		haswatcher = 0;
	} else if (ret == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
	        || ret == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
		haswatcher = 1;
	} else {
		fprintf(stderr, "edwm: StatusNotifierWatcher is owned by another tray; "
		                "queued for it\n");
		haswatcher = 0;
	}

	/* Items check for a host before they bother publishing themselves. */
	snprintf(hostname, sizeof hostname, "org.kde.StatusNotifierHost-%ld",
	         (long)getpid());
	dbus_bus_request_name(conn(), hostname, DBUS_NAME_FLAG_REPLACE_EXISTING, NULL);
	emitwatcher("StatusNotifierHostRegistered", NULL);

	addmatch("type='signal',interface='" ITEM_IFACE "'");
	addmatch("type='signal',interface='org.freedesktop.DBus',"
	         "member='NameOwnerChanged'");
	dbus_connection_flush(conn());

	/* Anything still registered from before a reconnect needs re-querying. */
	for (i = 0; i < MAXITEMS; i++)
		if (items[i].used)
			refreshitem(i);
}

void
sni_disconnected(void)
{
	int i;

	haswatcher = 0;
	for (i = 0; i < MAXPENDING; i++)
		pending[i].used = 0;
	/* Items are kept: they are almost certainly still running and will be
	 * re-queried once the bus comes back. */
}

void
sni_setmetrics(int size)
{
	int i;

	if (size <= 0 || size == iconsize)
		return;
	iconsize = size;
	/* cached files were chosen for the old size */
	for (i = 0; i < MAXCACHE; i++)
		iconcache[i].used = 0;
	for (i = 0; i < MAXITEMS; i++)
		if (items[i].used) {
			img_free(&items[i].icon);
			if (items[i].iconname[0])
				loadiconbyname(&items[i]);
		}
	changed();
}

void
sni_cleanup(void)
{
	int i;

	for (i = 0; i < MAXITEMS; i++)
		if (items[i].used)
			img_free(&items[i].icon);
	nitems = 0;
	if (conn() && haswatcher)
		dbus_bus_release_name(conn(), WATCHER_NAME, NULL);
	haswatcher = 0;
}

/* -------------------------------------------------------------- drawing */

/* Items with Status "Passive" are asking not to be shown. */
static int
visible(const Item *it)
{
	return it->used && !it->passive && it->icon.rgba;
}

int
sni_count(void)
{
	int i, n = 0;

	for (i = 0; i < MAXITEMS; i++)
		if (visible(&items[i]))
			n++;
	return n;
}

int
sni_width(int size, int spacing)
{
	int n = sni_count();

	return n ? n * size + (n - 1) * spacing : 0;
}

int
sni_indexat(int x, int xstart, int size, int spacing)
{
	int i, n = 0;

	for (i = 0; i < MAXITEMS; i++)
		if (visible(&items[i])) {
			int x0 = xstart + n * (size + spacing);
			if (x >= x0 && x < x0 + size)
				return i;
			n++;
		}
	return -1;
}

void
sni_draw(Drw *drw, int x, int y, int size, int spacing, unsigned long bg)
{
	int i;

	for (i = 0; i < MAXITEMS; i++)
		if (visible(&items[i])) {
			img_draw(drw, &items[i].icon, x, y, bg);
			x += size + spacing;
		}
}

void
sni_click(int index, int button, int xroot, int yroot)
{
	DBusMessage *m;
	const char *method;
	dbus_int32_t x = xroot, y = yroot;

	if (index < 0 || index >= MAXITEMS || !items[index].used || !conn())
		return;

	/* Items publish their menu as a D-Bus object rather than drawing it, so
	 * a right click means "fetch and show that", not "tell the app to". */
	if (button == 3 && items[index].menupath[0]) {
		dbusmenu_open(items[index].service, items[index].menupath,
		              menudrw, xroot, yroot);
		return;
	}

	switch (button) {
	case 1: method = "Activate"; break;
	case 2: method = "SecondaryActivate"; break;
	case 3: method = "ContextMenu"; break;
	default: return;
	}

	m = dbus_message_new_method_call(items[index].service, items[index].path,
	                                 ITEM_IFACE, method);
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y,
	                         DBUS_TYPE_INVALID);
	/* Fire and forget: the reply tells us nothing we act on, and waiting for
	 * it would stall the event loop on the application. */
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(conn(), m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(conn());
}

#endif /* EDWM_DBUS */
