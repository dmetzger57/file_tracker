# Drive Tracker GUI Guide

## Overview

The Drive Tracker GUI (`ft_drives_gui`) provides a graphical interface for managing external drive information. It works alongside the command-line `ft_drives` tool and shares the same SQLite database.

## Features

### Main Window Layout

The application uses a two-panel layout:

**Left Panel - Drive List:**
- Searchable list of all tracked drives
- Shows drive name, capacity, and description at a glance
- Click any drive to view full details
- Real-time search filtering

**Right Panel - Drive Details:**
- Drive name (large heading)
- Visual progress bar showing disk usage percentage
- Detailed statistics:
  - Capacity (in human-readable format + bytes)
  - Used space with percentage
  - Available space
  - Description
  - Storage container location
  - Last updated timestamp
  - Last verified timestamp

### Toolbar Actions

**Left Panel Toolbar:**
- **Add Drive** - Opens dialog to manually add a new drive
- **Refresh** - Reload drive list from database
- **Sync from Databases** - Auto-discover and add drives from file_tracker databases

**Right Panel Actions** (when drive selected):
- **Update Drive Info** - Refresh capacity information if drive is mounted
- **Edit Drive** - Manually edit description, capacity, and container fields
- **Mark as Verified** - Update the last verified timestamp
- **Delete Drive** - Remove drive from tracking (requires confirmation)

## Usage Workflows

### Auto-Discovery from file_tracker Databases

The GUI automatically discovers drives that have been scanned with file_tracker:

**Automatic on Startup:**
- Every time you launch ft_drives_gui, it scans `~/db/FileTracker/` for databases
- Any database that doesn't have a corresponding drive entry is automatically added
- This ensures all scanned drives are tracked, even if you never manually added them

**Manual Sync:**
1. Click **Sync from Databases** button
2. GUI scans for new file_tracker databases
3. Shows count of newly discovered drives
4. Drives are added with description "Auto-discovered from file_tracker database"
5. If drive is mounted, capacity is auto-detected

**Benefits:**
- Scan a drive with file_tracker → automatically appears in drives list
- No manual data entry required
- Ensures complete tracking of all scanned drives

### Manually Adding a New Drive

If you want to track a drive before scanning it:

1. Click **Add Drive** button
2. Fill in the form:
   - **Drive Name**: Exact name as it appears when mounted (e.g., "BackupDrive2024")
   - **Description**: Purpose or contents (e.g., "Time Machine backups")
   - **Container**: Physical storage location (e.g., "Drawer A", "Safe")
3. Click **Add**
4. If the drive is currently mounted, capacity info is auto-detected
5. If not mounted, capacity info shows "Not available"

### Viewing Drive Details

1. Type in the search box to filter drives (searches name, description, and container)
2. Click on a drive in the list
3. Full details appear in the right panel
4. All labels are selectable/copyable for easy reference

### Updating Drive Information

1. Select a drive from the list
2. Click **Update Drive Info**
3. If the drive is mounted at `/Volumes/<drive_name>`:
   - Capacity, used, and available space are refreshed
   - Last updated timestamp is updated
4. If not mounted:
   - Only the timestamp is updated
5. A notification shows the result

### Marking Drive as Verified

Use this after running `file_tracker` with checksum verification:

1. Run file_tracker against the drive:
   ```bash
   file_tracker -p /Volumes/MyDrive -u -c
   ```
2. In the GUI, select the drive
3. Click **Mark as Verified**
4. The "Last Verified" timestamp is updated

### Editing Drive Information

Manually edit drive details including description, capacity, and storage location:

1. Select a drive from the list
2. Click **Edit Drive** button
3. The edit dialog appears with current values pre-filled:
   - **Drive Name** (read-only, displayed at top)
   - **Description** - Edit the drive's purpose/contents description
   - **Container** - Edit the physical storage location
   - **Capacity (GB)** - Manually set drive capacity in gigabytes
   - **Used (GB)** - Manually set used space in gigabytes
   - **Available (GB)** - Manually set available space in gigabytes
4. Modify any fields you want to change
5. Click **Save** to update, or **Cancel** to discard changes
6. The drive details refresh to show updated information

**Use Cases:**
- Edit description or container without affecting capacity
- Set capacity values for unmounted drives
- Correct capacity information without mounting the drive
- Update all fields at once

**Note:** Leaving capacity fields empty preserves existing values. Only fields with values are updated in the database.

### Deleting a Drive

Remove a drive from tracking when you no longer need to track it:

1. Select the drive from the list
2. Click **Delete Drive** button (shown in red as a destructive action)
3. A confirmation dialog appears asking you to confirm
4. Click **Delete** to confirm, or **Cancel** to abort
5. The drive is removed from the database and the list
6. The details panel clears

**Note:** Deleting a drive from ft_drives does NOT delete the file_tracker database. It only removes the drive from the drives tracking database.

### Searching for Drives

- Type any text in the search box at the top of the left panel
- Search matches:
  - Drive name
  - Description text
  - Storage container location
- Results update in real-time as you type
- Clear the search box to show all drives

## Integration with CLI Tool

The GUI and CLI tools share the same database (`~/db/FileTracker/drives.db`), so:

- Drives added via CLI appear in GUI (click Refresh)
- Drives added via GUI can be queried via CLI
- Both tools can update the same drive data

Example workflow:
```bash
# Add via CLI
ft_drives add "MyBackup" -d "Family photos" -c "Office shelf"

# View in GUI
ft_drives_gui

# Update via CLI
ft_drives update "MyBackup" -d "Family photos and videos"

# Changes appear in GUI after refresh
```

## Tips

1. **Naming Convention**: Use the exact mount name for drives (check `/Volumes/` directory)
2. **Description**: Be descriptive - you'll search by this later
3. **Container**: Track physical location for large drive collections
4. **Auto-detection**: Mount the drive before adding it to auto-populate capacity
5. **Regular Updates**: Update drive info monthly to track capacity trends
6. **Verification**: Mark as verified after running file_tracker checksums

## Keyboard Shortcuts

- **Ctrl+F** / **Cmd+F**: Focus search box (if implemented by system)
- **Escape**: Close dialogs
- **Enter**: Confirm in dialogs

## Troubleshooting

**GUI doesn't start:**
- Ensure GTK4 is installed: `brew install gtk4`
- Check for errors: run from terminal to see output

**Drive capacity shows "Not available":**
- Drive must be mounted at `/Volumes/<drive_name>`
- Check mount point: `ls /Volumes/`
- Update after mounting to refresh capacity

**Can't find a drive:**
- Use the search box
- Check spelling (case-sensitive for exact matches)
- Click Refresh to reload from database

**Changes don't appear:**
- Click the Refresh button to reload from database
- GUI doesn't auto-refresh when CLI makes changes

## Requirements

- macOS (tested on current version)
- GTK4: `brew install gtk4`
- Shared database with ft_drives CLI tool

## Building from Source

```bash
make ft_drives_gui
```

The Makefile automatically uses `pkg-config` to find GTK4 libraries.
