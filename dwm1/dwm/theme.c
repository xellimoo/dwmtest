/* theme.c - runtime color themes for edwm.
 *
 * A theme file has two optional sections: [palette] names colors and
 * [scheme] maps them onto scheme rows ("sel = fg[, bg[, border]]", parts
 * are palette names or literal colors). A flat "norm_fg = #hex" layout is
 * also accepted. Unlisted slots keep the built-in config.h defaults.
 *
 * All disk I/O happens in theme_init()/theme_rescan(); theme_build()
 * performs no I/O so switching is cheap and cannot block the WM.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "theme.h"
#include "util.h"

#define LENGTH(X)               (sizeof X / sizeof X[0])
#define STOCKTHEMEDIR           "/usr/local/share/edwm/themes"
#define MAXPALETTE              64

typedef struct {
	char *name;
	char *val;
} PaletteEntry;

typedef struct {
	int scheme;
	char *ref[3];                 /* raw fg/bg/border parts, resolved after parse */
	int nref;
} SchemeEntry;

struct Theme {
	char *name;                 /* file basename sans extension */
	char *col[SchemeLast][3];   /* NULL slot = built-in default from config.h */
	char *dmenu[4];             /* NULL = default */
};

static Theme *themes;
static int nthemes, cur;           /* cur always valid once theme_init() ran */
static const char *(*builtin)[3]; /* config.h colors[][3], SchemeLast rows */
static char dmenu_def[4][32];     /* copied: the caller's array may be temporary */
static char userdir[512];         /* expanded at theme_init() for rescans */

char theme_dmenu_nb[32], theme_dmenu_nf[32], theme_dmenu_sb[32], theme_dmenu_sf[32];

static struct { const char *key; int scheme; } schemekeys[] = {
	{ "norm",    SchemeNorm },
	{ "sel",     SchemeSel },
	{ "tagsel",  SchemeTagSel },
	{ "tasksel", SchemeTaskSel },
	{ "urg",     SchemeUrg },
	{ "hid",     SchemeHid },
	{ "status",  SchemeStatus },
};

static struct { const char *key; int scheme, col; } slotkeys[] = {
	{ "norm_fg",     SchemeNorm,    ColFg },
	{ "norm_bg",     SchemeNorm,    ColBg },
	{ "norm_border", SchemeNorm,    ColBorder },
	{ "sel_fg",      SchemeSel,     ColFg },
	{ "sel_bg",      SchemeSel,     ColBg },
	{ "sel_border",  SchemeSel,     ColBorder },
	{ "urg_fg",      SchemeUrg,     ColFg },
	{ "urg_bg",      SchemeUrg,     ColBg },
	{ "urg_border",  SchemeUrg,     ColBorder },
	{ "hid_fg",      SchemeHid,     ColFg },
	{ "hid_bg",      SchemeHid,     ColBg },
	{ "hid_border",  SchemeHid,     ColBorder },
};

static char *
xstrdup(const char *s)
{
	char *d = strdup(s);

	if (!d)
		die("edwm: out of memory");
	return d;
}

static char *
trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	return s;
}

static void
freetheme(Theme *t)
{
	int i, j;

	free(t->name);
	for (i = 0; i < SchemeLast; i++)
		for (j = 0; j < 3; j++)
			free(t->col[i][j]);
	for (i = 0; i < 4; i++)
		free(t->dmenu[i]);
	memset(t, 0, sizeof(Theme));
}

/* append a zeroed theme with the given (owned) name, skipping duplicates;
 * returns its index or -1 when the name already exists */
static int
theme_add(char *name)
{
	Theme *t;
	int i;

	for (i = 0; i < nthemes; i++)
		if (!strcmp(themes[i].name, name)) {
			free(name);
			return -1;
		}
	t = realloc(themes, (nthemes + 1) * sizeof(Theme));
	if (!t) {
		free(name);
		return -1;
	}
	themes = t;
	memset(&themes[nthemes], 0, sizeof(Theme));
	themes[nthemes].name = name;
	return nthemes++;
}

