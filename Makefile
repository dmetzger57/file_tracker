CC = gcc
CFLAGS = -Wall -Wextra -O2 -Wno-deprecated-declarations
UNAME_S := $(shell uname -s)

# macOS specific paths (Homebrew)
ifeq ($(UNAME_S), Darwin)
    CFLAGS += -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/opt/sqlite/include
    LDFLAGS += -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/opt/sqlite/lib
    # Dock menu ("New Window") for the unified app
    UNIFIED_MACOS_SRC = macos_dock_menu.m
    UNIFIED_MACOS_LIBS = -framework Cocoa
endif

# Common Libraries
LIBS = -lssl -lcrypto -lsqlite3 -lpthread

.PHONY: all clean install apps

all: file_tracker_unified file_tracker

file_tracker: file_tracker.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o file_tracker file_tracker.c -lssl -lcrypto -lsqlite3

file_tracker_unified: file_tracker_unified.c $(UNIFIED_MACOS_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o file_tracker_unified file_tracker_unified.c $(UNIFIED_MACOS_SRC) -lssl -lcrypto -lsqlite3 $(UNIFIED_MACOS_LIBS)

clean:
	rm -f file_tracker_unified file_tracker *.o

install:
	@echo "Installing File Tracker Unified to /Applications..."
	@if [ -d "File Tracker Unified.app" ]; then \
		echo "Installing File Tracker Unified.app..."; \
		rm -rf "/Applications/File Tracker Unified.app"; \
		cp -R "File Tracker Unified.app" /Applications/; \
		echo ""; \
		echo "Installation complete!"; \
		echo "File Tracker Unified is now available in your Applications folder."; \
		echo "You can also add it to your Dock by dragging it from /Applications."; \
	else \
		echo "Error: File Tracker Unified.app not found."; \
		echo "Run 'make apps' first to create the app bundle."; \
		exit 1; \
	fi

apps: file_tracker_unified
	./create_app_bundles.sh
