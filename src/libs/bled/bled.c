#include "libbb.h"
#include "bb_archive.h"
#include "bled.h"

smallint bb_got_signal;
uint64_t bb_total_rb, bb_total_wb;
printf_t bled_printf = NULL;
read_t bled_read = NULL;
write_t bled_write = NULL;
progress_t bled_progress = NULL;
switch_t bled_switch = NULL;
unsigned long* bled_cancel_request;
bool bled_initialized = 0;
jmp_buf bb_error_jmp;
char* bb_virtual_buf = NULL;
size_t bb_virtual_len = 0, bb_virtual_pos = 0;
int bb_virtual_fd = -1;
bool bb_progress_on_write = 0;
uint32_t BB_BUFSIZE = 0x40000;

static int64_t unpack_none(transformer_state_t *xstate) {
	bb_error_msg("This compression type is not supported");
	return -1;
}

unpacker_t unpacker[BLED_COMPRESSION_MAX] = {
	unpack_none,
	unpack_zip_stream,
	unpack_Z_stream,
	unpack_gz_stream,
	unpack_lzma_stream,
	unpack_bz2_stream,
	unpack_xz_stream,
	unpack_none,
	unpack_vtsi_stream,
	unpack_zstd_stream,
};

int64_t bled_uncompress(const char* src, const char* dst, int type) {
	transformer_state_t xstate;
	int64_t ret = -1;

	if (!bled_initialized) {
		bb_error_msg("The library has not been initialized");
		return -1;
	}

	bb_total_rb = 0;
	bb_total_wb = 0;
	init_transformer_state(&xstate);
	xstate.src_fd = -1;
	xstate.dst_fd = -1;

	xstate.src_fd = open(src, O_RDONLY | O_BINARY, 0);
	if (xstate.src_fd < 0) {
		bb_error_msg("Could not open '%s' (errno: %d)", src, errno);
		goto err;
	}

	xstate.dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR);
	if (xstate.dst_fd < 0) {
		bb_error_msg("Could not open '%s' (errno: %d)", dst, errno);
		goto err;
	}

	if ((type < 0) || (type >= BLED_COMPRESSION_MAX)) {
		bb_error_msg("Unsupported compression format");
		goto err;
	}

	if (bled_progress != NULL) {
		xstate.src_size = lseek(xstate.src_fd, 0, SEEK_END);
		xstate.dst_size = get_uncompressed_size(xstate.src_fd, type);
		bb_progress_on_write = (xstate.dst_size > 0);
		bled_progress(bb_progress_on_write ? -xstate.dst_size : -xstate.src_size);
	}

	if (setjmp(bb_error_jmp))
		goto err;

	ret = unpacker[type](&xstate);

err:
	free(xstate.dst_name);
	if (xstate.src_fd > 0)
		close(xstate.src_fd);
	if (xstate.dst_fd > 0)
		close(xstate.dst_fd);
	return ret;
}

int64_t bled_uncompress_to_buffer(const char* src, char* buf, size_t size, int type) {
	transformer_state_t xstate;
	int64_t ret = -1;

	if (!bled_initialized) {
		bb_error_msg("The library has not been initialized");
		return -1;
	}

	if ((src == NULL) || (buf == NULL)) {
		bb_error_msg("Invalid parameter");
		return -1;
	}

	bb_total_rb = 0;
	bb_total_wb = 0;
	init_transformer_state(&xstate);
	xstate.src_fd = -1;
	xstate.dst_fd = -1;

	if (src[0] == 0) {
		xstate.src_fd = bb_virtual_fd;
	} else {
		xstate.src_fd = open(src, O_RDONLY | O_BINARY, 0);
	}
	if (xstate.src_fd < 0) {
		bb_error_msg("Could not open '%s' (errno: %d)", src, errno);
		goto err;
	}

	xstate.mem_output_buf = buf;
	xstate.mem_output_size = 0;
	xstate.mem_output_size_max = size;

	if ((type < 0) || (type >= BLED_COMPRESSION_MAX)) {
		bb_error_msg("Unsupported compression format");
		goto err;
	}

	if (bled_progress != NULL) {
		xstate.src_size = lseek(xstate.src_fd, 0, SEEK_END);
		xstate.dst_size = get_uncompressed_size(xstate.src_fd, type);
		bb_progress_on_write = (xstate.dst_size > 0);
		bled_progress(bb_progress_on_write ? -xstate.dst_size : -xstate.src_size);
	}

	if (setjmp(bb_error_jmp))
		goto err;

	ret = unpacker[type](&xstate);

err:
	free(xstate.dst_name);
	if ((src[0] != 0) && (xstate.src_fd > 0))
		close(xstate.src_fd);
	return ret;
}

