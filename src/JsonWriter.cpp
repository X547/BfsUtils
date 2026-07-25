#include "JsonWriter.h"

#include <stdio.h>


namespace bfs {


namespace {


constexpr int kIndentWidth = 2;


} // unnamed namespace


JsonWriter::JsonWriter(std::ostream &stream, bool pretty):
	fStream(stream),
	fWriter(pretty
		? std::variant<CompactWriter, IndentedWriter>(std::in_place_type<IndentedWriter>,
			fStream)
		: std::variant<CompactWriter, IndentedWriter>(std::in_place_type<CompactWriter>,
			fStream))
{
	if (IndentedWriter *writer = std::get_if<IndentedWriter>(&fWriter);
		writer != nullptr) {
		writer->SetIndent(' ', kIndentWidth);
	}
}


void JsonWriter::StartObject()
{
	std::visit([](auto &writer) {writer.StartObject();}, fWriter);
}


void JsonWriter::EndObject()
{
	std::visit([](auto &writer) {writer.EndObject();}, fWriter);
}


void JsonWriter::StartArray()
{
	std::visit([](auto &writer) {writer.StartArray();}, fWriter);
}


void JsonWriter::EndArray()
{
	std::visit([](auto &writer) {writer.EndArray();}, fWriter);
}


void JsonWriter::Key(const char *name)
{
	std::visit([name](auto &writer) {writer.Key(name);}, fWriter);
}


void JsonWriter::String(const std::string &value)
{
	std::visit([&value](auto &writer) {
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	}, fWriter);
}


void JsonWriter::Int(int64_t value)
{
	std::visit([value](auto &writer) {writer.Int64(value);}, fWriter);
}


void JsonWriter::Uint(uint64_t value)
{
	std::visit([value](auto &writer) {writer.Uint64(value);}, fWriter);
}


void JsonWriter::Double(double value)
{
	std::visit([value](auto &writer) {writer.Double(value);}, fWriter);
}


void JsonWriter::Bool(bool value)
{
	std::visit([value](auto &writer) {writer.Bool(value);}, fWriter);
}


void JsonWriter::Null()
{
	std::visit([](auto &writer) {writer.Null();}, fWriter);
}


void JsonWriter::MemberString(const char *name, const std::string &value)
{
	Key(name);
	String(value);
}


void JsonWriter::MemberInt(const char *name, int64_t value)
{
	Key(name);
	Int(value);
}


void JsonWriter::MemberUint(const char *name, uint64_t value)
{
	Key(name);
	Uint(value);
}


void JsonWriter::MemberDouble(const char *name, double value)
{
	Key(name);
	Double(value);
}


void JsonWriter::MemberBool(const char *name, bool value)
{
	Key(name);
	Bool(value);
}


void JsonWriter::MemberNull(const char *name)
{
	Key(name);
	Null();
}


void JsonWriter::MemberHex(const char *name, uint64_t value, int digits)
{
	char buffer[32];
	::snprintf(buffer, sizeof(buffer), "0x%0*llx", digits,
		static_cast<unsigned long long>(value));
	MemberString(name, buffer);
}


void JsonWriter::MemberBytesHex(const char *name, const uint8_t *data, size_t length)
{
	static const char kDigits[] = "0123456789abcdef";
	std::string hex;
	hex.resize(length * 2);
	for (size_t i = 0; i < length; i++) {
		hex[i * 2 + 0] = kDigits[data[i] >> 4];
		hex[i * 2 + 1] = kDigits[data[i] & 0x0f];
	}
	MemberString(name, hex);
}


void JsonWriter::Flush()
{
	// RapidJSON 1.1.0's writers have no Flush() of their own; they emit through
	// the stream wrapper as they go, so flushing that is what matters.
	fStream.Flush();
}


} // bfs
