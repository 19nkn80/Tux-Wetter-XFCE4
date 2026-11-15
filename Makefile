CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

NAME = xfce4-tux-wetter
SOURCES = src/plugin.c src/weather.c
OBJECTS = $(SOURCES:.c=.o)
PKGS = libxfce4panel-2.0 gtk+-3.0 libsoup-3.0 json-glib-1.0
CFLAGS += -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-prototypes \
          -Wmissing-prototypes -Werror=implicit-function-declaration \
          -fPIC $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS += -shared
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: lib$(NAME).so

lib$(NAME).so: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-weather: tests/test-weather.c src/weather.c src/weather.h
	$(CC) $(CFLAGS) -o $@ tests/test-weather.c src/weather.c \
		$(shell $(PKG_CONFIG) --cflags --libs json-glib-1.0) -lm

test: test-weather
	./test-weather

src/%.o: src/%.c src/weather.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: lib$(NAME).so
	install -Dm755 lib$(NAME).so "$(DESTDIR)$(LIBDIR)/xfce4/panel/plugins/lib$(NAME).so"
	install -Dm644 $(NAME).desktop "$(DESTDIR)$(PREFIX)/share/xfce4/panel/plugins/$(NAME).desktop"

clean:
	rm -f $(OBJECTS) lib$(NAME).so test-weather

.PHONY: all install clean test
