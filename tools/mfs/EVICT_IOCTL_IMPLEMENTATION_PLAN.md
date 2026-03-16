# MFS Evict IOCTL 实现方案

## 概述

本方案旨在为MFS添加 `MFS_IOC_EVICT` ioctl接口，使用户态daemon可以通过ioctl直接触发内核的page cache驱逐操作。这种方式相比使用 `posix_fadvise` 更高效，因为直接在内核空间操作，减少了用户/内核边界的跨越。

## 设计原则

- **新增而非修改**：保持原有 `mfsd_evict_simple.c` demo完全不变
- **提供两种方式**：同时支持 posix_fadvise 和 MFS_IOC_EVICT
- **业务灵活性**：驱逐时机完全由用户态控制
- **向后兼容**：不影响现有功能

---

## 第一阶段：内核层修改

### 文件1: `include/uapi/linux/mfs.h`

**修改点1：新增驱逐ioctl结构体**

在 `MFS_IOC_FSINFO` 定义后添加：

```c
struct mfs_ioc_evict {
    __u64 off;   /* 驱逐起始偏移 */
    __u64 len;   /* 驱逐长度，0表示整个文件 */
};
```

**修改点2：新增ioctl命令定义**

在 `MFS_IOC_FSINFO` 定义后添加：

```c
#define MFS_IOC_EVICT   _IOW(0xbc, 4, struct mfs_ioc_evict)
```

**修改理由**：
- UAPI接口，需要保持稳定性
- 为用户态提供统一的驱逐接口
- 支持部分驱逐（指定offset和len）和全文件驱逐（len=0）

---

### 文件2: `fs/mfs/cache.c`

**修改点1：实现驱逐函数**

在 `_ioc_ra()` 函数后添加：

```c
static long _ioc_evict(struct mfs_cache_object *object,
                      struct mfs_ioc_evict *evict)
{
    struct file *file = object->cache_file;
    struct address_space *mapping = file->f_mapping;
    pgoff_t start_index, end_index;

    /* len=0 表示驱逐整个文件 */
    if (evict->len == 0) {
        start_index = 0;
        end_index = ULONG_MAX;
    } else {
        start_index = evict->off >> PAGE_SHIFT;
        end_index = (evict->off + evict->len) >> PAGE_SHIFT;
    }

    /* 调用内核标准的page cache驱逐函数 */
    invalidate_mapping_pages(mapping, start_index, end_index);
    return 0;
}
```

**修改理由**：
- 封装驱逐逻辑到独立函数
- 复用Linux标准的page cache管理机制
- 处理全文件驱逐的特殊情况
- `invalidate_mapping_pages` 已处理并发访问

**修改点2：在fd_ioctl中添加新case**

在 `switch (cmd)` 中，在 `default:` 前添加：

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

**修改理由**：
- 添加新的ioctl命令处理
- 确保参数正确从用户态复制到内核态
- 返回错误码处理

---

### 文件3: `fs/mfs/internal.h`

**修改点：添加函数声明（可选）**

在函数声明区域添加：

```c
long _ioc_evict(struct mfs_cache_object *object,
                struct mfs_ioc_evict *evict);
```

**修改理由**：
- 保持代码规范，在头文件中声明内部函数
- 便于代码审查和维护

---

## 第二阶段：用户态修改

### 文件4（新建）: `tools/mfs/mfsd_evict_ioctl.c`

**完整的新demo文件**，结构与 `mfsd_evict_simple.c` 相似，但使用新ioctl。

**核心差异点对比**：

#### 差异1：file_track结构体

```c
// 原版（mfsd_evict_simple.c）：
struct file_track {
    char *path;
    int mfs_fd;
    int user_fd;     // ← 需要这个
    size_t size;
    size_t last_off;
    int valid;
};

// 新版（mfsd_evict_ioctl.c）：
struct file_track {
    char *path;
    int mfs_fd;
    // int user_fd;  // ← 不需要了
    size_t size;
    size_t last_off;
    int valid;
};
```

#### 差异2：删除open_user_fd函数

```c
// 原版：
static int open_user_fd(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        pr_err("open user fd failed for %s: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

// 新版：完全删除这个函数
```

#### 差异3：get_file_track函数

```c
// 原版：
path = get_file_path(mfs_fd);
if (!path) {
    pr_err("[ERROR] failed to get path for mfs_fd=%d\n", mfs_fd);
    return NULL;
}
files[i].path = path;
files[i].mfs_fd = mfs_fd;
files[i].user_fd = open_user_fd(path);  // ← 需要打开用户态fd

// 新版：
path = get_file_path(mfs_fd);
if (!path) {
    pr_err("[ERROR] failed to get path for mfs_fd=%d\n", mfs_fd);
    return NULL;
}
files[i].path = path;
files[i].mfs_fd = mfs_fd;
// 不再需要open_user_fd
```

#### 差异4：evict_file函数（核心差异）

