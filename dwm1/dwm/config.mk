# dwm version
VERSION = 6.5

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# System detection, done by the shell at make time so that both GNU make
# (Linux) and BSD make (FreeBSD) can build this tree unchanged: the two
# disagree about ifeq/.if syntax but agree about "!=" assignments whose
# right-hand side is a plain shell command. The BSDs install X11, freetype
# and friends under /usr/local, Linux distributions under /usr.
OPSYS != uname -s
X11PREFIX != case $(OPSYS) in FreeBSD|OpenBSD|NetBSD|DragonFly) echo /usr/local;; *) echo /usr;; esac

X11INC = ${X11PREFIX}/include
X11LIB = ${X11PREFIX}/lib

# Xinerama, comment if you don't want it
XINERAMALIBS  = -lXinerama
XINERAMAFLAGS = -DXINERAMA

# freetype
FREETYPELIBS = -lfontconfig -lXft
FREETYPEINC = ${X11PREFIX}/include/freetype2
#MANPREFIX = ${PREFIX}/man

# dbus (session bus bootstrap + org.edwm control service)
# "!=" runs the command at parse time and works in both GNU make and BSD
# make (FreeBSD); "$(shell ...)" would silently expand to nothing under
# BSD make and the headers would not be found. "|| true" keeps the build
# going (with dbus disabled) where pkg-config is not installed.
DBUSFLAGS != pkg-config --cflags dbus-1 2>/dev/null || true
DBUSLIBS != pkg-config --libs dbus-1 2>/dev/null || true

# includes and libs
INCS = -I${X11INC} -I${FREETYPEINC} ${DBUSFLAGS}
LIBS = -L${X11LIB} -lX11 ${XINERAMALIBS} -lXi ${FREETYPELIBS} ${DBUSLIBS}

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DVERSION=\"${VERSION}\" ${XINERAMAFLAGS}
#CFLAGS   = -g -std=c99 -pedantic -Wall -O0 ${INCS} ${CPPFLAGS}
CFLAGS   = -std=c99 -pedantic -Wall -Wno-deprecated-declarations -Os ${INCS} ${CPPFLAGS}
LDFLAGS  = ${LIBS}

# Solaris
#CFLAGS = -fast ${INCS} -DVERSION=\"${VERSION}\"
#LDFLAGS = ${LIBS}

# compiler and linker
CC = cc
