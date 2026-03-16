# MFS IOC_EVICT Implementation Plan

## Overview

This document describes the plan to add a new `MFS_IOC_EVICT` ioctl command to the MFS filesystem, allowing user-space daemons to evict page cache directly through kernel space instead of using `posix_fadvise`.

## Design Philosophy

- **Non-destructive**: Add new functionality without modifying existing demos
- **Side-by-side comparison**: Keep both `posix_fadvise` and `ioctl` versions
- **Flexibility**: User-space retains full control over eviction timing
- **Extensibility**: Kernel provides capability, user-space makes decisions

---

## Phase 1: Kernel Modifications

### File 1: `include/uapi/linux/mfs.h`

**Add new structure:**
```c
struct mfs_ioc_evict {
    __u64 off;   /* Eviction start offset */
    __u64 len;   /* Eviction length, 0 means entire file */
};
```

**Add ioctl command definition:**
```c
#define MFS_IOC_EVICT   _IOW(0xbc, 4, struct mfs_ioc_evict)
```

**Rationale:**
- UAPI interface requires stability
- Provides unified eviction interface for user-space
- Supports both partial and full-file eviction

---

### File 2: `fs/mfs/cache.c`

**Add eviction function:**
```c
static long _ioc_evict(struct mfs_cache_object *object,
                      struct mfs_ioc_evict *evict)
{
    struct file *file = object->cache_file;
    struct address_space *mapping = file->f_mapping;
    pgoff_t start_index, end_index;

    /* Handle len=0 special case: evict entire file */
    if (evict->len == 0) {
        start_index = 0;
        end_index = ULONG_MAX;
    } else {
        start_index = evict->off >> PAGE_SHIFT;
        end_index = (evict->off + evict->len) >> PAGE_SHIFT;
    }

    /* Call standard Linux page cache eviction function */
    invalidate_mapping_pages(mapping, start_index, end_index);
    return 0;
}
```

**Add ioctl case in `fd_ioctl`:**
```c
case MFS_IOC_EVICT:
{
    struct mfs_ioc_evict evict;

    if (copy_from_user(&evict, (void __user *)arg, sizeof(evict)))
        return -EFAULT;
    ret = _ioc_evict(object, &evict);
    break;
}
```

**Rationale:**
- Encapsulates eviction logic in independent function
- Reuses standard Linux page cache management
- Handles full-file eviction special case

---

### File 3: `fs/mfs/internal.h` (Optional)

**Add function declaration:**
```c
long _ioc_evict(struct mfs_cache_object *object,
                struct mfs_ioc_evict *evict);
```

**Rationale:**
- Maintains coding standards
- Facilitates code review and maintenance

---

## Phase 2: User-Space Modifications

### File 4 (New): `tools/mfs/mfsd_evict_ioctl.c`

Create a new demo file with identical structure to `mfsd_evict_simple.c` but using the new ioctl.

**Key Differences from mfsd_evict_simple.c:**

#### Difference 1: No user-space fd needed

```c
/* Original (mfsd_evict_simple.c): */
struct file_track {
    char *path;
    int mfs_fd;
    int user_fd;     /* ← Needed for posix_fadvise */
    size_t size;
    size_t last_off;
    int valid;
};

/* New (mfsd_evict_ioctl.c): */
struct file_track {
    char *path;
    int mfs_fd;
    /* int user_fd;  Not needed */
    size_t size;
    size_t last_off;
    int valid;
};
```

#### Difference 2: No open_user_fd function

Remove entire function:
```c
/* Remove: static int open_user_fd(const char *path) { ... } */
```

#### Difference 3: Simpler get_file_track

```c
/* Original: */
path = get_file_path(mfs_fd);
files[i].user_fd = open_user_fd(path);  /* ← Opens user-space fd */

/* New: */
path = get_file_path(mfs_fd);
/* No open_user_fd call needed */
```

#### Difference 4: evict_file uses ioctl

```c
/* Original (mfsd_evict_simple.c): */
static int evict_file(int user_fd, const char *path, const char *reason)
{
    ret = posix_fadvise(user_fd, 0, 0, POSIX_FADV_DONTNEED);
    /* ... */
}

/* New (mfsd_evict_ioctl.c): */
static int evict_file(int mfs_fd, const char *path, const char *reason)
{
    struct mfs_ioc_evict evict;

    evict.off = 0;
    evict.len = 0;

    ret = ioctl(mfs_device_fd, MFS_IOC_EVICT, (unsigned long)&evict);
    /* ... */
}
```

#### Difference 5: Global device fd

```c
static int mfs_device_fd = -1;  /* New: Save /dev/mfsX fd */

/* In main(): */
mfs_device_fd = fd;  /* Save for evict_file() */
```

#### Difference 6: Queue stores mfs_fd

```c
/* In evict_pending_files(): */
evict_file(pending->mfs_fd, pending->path, ...);
```

