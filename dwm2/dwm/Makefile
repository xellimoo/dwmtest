# dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = dbusif.c dbusmenu.c drw.c dwm.c img.c menu.c session.c sni.c systray.c theme.c util.c
OBJ = ${SRC:.c=.o}

all: dwm

.c.o:
	${CC} -c ${CFLAGS} $<

HDR = dbusif.h dbusmenu.h drw.h img.h menu.h session.h sni.h systray.h theme.h util.h

${OBJ}: config.h config.mk ${HDR}

config.h:
	cp config.def.h $@

dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

# The tray test clients, for checking the tray without hunting for a real
# applet. Not built by `all`. Named test-clients rather than tests so the
# target does not collide with the directory of the same name.
test-clients: tests/traytest

tests/traytest: tests/traytest.c
	${CC} -o $@ tests/traytest.c `pkg-config --cflags --libs x11`

clean:
	rm -f dwm ${OBJ} dwm-${VERSION}.tar.gz tests/traytest

dist: clean
	mkdir -p dwm-${VERSION}
	cp -R LICENSE Makefile README config.def.h config.mk\
		dwm.1 dbusif.h dbusmenu.h drw.h img.h menu.h session.h sni.h systray.h theme.h util.h ${SRC} dwm.png transient.c\
		edwmctl themes theme.conf.example theme.conf.sample TESTING\
		tests dwm-${VERSION}
	tar -cf dwm-${VERSION}.tar dwm-${VERSION}
	gzip dwm-${VERSION}.tar
	rm -rf dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f dwm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/dwm
	cp -f edwmctl ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/edwmctl
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dwm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1
	mkdir -p ${DESTDIR}${THEMEDIR}
	cp -f themes/*.theme ${DESTDIR}${THEMEDIR}
	chmod 644 ${DESTDIR}${THEMEDIR}/*.theme

# Drop a starter theme.conf into the user's config dir. Never overwrites.
# The directory is resolved by the shell rather than by make, since the two
# makes disagree about conditional functions.
install-config:
	@d="$${XDG_CONFIG_HOME:-$$HOME/.config}/edwm"; \
	mkdir -p "$$d/themes"; \
	for t in themes/*.theme; do \
		[ -f "$$d/themes/$${t##*/}" ] || cp "$$t" "$$d/themes/"; \
	done; \
	[ -f "$$d/theme.conf" ] || cp theme.conf.example "$$d/theme.conf"; \
	echo "edwm config in $$d"

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dwm\
		${DESTDIR}${PREFIX}/bin/edwmctl\
		${DESTDIR}${MANPREFIX}/man1/dwm.1
	rm -rf ${DESTDIR}${THEMEDIR}

.PHONY: all clean dist install install-config test-clients uninstall
