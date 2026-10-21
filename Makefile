CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic
TARGETS := pcf8591_reader light_sensor_server

.PHONY: all clean

all: $(TARGETS)

pcf8591_reader: pcf8591_reader.c
	$(CC) $(CFLAGS) -o $@ $<

light_sensor_server: light_sensor_server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
