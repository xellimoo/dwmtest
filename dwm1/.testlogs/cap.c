/* cap.c x y w h out.ppm - capture a region of the root window as PPM */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
	Display *d;
	XImage *im;
	FILE *f;
	int x, y, w, h, i, j;
	unsigned long p;
	if (argc != 6) return 1;
	x = atoi(argv[1]); y = atoi(argv[2]); w = atoi(argv[3]); h = atoi(argv[4]);
	d = XOpenDisplay(NULL);
	if (!d) return 2;
	im = XGetImage(d, RootWindow(d, DefaultScreen(d)), x, y, w, h, AllPlanes, ZPixmap);
	if (!im) return 3;
	f = fopen(argv[5], "wb");
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			p = XGetPixel(im, i, j);
			fputc((p >> 16) & 0xff, f); /* red mask 0xff0000 */
			fputc((p >> 8) & 0xff, f);
			fputc(p & 0xff, f);
		}
	fclose(f);
	return 0;
}
