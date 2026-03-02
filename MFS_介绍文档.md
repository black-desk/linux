# MFS 文件系统 介绍

## 1. 概述

MFS (Memory File System) 是一个可堆叠（stackable）的 Linux 文件系统，利用底层（lower）和缓存（cache）两层结构，为用户提供可编程的缓存能力。

### 核心特性

- **可堆叠文件系统**：MFS 可以挂载在其他文件系统（如 ext4、xfs）之上
- **只读操作**：仅支持普通文件、目录和符号链接的只读操作
- **数据一致性**：底层文件系统必须保持只读状态，防止数据不一致
- **三种工作模式**：none、local、remote
- **事件驱动架构**：通过字符设备向用户空间发送 MISS 事件
- **禁用默认预读**：完全由用户空间控制预取策略

### 使用场景（来自官方文档）

1. **加速模型权重加载**：用户态守护进程可以采用多种策略提升性能，如并发加载、更大 I/O 大小的 read-ahead、NUMA 感知分配、基于追踪的预取等
2. **追踪读取 I/O**：用户态守护进程可以解析消息中的 offset 和 length，记录运行进程的 MISS I/O

---

## 2. 挂载选项

### 2.1 基本挂载语法

```bash
mount -t mfs -o mode=<mode>,mtree=<path>,cachedir=<path> /dev/null <mountpoint>
```

### 2.2 挂载选项详解

| 选项 | 描述 | 说明 |
|------|------|------|
| mode=%s | 运行模式 | none、local、remote |
| mtree=%s | 底层路径 | 指定元数据源 |
| cachedir=%s | 缓存路径 | 指定数据源 |

### 2.3 运行模式

| 模式 | 描述 | 事件类型 | 缓存检查方式 |
|------|------|---------|-------------|
| none | 纯堆叠模式，直接透传操作到底层文件系统 | 无事件 | 不检查缓存 |
| local | 本地模式，lower 和 cachedir 都是本地文件系统 | 异步事件 | 检查 page cache |
| remote | 远程模式，目标数据在远程存储（如 OBS 或其他无 POSIX 接口的分布式文件系统） | 同步事件 | 使用 SEEK_HOLE/SEEK_DATA 检查本地磁盘缓存 |

### 2.4 路径限制

**重要**：`mtree` 和 `cachedir` 选项中的路径不能与挂载点相同，也不能互为子目录。

**Local/Remote 模式限制**：
- **local/none 模式**：mtree 和 cachedir **必须相同**
- **remote 模式**：mtree 和 cachedir **必须不同**，且 mtree 不能是 cachedir 的父目录

---

## 3. 通信框架

### 3.1 字符设备

每个 MFS 实例都有一个唯一的通信设备：`/dev/mfs${minor}`

获取 minor 号的方法：
```c
struct statfs buf;
statfs(mountpoint, &buf);
unsigned int minor = buf.f_spare[0];  // 从 f_spare[0] 获取
```

### 3.2 消息头结构

每个请求以消息头开头：

```c
struct mfs_msg {
    __u8 version;  // 版本号，用于扩展
    __u8 opcode;   // 事件类型
    __u16 len;     // 事件总长度（包括头和载荷）
    __u32 fd;      // 内部文件对象的文件句柄
    __u32 id;      // 事件的唯一 ID
    __u8 data[];   // 事件载荷
};
```

### 3.3 事件载荷结构（读取事件）

MFS 只在本地缓存（内存或磁盘）中数据缺失时发送读取事件：

```c
struct mfs_read {
    __u64 off;   // 触发此事件的读取请求偏移量
    __u64 len;   // 触发此事件的读取请求长度
    __s32 pid;   // 触发此事件的读取进程 PID
};
```

### 3.4 事件类型

```c
enum mfs_opcode {
    MFS_OP_READ = 0,   // 正常读取事件
    MFS_OP_FAULT,       // 页面错误事件
};
```

---

## 4. 运行模式详解

### 4.1 获取文件系统信息

用户态守护进程可以通过 ioctl 获取文件系统信息：

```c
struct mfs_ioc_fsinfo {
    __u8 mode;  // 0: none, 1: local, 2: remote
};

ioctl(fd, MFS_IOC_FSINFO, &fsinfo);
```

对应的枚举：
```c
enum {
    MFS_MODE_NONE = 0,
    MFS_MODE_LOCAL,
    MFS_MODE_REMOTE,
};
```

### 4.2 NONE 模式

