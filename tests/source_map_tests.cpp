#include "SourceMap.hpp"

#include <cassert>

int main()
{
    SourceFile file{"demo.cd", "let x = 1;\nprint(x);\n", SourceFileId{0}};
    assert(sourceLine(file, SourceLocation{2, 1}) == "print(x);");
    assert(sourceLine(file, SourceLocation{9, 1}).empty());
    assert(formatSourceSpan(SourceSpan{0, 2, 7}, {file}) == "demo.cd:2:7");
    const SourcePosition lineStart = sourcePositionAt(file, 11);
    assert(lineStart.line == 2 && lineStart.column == 1);
    const SourceRange range{SourceFileId{0}, 4, 5};
    assert(isValidSourceRange(range, {file}));
    assert(formatSourceRange(range, {file}) == "demo.cd:1:5-1:6");
    return 0;
}
