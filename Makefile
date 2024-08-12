# You can change the PREFIX when installing by running make install PREFIX=/your/path where /your/path is your desired installation path.

CC = gcc
CFLAGS = -Wall -ansi -W -std=gnu99 -g -ggdb -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O0 -fno-stack-protector -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include 
LIBS = -lfuse -lglib-2.0
TARGET = haread-fs
SOURCE =  haread-fs.c
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): $(SOURCE) 
	$(CC) -o $(TARGET) $(SOURCE)  $(CFLAGS)  $(LIBS)

install:
	install -d $(PREFIX)/bin/
	install -m 755 $(TARGET) $(PREFIX)/bin/

clean:
	rm -f $(TARGET)