- **行为**：直接将操作透传到底层文件系统
- **事件**：不报告任何事件
- **适用场景**：仅作为可堆叠文件系统使用

### 4.3 LOCAL 模式

- **缓存**：使用 page cache 作为本地缓存
- **事件**：当读取请求导致缓存未命中时，为非连续的缺失范围报告 MISS 事件
- **事件性质**：**异步事件**，内核不会阻塞等待
- **用户态处理**：用户态守护进程可以基于此事件预取后续数据，避免未来的缓存未命中

**LOCAL 模式事件流程**：
```
应用 read() / mmap 访问
    │
    ▼
检查 page cache
    │
    ├─ 命中 → 直接返回数据
    │
    └─ 未命中 → 异步发送 MISS 事件 → 用户态守护进程
                     │
                     ▼
              预取后续数据（可选）
                     │
                     ▼
              调用 MFS_IOC_RA 触发 readahead
```

### 4.4 REMOTE 模式

- **缓存**：使用本地磁盘（由 `cachedir` 指定）作为缓存
- **事件检查**：使用 `SEEK_HOLE` 和 `SEEK_DATA` 检查本地磁盘缓存是否命中
- **事件**：当本地磁盘缓存未命中时报告 MISS 事件
- **事件性质**：**同步事件**，内核会阻塞等待用户态守护进程响应对应的消息 ID
- **用户态处理**：
  1. 从远程存储获取目标数据
  2. 使用消息头中提供的 `fd` 通过 `write()` 系统调用将数据写入 MFS
  3. 调用 `MFS_IOC_DONE` ioctl 回复

**REMOTE 模式事件流程**：
```
应用 read() / mmap 访问
    │
    ▼
检查本地磁盘缓存（SEEK_HOLE/SEEK_DATA）
    │
    ├─ 命中 → 直接返回数据
    │
    └─ 未命中 → 同步发送 MISS 事件 → [内核阻塞]
                     │
                     ▼
              用户态守护进程接收事件
                     │
                     ▼
         1. 从远程存储获取数据
         2. write(fd, data, len) 写入 MFS
         3. ioctl(MFS_IOC_DONE) 回复内核
                     │
                     ▼
              [内核解除阻塞，返回数据]
```

---

## 5. 用户态 API

### 5.1 字符设备操作

#### open()
打开 `/dev/mfs${minor}` 设备，获取事件访问权限。

#### read()
从设备读取事件。返回完整的 `mfs_msg` 结构。

#### poll()
使用 `select/poll/epoll` 等待新事件。

#### close()
关闭设备，清理所有未处理的事件。

### 5.2 ioctl 命令

#### MFS_IOC_RA (LOCAL 模式)

用于触发 readahead，将数据加载到 page cache：

```c
struct mfs_ioc_ra {
    __u64 off;  // 预取的起始偏移量
    __u64 len;  // 预取的长度
};

// 用法
ioctl(fd, MFS_IOC_RA, &ra);
```

#### MFS_IOC_DONE (REMOTE 模式)

用于回复同步事件，通知内核数据处理完成：

```c
struct mfs_ioc_done {
    __u32 id;   // 消息头中的消息 ID
    __u32 ret;  // 事件返回码，0 表示成功
};

// 用法
ioctl(fd, MFS_IOC_DONE, &done);
```

#### MFS_IOC_RPATH

获取文件对象的完整路径，用于实现更复杂的策略（如基于追踪的策略）：

```c
struct mfs_ioc_rpath {
    __u16 max;  // 输入数据区域的最大长度，用于填充完整路径
    __u16 len;  // 完整路径的实际长度
    __u8 d[];   // 由用户态守护进程分配的输入数据区域
};

// 用法
struct mfs_ioc_rpath *rpath = malloc(sizeof(*rpath) + 1024);
rpath->max = 1024;
ioctl(fd, MFS_IOC_RPATH, rpath);
// rpath->d 现在包含完整路径
```

---

## 6. 内核代码实现要点

### 6.1 禁用 VFS 默认预读

根据官方文档，MFS 在 VFS 中禁用了默认预读，完全由用户态控制预取策略：

```c
// fs/mfs/data.c: mfs_fadvise()
/* avoid trigger readahead in event mode */
if (support_event(sbi))
    return generic_fadvise(file, offset, len, advice);
```

在事件模式下，MFS 拦截 `fadvise` 系统调用，避免触发内核的 readahead。

