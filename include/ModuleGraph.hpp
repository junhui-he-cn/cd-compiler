#pragma once

#include "SourceIdentity.hpp"

#include <cstddef>
#include <string>
#include <vector>

enum class ModuleGraphEdgeKind {
    Import,
    ReExport,
};

struct ModuleGraphNode {
    std::size_t moduleId = 0;
    SourceFileId sourceId;
    std::string path;
    std::string canonicalPath;
    bool isEntry = false;
};

struct ModuleGraphEdge {
    std::size_t importingModuleId = 0;
    std::size_t importedModuleId = 0;
    ModuleGraphEdgeKind kind = ModuleGraphEdgeKind::Import;
    std::string requestedPath;
};

struct ModuleGraph {
    std::vector<ModuleGraphNode> nodes;
    std::vector<ModuleGraphEdge> edges;
};
