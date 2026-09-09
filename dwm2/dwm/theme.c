/* See LICENSE file for copyright and license details. */
/* Theme file parsing and colour resolution. See theme.h for the model.
 * Nothing here touches X: the result is plain numbers for dwm.c to install. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "theme.h"

/* Long enough for any real config path; theme files are addressed by name, not
 * by arbitrarily deep paths. */
#define PATH_MAX_ISH 1024
#define MAXPALETTE  64
#define MAXLINE     512
#define MAXINCLUDE  8   /* include recursion limit */

#ifndef EDWM_THEMEDIR
#define EDWM_THEMEDIR "/usr/local/share/edwm/themes"
#endif

typedef struct {
	char name[THEME_MAXNAME];
	ThemeColor color;
} PaletteEntry;

/* A scheme recipe: three references, each either a palette name or a literal
 * colour. Kept as text until the whole file is read, because a scheme may
 * reference a palette entry defined further down. */
typedef struct {
	char ref[THEME_NCOLOR][THEME_MAXNAME];
	int set;
} SchemeRecipe;

typedef struct {
	PaletteEntry palette[MAXPALETTE];
	int npalette;
	SchemeRecipe recipe[SchemeLast];
	Theme *t;
	int depth;
} ParseCtx;

static const char *schemenames[SchemeLast] = {
	[SchemeNorm]     = "norm",
	[SchemeSel]      = "sel",
	[SchemeTagNorm]  = "tagnorm",
	[SchemeTagSel]   = "tagsel",
	[SchemeTaskNorm] = "tasknorm",
	[SchemeTaskSel]  = "tasksel",
	[SchemeTaskUrg]  = "taskurg",
	[SchemeTaskMin]  = "taskmin",
	[SchemeStatus]   = "status",
	[SchemeSystray]  = "systray",
	[SchemeMenu]     = "menu",
	[SchemeMenuSel]  = "menusel",
	[SchemeMenuDim]  = "menudim",
};

/* The built-in palette and recipes reproduce the historical config.h look:
 * norm is gray3-on-gray1 with a gray2 border, sel is gray4 on cyan. */
static const struct { const char *name, *value; } defpalette[] = {
	{ "bg",     "#222222" },
	{ "fg",     "#bbbbbb" },
	{ "bright", "#eeeeee" },
	{ "accent", "#005577" },
	{ "border", "#444444" },
	{ "urgent", "#cc3333" },
	{ "muted",  "#666666" },
};

static const struct { int scm; const char *fg, *bg, *border; } defrecipe[] = {
	{ SchemeNorm,     "fg",     "bg",     "border" },
	{ SchemeSel,      "bright", "accent", "accent" },
	{ SchemeTagNorm,  "fg",     "bg",     "border" },
	{ SchemeTagSel,   "bright", "accent", "accent" },
	{ SchemeTaskNorm, "fg",     "bg",     "border" },
	{ SchemeTaskSel,  "bright", "accent", "accent" },
	{ SchemeTaskUrg,  "bright", "urgent", "urgent" },
	{ SchemeTaskMin,  "muted",  "bg",     "border" },
	{ SchemeStatus,   "fg",     "bg",     "border" },
	{ SchemeSystray,  "fg",     "bg",     "border" },
	{ SchemeMenu,     "fg",     "bg",     "border" },
	{ SchemeMenuSel,  "bright", "accent", "accent" },
	{ SchemeMenuDim,  "muted",  "bg",     "border" },
};

/* Seeds supplied by dwm.c from config.h, applied under any theme file. */
static PaletteEntry seed[MAXPALETTE];
static int nseed = 0;
static int seedborderpx = -1;

/* ------------------------------------------------------------------ colour */

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Accepts #rgb, #rrggbb and #rrggbbaa. Channels are widened to 16 bits by
 * replication (0xab -> 0xabab) so that full-scale stays full-scale. */
static int
parsecolor(const char *s, ThemeColor *c)
{
	int v[8], i, n;

	if (!s || *s != '#')
		return 0;
	s++;
	for (n = 0; n < 8 && s[n]; n++) {
		if ((v[n] = hexval((unsigned char)s[n])) < 0)
			return 0;
	}
	if (s[n])
		return 0; /* trailing junk */

	c->a = 0xffff;
	switch (n) {
	case 3:
		c->r = v[0] * 0x1111; c->g = v[1] * 0x1111; c->b = v[2] * 0x1111;
		return 1;
	case 8:
		i = v[6] * 16 + v[7];
		c->a = i * 0x101;
		/* fallthrough */
	case 6:
		c->r = (v[0] * 16 + v[1]) * 0x101;
		c->g = (v[2] * 16 + v[3]) * 0x101;
		c->b = (v[4] * 16 + v[5]) * 0x101;
		return 1;
	}
	return 0;
}

