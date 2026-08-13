#include "ModuleCache.hpp"

#include "ModuleInterfaceArtifact.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

constexpr int kModuleCacheSchemaVersion = 4;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

const char* dependencyKindName(ModuleGraphEdgeKind kind)
{
    return kind == ModuleGraphEdgeKind::Import ? "import" : "re_export";
}

std::optional<ModuleGraphEdgeKind> parseDependencyKind(const std::string& value)
{
    if (value == "import") {
        return ModuleGraphEdgeKind::Import;
    }
    if (value == "re_export") {
        return ModuleGraphEdgeKind::ReExport;
    }
    return std::nullopt;
}

void appendField(std::ostringstream& out, const std::string& name, const std::string& value)
{
    out << name << ':' << value.size() << ':' << value;
}

std::string quotedString(const std::string& value)
{
    std::ostringstream out;
    out << std::quoted(value);
    return out.str();
}

bool sameDependency(const ModuleCacheDependency& left, const ModuleCacheDependency& right)
{
    return left.identity == right.identity
        && left.kind == right.kind
        && left.requestedPath == right.requestedPath
        && left.interfaceHash == right.interfaceHash;
}

bool sameDependencies(
    const std::vector<ModuleCacheDependency>& left,
    const std::vector<ModuleCacheDependency>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(), sameDependency);
}

bool sameModuleMetadata(const ModuleCacheModule& left, const ModuleCacheModule& right)
{
    return left.isEntry == right.isEntry
        && left.entryOrder == right.entryOrder;
}

bool sameOptimizationIdentity(
    const ModuleCacheModule& left,
    const ModuleCacheModule& right)
{
    return left.optimizationLevel == right.optimizationLevel
        && left.optimizerPipeline == right.optimizerPipeline;
}

std::string parseError(std::size_t line, const std::string& message)
{
    return "line " + std::to_string(line) + ": " + message;
}

std::string moduleCacheInterfaceArtifactPath(const ModuleCacheModule& module)
{
    return moduleInterfaceArtifactPath({}, module.identity).generic_string();
}

bool parseQuotedField(
    const std::string& line,
    const std::string& prefix,
    std::string& value)
{
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    std::istringstream input(line.substr(prefix.size()));
    if (!(input >> std::quoted(value))) {
        return false;
    }
    input >> std::ws;
    return input.eof();
}

bool parseBooleanField(
    const std::string& line,
    const std::string& prefix,
    bool& value)
{
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string text = line.substr(prefix.size());
    if (text == "true") {
        value = true;
        return true;
    }
    if (text == "false") {
        value = false;
        return true;
    }
    return false;
}

