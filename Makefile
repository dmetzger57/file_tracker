CC = gcc
CFLAGS = -Wall -Wextra -O2
UNAME_S := $(shell uname -s)

# macOS specific paths (Homebrew)
ifeq ($(UNAME_S), Darwin)
    CFLAGS += -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/opt/sqlite/include
    LDFLAGS += -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/opt/sqlite/lib
endif

# Common Libraries
LIBS = -lssl -lcrypto -lsqlite3 -lpthread

.PHONY: all clean

all: file_tracker file_locator ft_summary ft_logs ft_find_dupes

file_tracker: file_tracker.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o file_tracker file_tracker.c $(LIBS)

file_locator: file_locator.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o file_locator file_locator.c $(LIBS)

ft_summary: ft_summary.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o ft_summary ft_summary.c $(LIBS)

ft_logs: ft_logs.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o ft_logs ft_logs.c $(LIBS)

ft_find_dupes: ft_find_dupes.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o ft_find_dupes ft_find_dupes.c $(LIBS)

clean:
	rm -f file_tracker file_locator ft_summary ft_logs ft_find_dupes *.o

install:
	mv file_tracker file_locator ft_summary ft_logs ft_find_dupes ${HOME}/bin