const char *
theme_colorstr(ThemeColor c, char *buf, size_t len)
{
	snprintf(buf, len, "#%02x%02x%02x%02x",
	         c.r >> 8, c.g >> 8, c.b >> 8, c.a >> 8);
	return buf;
}

const char *
theme_colorstr_rgb(ThemeColor c, char *buf, size_t len)
{
	snprintf(buf, len, "#%02x%02x%02x", c.r >> 8, c.g >> 8, c.b >> 8);
	return buf;
}

/* ------------------------------------------------------------------- tone */

static double
clamp01(double v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static void
rgb2hsl(double r, double g, double b, double *h, double *s, double *l)
{
	double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	double d = mx - mn;

	*l = (mx + mn) / 2.0;
	if (d < 1e-9) {
		*h = *s = 0.0;
		return;
	}
	*s = *l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
	if (mx == r)
		*h = (g - b) / d + (g < b ? 6.0 : 0.0);
	else if (mx == g)
		*h = (b - r) / d + 2.0;
	else
		*h = (r - g) / d + 4.0;
	*h *= 60.0;
}

static double
hue2rgb(double p, double q, double t)
{
	if (t < 0.0) t += 1.0;
	if (t > 1.0) t -= 1.0;
	if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
	if (t < 1.0 / 2.0) return q;
	if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
	return p;
}

static void
hsl2rgb(double h, double s, double l, double *r, double *g, double *b)
{
	double q, p;

	if (s < 1e-9) {
		*r = *g = *b = l;
		return;
	}
	q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
	p = 2.0 * l - q;
	h /= 360.0;
	*r = hue2rgb(p, q, h + 1.0 / 3.0);
	*g = hue2rgb(p, q, h);
	*b = hue2rgb(p, q, h - 1.0 / 3.0);
}

/* Apply the tone transform in HSL. Alpha is left alone here; opacity is
 * applied only to scheme backgrounds, where it is meaningful. */
static void
applytone(const ThemeTone *tn, ThemeColor *c)
{
	double r, g, b, h, s, l;

	if (tn->hue == 0.0 && tn->saturation == 1.0
	&& tn->lightness == 1.0 && tn->contrast == 1.0)
		return;

	r = c->r / 65535.0; g = c->g / 65535.0; b = c->b / 65535.0;
	rgb2hsl(r, g, b, &h, &s, &l);

	h += tn->hue;
	while (h < 0.0) h += 360.0;
	while (h >= 360.0) h -= 360.0;
	s = clamp01(s * tn->saturation);
	l = clamp01(l * tn->lightness);
	l = clamp01(0.5 + (l - 0.5) * tn->contrast);

	hsl2rgb(h, s, l, &r, &g, &b);
	c->r = (unsigned short)(clamp01(r) * 65535.0 + 0.5);
	c->g = (unsigned short)(clamp01(g) * 65535.0 + 0.5);
	c->b = (unsigned short)(clamp01(b) * 65535.0 + 0.5);
}

/* ------------------------------------------------------------------ paths */

const char *
theme_configdir(void)
{
	static char dir[PATH_MAX_ISH];
	const char *xdg, *home;

	if (dir[0])
		return dir;
	if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg)
		snprintf(dir, sizeof dir, "%s/edwm", xdg);
	else if ((home = getenv("HOME")) && *home)
		snprintf(dir, sizeof dir, "%s/.config/edwm", home);
	else
		snprintf(dir, sizeof dir, "/tmp/edwm");
	return dir;
}

const char *
theme_configpath(void)
{
	static char path[PATH_MAX_ISH];

	if (!path[0])
		snprintf(path, sizeof path, "%s/theme.conf", theme_configdir());
	return path;
}

int
theme_findnamed(const char *name, char *out, size_t len)
{
	struct stat st;

	if (!name || !*name || strchr(name, '/'))
		return 0; /* names are single path components, never traversals */
	snprintf(out, len, "%s/themes/%s.theme", theme_configdir(), name);
	if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
		return 1;
	snprintf(out, len, "%s/%s.theme", EDWM_THEMEDIR, name);
	if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
		return 1;
	return 0;
}

/* --------------------------------------------------------------- parsing */

static char *
trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;
	if (!*s)
		return s;
	e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e))
		*e-- = '\0';
	return s;
}

