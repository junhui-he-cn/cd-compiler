cmake_minimum_required(VERSION 3.16)

project(compiler_design LANGUAGES CXX)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Keep the demo portable by requiring only standard C++17 facilities.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so editors and language servers can index the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(COMPILER_DESIGN_ENABLE_WARNINGS "Enable compiler warning flags for project C++ targets" OFF)

function(compiler_design_apply_warnings target_name)
    if(NOT COMPILER_DESIGN_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

option(COMPILER_DESIGN_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer for project C++ targets" OFF)

function(compiler_design_apply_sanitizers target_name)
    if(NOT COMPILER_DESIGN_ENABLE_SANITIZERS)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

function(compiler_design_configure_target target_name)
    target_include_directories(${target_name} PRIVATE include)
    compiler_design_apply_warnings(${target_name})
    compiler_design_apply_sanitizers(${target_name})
endfunction()

set(COMPILER_DESIGN_FRONTEND_SOURCES
    src/Ast.cpp
    src/DeclarationIndex.cpp
    src/Diagnostic.cpp
    src/FlowFacts.cpp
    src/FrontendSession.cpp
    src/Lexer.cpp
    src/LosslessSource.cpp
    src/ModuleCache.cpp
    src/ModuleInterfaceArtifact.cpp
    src/ModuleSymbols.cpp
    src/NativeStdlib.cpp
    src/Parser.cpp
    src/SourceMap.cpp
    src/TypeChecker.cpp
    src/TypeUtils.cpp
    src/Value.cpp
)

set(COMPILER_DESIGN_IR_SOURCES
    ${COMPILER_DESIGN_FRONTEND_SOURCES}
    src/IR.cpp
    src/IRCompiler.cpp
)

add_executable(compiler_design
    ${COMPILER_DESIGN_IR_SOURCES}
    src/Bytecode.cpp
    src/BytecodeTextEmitter.cpp
    src/BytecodeCompiler.cpp
    src/Formatter.cpp
    src/LanguageServer.cpp
    src/ModuleInterfaceEmitter.cpp
    src/main.cpp
)
compiler_design_configure_target(compiler_design)

add_executable(source_map_tests
    tests/source_map_tests.cpp
    src/SourceMap.cpp
)
compiler_design_configure_target(source_map_tests)

add_executable(ir_source_location_tests
    tests/ir_source_location_tests.cpp
    ${COMPILER_DESIGN_IR_SOURCES}
)
compiler_design_configure_target(ir_source_location_tests)

enable_testing()
add_test(NAME source_map COMMAND source_map_tests)
add_test(NAME ir_source_location COMMAND ir_source_location_tests)
add_test(
    NAME golden_runner_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_golden_tests_selftest.py
)

add_test(
    NAME benchmark_runner_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_benchmarks_selftest.py
)

add_test(
    NAME verification_inventory
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/verification_inventory.py
)

add_test(
    NAME verification_runner_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_verification_selftest.py
)

add_test(
    NAME boundary_comparison_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/boundary_comparison_selftest.py
)

add_test(
    NAME malformed_harness_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/malformed_tests_selftest.py
)

add_test(
    NAME verification_matrix_selftest
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/verification_matrix_selftest.py
)

add_test(
    NAME cmake_config
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/cmake_config_tests.py ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME ci_workflow
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/ci_workflow_tests.py ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME cli_multi_source
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/cli_multi_source_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME lexer_recovery
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/lexer_recovery_tests.py $<TARGET_FILE:compiler_design>
)

add_test(
    NAME golden
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_golden_tests.py $<TARGET_FILE:compiler_design>
)

add_test(
    NAME bytecode_artifacts
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/bytecode_artifact_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME bytecode_module_artifacts
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/bytecode_module_artifact_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME bytecode_module_cache
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/bytecode_module_cache_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME module_debug_metadata
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/module_debug_metadata_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME debugger
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/debugger_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME profile
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/profile_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_test(
    NAME rust_vm
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_rust_vm_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs --goldens
)

add_test(
    NAME vm_resource_budget
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/vm_resource_budget_tests.py $<TARGET_FILE:compiler_design> ${CMAKE_SOURCE_DIR}/vm-rs
)

add_executable(frontend_session_tests
    tests/frontend_session_tests.cpp
    ${COMPILER_DESIGN_FRONTEND_SOURCES}
)
compiler_design_configure_target(frontend_session_tests)
add_test(NAME frontend_session COMMAND frontend_session_tests)

add_executable(formatter_tests
    tests/formatter_tests.cpp
    src/Ast.cpp
    src/Diagnostic.cpp
    src/Formatter.cpp
    src/Lexer.cpp
    src/LosslessSource.cpp
    src/Parser.cpp
    src/SourceMap.cpp
)
compiler_design_configure_target(formatter_tests)
add_test(NAME formatter COMMAND formatter_tests)
add_test(
    NAME formatter_cli
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/formatter_cli_tests.py $<TARGET_FILE:compiler_design>
)
add_test(
    NAME formatter_corpus
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/formatter_corpus_tests.py $<TARGET_FILE:compiler_design>
)

add_test(
    NAME language_server
    COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/lsp_tests.py $<TARGET_FILE:compiler_design>
)

add_executable(flow_facts_tests
    tests/flow_facts_tests.cpp
    src/Ast.cpp
    src/FlowFacts.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(flow_facts_tests)
add_test(NAME flow_facts COMMAND flow_facts_tests)

add_executable(type_utils_tests
    tests/type_utils_tests.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(type_utils_tests)
add_test(NAME type_utils COMMAND type_utils_tests)

add_executable(value_tests
    tests/value_tests.cpp
    src/Value.cpp
)
compiler_design_configure_target(value_tests)
add_test(NAME value_tests COMMAND value_tests)

add_executable(module_symbols_tests
    tests/module_symbols_tests.cpp
    src/ModuleSymbols.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(module_symbols_tests)
add_test(NAME module_symbols COMMAND module_symbols_tests)

add_executable(module_interface_emitter_tests
    tests/module_interface_emitter_tests.cpp
    src/ModuleInterfaceEmitter.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(module_interface_emitter_tests)
add_test(NAME module_interface_emitter COMMAND module_interface_emitter_tests)

add_executable(bytecode_module_emitter_tests
    tests/bytecode_module_emitter_tests.cpp
    src/Bytecode.cpp
    src/BytecodeTextEmitter.cpp
    src/Value.cpp
)
compiler_design_configure_target(bytecode_module_emitter_tests)
add_test(NAME bytecode_module_emitter COMMAND bytecode_module_emitter_tests)

add_executable(module_cache_tests
    tests/module_cache_tests.cpp
    src/ModuleCache.cpp
    src/ModuleInterfaceArtifact.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(module_cache_tests)
add_test(NAME module_cache COMMAND module_cache_tests)

add_executable(module_interface_artifact_tests
    tests/module_interface_artifact_tests.cpp
    src/ModuleInterfaceArtifact.cpp
    src/ModuleCache.cpp
    src/TypeUtils.cpp
)
compiler_design_configure_target(module_interface_artifact_tests)
add_test(NAME module_interface_artifact COMMAND module_interface_artifact_tests)

set_tests_properties(
    cli_multi_source
    formatter_cli
    formatter_corpus
    golden
    bytecode_artifacts
    bytecode_module_artifacts
    bytecode_module_cache
    module_debug_metadata
    debugger
    profile
    rust_vm
    PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