bool parseSizeField(
    const std::string& line,
    const std::string& prefix,
    std::size_t& value)
{
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string text = line.substr(prefix.size());
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseDependencyLine(
    const std::string& line,
    std::size_t expectedIndex,
    ModuleCacheDependency& dependency)
{
    const std::string indexPrefix = "    d";
    if (line.rfind(indexPrefix, 0) != 0) {
        return false;
    }

    const std::size_t indexEnd = line.find(' ', indexPrefix.size());
    if (indexEnd == std::string::npos) {
        return false;
    }
    const std::string indexText = line.substr(indexPrefix.size(), indexEnd - indexPrefix.size());
    if (indexText.empty() || indexText.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    std::size_t index = 0;
    try {
        const unsigned long long parsed = std::stoull(indexText);
        if (parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        index = static_cast<std::size_t>(parsed);
    } catch (const std::exception&) {
        return false;
    }
    if (index != expectedIndex) {
        return false;
    }

    std::size_t cursor = indexEnd;
    const auto consume = [&line, &cursor](const std::string& text) {
        if (line.compare(cursor, text.size(), text) != 0) {
            return false;
        }
        cursor += text.size();
        return true;
    };
    const auto readQuoted = [&line, &cursor](std::string& value) {
        if (cursor >= line.size() || line[cursor] != '"') {
            return false;
        }
        std::istringstream input(line.substr(cursor));
        if (!(input >> std::quoted(value))) {
            return false;
        }
        const std::streamoff consumed = input.tellg();
        if (consumed < 0) {
            cursor = line.size();
            return true;
        }
        cursor += static_cast<std::size_t>(consumed);
        return true;
    };

    if (!consume(" target=") || !readQuoted(dependency.identity)
        || !consume(" kind=")) {
        return false;
    }
    const std::size_t kindEnd = line.find(' ', cursor);
    if (kindEnd == std::string::npos) {
        return false;
    }
    const std::optional<ModuleGraphEdgeKind> kind = parseDependencyKind(
        line.substr(cursor, kindEnd - cursor));
    if (!kind) {
        return false;
    }
    dependency.kind = *kind;
    cursor = kindEnd;
    if (!consume(" requested=") || !readQuoted(dependency.requestedPath)
        || !consume(" interface=") || !readQuoted(dependency.interfaceHash)) {
        return false;
    }
    return cursor == line.size();
}

struct CacheEvaluation {
    bool visiting = false;
    bool complete = false;
    bool ownPublicChanged = false;
    bool publicImpact = false;
    ModuleCacheDecision decision;
};

const ModuleCacheRecord* findRecord(
    const std::unordered_map<std::string, const ModuleCacheRecord*>& records,
    const std::string& identity)
{
    const auto found = records.find(identity);
    return found == records.end() ? nullptr : found->second;
}

void writeJsonString(std::ostream& out, const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u00" << hex[(ch >> 4) & 0xf] << hex[ch & 0xf];
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
}

} // namespace

std::string moduleCacheHash(const std::string& value)
{
    std::uint64_t hash = kFnvOffset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= kFnvPrime;
    }

    std::ostringstream output;
    output << "fnv1a64-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string moduleCacheArtifactDigest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return moduleCacheHash(contents.str());
}

std::string moduleCacheKey(const ModuleCacheModule& module)
{
    std::ostringstream input;
    input << "module-cache-key-v1\0";
    appendField(input, "artifact", "cdbc 0.1");
    appendField(input, "identity", module.identity);
    appendField(input, "source", module.sourceHash);
    appendField(input, "interface", module.interfaceHash);
    appendField(input, "optimization_level", module.optimizationLevel);
    appendField(input, "optimizer_pipeline", module.optimizerPipeline);
    appendField(input, "entry", module.isEntry ? "true" : "false");
    appendField(
        input,
        "entry_order",
        module.entryOrder ? std::to_string(*module.entryOrder) : "none");
    input << "dependencies:" << module.dependencies.size();
    for (const ModuleCacheDependency& dependency : module.dependencies) {
        appendField(input, "identity", dependency.identity);
        appendField(input, "kind", dependencyKindName(dependency.kind));
        appendField(input, "requested", dependency.requestedPath);
        appendField(input, "interface", dependency.interfaceHash);
    }
    return moduleCacheHash(input.str());
}

std::string moduleCacheArtifactPath(const ModuleCacheModule& module)
{
    const std::string key = module.cacheKey.empty() ? moduleCacheKey(module) : module.cacheKey;
    return "products/product-" + key + ".cdbc";
}

ModuleCacheLoadResult readModuleCache(const std::filesystem::path& path)
{
    ModuleCacheLoadResult result;
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        result.found = std::filesystem::exists(path, error);
        if (result.found) {
            result.error = "failed to read module cache";
        }
        return result;
    }
    result.found = true;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }

    std::size_t cursor = 0;
    const auto nextNonEmpty = [&lines, &cursor]() -> std::optional<std::pair<std::size_t, std::string>> {
        while (cursor < lines.size() && lines[cursor].empty()) {
            ++cursor;
        }
        if (cursor == lines.size()) {
            return std::nullopt;
        }
        const std::size_t lineNumber = cursor + 1;
        return std::make_pair(lineNumber, lines[cursor++]);
    };
    const auto fail = [&result](std::size_t lineNumber, const std::string& message) {
        result.manifest.reset();
        result.error = parseError(lineNumber, message);
    };

    const auto header = nextNonEmpty();
    if (!header || header->second != "cdbc-cache 0.2") {
        fail(header ? header->first : 1, "expected cdbc-cache 0.2");
        return result;
    }
    const auto schema = nextNonEmpty();
    std::size_t schemaVersion = 0;
    if (!schema || !parseSizeField(schema->second, "schema = ", schemaVersion)
        || schemaVersion != kModuleCacheSchemaVersion) {
        fail(schema ? schema->first : 1, "expected schema = 4");
        return result;
    }
    const auto modulesHeader = nextNonEmpty();
    if (!modulesHeader || modulesHeader->second != "modules:") {
        fail(modulesHeader ? modulesHeader->first : 1, "expected modules:");
        return result;
    }

    ModuleCacheManifest manifest;
    manifest.schemaVersion = static_cast<int>(schemaVersion);
    std::size_t expectedModuleIndex = 0;
    while (true) {
        const auto moduleHeader = nextNonEmpty();
        if (!moduleHeader) {
            break;
        }
        if (moduleHeader->second.rfind("module ", 0) != 0) {
            fail(moduleHeader->first, "expected module record");
            return result;
        }
        std::size_t moduleIndex = 0;
        if (!parseSizeField(moduleHeader->second, "module ", moduleIndex)
            || moduleIndex != expectedModuleIndex) {
            fail(moduleHeader->first, "module records must be ordered from zero");
            return result;
        }

        ModuleCacheRecord record;
        std::string value;
        const auto identity = nextNonEmpty();
        const auto sourceHash = nextNonEmpty();
        const auto interfaceHash = nextNonEmpty();
        const auto optimizationLevel = nextNonEmpty();
        const auto optimizerPipeline = nextNonEmpty();
        const auto cacheKey = nextNonEmpty();
        const auto artifact = nextNonEmpty();
        const auto interfaceArtifact = nextNonEmpty();
        const auto contentDigest = nextNonEmpty();
        const auto entry = nextNonEmpty();
        if (!identity || !sourceHash || !interfaceHash || !optimizationLevel
            || !optimizerPipeline || !cacheKey || !artifact
            || !interfaceArtifact || !contentDigest || !entry
            || !parseQuotedField(identity->second, "  identity = ", record.module.identity)
            || !parseQuotedField(sourceHash->second, "  source = ", record.module.sourceHash)
            || !parseQuotedField(interfaceHash->second, "  interface = ", record.module.interfaceHash)
            || !parseQuotedField(
                optimizationLevel->second,
                "  optimization_level = ",
                record.module.optimizationLevel)
            || !parseQuotedField(
                optimizerPipeline->second,
                "  optimizer_pipeline = ",
                record.module.optimizerPipeline)
            || !parseQuotedField(cacheKey->second, "  key = ", record.module.cacheKey)
            || !parseQuotedField(artifact->second, "  artifact = ", record.artifactPath)
            || !parseQuotedField(interfaceArtifact->second, "  interface_artifact = ", record.interfaceArtifactPath)
            || !parseQuotedField(contentDigest->second, "  content = ", record.module.contentDigest)
            || !parseBooleanField(entry->second, "  entry = ", record.module.isEntry)) {
            fail(identity ? identity->first : moduleHeader->first, "invalid module fields");
            return result;
        }
        if (record.module.identity.empty()
            || record.module.optimizationLevel.empty()
            || record.module.optimizerPipeline.empty()
            || std::filesystem::path(record.artifactPath).is_absolute()
            || std::filesystem::path(record.artifactPath).lexically_normal().string() != record.artifactPath
            || record.interfaceArtifactPath.empty()
            || std::filesystem::path(record.interfaceArtifactPath).is_absolute()
            || std::filesystem::path(record.interfaceArtifactPath).lexically_normal().generic_string()
                != record.interfaceArtifactPath
            || record.module.contentDigest.empty()) {
            fail(moduleHeader->first, "invalid module identity, key, or artifact path");
            return result;
        }

        const auto maybeEntryOrder = nextNonEmpty();
        if (maybeEntryOrder && maybeEntryOrder->second.rfind("  entry_order = ", 0) == 0) {
            std::size_t entryOrder = 0;
            if (!parseSizeField(maybeEntryOrder->second, "  entry_order = ", entryOrder)) {
                fail(maybeEntryOrder->first, "invalid entry order");
                return result;
            }
            record.module.entryOrder = entryOrder;
        } else if (maybeEntryOrder) {
            --cursor;
        }
        if (record.module.isEntry != record.module.entryOrder.has_value()) {
            fail(moduleHeader->first, "entry order must match entry flag");
            return result;
        }

        const auto dependenciesHeader = nextNonEmpty();
        if (!dependenciesHeader || dependenciesHeader->second != "  dependencies:") {
            fail(dependenciesHeader ? dependenciesHeader->first : moduleHeader->first, "expected dependencies:");
            return result;
        }
        while (true) {
            const auto dependencyLine = nextNonEmpty();
            if (!dependencyLine) {
                fail(moduleHeader->first, "unterminated module record");
                return result;
            }
            if (dependencyLine->second == "end") {
                break;
            }
            ModuleCacheDependency dependency;
            if (!parseDependencyLine(
                    dependencyLine->second,
                    record.module.dependencies.size(),
                    dependency)
                || dependency.identity.empty()) {
                fail(dependencyLine->first, "invalid dependency record");
                return result;
            }
            record.module.dependencies.push_back(std::move(dependency));
        }

        if (record.module.cacheKey != moduleCacheKey(record.module)
            || record.artifactPath != moduleCacheArtifactPath(record.module)
            || record.interfaceArtifactPath != moduleCacheInterfaceArtifactPath(record.module)) {
            fail(moduleHeader->first, "artifact path does not match module key");
            return result;
        }
        manifest.records.push_back(std::move(record));
        ++expectedModuleIndex;
    }

    std::unordered_map<std::string, bool> identities;
    for (const ModuleCacheRecord& record : manifest.records) {
        if (!identities.emplace(record.module.identity, true).second) {
            fail(1, "duplicate module identity");
            return result;
        }
    }
    result.manifest = std::move(manifest);
    return result;
}

