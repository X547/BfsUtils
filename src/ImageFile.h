#pragma once

#include <stdint.h>
#include <string>
#include <vector>


namespace bfs {


// The size in bytes of the device node at 'path', or 0 when the path is not a
// device (including when it does not exist). Lets a writer size a volume to the
// target before opening it for writing.
uint64_t DeviceSizeOf(const std::string &path);


// A block-addressable image backed by a regular file or by a block device.
//
// A regular file is created as a sparse file of the full volume size; only
// blocks the builder actually writes consume space, and every unwritten block
// reads back as zero (which is exactly what the free areas of a fresh BFS
// volume require). A device offers no such guarantee -- it holds whatever its
// previous owner left -- so a writer targeting one must zero what it relies on
// being zero. A device also cannot change size, so Truncate() only checks that
// the requested size fits.
class ImageFile {
public:
	enum class Access {
		ReadOnly,
		ReadWrite,
	};

	// Create a new image of the given size for writing (used by makebfs).
	ImageFile(const std::string &path, uint64_t sizeBytes, uint32_t blockSize);
	// Open an existing image read-only (used by bfsextract / bfscheck).
	explicit ImageFile(const std::string &path);
	// Open an existing image with the given access (used by bfsresize).
	ImageFile(const std::string &path, Access access);
	~ImageFile();

	ImageFile(const ImageFile &) = delete;
	ImageFile &operator=(const ImageFile &) = delete;

	uint32_t BlockSize() const {return fBlockSize;}
	uint64_t Size() const {return fSize;}

	// True when the target is a device node rather than a regular file.
	bool IsDevice() const {return fIsDevice;}
	// The device's logical sector size; 0 for a regular file.
	uint32_t SectorSize() const {return fSectorSize;}

	// Write one full block (fBlockSize bytes) at the given block number.
	void WriteBlock(int64_t blockNumber, const uint8_t *data);

	// Write an arbitrary byte range at an absolute offset.
	void WriteAt(uint64_t offset, const uint8_t *data, size_t length);

	// Read an arbitrary byte range from an absolute offset.
	void ReadAt(uint64_t offset, uint8_t *data, size_t length);

	// Change the file length (grow or shrink the backing file). On a device this
	// only verifies that the requested size fits, since the size is fixed.
	void Truncate(uint64_t sizeBytes);

	// Write zeroes over a byte range. Needed on a device, where unwritten blocks
	// hold stale data rather than the zeroes a sparse file provides.
	void ZeroRange(uint64_t offset, uint64_t length);

	void Flush();

private:
	int fFd = -1;
	uint32_t fBlockSize = 0;
	uint64_t fSize = 0;
	bool fIsDevice = false;
	uint32_t fSectorSize = 0;
	std::string fPath;
};


} // bfs
