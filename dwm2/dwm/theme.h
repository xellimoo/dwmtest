/* See LICENSE file for copyright and license details. */
#ifndef EDWM_THEME_H
#define EDWM_THEME_H

/* Runtime theming.
 *
 * This file deliberately knows nothing about X. It turns a theme file into
 * plain numbers -- resolved colours, font strings and metrics -- and dwm.c
 * does every X call needed to install them. That split keeps window
 * management in one place and makes the parser testable on its own.
 *
 * A theme is a named palette plus scheme "recipes" that reference palette
 * entries, plus a tone transform applied to every palette colour. Retinting
 * the whole WM is then one edit (change `accent`, or nudge [tone] hue) rather
 * than rewriting every scheme by hand.
 */

#include <stddef.h>

/* Colour schemes. SchemeNorm and SchemeSel keep indices 0 and 1, so the
 * existing drw_setscheme call sites in dwm.c are unaffected. */
enum { SchemeNorm, SchemeSel,
       SchemeTagNorm, SchemeTagSel,
       SchemeTaskNorm, SchemeTaskSel, SchemeTaskUrg, SchemeTaskMin,
       SchemeStatus, SchemeSystray,
       SchemeMenu, SchemeMenuSel, SchemeMenuDim,
       SchemeLast };

/* Scheme components, in the same order as drw.h's ColFg/ColBg/ColBorder so a
 * ThemeColor triple maps straight onto a drw scheme. Named separately here to
 * keep this header free of any X dependency. */
enum { ThemeFg, ThemeBg, ThemeBorder, THEME_NCOLOR };

#define THEME_MAXNAME 64
#define THEME_MAXFONT 256

/* Straight (not premultiplied) 16-bit channels, matching XRenderColor's range
 * so dwm.c can hand these to XftColorAllocValue directly. */
typedef struct {
	unsigned short r, g, b, a;
} ThemeColor;

typedef struct {
	double hue;        /* degrees to rotate, -360..360 */
	double saturation; /* multiplier */
	double lightness;  /* multiplier */
	double contrast;   /* push lightness away from mid grey */
	double opacity;    /* alpha applied to scheme backgrounds, 0..1 */
} ThemeTone;

typedef struct {
	char name[THEME_MAXNAME];
	ThemeColor scm[SchemeLast][THEME_NCOLOR];
	char font[THEME_MAXFONT];
	char fontfallback[THEME_MAXFONT];
	ThemeTone tone;
	/* metrics */
	int borderpx;
	int barpadding;
	int taskmaxw, taskminw;
	int taskicon;
	int systrayiconsize, systrayspacing;
} Theme;

/* Fill `t` with the built-in defaults, which reproduce the compiled-in look. */
void theme_defaults(Theme *t);

/* Seed a palette entry before parsing, so the values compiled into config.h
 * remain the defaults and a theme file only overrides what it names. dwm.c
 * seeds bg/fg/bright/accent/border from its colors[][] at startup. Values are
 * colour literals; unparseable ones are ignored. */
void theme_seed(const char *name, const char *value);

/* Seed the default metrics likewise. */
void theme_seedmetrics(int borderpx);

/* Parse `path` on top of the defaults. Returns 1 on success; on failure `t` is
 * left untouched and the reason has been written to stderr, so a broken theme
 * file leaves the running theme in place. */
int theme_parse(const char *path, Theme *t);

/* Like theme_parse, but forces `tone` instead of whatever the file specifies.
 * Used to sweep the tone interactively: re-resolving from the file each time
 * means successive adjustments compose from the base palette rather than
 * stacking on top of already-transformed colours. */
int theme_retone(const char *path, const ThemeTone *tone, Theme *t);

/* Format a colour back as #rrggbbaa, into a caller-supplied buffer of at least
 * 10 bytes. Used to hand colours to child processes such as dmenu. */
const char *theme_colorstr(ThemeColor c, char *buf, size_t len);

/* As above but always #rrggbb, with no alpha. XParseColor -- and therefore
 * dmenu, xsetroot and most other X programs -- rejects the 8-digit form, so
 * anything handed to a child process must use this one. */
const char *theme_colorstr_rgb(ThemeColor c, char *buf, size_t len);

/* ~/.config/edwm, and the theme file inside it. Both return pointers to
 * internal storage that stays valid for the life of the process. */
const char *theme_configdir(void);
const char *theme_configpath(void);

/* Resolve a theme name to themes/<name>.theme under the config dir, or under
 * the system theme dir. Returns 0 if no such file exists. */
int theme_findnamed(const char *name, char *out, size_t len);

/* Rewrite the `theme = <name>` line in the user's config, creating the file if
 * needed, so a theme switch survives a restart. Returns 1 on success. */
int theme_persistname(const char *name);

#endif /* EDWM_THEME_H */
