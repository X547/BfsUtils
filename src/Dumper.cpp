#include "Dumper.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <stdexcept>

#include "BPlusTreeReader.h"
#include "JsonWriter.h"
#include "TimeEncoding.h"


namespace bfs {


namespace {


bool IsPrintable(const uint8_t *data, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (data[i] < 0x20 || data[i] > 0x7e) {
			return false;
		}
	}
	return true;
}


// BFS type codes are four-character constants (see BFS_On-Disk_Format.md
// section 8), so render the readable form alongside the number.
bool FourCc(uint32_t type, std::string &out)
{
	uint8_t bytes[4] = {
		static_cast<uint8_t>(type >> 24),
		static_cast<uint8_t>(type >> 16),
		static_cast<uint8_t>(type >> 8),
		static_cast<uint8_t>(type),
	};
	if (!IsPrintable(bytes, sizeof(bytes))) {
		return false;
	}
	out.assign(reinterpret_cast<const char *>(bytes), sizeof(bytes));
	return true;
}


std::string ModeTypeName(uint32_t mode)
{
	switch (mode & kSIfmt) {
		case kSIfdir:
			return "directory";
		case kSIfreg:
			return "file";
		case kSIflnk:
			return "symlink";
		default:
			return "other";
	}
}


std::string PermissionString(uint32_t mode)
{
	static const char kBits[] = "rwx";
	std::string out(9, '-');
	for (int i = 0; i < 9; i++) {
		if ((mode & (1u << (8 - i))) != 0) {
			out[i] = kBits[i % 3];
		}
	}
	return out;
}


std::string IsoTime(int64_t seconds)
{
	time_t value = static_cast<time_t>(seconds);
	struct tm parts;
	if (::gmtime_r(&value, &parts) == nullptr) {
		return std::string();
	}
	char buffer[32];
	if (::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts) == 0) {
		return std::string();
	}
	return buffer;
}


std::vector<std::string> SplitPath(const std::string &path)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (start < path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string::npos) {
			end = path.size();
		}
		if (end > start) {
			parts.push_back(path.substr(start, end - start));
		}
		start = end + 1;
	}
	return parts;
}


} // unnamed namespace


Dumper::Dumper(BfsReader &reader, JsonWriter &json, const DumpOptions &options):
	fReader(reader),
	fJson(json),
	fOptions(options),
	fGeo(reader.GetGeometry()),
	fOrder(reader.Order())
{
}


bool Dumper::Wants(Section section) const
{
	for (const DumpRequest &request : fOptions.requests) {
		if (request.section == section) {
			return true;
		}
	}
	return false;
}


//#pragma mark - selectors


int64_t Dumper::ResolvePath(const std::string &path)
{
	int64_t block = fGeo.ToBlock(fReader.RootDirectory());
	for (const std::string &part : SplitPath(path)) {
		Inode directory = fReader.ReadInode(block);
		if (!directory.IsDirectory()) {
			throw std::runtime_error("'" + part + "' is not inside a directory");
		}
		bool found = false;
		for (const DirEntry &entry : fReader.ReadDirectory(directory)) {
			if (entry.name == part) {
				block = entry.inode;
				found = true;
				break;
			}
		}
		if (!found) {
			throw std::runtime_error("no such entry: '" + part + "'");
		}
	}
	return block;
}


int64_t Dumper::ResolveInode(const std::string &spec)
{
	if (spec == "root") {
		return fGeo.ToBlock(fReader.RootDirectory());
	}
	if (spec == "indices") {
		if (fReader.IndexDirectory().IsZero()) {
			throw std::runtime_error("volume has no index directory");
		}
		return fGeo.ToBlock(fReader.IndexDirectory());
	}
	if (!spec.empty() && spec[0] == '/') {
		return ResolvePath(spec);
	}

	// A block_run, written the way the format documents it: group:start:length.
	if (spec.find(':') != std::string::npos) {
		BlockRun run;
		if (::sscanf(spec.c_str(), "%" SCNd32 ":%" SCNu16 ":%" SCNu16,
				&run.group, &run.start, &run.length) < 2) {
			throw std::runtime_error("malformed block_run: '" + spec + "'");
		}
		return fGeo.ToBlock(run);
	}

	char *end = nullptr;
	long long value = ::strtoll(spec.c_str(), &end, 0);
	if (end == spec.c_str() || *end != '\0') {
		throw std::runtime_error("malformed inode selector: '" + spec + "'");
	}
	return value;
}


//#pragma mark - shared field writers


void Dumper::WriteBlockRun(const char *name, const BlockRun &run)
{
	fJson.Key(name);
	fJson.StartObject();
	fJson.MemberInt("group", run.group);
	fJson.MemberUint("start", run.start);
	fJson.MemberUint("length", run.length);
	if (!run.IsZero()) {
		int64_t block = fGeo.ToBlock(run);
		fJson.MemberInt("block", block);
		fJson.MemberBool("valid",
			block >= 0 && block + run.length <= fGeo.numBlocks);
	} else {
		fJson.MemberBool("zero", true);
	}
	fJson.EndObject();
}


