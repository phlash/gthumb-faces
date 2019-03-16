# Build the faces extension for gThumb
# - example uses autoconf/configure but appears completely busted so we ignore
#   and use gthumb-dev package (thus pkg-config works). *sigh*

GTHUMB_API_VERSION=3.4
CFLAGS=$(shell pkg-config --cflags gthumb-$(GTHUMB_API_VERSION) sqlite3) -I.
LIBS=$(shell pkg-config --libs gthumb-$(GTHUMB_API_VERSION) sqlite3)
TAG=$(shell git describe --dirty=-WIP --tags)
EXT_LIB=/usr/lib/x86_64-linux-gnu/gthumb/extensions
GLIB_SCHEMAS=/usr/share/glib-2.0/schemas

all: build build/libfaces.so build/faces.extension

clean:
	rm -rf build *~ .*~

build:
	mkdir build

build/libfaces.so: build/faces.o
	gcc -o $@ -shared -fPIC $< $(LIBS)

build/faces.extension: faces.extension
	sed -e "s/GIT_TAG/$(TAG)/" < $< > $@

build/%.o: %.c
	gcc -c -o $@ -fPIC $(CFLAGS) $<

install: all
	install -o root -g root -m 755 build/libfaces.so $(EXT_LIB)
	install -o root -g root -m 644 build/faces.extension $(EXT_LIB)
	install -o root -g root -m 644 org.gnome.gthumb.faces.gschema.xml $(GLIB_SCHEMAS)
	glib-compile-schemas $(GLIB_SCHEMAS)

uninstall remove:
	rm -f $(EXT_LIB)/libfaces.so $(EXT_LIB)/faces.extension $(GLIB_SCHEMAS)/org.gnome.gthumb.faces.gschema.xml
	glib-compile-schemas $(GLIB_SCHEMAS)
