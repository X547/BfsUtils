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


namespace bfs {


class ImageFile;
class BlockAllocator;


struct BuildOptions {
	uint32_t blockSize = 2048;
	std::string volumeName = "bfs";
	bool generateIndices = true;
	bool readAttributes = false;   // effective on Haiku
	uint64_t sizeOverride = 0;     // 0 == smallest possible image
};


// Turns a scanned source tree into a fresh, clean BFS image. See BfsBuilder.cpp
// for the four-phase algorithm (plan/measure, geometry, place, serialize).
class BfsBuilder {
public:
	explicit BfsBuilder(const BuildOptions &options);

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

	void SerializeSuperBlock(ImageFile &image, int64_t usedBlocks,
		const BlockRun &indicesRun);
	void SerializeInode(ImageFile &image, InodePlan &plan);
	std::vector<int64_t> TreeValues(const InodePlan &plan) const;

	int64_t ParentBlock(const InodePlan &plan) const;
	int64_t FirstFreeBlock() const;

	BuildOptions fOptions;
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