void Dumper::WriteTime(const char *name, int64_t encoded)
{
	int64_t seconds = 0;
	uint32_t nanoseconds = 0;
	DecodeTime(encoded, seconds, nanoseconds);

	fJson.Key(name);
	fJson.StartObject();
	fJson.MemberInt("raw", encoded);
	fJson.MemberInt("seconds", seconds);
	fJson.MemberUint("nanoseconds", nanoseconds);
	std::string iso = IsoTime(seconds);
	if (!iso.empty()) {
		fJson.MemberString("iso", iso);
	}
	fJson.EndObject();
}


void Dumper::WriteMode(uint32_t mode)
{
	fJson.Key("mode");
	fJson.StartObject();
	fJson.MemberUint("raw", mode);

	char octal[32];
	::snprintf(octal, sizeof(octal), "0%o", mode);
	fJson.MemberString("octal", octal);
	fJson.MemberString("type", ModeTypeName(mode));
	fJson.MemberString("permissions", PermissionString(mode));

	fJson.Key("bits");
	fJson.StartArray();
	struct {
		uint32_t bit;
		const char *name;
	} kBits[] = {
		{kSAttrDir, "S_ATTR_DIR"},
		{kSAttr, "S_ATTR"},
		{kSIndexDir, "S_INDEX_DIR"},
		{kSStrIndex, "S_STR_INDEX"},
		{kSIntIndex, "S_INT_INDEX"},
		{kSUIntIndex, "S_UINT_INDEX"},
		{kSLongLongIndex, "S_LONG_LONG_INDEX"},
		{kSULongLongIndex, "S_ULONG_LONG_INDEX"},
		{kSFloatIndex, "S_FLOAT_INDEX"},
		{kSDoubleIndex, "S_DOUBLE_INDEX"},
		{kSAllowDups, "S_ALLOW_DUPS"},
	};
	for (const auto &entry : kBits) {
		if ((mode & entry.bit) != 0) {
			fJson.String(entry.name);
		}
	}
	fJson.EndArray();
	fJson.EndObject();
}


void Dumper::WriteFlags(uint32_t flags)
{
	fJson.Key("flags");
	fJson.StartObject();
	fJson.MemberUint("raw", flags);
	fJson.Key("names");
	fJson.StartArray();
	struct {
		uint32_t bit;
		const char *name;
	} kFlags[] = {
		{kInodeInUse, "INODE_IN_USE"},
		{kInodeLogged, "INODE_LOGGED"},
		{kInodeDeleted, "INODE_DELETED"},
		{kInodeNotReady, "INODE_NOT_READY"},
		{kInodeLongSymlink, "INODE_LONG_SYMLINK"},
	};
	for (const auto &entry : kFlags) {
		if ((flags & entry.bit) != 0) {
			fJson.String(entry.name);
		}
	}
	fJson.EndArray();
	fJson.EndObject();
}


void Dumper::WriteTypeCode(const char *name, uint32_t type)
{
	fJson.Key(name);
	fJson.StartObject();
	fJson.MemberUint("raw", type);
	fJson.MemberHex("hex", type, 8);
	std::string fourCc;
	if (FourCc(type, fourCc)) {
		fJson.MemberString("fourcc", fourCc);
	}
	fJson.EndObject();
}


void Dumper::WriteBlob(const std::vector<uint8_t> &data)
{
	size_t length = data.size();
	size_t shown = length;
	if (fOptions.maxData >= 0 && shown > static_cast<size_t>(fOptions.maxData)) {
		shown = static_cast<size_t>(fOptions.maxData);
	}

	fJson.MemberUint("size", length);
	fJson.MemberBytesHex("data_hex", data.data(), shown);
	if (IsPrintable(data.data(), shown)) {
		fJson.MemberString("data_string",
			std::string(reinterpret_cast<const char *>(data.data()), shown));
	}
	fJson.MemberBool("truncated", shown < length);
}


void Dumper::WriteKey(uint32_t keyType, const TreeKey &key)
{
	fJson.MemberUint("key_length", key.length);
	fJson.MemberBytesHex("key_hex", key.data, key.length);

	// Numeric keys are only meaningful at their natural width; anything else is
	// left as the hex above rather than guessed at.
	switch (keyType) {
		case kBPlusTreeInt32Type:
			if (key.length == 4) {
				fJson.MemberInt("key", GetS32(key.data, fOrder));
			}
			break;
		case kBPlusTreeUInt32Type:
			if (key.length == 4) {
				fJson.MemberUint("key", GetU32(key.data, fOrder));
			}
			break;
		case kBPlusTreeInt64Type:
			if (key.length == 8) {
				fJson.MemberInt("key", GetS64(key.data, fOrder));
			}
			break;
		case kBPlusTreeUInt64Type:
			if (key.length == 8) {
				fJson.MemberUint("key", GetU64(key.data, fOrder));
			}
			break;
		case kBPlusTreeFloatType:
			if (key.length == 4) {
				uint32_t bits = GetU32(key.data, fOrder);
				float value;
				::memcpy(&value, &bits, sizeof(value));
				fJson.MemberDouble("key", value);
			}
			break;
		case kBPlusTreeDoubleType:
			if (key.length == 8) {
				uint64_t bits = GetU64(key.data, fOrder);
				double value;
				::memcpy(&value, &bits, sizeof(value));
				fJson.MemberDouble("key", value);
			}
			break;
		default:
			if (IsPrintable(key.data, key.length)) {
				fJson.MemberString("key",
					std::string(reinterpret_cast<const char *>(key.data), key.length));
			}
			break;
	}
}


