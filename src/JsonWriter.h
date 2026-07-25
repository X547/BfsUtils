#pragma once

#include <stdint.h>

#include <ostream>
#include <string>
#include <variant>
#include <vector>

#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>


namespace bfs {


// A streaming JSON writer over RapidJSON's SAX interface -- values are emitted
// as they are produced, so nothing builds a document tree in memory and a dump
// of a large volume costs no more than the piece being written.
//
// Compact and pretty output come from two unrelated RapidJSON types, so they
// are held in a variant and dispatched to; that keeps the choice a runtime flag
// without templating every caller on the writer type.
class JsonWriter {
public:
	JsonWriter(std::ostream &stream, bool pretty);

	void StartObject();
	void EndObject();
	void StartArray();
	void EndArray();

	void Key(const char *name);
	void String(const std::string &value);
	void Int(int64_t value);
	void Uint(uint64_t value);
	void Double(double value);
	void Bool(bool value);
	void Null();

	// Named members. The value type is spelled out in the name rather than
	// overloaded, because an integer literal would otherwise be ambiguous
	// between the integral, floating-point, and boolean forms.
	void MemberString(const char *name, const std::string &value);
	void MemberInt(const char *name, int64_t value);
	void MemberUint(const char *name, uint64_t value);
	void MemberDouble(const char *name, double value);
	void MemberBool(const char *name, bool value);
	void MemberNull(const char *name);

	// A 0x-prefixed hex string, zero-padded to 'digits'. Used for magics and
	// flag words, where the hex form is how the format documents them.
	void MemberHex(const char *name, uint64_t value, int digits);

	// Lowercase hex of a byte range, as one string.
	void MemberBytesHex(const char *name, const uint8_t *data, size_t length);

	void Flush();

private:
	using CompactWriter = rapidjson::Writer<rapidjson::OStreamWrapper>;
	using IndentedWriter = rapidjson::PrettyWriter<rapidjson::OStreamWrapper>;

	rapidjson::OStreamWrapper fStream;
	std::variant<CompactWriter, IndentedWriter> fWriter;
};


} // bfs
