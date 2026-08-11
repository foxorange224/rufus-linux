#pragma once

#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <alloca.h>

#define ONE_TB                          1099511627776ULL
#define SECTOR_ALIGNMENT                4096

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#ifndef get_le64
#define get_le64(ptr) (*(const uint64_t *)(ptr))
#endif
#ifndef get_le32
#define get_le32(ptr) (*(const uint32_t *)(ptr))
#endif
#ifndef get_le16
#define get_le16(ptr) (*(const uint16_t *)(ptr))
#endif

extern uint32_t BB_BUFSIZE;
extern smallint bb_got_signal;
extern uint32_t *global_crc32_table;
extern jmp_buf bb_error_jmp;
extern char* bb_virtual_buf;
extern size_t bb_virtual_len, bb_virtual_pos;
extern int bb_virtual_fd;
extern bool bb_progress_on_write;

uint32_t* crc32_filltable(uint32_t *crc_table, int endian);
uint32_t crc32_le(uint32_t crc, unsigned char const *p, size_t len, uint32_t *crc32table_le);
uint32_t crc32_be(uint32_t crc, unsigned char const *p, size_t len, uint32_t *crc32table_be);
#define crc32_block_endian0 crc32_le
#define crc32_block_endian1 crc32_be

typedef struct _llist_t {
	struct _llist_t *link;
	char *data;
} llist_t;

struct timeval64 {
	int64_t tv_sec;
	int32_t tv_usec;
};

extern void (*bled_printf) (const char* format, ...);
extern void (*bled_progress) (const int64_t processed_bytes);
extern void (*bled_switch) (const char* filename, const uint64_t filesize);
extern int (*bled_read)(int fd, void* buf, unsigned int count);
extern int (*bled_write)(int fd, const void* buf, unsigned int count);
extern unsigned long* bled_cancel_request;

#define xfunc_die() longjmp(bb_error_jmp, 1)
#define bb_printf(...) do { if (bled_printf != NULL) bled_printf(__VA_ARGS__); \
	else { printf(__VA_ARGS__); putchar('\n'); } } while(0)
#define bb_error_msg(...) bb_printf("\nError: " __VA_ARGS__)
#define bb_error_msg_and_die(...) do { bb_error_msg(__VA_ARGS__); xfunc_die(); } while(0)
#define bb_error_msg_and_err(...) do { bb_error_msg(__VA_ARGS__); goto err; } while(0)
#define bb_perror_msg bb_error_msg
#define bb_perror_msg_and_die bb_error_msg_and_die
#define bb_simple_error_msg bb_error_msg
#define bb_simple_perror_msg_and_die bb_error_msg_and_die
#define bb_simple_error_msg_and_die bb_error_msg_and_die
#define bb_putchar putchar

static inline void *xrealloc(void *ptr, size_t size) {
	void *ret = realloc(ptr, size);
	if (!ret)
		free(ptr);
	return ret;
}

#define bb_msg_read_error "read error"
#define bb_msg_write_error "write error"

extern uint64_t bb_total_rb, bb_total_wb;

static inline int full_read(int fd, void *buf, unsigned int count) {
	int rb;

	if (fd < 0) { errno = EBADF; return -1; }
	if (buf == NULL) { errno = EFAULT; return -1; }
	if (count > BB_BUFSIZE) { errno = E2BIG; return -1; }
	if ((bled_cancel_request != NULL) && (*bled_cancel_request != 0)) { errno = EINTR; return -1; }

	if (fd == bb_virtual_fd) {
		if (bb_virtual_pos + count > bb_virtual_len)
			count = (unsigned int)(bb_virtual_len - bb_virtual_pos);
		memcpy(buf, &bb_virtual_buf[bb_virtual_pos], count);
		bb_virtual_pos += count;
		rb = (int)count;
	} else {
		rb = (bled_read != NULL) ? bled_read(fd, buf, count) : (int)read(fd, buf, count);
	}
	if (bled_progress != NULL && !bb_progress_on_write && rb > 0) {
		bb_total_rb += rb;
		bled_progress(bb_total_rb);
	}
	return rb;
}