同时在打开文件时设置 `FMODE_RANDOM` 标志：
```c
// fs/mfs/data.c: mfs_open()
if (support_event(sbi))
    /* close the default readahead */
    cfile->f_mode |= FMODE_RANDOM;
```

### 6.2 缓存检查机制

**LOCAL 模式**：使用 `filemap_get_folio()` 检查 page cache

```c
// fs/mfs/data.c: range_check_mem()
folio = filemap_get_folio(mapping, cur_off >> PAGE_SHIFT);
if (IS_ERR(folio)) {
    r->status = RANGE_HOLE;  // 缓存未命中
} else {
    r->status = RANGE_DATA;   // 缓存命中
    folio_put(folio);
}
```

**REMOTE 模式**：使用 `vfs_llseek()` 配合 `SEEK_HOLE/SEEK_DATA`

```c
// fs/mfs/data.c: range_check_disk()
off = vfs_llseek(file, start, SEEK_DATA);
if (off < 0) {
    if (off == (loff_t)-ENXIO) {
        r->len = end - start;
        r->status = RANGE_HOLE;  // 全是空洞
        goto out;
    }
    err = (int)off;
    goto out;
}
to = vfs_llseek(file, start, SEEK_HOLE);
if (to < end) {
    r->len = to - start;
    r->status = RANGE_DATA;  // 找到数据
    goto out;
}
```

### 6.3 事件发送

事件使用 xarray 存储和管理：

```c
// fs/mfs/cache.c: mfs_post_event_read()
event = kzalloc(sizeof(*event) + sizeof(*msg), GFP_KERNEL);
event->msg.opcode = op;
event->msg.len = sizeof(struct mfs_msg) + sizeof(struct mfs_read);
msg = (void *)event->msg.data;
msg->off = off;
msg->len = len;
msg->pid = current->pid;

// 插入 xarray 并设置标记
xas_store(&xas, event);
xas_set_mark(&xas, MFS_EVENT_NEW);

// 唤醒等待的进程
wake_up_all(&caches->pollwq);
```

---

## 7. 用户态守护进程示例

### 7.1 基础守护进程 (mfsd.c)

这是 MFS 提供的基础示例，展示如何处理事件：

```c
int main(int argc, char *argv[]) {
    char *mountpoint = argv[1];
    struct statfs buf;
    char devname[10];

    // 1. 检查是否为 MFS 挂载点
    statfs(mountpoint, &buf);
    if (buf.f_type != MFS_SUPER_MAGIC) {
        fprintf(stderr, "Not an MFS mount point\n");
        return -1;
    }

    // 2. 构建设备路径并打开
    sprintf(devname, "/dev/mfs%ld", buf.f_spare[0]);
    int fd = open(devname, O_RDWR);

    // 3. 获取工作模式
    struct mfs_ioc_fsinfo fsinfo = {0};
    ioctl(fd, MFS_IOC_FSINFO, &fsinfo);
    mfs_mode = fsinfo.mode;

    // 4. 设置 poll
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    // 5. 事件循环
    while (1) {
        poll(&pfd, 1, -1);  // 阻塞等待事件
        if (pfd.revents & POLLIN) {
            process_req(fd);  // 处理事件
        }
    }
}

// LOCAL 模式的事件处理
static int process_local_read(struct mfs_msg *msg) {
    struct mfs_read *read = (struct mfs_read *)msg->data;
    struct mfs_ioc_ra ra;

    ra.off = read->off;
    ra.len = read->len;

    // 调用 ioctl 触发 readahead
    ioctl(msg->fd, MFS_IOC_RA, &ra);
    return 0;
}
```

### 7.2 预取守护进程 (mfsd_prefetch.c)

这是一个增强版本，在收到 MISS 事件时预取整个文件：

