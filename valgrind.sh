#!/bin/sh

# mapper-devusb must be compile with debug support => run:
#   ./configure --enable-debug
#   make
valgrind --leak-check=full --show-leak-kinds=all \
         ./mapper-devusb -D -f /var/arduino -l /tmp/mapper-devusb.log /dev/ttyUSB0 \
         2> valgrind.out

