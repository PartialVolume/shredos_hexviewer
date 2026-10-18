#
# Makefile.am for shredos_hexviewer
#

bin_PROGRAMS = shredos_hexviewer

shredos_hexviewer_SOURCES = \
    shredos_hexviewer.h \
    shredos_hexviewer.c \
    shredos_hexviewer_drive.c \
    shredos_hexviewer_ui.c \
    shredos_hexviewer_search.c \
    shredos_hexviewer_entropy.c

shredos_hexviewer_CFLAGS = $(NCURSES_CFLAGS) -Wall -Wextra -O2
shredos_hexviewer_LDADD = $(NCURSES_LIBS) -lm
