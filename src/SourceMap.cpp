#include "SourceMap.hpp"

#include <algorithm>
#include <sstream>

namespace {

const SourceFile* fileForId(const std::vector<SourceFile>& files, SourceFileId id)
{
    if (!id.valid()) {
        return nullptr;
    }

    for (const SourceFile& file : files) {
        if (file.id == id) {
            return &file;
        }
    }

    return nullptr;
}

} // namespace

std::string sourceLine(const SourceFile& file, SourceLocation location)
{
    if (location.line <= 0) {
        return {};
    }

    std::size_t line = 1;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= file.text.size(); ++index) {
        if (index != file.text.size() && file.text[index] != '\n') {
            continue;
        }

        if (line == static_cast<std::size_t>(location.line)) {
            std::size_t end = index;
            if (end > begin && file.text[end - 1] == '\r') {
                --end;
            }
            return file.text.substr(begin, end - begin);
        }

        ++line;
        begin = index + 1;
    }

    return {};
}

SourcePosition sourcePositionAt(const SourceFile& file, std::size_t byte)
{
    const std::size_t clamped = std::min(byte, file.text.size());
    SourcePosition position{clamped, 1, 1};
    for (std::size_t index = 0; index < clamped; ++index) {
        if (file.text[index] == '\n') {
            ++position.line;
            position.column = 1;
        } else {
            ++position.column;
        }
    }
    return position;
}

SourcePosition sourcePositionAt(const std::vector<SourceFile>& files, const SourceRange& range)
{
    if (const SourceFile* file = fileForId(files, range.source)) {
        return sourcePositionAt(*file, range.start);
    }
    return SourcePosition{range.start, 0, 0};
}

bool isValidSourceRange(const SourceRange& range, const std::vector<SourceFile>& files)
{
    const SourceFile* file = fileForId(files, range.source);
    return file && range.valid() && range.end <= file->text.size();
}

std::string formatSourceSpan(const SourceSpan& span, const std::vector<SourceFile>& files)
{
    const std::string path = span.source < files.size() && !files[span.source].path.empty()
        ? files[span.source].path
        : "<unknown>";
    std::ostringstream output;
    output << path << ':' << span.line << ':' << span.column;
    return output.str();
}

std::string formatSourceRange(const SourceRange& range, const std::vector<SourceFile>& files)
{
    const SourceFile* file = fileForId(files, range.source);
    const std::string path = file && !file->path.empty() ? file->path : "<unknown>";
    const SourcePosition start = file ? sourcePositionAt(*file, range.start)
                                      : SourcePosition{range.start, 0, 0};
    const SourcePosition end = file ? sourcePositionAt(*file, range.end)
                                    : SourcePosition{range.end, 0, 0};
    std::ostringstream output;
    output << path << ':' << start.line << ':' << start.column
           << '-' << end.line << ':' << end.column;
    return output.str();
}