static int
palette_set(ParseCtx *ctx, const char *name, ThemeColor c)
{
	int i;

	for (i = 0; i < ctx->npalette; i++)
		if (!strcmp(ctx->palette[i].name, name)) {
			ctx->palette[i].color = c;
			return 1;
		}
	if (ctx->npalette >= MAXPALETTE)
		return 0;
	snprintf(ctx->palette[ctx->npalette].name,
	         sizeof ctx->palette[0].name, "%s", name);
	ctx->palette[ctx->npalette].color = c;
	ctx->npalette++;
	return 1;
}

static int
palette_get(ParseCtx *ctx, const char *name, ThemeColor *c)
{
	int i;

	for (i = 0; i < ctx->npalette; i++)
		if (!strcmp(ctx->palette[i].name, name)) {
			*c = ctx->palette[i].color;
			return 1;
		}
	return 0;
}

static int schemeindex(const char *name)
{
	int i;

	for (i = 0; i < SchemeLast; i++)
		if (schemenames[i] && !strcmp(schemenames[i], name))
			return i;
	return -1;
}

static int parsefile(ParseCtx *ctx, const char *path);

/* Resolve a path that may be relative to the config directory. */
static void
resolvepath(const char *base, char *out, size_t len)
{
	if (base[0] == '/')
		snprintf(out, len, "%s", base);
	else
		snprintf(out, len, "%s/%s", theme_configdir(), base);
}

static void
handlepair(ParseCtx *ctx, const char *section, char *key, char *val, const char *path, int lineno)
{
	Theme *t = ctx->t;
	ThemeColor c;
	char sub[PATH_MAX_ISH];
	int si, i;
	char *p, *tok;

	if (!strcmp(key, "include")) {
		resolvepath(val, sub, sizeof sub);
		parsefile(ctx, sub);
		return;
	}
	if (!strcmp(key, "theme")) {
		if (theme_findnamed(val, sub, sizeof sub))
			parsefile(ctx, sub);
		else
			fprintf(stderr, "edwm: %s:%d: no such theme '%s'\n", path, lineno, val);
		snprintf(t->name, sizeof t->name, "%s", val);
		return;
	}

	if (!strcmp(section, "palette")) {
		if (!parsecolor(val, &c)) {
			fprintf(stderr, "edwm: %s:%d: bad colour '%s'\n", path, lineno, val);
			return;
		}
		if (!palette_set(ctx, key, c))
			fprintf(stderr, "edwm: %s:%d: palette full, ignoring '%s'\n", path, lineno, key);
		return;
	}

	if (!strcmp(section, "tone")) {
		double d = atof(val);
		if (!strcmp(key, "hue"))             t->tone.hue = d;
		else if (!strcmp(key, "saturation")) t->tone.saturation = d;
		else if (!strcmp(key, "lightness"))  t->tone.lightness = d;
		else if (!strcmp(key, "contrast"))   t->tone.contrast = d;
		else if (!strcmp(key, "opacity"))    t->tone.opacity = clamp01(d);
		else fprintf(stderr, "edwm: %s:%d: unknown tone key '%s'\n", path, lineno, key);
		return;
	}

	if (!strcmp(section, "scheme")) {
		if ((si = schemeindex(key)) < 0) {
			fprintf(stderr, "edwm: %s:%d: unknown scheme '%s'\n", path, lineno, key);
			return;
		}
		for (i = 0, p = val; i < THEME_NCOLOR; i++) {
			tok = p;
			if ((p = strchr(p, ',')))
				*p++ = '\0';
			else
				p = tok + strlen(tok);
			tok = trim(tok);
			if (!*tok)
				break;
			snprintf(ctx->recipe[si].ref[i], THEME_MAXNAME, "%s", tok);
		}
		if (i < THEME_NCOLOR) {
			fprintf(stderr, "edwm: %s:%d: scheme '%s' needs 3 colours (fg, bg, border)\n",
			        path, lineno, key);
			return;
		}
		ctx->recipe[si].set = 1;
		return;
	}

	if (!strcmp(section, "font")) {
		if (!strcmp(key, "main"))
			snprintf(t->font, sizeof t->font, "%s", val);
		else if (!strcmp(key, "fallback"))
			snprintf(t->fontfallback, sizeof t->fontfallback, "%s", val);
		else
			fprintf(stderr, "edwm: %s:%d: unknown font key '%s'\n", path, lineno, key);
		return;
	}

	if (!strcmp(section, "metrics")) {
		int v = atoi(val);
		if (!strcmp(key, "border.width"))          t->borderpx = v < 0 ? 0 : v;
		else if (!strcmp(key, "bar.padding"))      t->barpadding = v < 0 ? 0 : v;
		else if (!strcmp(key, "task.maxwidth"))    t->taskmaxw = v;
		else if (!strcmp(key, "task.minwidth"))    t->taskminw = v;
		else if (!strcmp(key, "task.icon"))        t->taskicon = !!v;
		else if (!strcmp(key, "systray.iconsize")) t->systrayiconsize = v;
		else if (!strcmp(key, "systray.spacing"))  t->systrayspacing = v;
		else fprintf(stderr, "edwm: %s:%d: unknown metric '%s'\n", path, lineno, key);
		return;
	}

	fprintf(stderr, "edwm: %s:%d: key '%s' outside a known section\n", path, lineno, key);
}

