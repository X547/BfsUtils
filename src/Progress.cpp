#include "Progress.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>


namespace bfs {


namespace {


// Past ~10 Hz a numeric readout just blurs, so a faster repaint would cost
// terminal bandwidth without telling the reader anything more.
constexpr int64_t kRepaintIntervalUs = 100000;

// Aim for this many clock reads per repaint interval. High enough that a phase
// which suddenly slows down still repaints promptly, low enough to vanish next
// to the per-block syscalls the tick sits between.
constexpr uint64_t kChecksPerInterval = 16;

// Even at the fastest plausible tick rate this keeps a clock read within a few
// milliseconds of real time, so a stall cannot freeze the display for long.
constexpr uint64_t kMaxTickMask = 1023;

// Width of the phase-name column; the longest name is "laying out".
constexpr int kPhaseWidth = 14;


int64_t MonotonicUs()
{
	struct timespec now;
	if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return static_cast<int64_t>(now.tv_sec) * 1000000
		+ static_cast<int64_t>(now.tv_nsec) / 1000;
}


// One write() per repaint: stderr is unbuffered, so formatting straight to it
// could issue several syscalls and let another writer tear the line apart.
void WriteAll(const char *data, size_t length)
{
	size_t written = 0;
	while (written < length) {
		ssize_t n = ::write(STDERR_FILENO, data + written, length - written);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;   // a broken stderr must never derail the build
		}
		written += static_cast<size_t>(n);
	}
}


std::string FormatBytes(int64_t bytes)
{
	static const char *kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	constexpr size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

	double value = static_cast<double>(bytes);
	size_t unit = 0;
	while (value >= 1024.0 && unit + 1 < kUnitCount) {
		value /= 1024.0;
		unit++;
	}

	char buffer[64];
	if (unit == 0) {
		::snprintf(buffer, sizeof(buffer), "%lld B",
			static_cast<long long>(bytes));
	} else if (value < 10.0) {
		::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unit]);
	} else if (value < 100.0) {
		::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
	} else {
		::snprintf(buffer, sizeof(buffer), "%.0f %s", value, kUnits[unit]);
	}
	return buffer;
}


} // unnamed namespace


Progress::Progress(bool enabled, bool verbose):
	fEnabled(enabled),
	fVerbose(verbose)
{
}


Progress::~Progress()
{
	Finish();
}


bool ProgressIsUseful()
{
	return ::isatty(STDERR_FILENO) != 0;
}


void Progress::StartPhase(const char *name, const char *noun, int64_t total,
	Unit unit, bool counted)
{
	Finish();

	fPhase = name;
	fNoun = noun == nullptr ? "" : noun;
	fUnit = unit;
	fCounted = counted;
	fTotal = total;
	fCurrent = 0;

	fTicks = 0;
	fTickMask = 0;
	fLastPaintTicks = 0;

	fPhaseStartUs = MonotonicUs();
	fLastPaintUs = fPhaseStartUs;
	fLastWidth = 0;
	fRenderedCurrent = -1;

	if (fEnabled) {
		Render();
	}
}


void Progress::BeginCountPhase(const char *name, const char *noun, int64_t total)
{
	StartPhase(name, noun, total, Unit::Items, true);
}


void Progress::BeginBytePhase(const char *name, int64_t totalBytes)
{
	StartPhase(name, nullptr, totalBytes, Unit::Bytes, true);
}


void Progress::BeginPhase(const char *name)
{
	StartPhase(name, nullptr, 0, Unit::Items, false);
}


void Progress::MaybeRepaint()
{
	int64_t now = MonotonicUs();
	if (now - fLastPaintUs < kRepaintIntervalUs) {
		return;
	}

	uint64_t ticksSince = fTicks - fLastPaintTicks;
	uint64_t perCheck = ticksSince / kChecksPerInterval;
	uint64_t mask = 0;
	while (mask + 1 < perCheck && mask < kMaxTickMask) {
		mask = mask * 2 + 1;
	}
	fTickMask = mask;

	fLastPaintUs = now;
	fLastPaintTicks = fTicks;
	Render();
}


void Progress::Render()
{
	char body[192];
	int length = 0;

	if (!fCounted) {
		length = ::snprintf(body, sizeof(body), "%s", fPhase);
	} else if (fUnit == Unit::Bytes) {
		std::string current = FormatBytes(fCurrent);
		int64_t elapsed = MonotonicUs() - fPhaseStartUs;

		char rate[48];
		rate[0] = '\0';
		if (elapsed > 200000 && fCurrent > 0) {
			std::string perSecond = FormatBytes(
				fCurrent * 1000000 / elapsed);
			::snprintf(rate, sizeof(rate), "   %s/s", perSecond.c_str());
		}

		if (fTotal > 0) {
			int64_t percent = fCurrent * 100 / fTotal;
			if (percent > 100) {
				percent = 100;
			}
			length = ::snprintf(body, sizeof(body), "%-*s%3lld%%  %s / %s%s",
				kPhaseWidth, fPhase, static_cast<long long>(percent),
				current.c_str(), FormatBytes(fTotal).c_str(), rate);
		} else {
			length = ::snprintf(body, sizeof(body), "%-*s%s%s",
				kPhaseWidth, fPhase, current.c_str(), rate);
		}
	} else if (fTotal > 0) {
		length = ::snprintf(body, sizeof(body), "%-*s%lld / %lld %s",
			kPhaseWidth, fPhase, static_cast<long long>(fCurrent),
			static_cast<long long>(fTotal), fNoun);
	} else {
		length = ::snprintf(body, sizeof(body), "%-*s%lld %s",
			kPhaseWidth, fPhase, static_cast<long long>(fCurrent), fNoun);
	}

	if (length < 0) {
		return;
	}
	size_t width = static_cast<size_t>(length);
	if (width >= sizeof(body)) {
		width = sizeof(body) - 1;
	}

	// Pad over whatever the previous, longer line left behind. The next repaint
	// returns to column zero, so the trailing blanks need no cursor fixup.
	std::vector<char> line;
	line.reserve(1 + width + fLastWidth);
	line.push_back('\r');
	line.insert(line.end(), body, body + width);
	for (size_t i = width; i < fLastWidth; i++) {
		line.push_back(' ');
	}
	WriteAll(line.data(), line.size());

	fLastWidth = width;
	fRenderedCurrent = fCurrent;
	fLineLive = true;
}


void Progress::EraseLine()
{
	if (!fLineLive) {
		return;
	}

	std::vector<char> line;
	line.reserve(2 + fLastWidth);
	line.push_back('\r');
	for (size_t i = 0; i < fLastWidth; i++) {
		line.push_back(' ');
	}
	line.push_back('\r');
	WriteAll(line.data(), line.size());

	fLastWidth = 0;
	fLineLive = false;
}


void Progress::Message(const std::string &text)
{
	EraseLine();

	std::string line = text;
	line.push_back('\n');
	WriteAll(line.data(), line.size());

	if (fEnabled) {
		Render();
	}
}


void Progress::Note(const std::string &text)
{
	if (!fVerbose) {
		return;
	}
	Message(text);
}


void Progress::Finish()
{
	if (!fLineLive) {
		return;
	}
	// The last Advance() was probably throttled out, so the closing numbers still
	// need a paint — unless nothing has moved since the line was last drawn.
	if (fRenderedCurrent != fCurrent) {
		Render();
	}
	WriteAll("\n", 1);
	fLineLive = false;
	fLastWidth = 0;
}


} // bfs
