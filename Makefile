CC = gcc
CFLAGS = -Wall -Wextra -O2 -Wno-deprecated-declarations
UNAME_S := $(shell uname -s)

# macOS specific paths (Homebrew)
ifeq ($(UNAME_S), Darwin)
    CFLAGS += -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/opt/sqlite/include
    LDFLAGS += -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/opt/sqlite/lib
endif

# Common Libraries
LIBS = -lssl -lcrypto -lsqlite3 -lpthread

.PHONY: all clean

all: file_tracker_unified

file_tracker_unified: file_tracker_unified.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o file_tracker_unified file_tracker_unified.c -lssl -lcrypto -lsqlite3

clean:
	rm -f file_tracker_unified *.o

install:
	@if [ -f file_tracker_unified ]; then mv file_tracker_unified ${HOME}/bin; fi

apps: file_tracker_unified
	./create_app_bundles.sh
