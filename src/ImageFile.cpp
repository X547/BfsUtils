#include "ImageFile.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#	include <linux/fs.h>
#	include <sys/ioctl.h>
#elif defined(__HAIKU__)
#	include <Drivers.h>
#endif

#include <stdexcept>
#include <system_error>
#include <vector>

#include "MountCheck.h"


namespace bfs {


namespace {


// Whether a stat result names a disk device node.
bool IsDeviceNode(const struct stat &info)
{
#if defined(__HAIKU__)
	// Haiku publishes the whole-device 'raw' node as a character device and each
	// partition on it as a block device, so both spellings can name a disk.
	return S_ISBLK(info.st_mode) || S_ISCHR(info.st_mode);
#else
	return S_ISBLK(info.st_mode);
#endif
}


// Determine a device's size and logical sector size. st_size cannot be used:
// Linux reports 0 for a block device, and Haiku fills it in for a partition node
// but reports 0 for the whole-device 'raw' node.
void QueryDeviceGeometry(int fd, const std::string &path, uint64_t &size,
	uint32_t &sectorSize)
{
#if defined(__linux__)
	uint64_t bytes = 0;
	if (::ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot determine the size of device " + path);
	}
	int logical = 0;
	if (::ioctl(fd, BLKSSZGET, &logical) != 0 || logical <= 0) {
		logical = 512;
	}
	size = bytes;
	sectorSize = static_cast<uint32_t>(logical);
#elif defined(__HAIKU__)
	device_geometry geometry;
	if (::ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) != 0) {
		// A character device that is not a disk (or a disk driver that does not
		// implement the call) lands here, with ENOTTY.
		throw std::system_error(errno, std::generic_category(),
			"cannot determine the size of device " + path);
	}
	// B_GET_DEVICE_SIZE is deliberately not consulted as a fallback: on a
	// partition node it reports the size of the whole underlying device, which
	// would let a writer run off the end of the partition. B_GET_GEOMETRY is
	// partition-relative on both node types, so it is the only source used.
	size = static_cast<uint64_t>(geometry.bytes_per_sector)
		* geometry.sectors_per_track * geometry.cylinder_count
		* geometry.head_count;
	// bytes_per_physical_sector is not trustworthy -- the file-backed virtual
	// driver leaves it uninitialized -- so the logical size is what is used.
	sectorSize = geometry.bytes_per_sector != 0 ? geometry.bytes_per_sector : 512;
#else
	(void)fd;
	(void)size;
	(void)sectorSize;
	throw std::runtime_error(
		"operating on device nodes is not supported on this platform: " + path);
#endif
	if (size == 0) {
		throw std::runtime_error("device reports a size of zero: " + path);
	}
}


// Writing into a device that a file system is currently serving corrupts it
// under the running kernel, so this is refused outright rather than warned
// about. Read-only access is unaffected.
void RefuseIfMounted(const std::string &path)
{
	std::string where;
	if (IsDeviceMounted(path, where)) {
		throw std::runtime_error("device " + path + " is mounted"
			+ (where.empty() ? "" : " " + where) + "; unmount it first");
	}
}


} // unnamed namespace


uint64_t DeviceSizeOf(const std::string &path)
{
	struct stat info;
	if (::stat(path.c_str(), &info) != 0 || !IsDeviceNode(info)) {
		return 0;
	}
	int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot open device " + path);
	}
	uint64_t size = 0;
	uint32_t sectorSize = 0;
	try {
		QueryDeviceGeometry(fd, path, size, sectorSize);
	} catch (...) {
		::close(fd);
		throw;
	}
	::close(fd);
	return size;
}


