#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Attribute.h"
#include "BfsFormat.h"
#include "Geometry.h"


namespace bfs {


class ImageFile;


// The parsed data_stream of an inode (see BFS_On-Disk_Format.md section 7).
struct DataStreamInfo {
	BlockRun direct[kNumDirectBlocks];
	int64_t maxDirectRange = 0;
	BlockRun indirect;
	int64_t maxIndirectRange = 0;
	BlockRun doubleIndirect;
	int64_t maxDoubleIndirectRange = 0;
	int64_t size = 0;
};


struct Inode {
	int64_t block = 0;
	uint32_t mode = 0;
	uint32_t flags = 0;
	uint32_t uid = 0;
	uint32_t gid = 0;
	int64_t createTime = 0;         // BFS-encoded (see TimeEncoding.h)
	int64_t modifiedTime = 0;
	int64_t statusChangeTime = 0;
	uint32_t type = 0;
	BlockRun parent;
	BlockRun attributes;
	DataStreamInfo stream;
	std::vector<uint8_t> raw;       // full inode block (for small_data / symlink)

	bool IsDirectory() const {return (mode & kSIfmt) == kSIfdir;}
	bool IsSymlink() const {return (mode & kSIfmt) == kSIflnk;}
	bool IsRegularFile() const
	{
		return (mode & (kSIfmt | kSAttrDir | kSAttr | kSIndexDir)) == kSIfreg;
	}
	bool IsLongSymlink() const {return (flags & kInodeLongSymlink) != 0;}
};


struct DirEntry {
	std::string name;
	int64_t inode = 0;   // block number of the entry's inode
};


// Read-only interpreter of a BFS volume image. Everything is parsed on demand;
// no full in-memory tree is built. Reads pass through an in-memory overlay so
// that ReplayLog() can present the post-replay state without touching the file.
class BfsReader {
public:
	explicit BfsReader(ImageFile &image);

	const Geometry &GetGeometry() const {return fGeometry;}
	BlockRun RootDirectory() const {return fRootDir;}
	BlockRun IndexDirectory() const {return fIndices;}
	const std::string &VolumeName() const {return fName;}
	int64_t UsedBlocks() const {return fUsedBlocks;}
	bool IsClean() const {return fLogStart == fLogEnd;}

	// Replay the journal into the overlay so subsequent reads observe the
	// post-transaction state (see BFS_On-Disk_Format.md section 14).
	void ReplayLog();

	Inode ReadInode(int64_t block);

	// Stream a data stream in order, invoking 'sink' for each chunk.
	void ReadStream(const DataStreamInfo &stream,
		const std::function<void(const uint8_t *, size_t)> &sink);
	std::vector<uint8_t> ReadStreamFully(const DataStreamInfo &stream);

	std::string ReadSymlink(const Inode &inode);
	std::vector<DirEntry> ReadDirectory(const Inode &inode);
	std::vector<Attribute> ReadAttributes(const Inode &inode);

	// Raw block read (consults the journal overlay). Exposed for the integrity
	// checker, which needs low-level access beyond the high-level readers.
	void ReadBlock(int64_t block, uint8_t *out);

	// Reserved-region size in blocks: boot block + bitmap + log.
	int64_t ReservedBlocks() const
	{
		return 1 + fGeometry.bitmapBlocks + fGeometry.logBlocks.length;
	}

private:
	std::vector<uint8_t> ReadRun(const BlockRun &run);
	BlockRun ResolveRun(const DataStreamInfo &stream, int64_t pos, int64_t &runStart);
	void ReadSmallData(const Inode &inode, std::vector<Attribute> &out);
	void IterateDirTree(const std::vector<uint8_t> &tree, std::vector<DirEntry> &out);

	ImageFile &fImage;
	Geometry fGeometry;
	BlockRun fRootDir;
	BlockRun fIndices;
	int64_t fLogStart = 0;
	int64_t fLogEnd = 0;
	int64_t fUsedBlocks = 0;
	std::string fName;
	std::map<int64_t, std::vector<uint8_t>> fOverlay;
};


} // bfs
