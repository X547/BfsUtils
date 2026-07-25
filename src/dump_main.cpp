#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "BfsReader.h"
#include "Dumper.h"
#include "ImageFile.h"
#include "JsonWriter.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <image>\n"
		"\n"
		"Inspect a BFS image and write its structures as JSON (read-only).\n"
		"With no section selected, the superblock is dumped.\n"
		"\n"
		"A SPEC selects an inode, and is one of: an absolute path in the volume\n"
		"('/boot/beos'), a block number ('1234' or '0x4d2'), a block_run\n"
		"('group:start:length'), or the names 'root' and 'indices'.\n"
		"\n"
		"Sections (repeatable):\n"
		"      --superblock         volume header, geometry, and log state\n"
		"      --inode SPEC         one inode's fields\n"
		"      --data-stream SPEC   that inode's data_stream, tier by tier\n"
		"      --directory SPEC     directory entries\n"
		"      --index              list the volume's indices\n"
		"      --index=NAME         dump one index as a key -> inodes map\n"
		"      --btree SPEC         B+tree as a sorted key -> value map\n"
		"      --btree-nodes SPEC   B+tree node by node, with links and stats\n"
		"\n"
		"Options:\n"
		"      --depth N            recursion depth for --directory (default 1)\n"
		"      --attributes         include attributes in --inode output\n"
		"      --max-entries N      cap entries per directory or tree\n"
		"      --max-nodes N        cap nodes per --btree-nodes dump\n"
		"      --max-data N         cap dumped blob bytes (default 256)\n"
		"      --replay-log         dump the post-replay state of an unclean volume\n"
		"      --compact            single-line JSON (default: indented)\n"
		"  -o, --output FILE        write to FILE instead of stdout\n"
		"  -h, --help               show this help\n"
		"\n"
		"\n"
		"Every option also accepts the '--option=value' form.\n"
		"\n"
		"Exit status: 0 = ok, 2 = could not open/parse/write.\n",
		program);
}


// An option's value, taken from '--option=value' when it was written that way
// and from the next argument otherwise.
class ValueTaker {
public:
	ValueTaker(int argc, char **argv, int &index, const std::string &inlineValue,
		bool hasInlineValue):
		fArgc(argc),
		fArgv(argv),
		fIndex(index),
		fInline(inlineValue),
		fHasInline(hasInlineValue)
	{
	}

	bool Take(const char *option, std::string &out) const
	{
		if (fHasInline) {
			out = fInline;
			return true;
		}
		if (fIndex + 1 >= fArgc) {
			fprintf(stderr, "%s: %s needs a value\n", fArgv[0], option);
			return false;
		}
		out = fArgv[++fIndex];
		return true;
	}

private:
	int fArgc;
	char **fArgv;
	int &fIndex;
	std::string fInline;
	bool fHasInline;
};


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::DumpOptions options;
	bool replayLog = false;
	bool pretty = true;
	std::string imagePath;
	std::string outputPath;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		std::string value;

		// Split '--option=value' so an option whose value is optional cannot
		// swallow the image path.
		std::string inlineValue;
		bool hasInlineValue = false;
		if (arg.compare(0, 2, "--") == 0) {
			size_t equals = arg.find('=');
			if (equals != std::string::npos) {
				inlineValue = arg.substr(equals + 1);
				hasInlineValue = true;
				arg = arg.substr(0, equals);
			}
		}
		const ValueTaker taker(argc, argv, i, inlineValue, hasInlineValue);

		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "--superblock") {
			options.requests.push_back({bfs::Section::Superblock, ""});
		} else if (arg == "--inode") {
			if (!taker.Take("--inode", value)) {
				return 2;
			}
			options.requests.push_back({bfs::Section::Inode, value});
		} else if (arg == "--data-stream") {
			if (!taker.Take("--data-stream", value)) {
				return 2;
			}
			options.requests.push_back({bfs::Section::DataStream, value});
		} else if (arg == "--directory") {
			if (!taker.Take("--directory", value)) {
				return 2;
			}
			options.requests.push_back({bfs::Section::Directory, value});
		} else if (arg == "--index") {
			// Bare '--index' lists every index; a name is given as
			// '--index=NAME', so the image path can never be mistaken for one.
			options.requests.push_back({bfs::Section::Index, inlineValue});
		} else if (arg == "--btree") {
			if (!taker.Take("--btree", value)) {
				return 2;
			}
			options.requests.push_back({bfs::Section::BTreeMap, value});
		} else if (arg == "--btree-nodes") {
			if (!taker.Take("--btree-nodes", value)) {
				return 2;
			}
			options.requests.push_back({bfs::Section::BTreeNodes, value});
		} else if (arg == "--depth") {
			if (!taker.Take("--depth", value)) {
				return 2;
			}
			options.depth = atoi(value.c_str());
		} else if (arg == "--attributes") {
			options.attributes = true;
		} else if (arg == "--max-entries") {
			if (!taker.Take("--max-entries", value)) {
				return 2;
			}
			options.maxEntries = atoll(value.c_str());
		} else if (arg == "--max-nodes") {
			if (!taker.Take("--max-nodes", value)) {
				return 2;
			}
			options.maxNodes = atoll(value.c_str());
		} else if (arg == "--max-data") {
			if (!taker.Take("--max-data", value)) {
				return 2;
			}
			options.maxData = atoll(value.c_str());
		} else if (arg == "--replay-log") {
			replayLog = true;
		} else if (arg == "--compact") {
			pretty = false;
		} else if (arg == "-o" || arg == "--output") {
			if (!taker.Take("--output", outputPath)) {
				return 2;
			}
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 2;
		} else if (imagePath.empty()) {
			imagePath = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 2;
		}
	}

	if (imagePath.empty()) {
		PrintUsage(argv[0]);
		return 2;
	}
	if (options.requests.empty()) {
		options.requests.push_back({bfs::Section::Superblock, ""});
	}

	try {
		bfs::ImageFile image(imagePath);
		bfs::BfsReader reader(image);

		if (replayLog && !reader.IsClean()) {
			reader.ReplayLog();
		}

		std::ofstream file;
		if (!outputPath.empty()) {
			file.open(outputPath, std::ios::out | std::ios::trunc);
			if (!file) {
				throw std::runtime_error("cannot open " + outputPath + " for writing");
			}
		}
		std::ostream &stream = outputPath.empty()
			? std::cout
			: static_cast<std::ostream &>(file);

		bfs::JsonWriter json(stream, pretty);
		bfs::Dumper dumper(reader, json, options);
		dumper.Dump();
		stream << '\n';

		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "bfsdump: %s\n", error.what());
		return 2;
	}
}