void writeModuleCache(
    const std::filesystem::path& path,
    const ModuleCacheManifest& manifest)
{
    if (manifest.schemaVersion != kModuleCacheSchemaVersion) {
        throw std::runtime_error("unsupported module cache schema version");
    }
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            throw std::runtime_error("failed to create module cache directory: " + error.message());
        }
    }

    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("failed to open module cache: " + path.string());
    }
    output << "cdbc-cache 0.2\n\n"
           << "schema = " << manifest.schemaVersion << '\n'
           << "modules:\n";
    for (std::size_t index = 0; index < manifest.records.size(); ++index) {
        const ModuleCacheRecord& record = manifest.records[index];
        ModuleCacheModule module = record.module;
        if (module.cacheKey.empty()) {
            module.cacheKey = moduleCacheKey(module);
        }
        const std::string interfaceArtifactPath = record.interfaceArtifactPath.empty()
            ? moduleCacheInterfaceArtifactPath(module)
            : record.interfaceArtifactPath;
        if (module.cacheKey != moduleCacheKey(module)
            || record.artifactPath != moduleCacheArtifactPath(module)
            || interfaceArtifactPath != moduleCacheInterfaceArtifactPath(module)
            || module.contentDigest.empty()) {
            throw std::runtime_error("invalid module cache record");
        }
        output << "module " << index << '\n'
               << "  identity = " << quotedString(module.identity) << '\n'
               << "  source = " << quotedString(module.sourceHash) << '\n'
               << "  interface = " << quotedString(module.interfaceHash) << '\n'
               << "  optimization_level = " << quotedString(module.optimizationLevel) << '\n'
               << "  optimizer_pipeline = " << quotedString(module.optimizerPipeline) << '\n'
               << "  key = " << quotedString(module.cacheKey) << '\n'
               << "  artifact = " << quotedString(record.artifactPath) << '\n'
               << "  interface_artifact = " << quotedString(interfaceArtifactPath) << '\n'
               << "  content = " << quotedString(module.contentDigest) << '\n'
               << "  entry = " << (module.isEntry ? "true" : "false") << '\n';
        if (module.entryOrder) {
            output << "  entry_order = " << *module.entryOrder << '\n';
        }
        output << "  dependencies:\n";
        for (std::size_t dependencyIndex = 0; dependencyIndex < module.dependencies.size(); ++dependencyIndex) {
            const ModuleCacheDependency& dependency = module.dependencies[dependencyIndex];
            output << "    d" << dependencyIndex
                   << " target=" << quotedString(dependency.identity)
                   << " kind=" << dependencyKindName(dependency.kind)
                   << " requested=" << quotedString(dependency.requestedPath)
                   << " interface=" << quotedString(dependency.interfaceHash)
                   << '\n';
        }
        output << "end\n";
    }
    if (!output) {
        throw std::runtime_error("failed to write module cache: " + path.string());
    }
}

