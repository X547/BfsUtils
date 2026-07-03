#pragma once

#include <stdint.h>

#include <vector>


namespace bfs {


// A compact fixed-size bit array, used for the "referenced blocks" set the
// integrity checker builds while walking the volume.
class BitArray {
public:
	explicit BitArray(int64_t bits):
		fBits(bits),
		fWords(static_cast<size_t>((bits + 7) / 8), 0)
	{
	}

	int64_t Bits() const {return fBits;}

	bool Test(int64_t index) const
	{
		return (fWords[static_cast<size_t>(index >> 3)] >> (index & 7)) & 1;
	}

	void Set(int64_t index)
	{
		fWords[static_cast<size_t>(index >> 3)] |=
			static_cast<uint8_t>(1u << (index & 7));
	}

	// Set the bit and return whether it was already set (collision detection).
	bool SetAndTest(int64_t index)
	{
		uint8_t &byte = fWords[static_cast<size_t>(index >> 3)];
		uint8_t mask = static_cast<uint8_t>(1u << (index & 7));
		bool wasSet = (byte & mask) != 0;
		byte |= mask;
		return wasSet;
	}

	int64_t CountSet() const
	{
		int64_t count = 0;
		for (uint8_t byte : fWords) {
			count += __builtin_popcount(byte);
		}
		return count;
	}

private:
	int64_t fBits;
	std::vector<uint8_t> fWords;
};


} // bfs
