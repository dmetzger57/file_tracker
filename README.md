# File Tracker

A suite of command-line tools for tracking files via SHA-256 checksums stored in SQLite databases. Designed for detecting bit-rot and silent corruption on archival storage drives.

Run `file_tracker` against your storage media periodically to detect unauthorized changes or silent file corruption via checksum verification.

## Tools

### file_tracker

The core engine. Recursively scans directory trees, computes SHA-256 hashes, and stores file metadata in per-path SQLite databases. On subsequent runs, detects new, changed, and missing files by comparing against stored records. Spawns one thread per path for parallel processing.

```
file_tracker -p path1,path2,pathN [-c] [-u] [-P] [-s] [-v]
```

| Option | Description |
|--------|-------------|
| `-p`   | Paths to scan (required, comma-separated). One thread per path. |
| `-c`   | Compare checksums even when file modification time is unchanged. |
| `-u`   | Update the database with changes. Without this, differences are only reported. |
| `-P`   | Show percent-complete progress in 1% increments. |
| `-s`   | Print an aggregate summary when processing completes. |
| `-v`   | Verbose output. |

**Default behavior (no `-c`):** Files are compared by modification time only. If the mtime matches the stored value, the file is marked unchanged without recomputing its hash. Use `-c` for a full checksum verification pass.

### file_locator

Search tracker databases for a specific file by name.

```
file_locator -f filename [-p] [-d database] [-v]
```

| Option | Description |
|--------|-------------|
| `-f`   | Filename to search for (required). |
| `-p`   | Partial/fuzzy match (SQL `LIKE`). |
| `-d`   | Search a specific database file instead of all databases. |
| `-v`   | Verbose output with full file metadata. |

When the same filename appears in multiple databases, `file_locator` flags entries whose checksums differ from the first match.

### ft_summary

Report run history and statistics from a tracker database.

```
ft_summary -d database [-a] [-m] [-c] [-n]
```

| Option | Description |
|--------|-------------|
| `-d`   | Database name, without the `.db` extension (required). |
| `-a`   | Show all recorded runs in a table. Default: last run only. |
| `-m`   | List files found missing in the last run. |
| `-c`   | List files found changed in the last run. |
| `-n`   | List files found new in the last run. |

## Storage Layout

| Path | Contents |
|------|----------|
| `~/db/FileTracker/` | SQLite databases (one per scanned path, named after the path's basename) |
| `~/logs/FileTracker/` | Timestamped log files from each run |
| `~/.rsync-ignore` | Optional ignore list (one entry per line); matched files/directories are skipped |

## Building

Requires GCC, OpenSSL 3, SQLite3, and pthreads.

```sh
make          # build all three tools
make install  # copy binaries to ~/bin
make clean    # remove build artifacts
```

On macOS the Makefile automatically picks up Homebrew paths for OpenSSL and SQLite. On Linux no extra flags are needed if the libraries are installed system-wide.

### Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL (`libssl`, `libcrypto`) | SHA-256 hashing |
| SQLite3 | File metadata storage |
| pthreads | Multi-path parallel scanning |

**macOS (Homebrew):**

```sh
brew install openssl@3 sqlite
```

**Debian/Ubuntu:**

```sh
sudo apt install libssl-dev libsqlite3-dev
```

**Fedora/RHEL:**

```sh
sudo dnf install openssl-devel sqlite-devel
```

## Example Workflow

```sh
# Initial scan — hash all files and record them in the database
file_tracker -p /mnt/archive -u -P -s

# Periodic quick check — compare modification times, update database
file_tracker -p /mnt/archive -u -P -s

# Deep verification — recompute and compare every checksum
file_tracker -p /mnt/archive -c -u -P -s

# Check what changed
ft_summary -d archive -m -c

# Find a file across all tracked volumes
file_locator -f important_document.pdf
```

## License

This project is provided as-is with no warranty. See source files for details.
