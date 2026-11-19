// Wave64 Ballot lowering on a 32-wide host: the lowering adds workgroup barriers, so it must only run where the
// whole workgroup takes the same path. Hand-built IR, the production pass: no Vulkan, no window, no game files.
#include <cstdio>
#include <functional>

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace {

using namespace Shader;
using Node = IR::AbstractSyntaxNode;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(condition)) {                                                                                    \
            ++g_failures;                                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                  \
        }                                                                                                      \
    } while (0)

constexpr u32 ReadOnlyBuffer = 0;
constexpr u32 WrittenBuffer = 1;

struct ComputeShader {
    Common::ObjectPool<IR::Inst> insts{1024};
    Common::ObjectPool<IR::Block> block_pool{16};
    Info info{};
    RuntimeInfo runtime{};
    Profile profile{};
    IR::Program program{info};

    ComputeShader() {
        info.hw_stage = HwStage::Compute;
        info.sw_stage = SwStage::Compute;
        info.buffers.push_back(BufferResource{.is_written = false});
        info.buffers.push_back(BufferResource{.is_written = true});
        runtime.Initialize(HwStage::Compute, SwStage::Compute);
        runtime.hw.cs.workgroup_size = {64, 1, 1};
        profile.subgroup_size = 32;
    }

    IR::Block* Block() {
        IR::Block* block = block_pool.Create(insts);
        program.blocks.push_back(block);
        program.syntax_list.push_back(Node{.data = {.block = block}, .type = Node::Type::Block});
        return block;
    }

    // `cond` is built in `from`; the body runs `body` and returns the body block.
    IR::Block* If(IR::Block* from, const std::function<IR::U1(IR::IREmitter&)>& cond,
                  const std::function<void(IR::Block*)>& body) {
        IR::IREmitter ir{*from};
        const IR::U1 condition = cond(ir);
        IR::Block* merge = block_pool.Create(insts);
        Node if_node{.type = Node::Type::If};
        if_node.data.if_node = {condition, nullptr, merge};
        program.syntax_list.push_back(if_node);
        IR::Block* inner = Block();
        body(inner);
        Node end{.type = Node::Type::EndIf};
        end.data.end_if.merge = merge;
        program.syntax_list.push_back(end);
        program.blocks.push_back(merge);
        program.syntax_list.push_back(Node{.data = {.block = merge}, .type = Node::Type::Block});
        return inner;
    }

    static IR::Inst* Ballot(IR::Block* block) {
        IR::IREmitter ir{*block};
        const IR::U64 mask = ir.Ballot(ir.IEqual(ir.GetAttributeU32(IR::Attribute::LocalInvocationId), ir.Imm32(0u)));
        const IR::Value unpacked = ir.UnpackUint2x32(mask);
        ir.WriteShared(32, ir.CompositeExtract(unpacked, 1), ir.Imm32(0u));
        return mask.Inst();
    }

    void Lower() {
        Optimization::LowerWave64BallotPass(program, runtime, profile);
    }
};

size_t Barriers(const IR::Block* block) {
    size_t count = 0;
    for (const IR::Inst& inst : *block) {
        count += inst.GetOpcode() == IR::Opcode::Barrier;
    }
    return count;
}

size_t Barriers(const ComputeShader& shader) {
    size_t count = 0;
    for (const IR::Block* block : shader.program.blocks) {
        count += Barriers(block);
    }
    return count;
}

size_t Ballots(const IR::Block* block) {
    size_t count = 0;
    for (const IR::Inst& inst : *block) {
        count += inst.GetOpcode() == IR::Opcode::Ballot;
    }
    return count;
}

using Condition = std::function<IR::U1(IR::IREmitter&)>;

// A Ballot under `cond` keeps its subgroup-wide form and no barrier is added anywhere.
void StaysSubgroupWide(const char* name, const Condition& cond) {
    ComputeShader shader;
    IR::Block* entry = shader.Block();
    IR::Block* body = shader.If(entry, cond, [](IR::Block* inner) { ComputeShader::Ballot(inner); });
    shader.Lower();
    const bool ok = Barriers(shader) == 0 && Ballots(body) == 1;
    if (!ok) {
        std::printf("  %s: %zu barriers, %zu Ballot in the branch\n", name, Barriers(shader), Ballots(body));
    }
    CHECK(ok);
}