**Complete file structure:**
```c
// SPDX-License-Identifier: GPL-2.0
/*
 * User-space demo of mfs evict using MFS_IOC_EVICT ioctl
 *
 * This demo demonstrates how to use MFS_IOC_EVICT ioctl
 * to evict page cache, which is more efficient than posix_fadvise
 * because it operates directly in kernel space.
 *
 * Design is identical to mfsd_evict_simple, but uses ioctl
 * instead of posix_fadvise for eviction.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/stat.h>

#include "../../include/uapi/linux/mfs.h"
#include "../../include/uapi/linux/magic.h"

#define pr_err(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)

#define MAX_FILES 256
#define PATH_MAX 4096

struct file_track {
    char *path;
    int mfs_fd;
    size_t size;
    size_t last_off;
    int valid;
};

/* ... Rest of implementation identical to mfsd_evict_simple.c ... */
/* But evict_file() uses ioctl instead of posix_fadvise ... */
```

---

### File 5: `tools/mfs/Makefile`

**Add new build target:**
```makefile
PROGS := mfsd mfsd_prefetch mfsd_evict_simple mfsd_evict_ioctl
```

**Rationale:**
- Compile both old and new demos
- Enable comparison testing

---

## Phase 3: Documentation

### File 6 (New): `tools/mfs/README_evict_ioctl.md`

Create new documentation:

```markdown
# mfsd_evict_ioctl - MFS Page Cache Eviction Demo (ioctl version)

## Description

mfsd_evict_ioctl demonstrates page cache eviction using MFS_IOC_EVICT ioctl,
which is more efficient than posix_fadvise as it operates directly in kernel space.

## Comparison with mfsd_evict_simple

| Feature | mfsd_evict_simple | mfsd_evict_ioctl |
|---------|-------------------|-------------------|
| Eviction method | posix_fadvise | MFS_IOC_EVICT ioctl |
| User-space fd required | Yes (user_fd) | No |
| Kernel round trips | More | Fewer |
| Performance | Good | Better |
| Maturity | Proven | New |

## Usage

```bash
# Build
make mfsd_evict_ioctl

# Run
./mfsd_evict_ioctl /var/tmp/mfs/mount /var/tmp/mfs/data
```

## Why use ioctl over posix_fadvise?

1. **No need to open user-space fd**: Eliminates the overhead of opening
   and managing additional file descriptors
2. **Direct kernel operation**: Eviction happens in kernel space without
   crossing user/kernel boundary multiple times
3. **Consistent with MFS architecture**: Uses the same device fd (/dev/mfsX)
   that is already open for event handling

## Implementation Details

The core difference from mfsd_evict_simple is in evict_file():

```c
// Old way (posix_fadvise):
int user_fd = open(path, O_RDONLY);
posix_fadvise(user_fd, 0, 0, POSIX_FADV_DONTNEED);
close(user_fd);

// New way (ioctl):
struct mfs_ioc_evict evict = {.off = 0, .len = 0};
ioctl(mfs_device_fd, MFS_IOC_EVICT, &evict);
```

## Testing

The test_evict_simple script can be used with mfsd_evict_ioctl
by changing the daemon command.
```

---

### File 7 (Update): `tools/mfs/README_evict_simple.md`

**Add comparison note at end:**

```markdown
## Alternative: mfsd_evict_ioctl

For better performance using kernel ioctl, see mfsd_evict_ioctl demo.
It uses MFS_IOC_EVICT ioctl instead of posix_fadvise and does not
require opening user-space file descriptors.

Both demos implement the same eviction strategy and can be used
interchangeably based on preference.
```

---

## File Modification Summary

| File | Operation | Description |
|------|-----------|-------------|
| `include/uapi/linux/mfs.h` | Modify | Add ioctl definitions |
| `fs/mfs/cache.c` | Modify | Implement ioctl handler |
| `fs/mfs/internal.h` | Modify (Optional) | Add function declaration |
| `tools/mfs/mfsd_evict_ioctl.c` | **New** | New demo using ioctl |
| `tools/mfs/Makefile` | Modify | Add new build target |
| `tools/mfs/README_evict_ioctl.md` | **New** | Documentation for new demo |
| `tools/mfs/README_evict_simple.md` | Modify | Add comparison note |

## Files Unchanged

| File | Description |
|------|-------------|
| `tools/mfs/mfsd_evict_simple.c` | **Completely unchanged**, continues using posix_fadvise |
| `tools/mfs/test_evict_simple` | **Completely unchanged** |

---

## Implementation Order

### Order 1: Kernel Layer (Complete first for testing)
1. Modify `include/uapi/linux/mfs.h` - Add UAPI definitions
2. Modify `fs/mfs/cache.c` - Implement ioctl
3. Modify `fs/mfs/internal.h` - Add function declaration (optional)
4. **Compile kernel and test ioctl interface**

### Order 2: User-Space (After kernel testing passes)
5. Create `tools/mfs/mfsd_evict_ioctl.c` - Complete eviction logic replacement
6. Modify `tools/mfs/Makefile` - Add build target
7. **Compile and test daemon**

### Order 3: Verification and Documentation (After feature verification)
8. Create `tools/mfs/README_evict_ioctl.md` - Document new demo
9. Update `tools/mfs/README_evict_simple.md` - Add comparison note
10. Run `tools/mfs/test_evict_simple` - Full flow testing

