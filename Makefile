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

.PHONY: all clean install apps

all: file_tracker_unified

file_tracker_unified: file_tracker_unified.c
	$(CC) $(CFLAGS) $(LDFLAGS) `pkg-config --cflags --libs gtk4` -o file_tracker_unified file_tracker_unified.c -lssl -lcrypto -lsqlite3

clean:
	rm -f file_tracker_unified *.o

install:
	@echo "Installing File Tracker Unified to /Applications..."
	@if [ -d "File Tracker Unified.app" ]; then \
		echo "Installing File Tracker Unified.app..."; \
		cp -r "File Tracker Unified.app" /Applications/; \
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
