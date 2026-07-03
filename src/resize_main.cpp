#include <stdint.h>
#include <stdio.h>

#include <stdexcept>
#include <string>

#include "Resizer.h"


namespace {


void PrintUsage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options] <image> <new-size>\n"
		"\n"
		"Resize a BFS image in place (grow or shrink). Crash-safe via a sidecar\n"
		"redo-journal: an interrupted resize leaves a mountable volume and can be\n"
		"re-run to completion.\n"
		"\n"
		"<new-size> is in bytes and accepts K/M/G suffixes (rounded down to a\n"
		"block multiple).\n"
		"\n"
		"Options:\n"
		"  -n, --dry-run   report the plan without modifying the image\n"
		"  -v, --verbose   verbose output\n"
		"  -h, --help      show this help\n",
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


} // unnamed namespace


int main(int argc, char **argv)
{
	bfs::ResizeOptions options;
	std::string imagePath;
	std::string sizeText;

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			PrintUsage(argv[0]);
			return 0;
		} else if (arg == "-n" || arg == "--dry-run") {
			options.dryRun = true;
		} else if (arg == "-v" || arg == "--verbose") {
			options.verbose = true;
		} else if (!arg.empty() && arg[0] == '-') {
			fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg.c_str());
			PrintUsage(argv[0]);
			return 1;
		} else if (imagePath.empty()) {
			imagePath = arg;
		} else if (sizeText.empty()) {
			sizeText = arg;
		} else {
			fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], arg.c_str());
			return 1;
		}
	}

	if (imagePath.empty() || sizeText.empty()) {
		PrintUsage(argv[0]);
		return 1;
	}

	uint64_t newSize = 0;
	if (!ParseSize(sizeText, newSize)) {
		fprintf(stderr, "%s: invalid size '%s'\n", argv[0], sizeText.c_str());
		return 1;
	}

	try {
		bfs::Resizer resizer(imagePath, options);
		resizer.Run(newSize);
	} catch (const std::exception &error) {
		fprintf(stderr, "bfsresize: %s\n", error.what());
		return 1;
	}

	return 0;
}
