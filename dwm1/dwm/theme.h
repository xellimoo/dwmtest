/* theme.h - runtime color themes for edwm.
 *
 * Themes are INI-like files scanned and parsed once at startup; switching
 * only swaps already-parsed strings, so cycling costs a handful of
 * XftColorAlloc calls and one bar redraw. Two layouts are accepted:
 *
 *   [palette]                      named colors...
 *   bg = #282828
 *   [scheme]                       ...mapped onto scheme slots
 *   sel      = bright, accent, accent
 *   tasksel  = bright, accent, accent
 *
 * where each scheme row is "fg[, bg[, border]]" and every part is either a
 * [palette] name or a literal color; unlisted slots keep the built-in
 * defaults (tagsel falls back to sel, tasksel to sel, status to norm).
 * A flat "norm_fg = #hex" key layout is also accepted.
 *
 * themes[0] is always the synthesized "default" built from config.h's
 * colors[][3], so edwm works with no theme directory at all. */
#ifndef THEME_H
#define THEME_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "drw.h"

enum { SchemeNorm, SchemeSel, SchemeTagSel, SchemeTaskSel, SchemeUrg,
       SchemeHid, SchemeStatus, SchemeLast }; /* color schemes */

typedef struct Theme Theme;

/* dwm.c: resolve and atomically apply a theme (swap scheme, retint borders,
 * tray and dmenu, redraw). dir +1/-1 cycles, 0 applies by name.
 * Returns 0 on success, -1 if the theme is unknown or has bad colors. */
int theme_action(int dir, const char *name);

/* theme.c */
int theme_init(const char *dir, const char *builtin[][3], const char *dmenudef[4]);
void theme_rescan(void);             /* re-read the theme directories */
int theme_count(void);
const char *theme_name(int i);       /* i in [0, theme_count()) */
const char *theme_current(void);     /* name of the cursor theme */
Theme *theme_advance(int dir);       /* move cursor, returns the new theme */
Theme *theme_select(const char *name); /* set cursor by name, NULL when unknown */
int theme_build(Drw *drw, Theme *t, Clr **scm); /* 0 ok, -1 bad colors (never dies) */
void theme_schemefree(Drw *drw, Clr **scm);     /* XftColorFree + free rows + array */
void theme_dmenu_apply(Theme *t);    /* rewrite the dmenu color argv buffers */

/* dmenu argv color slots, referenced by config.h's dmenucmd[]; rewritten in
 * place on every theme change (pointers stay valid, like dmenumon) */
extern char theme_dmenu_nb[32], theme_dmenu_nf[32], theme_dmenu_sb[32], theme_dmenu_sf[32];

#endif