static int
parsefile(ParseCtx *ctx, const char *path)
{
	FILE *fp;
	char line[MAXLINE], section[THEME_MAXNAME] = "";
	char *s, *cp, *eq, *key, *val;
	int lineno = 0;

	if (ctx->depth >= MAXINCLUDE) {
		fprintf(stderr, "edwm: %s: include nested too deeply, ignoring\n", path);
		return 0;
	}
	if (!(fp = fopen(path, "r")))
		return 0; /* absent files are not an error: defaults apply */

	ctx->depth++;
	while (fgets(line, sizeof line, fp)) {
		lineno++;
		s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue; /* blank or whole-line comment */
		/* Strip a trailing comment. '#' also introduces colour literals, so it
		 * only ends a value when it is surrounded by whitespace; ';' always
		 * starts a comment. This keeps "bg = #2E3440" intact while still
		 * allowing "bg = #2E3440  # nord polar night". */
		for (cp = s; *cp; cp++) {
			if (*cp == ';'
			|| (*cp == '#' && cp > s && isspace((unsigned char)cp[-1])
			    && (cp[1] == '\0' || isspace((unsigned char)cp[1])))) {
				*cp = '\0';
				break;
			}
		}
		s = trim(s);
		if (!*s)
			continue;
		if (*s == '[') {
			if (!(eq = strchr(s, ']'))) {
				fprintf(stderr, "edwm: %s:%d: unterminated section header\n", path, lineno);
				continue;
			}
			*eq = '\0';
			snprintf(section, sizeof section, "%s", trim(s + 1));
			continue;
		}
		if (!(eq = strchr(s, '='))) {
			fprintf(stderr, "edwm: %s:%d: expected 'key = value'\n", path, lineno);
			continue;
		}
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		if (*key)
			handlepair(ctx, section, key, val, path, lineno);
	}
	ctx->depth--;
	fclose(fp);
	return 1;
}

/* ------------------------------------------------------------- public API */

void
theme_seed(const char *name, const char *value)
{
	ThemeColor c;
	int i;

	if (!name || !*name || !parsecolor(value, &c))
		return;
	for (i = 0; i < nseed; i++)
		if (!strcmp(seed[i].name, name)) {
			seed[i].color = c;
			return;
		}
	if (nseed >= MAXPALETTE)
		return;
	snprintf(seed[nseed].name, sizeof seed[0].name, "%s", name);
	seed[nseed].color = c;
	nseed++;
}

void
theme_seedmetrics(int borderpx)
{
	seedborderpx = borderpx;
}

void
theme_defaults(Theme *t)
{
	memset(t, 0, sizeof *t);
	snprintf(t->name, sizeof t->name, "%s", "default");
	t->tone.hue = 0.0;
	t->tone.saturation = 1.0;
	t->tone.lightness = 1.0;
	t->tone.contrast = 1.0;
	t->tone.opacity = 1.0;
	t->borderpx = 2;
	t->barpadding = 0;
	t->taskmaxw = 220;
	t->taskminw = 60;
	t->taskicon = 1;
	t->systrayiconsize = 16;
	t->systrayspacing = 2;
	t->font[0] = '\0';         /* empty means "keep config.h fonts" */
	t->fontfallback[0] = '\0';
	/* colours are filled by resolving the default recipes in theme_parse */
}

static void
resolve(ParseCtx *ctx)
{
	Theme *t = ctx->t;
	ThemeColor c;
	int i, j;

	for (i = 0; i < SchemeLast; i++) {
		for (j = 0; j < THEME_NCOLOR; j++) {
			const char *ref = ctx->recipe[i].ref[j];

			if (!palette_get(ctx, ref, &c) && !parsecolor(ref, &c)) {
				/* Unresolvable reference: fall back to something legible
				 * rather than to whatever happens to be in memory, and say
				 * so. The rest of the theme still applies. */
				fprintf(stderr, "edwm: scheme %s: unknown colour '%s', "
				        "using a safe fallback\n",
				        schemenames[i] ? schemenames[i] : "?", ref);
				c.r = c.g = c.b = (j == ThemeFg) ? 0xffff : 0x2222;
				c.a = 0xffff;
			} else {
				applytone(&t->tone, &c);
			}
			/* Opacity is only meaningful behind text, so it applies to the
			 * background component and leaves fg/border fully opaque. */
			if (j == ThemeBg)
				c.a = (unsigned short)(c.a * t->tone.opacity);
			t->scm[i][j] = c;
		}
	}
}