```c
// 1. 扫描目录，构建文件列表
static int get_files(const char *parent) {
    DIR *dir = opendir(parent);
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(filepath, sizeof(filepath), "%s/%s", parent, entry->d_name);
        stat(filepath, &buf);

        if (S_ISREG(buf.st_mode)) {
            files[file_count].path = strdup(filepath);
            files[file_count].len = buf.st_size;
            file_count++;
        }
    }
    closedir(dir);
    return 0;
}

// 2. 预取线程：逐页读取文件到 cache
static void *fault(void *arg) {
    struct thread_ctx *ctx = arg;

    int fd = open(ctx->path, O_RDONLY);

    // mmap 整个文件
    void *addr = mmap(NULL, ctx->len, PROT_READ, MAP_SHARED, fd, 0);

    // 逐页访问，触发页面加载到 page cache
    char *buffer = (char *)addr;
    for (uint64_t idx = 0; idx < ctx->len; idx += 4096) {
        char tmp = buffer[idx];  // 访问每一页
    }

    munmap(addr, ctx->len);
    close(fd);
    free(ctx);
    return NULL;
}

// 3. 处理 LOCAL 模式读取事件：为所有文件创建预取线程
static int process_local_read(struct mfs_msg *msg) {
    for (int i = 0; i < file_count; i++) {
        struct thread_ctx *ctx = malloc(sizeof(*ctx));
        ctx->path = strdup(files[i].path);
        ctx->len = files[i].len;
        pthread_create(&t0, NULL, fault, ctx);
        pthread_detach(t0);
    }
    return 0;
}
```

---

## 8. 测试脚本详解

### 8.1 非预取测试 (test)

这个测试验证 MFS 的基本事件机制，使用 `mfsd` 守护进程：

```bash
#!/usr/bin/env bash

# 创建目录结构
mkdir -p /var/tmp/mfs/{data,mount}

# 挂载 MFS (LOCAL 模式，mtree == cachedir)
mount -t mfs -o mode=local,mtree=/var/tmp/mfs/data,cachedir=/var/tmp/mfs/data \
    /dev/null /var/tmp/mfs/mount

# 创建 1MB 测试文件
dd if=/dev/zero of=/var/tmp/mfs/data/bigfile bs=1M count=1

# 清空 page cache，确保文件不在缓存中
echo 3 > /proc/sys/vm/drop_caches

# 使用 vmtouch 检查初始状态（应该显示 0/256 页面）
vmtouch -v /var/tmp/mfs/data/bigfile

# 启动基础守护进程（不包含预取功能）
tools/mfs/mfsd /var/tmp/mfs/mount &

# 只读取 4KB（触发 CACHE MISS 事件）
dd if=/var/tmp/mfs/mount/bigfile of=/dev/null bs=4K count=1

# 检查缓存状态（应该只有 1/256 页面，因为无预取）
vmtouch -v /var/tmp/mfs/data/bigfile
```

**预期结果**：
- 只读 4KB 后，vmtouch 应显示只有约 1/256 页面在缓存中
- 说明 MFS 本身没有预取功能，只是响应事件

### 8.2 预取测试 (test_prefetch)

这个测试验证 MFS 配合用户空间预取的功能：

```bash
#!/usr/bin/env bash

# 挂载 MFS (LOCAL 模式)
mount -t mfs -o mode=local,mtree=/var/tmp/mfs/data,cachedir=/var/tmp/mfs/data \
    /dev/null /var/tmp/mfs/mount

# 创建 1MB 测试文件
dd if=/dev/zero of=/var/tmp/mfs/data/bigfile bs=1M count=1

# 清空 page cache
echo 3 > /proc/sys/vm/drop_caches

# 使用 vmtouch 检查初始状态（应该显示 0/256 页面）
vmtouch -v /var/tmp/mfs/data/bigfile

# 启动预取守护进程（传入 data 目录）
tools/mfs/mfsd_prefetch /var/tmp/mfs/mount /var/tmp/mfs/data &

# 等待守护进程启动并扫描文件
sleep 3

# 只读取 4KB（触发 CACHE MISS 事件）
dd if=/var/tmp/mfs/mount/bigfile of=/dev/null bs=4K count=1

# 等待预取完成
sleep 5

# 检查缓存状态（应该显示 256/256 页面全部在缓存中）
vmtouch -v /var/tmp/mfs/data/bigfile
```

**预期结果**：
- 只读 4KB 后，vmtouch 显示 256/256 页面全部在缓存中
- 说明用户空间预取成功加载了整个文件

### 8.3 测试工具说明

#### vmtouch
vmtouch 是一个工具，用于检查文件的哪些页面在内存中：
```bash
# 安装
apt-get install vmtouch

# 查看页面缓存状态
vmtouch -v /path/to/file

# 输出示例
[OOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOOO] 256/256
           Files: 1
     Directories: 0
  Resident Pages: 256/256  1M/1M  100%
         Elapsed: 0.000171 seconds
```

#### drop_caches
用于清空 page cache：
```bash
# 清空 page cache、dentries 和 inodes
echo 3 > /proc/sys/vm/drop_caches

# 只清空 page cache
echo 1 > /proc/sys/vm/drop_caches
```

---

## 9. 完整使用示例

