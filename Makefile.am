# Makefile.am

ACLOCAL_AMFLAGS=-I admin

AM_CFLAGS=-Wall -Wextra -Wuninitialized -Wshadow -DSYSCONFDIR=\"$(sysconfdir)\"
AM_LDFLAGS=-Wall -Wextra

dist_doc_DATA=README

bin_PROGRAMS=mapper-devusb
mapper_devusb_SOURCES=serial_speed.h mapper-devusb.c

AM_DISTCHECK_CONFIGURE_FLAGS=\
	--with-systemdsystemunitdir=$$dc_install_base/$(systemdsystemunitdir)

EXTRA_DIST=mapper-devusb.service.in

CLEANFILES=mapper-devusb.service

SERVICE_SUBS = s,[@]bindir[@],$(bindir),g

mapper-devusb.service: mapper-devusb.service.in
	sed -e '$(SERVICE_SUBS)' < $< > $@

if HAVE_SYSTEMD
systemdsystemunit_DATA=mapper-devusb.service
AM_CFLAGS+=-DHAVE_SYSTEMD
AM_LDFLAGS+=-lsystemd
endif

install-exec-hook:
	if [ `id -u` -eq 0 ]; then \
		mkdir -p /var/log/mapper-devusb; \
		chown www-data /var/log/mapper-devusb; \
		rm -f /var/arduino; \
		mkfifo /var/arduino; \
		chmod a+w /var/arduino; \
	fi

dist-hook:
	rm -rf `find $(distdir) -name .git`

