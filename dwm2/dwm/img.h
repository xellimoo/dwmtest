/* See LICENSE file for copyright and license details. */
#ifndef EDWM_IMG_H
#define EDWM_IMG_H

/* Small ARGB image helper, shared by the tray and the menus.
 *
 * Both need the same three things: decode a PNG (from a theme file, or from
 * the bytes an application hands over D-Bus), scale it to the size we want,
 * and paint it against a known solid background because there is no alpha
 * channel to hand to X.
 */

#include <stddef.h>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "drw.h"

/* Straight (non-premultiplied) RGBA, 4 bytes per pixel. */
typedef struct {
	unsigned char *rgba;
	int w, h;
} Img;

void img_free(Img *im);

/* Decode a PNG. Returns 0 and leaves `out` empty on any failure -- a corrupt
 * icon from an application must not be fatal. */
int img_loadfile(const char *path, Img *out);
int img_loadmem(const unsigned char *data, size_t len, Img *out);

/* Decode ARGB32 pixels as they arrive over D-Bus: big-endian, `n` bytes. */
int img_fromargb32(const unsigned char *data, int w, int h, Img *out);

/* Box filter with alpha-weighted averaging, so transparent pixels do not drag
 * the colour toward black. Frees nothing; `dst` must be empty. */
int img_scale(const Img *src, Img *dst, int w, int h);

/* Scale in place to a square of `size`, if it is not already. */
int img_fit(Img *im, int size);

/* Composite onto drw's drawable at (x, y) over the solid colour `bg`. */
void img_draw(Drw *drw, const Img *im, int x, int y, unsigned long bg);

#endif /* EDWM_IMG_H */
