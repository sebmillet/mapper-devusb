#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

#
# Copyright 2020 Sébastien Millet
#

AC_PREREQ([2.69])
AC_INIT([mapper-devusb], [1.0], [milletseb@laposte.net])
AC_CONFIG_SRCDIR([mapper-devusb.c])
AC_CONFIG_AUX_DIR([admin])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h sys/time.h termios.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_CHECK_FUNCS([atexit gettimeofday mkfifo select strerror])

AC_ARG_ENABLE(debug,
			  [  --enable-debug          enable debugging information],
			  enable_debug="$enableval", enable_debug="no")

if test "${enable_debug}" = yes ; then
	CFLAGS="$CFLAGS -DDEBUG"
fi

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

