# Makefile for file_tracker
# make clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I"/opt/homebrew/opt/openssl@3/include" 
LDFLAGS = -L"/opt/homebrew/opt/openssl@3/lib"

# Try to get OpenSSL/SQLite flags from pkg-config if available
PKG_CONFIG ?= pkg-config
SSL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
SSL_LIBS    := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
SQLITE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell $(PKG_CONFIG) --libs sqlite3 2>/dev/null)

# Detect architecture (arm64 = Apple Silicon, x86_64 = Intel)
ARCH := $(shell uname -m)

# Fallback if pkg-config not available
ifeq ($(SSL_CFLAGS),)
ifeq ($(ARCH),arm64)
SSL_CFLAGS = -I/opt/homebrew/opt/openssl@3/include
SSL_LIBS   = -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto
else
SSL_CFLAGS = -I/usr/local/opt/openssl@3/include
SSL_LIBS   = -L/usr/local/opt/openssl@3/lib -lssl -lcrypto
endif
endif

ifeq ($(SQLITE_LIBS),)
SQLITE_LIBS = -lsqlite3
endif

# pthread is always needed
LIBS    = -lpthread $(SQLITE_LIBS) $(SSL_LIBS)
CFLAGS += $(SSL_CFLAGS) $(SQLITE_CFLAGS)

all: file_tracker file_locator ft_summary

file_tracker: file_tracker.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

file_locator: file_locator.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

ft_summary: ft_summary.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGET) *.o

install:
	mv file_tracker file_locator ft_summary ${HOME}/Desktop/bin