---

## Verification Plan

### Kernel Layer Verification

```bash
# 1. Compile kernel
make -j$(nproc)

# 2. Install kernel
sudo make modules_install install
sudo reboot

# 3. Test ioctl interface (manual test)
# Create test program to call MFS_IOC_EVICT
# Verify cache eviction using vmtouch
```

### User-Space Verification

```bash
# 1. Compile daemon
cd tools/mfs
make mfsd_evict_ioctl

# 2. Run test script
./test_evict_simple

# 3. Check log output
# Verify EVICT messages are printed correctly
# Verify cache is properly evicted
```

### Comparison Testing

```bash
# 1. Compile both daemons
make mfsd_evict_simple mfsd_evict_ioctl

# 2. Run both daemons simultaneously (for observation)
./mfsd_evict_simple /var/tmp/mfs/mount /var/tmp/mfs/data &
./mfsd_evict_ioctl /var/tmp/mfs/mount /var/tmp/mfs/data &

# 3. Run tests with each daemon separately
# Compare eviction effectiveness

# 4. Performance comparison
# Use time or other tools to compare performance
```

---

## Risks and Mitigations

### Risk 1: Concurrent Access
- **Issue**: Eviction may occur during concurrent read/write
- **Mitigation**: Kernel's `invalidate_mapping_pages` already handles concurrency

### Risk 2: Invalid FD References
- **Issue**: mfs_fd stored in evict_queue may have become invalid
- **Mitigation**: Add fd validity check in evict_file

### Risk 3: Memory Leaks
- **Issue**: path strings may not be freed correctly
- **Mitigation**: Carefully review all free() call sites

### Risk 4: Page Alignment Issues
- **Issue**: Partial eviction may not handle page boundaries correctly
- **Mitigation**: Ensure proper PAGE_SHIFT calculations in _ioc_evict

---

## Design Advantages

### For Original mfsd_evict_simple Users

1. **Zero Disruption**: Original demo remains fully functional
2. **Gradual Migration**: Users can choose when to migrate to ioctl version
3. **Side-by-Side Testing**: Can run both daemons for comparison

### For New Users

1. **Simpler Implementation**: No need to manage user-space fds
2. **Better Performance**: Fewer kernel round trips
3. **Cleaner Architecture**: Consistent with MFS event-driven design

### For Developers

1. **Clear Separation**: Old and new implementations are independent
2. **Easy Testing**: Can verify new functionality without breaking existing code
3. **Low Risk**: New feature isolated from existing stable code

---

## Future Extensions

### Extension 1: Partial Eviction
```c
// Evict first 10MB of file
evict.off = 0;
evict.len = 10 * 1024 * 1024;
ioctl(fd, MFS_IOC_EVICT, &evict);
```

### Extension 2: Eviction Statistics
```c
struct mfs_ioc_evict_info {
    __u64 off;
    __u64 len;
    __u32 bytes_evicted;  // Actual bytes evicted
    __u32 pages_evicted;  // Actual pages evicted
};

#define MFS_IOC_EVICT_INFO _IOWR(0xbc, 5, struct mfs_ioc_evict_info)
```

### Extension 3: Batch Eviction
```c
struct mfs_ioc_batch_evict {
    __u32 count;
    __u32 fds[];
};

#define MFS_IOC_BATCH_EVICT _IOW(0xbc, 6, struct mfs_ioc_batch_evict)
```

---

## Flexibility Advantages of Approach 2

The user-space daemon can trigger eviction based on any policy:

- ✅ Read completion (current mfsd_evict_simple policy)
- ✅ Time windows (e.g., evict after 5 minutes)
- ✅ Memory pressure (e.g., when available memory below threshold)
- ✅ Access frequency (e.g., files not accessed for 1 hour)
- ✅ File size (e.g., prioritize evicting files > 100MB)
- ✅ Business stage (e.g., after model loading completes)
- ✅ Composite policies (e.g., read complete + wait 3s + next file starts)

### Example Scenarios

**Scenario 1: Model Shard Loading**
```
file1: read → upload to GPU → [wait] → evict
                            ↑
                  May need to wait for GPU processing
```
User-space can trigger eviction after GPU upload completes, not immediately after read.

**Scenario 2: Cache Warming**
```
Read multiple files sequentially, keep all in cache
After all files processed, evict all at once
```
Approach 1 would evict after each file completes, preventing this batch delayed eviction.

**Scenario 3: Memory-Aware Eviction**
```
Monitor /proc/meminfo or system events
When memory pressure high, actively evict old files
```
This cannot be implemented through kernel event-driven approach.

---

## Summary

This implementation plan follows the "add new functionality without modifying existing" principle:

- **Kernel**: Add MFS_IOC_EVICT ioctl capability
- **User-space**: Create new demo (mfsd_evict_ioctl) alongside existing (mfsd_evict_simple)
- **Documentation**: Document both approaches and their differences

The key design decision is that kernel provides eviction capability, while user-space makes eviction decisions. This provides maximum flexibility for implementing any eviction policy while keeping the implementation simple and maintainable.
