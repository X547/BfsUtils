#pragma once

#include <stdint.h>


namespace bfs {


// BFS volumes are little-endian on disk. These helpers store host integers as
// little-endian byte sequences regardless of the host byte order, so the tool
// produces identical images on any platform.

inline void PutU8(uint8_t *p, uint8_t v)
{
	p[0] = v;
}


inline void PutU16(uint8_t *p, uint16_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
}


inline void PutU32(uint8_t *p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v);
	p[1] = static_cast<uint8_t>(v >> 8);
	p[2] = static_cast<uint8_t>(v >> 16);
	p[3] = static_cast<uint8_t>(v >> 24);
}


inline void PutU64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		p[i] = static_cast<uint8_t>(v >> (8 * i));
	}
}


inline void PutS32(uint8_t *p, int32_t v)
{
	PutU32(p, static_cast<uint32_t>(v));
}


inline void PutS64(uint8_t *p, int64_t v)
{
	PutU64(p, static_cast<uint64_t>(v));
}


inline uint16_t GetU16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}


inline uint32_t GetU32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
		| (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}


inline int32_t GetS32(const uint8_t *p)
{
	return static_cast<int32_t>(GetU32(p));
}


inline uint64_t GetU64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) {
		v |= static_cast<uint64_t>(p[i]) << (8 * i);
	}
	return v;
}


inline int64_t GetS64(const uint8_t *p)
{
	return static_cast<int64_t>(GetU64(p));
}


} // bfs