void Dumper::WriteAttributeList(const std::vector<Attribute> &attributes)
{
	fJson.StartArray();
	for (const Attribute &attribute : attributes) {
		fJson.StartObject();
		fJson.MemberString("name", attribute.name);
		WriteTypeCode("type", attribute.type);
		WriteBlob(attribute.data);
		fJson.EndObject();
	}
	fJson.EndArray();
}


//#pragma mark - sections


void Dumper::DumpSuperblock()
{
	fJson.StartObject();
	fJson.MemberString("name", fReader.VolumeName());

	// The volume would not have opened had any magic been wrong, so these are
	// reported as the constants they are required to be.
	fJson.MemberHex("magic1", kSuperBlockMagic1, 8);
	fJson.MemberHex("magic2", kSuperBlockMagic2, 8);
	fJson.MemberHex("magic3", kSuperBlockMagic3, 8);
	fJson.MemberHex("fs_byte_order", kSuperBlockFsLendian, 8);

	fJson.MemberUint("block_size", fGeo.blockSize);
	fJson.MemberUint("block_shift", fGeo.blockShift);
	fJson.MemberInt("num_blocks", fGeo.numBlocks);
	fJson.MemberInt("used_blocks", fReader.UsedBlocks());
	fJson.MemberUint("inode_size", fReader.InodeSize());
	fJson.MemberInt("blocks_per_ag", fGeo.blocksPerAg);
	fJson.MemberInt("ag_shift", fGeo.agShift);
	fJson.MemberInt("num_ags", fGeo.numAgs);

	fJson.Key("flags");
	fJson.StartObject();
	fJson.MemberHex("raw", fReader.Flags(), 8);
	const char *flagName = "unknown";
	if (fReader.Flags() == kSuperBlockDiskClean) {
		flagName = "clean";
	} else if (fReader.Flags() == kSuperBlockDiskDirty) {
		flagName = "dirty";
	}
	fJson.MemberString("name", flagName);
	fJson.EndObject();

	WriteBlockRun("log_blocks", fGeo.logBlocks);
	fJson.MemberInt("log_start", fReader.LogStart());
	fJson.MemberInt("log_end", fReader.LogEnd());
	fJson.MemberBool("clean", fReader.IsClean());

	WriteBlockRun("root_dir", fReader.RootDirectory());
	WriteBlockRun("indices", fReader.IndexDirectory());

	fJson.Key("derived");
	fJson.StartObject();
	fJson.MemberInt("bitmap_blocks", fGeo.bitmapBlocks);
	fJson.MemberInt("reserved_blocks", fReader.ReservedBlocks());
	fJson.MemberInt("blocks_per_group", fGeo.BlocksPerGroup());
	fJson.MemberUint("volume_bytes",
		static_cast<uint64_t>(fGeo.numBlocks) << fGeo.blockShift);
	fJson.MemberInt("free_blocks", fGeo.numBlocks - fReader.UsedBlocks());
	fJson.EndObject();

	fJson.EndObject();
}


void Dumper::DumpInodeBody(const Inode &inode)
{
	const uint8_t *raw = inode.raw.data();

	fJson.MemberInt("block", inode.block);
	WriteBlockRun("inode_num", GetBlockRun(raw + inode::kInodeNum, fOrder));
	fJson.MemberBool("magic_valid",
		GetU32(raw + inode::kMagic1, fOrder) == kInodeMagic1);
	fJson.MemberUint("uid", inode.uid);
	fJson.MemberUint("gid", inode.gid);
	WriteMode(inode.mode);
	WriteFlags(inode.flags);
	WriteTypeCode("type", inode.type);
	fJson.MemberUint("inode_size", GetU32(raw + inode::kInodeSize, fOrder));

	WriteTime("create_time", inode.createTime);
	WriteTime("last_modified_time", inode.modifiedTime);
	WriteTime("status_change_time", inode.statusChangeTime);

	WriteBlockRun("parent", inode.parent);
	WriteBlockRun("attributes", inode.attributes);

	fJson.Key("data_stream");
	fJson.StartObject();
	fJson.MemberInt("size", inode.stream.size);
	fJson.MemberBool("long_symlink", inode.IsLongSymlink());
	fJson.EndObject();

	if (inode.IsSymlink()) {
		fJson.MemberString("symlink", fReader.ReadSymlink(inode));
	}

	std::vector<Attribute> smallData;
	fReader.ReadSmallData(inode, smallData);
	fJson.Key("small_data");
	WriteAttributeList(smallData);

	if (fOptions.attributes) {
		fJson.Key("attributes_list");
		WriteAttributeList(fReader.ReadAttributes(inode));
	}
}


