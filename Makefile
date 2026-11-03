CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
PKG     := gtk4 gdk-pixbuf-2.0 cairo
BIN     := wallpaper-selector
SRC     := src/main.c
OBJ     := $(SRC:.c=.o)

PKG_CFLAGS := $(shell pkg-config --cflags $(PKG))
PKG_LIBS   := $(shell pkg-config --libs $(PKG))

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(PKG_LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(OBJ)