ImageFile::ImageFile(const std::string &path, uint64_t sizeBytes, uint32_t blockSize):
	fBlockSize(blockSize),
	fPath(path)
{
	// A device already exists and cannot be created or resized, so it is opened
	// in place; only a regular file is created and truncated to size.
	struct stat existing;
	bool isDevice = ::stat(path.c_str(), &existing) == 0 && IsDeviceNode(existing);
	if (isDevice) {
		RefuseIfMounted(path);
	}

	fFd = ::open(path.c_str(), isDevice ? O_RDWR : (O_RDWR | O_CREAT | O_TRUNC), 0644);
	if (fFd < 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot create image file " + path);
	}

	if (isDevice) {
		try {
			QueryDeviceGeometry(fFd, path, fSize, fSectorSize);
		} catch (...) {
			::close(fFd);
			fFd = -1;
			throw;
		}
		fIsDevice = true;
		if (sizeBytes > fSize) {
			::close(fFd);
			fFd = -1;
			throw std::runtime_error("requested volume is larger than device "
				+ path + " (" + std::to_string(sizeBytes) + " > "
				+ std::to_string(fSize) + " bytes)");
		}
		// Blocks that straddle sector boundaries would turn every block write
		// into a read-modify-write, and on a device that rejects unaligned I/O
		// they would simply fail. A 4Kn disk with a 1 KiB block size is the case
		// this catches.
		if (fSectorSize != 0 && fBlockSize % fSectorSize != 0) {
			::close(fFd);
			fFd = -1;
			throw std::runtime_error("block size " + std::to_string(fBlockSize)
				+ " is not a multiple of the " + std::to_string(fSectorSize)
				+ "-byte sector size of device " + path);
		}
		return;
	}

	if (::ftruncate(fFd, static_cast<off_t>(sizeBytes)) != 0) {
		int err = errno;
		::close(fFd);
		fFd = -1;
		throw std::system_error(err, std::generic_category(),
			"cannot size image file " + path);
	}
	fSize = sizeBytes;
}


ImageFile::ImageFile(const std::string &path):
	ImageFile(path, Access::ReadOnly)
{
}


ImageFile::ImageFile(const std::string &path, Access access):
	fPath(path)
{
	int flags = access == Access::ReadWrite ? O_RDWR : O_RDONLY;
	if (access == Access::ReadWrite && DeviceSizeOf(path) != 0) {
		RefuseIfMounted(path);
	}
	fFd = ::open(path.c_str(), flags);
	if (fFd < 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot open image file " + path);
	}
	struct stat info;
	if (::fstat(fFd, &info) != 0) {
		int err = errno;
		::close(fFd);
		fFd = -1;
		throw std::system_error(err, std::generic_category(),
			"cannot stat image file " + path);
	}

	if (IsDeviceNode(info)) {
		try {
			QueryDeviceGeometry(fFd, path, fSize, fSectorSize);
		} catch (...) {
			::close(fFd);
			fFd = -1;
			throw;
		}
		fIsDevice = true;
		return;
	}
	fSize = static_cast<uint64_t>(info.st_size);
}


ImageFile::~ImageFile()
{
	if (fFd >= 0) {
		::close(fFd);
	}
}


void ImageFile::Truncate(uint64_t sizeBytes)
{
	if (fIsDevice) {
		// A device keeps its size: shrinking simply stops addressing the tail.
		// Growing past the end is a caller error, not something to ignore.
		if (sizeBytes > fSize) {
			throw std::runtime_error("volume of " + std::to_string(sizeBytes)
				+ " bytes does not fit device " + fPath + " ("
				+ std::to_string(fSize) + " bytes)");
		}
		return;
	}
	if (::ftruncate(fFd, static_cast<off_t>(sizeBytes)) != 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot resize image file " + fPath);
	}
	fSize = sizeBytes;
}


void ImageFile::ZeroRange(uint64_t offset, uint64_t length)
{
	const size_t kChunk = 1024 * 1024;
	std::vector<uint8_t> zeros(length < kChunk ? static_cast<size_t>(length) : kChunk, 0);
	uint64_t written = 0;
	while (written < length) {
		uint64_t remaining = length - written;
		size_t take = remaining < zeros.size() ? static_cast<size_t>(remaining) : zeros.size();
		WriteAt(offset + written, zeros.data(), take);
		written += take;
	}
}


void ImageFile::ReadAt(uint64_t offset, uint8_t *data, size_t length)
{
	size_t read = 0;
	while (read < length) {
		ssize_t n = ::pread(fFd, data + read, length - read,
			static_cast<off_t>(offset + read));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::system_error(errno, std::generic_category(),
				"read from image failed");
		}
		if (n == 0) {
			throw std::runtime_error("unexpected end of image file");
		}
		read += static_cast<size_t>(n);
	}
}


void ImageFile::WriteAt(uint64_t offset, const uint8_t *data, size_t length)
{
	size_t written = 0;
	while (written < length) {
		ssize_t n = ::pwrite(fFd, data + written, length - written,
			static_cast<off_t>(offset + written));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::system_error(errno, std::generic_category(),
				"write to image failed");
		}
		written += static_cast<size_t>(n);
	}
}


void ImageFile::WriteBlock(int64_t blockNumber, const uint8_t *data)
{
	WriteAt(static_cast<uint64_t>(blockNumber) * fBlockSize, data, fBlockSize);
}


void ImageFile::Flush()
{
	if (fFd >= 0) {
		::fsync(fFd);
	}
}


} // bfs