std::vector<ModuleCacheDecision> planModuleCacheBuild(
    const std::vector<ModuleCacheModule>& modules,
    const ModuleCacheManifest& previous,
    const std::filesystem::path& cacheDirectory,
    const std::string& emptyCacheReason)
{
    std::unordered_map<std::string, const ModuleCacheModule*> currentByIdentity;
    std::unordered_map<std::string, const ModuleCacheRecord*> previousByIdentity;
    std::vector<ModuleCacheModule> normalizedModules;
    normalizedModules.reserve(modules.size());
    for (const ModuleCacheModule& source : modules) {
        ModuleCacheModule module = source;
        module.cacheKey = moduleCacheKey(module);
        if (!currentByIdentity.emplace(module.identity, nullptr).second) {
            throw std::runtime_error("duplicate current module cache identity");
        }
        normalizedModules.push_back(std::move(module));
    }
    for (ModuleCacheModule& module : normalizedModules) {
        currentByIdentity[module.identity] = &module;
    }
    for (const ModuleCacheRecord& record : previous.records) {
        if (!previousByIdentity.emplace(record.module.identity, &record).second) {
            throw std::runtime_error("duplicate previous module cache identity");
        }
    }

    std::unordered_map<std::string, CacheEvaluation> evaluations;
    std::function<void(const std::string&)> evaluate = [&](const std::string& identity) {
        CacheEvaluation& evaluation = evaluations[identity];
        if (evaluation.complete) {
            return;
        }
        if (evaluation.visiting) {
            throw std::runtime_error("module cache graph contains a cycle at " + identity);
        }
        const auto current = currentByIdentity.find(identity);
        if (current == currentByIdentity.end() || !current->second) {
            throw std::runtime_error("module cache dependency targets missing module " + identity);
        }
        evaluation.visiting = true;
        const ModuleCacheModule& module = *current->second;
        const ModuleCacheRecord* previousRecord = findRecord(previousByIdentity, identity);
        const bool hasPrevious = previousRecord != nullptr;
        const bool sourceChanged = !hasPrevious
            || previousRecord->module.sourceHash != module.sourceHash;
        const bool publicChanged = !hasPrevious
            || previousRecord->module.interfaceHash != module.interfaceHash;
        const bool dependencyChanged = !hasPrevious
            || !sameDependencies(previousRecord->module.dependencies, module.dependencies);
        const bool metadataChanged = !hasPrevious
            || !sameModuleMetadata(previousRecord->module, module);
        const bool optimizationChanged = !hasPrevious
            || !sameOptimizationIdentity(previousRecord->module, module);
        const bool keyChanged = !hasPrevious
            || previousRecord->module.cacheKey != module.cacheKey;

        bool dependencyPublicImpact = false;
        bool directDependencyPublicImpact = false;
        for (const ModuleCacheDependency& dependency : module.dependencies) {
            evaluate(dependency.identity);
            const CacheEvaluation& dependencyEvaluation = evaluations.at(dependency.identity);
            dependencyPublicImpact = dependencyPublicImpact || dependencyEvaluation.publicImpact;
            directDependencyPublicImpact = directDependencyPublicImpact
                || dependencyEvaluation.ownPublicChanged;
        }

        const bool artifactMissing = !std::filesystem::exists(
            cacheDirectory / moduleCacheArtifactPath(module));
        bool artifactDigestMismatch = false;
        if (!artifactMissing && hasPrevious
            && !previousRecord->module.contentDigest.empty()) {
            const std::string currentDigest = moduleCacheArtifactDigest(
                cacheDirectory / moduleCacheArtifactPath(module));
            artifactDigestMismatch = currentDigest
                != previousRecord->module.contentDigest;
        }
        const bool needsRebuild = !hasPrevious
            || sourceChanged
            || dependencyChanged
            || dependencyPublicImpact
            || metadataChanged
            || optimizationChanged
            || keyChanged
            || artifactMissing
            || artifactDigestMismatch;

        evaluation.ownPublicChanged = publicChanged;
        evaluation.publicImpact = publicChanged || dependencyPublicImpact;
        evaluation.decision.module = module;
        evaluation.decision.status = needsRebuild
            ? ModuleCacheDecisionStatus::Rebuilt
            : ModuleCacheDecisionStatus::Reused;
        evaluation.decision.artifactPath = moduleCacheArtifactPath(module);
        evaluation.decision.publicImpact = evaluation.publicImpact;
        if (!needsRebuild) {
            evaluation.decision.reason = "cache_hit";
        } else if (!hasPrevious) {
            evaluation.decision.reason = emptyCacheReason;
        } else if (sourceChanged && publicChanged) {
            evaluation.decision.reason = "source_and_public_interface_changed";
        } else if (sourceChanged) {
            evaluation.decision.reason = "source_changed";
        } else if (directDependencyPublicImpact) {
            evaluation.decision.reason = "dependency_interface_changed";
        } else if (dependencyPublicImpact) {
            evaluation.decision.reason = "transitive_dependency_interface_changed";
        } else if (dependencyChanged) {
            evaluation.decision.reason = "dependency_graph_changed";
        } else if (publicChanged) {
            evaluation.decision.reason = "public_interface_changed";
        } else if (metadataChanged) {
            evaluation.decision.reason = "module_metadata_changed";
        } else if (optimizationChanged) {
            evaluation.decision.reason = "optimization_configuration_changed";
        } else if (keyChanged) {
            evaluation.decision.reason = "cache_key_changed";
        } else if (artifactDigestMismatch) {
            evaluation.decision.reason = "cache_artifact_invalid";
        } else {
            evaluation.decision.reason = "cache_artifact_missing";
        }
        evaluation.visiting = false;
        evaluation.complete = true;
    };

    for (const ModuleCacheModule& module : normalizedModules) {
        evaluate(module.identity);
    }

    std::vector<ModuleCacheDecision> decisions;
    decisions.reserve(normalizedModules.size());
    for (const ModuleCacheModule& module : normalizedModules) {
        decisions.push_back(evaluations.at(module.identity).decision);
    }
    return decisions;
}

