#include "Findings.h"

#include <stdio.h>


namespace bfs {


void Findings::Report(Severity severity, const std::string &category,
	const std::string &message, int64_t block, const std::string &path)
{
	CategoryStat &stat = fStats[category];
	if (severity == Severity::Error) {
		fErrors++;
		stat.errors++;
	} else {
		fWarnings++;
		stat.warnings++;
	}

	if (stat.printed < fCap) {
		stat.printed++;
		const char *label = severity == Severity::Error ? "ERROR" : "warning";
		printf("%s [%s]", label, category.c_str());
		if (block >= 0) {
			printf(" block %lld", static_cast<long long>(block));
		}
		if (!path.empty()) {
			printf(" %s", path.c_str());
		}
		printf(": %s\n", message.c_str());
		if (stat.printed == fCap) {
			printf("  ... further '%s' findings suppressed\n", category.c_str());
		}
	}
}


void Findings::PrintSummary() const
{
	printf("\nSummary:\n");
	for (const auto &entry : fStats) {
		printf("  %-16s %zu error(s), %zu warning(s)\n", entry.first.c_str(),
			entry.second.errors, entry.second.warnings);
	}
	printf("Total: %zu error(s), %zu warning(s)\n", fErrors, fWarnings);
}


} // bfs
