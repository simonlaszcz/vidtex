man_MANS=src/vidtex.1
bin_PROGRAMS=vidtex

configdir=${sysconfdir}/vidtex
config_DATA=src/vidtexrc

vidtex_CFLAGS=-g @CURSES_CFLAGS@ -DSYSCONFDIR=\"${configdir}\"
vidtex_LDADD=@CURSES_LIBS@
vidtex_SOURCES=\
	src/bedstead.c \
	src/bedstead.h \
	src/decoder.c \
	src/decoder.h \
	src/galax.c \
	src/galax.h \
	src/main.c \
	src/telesoft.c \
	src/telesoft.h \
	src/vidtexrc \
	src/vidtex.1
