/* See LICENSE file for copyright and license details. */
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "img.h"

void
img_free(Img *im)
{
	free(im->rgba);
	im->rgba = NULL;
	im->w = im->h = 0;
}

static int
finishread(png_image *img, Img *out)
{
	unsigned char *buf;

	img->format = PNG_FORMAT_RGBA;
	if (!(buf = malloc(PNG_IMAGE_SIZE(*img)))) {
		png_image_free(img);
		return 0;
	}
	if (!png_image_finish_read(img, NULL, buf, 0, NULL)) {
		free(buf);
		png_image_free(img);
		return 0;
	}
	out->rgba = buf;
	out->w = (int)img->width;
	out->h = (int)img->height;
	png_image_free(img);
	return 1;
}

int
img_loadfile(const char *path, Img *out)
{
	png_image img;

	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path))
		return 0;
	return finishread(&img, out);
}

int
img_loadmem(const unsigned char *data, size_t len, Img *out)
{
	png_image img;

	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, data, len))
		return 0;
	return finishread(&img, out);
}

int
img_fromargb32(const unsigned char *data, int w, int h, Img *out)
{
	int i;

	if (w <= 0 || h <= 0 || !(out->rgba = malloc((size_t)w * h * 4)))
		return 0;
	out->w = w;
	out->h = h;
	for (i = 0; i < w * h; i++) {
		/* ARGB big-endian on the wire -> RGBA bytes here */
		out->rgba[i * 4 + 0] = data[i * 4 + 1];
		out->rgba[i * 4 + 1] = data[i * 4 + 2];
		out->rgba[i * 4 + 2] = data[i * 4 + 3];
		out->rgba[i * 4 + 3] = data[i * 4 + 0];
	}
	return 1;
}

int
img_scale(const Img *src, Img *dst, int dw, int dh)
{
	int x, y, sx0, sx1, sy0, sy1, sx, sy, n;
	unsigned long r, g, b, a;
	const unsigned char *p;
	unsigned char *q;

	if (!src->rgba || dw <= 0 || dh <= 0)
		return 0;
	if (!(dst->rgba = calloc((size_t)dw * dh, 4)))
		return 0;
	dst->w = dw;
	dst->h = dh;

	for (y = 0; y < dh; y++) {
		sy0 = y * src->h / dh;
		sy1 = (y + 1) * src->h / dh;
		if (sy1 <= sy0)
			sy1 = sy0 + 1;
		for (x = 0; x < dw; x++) {
			sx0 = x * src->w / dw;
			sx1 = (x + 1) * src->w / dw;
			if (sx1 <= sx0)
				sx1 = sx0 + 1;
			r = g = b = a = 0;
			n = 0;
			for (sy = sy0; sy < sy1 && sy < src->h; sy++)
				for (sx = sx0; sx < sx1 && sx < src->w; sx++) {
					p = src->rgba + ((size_t)sy * src->w + sx) * 4;
					/* weight colour by alpha, or transparent pixels drag the
					 * average toward black and edges come out dirty */
					r += (unsigned long)p[0] * p[3];
					g += (unsigned long)p[1] * p[3];
					b += (unsigned long)p[2] * p[3];
					a += p[3];
					n++;
				}
			q = dst->rgba + ((size_t)y * dw + x) * 4;
			q[0] = a ? (unsigned char)(r / a) : 0;
			q[1] = a ? (unsigned char)(g / a) : 0;
			q[2] = a ? (unsigned char)(b / a) : 0;
			q[3] = n ? (unsigned char)(a / n) : 0;
		}
	}
	return 1;
}

int
img_fit(Img *im, int size)
{
	Img scaled;

	if (!im->rgba)
		return 0;
	if (im->w == size && im->h == size)
		return 1;
	memset(&scaled, 0, sizeof scaled);
	if (!img_scale(im, &scaled, size, size))
		return 0;
	img_free(im);
	*im = scaled;
	return 1;
}

void
img_draw(Drw *drw, const Img *im, int x, int y, unsigned long bg)
{
	XImage *xi;
	char *buf;
	int px, py, bpl;
	unsigned r, g, b, a, br, bgc, bb;
	const unsigned char *p;

	if (!im->rgba)
		return;
	bpl = im->w * 4;
	if (!(buf = malloc((size_t)bpl * im->h)))
		return;

	/* Unpack the background once, using the visual's own masks. */
	br  = (unsigned)((bg & drw->visual->red_mask) >> 16) & 0xff;
	bgc = (unsigned)((bg & drw->visual->green_mask) >> 8) & 0xff;
	bb  = (unsigned)(bg & drw->visual->blue_mask) & 0xff;

	for (py = 0; py < im->h; py++)
		for (px = 0; px < im->w; px++) {
			p = im->rgba + ((size_t)py * im->w + px) * 4;
			a = p[3];
			r = (p[0] * a + br  * (255 - a)) / 255;
			g = (p[1] * a + bgc * (255 - a)) / 255;
			b = (p[2] * a + bb  * (255 - a)) / 255;
			*(unsigned long *)(buf + (size_t)py * bpl + px * 4) =
				((unsigned long)r << 16) | ((unsigned long)g << 8) | b
				| (drw->depth == 32 ? 0xff000000UL : 0UL);
		}

	xi = XCreateImage(drw->dpy, drw->visual, drw->depth, ZPixmap, 0,
	                  buf, im->w, im->h, 32, bpl);
	if (xi) {
		XPutImage(drw->dpy, drw->drawable, drw->gc, xi, 0, 0, x, y, im->w, im->h);
		XDestroyImage(xi); /* frees buf */
	} else {
		free(buf);
	}
}
