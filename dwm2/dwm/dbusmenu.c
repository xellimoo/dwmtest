/* See LICENSE file for copyright and license details. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbusmenu.h"

#ifndef EDWM_DBUS

void dbusmenu_open(const char *s, const char *p, Drw *d, int x, int y)
{ (void)s; (void)p; (void)d; (void)x; (void)y; }
int  dbusmenu_handle_message(void *m) { (void)m; return 0; }
void dbusmenu_reset(void) {}

#else /* EDWM_DBUS */

#include <dbus/dbus.h>

#include "dbusif.h"
#include "img.h"
#include "menu.h"

#define MENU_IFACE "com.canonical.dbusmenu"
#define MAXCHILDREN 128
#define MAXDEPTH    6

/* The menu we are currently fetching or showing. */
static char cur_service[128];
static char cur_path[512];
static Drw *cur_drw;
static int cur_x, cur_y;
static dbus_uint32_t pending_layout;   /* serial of the GetLayout in flight */
static dbus_uint32_t pending_abouttoshow;
static MenuItem *shown;                /* tree currently owned by the menu */
static int nshown;
static int iconsize = 16;

/* ------------------------------------------------------------- parsing */

/* Strip the "_" mnemonic markers GTK puts in labels ("_Available networks"),
 * and collapse "__" to a literal underscore. */
static void
striplabel(const char *in, char *out, size_t len)
{
	size_t o = 0;

	while (*in && o + 1 < len) {
		if (*in == '_') {
			in++;
			if (*in == '_')
				out[o++] = *in++;
			continue;
		}
		out[o++] = *in++;
	}
	out[o] = '\0';
}

static int
variant_bool(DBusMessageIter *v, int dflt)
{
	dbus_bool_t b;
	dbus_int32_t i;

	switch (dbus_message_iter_get_arg_type(v)) {
	case DBUS_TYPE_BOOLEAN:
		dbus_message_iter_get_basic(v, &b);
		return b ? 1 : 0;
	case DBUS_TYPE_INT32:
		dbus_message_iter_get_basic(v, &i);
		return i ? 1 : 0;
	case DBUS_TYPE_STRING: {
		const char *s;
		dbus_message_iter_get_basic(v, &s);
		return s && (!strcmp(s, "true") || !strcmp(s, "1"));
	}
	}
	return dflt;
}

static int parse_node(DBusMessageIter *node, MenuItem *out, int depth);

/* children is av, each variant holding another (ia{sv}av) */
static int
parse_children(DBusMessageIter *arr, MenuItem **out, int depth)
{
	MenuItem *items;
	DBusMessageIter var;
	int n = 0, cap = 8;

	if (!(items = calloc(cap, sizeof *items)))
		return 0;

	while (dbus_message_iter_get_arg_type(arr) == DBUS_TYPE_VARIANT
	&& n < MAXCHILDREN) {
		dbus_message_iter_recurse(arr, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRUCT) {
			if (n == cap) {
				MenuItem *bigger = realloc(items, (size_t)cap * 2 * sizeof *items);
				if (!bigger)
					break;
				items = bigger;
				memset(items + cap, 0, (size_t)cap * sizeof *items);
				cap *= 2;
			}
			if (parse_node(&var, &items[n], depth))
				n++;
		}
		dbus_message_iter_next(arr);
	}
	if (!n) {
		free(items);
		return 0;
	}
	*out = items;
	return n;
}

static int
parse_node(DBusMessageIter *node, MenuItem *out, int depth)
{
	DBusMessageIter st, props, entry, val, kids;
	dbus_int32_t id = 0;
	const char *key, *s;
	char raw[192];
	int visible = 1;
	unsigned char *bytes;
	int nbytes;

	memset(out, 0, sizeof *out);
	out->enabled = 1;

	dbus_message_iter_recurse(node, &st);
	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
		return 0;
	dbus_message_iter_get_basic(&st, &id);
	out->id = (int)id;
	dbus_message_iter_next(&st);

	if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY)
		return 0;
	dbus_message_iter_recurse(&st, &props);
	while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&props, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &val);

		if (!strcmp(key, "label")) {
			if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&val, &s);
				snprintf(raw, sizeof raw, "%s", s ? s : "");
				striplabel(raw, out->label, sizeof out->label);
			}
		} else if (!strcmp(key, "enabled")) {
			out->enabled = variant_bool(&val, 1);
		} else if (!strcmp(key, "visible")) {
			visible = variant_bool(&val, 1);
		} else if (!strcmp(key, "type")) {
			if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&val, &s);
				out->separator = s && !strcmp(s, "separator");
			}
		} else if (!strcmp(key, "toggle-type")) {
			if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&val, &s);
				if (s && !strcmp(s, "checkmark"))
					out->toggletype = MenuToggleCheck;
				else if (s && !strcmp(s, "radio"))
					out->toggletype = MenuToggleRadio;
			}
		} else if (!strcmp(key, "toggle-state")) {
			if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_INT32) {
				dbus_int32_t t;
				dbus_message_iter_get_basic(&val, &t);
				out->togglestate = (int)t;
			}
		} else if (!strcmp(key, "icon-data")) {
			if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_ARRAY) {
				DBusMessageIter ba;
				dbus_message_iter_recurse(&val, &ba);
				if (dbus_message_iter_get_arg_type(&ba) == DBUS_TYPE_BYTE) {
					dbus_message_iter_get_fixed_array(&ba, &bytes, &nbytes);
					if (nbytes > 8 && img_loadmem(bytes, (size_t)nbytes, &out->icon))
						img_fit(&out->icon, iconsize);
				}
			}
		}
		dbus_message_iter_next(&props);
	}
	dbus_message_iter_next(&st);

	if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_ARRAY && depth < MAXDEPTH) {
		dbus_message_iter_recurse(&st, &kids);
		out->nchild = parse_children(&kids, &out->child, depth + 1);
	}

	if (!visible) {
		img_free(&out->icon);
		menu_freeitems(out->child, out->nchild);
		out->child = NULL;
		out->nchild = 0;
		return 0; /* the application asked for this one to be hidden */
	}
	return 1;
}

