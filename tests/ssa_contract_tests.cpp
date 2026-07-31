#include "SSA.hpp"

#include <cassert>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void assertThrowsLogic(const std::function<void()>& action, const std::string& fragment)
{
    try {
        action();
    } catch (const std::logic_error& error) {
        assert(std::string(error.what()).find(fragment) != std::string::npos);
        return;
    }
    assert(false && "expected logic_error");
}

void test_binding_metadata_is_explicit_and_conservative()
{
    const IRBinding local{BindingId{1}, "local#1", BindingStorageClass::Local};
    const IRBinding captured{BindingId{2}, "captured#2", BindingStorageClass::Captured};
    const IRBinding unknown{BindingId{3}, "unknown#3", BindingStorageClass::Unknown};

    assert(local.canPromote());
    assert(!captured.canPromote());
    assert(!unknown.canPromote());

    const SSAMemorySlot slot{
        0,
        local.resolvedName,
        SSAMemoryStorage::Local};
    assert(slot.canPromote());
}

void test_ir_program_keeps_binding_tables_outside_printed_ir()
{
    IRProgram program;
    program.addBinding(IRBinding{BindingId{10}, "top#10", BindingStorageClass::Module});
    assert(program.bindings().size() == 1);
    assert(program.bindings().front().bindingId == BindingId{10});

    program.beginFunction("worker", {});
    program.addBinding(IRBinding{BindingId{11}, "local#11", BindingStorageClass::Local});
    const IRRegister loaded = program.emitLoadVar("local#11", BindingId{11});
    program.emitReturn(loaded);
    const std::size_t functionIndex = program.endFunction();

    assert(program.functions()[functionIndex].bindings.size() == 1);
    assert(program.functions()[functionIndex].bindings.front().canPromote());
    assert(program.functions()[functionIndex].instructions.front().bindingId
        == std::optional<BindingId>(BindingId{11}));

    std::ostringstream printed;
    program.print(printed);
    assert(printed.str().find("load_var") != std::string::npos);
    assert(printed.str().find("bindings") == std::string::npos);
}

void test_binding_metadata_rejects_missing_and_duplicate_ids()
{
    IRProgram program;
    assertThrowsLogic(
        [&program] {
            program.addBinding(IRBinding{BindingId{}, "missing", BindingStorageClass::Local});
        },
        "valid binding ID");
    assertThrowsLogic(
        [&program] {
            program.addBinding(IRBinding{BindingId{1}, "", BindingStorageClass::Local});
        },
        "resolved name");

    program.addBinding(IRBinding{BindingId{1}, "one#1", BindingStorageClass::Local});
    assertThrowsLogic(
        [&program] {
            program.addBinding(IRBinding{BindingId{1}, "again#1", BindingStorageClass::Local});
        },
        "duplicate");

    program.beginFunction("worker", {});
    assertThrowsLogic(
        [&program] {
            program.addBinding(IRBinding{BindingId{1}, "again-in-function#1", BindingStorageClass::Local});
        },
        "duplicate");
    program.endFunction();
}

void test_effect_summary_is_conservative_and_deterministic()
{
    const IREffectSummary constant = irEffectSummary(IROp::Constant);
    assert(constant.isPure());

    const IREffectSummary load = irEffectSummary(IROp::LoadVar);
    assert(load.readsMemory);
    assert(!load.writesMemory);
    assert(!load.isPure());

    const IREffectSummary assign = irEffectSummary(IROp::AssignIndex);
    assert(assign.readsMemory);
    assert(assign.writesMemory);
    assert(assign.mayTrap);

    const IREffectSummary call = irEffectSummary(IROp::Call);
    assert(call.readsMemory);
    assert(call.writesMemory);
    assert(call.calls);
    assert(call.mayTrap);

    const IREffectSummary allocation = irEffectSummary(IROp::Array);
    assert(allocation.allocates);
    assert(!allocation.isPure());

    const IREffectSummary print = irEffectSummary(IROp::Print);
    assert(print.observable);

    const IREffectSummary branch = irEffectSummary(IROp::JumpIfFalse);
    assert(branch.controlFlow);
}

} // namespace

int main()
{
    test_binding_metadata_is_explicit_and_conservative();
    test_ir_program_keeps_binding_tables_outside_printed_ir();
    test_binding_metadata_rejects_missing_and_duplicate_ids();
    test_effect_summary_is_conservative_and_deterministic();
    return 0;
}
