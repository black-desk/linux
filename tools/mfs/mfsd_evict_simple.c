// SPDX-License-Identifier: GPL-2.0
/*
 * User-space demo of mfs evict
 *
 * Example use:
 * ./mfsd_evict_simple [mfs_mountpoint] [mtree_path]
 * mfsd_evict_simple demonstrates how to evict page cache when
 * file is read to end. This is useful for model weight loading
 * scenarios where weights are read once into GPU memory.
 *
 * Design:
 * - When a file is read to end, it is queued for eviction (not evicted immediately)
 * - When a new file starts reading, queued files are evicted
 * - This ensures the application has time to process the data before eviction
 *
 * See Documentation/filesystems/mfs.rst
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
	char *path;      /* Full file path in real filesystem */
	int mfs_fd;      /* MFS internal file descriptor */
	int user_fd;     /* Userspace fd for posix_fadvise */
	size_t size;     /* File size */
	size_t last_off; /* Last read offset */
	int valid;       /* Slot is valid */
};

static struct file_track files[MAX_FILES];
static struct file_track evict_queue[MAX_FILES];
static int evict_queue_head = 0;
static int evict_queue_tail = 0;
static int evict_queue_count = 0;
static int mfs_mode = -1;
static int current_mfs_fd = -1;
static char *mtree_path = NULL;  /* Data directory path (corresponding to mtree) */

/* Get full file path from MFS internal fd using MFS_IOC_RPATH ioctl */
/* MFS_IOC_RPATH returns relative path to mtree, need to prepend mtree_path */
static char *get_file_path(int mfs_fd)
{
	struct mfs_ioc_rpath *rpath;
	char *path = NULL;
	char *full_path = NULL;
	int ret;

	rpath = (struct mfs_ioc_rpath *)malloc(sizeof(struct mfs_ioc_rpath) + PATH_MAX);
	if (!rpath) {
		pr_err("malloc for path failed\n");
		return NULL;
	}

	rpath->max = PATH_MAX;
	ret = ioctl(mfs_fd, MFS_IOC_RPATH, (unsigned long)rpath);
	if (ret) {
		pr_err("ioctl MFS_IOC_RPATH failed: %s\n", strerror(errno));
		free(rpath);
		return NULL;
	}

	path = (char *)rpath->d;

	/* MFS_IOC_RPATH returns path relative to mtree, need to prepend mtree_path */
	if (mtree_path) {
		int mtree_len = strlen(mtree_path);
		/* Check if mtree_path ends with '/' */
		if (mtree_path[mtree_len - 1] == '/') {
			/* Already has '/', directly concatenate (path starts with '/') */
			asprintf(&full_path, "%s%s", mtree_path, path + 1);
		} else {
			/* No '/', add '/' */
			asprintf(&full_path, "%s/%s", mtree_path, path + 1);
		}
	} else {
		pr_err("mtree_path is NULL\n");
		free(rpath);
		return NULL;
	}

	pr_err("[PATH] mfs_path=%s -> full_path=%s\n", path, full_path);
	free(rpath);
	return full_path;
}

/* Open userspace file descriptor for posix_fadvise */
static int open_user_fd(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		pr_err("open user fd failed for %s: %s\n", path, strerror(errno));
		return -1;
	}
	return fd;
}

static struct file_track *get_file_track(int mfs_fd)
{
	int i;

	/* Find existing track */
	for (i = 0; i < MAX_FILES; i++) {
		if (files[i].valid && files[i].mfs_fd == mfs_fd)
			return &files[i];
	}

	/* Find empty slot and create new track */
	for (i = 0; i < MAX_FILES; i++) {
		if (!files[i].valid) {
			struct stat st;
			char *path;

			path = get_file_path(mfs_fd);
			if (!path) {
				pr_err("[ERROR] failed to get path for mfs_fd=%d\n", mfs_fd);
				return NULL;
			}

			files[i].path = path;
			files[i].mfs_fd = mfs_fd;
			files[i].user_fd = open_user_fd(path);
			files[i].size = 0;
			files[i].last_off = 0;
			files[i].valid = 1;

			if (stat(path, &st) == 0)
				files[i].size = st.st_size;

			pr_err("[TRACK] mfs_fd=%d user_fd=%d path=%s size=%zu\n",
			       mfs_fd, files[i].user_fd, path, files[i].size);
			return &files[i];
		}
	}

	pr_err("[ERROR] too many files tracked\n");
	return NULL;
}

/* Evict file page cache using posix_fadvise */
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
	pr_err("[ERROR] evict %s fd=%d failed: %s\n", path, user_fd, strerror(ret));
	return -1;
}

