#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BPlusTreeBuilder.h"
#include "BfsFormat.h"
#include "DataStream.h"
#include "Geometry.h"
#include "Node.h"
#include "Progress.h"


namespace bfs {


class ImageFile;
class BlockAllocator;


struct BuildOptions {
	uint32_t blockSize = 2048;
	std::string volumeName = "bfs";
	bool generateIndices = true;
	bool readAttributes = false;   // effective on Haiku
	uint64_t sizeOverride = 0;     // 0 == smallest possible image, or fill a device
	bool zeroFree = false;         // zero the whole target first (devices only)
	ByteOrder byteOrder = ByteOrder::Little;   // on-disk byte order to write
};


// Turns a scanned source tree into a fresh, clean BFS image. See BfsBuilder.cpp
// for the four-phase algorithm (plan/measure, geometry, place, serialize).
class BfsBuilder {
public:
	BfsBuilder(const BuildOptions &options, Progress &progress);

	void Build(Node &root, const std::string &outputPath);

private:
	enum class StreamKind {
		None,
		File,
		Bytes,
		ShortSymlink,
		Tree,
	};

	struct InodePlan {
		int64_t block = 0;

		uint32_t mode = 0;
		uint32_t flags = kInodeInUse;
		uint32_t uid = 0;
		uint32_t gid = 0;
		ScanTime createTime;
		ScanTime modifiedTime;
		ScanTime changeTime;
		uint32_t type = 0;

		int parentPlan = -1;   // -1 => parent is the inode itself
		int attrDirPlan = -1;  // -1 => no attribute directory

		std::vector<uint8_t> smallData;

		StreamKind streamKind = StreamKind::None;
		std::string filePath;
		std::vector<uint8_t> bytes;
		std::string shortSymlink;
		uint64_t streamSize = 0;
		int64_t reportedSize = 0;

		std::shared_ptr<BPlusTreeBuilder> tree;
		std::vector<std::vector<int>> treeGroups;

		int64_t metadataBlocks = 0;   // reserved array/alignment blocks (indirect tiers)
		StreamLayout layout;
	};

	int NewInode();
	int PlanNode(Node &node, int parentPlan, bool isRoot);
	int PlanAttributeDir(Node &node, int ownerPlan,
		const std::vector<const Attribute *> &largeAttributes);
	void PlanIndices();
	int PlanIndex(const std::string &name, uint32_t dataType, uint32_t modeTypeBit,
		int parentPlan, std::vector<std::pair<std::vector<uint8_t>, Node *>> &inputs);

	int64_t DataBlocks(const InodePlan &plan) const;
	int64_t StreamBlocks(const InodePlan &plan) const;
	void ComputeGeometry();
	void Place(BlockAllocator &allocator);

	// Blocks the serialize phase will actually write, which is the denominator of
	// the "writing" phase. This is deliberately not allocator.UsedBlocks(): that
	// also counts the log area and the reserved prefix, which are never written,
	// and would leave the phase stuck short of 100%.
	int64_t BlocksToWrite() const;

	void SerializeSuperBlock(ImageFile &image, int64_t usedBlocks,
		const BlockRun &indicesRun);
	void SerializeInode(ImageFile &image, InodePlan &plan);
	std::vector<int64_t> TreeValues(const InodePlan &plan) const;

	int64_t ParentBlock(const InodePlan &plan) const;
	int64_t FirstFreeBlock() const;

	// On-disk field writers bound to the volume's byte order (fOptions.byteOrder).
	void WU16(uint8_t *p, uint16_t v) const {PutU16(p, v, fOptions.byteOrder);}
	void WU32(uint8_t *p, uint32_t v) const {PutU32(p, v, fOptions.byteOrder);}
	void WS32(uint8_t *p, int32_t v) const {PutS32(p, v, fOptions.byteOrder);}
	void WS64(uint8_t *p, int64_t v) const {PutS64(p, v, fOptions.byteOrder);}
	void WRun(uint8_t *p, const BlockRun &r) const {PutBlockRun(p, r, fOptions.byteOrder);}

	BuildOptions fOptions;
	Progress &fProgress;
	Geometry fGeometry;
	StreamTuning fTuning;
	std::vector<InodePlan> fInodes;
	std::vector<Node *> fContentNodes;   // every scanned node except the root
	int fRootPlan = -1;
	int fIndexDirPlan = -1;
	int64_t fContentBlocks = 0;
	int64_t fLogBlockCount = 0;
};


} // bfs
