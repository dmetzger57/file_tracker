# File Tracker GUI Applications

## How to Launch the GUI Apps

The GUI applications are packaged as macOS `.app` bundles. **Double-click these in Finder**:

- **File Tracker.app** - Main file tracking GUI
- **File Tracker Logs.app** - Log viewer GUI  
- **File Tracker Summary.app** - Summary viewer GUI
- **File Tracker Drives.app** - Drive management GUI

These .app bundles will launch **without showing a terminal window**.

## Important: Don't Click the Raw Executables

Do **NOT** double-click these raw executables directly:
- `file_tracker_gui`
- `ft_logs_gui`
- `ft_summary_gui`
- `ft_drives_gui`

These are command-line executables that will always open a terminal window. They are meant to be run from the terminal or wrapped by the .app bundles.

## Installation (Optional)

For easier access, you can:

1. **Move the .app bundles to /Applications**:
   ```bash
   cp -r "File Tracker Logs.app" /Applications/
   ```

2. **Or create aliases/shortcuts** on your Desktop or in Finder favorites

3. **Or add them to your Dock** by dragging the .app bundles to the Dock

## Rebuilding the .app Bundles

If you rebuild the executables, recreate the .app bundles:
```bash
make apps
```

This will rebuild all GUI executables and create fresh .app bundles.
