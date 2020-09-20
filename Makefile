# Makefile

# Makefile for mapper-devusb

# Copyright 2019, 2020 Sébastien Millet

CFLAGS = -O3 -Wall -Wextra -Wuninitialized -Wshadow

all: executable

fortified: CFLAGS += -Wunreachable-code -Wformat=2 \
	-D_FORTIFY_SOURCE=2 -fstack-protector --param ssp-buffer-size=4 \
	-fPIE -pie -Wl,-z,relro,-z,now
fortified: executable

debug: CFLAGS += -DDEBUG -g
debug: executable

executable: mapper-devusb

mapper-devusb: mapper-devusb.c
	gcc -o $@ $(CFLAGS) $<

.PHONY: clean mrproper

mrproper: clean

clean:
	rm -f mapper-devusb