void writeModuleRebuildReport(
    std::ostream& out,
    const std::vector<ModuleCacheDecision>& decisions,
    bool cacheFound,
    const std::string& cacheError)
{
    std::size_t reused = 0;
    for (const ModuleCacheDecision& decision : decisions) {
        if (decision.status == ModuleCacheDecisionStatus::Reused) {
            ++reused;
        }
    }
    out << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"cache_schema\": \"cdbc-cache 0.2\",\n"
        << "  \"cache_status\": ";
    writeJsonString(out, cacheError.empty() ? (cacheFound ? "loaded" : "missing") : "invalid");
    out << ",\n";
    if (!cacheError.empty()) {
        out << "  \"cache_error\": ";
        writeJsonString(out, cacheError);
        out << ",\n";
    }
    out << "  \"summary\": {\n"
        << "    \"module_count\": " << decisions.size() << ",\n"
        << "    \"reused\": " << reused << ",\n"
        << "    \"rebuilt\": " << (decisions.size() - reused) << "\n"
        << "  },\n"
        << "  \"modules\": [\n";
    for (std::size_t index = 0; index < decisions.size(); ++index) {
        const ModuleCacheDecision& decision = decisions[index];
        out << "    {\n"
            << "      \"identity\": ";
        writeJsonString(out, decision.module.identity);
        out << ",\n      \"source_hash\": ";
        writeJsonString(out, decision.module.sourceHash);
        out << ",\n      \"interface_hash\": ";
        writeJsonString(out, decision.module.interfaceHash);
        out << ",\n      \"optimization_level\": ";
        writeJsonString(out, decision.module.optimizationLevel);
        out << ",\n      \"optimizer_pipeline\": ";
        writeJsonString(out, decision.module.optimizerPipeline);
        out << ",\n      \"cache_key\": ";
        writeJsonString(out, decision.module.cacheKey);
        out << ",\n      \"artifact\": ";
        writeJsonString(out, decision.artifactPath);
        out << ",\n      \"status\": ";
        writeJsonString(
            out,
            decision.status == ModuleCacheDecisionStatus::Reused ? "reused" : "rebuilt");
        out << ",\n      \"reason\": ";
        writeJsonString(out, decision.reason);
        out << ",\n      \"public_impact\": "
            << (decision.publicImpact ? "true" : "false") << "\n"
            << "    }" << (index + 1 == decisions.size() ? "" : ",") << '\n';
    }
    out << "  ]\n}\n";
}