void Dumper::DumpInode(const std::string &spec)
{
	fJson.StartObject();
	fJson.MemberString("spec", spec);
	try {
		int64_t block = ResolveInode(spec);
		DumpInodeBody(fReader.ReadInode(block));
	} catch (const std::exception &error) {
		fJson.MemberString("error", error.what());
	}
	fJson.EndObject();
}


void Dumper::DumpStreamBody(const DataStreamInfo &stream)
{
	uint32_t shift = fGeo.blockShift;
	int64_t dataBlocks = 0;
	int64_t metaBlocks = 0;
	int64_t coveredBytes = 0;
	int64_t runCount = 0;
	int64_t previousEnd = -1;
	int64_t fragments = 0;

	auto writeRun = [&](const BlockRun &run) {
		int64_t block = fGeo.ToBlock(run);
		fJson.StartObject();
		fJson.MemberInt("group", run.group);
		fJson.MemberUint("start", run.start);
		fJson.MemberUint("length", run.length);
		fJson.MemberInt("block", block);
		fJson.MemberUint("bytes", static_cast<uint64_t>(run.length) << shift);
		fJson.MemberBool("valid", block >= 0 && block + run.length <= fGeo.numBlocks);
		fJson.EndObject();

		dataBlocks += run.length;
		coveredBytes += static_cast<int64_t>(run.length) << shift;
		runCount++;
		if (block != previousEnd) {
			fragments++;
		}
		previousEnd = block + run.length;
	};

	fJson.MemberInt("size", stream.size);
	fJson.MemberInt("max_direct_range", stream.maxDirectRange);
	fJson.MemberInt("max_indirect_range", stream.maxIndirectRange);
	fJson.MemberInt("max_double_indirect_range", stream.maxDoubleIndirectRange);

	fJson.Key("direct");
	fJson.StartArray();
	for (int i = 0; i < kNumDirectBlocks; i++) {
		if (stream.direct[i].IsZero()) {
			break;
		}
		writeRun(stream.direct[i]);
	}
	fJson.EndArray();

	fJson.Key("indirect");
	fJson.StartObject();
	WriteBlockRun("run", stream.indirect);
	fJson.Key("runs");
	fJson.StartArray();
	if (!stream.indirect.IsZero()) {
		metaBlocks += stream.indirect.length;
		try {
			std::vector<uint8_t> array(
				static_cast<size_t>(stream.indirect.length) * fGeo.blockSize);
			int64_t base = fGeo.ToBlock(stream.indirect);
			for (uint16_t i = 0; i < stream.indirect.length; i++) {
				fReader.ReadBlock(base + i, array.data() + i * fGeo.blockSize);
			}
			for (size_t j = 0; j + 8 <= array.size(); j += 8) {
				BlockRun run = GetBlockRun(array.data() + j, fOrder);
				if (run.IsZero()) {
					break;
				}
				writeRun(run);
			}
		} catch (const std::exception &) {
			// A damaged indirect array simply contributes no runs.
		}
	}
	fJson.EndArray();
	fJson.EndObject();

	fJson.Key("double_indirect");
	fJson.StartObject();
	WriteBlockRun("run", stream.doubleIndirect);
	fJson.Key("arrays");
	fJson.StartArray();
	if (!stream.doubleIndirect.IsZero()) {
		metaBlocks += stream.doubleIndirect.length;
		try {
			std::vector<uint8_t> top(
				static_cast<size_t>(stream.doubleIndirect.length) * fGeo.blockSize);
			int64_t base = fGeo.ToBlock(stream.doubleIndirect);
			for (uint16_t i = 0; i < stream.doubleIndirect.length; i++) {
				fReader.ReadBlock(base + i, top.data() + i * fGeo.blockSize);
			}
			for (size_t t = 0; t + 8 <= top.size(); t += 8) {
				BlockRun arrayRun = GetBlockRun(top.data() + t, fOrder);
				if (arrayRun.IsZero()) {
					break;
				}
				metaBlocks += arrayRun.length;

				fJson.StartObject();
				WriteBlockRun("run", arrayRun);
				fJson.Key("runs");
				fJson.StartArray();
				std::vector<uint8_t> level(
					static_cast<size_t>(arrayRun.length) * fGeo.blockSize);
				int64_t arrayBase = fGeo.ToBlock(arrayRun);
				for (uint16_t i = 0; i < arrayRun.length; i++) {
					fReader.ReadBlock(arrayBase + i, level.data() + i * fGeo.blockSize);
				}
				for (size_t j = 0; j + 8 <= level.size(); j += 8) {
					BlockRun run = GetBlockRun(level.data() + j, fOrder);
					if (run.IsZero()) {
						break;
					}
					writeRun(run);
				}
				fJson.EndArray();
				fJson.EndObject();
			}
		} catch (const std::exception &) {
		}
	}
	fJson.EndArray();
	fJson.EndObject();

	fJson.Key("totals");
	fJson.StartObject();
	fJson.MemberInt("data_blocks", dataBlocks);
	fJson.MemberInt("meta_blocks", metaBlocks);
	fJson.MemberInt("covered_bytes", coveredBytes);
	fJson.MemberInt("runs", runCount);
	fJson.MemberInt("fragments", fragments);
	fJson.EndObject();

	fJson.MemberBool("consistent", coveredBytes >= stream.size);
}


