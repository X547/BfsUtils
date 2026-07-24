#pragma once

#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include "BfsReader.h"
#include "Geometry.h"


namespace bfs {


// One classification per block. The order matters: when two roles would apply
// to the same block, the walker keeps the first (more specific) assignment, and
// blocks still unclassified after the walk but marked used in the bitmap become
// kBlockLeaked.
enum BlockType : uint8_t {
	kBlockFree = 0,       // not allocated in the bitmap
	kBlockReserved,       // boot block + superblock
	kBlockBitmap,         // allocation-group block bitmap
	kBlockJournal,        // journal / log area
	kBlockInode,          // an inode (file, directory, or attribute-dir header)
	kBlockMetadata,       // directory / attribute-dir B+tree stream blocks
	kBlockIndex,          // index directory / index inodes and their B+trees
	kBlockIndirect,       // (double-)indirect block_run array blocks
	kBlockFileData,       // regular-file data stored in a single run
	kBlockFragmented,     // regular-file data belonging to a multi-run file
	kBlockAttrData,       // attribute value data
	kBlockLeaked,         // allocated in the bitmap but unreachable from the root
	kBlockTypeCount
};


struct BlockTypeInfo {
	const char *name;
	uint8_t r;
	uint8_t g;
	uint8_t b;
};


// Display name and swatch color for each BlockType, indexed by the enum.
extern const BlockTypeInfo kBlockTypeInfo[kBlockTypeCount];


// Walks a BFS volume (read-only) and assigns every block a BlockType, so the
// result can be rendered as a usage map. It mirrors the checker's traversal but
// records block roles instead of validating them.
class BlockMap {
public:
	explicit BlockMap(BfsReader &reader);

	// Classify every block. Safe on damaged volumes: unreadable inodes and
	// out-of-range runs are skipped rather than aborting the scan.
	void Build();

	const std::vector<uint8_t> &Types() const {return fTypes;}
	const int64_t *Counts() const {return fCounts;}
	int64_t NumBlocks() const {return fGeo.numBlocks;}

private:
	void SetType(int64_t block, BlockType type);
	bool InRange(int64_t block) const
	{
		return block >= 0 && block < fGeo.numBlocks;
	}

	void MarkReserved();
	void LoadBitmap();
	bool BitmapAllocated(int64_t block) const;

	// How to treat an inode reached during the walk. The indices directory and
	// each index both carry S_INDEX_DIR, so the role — not the mode — decides
	// whether to recurse: we descend into the index root's entries (they are
	// indexes) but never into an index's entries (they are file references).
	enum InodeRole {
		kRoleNormal,      // an inode in the main file hierarchy
		kRoleIndexRoot,   // the indices directory itself
		kRoleIndex,       // an individual index
	};

	std::vector<uint8_t> ReadRunBytes(const BlockRun &run);

	// Collect the data runs and indirect-array runs of a stream. 'dataRuns'
	// receives (startBlock, length) extents holding stream content; 'metaRuns'
	// receives the indirect / double-indirect array blocks.
	void CollectStream(const DataStreamInfo &stream,
		std::vector<std::pair<int64_t, int32_t>> &dataRuns,
		std::vector<std::pair<int64_t, int32_t>> &metaRuns);

	void WalkInode(int64_t block, InodeRole role);
	void WalkAttributeDir(int64_t block);

	BfsReader &fReader;
	Geometry fGeo;
	ByteOrder fOrder;
	int64_t fReserved = 0;
	std::vector<uint8_t> fTypes;      // one BlockType per block
	std::vector<uint8_t> fBitmap;
	std::set<int64_t> fVisited;
	int64_t fCounts[kBlockTypeCount] = {};
};


} // bfs