static inline int full_write(int fd, const void* buffer, unsigned int count) {
	int wb;
	if (count > BB_BUFSIZE) { errno = E2BIG; return -1; }
	wb = (bled_write != NULL) ? bled_write(fd, buffer, count) : (int)write(fd, buffer, count);
	if (bled_progress != NULL && bb_progress_on_write && wb > 0) {
		bb_total_wb += wb;
		bled_progress(bb_total_wb);
	}
	return wb;
}

static inline void bb_copyfd_exact_size(int fd1, int fd2, off_t size) {
	off_t rb = 0;
	uint8_t* buf = NULL;

	if (fd1 < 0 || fd2 < 0)
		bb_error_msg_and_die("invalid fd");
	if (size > ONE_TB)
		bb_error_msg_and_die("too large");

	buf = aligned_alloc(SECTOR_ALIGNMENT, BB_BUFSIZE);
	if (buf == NULL)
		bb_error_msg_and_die("out of memory");

	while (rb < size) {
		int r, w;
		r = full_read(fd1, buf, (unsigned int)MIN(size - rb, BB_BUFSIZE));
		if (r < 0) { free(buf); bb_error_msg_and_die("read error"); }
		if (r == 0) { bb_error_msg("short read"); break; }
		w = full_write(fd2, buf, r);
		if (w < 0) { free(buf); bb_error_msg_and_die("write error"); }
		if (w != r) { bb_error_msg("short write"); break; }
		rb += r;
	}
	free(buf);
}

#define ENABLE_DESKTOP 1
#if ENABLE_DESKTOP
#define IF_DESKTOP(x) x
#define IF_NOT_DESKTOP(x)
#else
#define IF_DESKTOP(x)
#define IF_NOT_DESKTOP(x) x
#endif
#define IF_NOT_FEATURE_LZMA_FAST(x) x
#define ENABLE_FEATURE_UNZIP_CDF 1
#define ENABLE_FEATURE_UNZIP_BZIP2 1
#define ENABLE_FEATURE_UNZIP_LZMA 1
#define ENABLE_FEATURE_UNZIP_XZ 1
#define ENABLE_FEATURE_CLEAN_UP 1
#define uoff_t unsigned off_t
#define OFF_FMT "ll"

#define SEAMLESS_COMPRESSION 0
#if (SEAMLESS_COMPRESSION)
#define ENABLE_FEATURE_SEAMLESS_BZ2 1
#define ENABLE_FEATURE_SEAMLESS_GZ 1
#define ENABLE_FEATURE_SEAMLESS_LZMA 1
#define ENABLE_FEATURE_SEAMLESS_XZ 1
#define ENABLE_FEATURE_SEAMLESS_Z 1
#define ENABLE_FEATURE_SEAMLESS_ZSTD 1
#define IF_FEATURE_SEAMLESS_BZ2(x) x
#define IF_FEATURE_SEAMLESS_XZ(x) x
#define IF_FEATURE_SEAMLESS_ZSTD(x) x
#endif

#define safe_read full_read
#define lstat stat
#define xmalloc malloc
#define xzalloc(x) calloc(x, 1)
#define malloc_or_warn malloc
#define aligned_xmalloc(x) aligned_alloc(SECTOR_ALIGNMENT, x)
static inline void* aligned_xzalloc(size_t x) { void* r = aligned_xmalloc(x); if (r) memset(r, 0, x); return r; }
#define aligned_free free

#include <sys/time.h>
static inline void xasprintf(char **strp, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	*strp = (char*)malloc(1024);
	if (*strp) vsnprintf(*strp, 1024, fmt, ap);
	va_end(ap);
}
static inline void xrename(const char *oldpath, const char *newpath) {
	rename(oldpath, newpath);
}
#define setfscreatecon(x) ((void)0)
struct fd_pair { int rd; int wr; };
void xpipe(int filedes[2]) FAST_FUNC;
#define xpiped_pair(pair) xpipe(&((pair).rd))
#define xlseek lseek
#define xread safe_read
static inline void xmove_fd(int from, int to) {
	if (from != to) { (void)dup2(from, to); close(from); }
}
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define FILEUTILS_RECUR 0x01
static inline int bb_make_directory(const char *path, long mode, int flags) {
	(void)mode;
	(void)flags;
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "mkdir -p '%s'", path);
	return system(tmp);
}