static int
parse_internal(const char *path, const ThemeTone *force, Theme *t)
{
	ParseCtx ctx;
	Theme staging;
	ThemeColor c;
	size_t i;
	int j;

	theme_defaults(&staging);

	memset(&ctx, 0, sizeof ctx);
	ctx.t = &staging;

	/* seed the palette and recipes with the built-ins, so a theme file only
	 * has to override what it cares about */
	for (i = 0; i < sizeof defpalette / sizeof defpalette[0]; i++) {
		if (parsecolor(defpalette[i].value, &c))
			palette_set(&ctx, defpalette[i].name, c);
	}
	/* config.h wins over the built-ins, and the theme file wins over both */
	for (j = 0; j < nseed; j++)
		palette_set(&ctx, seed[j].name, seed[j].color);
	if (seedborderpx >= 0)
		staging.borderpx = seedborderpx;
	for (i = 0; i < sizeof defrecipe / sizeof defrecipe[0]; i++) {
		j = defrecipe[i].scm;
		snprintf(ctx.recipe[j].ref[0], THEME_MAXNAME, "%s", defrecipe[i].fg);
		snprintf(ctx.recipe[j].ref[1], THEME_MAXNAME, "%s", defrecipe[i].bg);
		snprintf(ctx.recipe[j].ref[2], THEME_MAXNAME, "%s", defrecipe[i].border);
		ctx.recipe[j].set = 1;
	}

	if (path && *path)
		parsefile(&ctx, path);

	if (force)
		staging.tone = *force;

	resolve(&ctx);
	*t = staging;
	return 1;
}

int
theme_parse(const char *path, Theme *t)
{
	return parse_internal(path, NULL, t);
}

int
theme_retone(const char *path, const ThemeTone *tone, Theme *t)
{
	return parse_internal(path, tone, t);
}

int
theme_persistname(const char *name)
{
	char path[PATH_MAX_ISH], tmp[PATH_MAX_ISH + 8], tmp2[PATH_MAX_ISH + 8];
	char line[MAXLINE];
	FILE *in, *out;
	int wrote = 0;

	if (!name || !*name || strchr(name, '/'))
		return 0;

	snprintf(path, sizeof path, "%s", theme_configpath());
	snprintf(tmp, sizeof tmp, "%s.tmp", path);

	mkdir(theme_configdir(), 0755); /* harmless if it exists */
	if (!(out = fopen(tmp, "w"))) {
		fprintf(stderr, "edwm: cannot write %s: %s\n", tmp, strerror(errno));
		return 0;
	}
	if ((in = fopen(path, "r"))) {
		while (fgets(line, sizeof line, in)) {
			char copy[MAXLINE], *s, *eq;

			snprintf(copy, sizeof copy, "%s", line);
			s = copy;
			if ((eq = strchr(s, '#')))
				*eq = '\0';
			s = trim(s);
			if ((eq = strchr(s, '=')) && (*eq = '\0', !strcmp(trim(s), "theme"))) {
				fprintf(out, "theme = %s\n", name);
				wrote = 1;
				continue;
			}
			fputs(line, out);
		}
		fclose(in);
	}
	fclose(out);

	/* No existing line to replace: the preset has to go at the *top*.
	 * A theme file is read in order and later keys win, so "theme = X" acts
	 * as the base that the user's own settings below then customise. Appending
	 * it instead would let the preset silently override everything they
	 * wrote. */
	if (!wrote) {
		FILE *body;
		char c;

		if (!(body = fopen(tmp, "r")))
			return 0;
		snprintf(tmp2, sizeof tmp2, "%s.tmp2", path);
		if (!(out = fopen(tmp2, "w"))) {
			fclose(body);
			return 0;
		}
		fprintf(out, "theme = %s\n", name);
		while ((c = fgetc(body)) != EOF)
			fputc(c, out);
		fclose(body);
		fclose(out);
		unlink(tmp);
		snprintf(tmp, sizeof tmp, "%s", tmp2);
	}

	if (rename(tmp, path) < 0) {
		fprintf(stderr, "edwm: cannot replace %s: %s\n", path, strerror(errno));
		unlink(tmp);
		return 0;
	}
	return 1;
}
