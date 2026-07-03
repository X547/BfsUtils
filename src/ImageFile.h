#pragma once

#include <stdint.h>
#include <string>
#include <vector>


namespace bfs {


// A block-addressable output image backed by a regular file. The file is
// created as a sparse file of the full volume size; only blocks the builder
// actually writes consume space, and every unwritten block reads back as zero
// (which is exactly what the free areas of a fresh BFS volume require).
class ImageFile {
public:
	// Create a new image of the given size for writing (used by makebfs).
	ImageFile(const std::string &path, uint64_t sizeBytes, uint32_t blockSize);
	// Open an existing image read-only (used by bfsextract).
	explicit ImageFile(const std::string &path);
	~ImageFile();

	ImageFile(const ImageFile &) = delete;
	ImageFile &operator=(const ImageFile &) = delete;

	uint32_t BlockSize() const {return fBlockSize;}
	uint64_t Size() const {return fSize;}

	// Write one full block (fBlockSize bytes) at the given block number.
	void WriteBlock(int64_t blockNumber, const uint8_t *data);

	// Write an arbitrary byte range at an absolute offset.
	void WriteAt(uint64_t offset, const uint8_t *data, size_t length);

	// Read an arbitrary byte range from an absolute offset.
	void ReadAt(uint64_t offset, uint8_t *data, size_t length);

	void Flush();

private:
	int fFd = -1;
	uint32_t fBlockSize = 0;
	uint64_t fSize = 0;
	std::string fPath;
};


} // bfs
