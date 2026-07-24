#pragma once

#include <stdint.h>


namespace bfs {


// The byte order a BFS volume was created with, recorded in the superblock's
// fs_byte_order marker (see BFS_On-Disk_Format.md, "Byte order"). Volumes are
// almost always little-endian; big-endian volumes (e.g. BeOS/PPC) are supported
// read-only. Every multi-byte field -- including the 32-bit magics -- is stored
// in this order.
enum class ByteOrder {
	Little,
	Big,
};


// Both the Get* and Put* helpers take the volume's ByteOrder and serialize
// independently of the host byte order, so the tools read and write identical
// images on any platform. The order defaults to little-endian (the usual BFS
// layout); pass ByteOrder::Big for big-endian volumes.

inline void PutU8(uint8_t *p, uint8_t v)
{
	p[0] = v;
}


inline void PutU16(uint8_t *p, uint16_t v, ByteOrder order = ByteOrder::Little)
{
	if (order == ByteOrder::Big) {
		p[0] = static_cast<uint8_t>(v >> 8);
		p[1] = static_cast<uint8_t>(v);
	} else {
		p[0] = static_cast<uint8_t>(v);
		p[1] = static_cast<uint8_t>(v >> 8);
	}
}


inline void PutU32(uint8_t *p, uint32_t v, ByteOrder order = ByteOrder::Little)
{
	if (order == ByteOrder::Big) {
		p[0] = static_cast<uint8_t>(v >> 24);
		p[1] = static_cast<uint8_t>(v >> 16);
		p[2] = static_cast<uint8_t>(v >> 8);
		p[3] = static_cast<uint8_t>(v);
	} else {
		p[0] = static_cast<uint8_t>(v);
		p[1] = static_cast<uint8_t>(v >> 8);
		p[2] = static_cast<uint8_t>(v >> 16);
		p[3] = static_cast<uint8_t>(v >> 24);
	}
}


inline void PutU64(uint8_t *p, uint64_t v, ByteOrder order = ByteOrder::Little)
{
	if (order == ByteOrder::Big) {
		for (int i = 0; i < 8; i++) {
			p[i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
		}
	} else {
		for (int i = 0; i < 8; i++) {
			p[i] = static_cast<uint8_t>(v >> (8 * i));
		}
	}
}


inline void PutS32(uint8_t *p, int32_t v, ByteOrder order = ByteOrder::Little)
{
	PutU32(p, static_cast<uint32_t>(v), order);
}


inline void PutS64(uint8_t *p, int64_t v, ByteOrder order = ByteOrder::Little)
{
	PutU64(p, static_cast<uint64_t>(v), order);
}


inline uint16_t GetU16(const uint8_t *p, ByteOrder order = ByteOrder::Little)
{
	if (order == ByteOrder::Big) {
		return static_cast<uint16_t>((p[0] << 8) | p[1]);
	}
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}


inline uint32_t GetU32(const uint8_t *p, ByteOrder order = ByteOrder::Little)
{
	if (order == ByteOrder::Big) {
		return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
			| (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
	}
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
		| (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}


inline int32_t GetS32(const uint8_t *p, ByteOrder order = ByteOrder::Little)
{
	return static_cast<int32_t>(GetU32(p, order));
}


inline uint64_t GetU64(const uint8_t *p, ByteOrder order = ByteOrder::Little)
{
	uint64_t v = 0;
	if (order == ByteOrder::Big) {
		for (int i = 0; i < 8; i++) {
			v |= static_cast<uint64_t>(p[i]) << (8 * (7 - i));
		}
	} else {
		for (int i = 0; i < 8; i++) {
			v |= static_cast<uint64_t>(p[i]) << (8 * i);
		}
	}
	return v;
}


inline int64_t GetS64(const uint8_t *p, ByteOrder order = ByteOrder::Little)
{
	return static_cast<int64_t>(GetU64(p, order));
}


} // bfs
