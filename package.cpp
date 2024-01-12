#include "package.h"
#include <sys/stat.h>
#include <lz4.h>
#include <xxhash.h>
#include <vector>

#ifdef _WIN32
#include <windows_dirent.h>
#define stat _stat
#define PATH_DELIM '\\'
#else
#include <dirent.h>
#define PATH_DELIM '/'
#endif

struct Package_Header_ {
	uint32_t magic;
	uint32_t version;
	uint32_t block_count;
};

// For a directory, uncompressed_size is the number of files and compressed_size is UINT32_MAX
struct Block_Header_ {
	uint32_t hash;
	uint32_t compressed_size;
	uint32_t uncompressed_size;
	uint32_t offset;
};

struct Directory_Read_ {
	uint32_t hash;
	char path[512-4];
};

static bool block_is_directory_(const Block_Header_ *header) {
	return header->compressed_size == UINT32_MAX;
}

static uint32_t count_directory_(char *path_buffer, uint32_t dir_hash, int base_path_length, int path_buffer_length, 
								 std::vector<Directory_Read_> *read_queue) {
	DIR *dir = opendir(path_buffer);
	struct dirent *dent;
	struct stat st;
	uint32_t count = 1; // count needs to include this directory
	
	if (!dir) {
		printf("Failed to open directory %s\n", path_buffer);
		return 0;
	}
	else {
		Directory_Read_ read = {};
		read.hash = dir_hash;
		strcpy(read.path, path_buffer);
		read_queue->push_back(read);
	}
	
	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, "..") || !strcmp(dent->d_name, ".")) continue;
		size_t file_name_length = strlen(dent->d_name);
		strcpy(&path_buffer[path_buffer_length], dent->d_name);
		stat(path_buffer, &st);
		
		const uint32_t hash = XXH32(dent->d_name, strlen(dent->d_name), 0);
		
		if (S_ISDIR(st.st_mode)) {
			path_buffer[path_buffer_length + file_name_length] = PATH_DELIM;
			count += 
				count_directory_(path_buffer, hash, base_path_length, path_buffer_length + file_name_length + 1, read_queue);
			path_buffer[path_buffer_length + file_name_length] = 0;
		}
		else if (S_ISREG(st.st_mode)) {
			count++;
		}
		
		memset(&path_buffer[path_buffer_length], 0, file_name_length);
	}
	
	closedir(dir);
	
	return count;
}

static uint64_t read_directory_(const Directory_Read_& read, uint64_t data_offset, FILE *output, bool compress) {
	DIR *dir = opendir(read.path);
	struct dirent *dent;
	Block_Header_ dir_header = {};
	char path_buffer[512];
	dir_header.hash = read.hash;
	dir_header.compressed_size = UINT32_MAX;
	
	printf("%s (0x%x)\n", read.path, read.hash);
	
	strcpy(path_buffer, read.path);
	size_t path_length = strlen(path_buffer);
	path_buffer[path_length] = PATH_DELIM;
	
	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) continue;
		dir_header.uncompressed_size++;
	}
	
	rewinddir(dir);
	
	fwrite(&dir_header, sizeof(dir_header), 1, output);
	
	while ((dent = readdir(dir))) {
		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, "..")) continue;
		struct stat st;
		strcpy(&path_buffer[path_length], dent->d_name);
		stat(path_buffer, &st);
		
		if (S_ISREG(st.st_mode)) {
			Block_Header_ header;
			void *compressed_buffer = NULL;
			void *uncompressed_buffer;
			const long header_pos = ftell(output);
			int compressed_size;
			
			// Open input file
			FILE *infile = fopen(path_buffer, "rb");
			
			uncompressed_buffer = malloc(st.st_size);
			
			// Read the whole file
			fread(uncompressed_buffer, st.st_size, 1, infile);
			fclose(infile);
			
			// Compress file
			if (compress) {
				compressed_buffer = malloc(LZ4_compressBound(st.st_size));
				compressed_size = LZ4_compress_default((char *)uncompressed_buffer, (char *)compressed_buffer,
					st.st_size, LZ4_compressBound(st.st_size));
				header.compressed_size = (uint32_t)compressed_size;
			}
			else {
				compressed_buffer = uncompressed_buffer;
				compressed_size = (uint32_t)st.st_size;
			}

			header.hash = XXH32(dent->d_name, strlen(dent->d_name), 0);
			header.compressed_size = compressed_size;
			header.uncompressed_size = (uint32_t)st.st_size;
			
			header.offset = data_offset;
			
			printf("%s (0x%x): %g\n", path_buffer, header.hash, (float)st.st_size / (float)compressed_size);
			
			// Write compressed file data
			fseek(output, data_offset, SEEK_SET);
			fwrite(compressed_buffer, compressed_size, 1, output);
			data_offset += compressed_size;
			
			// Write header
			fseek(output, header_pos, SEEK_SET);
			fwrite(&header, sizeof(header), 1, output);
			
			if (compressed_buffer != uncompressed_buffer) free(compressed_buffer);
			free(uncompressed_buffer);
		}
	}
	
	closedir(dir);
	return data_offset;
}

