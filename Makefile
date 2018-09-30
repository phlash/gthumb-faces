# Build the faces extension for gThumb
# - example uses autoconf/configure but appears completely busted so we ignore
#   and use gthumb-dev package (thus pkg-config works). *sigh*

GTHUMB_API_VERSION=3.4
CFLAGS=$(shell pkg-config --cflags gthumb-$(GTHUMB_API_VERSION) sqlite3) -I.
LIBS=$(shell pkg-config --libs gthumb-$(GTHUMB_API_VERSION) sqlite3)
EXT_LIB=/usr/lib/x86_64-linux-gnu/gthumb/extensions

all: build build/libfaces.so

clean:
	rm -rf build *~ .*~

build:
	mkdir build

build/libfaces.so: build/faces.o
	gcc -o $@ -shared -fPIC $< $(LIBS)

build/%.o: %.c
	gcc -c -o $@ -fPIC $(CFLAGS) $<

install: all
	install -o root -g root -m 755 build/libfaces.so $(EXT_LIB)
	install -o root -g root -m 644 faces.extension $(EXT_LIB)

uninstall remove:
	rm -f $(EXT_LIB)/libfaces.so $(EXT_LIB)/faces.extension