void Dumper::DumpDataStream(const std::string &spec)
{
	fJson.StartObject();
	fJson.MemberString("spec", spec);
	try {
		int64_t block = ResolveInode(spec);
		fJson.MemberInt("inode", block);
		DumpStreamBody(fReader.ReadInode(block).stream);
	} catch (const std::exception &error) {
		fJson.MemberString("error", error.what());
	}
	fJson.EndObject();
}


void Dumper::DumpDirectoryBody(int64_t block, int depth)
{
	Inode inode = fReader.ReadInode(block);
	fJson.MemberInt("inode", block);
	if (!inode.IsDirectory()) {
		fJson.MemberString("error", "not a directory");
		return;
	}

	std::vector<DirEntry> entries = fReader.ReadDirectory(inode);
	fJson.MemberInt("entry_count", static_cast<int64_t>(entries.size()));

	int64_t limit = static_cast<int64_t>(entries.size());
	if (fOptions.maxEntries >= 0 && limit > fOptions.maxEntries) {
		limit = fOptions.maxEntries;
	}
	fJson.MemberBool("truncated", limit < static_cast<int64_t>(entries.size()));

	fJson.Key("entries");
	fJson.StartArray();
	for (int64_t i = 0; i < limit; i++) {
		const DirEntry &entry = entries[i];
		fJson.StartObject();
		fJson.MemberString("name", entry.name);
		fJson.MemberInt("inode", entry.inode);
		try {
			Inode child = fReader.ReadInode(entry.inode);
			fJson.MemberString("type", ModeTypeName(child.mode));
			if (depth > 1 && child.IsDirectory()) {
				fJson.Key("children");
				fJson.StartObject();
				DumpDirectoryBody(entry.inode, depth - 1);
				fJson.EndObject();
			}
		} catch (const std::exception &error) {
			fJson.MemberString("error", error.what());
		}
		fJson.EndObject();
	}
	fJson.EndArray();
}


void Dumper::DumpDirectory(const std::string &spec)
{
	fJson.StartObject();
	fJson.MemberString("spec", spec);
	try {
		DumpDirectoryBody(ResolveInode(spec), fOptions.depth);
	} catch (const std::exception &error) {
		fJson.MemberString("error", error.what());
	}
	fJson.EndObject();
}


void Dumper::DumpIndices()
{
	fJson.StartObject();

	if (fReader.IndexDirectory().IsZero()) {
		fJson.MemberBool("present", false);
		fJson.EndObject();
		return;
	}
	fJson.MemberBool("present", true);
	WriteBlockRun("index_directory", fReader.IndexDirectory());

	// The full listing answers a bare --index. When particular indices were
	// named there is no reason to also enumerate the other forty.
	bool wantsList = false;
	for (const DumpRequest &request : fOptions.requests) {
		if (request.section == Section::Index && request.spec.empty()) {
			wantsList = true;
			break;
		}
	}

	if (wantsList) {
		fJson.Key("list");
		fJson.StartArray();
		try {
			int64_t block = fGeo.ToBlock(fReader.IndexDirectory());
			Inode directory = fReader.ReadInode(block);
			for (const DirEntry &entry : fReader.ReadDirectory(directory)) {
				fJson.StartObject();
				fJson.MemberString("name", entry.name);
				fJson.MemberInt("inode", entry.inode);
				try {
					Inode index = fReader.ReadInode(entry.inode);
					fJson.MemberString("key_type",
						KeyTypeName(KeyTypeFromMode(index.mode)));
					fJson.MemberBool("allows_duplicates",
						(index.mode & kSAllowDups) != 0);
					WriteMode(index.mode);
					fJson.MemberInt("tree_size", index.stream.size);
				} catch (const std::exception &error) {
					fJson.MemberString("error", error.what());
				}
				fJson.EndObject();
			}
		} catch (const std::exception &error) {
			fJson.MemberString("error", error.what());
		}
		fJson.EndArray();
	}

	// Individually requested indices are dumped as ordered key -> value maps.
	bool any = false;
	for (const DumpRequest &request : fOptions.requests) {
		if (request.section != Section::Index || request.spec.empty()) {
			continue;
		}
		if (!any) {
			fJson.Key("dumps");
			fJson.StartArray();
			any = true;
		}
		DumpIndexEntries(request.spec);
	}
	if (any) {
		fJson.EndArray();
	}

	fJson.EndObject();
}