/* ------------------------------------------------------------ requests */

static void
send_event(int id, const char *eventid)
{
	DBusMessage *m;
	DBusMessageIter it, var;
	dbus_int32_t i = id;
	dbus_uint32_t ts = 0;
	const char *empty = "";

	if (!dbusif_conn() || !cur_service[0])
		return;
	m = dbus_message_new_method_call(cur_service, cur_path, MENU_IFACE, "Event");
	if (!m)
		return;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &i);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &eventid);
	/* the spec's "data" argument; nothing we send needs it */
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &empty);
	dbus_message_iter_close_container(&it, &var);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &ts);

	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(dbusif_conn(), m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(dbusif_conn());
}

static void
on_activate(int id, void *ud)
{
	(void)ud;
	send_event(id, "clicked");
}

static void
on_closed(void *ud)
{
	(void)ud;
	/* Tell the application its menu closed, then release the tree the menu
	 * was drawing from. */
	send_event(0, "closed");
	menu_freeitems(shown, nshown);
	shown = NULL;
	nshown = 0;
}

static void
request_layout(void)
{
	DBusMessage *m;
	DBusMessageIter it, arr;
	dbus_int32_t parent = 0, depth = -1;

	if (!dbusif_conn())
		return;
	m = dbus_message_new_method_call(cur_service, cur_path, MENU_IFACE, "GetLayout");
	if (!m)
		return;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &parent);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &depth);
	/* empty property filter: give us everything */
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	dbus_message_iter_close_container(&it, &arr);

	if (!dbus_connection_send(dbusif_conn(), m, &pending_layout))
		pending_layout = 0;
	dbus_message_unref(m);
	dbus_connection_flush(dbusif_conn());
}

void
dbusmenu_open(const char *service, const char *path, Drw *drw, int x, int y)
{
	DBusMessage *m;
	dbus_int32_t root = 0;

	if (!dbusif_conn() || !service || !path || !*path)
		return;

	snprintf(cur_service, sizeof cur_service, "%s", service);
	snprintf(cur_path, sizeof cur_path, "%s", path);
	cur_drw = drw;
	cur_x = x;
	cur_y = y;
	if (drw && drw->fonts)
		iconsize = drw->fonts->h;

	/* Tell the application to refresh, then ask for the layout. Both are sent
	 * without waiting; AboutToShow's answer only says whether the layout
	 * changed, and we are about to fetch it regardless. */
	if ((m = dbus_message_new_method_call(cur_service, cur_path, MENU_IFACE,
	                                      "AboutToShow"))) {
		dbus_message_append_args(m, DBUS_TYPE_INT32, &root, DBUS_TYPE_INVALID);
		dbus_connection_send(dbusif_conn(), m, &pending_abouttoshow);
		dbus_message_unref(m);
	}
	send_event(0, "opened");
	request_layout();
}

int
dbusmenu_handle_message(void *vmsg)
{
	DBusMessage *msg = vmsg;
	DBusMessageIter it, node;
	dbus_uint32_t rs;
	MenuItem root;
	int type;

	if (!msg)
		return 0;
	type = dbus_message_get_type(msg);
	if (type != DBUS_MESSAGE_TYPE_METHOD_RETURN && type != DBUS_MESSAGE_TYPE_ERROR)
		return 0;

	rs = dbus_message_get_reply_serial(msg);
	if (pending_abouttoshow && rs == pending_abouttoshow) {
		pending_abouttoshow = 0;
		return 1;
	}
	if (!pending_layout || rs != pending_layout)
		return 0;
	pending_layout = 0;

	if (type == DBUS_MESSAGE_TYPE_ERROR) {
		fprintf(stderr, "edwm: menu fetch failed: %s\n",
		        dbus_message_get_error_name(msg));
		return 1;
	}

	/* reply is (u revision, (ia{sv}av) layout) */
	if (!dbus_message_iter_init(msg, &it))
		return 1;
	dbus_message_iter_next(&it); /* skip revision */
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRUCT)
		return 1;

	memset(&root, 0, sizeof root);
	if (!parse_node(&it, &root, 0) || root.nchild <= 0) {
		menu_freeitems(root.child, root.nchild);
		return 1;
	}
	(void)node;

	/* The root node is the menu itself; its children are the rows. Hand
	 * ownership to us, and free it when the menu closes. */
	menu_freeitems(shown, nshown);
	shown = root.child;
	nshown = root.nchild;
	img_free(&root.icon);

	menu_show(cur_drw, shown, nshown, cur_x, cur_y, on_activate, on_closed, NULL);
	return 1;
}

void
dbusmenu_reset(void)
{
	pending_layout = 0;
	pending_abouttoshow = 0;
}

#endif /* EDWM_DBUS */