int package_create(const Package_Create *info, const char *out_path) {
	FILE *output = fopen(out_path, "wb");
	
	if (!output) {
		return -1;
	}
	
	Package_Header_ header = {};
	header.magic = PKG_MAGIC;
	
	// Format path
	char path_buffer[1024] = {};
	int path_buffer_length;
	
	snprintf(path_buffer, sizeof(path_buffer), "%s%c", info->base_path, PATH_DELIM);
	path_buffer_length = strlen(path_buffer);
	
	// Count directory files/subdirectories to calculate tree size
	std::vector<Directory_Read_> read_queue = std::vector<Directory_Read_>();
	header.block_count +=
		count_directory_(path_buffer, 0, path_buffer_length, path_buffer_length, &read_queue);
	
	// Write the header
	fwrite(&header, sizeof(header), 1, output);
	
	// Calculate size of block headers
	const size_t tree_size = header.block_count * sizeof(Block_Header_);
	printf("Tree size: %zu bytes\n", tree_size);
	
	// Write output
	uint64_t data_offset = tree_size+sizeof(header);
	for (uint32_t i = 0; i < read_queue.size(); ++i) {
		data_offset = read_directory_(read_queue[i], data_offset, output, !info->disable_compression);
	}
	
	fclose(output);
	return 0;
}

int package_open(const char *path, Package *out) {
	FILE *input = fopen(path, "rb");
	if (!input) return -1;
	
	Package_Header_ header;
	fread(&header, sizeof(header), 1, input);
	if (header.magic != PKG_MAGIC) {
		printf("%s is not a DHM file\n", path);
		return -1;
	}
	
	out->blocks = malloc(header.block_count * sizeof(Block_Header_));
	fread(out->blocks, sizeof(Block_Header_), header.block_count, input);
	out->block_count = header.block_count;
	out->file = input;
	
	return 0;
}

void package_close(Package *package) {
	fclose(package->file);
}

int package_get_path_depth(const char *path) {
	int depth = 0;
	int e = 0;
	
	for (; *path; ++path) {
		if ((*path == '/') || (*path == '\\')) {
			depth++;
			e = path[1] != 0;
		}
	}
	
	return depth + e;
}

static inline int length_until_delim(const char *s) {
	int length = 0;
	
	for (; *s; ++s, ++length) {
		if (*s == '/' || *s == '\\') {
			return length;
		}
	}
	
	return length;
}

int package_lookup_file(const Package *package, const char *path) {
	const Block_Header_ *blocks = static_cast<Block_Header_*>(package->blocks);
	const int count = (int)package->block_count;
	
	int len = length_until_delim(path);
	uint32_t hash = XXH32(path, len, 0);
	
	int i;
	for (i = 1; i < count; ++i) {
		const Block_Header_ &block = blocks[i];
		if ((block.hash == hash) && (path[len] == 0)) {
			return i;
		}
		else if (block.hash == hash) {
			path += len+1;
			len = length_until_delim(path);
			hash = XXH32(path, len, 0);
		}
		else if (block_is_directory_(&blocks[i])) {
			i += block.uncompressed_size;
		}
	}
	
	return -1;
}

bool package_lookup_file_info(const Package *package, const char *path, Package_File *out) {
	int index = package_lookup_file(package, path);
	if (index != -1) {
		const Block_Header_ *blocks = (Block_Header_*)package->blocks;
		out->compressed_size = blocks[index].compressed_size;
		out->uncompressed_size = blocks[index].uncompressed_size;
		out->offset = blocks[index].offset;
		return true;
	}
	else return false;
}

void package_get_file_info(const Package *package, int index, Package_File *out) {
	const Block_Header_ *blocks = (Block_Header_*)package->blocks;
	out->compressed_size = blocks[index].compressed_size;
	out->uncompressed_size = blocks[index].uncompressed_size;
	out->offset = blocks[index].offset;
}

void package_read_file(const Package *package, Package_File *file, 
					   void *compressed_buffer, void *decompressed_buffer) {
	fseek(package->file, file->offset, SEEK_SET);
	fread(compressed_buffer, file->compressed_size, 1, package->file);
	
	LZ4_decompress_safe((char*)compressed_buffer, (char*)decompressed_buffer, 
						file->compressed_size, file->uncompressed_size);
}