void Dumper::DumpIndexEntries(const std::string &name)
{
	fJson.StartObject();
	fJson.MemberString("name", name);
	try {
		if (fReader.IndexDirectory().IsZero()) {
			throw std::runtime_error("volume has no index directory");
		}
		Inode directory = fReader.ReadInode(fGeo.ToBlock(fReader.IndexDirectory()));
		int64_t block = -1;
		for (const DirEntry &entry : fReader.ReadDirectory(directory)) {
			if (entry.name == name) {
				block = entry.inode;
				break;
			}
		}
		if (block < 0) {
			throw std::runtime_error("no such index");
		}

		Inode index = fReader.ReadInode(block);
		fJson.MemberInt("inode", block);
		DumpBTreeMapBody(index, fReader.ReadStreamFully(index.stream));
	} catch (const std::exception &error) {
		fJson.MemberString("error", error.what());
	}
	fJson.EndObject();
}


//#pragma mark - B+trees


void Dumper::DumpBTreeMapBody(const Inode &inode, const std::vector<uint8_t> &tree)
{
	uint32_t keyType = KeyTypeFromMode(inode.mode);
	BPlusTreeReader reader(tree, fOrder);

	fJson.MemberString("format", "map");
	fJson.MemberString("key_type", KeyTypeName(keyType));

	TreeStatus status = reader.Open();
	if (status != TreeStatus::Ok) {
		fJson.MemberString("status", TreeStatusName(status));
		return;
	}

	const TreeHeader &header = reader.Header();
	fJson.Key("header");
	fJson.StartObject();
	fJson.MemberBool("valid", true);
	fJson.MemberInt("node_size", header.nodeSize);
	fJson.MemberUint("max_number_of_levels", header.maxNumberOfLevels);
	fJson.MemberUint("data_type", header.dataType);
	fJson.MemberString("data_type_name", KeyTypeName(header.dataType));
	fJson.MemberBool("data_type_matches_mode", header.dataType == keyType);
	fJson.MemberInt("maximum_size", header.maximumSize);
	fJson.MemberBool("matches_stream_size", reader.MaxSizeMatchesStream());
	fJson.EndObject();

	// The entries are streamed straight out of the leaf walk, so a large index
	// costs no more memory than one node.
	int64_t emitted = 0;
	bool truncated = false;
	fJson.Key("entries");
	fJson.StartArray();
	TreeStatus walk = reader.ForEachLeafEntry([&](const TreeKey &key) {
		if (fOptions.maxEntries >= 0 && emitted >= fOptions.maxEntries) {
			truncated = true;
			return false;
		}
		fJson.StartObject();
		WriteKey(keyType, key);

		TreeValue value;
		TreeStatus resolved = reader.ResolveValue(key.value, value);
		fJson.MemberInt("value_raw", key.value);
		if (value.kind == ValueKind::DuplicateNode) {
			fJson.MemberString("duplicate_kind", "node");
			fJson.MemberInt("container_offset", value.containerOffset);
		} else if (value.kind == ValueKind::DuplicateFragment) {
			fJson.MemberString("duplicate_kind", "fragment");
			fJson.MemberInt("container_offset", value.containerOffset);
			fJson.MemberInt("fragment_index", value.fragmentIndex);
		}
		if (resolved != TreeStatus::Ok) {
			fJson.MemberString("value_status", TreeStatusName(resolved));
		}
		fJson.Key("values");
		fJson.StartArray();
		for (int64_t v : value.values) {
			fJson.Int(v);
		}
		fJson.EndArray();
		fJson.EndObject();

		emitted++;
		return true;
	});
	fJson.EndArray();

	fJson.MemberInt("entry_count", emitted);
	fJson.MemberBool("truncated", truncated);
	if (walk != TreeStatus::Ok) {
		fJson.MemberString("status", TreeStatusName(walk));
	}
}


