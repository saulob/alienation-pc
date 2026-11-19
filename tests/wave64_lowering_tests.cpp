// Wave64 Ballot lowering on a 32-wide host: the lowering adds workgroup barriers, so it must only run where the
// whole workgroup takes the same path. Hand-built IR, the production pass: no Vulkan, no window, no game files.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/gds_lane_offset.h"
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

    // A counter-bounded loop around `body`; the counter is tested at the end of each iteration.
    IR::Block* Loop(const std::function<void(IR::Block*)>& body) {
        IR::Block* merge = block_pool.Create(insts);
        Node loop{.type = Node::Type::Loop};
        loop.data.loop.merge = merge;
        program.syntax_list.push_back(loop);
        IR::Block* inner = Block();
        body(inner);
        IR::IREmitter ir{*inner};
        const IR::U1 again = ir.ILessThan(ir.GetAttributeU32(IR::Attribute::WorkgroupId), ir.Imm32(4u), false);
        Node repeat{.type = Node::Type::Repeat};
        repeat.data.repeat = {again, inner, merge};
        program.syntax_list.push_back(repeat);
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

// The GDS append idiom of the ALN-NKBTJV compute shaders (cs 0x91516e37, 0x8c0c9537, 0x1016fb25, 0x7d39f28a): the
// lane rank from ballot(exec) is added to a ds_append result, under branches on the invocation id, once inside a
// counter-bounded loop. The rank also seeds other data, so the Ballot stays alive.
struct AppendIdiom {
    IR::Inst* ballot;
    IR::Inst* append;
    IR::Inst* index;
};

AppendIdiom EmitAppendIdiom(IR::Block* block) {
    IR::IREmitter ir{*block};
    const IR::U1 exec = ir.ILessThan(ir.GetAttributeU32(IR::Attribute::LocalInvocationId), ir.Imm32(48u), false);
    const IR::U64 mask{ir.Ballot(exec)};
    const IR::Value parts{ir.UnpackUint2x32(mask)};
    const IR::U32 high{ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 1)}, ir.Imm32(0u), true)};
    const IR::U32 append{ir.DataAppend(ir.Imm32(4u))};
    append.Inst()->SetArg(1, exec);
    const IR::U32 rank{ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 0)}, high, false)};
    const IR::U32 index{ir.IAdd(append, rank)};
    ir.WriteShared(32, index, ir.Imm32(0u));
    ir.WriteShared(32, ir.IMul(rank, ir.Imm32(0x19660du)), ir.Imm32(4u));
    return {mask.Inst(), append.Inst(), index.Inst()};
}

bool DependsOnBallot(const IR::Value& value) {
    return !value.IsImmediate() &&
           IR::BreadthFirstSearch(value.Inst(), [](const IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::Ballot) {
                   return true;
               }
               return std::nullopt;
           }).value_or(false);
}

// ALN-NKBTJV: the shaders that warned "left subgroup-wide" before the device loss. On a 32-wide host that Ballot sees
// only its own half of the wave, so it must neither get a workgroup barrier (only part of the workgroup is in the
// branch) nor reach the append index (lanes 32-63 would take the slots of lanes 0-31).
void ReportAppendIdiomStaysSafe() {
    ComputeShader shader;
    const auto lane_branch = [](IR::IREmitter& ir) {
        return ir.ILessThan(ir.GetAttributeU32(IR::Attribute::LocalInvocationId), ir.Imm32(60u), false);
    };
    std::vector<AppendIdiom> idioms;
    std::vector<IR::Block*> bodies;
    IR::Block* entry = shader.Block();
    shader.If(entry, lane_branch, [&](IR::Block* outer) {
        shader.If(outer, lane_branch, [&](IR::Block* inner) {
            idioms.push_back(EmitAppendIdiom(inner));
            bodies.push_back(inner);
            bodies.push_back(shader.Loop([&](IR::Block* loop_body) { idioms.push_back(EmitAppendIdiom(loop_body)); }));
        });
    });

    for (const AppendIdiom& idiom : idioms) {
        Optimization::RemoveAppendBufferLaneOffset(*idiom.append);
    }
    shader.Lower();

    CHECK(idioms.size() == 2);
    CHECK(Barriers(shader) == 0);
    for (const AppendIdiom& idiom : idioms) {
        CHECK(idiom.ballot->GetOpcode() == IR::Opcode::Ballot);
        CHECK(!DependsOnBallot(IR::Value{idiom.index}));
        CHECK(idiom.index->Arg(0) == IR::Value{idiom.append});
    }
    for (const IR::Block* body : bodies) {
        CHECK(Ballots(body) == 1);
    }
    const auto repeat = std::ranges::find_if(shader.program.syntax_list,
                                             [](const Node& node) { return node.type == Node::Type::Repeat; });
    CHECK(repeat != shader.program.syntax_list.end() && !DependsOnBallot(repeat->data.repeat.cond));
}

} // namespace

int main() {
    UniformBallotIsLowered();
    UniformConditionsStillLower();
    DivergentConditionsKeepBarriersOut();
    NestedDivergence();
    ReportAppendIdiomStaysSafe();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
