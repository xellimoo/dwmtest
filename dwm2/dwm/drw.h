/* See LICENSE file for copyright and license details. */
#ifndef EDWM_DRW_H
#define EDWM_DRW_H

typedef struct {
	Cursor cursor;
} Cur;

typedef struct Fnt {
	Display *dpy;
	unsigned int h;
	XftFont *xfont;
	FcPattern *pattern;
	struct Fnt *next;
} Fnt;

enum { ColFg, ColBg, ColBorder }; /* Clr scheme index */
typedef XftColor Clr;

typedef struct {
	unsigned int w, h;
	Display *dpy;
	int screen;
	Window root;
	Visual *visual;
	unsigned int depth;
	Colormap cmap;
	Drawable drawable;
	GC gc;
	Clr *scheme;
	Fnt *fonts;
	/* caches derived from the current fontset; reset by drw_fontcache_reset()
	 * whenever the fonts change, otherwise a runtime font swap keeps using
	 * measurements taken with the previous font. */
	unsigned int ellipsis_width;
	struct { long codepoint[64]; unsigned int idx; } nomatches;
} Drw;

/* Drawable abstraction */
Drw *drw_create(Display *dpy, int screen, Window win, unsigned int w, unsigned int h,
                Visual *visual, unsigned int depth, Colormap cmap);
void drw_resize(Drw *drw, unsigned int w, unsigned int h);
void drw_free(Drw *drw);

/* Fnt abstraction */
Fnt *drw_fontset_create(Drw* drw, const char *fonts[], size_t fontcount);
void drw_fontset_free(Fnt* set);
void drw_fontcache_reset(Drw *drw);
unsigned int drw_fontset_getwidth(Drw *drw, const char *text);
unsigned int drw_fontset_getwidth_clamp(Drw *drw, const char *text, unsigned int n);
void drw_font_getexts(Fnt *font, const char *text, unsigned int len, unsigned int *w, unsigned int *h);

/* Colorscheme abstraction.
 * These report failure instead of calling die(): a bad colour coming from a
 * theme file reloaded at runtime must never take the window manager down. */
int drw_clr_create(Drw *drw, Clr *dest, const char *clrname);
int drw_clr_create_value(Drw *drw, Clr *dest, XRenderColor rc);
Clr *drw_scm_create(Drw *drw, const char *clrnames[], size_t clrcount);
void drw_scm_free(Drw *drw, Clr *scm, size_t clrcount);

/* Cursor abstraction */
Cur *drw_cur_create(Drw *drw, int shape);
void drw_cur_free(Drw *drw, Cur *cursor);

/* Drawing context manipulation */
void drw_setfontset(Drw *drw, Fnt *set);
void drw_setscheme(Drw *drw, Clr *scm);

/* Drawing functions */
void drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled, int invert);
int drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h, unsigned int lpad, const char *text, int invert);

/* Map functions */
void drw_map(Drw *drw, Window win, int x, int y, unsigned int w, unsigned int h);

#endif /* EDWM_DRW_H */
