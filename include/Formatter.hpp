#pragma once

#include "LosslessSource.hpp"

#include <cstddef>
#include <string>

struct FormatterOptions {
    std::size_t indentWidth = 2;
};

// Format one source file from the production lossless token/trivia view.
// Token lexemes, string contents, and line-comment text are copied verbatim;
// whitespace is normalized using the stable formatter layout.
std::string formatLosslessSource(
    const LosslessSourceFileView& source,
    FormatterOptions options = {});