int64_t bled_uncompress_to_dir(const char* src, const char* dir, int type) {
	transformer_state_t xstate;
	int64_t ret = -1;

	if (!bled_initialized) {
		bb_error_msg("The library has not been initialized");
		return -1;
	}

	bb_total_rb = 0;
	bb_total_wb = 0;
	init_transformer_state(&xstate);
	xstate.src_fd = -1;
	xstate.dst_fd = -1;

	xstate.src_fd = open(src, O_RDONLY | O_BINARY, 0);
	if (xstate.src_fd < 0) {
		bb_error_msg("Could not open '%s' (errno: %d)", src, errno);
		goto err;
	}

	xstate.dst_dir = dir;

	if (type != BLED_COMPRESSION_ZIP) {
		bb_error_msg("This compression format is not supported for directory extraction");
		goto err;
	}

	if (bled_progress != NULL) {
		xstate.src_size = lseek(xstate.src_fd, 0, SEEK_END);
		xstate.dst_size = 0;
		bb_progress_on_write = 0;
		bled_progress(-xstate.src_size);
	}

	if (setjmp(bb_error_jmp))
		goto err;

	ret = unpacker[type](&xstate);

err:
	free(xstate.dst_name);
	if (xstate.src_fd > 0)
		close(xstate.src_fd);
	if (xstate.dst_fd > 0)
		close(xstate.dst_fd);
	return ret;
}

int64_t bled_uncompress_from_buffer_to_buffer(const char* src, const size_t src_len,
	char* dst, size_t dst_len, int type) {
	int64_t ret;

	if (!bled_initialized) {
		bb_error_msg("The library has not been initialized");
		return -1;
	}

	if ((src == NULL) || (dst == NULL)) {
		bb_error_msg("Invalid parameter");
		return -1;
	}

	if (bb_virtual_buf != NULL) {
		bb_error_msg("Can not decompress more than one buffer at once");
		return -1;
	}

	bb_virtual_buf = (char*)src;
	bb_virtual_len = src_len;
	bb_virtual_pos = 0;
	bb_virtual_fd = 0;

	ret = bled_uncompress_to_buffer("", dst, dst_len, type);

	bb_virtual_buf = NULL;
	bb_virtual_len = 0;
	bb_virtual_fd = -1;

	return ret;
}

int64_t bled_get_uncompressed_size(const char* src, int type) {
	int fd = -1;
	int64_t ret = -1;

	if (!bled_initialized) {
		bb_error_msg("The library has not been initialized");
		return -1;
	}

	if ((type < 0) || (type >= BLED_COMPRESSION_MAX)) {
		bb_error_msg("Unsupported compression format");
		goto err;
	}

	fd = open(src, O_RDONLY | O_BINARY, 0);
	if (fd < 0)
		return -1;

	ret = get_uncompressed_size(fd, type);

err:
	if (fd > 0)
		close(fd);
	return ret;
}

int bled_init(uint32_t buffer_size, printf_t print_function, read_t read_function,
	write_t write_function, progress_t progress_function, switch_t switch_function,
	unsigned long* cancel_request) {
	if (bled_initialized)
		return -1;
	BB_BUFSIZE = buffer_size;
	if (buffer_size < 0x40000 || (buffer_size & (buffer_size - 1)) != 0) {
		if (buffer_size != 0 && print_function != NULL)
			print_function("bled_init: invalid buffer_size, defaulting to 64 KB");
		BB_BUFSIZE = 0x40000;
	}
	bled_printf = print_function;
	bled_read = read_function;
	bled_write = write_function;
	bled_progress = progress_function;
	bled_switch = switch_function;
	bled_cancel_request = cancel_request;
	bled_initialized = true;
	return 0;
}

void bled_exit(void) {
	bled_printf = NULL;
	bled_progress = NULL;
	bled_switch = NULL;
	bled_cancel_request = NULL;
	if (global_crc32_table) {
		free(global_crc32_table);
		global_crc32_table = NULL;
	}
	bled_initialized = false;
}
