#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

#
# Copyright 2020 Sébastien Millet
#

AC_PREREQ([2.69])
AC_INIT([mapper-devusb], [1.1], [milletseb@laposte.net])
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

dnl ==================== debug option ===============================

AC_ARG_ENABLE(debug,
              [  --enable-debug          enable debugging information],
              enable_debug="$enableval", enable_debug="no")

if test "${enable_debug}" = yes ; then
    CFLAGS="$CFLAGS -DDEBUG"
fi

dnl ==================== fortified generation =======================

AC_ARG_ENABLE(fortified,
              [  --enable-fortified      compilation with options to make running code more robust],
              enable_fortified="$enableval", enable_fortified="yes")

if test "${enable_fortified}" = yes ; then
    CFLAGS="$CFLAGS -Wunreachable-code -Wformat=2 -D_FORTIFY_SOURCE=2 \
-fstack-protector --param ssp-buffer-size=4 -fPIE -pie \
-Wl,-z,relro,-z,now"
fi

dnl ==================== systemd service ============================
dnl From
dnl   https://stackoverflow.com/questions/58017453/install-systemd-service-using-autotools

PKG_PROG_PKG_CONFIG
AC_ARG_WITH([systemdsystemunitdir],
     [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files])],,
     [with_systemdsystemunitdir=auto])
AS_IF([test "x$with_systemdsystemunitdir" = "xyes" -o "x$with_systemdsystemunitdir" = "xauto"], [
     def_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)

     AS_IF([test "x$def_systemdsystemunitdir" = "x"],
   [AS_IF([test "x$with_systemdsystemunitdir" = "xyes"],
    [AC_MSG_ERROR([systemd support requested but pkg-config unable to query systemd package])])
    with_systemdsystemunitdir=no],
   [with_systemdsystemunitdir="$def_systemdsystemunitdir"])])
AS_IF([test "x$with_systemdsystemunitdir" != "xno"],
      [AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])])
AM_CONDITIONAL([HAVE_SYSTEMD], [test "x$with_systemdsystemunitdir" != "xno"])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

