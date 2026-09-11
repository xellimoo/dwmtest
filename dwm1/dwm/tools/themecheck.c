/* themecheck.c - validate every theme file without running the WM.
 * Runs the real theme.c parser and the real XftColorAllocName against
 * each theme found in the theme directory: exercises parsing, palette
 * resolution, scheme fallbacks and color allocatability end to end.
 * Prints one line per theme; exit 1 if any theme fails. */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "theme.h"
#include "drw.h"

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "~/.config/edwm/themes";
	const char *builtin[][3] = {
		{ "#bbbbbb", "#222222", "#444444" }, /* Norm */
		{ "#eeeeee", "#005577", "#005577" }, /* Sel */
		{ "#eeeeee", "#005577", "#005577" }, /* TagSel */
		{ "#eeeeee", "#005577", "#005577" }, /* TaskSel */
		{ "#222222", "#ff5555", "#ff5555" }, /* Urg */
		{ "#777777", "#222222", "#444444" }, /* Hid */
		{ "#bbbbbb", "#222222", "#444444" }, /* Status */
	};
	const char *dmenudef[] = { "#222222", "#bbbbbb", "#005577", "#eeeeee" };
	Display *d = XOpenDisplay(NULL);
	Drw drw;
	int i, fail = 0;

	if (!d) {
		fprintf(stderr, "themecheck: cannot open display\n");
		return 2;
	}
	drw.dpy = d;
	drw.screen = DefaultScreen(d);
	if (theme_init(dir, builtin, dmenudef) < 0) {
		fprintf(stderr, "themecheck: theme_init failed\n");
		return 2;
	}
	printf("%d themes in %s\n", theme_count(), dir);
	for (i = 0; i < theme_count(); i++) {
		Clr **scm = calloc(SchemeLast, sizeof(Clr *));
		Theme *t = theme_select(theme_name(i));
		int rc = theme_build(&drw, t, scm);
		if (rc < 0) {
			printf("  FAIL  %s (bad or unallocatable color)\n", theme_name(i));
			fail++;
		} else {
			theme_schemefree(&drw, scm);
			printf("  ok    %s%s\n", theme_name(i),
			       i == 0 ? " (built-in)" : "");
		}
	}
	XCloseDisplay(d);
	return fail ? 1 : 0;
}
