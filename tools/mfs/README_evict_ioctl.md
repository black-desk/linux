# mfsd_evict_ioctl - MFS Page Cache Eviction Demo (ioctl version)

## Description

mfsd_evict_ioctl monitors MFS events and implements a queue-based page cache eviction
strategy using the MFS_IOC_EVICT ioctl. When a file is read to completion,
it is queued for eviction. When a new file starts reading, queued files are evicted.

## Why use ioctl over posix_fadvise?

| Aspect | posix_fadvise | MFS_IOC_EVICT ioctl |
|---------|---------------|---------------------|
| User-space fd required | Yes (need to open actual file) | No |
| Kernel round trips | Multiple (open + advise + close) | Single (ioctl) |
| Performance | Good | Better |
| Consistency with MFS | Less | More (uses /dev/mfsX) |
| Mature | Proven | New |

**Key advantages**:
1. **No need to open user-space fd**: Eliminates the overhead of opening
   and managing additional file descriptors for each file
2. **Direct kernel operation**: Eviction happens directly in kernel space without
   crossing user/kernel boundary multiple times
3. **Consistent with MFS architecture**: Uses the same device fd (/dev/mfsX)
   that is already open for event handling
4. **Simpler code**: No need to manage user_fd lifecycle

## Usage

```bash
# Build
make mfsd_evict_ioctl

# Run
./mfsd_evict_ioctl /var/tmp/mfs/mount /var/tmp/mfs/data
```

**Parameters**:
- First argument: MFS mountpoint
- Second argument: mtree path (data directory for locating actual files)

## Implementation Details

### Core Difference from mfsd_evict_simple

The eviction function uses ioctl instead of posix_fadvise:

```c
// Old way (mfsd_evict_simple):
int user_fd = open(path, O_RDONLY);
posix_fadvise(user_fd, 0, 0, POSIX_FADV_DONTNEED);
close(user_fd);

// New way (mfsd_evict_ioctl):
struct mfs_ioc_evict evict = {.off = 0, .len = 0};
ioctl(mfs_device_fd, MFS_IOC_EVICT, &evict);
```

### File Tracking

Unlike mfsd_evict_simple, this demo does not maintain user_fd:

```c
struct file_track {
    char *path;
    int mfs_fd;      /* Only need MFS internal fd */
    // int user_fd;  /* Not needed */
    size_t size;
    size_t last_off;
    int valid;
};
```

A global `mfs_device_fd` is used for all eviction operations:

```c
static int mfs_device_fd = -1;

/* In main(): */
fd = open(devname, O_RDWR);
mfs_device_fd = fd;  /* Save for ioctl eviction */

/* In evict_file(): */
ret = ioctl(mfs_device_fd, MFS_IOC_EVICT, (unsigned long)&evict);
```

## Features

- Follows existing MFS daemon coding style
- Uses kernel headers: `#include "../../include/uapi/linux/mfs.h"`
- Uses `pr_err()` macro for error output
- Uses `poll()` for infinite wait
- Checks `MFS_SUPER_MAGIC` to verify MFS filesystem
- Uses fixed-size arrays, no dynamic memory allocation
- **Path Mapping**: MFS_IOC_RPATH returns relative path, needs to be prepended
  with mtree path
- **Single FD Mechanism**: Uses global mfs_device_fd for all eviction operations
- **Queue Mechanism**: Files are queued after reading, evicted when new file starts

## Example Output

```
Listening for events...
Will evict page cache when new file starts reading.
(Files read to end are queued until next file starts)

[PATH] mfs_path=/file1 -> full_path=/var/tmp/mfs/data/file1
[TRACK] mfs_fd=3 path=/var/tmp/mfs/data/file1 size=1048576
[PATH] mfs_path=/file2 -> full_path=/var/tmp/mfs/data/file2
[TRACK] mfs_fd=5 path=/var/tmp/mfs/data/file2 size=10485760
[QUEUE] /var/tmp/mfs/data/file1 waiting for next file
[EVICT] /var/tmp/mfs/data/file1 fd=3 (new file read started)
[QUEUE] /var/tmp/mfs/data/file2 waiting for next file
```

## Use Cases

- Sequential reading of multiple large files (e.g., model weight shard files)
- Each file is processed immediately after reading (uploaded to GPU)
- Avoid cache misses caused by eviction during reading
- Free page cache after model weights are loaded to GPU memory

## Comparison with mfsd_evict_simple

Both demos implement the same eviction strategy and can be used
interchangeably. The choice between them depends on:

1. **Performance requirements**: Use ioctl version for better performance
2. **Maturity**: Use posix_fadvise version for proven stability
3. **Preference**: Both work correctly, choose based on needs

## Timeline

```
Application:  read(file1) → ... → read(file1, end) → process file1 → read(file2)
MFS:          [TRACK] → READ → [QUEUE file1] → ... → [TRACK file2] → [EVICT file1]
                                          ↑                      ↑
                                    Queued, not evicted      New file starts, evict queue
```

## Testing

Run the test script to verify the eviction mechanism:

```bash
# Modify test_evict_simple to use mfsd_evict_ioctl instead
# or create a new test script
```

The test will verify that page cache is correctly evicted
when files are queued and later triggered.
