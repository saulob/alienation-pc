// The launcher's own text. Every call site goes through Tr so a translation table can be added without touching them.
#pragma once

#include <string>

namespace aln {

const char* Tr(const char* english);
std::string Tr(const std::string& english);
// Tr for text built at runtime; never used as a format string.
std::string TrValue(const std::string& english);

} // namespace aln