/* Evict all files in the pending queue */
static void evict_pending_files(void)
{
	while (evict_queue_count > 0) {
		struct file_track *pending = &evict_queue[evict_queue_head];
		if (pending->valid) {
			evict_file(pending->user_fd, pending->path, "new file read started");
			free(pending->path);
			close(pending->user_fd);
			pending->valid = 0;
		}
		evict_queue_head = (evict_queue_head + 1) % MAX_FILES;
		evict_queue_count--;
	}
}

static int process_read(struct mfs_msg *msg)
{
	struct mfs_read *rd = (struct mfs_read *)msg->data;
	struct file_track *track;
	size_t end_off;
	int mfs_fd = msg->fd;

	track = get_file_track(mfs_fd);
	if (!track)
		return -1;

	/* Check if this is a new file */
	if (current_mfs_fd != mfs_fd) {
		current_mfs_fd = mfs_fd;
		/* New file starts reading, evict queued files */
		evict_pending_files();
	}

	end_off = rd->off + rd->len;
	if (end_off > track->last_off)
		track->last_off = end_off;

	/* File read to end, add to evict queue (not evicted immediately) */
	if (track->size > 0 && track->last_off >= track->size) {
		if (evict_queue_count < MAX_FILES) {
			int slot = evict_queue_tail;
			evict_queue[slot].path = track->path;
			evict_queue[slot].user_fd = track->user_fd;
			evict_queue[slot].valid = 1;
			evict_queue_tail = (evict_queue_tail + 1) % MAX_FILES;
			evict_queue_count++;
			pr_err("[QUEUE] %s waiting for next file\n", track->path);
		} else {
			pr_err("[ERROR] evict queue full, force evict %s\n", track->path);
			evict_file(track->user_fd, track->path, "read to end (queue full)");
			free(track->path);
			close(track->user_fd);
		}
		track->valid = 0;
		current_mfs_fd = -1;
	}

	return 0;
}

static int process_req(int fd)
{
	char buf[1024];
	struct mfs_msg *msg;
	int ret;

	memset(buf, 0, sizeof(buf));
	ret = read(fd, buf, sizeof(buf));
	if (ret <= 0) {
		if (ret < 0)
			pr_err("read failed, ret:%d\n", ret);
		return -1;
	}

	msg = (struct mfs_msg *)buf;
	if (ret != msg->len) {
		pr_err("invalid message length, read:%d, need:%d\n", ret, msg->len);
		return -1;
	}

	if (msg->opcode == MFS_OP_READ || msg->opcode == MFS_OP_FAULT)
		return process_read(msg);

	pr_err("invalid opcode:%d\n", msg->opcode);
	return -1;
}

static void ioctl_mfs_mode(int fd)
{
	struct mfs_ioc_fsinfo fsinfo = {0};
	int ret;

	ret = ioctl(fd, MFS_IOC_FSINFO, (unsigned long)&fsinfo);
	if (ret < 0) {
		perror("failed to ioctl mfs_ioc_fsinfo");
		close(fd);
		exit(-1);
	}

	mfs_mode = fsinfo.mode;
}

int main(int argc, char *argv[])
{
	struct pollfd pfd;
	struct statfs buf;
	char *mountpoint;
	char devname[10];
	int fd, ret;

	if (argc != 3) {
		printf("./mfsd_evict_simple ${mfs_mountpoint} ${mtree_path}\n");
		return -1;
	}
	mountpoint = argv[1];
	mtree_path = argv[2];

	ret = statfs(mountpoint, &buf);
	if (ret) {
		pr_err("statfs %s failed\n", mountpoint);
		return -1;
	}
	if (buf.f_type != MFS_SUPER_MAGIC) {
		pr_err("fstype(%lx) is invalid, please check mountpoint\n", buf.f_type);
		return -1;
	}

	sprintf(devname, "/dev/mfs%ld", buf.f_spare[0]);
	fd = open(devname, O_RDWR);
	if (fd < 0) {
		pr_err("open %s failed\n", devname);
		return -1;
	}

	ioctl_mfs_mode(fd);

	memset(files, 0, sizeof(files));
	memset(evict_queue, 0, sizeof(evict_queue));

	pfd.fd = fd;
	pfd.events = POLLIN;

	pr_err("Listening for events...\n");
	pr_err("Will evict page cache when new file starts reading.\n");
	pr_err("(Files read to end are queued until next file starts)\n\n");

	while (1) {
		ret = poll(&pfd, 1, -1);
		if (ret < 0) {
			pr_err("poll failed\n");
			return -1;
		}

		if (ret == 0 || !(pfd.revents & POLLIN)) {
			pr_err("poll event error, ret:%d, revents:%x\n", ret, pfd.revents);
			continue;
		}

		if (process_req(fd) == -1)
			pr_err("process req failed, errcode:%d\n", errno);
	}
	return 0;
}
