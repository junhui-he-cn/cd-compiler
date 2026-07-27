#pragma once

#include "LosslessSource.hpp"

#include <cstddef>
#include <string>

struct FormatterOptions {
    std::size_t indentWidth = 2;
};

// Format one source file from the production lossless token/trivia view.
// Token lexemes, string contents, and line-comment text are copied verbatim;
// whitespace is normalized using the stable formatter layout. At top level,
// at most one source blank line between syntax items is retained, and
// parser-accepted trailing comma tokens are preserved. Comma-separated array
// and parenthesized lists use the canonical 100-byte line-width policy.
std::string formatLosslessSource(
    const LosslessSourceFileView& source,
    FormatterOptions options = {});
