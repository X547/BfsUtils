#include "ResizeJournal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>

#include "Endian.h"
#include "ImageFile.h"


namespace bfs {


namespace {


constexpr uint32_t kJournalMagic = 0x314a5242;   // 'BJR1'
constexpr uint32_t kJournalDone = 0x454e4f44;    // 'DONE'
constexpr int kHeaderSize = 16;   // magic, blockSize, entryCount, reserved
constexpr int kFooterSize = 12;   // checksum(8) + DONE(4)


uint64_t Fnv1a(const uint8_t *data, size_t length)
{
	uint64_t hash = 1469598103934665603ull;
	for (size_t i = 0; i < length; i++) {
		hash ^= data[i];
		hash *= 1099511628211ull;
	}
	return hash;
}


} // unnamed namespace


ResizeJournal::ResizeJournal(const std::string &imagePath):
	fPath(imagePath + ".bfsresize-journal")
{
	fFd = ::open(fPath.c_str(), O_RDWR | O_CREAT, 0644);
	if (fFd < 0) {
		throw std::system_error(errno, std::generic_category(),
			"cannot open journal " + fPath);
	}
}


ResizeJournal::~ResizeJournal()
{
	if (fFd >= 0) {
		// Remove an empty journal (no committed transaction) so a failed or
		// no-op run leaves nothing behind; keep a non-empty one for recovery.
		off_t size = ::lseek(fFd, 0, SEEK_END);
		::close(fFd);
		fFd = -1;
		if (size == 0) {
			::unlink(fPath.c_str());
		}
	}
}


void ResizeJournal::WriteAll(uint64_t offset, const uint8_t *data, size_t length)
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
				"journal write failed");
		}
		written += static_cast<size_t>(n);
	}
}


std::vector<uint8_t> ResizeJournal::Serialize() const
{
	size_t body = static_cast<size_t>(kHeaderSize)
		+ fEntries.size() * (8 + fBlockSize);
	std::vector<uint8_t> buffer(body + kFooterSize, 0);

	PutU32(&buffer[0], kJournalMagic);
	PutU32(&buffer[4], fBlockSize);
	PutU32(&buffer[8], static_cast<uint32_t>(fEntries.size()));

	size_t offset = kHeaderSize;
	for (const auto &entry : fEntries) {
		PutS64(&buffer[offset], entry.first);
		::memcpy(&buffer[offset + 8], entry.second.data(), fBlockSize);
		offset += 8 + fBlockSize;
	}

	uint64_t checksum = Fnv1a(buffer.data(), body);
	PutU64(&buffer[body], checksum);
	PutU32(&buffer[body + 8], kJournalDone);
	return buffer;
}


void ResizeJournal::Stage(int64_t block, const uint8_t *bytes)
{
	fEntries.emplace_back(block, std::vector<uint8_t>(bytes, bytes + fBlockSize));
}


void ResizeJournal::Commit(ImageFile &image)
{
	if (fEntries.empty()) {
		return;
	}

	std::vector<uint8_t> buffer = Serialize();
	size_t body = buffer.size() - kFooterSize;

	// Two-phase durability: entries first, then the validating footer.
	if (::ftruncate(fFd, 0) != 0) {
		throw std::system_error(errno, std::generic_category(), "journal reset failed");
	}
	WriteAll(0, buffer.data(), body);
	::fsync(fFd);
	WriteAll(body, buffer.data() + body, kFooterSize);
	if (::ftruncate(fFd, static_cast<off_t>(buffer.size())) != 0) {
		throw std::system_error(errno, std::generic_category(), "journal size failed");
	}
	::fsync(fFd);

	// Test hook: simulate a crash after the transaction is durably committed to
	// the journal but before it is applied to the image. The next run recovers.
	if (::getenv("BFSRESIZE_CRASH_AFTER_COMMIT") != nullptr) {
		::_exit(99);
	}

	// Now durable; apply to the image. Write by explicit offset: the image may
	// have been opened without a known block size.
	for (const auto &entry : fEntries) {
		image.WriteAt(static_cast<uint64_t>(entry.first) * fBlockSize,
			entry.second.data(), fBlockSize);
	}
	image.Flush();

	// Transaction done: clear the journal.
	if (::ftruncate(fFd, 0) != 0) {
		throw std::system_error(errno, std::generic_category(), "journal clear failed");
	}
	::fsync(fFd);
	fEntries.clear();
}


bool ResizeJournal::Recover(ImageFile &image)
{
	off_t size = ::lseek(fFd, 0, SEEK_END);
	if (size < kHeaderSize + kFooterSize) {
		if (size > 0) {
			(void)!::ftruncate(fFd, 0);
		}
		return false;
	}

	std::vector<uint8_t> buffer(static_cast<size_t>(size));
	size_t read = 0;
	while (read < buffer.size()) {
		ssize_t n = ::pread(fFd, buffer.data() + read, buffer.size() - read,
			static_cast<off_t>(read));
		if (n <= 0) {
			(void)!::ftruncate(fFd, 0);
			return false;
		}
		read += static_cast<size_t>(n);
	}

	size_t body = buffer.size() - kFooterSize;
	if (GetU32(&buffer[0]) != kJournalMagic
		|| GetU32(&buffer[body + 8]) != kJournalDone
		|| GetU64(&buffer[body]) != Fnv1a(buffer.data(), body)) {
		(void)!::ftruncate(fFd, 0);   // torn / uncommitted: discard
		return false;
	}
	fprintf(stderr,
		"bfsresize: recovering a transaction from a previously interrupted run\n");

	uint32_t blockSize = GetU32(&buffer[4]);
	uint32_t count = GetU32(&buffer[8]);
	size_t expected = static_cast<size_t>(kHeaderSize)
		+ static_cast<size_t>(count) * (8 + blockSize) + kFooterSize;
	if (blockSize == 0 || expected != buffer.size()) {
		(void)!::ftruncate(fFd, 0);
		return false;
	}

	size_t offset = kHeaderSize;
	for (uint32_t i = 0; i < count; i++) {
		int64_t block = GetS64(&buffer[offset]);
		image.WriteAt(static_cast<uint64_t>(block) * blockSize,
			&buffer[offset + 8], blockSize);
		offset += 8 + blockSize;
	}
	image.Flush();

	(void)!::ftruncate(fFd, 0);
	::fsync(fFd);
	return true;
}


void ResizeJournal::Remove()
{
	if (fFd >= 0) {
		::close(fFd);
		fFd = -1;
	}
	::unlink(fPath.c_str());
}


} // bfs
