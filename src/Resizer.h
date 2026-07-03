#pragma once

#include <stdint.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "BfsFormat.h"
#include "BfsReader.h"
#include "Geometry.h"
#include "ImageFile.h"
#include "ResizeJournal.h"


namespace bfs {


struct ResizeOptions {
	bool dryRun = false;
	bool verbose = false;
};


// Resizes a BFS image in place (offset fixed). Crash-safety comes from a sidecar
// redo-journal plus a superblock-last / copy-before-destroy ordering, so an
// interruption always leaves a mountable volume (the old size until the final
// commit) that a re-run can finish.
class Resizer {
public:
	Resizer(const std::string &imagePath, const ResizeOptions &options);

	// Resize to 'newSizeBytes' (rounded down to a block multiple). Returns the
	// number of blocks relocated.
	int64_t Run(uint64_t newSizeBytes);

private:
	int64_t Reserved(int64_t bitmapBlocks) const;
	int64_t BitmapBlocksFor(int64_t numBlocks) const;

	bool Allocated(int64_t block) const;
	void SetAllocated(int64_t block, bool used);
	int64_t AllocateBlock(int64_t low, int64_t limit);
	int64_t AllocateRun(int64_t length, int64_t low, int64_t limit);

	void StageDirtyBitmap();
	int64_t CountUsed(int64_t numBlocks) const;
	// Read block 0, patch num_blocks / num_ags / used_blocks (and indices when
	// given a non-null run), and return the full block for staging.
	std::vector<uint8_t> BuildSuperBlock(int64_t numBlocks, int64_t numAgs,
		int64_t usedBlocks, const BlockRun *indices);

	void Grow(int64_t newNum);
	void ShrinkFreeTail(int64_t newNum);
	int64_t Relocate(int64_t newNum);

	void CollectInodes(int64_t block, std::set<int64_t> &visited,
		std::vector<Inode> &out);
	void CollectContainerChildren(const Inode &container, std::set<int64_t> &visited,
		std::vector<Inode> &out);
	std::vector<Inode> CollectAllInodes();
	std::vector<uint8_t> ReadRunBytes(const BlockRun &run);
	void CopyRun(int64_t oldStart, int64_t newStart, uint16_t length);

	// Phase A: relocate the tail-crossing data of one inode's stream (direct,
	// indirect array + its payload runs, double-indirect at all three levels).
	// Returns the number of blocks moved; with 'dryRun' it only counts.
	int64_t RelocateStreamTail(const Inode &inode, int64_t newNum, bool dryRun);
	// Copy 'oldRun' to fresh space below 'newNum', repoint the block_run stored at
	// (ownerBlock, slotOffset) to the copy, free the old run, all in one atomic
	// journal transaction. 'ownerBlock' may itself be an inode, an indirect array
	// block, or a double-indirect top block.
	void RelocateRun(int64_t ownerBlock, int slotOffset, const BlockRun &oldRun,
		int64_t newNum);

	// Phase B: relocate inodes out of the tail with full reference fixup.
	struct RunFieldRef {          // a block_run field to repoint to the moved inode
		int64_t block;            // block holding the field (inode block, or 0 = superblock)
		int offset;               // byte offset of the block_run within that block
	};
	struct TreeValueRef {         // an int64 inode-number slot inside a B+tree stream
		int treeIndex;            // index into fTreeStreams (the owning tree's stream)
		int64_t logicalOffset;    // byte offset of the value within that stream
	};
	struct InodeRefs {
		std::vector<RunFieldRef> fields;
		std::vector<TreeValueRef> treeValues;
	};

	void BuildReferenceMap(const std::vector<Inode> &inodes);
	void ForEachTreeValue(const std::vector<uint8_t> &tree,
		const std::function<void(int64_t logicalOffset, int64_t value)> &cb);
	int64_t StreamPhysicalBlock(const DataStreamInfo &stream, int64_t logicalBlock);
	int64_t CurrentBlock(int64_t originalBlock) const;
	void RelocateInode(int64_t oldBlock, int64_t newNum);

	std::string fImagePath;
	ResizeOptions fOptions;
	ImageFile fImage;
	ResizeJournal fJournal;
	BfsReader fReader;
	Geometry fGeo;
	int64_t fUsedBlocks = 0;

	std::vector<uint8_t> fBitmap;       // full on-disk allocation bitmap
	std::set<int64_t> fDirtyBitmap;     // bitmap block indices changed in memory
	int64_t fRelocated = 0;

	// Reference fixup state (Phase B), built after Phase A has finalized streams.
	std::map<int64_t, InodeRefs> fRefs;         // original inode block -> its references
	std::vector<DataStreamInfo> fTreeStreams;   // stream of each tree-owning inode
	std::map<int64_t, int64_t> fMoved;          // original inode block -> new block
};


} // bfs