void UniformBallotIsLowered() {
    ComputeShader shader;
    IR::Block* entry = shader.Block();
    ComputeShader::Ballot(entry);
    shader.Lower();
    CHECK(Barriers(entry) == 2);
}

void UniformConditionsStillLower() {
    const Condition workgroup = [](IR::IREmitter& ir) {
        return ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId), ir.Imm32(0u));
    };
    const Condition read_only_load = [](IR::IREmitter& ir) {
        return ir.IEqual(IR::U32{ir.LoadBufferU32(1, ir.Imm32(ReadOnlyBuffer), ir.Imm32(0u), {})}, ir.Imm32(1u));
    };
    for (const Condition* cond : {&workgroup, &read_only_load}) {
        ComputeShader shader;
        IR::Block* entry = shader.Block();
        IR::Block* body = shader.If(entry, *cond, [](IR::Block* inner) { ComputeShader::Ballot(inner); });
        shader.Lower();
        CHECK(Barriers(body) == 2);
    }
}

// ALN-XXJJ7R: conditions that differ between the invocations of one workgroup. Before, only lane and invocation ids
// counted, so a Ballot under any of these got a workgroup barrier only part of the workgroup reaches (a GPU hang).
void DivergentConditionsKeepBarriersOut() {
    StaysSubgroupWide("local invocation id", [](IR::IREmitter& ir) {
        return ir.IEqual(ir.GetAttributeU32(IR::Attribute::LocalInvocationId), ir.Imm32(3u));
    });
    StaysSubgroupWide("per-lane buffer atomic", [](IR::IREmitter& ir) {
        const IR::U32 slot{ir.BufferAtomicIAdd(ir.Imm32(WrittenBuffer), ir.Imm32(0u), ir.Imm32(1u), {})};
        return ir.ILessThan(slot, ir.Imm32(256u), false);
    });
    StaysSubgroupWide("GDS append index", [](IR::IREmitter& ir) {
        const IR::U32 index = ir.DataAppend(ir.Imm32(0u));
        index.Inst()->SetArg(1, ir.Imm1(true));
        return ir.ILessThan(index, ir.Imm32(512u), false);
    });
    StaysSubgroupWide("shared atomic", [](IR::IREmitter& ir) {
        const IR::U32 count{ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false)};
        return ir.IEqual(count, ir.Imm32(0u));
    });
    StaysSubgroupWide("shared memory read", [](IR::IREmitter& ir) {
        return ir.IEqual(IR::U32{ir.LoadShared(32, false, ir.Imm32(0u))}, ir.Imm32(0u));
    });
    StaysSubgroupWide("wave-uniform value", [](IR::IREmitter& ir) {
        return ir.IEqual(ir.ReadFirstLane(ir.Imm32(7u)), ir.Imm32(7u));
    });
    StaysSubgroupWide("load from a written buffer", [](IR::IREmitter& ir) {
        return ir.IEqual(IR::U32{ir.LoadBufferU32(1, ir.Imm32(WrittenBuffer), ir.Imm32(0u), {})}, ir.Imm32(1u));
    });
}

// A uniform branch around a divergent one: only the Ballot outside the divergent branch is lowered.
void NestedDivergence() {
    ComputeShader shader;
    IR::Block* entry = shader.Block();
    IR::Block* inner = nullptr;
    IR::Block* outer = shader.If(
        entry,
        [](IR::IREmitter& ir) { return ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId), ir.Imm32(0u)); },
        [&](IR::Block* outer_body) {
            ComputeShader::Ballot(outer_body);
            inner = shader.If(
                outer_body,
                [](IR::IREmitter& ir) {
                    const IR::U32 slot{ir.BufferAtomicIAdd(ir.Imm32(WrittenBuffer), ir.Imm32(0u), ir.Imm32(1u), {})};
                    return ir.ILessThan(slot, ir.Imm32(64u), false);
                },
                [](IR::Block* inner_body) { ComputeShader::Ballot(inner_body); });
        });
    shader.Lower();
    CHECK(Barriers(outer) == 2);
    CHECK(inner && Barriers(inner) == 0 && Ballots(inner) == 1);
    CHECK(Barriers(shader) == 2);
}

} // namespace

int main() {
    UniformBallotIsLowered();
    UniformConditionsStillLower();
    DivergentConditionsKeepBarriersOut();
    NestedDivergence();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
