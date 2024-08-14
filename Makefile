prefix = /usr/

CFLAGS = -Wall -ansi -W -std=gnu99 -g -ggdb -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O0 -fno-stack-protector -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include 
LIBS = -lfuse -lglib-2.0

all: haread-fs

haread-fs: haread-fs.c
		@echo "CFLAGS=$(CFLAGS)" | \
				fold -s -w 70 | \
				sed -e 's/^/# /'
		$(CC) -o haread-fs haread-fs.c $(CPPFLAGS) $(CFLAGS) $(LDCFLAGS)  $(LIBS)

install: haread-fs
		install -D haread-fs \
				$(DESTDIR)$(prefix)/bin/haread-fs

clean:
		-rm -f haread-fs

distclean: clean

uninstall:
		-rm -f $(DESTDIR)$(prefix)/bin/haread-fs

.PHONY: all install clean distclean uninstall

