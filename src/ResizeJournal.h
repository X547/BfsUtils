#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>


namespace bfs {


class ImageFile;


// Where the sidecar journal for a target lives, absent an explicit override.
// For a regular file it sits beside it. A device node cannot host a sidecar --
// devfs is not writable -- so the journal goes into the current directory under
// a name derived from the device path. That derivation must stay deterministic:
// recovery on the next run finds the journal by recomputing this name, so a
// temporary or randomized one would quietly defeat the crash-safety.
std::string DefaultJournalPath(const std::string &imagePath, bool isDevice);


// A sidecar write-ahead redo-journal that makes a set of block writes atomic
// with respect to a crash. A transaction's blocks are written durably to the
// journal file (with a validating footer) before being applied to the image;
// if the process is killed mid-apply, Recover() re-applies the committed
// transaction on the next run. A torn journal write fails the footer check and
// is discarded, leaving the image in its pre-transaction state.
class ResizeJournal {
public:
	// Takes the journal's own path (see DefaultJournalPath), not the image's.
	explicit ResizeJournal(const std::string &journalPath);
	~ResizeJournal();

	ResizeJournal(const ResizeJournal &) = delete;
	ResizeJournal &operator=(const ResizeJournal &) = delete;

	// Replay a committed-but-unapplied transaction into the image (idempotent),
	// then clear the journal. Must be called before reading the superblock, as a
	// pending transaction may include it. Returns true if something was replayed.
	bool Recover(ImageFile &image);

	void SetBlockSize(uint32_t blockSize) {fBlockSize = blockSize;}

	void Begin() {fEntries.clear();}
	void Stage(int64_t block, const uint8_t *bytes);
	bool Empty() const {return fEntries.empty();}
	size_t Count() const {return fEntries.size();}

	// Durably record the staged blocks, apply them to the image, and clear.
	void Commit(ImageFile &image);

	// Delete the journal file (call once the whole resize has completed).
	void Remove();

private:
	std::vector<uint8_t> Serialize() const;
	void WriteAll(uint64_t offset, const uint8_t *data, size_t length);

	std::string fPath;
	int fFd = -1;
	uint32_t fBlockSize = 0;
	std::vector<std::pair<int64_t, std::vector<uint8_t>>> fEntries;
};


} // bfs