static void
theme_parse(Theme *t, const char *path)
{
	FILE *f;
	char line[512], *key, *val, *p, *part;
	char section[32] = "";
	PaletteEntry pal[MAXPALETTE];
	SchemeEntry ents[MAXPALETTE];
	int npal = 0, nent = 0, i, j;
	size_t k;

	if (!(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		p = line + strlen(line);
		while (p > line && (p[-1] == '\n' || p[-1] == '\r'))
			*--p = '\0';
		p = trim(line);
		if (!*p || *p == '#' || *p == ';')
			continue;
		if (*p == '[') { /* section header */
			if ((p = strchr(p, ']'))) {
				*p = '\0';
				snprintf(section, sizeof section, "%s", trim(line + 1));
			}
			continue;
		}
		key = p;
		if (!(p = strchr(key, '=')))
			continue;
		*p = '\0';
		val = trim(p + 1);
		key = trim(key);
		if (!*key || !*val)
			continue;

		if (!strcmp(section, "palette")) {
			if (npal < MAXPALETTE) {
				pal[npal].name = xstrdup(key);
				pal[npal].val = xstrdup(val);
				npal++;
			}
			continue;
		}
		/* dmenu colors are accepted in any section */
		if (!strcmp(key, "dmenu_nb") || !strcmp(key, "dmenu_nf")
		|| !strcmp(key, "dmenu_sb") || !strcmp(key, "dmenu_sf")) {
			i = key[5] == 'n' ? (key[6] == 'b' ? 0 : 1) : (key[6] == 'b' ? 2 : 3);
			free(t->dmenu[i]);
			t->dmenu[i] = xstrdup(val);
			continue;
		}
		if (!strcmp(section, "scheme")) {
			for (i = 0; i < (int)LENGTH(schemekeys); i++) {
				if (strcmp(key, schemekeys[i].key))
					continue;
				if (nent < MAXPALETTE) {
					ents[nent].scheme = schemekeys[i].scheme;
					ents[nent].nref = 0;
					/* split "fg[, bg[, border]]" into raw parts */
					for (part = val; part && ents[nent].nref < 3; ents[nent].nref++) {
						char *comma = strchr(part, ',');
						if (comma)
							*comma = '\0';
						ents[nent].ref[ents[nent].nref] = xstrdup(trim(part));
						part = comma ? comma + 1 : NULL;
					}
					nent++;
				}
				break;
			}
			continue;
		}
		/* no section: flat keys */
		for (k = 0; k < LENGTH(slotkeys); k++)
			if (!strcmp(key, slotkeys[k].key)) {
				free(t->col[slotkeys[k].scheme][slotkeys[k].col]);
				t->col[slotkeys[k].scheme][slotkeys[k].col] = xstrdup(val);
				goto next;
			}
		/* unknown keys are ignored: forward compatibility */
next:		;
	}
	fclose(f);

	/* resolve [scheme] refs now that the whole palette is known: each part
	 * is a palette name, or failing that a literal color string */
	for (i = 0; i < nent; i++) {
		for (j = 0; j < ents[i].nref && j < 3; j++) {
			char *resolved = ents[i].ref[j];
			for (k = 0; k < (size_t)npal; k++)
				if (!strcmp(pal[k].name, ents[i].ref[j])) {
					resolved = pal[k].val;
					break;
				}
			free(t->col[ents[i].scheme][j]);
			t->col[ents[i].scheme][j] = xstrdup(resolved);
		}
		for (j = 0; j < ents[i].nref; j++)
			free(ents[i].ref[j]);
	}
	for (i = 0; i < npal; i++) {
		free(pal[i].name);
		free(pal[i].val);
	}
}

static int
hasthemeext(const char *name, size_t len)
{
	return (len > 5 && !strcmp(name + len - 5, ".conf"))
	    || (len > 6 && !strcmp(name + len - 6, ".theme"));
}

static void
scan_dir(const char *dirpath)
{
	char namebuf[256], path[512];
	struct dirent **ents;
	int n, i, idx;

	n = scandir(dirpath, &ents, NULL, alphasort);
	if (n < 0)
		return;
	for (i = 0; i < n; i++) {
		size_t len = strlen(ents[i]->d_name), stem;
		if (!hasthemeext(ents[i]->d_name, len)) {
			free(ents[i]);
			continue;
		}
		stem = len - (len > 6 && !strcmp(ents[i]->d_name + len - 6, ".theme") ? 6 : 5);
		if (stem == 0 || stem >= sizeof namebuf
		|| snprintf(path, sizeof path, "%s/%s", dirpath, ents[i]->d_name) >= (int)sizeof path) {
			free(ents[i]);
			continue;
		}
		snprintf(namebuf, sizeof namebuf, "%.*s", (int)stem, ents[i]->d_name);
		if ((idx = theme_add(xstrdup(namebuf))) >= 0)
			theme_parse(&themes[idx], path);
		free(ents[i]);
	}
	free(ents);
}

static void
scan_all(void)
{
	/* user directory first (its names win over the installed ones) */
	if (userdir[0])
		scan_dir(userdir);
	scan_dir(STOCKTHEMEDIR);
}

int
theme_init(const char *dir, const char *b[][3], const char *ddef[4])
{
	const char *home;
	int i;

	builtin = b;
	for (i = 0; i < 4; i++)
		snprintf(dmenu_def[i], 32, "%.31s", ddef[i]);
	cur = 0;
	userdir[0] = '\0';
	if (dir[0] == '~' && (home = getenv("HOME")) && *home)
		snprintf(userdir, sizeof userdir, "%s%s", home, dir + 1);
	else if (dir[0])
		snprintf(userdir, sizeof userdir, "%s", dir);
	/* themes[0]: pure built-in default */
	if (theme_add(xstrdup("default")) < 0)
		return -1;
	for (i = 0; i < 4; i++)
		snprintf(i == 0 ? theme_dmenu_nb : i == 1 ? theme_dmenu_nf
			: i == 2 ? theme_dmenu_sb : theme_dmenu_sf, 32, "%.31s", dmenu_def[i]);
	scan_all();
	return 0;
}

void
theme_rescan(void)
{
	char keep[256];
	int i, idx = 0;

	snprintf(keep, sizeof keep, "%s", nthemes ? themes[cur].name : "default");
	for (i = 0; i < nthemes; i++)
		freetheme(&themes[i]);
	free(themes);
	themes = NULL;
	nthemes = 0;
	cur = 0;
	theme_add(xstrdup("default"));
	scan_all();
	for (i = 0; i < nthemes; i++)
		if (!strcmp(themes[i].name, keep))
			idx = i;
	cur = idx;
}

int
theme_count(void)
{
	return nthemes;
}

const char *
theme_name(int i)
{
	return (i >= 0 && i < nthemes) ? themes[i].name : "default";
}

const char *
theme_current(void)
{
	return theme_name(cur);
}

Theme *
theme_advance(int dir)
{
	if (!nthemes)
		return NULL;
	cur = (cur + (dir >= 0 ? 1 : -1) + nthemes) % nthemes;
	return &themes[cur];
}

Theme *
theme_select(const char *name)
{
	int i;

	if (!name)
		return NULL;
	for (i = 0; i < nthemes; i++)
		if (!strcmp(themes[i].name, name)) {
			cur = i; /* by-name applies move the cursor too, so Current
			          * and ThemeChanged report what is really shown */
			return &themes[i];
		}
	return NULL;
}

/* Fallbacks so themes can stay small: tagsel->sel, tasksel->sel,
 * status->norm, and a NULL row column falls through to config.h. */
static const char *
slotcolor(Theme *t, int scheme, int col)
{
	static const int fallback[SchemeLast] = {
		[SchemeNorm] = SchemeNorm, [SchemeSel] = SchemeSel,
		[SchemeTagSel] = SchemeSel, [SchemeTaskSel] = SchemeSel,
		[SchemeUrg] = SchemeUrg, [SchemeHid] = SchemeHid,
		[SchemeStatus] = SchemeNorm,
	};

	if (t && t->col[scheme][col])
		return t->col[scheme][col];
	scheme = fallback[scheme];
	if (t && t->col[scheme][col])
		return t->col[scheme][col];
	return builtin[scheme][col] ? builtin[scheme][col] : "#000000";
}

/* Build a full scheme into scm[0..SchemeLast) (caller-allocated array).
 * On failure every row and the array itself are freed: caller must not
 * free or use scm after a -1 return. Never dies: runtime theme data is
 * untrusted and the WM must keep the previous colors on a bad theme. */
int
theme_build(Drw *drw, Theme *t, Clr **scm)
{
	int i, j, k;
	const char *name;

	for (i = 0; i < SchemeLast; i++) {
		scm[i] = ecalloc(3, sizeof(Clr));
		for (j = 0; j < 3; j++) {
			name = slotcolor(t, i, j);
			if (!XftColorAllocName(drw->dpy, DefaultVisual(drw->dpy, drw->screen),
			                       DefaultColormap(drw->dpy, drw->screen), name, &scm[i][j])) {
				fprintf(stderr, "edwm: theme '%s': cannot allocate color '%s'\n",
				        t ? t->name : "default", name);
				for (k = 0; k < j; k++)
					XftColorFree(drw->dpy, DefaultVisual(drw->dpy, drw->screen),
					             DefaultColormap(drw->dpy, drw->screen), &scm[i][k]);
				free(scm[i]);
				scm[i] = NULL;
				theme_schemefree(drw, scm); /* frees the rows made so far */
				return -1;
			}
		}
	}
	return 0;
}

void
theme_schemefree(Drw *drw, Clr **scm)
{
	int i, j;

	if (!scm)
		return;
	for (i = 0; i < SchemeLast; i++) {
		if (!scm[i])
			continue;
		for (j = 0; j < 3; j++)
			XftColorFree(drw->dpy, DefaultVisual(drw->dpy, drw->screen),
			             DefaultColormap(drw->dpy, drw->screen), &scm[i][j]);
		free(scm[i]);
	}
	free(scm);
}

void
theme_dmenu_apply(Theme *t)
{
	int i;

	for (i = 0; i < 4; i++)
		snprintf(i == 0 ? theme_dmenu_nb : i == 1 ? theme_dmenu_nf
			: i == 2 ? theme_dmenu_sb : theme_dmenu_sf, 32, "%.31s",
			(t && t->dmenu[i]) ? t->dmenu[i] : dmenu_def[i]);
}
