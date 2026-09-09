# edwm version
VERSION = 6.5

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man
# where `make install` puts the bundled theme presets, and where edwm looks for
# them when the user's own config dir has no match
THEMEDIR = ${PREFIX}/share/edwm/themes

# Library flags come from pkg-config rather than hardcoded paths, which is what
# lets one config.mk work on both Linux (/usr) and the BSDs (/usr/local)
# without editing. The backticks are evaluated by the shell when a compile
# runs, so this works under GNU make and BSD make alike.
#
# Which file-change API is used for live theme reload -- inotify or kqueue --
# is decided by the compiler's own platform macros in dwm.c, so there is
# nothing to configure here.

# Xinerama, comment out both to build without it
XINERAMALIBS  = `pkg-config --libs xinerama`
XINERAMAFLAGS = -DXINERAMA

# XInput2, for the two-button chord drag
XINPUT2LIBS = `pkg-config --libs xi`

FREETYPELIBS = `pkg-config --libs xft fontconfig`

# libpng decodes tray and menu icons. Not optional -- it is used whenever an
# icon is drawn -- but it costs nothing new, being already pulled in by
# freetype.
PNGLIBS  = `pkg-config --libs libpng`
PNGFLAGS = `pkg-config --cflags libpng`

# D-Bus. Comment both out to build without libdbus: edwm still runs, and
# theming is still scriptable through the _EDWM_CMD root property, but it
# stops offering the org.edwm.WM service, the StatusNotifierItem tray and
# applet menus.
DBUSLIBS  = `pkg-config --libs dbus-1`
DBUSFLAGS = `pkg-config --cflags dbus-1` -DEDWM_DBUS

# includes and libs
INCS = `pkg-config --cflags x11 xft xinerama xi fontconfig` ${PNGFLAGS} ${DBUSFLAGS}
LIBS = `pkg-config --libs x11` ${XINERAMALIBS} ${XINPUT2LIBS} ${FREETYPELIBS} \
       ${PNGLIBS} ${DBUSLIBS}

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DVERSION=\"${VERSION}\" ${XINERAMAFLAGS}\
	   -DEDWM_THEMEDIR=\"${THEMEDIR}\"
#CFLAGS   = -g -std=c99 -pedantic -Wall -O0 ${INCS} ${CPPFLAGS}
CFLAGS   = -std=c99 -pedantic -Wall -Wno-deprecated-declarations -Os ${INCS} ${CPPFLAGS}
LDFLAGS  = ${LIBS}

# compiler and linker
CC = cc
