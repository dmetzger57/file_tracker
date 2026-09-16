# File Tracker GUI Application

## How to Launch

The application is packaged as a macOS `.app` bundle. **Double-click in Finder**:

- **File Tracker Unified.app** - All-in-one file tracking application

This `.app` bundle will launch **without showing a terminal window**.

## Features

File Tracker Unified is a tabbed GTK4 application that provides:

1. **File Scanner** - Scan directories and compute SHA-256 checksums
2. **Summary** - View scan history and statistics
3. **Logs** - Browse detailed per-file change logs
4. **Drives** - Track external drive information
5. **Locator** - Search for files across databases
6. **Compare** - Compare two scan runs

## Important: Don't Click the Raw Executable

Do **NOT** double-click the raw executable directly:
- `file_tracker_unified`

This is a command-line executable that will open a terminal window. It's meant to be run from the terminal or wrapped by the `.app` bundle.

## Installation (Optional)

For easier access, you can:

1. **Move the .app bundle to /Applications**:
   ```bash
   cp -r "File Tracker Unified.app" /Applications/
   ```
   Or run the provided install script:
   ```bash
   ./install_apps.sh
   ```

2. **Or create an alias/shortcut** on your Desktop or in Finder favorites

3. **Or add it to your Dock** by dragging the .app bundle to the Dock

## Running from Terminal

You can also run the executable directly from terminal if you prefer:

```bash
./file_tracker_unified
```

## Rebuilding the .app Bundle

If you rebuild the executable, recreate the .app bundle:

```bash
make apps
```

This will rebuild the executable and create a fresh `.app` bundle.

## System Requirements

- macOS (tested on recent versions)
- GTK4 runtime (installed via Homebrew)
- OpenSSL 3 (installed via Homebrew)
- SQLite3 (included with macOS)

If launching the app shows errors about missing libraries, ensure dependencies are installed:

```bash
brew install gtk4 openssl@3
```
