#pragma once

#include <stdint.h>

#include "Endian.h"


// On-disk constants and field offsets for the Be File System, taken directly
// from local/BFS_On-Disk_Format.md. Nothing here is derived from a driver
// implementation. Every multi-byte field is written little-endian (see
// Endian.h); the four-character magics use the C multi-character value
// convention, e.g. 'BFS1' == 0x42465331.

namespace bfs {


// Superblock magics and flags.
constexpr uint32_t kSuperBlockMagic1 = 0x42465331;   // 'BFS1'
constexpr uint32_t kSuperBlockMagic2 = 0xdd121031;
constexpr uint32_t kSuperBlockMagic3 = 0x15b6830e;
constexpr uint32_t kSuperBlockFsLendian = 0x42494745; // 'BIGE'
constexpr uint32_t kSuperBlockDiskClean = 0x434c454e; // 'CLEN'
constexpr uint32_t kSuperBlockDiskDirty = 0x44495254; // 'DIRT'

constexpr int kDiskNameLength = 32;

// The superblock lives at byte offset 512 from the start of the device.
constexpr uint64_t kSuperBlockOffset = 512;

// block_run: max consecutive blocks a single run can cover.
constexpr uint32_t kMaxBlockRunLength = 65535;

// Inode.
constexpr uint32_t kInodeMagic1 = 0x3bbe0ad9;
constexpr int kShortSymlinkNameLength = 144;
constexpr int kNumDirectBlocks = 12;

// data_stream indirect / double-indirect array sizing (see BFS_On-Disk_Format.md
// section 7). The double-indirect tier stores fixed-length runs of
// 'double_indirect.length' (== base) blocks each.
constexpr int kNumArrayBlocks = 4;
constexpr int kDoubleIndirectArraySize = 4096;

constexpr int kInodeTimeShift = 16;
constexpr uint32_t kInodeTimeMask = 0xfff0;

// inode_flags (only the low 16 bits are persistent).
constexpr uint32_t kInodeInUse = 0x00000001;
constexpr uint32_t kInodeLogged = 0x00000008;
constexpr uint32_t kInodeDeleted = 0x00000010;
constexpr uint32_t kInodeNotReady = 0x00000020;
constexpr uint32_t kInodeLongSymlink = 0x00000040;

// Mode bits: POSIX type/permission bits plus BFS type bits.
constexpr uint32_t kSAttrDir = 01000000000u;
constexpr uint32_t kSAttr = 02000000000u;
constexpr uint32_t kSIndexDir = 04000000000u;

constexpr uint32_t kSStrIndex = 00100000000u;
constexpr uint32_t kSIntIndex = 00200000000u;
constexpr uint32_t kSUIntIndex = 00400000000u;
constexpr uint32_t kSLongLongIndex = 00010000000u;
constexpr uint32_t kSULongLongIndex = 00020000000u;
constexpr uint32_t kSFloatIndex = 00040000000u;
constexpr uint32_t kSDoubleIndex = 00001000000u;
constexpr uint32_t kSAllowDups = 00002000000u;

constexpr uint32_t kSIfmt = 00000170000u;
constexpr uint32_t kSIflnk = 00000120000u;
constexpr uint32_t kSIfreg = 00000100000u;
constexpr uint32_t kSIfdir = 00000040000u;

// small_data.
constexpr uint32_t kFileNameType = 0x43535452;  // 'CSTR'
constexpr uint8_t kFileNameName = 0x13;
constexpr int kFileNameNameLength = 1;
constexpr int kMaxIndexKeyLength = 255;

// B+tree.
constexpr uint32_t kBPlusTreeMagic = 0x69f6c2e8;
constexpr uint32_t kBPlusTreeNodeSize = 1024;
constexpr uint32_t kBPlusTreeMaxKeyLength = 256;
constexpr int64_t kBPlusTreeNull = -1;
constexpr int64_t kBPlusTreeFree = -2;

// B+tree key types (bplustree_types).
constexpr uint32_t kBPlusTreeStringType = 0;
constexpr uint32_t kBPlusTreeInt32Type = 1;
constexpr uint32_t kBPlusTreeUInt32Type = 2;
constexpr uint32_t kBPlusTreeInt64Type = 3;
constexpr uint32_t kBPlusTreeUInt64Type = 4;
constexpr uint32_t kBPlusTreeFloatType = 5;
constexpr uint32_t kBPlusTreeDoubleType = 6;

// Duplicate handling.
constexpr int64_t kBPlusTreeDuplicateNode = 2;
constexpr int64_t kBPlusTreeDuplicateFragment = 3;
constexpr int kNumFragmentValues = 7;
constexpr int kNumDuplicateValues = 125;

// Field byte offsets within the disk_super_block structure. The superblock is
// written at kSuperBlockOffset.
namespace super {
	constexpr int kName = 0;
	constexpr int kMagic1 = 32;
	constexpr int kFsByteOrder = 36;
	constexpr int kBlockSize = 40;
	constexpr int kBlockShift = 44;
	constexpr int kNumBlocks = 48;
	constexpr int kUsedBlocks = 56;
	constexpr int kInodeSize = 64;
	constexpr int kMagic2 = 68;
	constexpr int kBlocksPerAg = 72;
	constexpr int kAgShift = 76;
	constexpr int kNumAgs = 80;
	constexpr int kFlags = 84;
	constexpr int kLogBlocks = 88;   // block_run (8 bytes)
	constexpr int kLogStart = 96;
	constexpr int kLogEnd = 104;
	constexpr int kMagic3 = 112;
	constexpr int kRootDir = 116;    // block_run (8 bytes)
	constexpr int kIndices = 124;    // block_run (8 bytes)
	constexpr int kSize = 132;       // used part; padded out to a block
}

// Field byte offsets within a bfs_inode block.
namespace inode {
	constexpr int kMagic1 = 0;
	constexpr int kInodeNum = 4;     // block_run (8 bytes)
	constexpr int kUid = 12;
	constexpr int kGid = 16;
	constexpr int kMode = 20;
	constexpr int kFlags = 24;
	constexpr int kCreateTime = 28;
	constexpr int kLastModifiedTime = 36;
	constexpr int kParent = 44;      // block_run (8 bytes)
	constexpr int kAttributes = 52;  // block_run (8 bytes)
	constexpr int kType = 60;
	constexpr int kInodeSize = 64;
	constexpr int kEtc = 68;
	constexpr int kData = 72;        // union: data_stream (144 bytes) or short_symlink
	constexpr int kStatusChangeTime = 216;
	constexpr int kPad = 224;
	constexpr int kSmallDataStart = 232;
}

// Field byte offsets within a data_stream, relative to inode::kData.
namespace stream {
	constexpr int kDirect = 0;              // block_run[12]
	constexpr int kMaxDirectRange = 96;
	constexpr int kIndirect = 104;          // block_run
	constexpr int kMaxIndirectRange = 112;
	constexpr int kDoubleIndirect = 120;    // block_run
	constexpr int kMaxDoubleIndirectRange = 128;
	constexpr int kSize = 136;
}

// B+tree header field offsets (at the start of the tree's data stream).
namespace btreehdr {
	constexpr int kMagic = 0;
	constexpr int kNodeSize = 4;
	constexpr int kMaxNumberOfLevels = 8;
	constexpr int kDataType = 12;
	constexpr int kRootNodePointer = 16;
	constexpr int kFreeNodePointer = 24;
	constexpr int kMaximumSize = 32;
	constexpr int kSize = 40;
}

// bplustree_node fixed header field offsets.
namespace btreenode {
	constexpr int kLeftLink = 0;
	constexpr int kRightLink = 8;
	constexpr int kOverflowLink = 16;
	constexpr int kAllKeyCount = 24;
	constexpr int kAllKeyLength = 26;
	// sizeof(bplustree_node) == 28: key data begins immediately after
	// all_key_length. Confirmed against a real BFS volume: the key-length table
	// sits at KeyAlign(28 + all_key_length). (BFS uses a packed on-disk node.)
	constexpr int kSize = 28;
}

// run_array header offsets (unused when creating a clean volume, kept for
// completeness).
namespace runarray {
	constexpr int kCount = 0;
	constexpr int kMaxRuns = 4;
	constexpr int kRuns = 8;
}


// An in-memory block_run. Serialized little-endian as
// (int32 group, uint16 start, uint16 length).
struct BlockRun {
	int32_t group = 0;
	uint16_t start = 0;
	uint16_t length = 0;

	bool IsZero() const {return group == 0 && start == 0 && length == 0;}
};


inline void PutBlockRun(uint8_t *p, const BlockRun &run)
{
	PutS32(p + 0, run.group);
	PutU16(p + 4, run.start);
	PutU16(p + 6, run.length);
}


inline BlockRun GetBlockRun(const uint8_t *p)
{
	BlockRun run;
	run.group = GetS32(p + 0);
	run.start = GetU16(p + 4);
	run.length = GetU16(p + 6);
	return run;
}


inline int64_t KeyAlign(int64_t x)
{
	return (x + 7) & ~static_cast<int64_t>(7);
}


} // bfs
