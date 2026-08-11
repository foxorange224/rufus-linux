#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

typedef void (*printf_t) (const char* format, ...);
typedef void (*progress_t) (const int64_t read_bytes);
typedef int (*read_t)(int fd, void* buf, unsigned int count);
typedef int (*write_t)(int fd, const void* buf, unsigned int count);
typedef void (*switch_t)(const char* filename, const uint64_t size);

typedef enum {
	BLED_COMPRESSION_NONE = 0,
	BLED_COMPRESSION_ZIP,
	BLED_COMPRESSION_LZW,
	BLED_COMPRESSION_GZIP,
	BLED_COMPRESSION_LZMA,
	BLED_COMPRESSION_BZIP2,
	BLED_COMPRESSION_XZ,
	BLED_COMPRESSION_7ZIP,
	BLED_COMPRESSION_VTSI,
	BLED_COMPRESSION_ZSTD,
	BLED_COMPRESSION_MAX
} bled_compression_type;

int64_t bled_get_uncompressed_size(const char* src, int type);
int64_t bled_uncompress(const char* src, const char* dst, int type);
int64_t bled_uncompress_to_buffer(const char* src, char* buf, size_t size, int type);
int64_t bled_uncompress_to_dir(const char* src, const char* dir, int type);
int64_t bled_uncompress_from_buffer_to_buffer(const char* src, const size_t src_len, char* dst, size_t dst_len, int type);

int bled_init(uint32_t buffer_size, printf_t print_function, read_t read_function, write_t write_function,
    progress_t progress_function, switch_t switch_function, unsigned long* cancel_request);

void bled_exit(void);
