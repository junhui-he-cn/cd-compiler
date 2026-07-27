#pragma once

#include <iosfwd>

// Run the first stdio JSON-RPC language-server boundary.  The service keeps
// document state in memory and delegates parsing, diagnostics, and formatting
// to the production compiler services.
int runLanguageServer(std::istream& input, std::ostream& output);