```c
// 原版（posix_fadvise方式）：
static int evict_file(int user_fd, const char *path, const char *reason)
{
    int ret;

    if (user_fd < 0) {
        pr_err("[ERROR] invalid user_fd for %s\n", path);
        return -1;
    }

    ret = posix_fadvise(user_fd, 0, 0, POSIX_FADV_DONTNEED);
    if (ret == 0) {
        pr_err("[EVICT] %s fd=%d (%s)\n", path, user_fd, reason);
        return 0;
    }
    pr_err("[ERROR] evict %s fd=%d failed: %s\n",
           path, user_fd, strerror(ret));
    return -1;
}

// 新版（ioctl方式）：
static int evict_file(int mfs_fd, const char *path, const char *reason)
{
    struct mfs_ioc_evict evict;
    int ret;

    if (mfs_fd < 0) {
        pr_err("[ERROR] invalid mfs_fd for %s\n", path);
        return -1;
    }

    evict.off = 0;
    evict.len = 0;  // 0表示驱逐整个文件

    ret = ioctl(mfs_device_fd, MFS_IOC_EVICT, (unsigned long)&evict);
    if (ret == 0) {
        pr_err("[EVICT] %s fd=%d (%s)\n", path, mfs_fd, reason);
        return 0;
    }
    pr_err("[ERROR] evict %s fd=%d failed: %s\n",
           path, mfs_fd, strerror(errno));
    return -1;
}
```

**关键区别**：
- 使用 ioctl 而非 posix_fadvise
- 使用全局的 mfs_device_fd（/dev/mfsX的fd）而非每个文件的user_fd
- 错误处理略有不同（errno vs 返回值）

#### 差异5：全局device fd变量

```c
// 新版新增：
static int mfs_device_fd = -1;

// main()中添加：
fd = open(devname, O_RDWR);
if (fd < 0) {
    pr_err("open %s failed\n", devname);
    return -1;
}
mfs_device_fd = fd;  // ← 保存device fd供evict_file使用
```

#### 差异6：队列中存储mfs_fd

```c
// 原版：
evict_queue[slot].path = track->path;
evict_queue[slot].user_fd = track->user_fd;
evict_queue[slot].valid = 1;

// 新版：
evict_queue[slot].path = track->path;
evict_queue[slot].mfs_fd = track->mfs_fd;  // ← 改为mfs_fd
evict_queue[slot].valid = 1;

// evict_pending_files中：
evict_file(pending->mfs_fd, pending->path, "new file read started");
```

#### 差异7：移除close调用

```c
// evict_pending_files函数中：

// 原版：
evict_file(pending->user_fd, pending->path, "new file read started");
free(pending->path);
close(pending->user_fd);  // ← 需要关闭user_fd
pending->valid = 0;

// 新版：
evict_file(pending->mfs_fd, pending->path, "new file read started");
free(pending->path);
// close调用被移除  // ← 不需要close
pending->valid = 0;
```

---

### 文件5: `tools/mfs/Makefile`

**修改点：添加新编译目标**

```makefile
# 原版：
PROGS := mfsd mfsd_prefetch mfsd_evict_simple

# 新版：
PROGS := mfsd mfsd_prefetch mfsd_evict_simple mfsd_evict_ioctl
```

**修改理由**：
- 同时编译新旧两个demo
- 便于对比测试

---

## 第三阶段：文档更新

### 文件6（新建）: `tools/mfs/README_evict_ioctl.md`

**新建完整的文档**：

```markdown
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
```

---

### 文件7（更新）: `tools/mfs/README_evict_simple.md`

**在文档末尾添加对比说明**：

```markdown
## Alternative: mfsd_evict_ioctl

For better performance using kernel ioctl, see `mfsd_evict_ioctl` demo.
It uses `MFS_IOC_EVICT` ioctl instead of `posix_fadvise` and does not
require opening user-space file descriptors.

**Quick comparison**:

| Feature | mfsd_evict_simple | mfsd_evict_ioctl |
|---------|-------------------|-------------------|
| Eviction method | posix_fadvise | MFS_IOC_EVICT ioctl |
| User-space fd required | Yes | No |
| Kernel round trips | More | Fewer |
| Code complexity | Slightly higher | Slightly lower |

Both demos implement the same eviction strategy and can be used
interchangeably based on your preference and requirements.
```

---

## 文件修改汇总

| 文件 | 操作 | 说明 |
|------|------|------|
| `include/uapi/linux/mfs.h` | 修改 | 添加 `struct mfs_ioc_evict` 和 `MFS_IOC_EVICT` 定义 |
| `fs/mfs/cache.c` | 修改 | 实现 `_ioc_evict()` 函数和ioctl处理 |
| `fs/mfs/internal.h` | 修改（可选） | 添加 `_ioc_evict()` 函数声明 |
| `tools/mfs/mfsd_evict_ioctl.c` | **新建** | 新demo，使用ioctl驱逐 |
| `tools/mfs/Makefile` | 修改 | 添加 `mfsd_evict_ioctl` 编译目标 |
| `tools/mfs/README_evict_ioctl.md` | **新建** | 新demo的文档 |
| `tools/mfs/README_evict_simple.md` | 修改 | 添加对比说明 |