void Dumper::DumpBTreeNodesBody(const Inode &inode, const std::vector<uint8_t> &tree)
{
	uint32_t keyType = KeyTypeFromMode(inode.mode);
	BPlusTreeReader reader(tree, fOrder);

	fJson.MemberString("format", "nodes");
	fJson.MemberString("key_type", KeyTypeName(keyType));

	TreeStatus status = reader.Open();
	if (status != TreeStatus::Ok) {
		fJson.MemberString("status", TreeStatusName(status));
		return;
	}

	const TreeHeader &header = reader.Header();
	fJson.Key("header");
	fJson.StartObject();
	fJson.MemberHex("magic", header.magic, 8);
	fJson.MemberBool("valid", true);
	fJson.MemberInt("node_size", header.nodeSize);
	fJson.MemberUint("max_number_of_levels", header.maxNumberOfLevels);
	fJson.MemberUint("data_type", header.dataType);
	fJson.MemberString("data_type_name", KeyTypeName(header.dataType));
	fJson.MemberBool("data_type_matches_mode", header.dataType == keyType);
	fJson.MemberInt("root_node_pointer", header.rootNodePointer);
	fJson.MemberBool("root_valid", reader.ValidLink(header.rootNodePointer));
	fJson.MemberInt("free_node_pointer", header.freeNodePointer);
	fJson.MemberInt("maximum_size", header.maximumSize);
	fJson.MemberBool("matches_stream_size", reader.MaxSizeMatchesStream());
	fJson.MemberBool("levels_valid", header.maxNumberOfLevels >= 1);
	fJson.EndObject();

	std::vector<int64_t> freeNodes;
	reader.ForEachFreeNode([&freeNodes](int64_t offset) {
		freeNodes.push_back(offset);
		return true;
	});

	int64_t nodeCount = 0;
	int64_t leaves = 0;
	int64_t keys = 0;
	int64_t leafKeys = 0;
	int64_t internalKeys = 0;
	int64_t levels = 0;
	int64_t usedTotal = 0;
	int64_t damaged = 0;
	bool truncated = false;
	std::vector<int64_t> accounted;

	fJson.Key("nodes");
	fJson.StartArray();
	TreeStatus walk = reader.ForEachNode([&](const TreeNode &node, int level,
		int64_t indexInLevel, TreeStatus nodeStatus) {
		accounted.push_back(node.offset);
		nodeCount++;
		levels = std::max<int64_t>(levels, level + 1);

		// A malformed node's counts are whatever the corrupt bytes happened to
		// say, so they are reported on the node but kept out of the totals.
		if (nodeStatus == TreeStatus::Ok) {
			keys += node.keyCount;
			usedTotal += node.used;
			if (node.IsLeaf()) {
				leaves++;
				leafKeys += node.keyCount;
			} else {
				internalKeys += node.keyCount;
			}
		} else {
			damaged++;
		}

		if (fOptions.maxNodes >= 0 && nodeCount > fOptions.maxNodes) {
			truncated = true;
			return false;
		}

		fJson.StartObject();
		fJson.MemberInt("offset", node.offset);
		if (nodeStatus != TreeStatus::Ok) {
			fJson.MemberString("status", TreeStatusName(nodeStatus));
		}
		fJson.MemberInt("level", level);
		fJson.MemberInt("index_in_level", indexInLevel);
		fJson.MemberBool("leaf", node.IsLeaf());
		fJson.MemberInt("left_link", node.leftLink);
		fJson.MemberInt("right_link", node.rightLink);
		fJson.MemberInt("overflow_link", node.overflowLink);
		fJson.MemberUint("all_key_count", node.keyCount);
		fJson.MemberUint("all_key_length", node.keyLength);
		fJson.MemberInt("used", node.used);
		fJson.MemberInt("capacity", header.nodeSize);
		if (header.nodeSize > 0) {
			fJson.MemberDouble("fill_percent",
				100.0 * static_cast<double>(node.used)
					/ static_cast<double>(header.nodeSize));
		}

		fJson.Key("keys");
		fJson.StartArray();
		int64_t keyOffset = btreenode::kSize;
		for (const TreeKey &key : node.keys) {
			fJson.StartObject();
			fJson.MemberInt("key_offset", keyOffset);
			WriteKey(keyType, key);
			fJson.MemberInt("value_raw", key.value);

			fJson.Key("value");
			fJson.StartObject();
			if (!node.IsLeaf()) {
				fJson.MemberString("kind", "child_node");
				fJson.MemberInt("offset", key.value);
				fJson.MemberBool("valid", reader.ValidLink(key.value));
			} else {
				TreeValue value;
				TreeStatus resolved = reader.ResolveValue(key.value, value);
				accounted.insert(accounted.end(), value.containers.begin(),
					value.containers.end());
				switch (value.kind) {
					case ValueKind::DuplicateNode:
						fJson.MemberString("kind", "duplicate_node");
						fJson.MemberInt("container_offset", value.containerOffset);
						break;
					case ValueKind::DuplicateFragment:
						fJson.MemberString("kind", "duplicate_fragment");
						fJson.MemberInt("container_offset", value.containerOffset);
						fJson.MemberInt("fragment_index", value.fragmentIndex);
						break;
					default:
						fJson.MemberString("kind", "inode");
						break;
				}
				fJson.MemberBool("chain_ok", value.chainOk);
				if (resolved != TreeStatus::Ok) {
					fJson.MemberString("status", TreeStatusName(resolved));
				}
				fJson.Key("values");
				fJson.StartArray();
				for (int64_t v : value.values) {
					fJson.Int(v);
				}
				fJson.EndArray();
			}
			fJson.EndObject();
			fJson.EndObject();

			keyOffset += key.length;
		}
		fJson.EndArray();
		fJson.EndObject();
		return true;
	});
	fJson.EndArray();

	// Node-sized slots that nothing accounts for: not a node of the tree, not a
	// duplicate container hanging off a leaf value, not on the free list. On a
	// healthy tree this is empty; on a damaged one it is where the orphans are.
	// A truncated walk has not looked at every node, so it cannot conclude
	// anything about what is left over.
	accounted.insert(accounted.end(), freeNodes.begin(), freeNodes.end());
	std::sort(accounted.begin(), accounted.end());

	fJson.Key("unreachable_nodes");
	fJson.StartArray();
	int64_t unreachable = 0;
	if (!truncated) {
		for (int64_t slot = 1; slot < reader.NodeCount(); slot++) {
			int64_t offset = slot * header.nodeSize;
			if (!std::binary_search(accounted.begin(), accounted.end(), offset)) {
				fJson.Int(offset);
				unreachable++;
			}
		}
	}
	fJson.EndArray();

	fJson.Key("free_node_chain");
	fJson.StartArray();
	for (int64_t offset : freeNodes) {
		fJson.Int(offset);
	}
	fJson.EndArray();

	fJson.Key("stats");
	fJson.StartObject();
	fJson.MemberInt("nodes", nodeCount);
	fJson.MemberInt("levels", levels);
	fJson.MemberInt("leaves", leaves);
	fJson.MemberInt("keys", keys);
	// Only the leaf keys are the tree's content; the internal ones are
	// separators. 'leaf_keys' is what the map format of this same tree reports.
	fJson.MemberInt("leaf_keys", leafKeys);
	fJson.MemberInt("internal_keys", internalKeys);
	fJson.MemberInt("free_nodes", static_cast<int64_t>(freeNodes.size()));
	fJson.MemberInt("damaged_nodes", damaged);
	fJson.MemberInt("unreachable_nodes", unreachable);
	fJson.MemberInt("slots", reader.NodeCount());
	if (nodeCount > 0 && header.nodeSize > 0) {
		fJson.MemberDouble("average_fill_percent",
			100.0 * static_cast<double>(usedTotal)
				/ (static_cast<double>(nodeCount) * static_cast<double>(header.nodeSize)));
	}
	fJson.EndObject();

	fJson.MemberBool("truncated", truncated);
	if (walk != TreeStatus::Ok) {
		fJson.MemberString("status", TreeStatusName(walk));
	}
}


