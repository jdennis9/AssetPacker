#ifndef PACKAGE_H
#define PACKAGE_H

#include <stdint.h>
#include <stdio.h>

struct Package_Create {
	const char *base_path;
	const void **data_pointers;
	uint32_t *data_sizes;
	uint32_t base_path_length;
	uint32_t data_count;
	bool disable_compression;
};

struct Package {
	FILE *file;
	void *blocks;
	uint32_t block_count;
	uint32_t padding1_;
};

struct Package_File {
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t offset;
};

#define PKG_MAGIC *(uint32_t*)".dhm"

// Returns 0 on success
int package_create(const Package_Create *info, const char *out_path);
// Returns 0 on success
int package_open(const char *path, Package *out);
void package_close(Package *package);
// Argument cannot contain double delimiters (e.g. folder//file). Both '\' and '/' delimiters permitted.
int package_get_path_depth(const char *path);
// Returns -1 if the file does not exist
int package_lookup_file(const Package *package, const char *path);
bool package_lookup_file_info(const Package *package, const char *path, Package_File *out);
void package_get_file_info(const Package *package, int index, Package_File *out);
void package_read_file(const Package *package, Package_File *file, void *compressed_buffer, void *decompressed_buffer);

#define PACKAGE_FILE_IS_DIR(file) ((file).compressed_size == UINT32_MAX)

#endif //PACKAGE_H
