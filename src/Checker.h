#pragma once

#include <stdint.h>

#include <set>
#include <string>
#include <vector>

#include "BfsReader.h"
#include "BitArray.h"
#include "Findings.h"
#include "Geometry.h"


namespace bfs {


struct CheckOptions {
	bool scanOrphans = false;
	bool replayLog = false;
	bool strict = false;
	bool verbose = false;
};


// Read-only integrity checker. Walks the volume, collecting findings without
// stopping on the first error, and cross-checks the block bitmap against actual
// usage. It never modifies the image.
class Checker {
public:
	Checker(BfsReader &reader, Findings &findings, const CheckOptions &options);

	// Returns true if the volume passed: no errors (and, under --strict, no
	// warnings either).
	bool Run();

private:
	std::vector<uint8_t> ReadRunBytes(const BlockRun &run);
	bool ValidRun(const BlockRun &run, const std::string &category,
		const std::string &path, int64_t owner);
	void Reference(int64_t block, const std::string &path, int64_t owner);
	bool InRange(int64_t block) const
	{
		return block >= 0 && block < fGeo.numBlocks;
	}

	void LoadBitmap();
	bool BitmapAllocated(int64_t block) const;
	void MarkReserved();

	void CheckDataStream(const Inode &inode, const std::string &path);
	void CheckBTree(const std::vector<uint8_t> &tree, uint32_t keyType,
		bool isDirectory, const std::string &path, std::vector<DirEntry> *entriesOut);

	void CheckInode(int64_t block, const std::string &path, int64_t expectedParent);
	void CheckDirectory(const Inode &inode, const std::string &path);
	void CheckAttributeDir(int64_t block, const std::string &path);
	void CheckIndexDirectory(int64_t block);
	void CheckIndex(int64_t block, const std::string &name);

	void OrphanScan();
	void CrossCheckBitmap();
	void CheckSuperblock();
	void SetPhase(const char *phase);
	void Tick();

	uint32_t KeyTypeFromMode(uint32_t mode) const;

	// On-disk field accessors bound to the volume's byte order.
	uint32_t U32(const uint8_t *p) const {return GetU32(p, fOrder);}
	int32_t S32(const uint8_t *p) const {return GetS32(p, fOrder);}
	BlockRun Run(const uint8_t *p) const {return GetBlockRun(p, fOrder);}

	BfsReader &fReader;
	Findings &fFindings;
	CheckOptions fOptions;
	Geometry fGeo;
	ByteOrder fOrder;
	int64_t fReserved = 0;
	std::vector<uint8_t> fBitmap;
	BitArray fReferenced;
	std::set<int64_t> fVisitedInodes;
	std::set<int64_t> fOrphanBlocks;
	int64_t fNodesChecked = 0;
	const char *fPhase = "checking";
};


} // bfs
