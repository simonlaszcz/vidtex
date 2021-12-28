AC_INIT([vidtex], [1.0.0])
AX_WITH_CURSES
AC_ARG_WITH([ncursesw], AC_MSG_ERROR([ncursesw not installed]))
AC_CONFIG_SRCDIR([src/main.c])
AM_INIT_AUTOMAKE([subdir-objects])
AC_PROG_CC
AC_CONFIG_FILES([Makefile])
AC_CONFIG_HEADERS([src/config.h])
AH_TEMPLATE([SYSCONFDIR], [Path to global config file])
AC_DEFINE([SYSCONFDIR], [$sysconfdir])
AC_OUTPUT