void Dumper::DumpBTree(const std::string &spec, bool nodes)
{
	fJson.StartObject();
	fJson.MemberString("spec", spec);
	try {
		int64_t block = ResolveInode(spec);
		fJson.MemberInt("inode", block);
		Inode inode = fReader.ReadInode(block);
		std::vector<uint8_t> tree = fReader.ReadStreamFully(inode.stream);
		if (nodes) {
			DumpBTreeNodesBody(inode, tree);
		} else {
			DumpBTreeMapBody(inode, tree);
		}
	} catch (const std::exception &error) {
		fJson.MemberString("error", error.what());
	}
	fJson.EndObject();
}


//#pragma mark - top level


void Dumper::Dump()
{
	fJson.StartObject();
	fJson.MemberString("byte_order", fOrder == ByteOrder::Big ? "big" : "little");
	fJson.MemberUint("block_size", fGeo.blockSize);

	if (Wants(Section::Superblock)) {
		fJson.Key("superblock");
		DumpSuperblock();
	}

	if (Wants(Section::Inode)) {
		fJson.Key("inodes");
		fJson.StartArray();
		for (const DumpRequest &request : fOptions.requests) {
			if (request.section == Section::Inode) {
				DumpInode(request.spec);
			}
		}
		fJson.EndArray();
	}

	if (Wants(Section::DataStream)) {
		fJson.Key("data_streams");
		fJson.StartArray();
		for (const DumpRequest &request : fOptions.requests) {
			if (request.section == Section::DataStream) {
				DumpDataStream(request.spec);
			}
		}
		fJson.EndArray();
	}

	if (Wants(Section::Directory)) {
		fJson.Key("directories");
		fJson.StartArray();
		for (const DumpRequest &request : fOptions.requests) {
			if (request.section == Section::Directory) {
				DumpDirectory(request.spec);
			}
		}
		fJson.EndArray();
	}

	if (Wants(Section::Index)) {
		fJson.Key("indices");
		DumpIndices();
	}

	if (Wants(Section::BTreeMap) || Wants(Section::BTreeNodes)) {
		fJson.Key("btrees");
		fJson.StartArray();
		for (const DumpRequest &request : fOptions.requests) {
			if (request.section == Section::BTreeMap) {
				DumpBTree(request.spec, false);
			} else if (request.section == Section::BTreeNodes) {
				DumpBTree(request.spec, true);
			}
		}
		fJson.EndArray();
	}

	fJson.EndObject();
	fJson.Flush();
}


} // bfs
