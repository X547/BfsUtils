#pragma once

#include <stdint.h>

#include <string>


namespace bfs {


// A single-line, self-erasing progress indicator on stderr.
//
// Advance() is called from the builder's per-block write loop, where the
// surrounding work is a couple of syscalls, so its fast path must stay in the
// noise: it is inline, and it only consults the clock every fTickMask + 1 ticks.
// The mask is recomputed at every repaint from the tick rate actually observed,
// so a hot loop reaches the clock rarely while a slow phase — which starts at a
// mask of zero and never grows it — still repaints promptly.
class Progress {
public:
	enum class Unit {
		Items,
		Bytes,
	};

	Progress(bool enabled, bool verbose);
	~Progress();

	Progress(const Progress &) = delete;
	Progress &operator=(const Progress &) = delete;

	bool IsEnabled() const {return fEnabled;}
	bool IsVerbose() const {return fVerbose;}

	// Start a phase, ending the previous phase's line so finished phases stay on
	// screen as a log. A 'total' of 0 means the extent is not known in advance,
	// and the phase renders a bare running count instead of a percentage.
	void BeginCountPhase(const char *name, const char *noun, int64_t total = 0);
	void BeginBytePhase(const char *name, int64_t totalBytes = 0);

	// A phase with no counter, for a span that is short but not instant.
	void BeginPhase(const char *name);

	void Advance(int64_t amount = 1)
	{
		if (!fEnabled) {
			return;
		}
		fCurrent += amount;
		if ((++fTicks & fTickMask) == 0) {
			MaybeRepaint();
		}
	}

	// Print a standalone line above the progress line, which is redrawn after it.
	void Message(const std::string &text);

	// Message(), but only in verbose mode.
	void Note(const std::string &text);

	// Repaint a final time and end the line. Idempotent, so it is safe to call on
	// both the success and the error path.
	void Finish();

private:
	void StartPhase(const char *name, const char *noun, int64_t total, Unit unit,
		bool counted);
	void MaybeRepaint();
	void Render();
	void EraseLine();

	bool fEnabled = false;
	bool fVerbose = false;
	bool fLineLive = false;   // an unterminated line is on screen

	const char *fPhase = "";
	const char *fNoun = "";
	Unit fUnit = Unit::Items;
	bool fCounted = false;
	int64_t fTotal = 0;
	int64_t fCurrent = 0;
	int64_t fRenderedCurrent = -1;   // fCurrent as of the last paint

	uint64_t fTicks = 0;
	uint64_t fTickMask = 0;   // consult the clock every fTickMask + 1 ticks
	uint64_t fLastPaintTicks = 0;

	int64_t fPhaseStartUs = 0;
	int64_t fLastPaintUs = 0;
	size_t fLastWidth = 0;
};


// Whether a progress indicator would be useful on stderr, i.e. whether it is a
// terminal. Callers combine this with an explicit opt-out.
bool ProgressIsUseful();


} // bfs
