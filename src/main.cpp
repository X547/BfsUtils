#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "BfsBuilder.h"
#include "Node.h"
#include "SourceScanner.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <source-directory> <output-image>\n"
		"\n"
		"Generate a fresh BFS image containing the given directory tree.\n"
		"\n"
		"Options:\n"
		"  -b, --block-size N   block size: 1024, 2048, 4096, or 8192 (default 2048)\n"
		"  -s, --size BYTES     total image size; accepts K/M/G suffixes\n"
		"                       (default: smallest image that fits the content)\n"
		"  -n, --name NAME      volume name (default: source directory name)\n"
		"      --endian ORDER   on-disk byte order: little (default) or big\n"
		"      --no-index       do not generate the standard BFS indices\n"
		"      --no-attributes  do not archive BFS attributes (Haiku only)\n"
		"  -h, --help           show this help\n",
		program);
}


bool ParseSize(const std::string &text, uint64_t &out)
{
	if (text.empty()) {
		return false;
	}

	uint64_t multiplier = 1;
	std::string digits = text;
	char suffix = text.back();
	if (suffix == 'K' || suffix == 'k') {
		multiplier = 1024;
		digits.pop_back();
	} else if (suffix == 'M' || suffix == 'm') {
		multiplier = 1024ull * 1024;
		digits.pop_back();
	} else if (suffix == 'G' || suffix == 'g') {
		multiplier = 1024ull * 1024 * 1024;
		digits.pop_back();
	}

	if (digits.empty()) {
		return false;
	}

	uint64_t value = 0;
	for (char c : digits) {
		if (c < '0' || c > '9') {
			return false;
		}
		value = value * 10 + static_cast<uint64_t>(c - '0');
	}
	out = value * multiplier;
	return true;
}


std::string BaseName(const std::string &path)
{
	std::string trimmed = path;
	while (trimmed.size() > 1
		&& (trimmed.back() == '/' || trimmed.back() == '\\')) {
		trimmed.pop_back();
	}
	size_t slash = trimmed.find_last_of("/\\");
	std::string base = slash == std::string::npos
		? trimmed : trimmed.substr(slash + 1);
	return base.empty() ? std::string("bfs") : base;
}


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::BuildOptions options;
	options.readAttributes = true;   // no-op off Haiku

	std::string source;
	std::string output;
	bool haveName = false;

	int i = 1;
	for (; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "--no-index") {
			options.generateIndices = false;
		} else if (arg == "--no-attributes") {
			options.readAttributes = false;
		} else if (arg == "-b" || arg == "--block-size") {
			if (++i >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg.c_str());
				return 1;
			}
			options.blockSize = static_cast<uint32_t>(strtoul(argv[i], nullptr, 10));
			if (options.blockSize != 1024 && options.blockSize != 2048
				&& options.blockSize != 4096 && options.blockSize != 8192) {
				fprintf(stderr, "%s: block size must be 1024, 2048, 4096, or 8192\n",
					argv[0]);
				return 1;
			}
		} else if (arg == "-s" || arg == "--size") {
			if (++i >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg.c_str());
				return 1;
			}
			if (!ParseSize(argv[i], options.sizeOverride)) {
				fprintf(stderr, "%s: invalid size '%s'\n", argv[0], argv[i]);
				return 1;
			}
		} else if (arg == "-n" || arg == "--name") {
			if (++i >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg.c_str());
				return 1;
			}
			options.volumeName = argv[i];
			haveName = true;
		} else if (arg == "--endian") {
			if (++i >= argc) {
				fprintf(stderr, "%s: missing value for %s\n", argv[0], arg.c_str());
				return 1;
			}
			std::string order = argv[i];
			if (order == "little" || order == "le") {
				options.byteOrder = bfs::ByteOrder::Little;
			} else if (order == "big" || order == "be") {
				options.byteOrder = bfs::ByteOrder::Big;
			} else {
				fprintf(stderr, "%s: --endian must be 'little' or 'big'\n", argv[0]);
				return 1;
			}
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 1;
		} else if (source.empty()) {
			source = arg;
		} else if (output.empty()) {
			output = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 1;
		}
	}

	if (source.empty() || output.empty()) {
		PrintUsage(argv[0]);
		return 1;
	}

	if (!haveName) {
		options.volumeName = BaseName(source);
	}

	try {
		std::unique_ptr<bfs::Node> root = bfs::ScanSource(source, options.readAttributes);
		bfs::BfsBuilder builder(options);
		builder.Build(*root, output);
	} catch (const std::exception &error) {
		fprintf(stderr, "makebfs: %s\n", error.what());
		return 1;
	}

	printf("Wrote BFS image '%s' from '%s'.\n", output.c_str(), source.c_str());
	return 0;
}
