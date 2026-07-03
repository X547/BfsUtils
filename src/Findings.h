#pragma once

#include <stdint.h>

#include <map>
#include <string>


namespace bfs {


enum class Severity {
	Error,
	Warning,
};


// Collects and reports integrity findings. Findings are printed inline as they
// are reported (so a later hang or crash still leaves a trail), capped per
// category to avoid flooding, and tallied for a final summary.
class Findings {
public:
	explicit Findings(size_t perCategoryCap = 20):
		fCap(perCategoryCap)
	{
	}

	void Report(Severity severity, const std::string &category,
		const std::string &message, int64_t block = -1,
		const std::string &path = std::string());

	void Error(const std::string &category, const std::string &message,
		int64_t block = -1, const std::string &path = std::string())
	{
		Report(Severity::Error, category, message, block, path);
	}

	void Warning(const std::string &category, const std::string &message,
		int64_t block = -1, const std::string &path = std::string())
	{
		Report(Severity::Warning, category, message, block, path);
	}

	size_t Errors() const {return fErrors;}
	size_t Warnings() const {return fWarnings;}

	void PrintSummary() const;

private:
	struct CategoryStat {
		size_t errors = 0;
		size_t warnings = 0;
		size_t printed = 0;
	};

	std::map<std::string, CategoryStat> fStats;
	size_t fErrors = 0;
	size_t fWarnings = 0;
	size_t fCap;
};


} // bfs
