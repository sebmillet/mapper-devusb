# Makefile.am

ACLOCAL_AMFLAGS = -I admin

AM_CFLAGS = -Wall -Wextra -Wuninitialized -Wshadow

#     -Wunreachable-code -Wformat=2 \
#     -D_FORTIFY_SOURCE=2 -fstack-protector --param ssp-buffer-size=4 \
#     -fPIE -pie -Wl,-z,relro,-z,now

dist_doc_DATA=README

bin_PROGRAMS=mapper-devusb
mapper_devusb_SOURCES=serial_speed.h mapper-devusb.c

dist-hook:
	rm -rf `find $(distdir) -name .git`