---

## 保持不变的文件

| 文件 | 说明 |
|------|------|
| `tools/mfs/mfsd_evict_simple.c` | **完全不变**，继续使用 `posix_fadvise` |
| `tools/mfs/test_evict_simple` | **完全不变**，可与两种demo配合使用 |

---

## 修改顺序建议

### 阶段1：内核层实现（优先）
1. 编译内核并测试ioctl接口是否正确添加
2. 使用简单的测试程序验证ioctl基本功能

### 阶段2：用户态实现
3. 创建 `mfsd_evict_ioctl.c` 文件
4. 编译并测试新demo

### 阶段3：文档和验证
5. 创建和更新文档
6. 完整的功能测试和性能对比

---

## 验证计划

### 验证1：内核ioctl接口
```bash
# 1. 编译内核
cd /path/to/kernel
make -j$(nproc)

# 2. 安装并重启
sudo make modules_install install
sudo reboot

# 3. 手动测试ioctl
# 创建简单测试程序调用MFS_IOC_EVICT
# 使用vmtouch验证cache是否被驱逐
```

### 验证2：新demo功能
```bash
# 编译
cd tools/mfs
make mfsd_evict_ioctl

# 运行测试（可复用test_evict_simple，只需修改启动命令）
./test_evict_simple  # 修改脚本中的daemon启动命令
```

### 验证3：原有demo不受影响
```bash
# 确认原demo仍能正常编译运行
make mfsd_evict_simple
./mfsd_evict_simple /var/tmp/mfs/mount /var/tmp/mfs/data
```

### 验证4：功能等价性
```bash
# 分别使用两个daemon运行相同的测试
# 对比驱逐效果是否一致
# 验证cache状态、内存使用等指标
```

### 验证5：性能对比
```bash
# 使用time或其他性能工具对比
# 关注：CPU使用、系统调用次数、内存占用等
```

---

## 业务灵活性优势

方案2（ioctl方式）保留了完全的业务灵活性，用户态daemon可以根据任意策略触发驱逐：

✅ **基于读取完成**（当前mfsd_evict_ioctl的策略）
✅ **基于时间窗口**（例如：5分钟后驱逐）
✅ **基于内存压力**（例如：当系统可用内存低于阈值时）
✅ **基于访问频率**（例如：未被访问超过1小时的文件）
✅ **基于文件大小**（例如：优先驱逐大于100MB的文件）
✅ **基于业务阶段**（例如：模型加载完成后）
✅ **组合策略**（例如：读取完成 + 等待3秒 + 下一个文件开始）

---

## 风险和注意事项

### 风险1：并发访问
- **问题**：驱逐过程中可能有并发读写
- **缓解**：内核的 `invalidate_mapping_pages` 已处理并发
- **注意**：无需用户态额外同步

### 风险2：fd引用
- **问题**：evict_queue中存储的mfs_fd可能已失效
- **缓解**：在evict_file中添加fd有效性检查
- **现状**：现有demo已有fd有效性检查机制

### 风险3：内存泄露
- **问题**：path字符串没有正确释放
- **缓解**：仔细审查所有free()调用点
- **注意**：直接参考mfsd_evict_simple.c的实现

### 风险4：ioctl兼容性
- **问题**：如果MFS_IOC_EVICT不受支持，新demo无法工作
- **缓解**：在启动时检测ioctl支持，给出友好提示
- **建议**：保持mfsd_evict_simple.c作为fallback

---

## 未来扩展

基于当前的ioctl接口，未来可以轻松扩展：

### 1. 支持部分驱逐
```c
// 用户态可以驱逐文件的前半部分
struct mfs_ioc_evict evict;
evict.off = 0;
evict.len = 10 * 1024 * 1024;  // 10MB
ioctl(fd, MFS_IOC_EVICT, &evict);
```

### 2. 添加驱逐统计信息
```c
struct mfs_ioc_evict_info {
    __u64 off;
    __u64 len;
    __u32 bytes_evicted;  // 实际驱逐的字节数
    __u32 pages_evicted;  // 实际驱逐的页数
};

#define MFS_IOC_EVICT_INFO _IOWR(0xbc, 5, struct mfs_ioc_evict_info)
```

### 3. 添加批量驱逐
```c
struct mfs_ioc_batch_evict {
    __u32 count;
    __u32 fds[0];
};

#define MFS_IOC_BATCH_EVICT _IOW(0xbc, 6, struct mfs_ioc_batch_evict)
```

---

## 总结

本方案通过添加 `MFS_IOC_EVICT` ioctl接口，为MFS提供了一种更高效的page cache驱逐方式。关键特点：

1. **零破坏性**：原有demo完全不受影响
2. **对比验证**：可以同时运行两个demo进行功能和性能对比
3. **渐进式迁移**：用户可以选择继续使用旧方式或迁移到新方式
4. **便于测试**：新功能独立测试，不影响原有稳定性
5. **业务灵活**：驱逐时机完全由用户态控制

这样的设计遵循"新增而非修改"的原则，确保了系统的稳定性和可扩展性。
