# mfsd_evict_simple - MFS Page Cache Eviction Demo

## Description

mfsd_evict_simple monitors MFS events and implements a queue-based page cache eviction strategy. When a file is read to completion, it is queued for eviction. When a new file starts reading, queued files are evicted. This is particularly useful for model weight loading scenarios.

**Design Principle**:
- Files are not evicted immediately after reading completes, allowing the application time to process data (e.g., mmap access)
- When a new file starts reading, it indicates the previous file has been processed, making eviction safe
- Optimized for sequential model weight loading scenarios

## Usage

```bash
# Build
make mfsd_evict_simple

# Run
./mfsd_evict_simple /var/tmp/mfs/mount /var/tmp/mfs/data
```

**Parameters**:
- First argument: MFS mountpoint
- Second argument: mtree path (data directory for locating actual files)

## Example Output

```
Listening for events...
Will evict page cache when new file starts reading.
(Files read to end are queued until next file starts)

[PATH] mfs_path=/file1 -> full_path=/var/tmp/mfs/data/file1
[TRACK] mfs_fd=3 user_fd=4 path=/var/tmp/mfs/data/file1 size=1048576
[PATH] mfs_path=/file2 -> full_path=/var/tmp/mfs/data/file2
[TRACK] mfs_fd=5 user_fd=6 path=/var/tmp/mfs/data/file2 size=10485760
[QUEUE] /var/tmp/mfs/data/file1 waiting for next file
[EVICT] /var/tmp/mfs/data/file1 fd=4 (new file read started)
[QUEUE] /var/tmp/mfs/data/file2 waiting for next file
```

## Core Logic

```c
// 1. Track files using fixed-size array
static struct file_track files[256];
// file_track contains: path, mfs_fd, user_fd, size, last_off, valid

// 2. Use queue to store files pending eviction
static struct file_track evict_queue[256];
static int evict_queue_count = 0;
static int current_mfs_fd = -1;  // Currently reading MFS fd

// 3. Get full file path (MFS_IOC_RPATH returns relative path, prepend mtree)
char *full_path = get_file_path(mfs_fd);  // /file1 -> /var/tmp/mfs/data/file1

// 4. Open userspace file descriptor
int user_fd = open(full_path, O_RDONLY);

// 5. Check if this is a new file
if (current_mfs_fd != mfs_fd) {
    current_mfs_fd = mfs_fd;
    evict_pending_files();  // New file starts, evict queued files
}

// 6. Check if file read to end
if (track->last_off >= track->size) {
    // Add to eviction queue, not evicted immediately
    evict_queue[slot].path = track->path;
    evict_queue[slot].user_fd = track->user_fd;
    evict_queue[slot].valid = 1;
    evict_queue_count++;
}

// 7. Use userspace fd for eviction
posix_fadvise(user_fd, 0, 0, POSIX_FADV_DONTNEED);
```

## Features

- Follows existing MFS daemon coding style
- Uses kernel headers: `#include "../../include/uapi/linux/mfs.h"`
- Uses `pr_err()` macro for error output
- Uses `poll()` for infinite wait
- Checks `MFS_SUPER_MAGIC` to verify MFS filesystem
- Uses fixed-size arrays, no dynamic memory allocation
- **Path Mapping**: MFS_IOC_RPATH returns relative path (e.g., `/file1`), needs to be prepended with mtree path (e.g., `/var/tmp/mfs/data/file1`)
- **Dual FD Mechanism**: Uses mfs_fd for event tracking, user_fd for posix_fadvise
- **Queue Mechanism**: Files are queued after reading, evicted when new file starts

## Use Cases

- Sequential reading of multiple large files (e.g., model weight shard files)
- Each file is processed immediately after reading (uploaded to GPU), then read next file
- Avoid cache misses caused by eviction during reading (especially mmap scenarios)
- Free page cache after model weights are loaded to GPU memory

## Timeline

```
Application:  read(file1) → ... → read(file1, end) → process file1 → read(file2)
MFS:          [TRACK] → READ → [QUEUE file1] → ... → [TRACK file2] → [EVICT file1]
                                          ↑                      ↑
                                    Queued, not evicted      New file starts, evict queue
```

This strategy ensures:
- file1 is not evicted immediately after reading, giving application time to process
- When reading file2 starts, it indicates file1 has been processed, making eviction safe

## Testing

Run the test script to verify the eviction mechanism:

```bash
./test_evict_simple
```

The test script will:
1. Create MFS mount and three 1MB files
2. Start mfsd_evict_simple daemon
3. Read files sequentially
4. Verify eviction behavior using vmtouch
