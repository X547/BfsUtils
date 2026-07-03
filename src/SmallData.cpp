#include "SmallData.h"

#include <string.h>

#include "BfsFormat.h"
#include "Endian.h"


namespace bfs {


namespace {


// Total on-disk size of a small_data record (see BFS_On-Disk_Format.md section
// 8): 8-byte header + name + 3 pad + data + 1 pad.
size_t RecordSize(size_t nameSize, size_t dataSize)
{
	return 8 + nameSize + 3 + dataSize + 1;
}


void AppendRecord(std::vector<uint8_t> &out, uint32_t type,
	const uint8_t *name, size_t nameSize,
	const uint8_t *data, size_t dataSize)
{
	size_t start = out.size();
	out.resize(start + RecordSize(nameSize, dataSize), 0);
	uint8_t *p = &out[start];

	PutU32(p + 0, type);
	PutU16(p + 4, static_cast<uint16_t>(nameSize));
	PutU16(p + 6, static_cast<uint16_t>(dataSize));
	if (nameSize > 0) {
		::memcpy(p + 8, name, nameSize);
	}
	// name NUL + padding occupy the 3 bytes at p + 8 + nameSize (already zero).
	if (dataSize > 0) {
		::memcpy(p + 8 + nameSize + 3, data, dataSize);
	}
	// trailing data NUL is the final zero byte (already zero).
}


} // unnamed namespace


SmallDataResult BuildSmallData(const std::string &name,
	const std::vector<Attribute> &attributes, size_t smallDataCapacity)
{
	SmallDataResult result;

	// The name record: type 'CSTR', a one-byte 0x13 name tag, and the file name
	// as the record data.
	uint8_t nameTag = kFileNameName;
	AppendRecord(result.bytes, kFileNameType,
		&nameTag, kFileNameNameLength,
		reinterpret_cast<const uint8_t *>(name.data()), name.size());

	for (const Attribute &attribute : attributes) {
		bool fits = attribute.name.size() <= 0xffff
			&& attribute.data.size() <= 0xffff;
		size_t needed = RecordSize(attribute.name.size(), attribute.data.size());
		if (fits && result.bytes.size() + needed <= smallDataCapacity) {
			AppendRecord(result.bytes, attribute.type,
				reinterpret_cast<const uint8_t *>(attribute.name.data()),
				attribute.name.size(),
				attribute.data.data(), attribute.data.size());
		} else {
			result.largeAttributes.push_back(&attribute);
		}
	}

	return result;
}


} // bfs
