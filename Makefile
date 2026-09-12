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

all: file_tracker file_locator ft_summary ft_logs ft_find_dupes ft_drives ft_drives_gui file_tracker_gui ft_summary_gui ft_logs_gui

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

ft_drives: ft_drives.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o ft_drives ft_drives.c $(LIBS)

ft_drives_gui: ft_drives_gui.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o ft_drives_gui ft_drives_gui.c -lsqlite3

file_tracker_gui: file_tracker_gui.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o file_tracker_gui file_tracker_gui.c $(LIBS)

ft_summary_gui: ft_summary_gui.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o ft_summary_gui ft_summary_gui.c -lsqlite3

ft_logs_gui: ft_logs_gui.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o ft_logs_gui ft_logs_gui.c -lsqlite3

clean:
	rm -f file_tracker file_locator ft_summary ft_logs ft_find_dupes ft_drives ft_drives_gui file_tracker_gui ft_summary_gui ft_logs_gui *.o

install:
	mv file_tracker file_locator ft_summary ft_logs ft_find_dupes ft_drives ${HOME}/bin
	@if [ -f ft_drives_gui ]; then mv ft_drives_gui ${HOME}/bin; fi
	@if [ -f file_tracker_gui ]; then mv file_tracker_gui ${HOME}/bin; fi
	@if [ -f ft_summary_gui ]; then mv ft_summary_gui ${HOME}/bin; fi
	@if [ -f ft_logs_gui ]; then mv ft_logs_gui ${HOME}/bin; fi

apps: file_tracker_gui ft_summary_gui ft_drives_gui ft_logs_gui
	./create_app_bundles.sh