### 9.1 本地存储加速（LOCAL 模式）

```bash
# 1. 准备目录
mkdir -p /var/mfs/{data,mount}

# 2. 创建测试数据
dd if=/dev/urandom of=/var/mfs/data/largefile bs=1M count=100

# 3. 挂载 MFS
mount -t mfs -o mode=local,mtree=/var/mfs/data,cachedir=/var/mfs/data \
    /dev/null /var/mfs/mount

# 4. 启动预取守护进程
./mfsd_prefetch /var/mfs/mount /var/mfs/data &

# 5. 应用访问（会自动触发预取）
cat /var/mfs/mount/largefile > /dev/null
```

### 9.2 远程存储缓存（REMOTE 模式）

```bash
# 1. 准备目录
mkdir -p /var/mfs/{cache,mount}

# 2. 假设 /mnt/remote 是已挂载的远程存储
# 3. 挂载 MFS
mount -t mfs -o mode=remote,mtree=/mnt/remote,cachedir=/var/mfs/cache \
    /dev/null /var/mfs/mount

# 4. 启动守护进程（需要实现远程数据获取逻辑）
./mfsd_remote /var/mfs/mount &

# 5. 应用访问
cat /var/mfs/mount/file > /dev/null
```

---

## 10. 性能优化建议

### 10.1 用户态守护进程优化

1. **使用 epoll**：相比 poll，epoll 在大量文件描述符时更高效
2. **批量处理事件**：积累多个事件后批量处理，减少系统调用
3. **异步 I/O**：使用 io_uring 进行异步文件操作
4. **线程池**：使用线程池并发处理多个预取请求
5. **NUMA 感知**：在多 NUMA 节点系统上，在对应的节点分配内存

### 10.2 预取策略

1. **顺序预取**：检测顺序访问模式，预取后续数据
2. **智能预取量**：根据历史访问模式动态调整预取量
3. **热点识别**：识别频繁访问的文件，提前预热缓存
4. **带宽控制**：避免预取占用过多 I/O 带宽

---

## 11. 调试和监控

### 11.1 内核日志

```bash
# 查看 MFS 相关日志
dmesg | grep MFS

# 实时监控
dmesg -w | grep MFS
```

关键日志信息：
- `range_check_mem` - 缓存检查
- `RANGE_DATA` / `RANGE_HOLE` - 缓存命中/未命中
- `mfs_post_event_read` - 事件发送
- `event inserted successfully` - 事件插入成功

### 11.2 vmtouch 监控

```bash
# 持续监控文件缓存状态
watch -n 1 'vmtouch -v /path/to/file | tail -5'
```

---

## 12. 常见问题

### Q: MFS 支持写操作吗？
A: 不支持。MFS 是只读文件系统，底层文件系统也必须保持只读状态。

### Q: LOCAL 和 REMOTE 模式的主要区别是什么？
A: 主要区别：
- **LOCAL**：使用 page cache，发送异步事件
- **REMOTE**：使用本地磁盘缓存，使用 SEEK_HOLE/SEEK_DATA 检查，发送同步事件

### Q: 为什么 MFS 要禁用 VFS 默认预读？
A: 为了让用户空间完全控制预取策略。官方文档明确说明："So in MFS, default prefetch in VFS is disabled."

### Q: 可以同时运行多个守护进程吗？
A: 不可以。每个 MFS 挂载点只允许一个守护进程打开对应的 `/dev/mfs<N>` 设备。

---

## 13. 总结

MFS 是一个灵活的可堆叠文件系统，通过事件机制将缓存控制权交给用户空间，实现了高度可定制的缓存和预取策略。其核心优势包括：

1. **灵活性**：用户空间完全控制缓存行为
2. **可扩展性**：支持自定义预取算法
3. **透明性**：对应用完全透明
4. **模式丰富**：支持 NONE/LOCAL/REMOTE 三种模式

官方文档提到的典型应用场景包括：
- 加速模型权重加载（并发加载、NUMA 感知、基于追踪的预取）
- 追踪读取 I/O（记录 MISS 事件的 offset 和 length）

---

## 14. 参考资料

- **官方文档**：`Documentation/filesystems/mfs.rst`
- **内核源码**：`fs/mfs/`
- **用户态示例**：`tools/mfs/mfsd.c`, `tools/mfs/mfsd_prefetch.c`
- **UAPI 头文件**：`include/uapi/linux/mfs.h`
- **内部头文件**：`fs/mfs/internal.h`
