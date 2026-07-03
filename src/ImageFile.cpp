#include "ImageFile.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>


namespace bfs {


ImageFile::ImageFile(const std::string &path, uint64_t sizeBytes, uint32_t blockSize):
	fBlockSize(blockSize),
	fPath(path)
{
	fFd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fFd < 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot create image file " + path);
	}
	if (::ftruncate(fFd, static_cast<off_t>(sizeBytes)) != 0) {
		int err = errno;
		::close(fFd);
		fFd = -1;
		throw std::system_error(err, std::generic_category(),
			"cannot size image file " + path);
	}
}


ImageFile::ImageFile(const std::string &path):
	fPath(path)
{
	fFd = ::open(path.c_str(), O_RDONLY);
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
	fSize = static_cast<uint64_t>(info.st_size);
}


ImageFile::~ImageFile()
{
	if (fFd >= 0) {
		::close(fFd);
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
