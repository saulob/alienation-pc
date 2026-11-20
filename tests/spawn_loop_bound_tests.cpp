// Issue #10: particle spawn loops (cs 0xe114750a and the 12 spawners in flight in alienation_144's tick 5979) keep
// appending to a spent GDS budget, millions of times per lane when the emitter count saturates. Hand-built IR of
// that loop, the production pass and an interpreter of the structured IR: no Vulkan, no window, no game files.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/object_pool.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/spawn_loop_bound.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/pipeline_cache_policy.h"

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

constexpr u64 Spawner106 = 0xe114750aULL;
constexpr u64 Spawner100 = 0xb7f2b0b1ULL;
constexpr u32 Budget = 4096;
constexpr u32 Lanes = 64;
constexpr u32 EmitterBuffer = 0;
constexpr u32 ParticleBuffer = 1;
constexpr u32 GdsCounter = 2;
constexpr u64 MaxAtomicsPerEmitter = Budget + 2 * Lanes;

enum class Variant {
    Exact,
    StoreOutsideGuard,
    NoBudgetTest,
    LiveOut,
    AppendInsideIf,
    AppendAddsTwo,
    AppendToGuestBuffer,
    IncrementFirst,
    IncrementBesideAppend,
    CounterConsumedElsewhere,
    VertexStage,
};

// The loop as the recompiler leaves cs 0xe114750a: count = int(18.849556 * r^2 * float(density)), 64 lanes split
// it, each appends once per index and writes a particle while the index is below 4096.
struct Spawner {
    Common::ObjectPool<IR::Inst> insts{1024};
    Common::ObjectPool<IR::Block> block_pool{32};
    Info info{};
    IR::Program program{info};
    IR::Inst* count{};
    IR::Inst* append{};
    IR::Inst* increment{};

    explicit Spawner(Variant variant = Variant::Exact) {
        info.hw_stage = variant == Variant::VertexStage ? HwStage::Vertex : HwStage::Compute;
        info.sw_stage = SwStage::Compute;
        info.buffers.push_back(BufferResource{.buffer_type = BufferType::Guest});
        info.buffers.push_back(BufferResource{.buffer_type = BufferType::Guest, .is_written = true});
        info.buffers.push_back(BufferResource{.buffer_type = BufferType::GdsBuffer, .is_written = true});
        Build(variant);
    }

    IR::Block* NewBlock() {
        return block_pool.Create(insts);
    }
    void Place(IR::Block* block) {
        program.blocks.push_back(block);
        program.syntax_list.push_back(Node{.data = {.block = block}, .type = Node::Type::Block});
    }
    static IR::Inst* NewPhi(IR::Block* block, IR::Type type) {
        IR::Inst* phi = &*block->PrependNewInst(block->end(), IR::Opcode::Phi);
        phi->SetFlags(type);
        return phi;
    }

    void Build(Variant variant) {
        IR::Block* pre = NewBlock();
        IR::Block* header = NewBlock();
        IR::Block* range = NewBlock();
        IR::Block* body = NewBlock();
        IR::Block* write = NewBlock();
        IR::Block* latch = NewBlock();
        IR::Block* cont = NewBlock();
        IR::Block* merge = NewBlock();

        IR::IREmitter p{*pre};
        const IR::U32 lane = p.GetAttributeU32(IR::Attribute::LocalInvocationId);
        const IR::F32 radius = p.BitCast<IR::F32, IR::U32>(
            IR::U32{p.LoadBufferU32(1, p.Imm32(EmitterBuffer), p.Imm32(3u), {})});
        const IR::U32 density{p.LoadBufferU32(1, p.Imm32(EmitterBuffer), p.Imm32(4u), {})};
        const IR::F32 area = p.FPMul(radius, radius);
        const IR::F32 scaled = p.FPMul(area, IR::F32{p.ConvertSToF(32, 32, density)});
        const IR::U32 total{p.ConvertFToS(32, p.FPMul(p.Imm32(18.849556f), scaled))};
        count = total.Inst();
        const IR::U32 per_lane{p.ShiftRightLogical(p.IAdd(total, p.Imm32(63u)), p.Imm32(6u))};
        const IR::U32 start{p.IMul(lane, per_lane)};
        const IR::U32 end{p.IAdd(per_lane, start)};
        const IR::U32 bound = p.SMin(total, end);
        if (variant == Variant::CounterConsumedElsewhere) {
            (void)p.BufferAtomicISub(p.Imm32(GdsCounter), p.Imm32(0u), p.Imm32(1u), {});
        }
        Place(pre);

        IR::Inst* index = NewPhi(header, IR::Type::U32);
        IR::Inst* active = NewPhi(header, IR::Type::U1);
        Place(header);

        Node loop{.type = Node::Type::Loop};
        loop.data.loop = {range, cont, merge};
        program.syntax_list.push_back(loop);

        IR::IREmitter r{*range};
        const IR::U1 in_range = r.ILessThan(IR::U32{IR::Value{index}}, bound, true);
        const IR::U1 still = r.LogicalAnd(IR::U1{IR::Value{active}}, in_range);
        const IR::U1 leave = r.ConditionRef(r.LogicalNot(still));
        std::optional<IR::U32> early_next;
        if (variant == Variant::IncrementFirst) {
            early_next = IR::U32{r.IAdd(IR::U32{IR::Value{index}}, r.Imm32(1u))};
        }
        Place(range);
        Node brk{.type = Node::Type::Break};
        brk.data.break_node = {leave, merge, body};
        program.syntax_list.push_back(brk);

        std::optional<IR::IREmitter> b;
        b.emplace(*body);
        IR::U1 guard;
        IR::U32 slot;
        const auto emit_append = [&](IR::IREmitter& ir) {
            const u32 handle = variant == Variant::AppendToGuestBuffer ? ParticleBuffer : GdsCounter;
            const u32 amount = variant == Variant::AppendAddsTwo ? 2u : 1u;
            slot = IR::U32{ir.BufferAtomicIAdd(ir.Imm32(handle), ir.Imm32(0u), ir.Imm32(amount), {})};
            append = slot.Inst();
        };
        if (variant == Variant::AppendInsideIf) {
            const IR::U1 wrap = b->ConditionRef(still);
            Place(body);
            Node open{.type = Node::Type::If};
            IR::Block* inner = NewBlock();
            IR::Block* after = NewBlock();
            open.data.if_node = {wrap, inner, after};
            program.syntax_list.push_back(open);
            IR::IREmitter in{*inner};
            emit_append(in);
            Place(inner);
            Node close{.type = Node::Type::EndIf};
            close.data.end_if.merge = after;
            program.syntax_list.push_back(close);
            Place(after);
            b.emplace(*after);
        } else {
            emit_append(*b);
            if (variant == Variant::IncrementBesideAppend) {
                early_next = IR::U32{b->IAdd(IR::U32{IR::Value{index}}, b->Imm32(1u))};
            }
        }
        const IR::U1 under_budget = variant == Variant::NoBudgetTest
                                        ? b->INotEqual(slot, b->Imm32(0xFFFFFFFFu))
                                        : b->IGreaterThan(b->Imm32(Budget), slot, false);
        guard = b->ConditionRef(b->LogicalAnd(still, under_budget));
        if (variant == Variant::StoreOutsideGuard) {
            b->StoreBufferU32(1, b->Imm32(ParticleBuffer), b->Imm32(0x100000u), IR::Value{index}, {});
        }
        if (variant != Variant::AppendInsideIf) {
            Place(body);
        }

        Node open{.type = Node::Type::If};
        open.data.if_node = {guard, write, latch};
        program.syntax_list.push_back(open);
        IR::IREmitter w{*write};
        w.StoreBufferU32(1, w.Imm32(ParticleBuffer), slot, IR::Value{index}, {});
        Place(write);
        Node close{.type = Node::Type::EndIf};
        close.data.end_if.merge = latch;
        program.syntax_list.push_back(close);

        IR::IREmitter l{*latch};
        const IR::U32 next =
            early_next ? *early_next : IR::U32{l.IAdd(IR::U32{IR::Value{index}}, l.Imm32(1u))};
        increment = next.Inst();
        Place(latch);

        IR::IREmitter c{*cont};
        const IR::U1 again = c.ConditionRef(c.Imm1(true));
        Place(cont);
        Node repeat{.type = Node::Type::Repeat};
        repeat.data.repeat = {again, header, merge};
        program.syntax_list.push_back(repeat);

        IR::IREmitter m{*merge};
        if (variant == Variant::LiveOut) {
            m.StoreBufferU32(1, m.Imm32(ParticleBuffer), m.Imm32(0x200000u), IR::Value{index}, {});
        }
        Place(merge);

        index->AddPhiOperand(pre, start);
        index->AddPhiOperand(cont, next);
        active->AddPhiOperand(pre, IR::Value{true});
        active->AddPhiOperand(cont, still);
    }

    size_t InstCount() const {
        size_t n = 0;
        for (const IR::Block* block : program.blocks) {
            n += std::distance(block->begin(), block->end());
        }
        return n;
    }
};

// Executes the structured IR for one invocation. Atomics are executed in program order across the invocations
// run on one Machine, like lanes serialised on the GDS counter.
struct Machine {
    const IR::Program& program;
    std::vector<u32> emitter = std::vector<u32>(16, 0);
    u32 counter = 0;
    u64 atomics = 0;
    u64 lane_atomics = 0;
    u32 last_count = 0;
    std::vector<std::pair<u32, u32>> writes;
    std::vector<std::pair<u32, u32>> other_writes;
    u64 iteration_limit = 1u << 16;
    std::string error;

    explicit Machine(const IR::Program& p) : program{p} {}

    static float F(u32 bits) {
        return std::bit_cast<float>(bits);
    }
    static u32 U(float value) {
        return std::bit_cast<u32>(value);
    }
    // v_cvt_i32_f32: NaN is 0, out-of-range values saturate.
    static u32 CvtI32(float value) {
        if (std::isnan(value)) {
            return 0;
        }
        if (value >= 2147483648.0f) {
            return 0x7FFFFFFFu;
        }
        if (value <= -2147483648.0f) {
            return 0x80000000u;
        }
        return u32(s32(value));
    }

    u32 Get(const std::unordered_map<const IR::Inst*, u32>& values, const IR::Value& v) const {
        if (v.IsImmediate()) {
            switch (v.Type()) {
            case IR::Type::U1:
                return v.U1() ? 1 : 0;
            case IR::Type::U32:
                return v.U32();
            case IR::Type::F32:
                return U(v.F32());
            default:
                return 0;
            }
        }
        const auto it = values.find(v.Inst());
        return it == values.end() ? 0 : it->second;
    }

    bool Exec(const IR::Block* block, const IR::Block* pred, u32 lane,
              std::unordered_map<const IR::Inst*, u32>& values) {
        std::vector<std::pair<const IR::Inst*, u32>> phis;
        for (const IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::Phi) {
                continue;
            }
            bool found = false;
            for (size_t k = 0; k < inst.NumArgs(); ++k) {
                if (inst.PhiBlock(k) == pred) {
                    phis.emplace_back(&inst, Get(values, inst.Arg(k)));
                    found = true;
                }
            }
            if (!found) {
                error = "phi without predecessor";
                return false;
            }
        }
        for (const auto& [phi, value] : phis) {
            values[phi] = value;
        }
        for (const IR::Inst& inst : *block) {
            const auto a = [&](size_t i) { return Get(values, inst.Arg(i)); };
            u32 out = 0;
            switch (inst.GetOpcode()) {
            case IR::Opcode::Phi:
                continue;
            case IR::Opcode::GetAttributeU32:
                out = lane;
                break;
            case IR::Opcode::LoadBufferU32:
                out = emitter.at(a(1));
                break;
            case IR::Opcode::BitCastF32U32:
            case IR::Opcode::ConditionRef:
                out = a(0);
                break;
            case IR::Opcode::FPMul32:
                out = U(F(a(0)) * F(a(1)));
                break;
            case IR::Opcode::ConvertF32S32:
                out = U(float(s32(a(0))));
                break;
            case IR::Opcode::ConvertS32F32:
                out = CvtI32(F(a(0)));
                last_count = out;
                break;
            case IR::Opcode::IAdd32:
                out = a(0) + a(1);
                break;
            case IR::Opcode::ISub32:
                out = a(0) - a(1);
                break;
            case IR::Opcode::IMul32:
                out = a(0) * a(1);
                break;
            case IR::Opcode::ShiftRightLogical32:
                out = a(0) >> (a(1) & 31);
                break;
            case IR::Opcode::SMin32:
                out = u32(std::min(s32(a(0)), s32(a(1))));
                break;
            case IR::Opcode::SLessThan32:
                out = s32(a(0)) < s32(a(1));
                break;
            case IR::Opcode::UGreaterThan32:
                out = a(0) > a(1);
                break;
            case IR::Opcode::UGreaterThanEqual32:
                out = a(0) >= a(1);
                break;
            case IR::Opcode::INotEqual32:
                out = a(0) != a(1);
                break;
            case IR::Opcode::LogicalAnd:
                out = a(0) && a(1);
                break;
            case IR::Opcode::LogicalNot:
                out = !a(0);
                break;
            case IR::Opcode::SelectU32:
                out = a(0) ? a(1) : a(2);
                break;
            case IR::Opcode::BufferAtomicISub32:
                out = counter;
                counter -= a(2);
                break;
            case IR::Opcode::BufferAtomicIAdd32:
                out = counter;
                counter += a(2);
                ++atomics;
                ++lane_atomics;
                break;
            case IR::Opcode::StoreBufferU32:
                (a(1) < 0x100000u ? writes : other_writes).emplace_back(a(1), a(2));
                continue;
            default:
                error = std::string{"unsupported "} + std::string{IR::NameOf(inst.GetOpcode())};
                return false;
            }
            values[&inst] = out;
        }
        return true;
    }

    size_t Find(Node::Type type, const IR::Block* block, size_t from = 0) const {
        const auto& list = program.syntax_list;
        for (size_t n = from; n < list.size(); ++n) {
            if (list[n].type != type) {
                continue;
            }
            if (type == Node::Type::Block && list[n].data.block == block) {
                return n;
            }
            if (type == Node::Type::EndIf && list[n].data.end_if.merge == block) {
                return n;
            }
        }
        return list.size();
    }

    // False when the invocation does not finish within the iteration limit.
    bool Run(u32 lane) {
        const auto& list = program.syntax_list;
        std::unordered_map<const IR::Inst*, u32> values;
        const IR::Block* pred = nullptr;
        u64 iterations = 0;
        lane_atomics = 0;
        for (size_t pc = 0; pc < list.size();) {
            const Node& node = list[pc];
            switch (node.type) {
            case Node::Type::Block:
                if (!Exec(node.data.block, pred, lane, values)) {
                    return false;
                }
                pred = node.data.block;
                ++pc;
                break;
            case Node::Type::If:
                pc = Get(values, node.data.if_node.cond) ? pc + 1
                                                         : Find(Node::Type::EndIf, node.data.if_node.merge, pc) + 1;
                break;
            case Node::Type::Break:
                pc = Get(values, node.data.break_node.cond) ? Find(Node::Type::Block, node.data.break_node.merge)
                                                            : pc + 1;
                break;
            case Node::Type::Repeat:
                if (++iterations > iteration_limit) {
                    return false;
                }
                pc = Get(values, node.data.repeat.cond) ? Find(Node::Type::Block, node.data.repeat.loop_header)
                                                        : pc + 1;
                break;
            default:
                ++pc;
                break;
            }
        }
        return true;
    }

    // One emitter: its 64 lanes, serialised on the counter. False if any lane does not finish.
    bool RunEmitter(u64* max_lane_atomics = nullptr) {
        bool finished = true;
        for (u32 lane = 0; lane < Lanes; ++lane) {
            finished &= Run(lane);
            if (max_lane_atomics) {
                *max_lane_atomics = std::max(*max_lane_atomics, lane_atomics);
            }
        }
        return finished;
    }
};

void SetEmitter(Machine& m, float radius, s32 density) {
    m.emitter[3] = Machine::U(radius);
    m.emitter[4] = u32(density);
}

// The counter value the original loop leaves: every lane appends once per index of its range.
u32 OriginalIncrements(u32 count) {
    const u32 per_lane = (count + 63u) >> 6;
    u32 total = 0;
    for (u32 lane = 0; lane < Lanes; ++lane) {
        const s32 start = s32(lane * per_lane);
        const s32 end = std::min(s32(count), s32(per_lane + lane * per_lane));
        if (start < end) {
            total += u32(end - start);
        }
    }
    return total;
}

// Inputs whose emitter count is exactly `target` under the shader's float arithmetic.
std::optional<std::pair<float, s32>> InputsFor(u32 target) {
    Spawner probe;
    Machine m{probe.program};
    for (s32 density = 1; density < 200000; ++density) {
        float radius = std::sqrt(float(target) / (18.849556f * float(density)));
        for (int step = 0; step < 64; ++step) {
            SetEmitter(m, radius, density);
            m.iteration_limit = 0;
            m.Run(Lanes);
            if (m.last_count == target) {
                return std::pair{radius, density};
            }
            radius = std::nextafter(radius, m.last_count < target ? 1e30f : 0.0f);
        }
    }
    return std::nullopt;
}

struct Outcome {
    bool finished{};
    u32 counter{};
    u64 atomics{};
    u64 max_lane_atomics{};
    std::vector<std::pair<u32, u32>> writes;
};

Outcome RunOne(bool bounded, float radius, s32 density, u32 counter_start, u64 limit = 1u << 16,
               Variant variant = Variant::Exact) {
    Spawner s{variant};
    if (bounded) {
        Optimization::BoundSpawnAppendLoops(s.program);
    }
    Machine m{s.program};
    m.iteration_limit = limit;
    m.counter = counter_start;
    SetEmitter(m, radius, density);
    Outcome o;
    o.finished = m.RunEmitter(&o.max_lane_atomics);
    o.counter = m.counter;
    o.atomics = m.atomics;
    o.writes = m.writes;
    std::sort(o.writes.begin(), o.writes.end());
    return o;
}

u32 CountOf(float radius, s32 density) {
    Spawner s;
    Machine m{s.program};
    m.iteration_limit = 0;
    SetEmitter(m, radius, density);
    m.Run(Lanes);
    return m.last_count;
}

Info ComputeInfo(bool gds) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.buffers.push_back(BufferResource{.buffer_type = BufferType::Guest});
    if (gds) {
        info.buffers.push_back(BufferResource{.buffer_type = BufferType::GdsBuffer, .is_written = true});
    }
    return info;
}

// Any compute shader using GDS may be rewritten, whatever its hash; nothing else changes.
void TestRevision() {
    CHECK(Optimization::SpawnLoopBoundRevision(ComputeInfo(true)) == 2);
    CHECK(Optimization::SpawnLoopBoundRevision(ComputeInfo(false)) == 0);
    Info vertex = ComputeInfo(true);
    vertex.hw_stage = HwStage::Vertex;
    CHECK(Optimization::SpawnLoopBoundRevision(vertex) == 0);
    for (const u64 hash : {Spawner106, Spawner100, 0xc7ba9905ULL, 0x5f5d3b8dULL, 0x12345678ULL}) {
        Spawner s;
        s.info.pgm_hash = hash;
        CHECK(Optimization::BoundSpawnAppendLoops(s.program) == 1);
    }
}

// The normal log names every shader whose loop was bounded.
void TestNotice() {
    CHECK(Optimization::SpawnLoopBoundNotice(Spawner106, 1) == "Particle spawn loop bounded: cs 0xe114750a, 1 loop(s)");
    CHECK(Optimization::SpawnLoopBoundNotice(0x0d17f243, 2) == "Particle spawn loop bounded: cs 0x0d17f243, 2 loop(s)");
}

void TestPatternIsExact() {
    Spawner exact;
    CHECK(Optimization::BoundSpawnAppendLoops(exact.program) == 1);

    for (const Variant v : {Variant::StoreOutsideGuard, Variant::NoBudgetTest, Variant::LiveOut,
                            Variant::AppendInsideIf, Variant::AppendAddsTwo, Variant::AppendToGuestBuffer,
                            Variant::CounterConsumedElsewhere, Variant::VertexStage}) {
        Spawner s{v};
        const size_t before = s.InstCount();
        CHECK(Optimization::BoundSpawnAppendLoops(s.program) == 0);
        CHECK(s.InstCount() == before);
        CHECK(s.append->Arg(2).IsImmediate());
    }
}

// The compiled loop carries the bound: the append adds a header Phi that starts at 1, the index advances by it,
// and the Phi takes what the lane has left once an append index reaches the budget.
void TestBoundInIr() {
    Spawner s;
    CHECK(Optimization::BoundSpawnAppendLoops(s.program) == 1);
    const IR::Value step_value = s.append->Arg(2);
    CHECK(!step_value.IsImmediate());
    if (step_value.IsImmediate()) {
        return;
    }
    IR::Inst* step = step_value.Inst();
    CHECK(step->GetOpcode() == IR::Opcode::Phi);
    CHECK(step->NumArgs() == 2);
    CHECK(s.increment->Arg(1) == IR::Value{step});
    bool from_preheader_one = false;
    IR::Inst* next = nullptr;
    for (size_t k = 0; k < step->NumArgs(); ++k) {
        if (step->Arg(k).IsImmediate()) {
            from_preheader_one = step->Arg(k).U32() == 1;
        } else {
            next = step->Arg(k).Inst();
        }
    }
    CHECK(from_preheader_one);
    CHECK(next && next->GetOpcode() == IR::Opcode::SelectU32);
    if (!next) {
        return;
    }
    const IR::Inst* spent = next->Arg(0).TryInst();
    CHECK(spent && spent->GetOpcode() == IR::Opcode::UGreaterThanEqual32);
    CHECK(spent && spent->Arg(0) == IR::Value{s.append});
    CHECK(spent && spent->Arg(1).IsImmediate() && spent->Arg(1).U32() == Budget);
    const IR::Inst* rest = next->Arg(1).TryInst();
    CHECK(rest && rest->GetOpcode() == IR::Opcode::ISub32 && rest->Arg(1) == IR::Value{s.increment});
    CHECK(next->Arg(2).IsImmediate() && next->Arg(2).U32() == 1);
}

// Normal emitters: identical particles (index and slot), identical counter, nothing bounded.
void TestNormalCountsUnchanged() {
    std::vector<u32> targets{0, 1, 18, 63, 64, 65, 1000, 4095, 4096, 4097, 8192, 16384};
    for (const u32 target : targets) {
        const auto inputs = target == 0 ? std::optional{std::pair{0.0f, 5}} : InputsFor(target);
        CHECK(inputs.has_value());
        if (!inputs) {
            std::printf("  no inputs for count %u\n", target);
            continue;
        }
        CHECK(CountOf(inputs->first, inputs->second) == target);
        for (const u32 start : {0u, 1000u, Budget - target % Budget, Budget - 1, Budget, Budget + 7}) {
            const Outcome original = RunOne(false, inputs->first, inputs->second, start);
            const Outcome bounded = RunOne(true, inputs->first, inputs->second, start);
            CHECK(original.finished && bounded.finished);
            CHECK(bounded.writes == original.writes);
            CHECK(bounded.counter == original.counter);
            CHECK(original.counter == start + OriginalIncrements(target));
            if (start + target <= Budget) {
                CHECK(bounded.atomics == original.atomics);
            }
            CHECK(bounded.atomics <= std::min<u64>(original.atomics, u64(Budget - std::min(start, Budget)) + 2 * Lanes));
            if (bounded.writes != original.writes || bounded.counter != original.counter) {
                std::printf("  count %u start %u: writes %zu/%zu counter %u/%u\n", target, start,
                            bounded.writes.size(), original.writes.size(), bounded.counter, original.counter);
            }
        }
    }
}

// The index may advance in the append's own block (41f570d0) or before the append (71a55369, ef9e164c).
void TestIncrementPlacement(Variant variant) {
    Spawner s{variant};
    CHECK(Optimization::BoundSpawnAppendLoops(s.program) == 1);
    for (const u32 target : {18u, 4097u, 16384u}) {
        const auto inputs = InputsFor(target);
        CHECK(inputs.has_value());
        if (!inputs) {
            continue;
        }
        for (const u32 start : {0u, 4000u}) {
            const Outcome original = RunOne(false, inputs->first, inputs->second, start, 1u << 16,
                                            variant);
            const Outcome bounded = RunOne(true, inputs->first, inputs->second, start, 1u << 16, variant);
            CHECK(original.finished && bounded.finished);
            CHECK(bounded.writes == original.writes);
            CHECK(bounded.counter == original.counter);
        }
    }
    const Outcome saturated =
        RunOne(true, 1.0e6f, 1000, 0, MaxAtomicsPerEmitter, variant);
    CHECK(saturated.finished);
    CHECK(saturated.atomics <= MaxAtomicsPerEmitter);
    CHECK(saturated.counter == OriginalIncrements(0x7FFFFFFFu));
}

// Each particle index of the lane ranges is written once, at the slot its append returned.
void TestRangesAndSlots() {
    const auto inputs = InputsFor(3000);
    CHECK(inputs.has_value());
    if (!inputs) {
        return;
    }
    const Outcome o = RunOne(true, inputs->first, inputs->second, 0);
    CHECK(o.writes.size() == 3000);
    std::vector<u32> indices;
    for (size_t k = 0; k < o.writes.size(); ++k) {
        CHECK(o.writes[k].first == k);
        indices.push_back(o.writes[k].second);
    }
    std::sort(indices.begin(), indices.end());
    for (size_t k = 0; k < indices.size(); ++k) {
        CHECK(indices[k] == k);
    }
}

// Pathological emitters finish after a bounded number of appends, write exactly the budget and leave the counter
// where the original loop would.
void TestPathologicalBounded() {
    struct Case {
        const char* name;
        float radius;
        s32 density;
    };
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Case cases[] = {
        {"reporter saturation", 1.0e6f, 1000},
        {"near INT_MAX", 1.0f, 113923000},
        {"positive infinity radius", inf, 1},
        {"NaN radius", nan, 3},
        {"negative radius", -2.0e5f, 9},
        {"extreme density", 3.0f, std::numeric_limits<s32>::max()},
        {"negative density", 4.0f, -500},
        {"minimum density", 4.0f, std::numeric_limits<s32>::min()},
        {"zero density", 4.0f, 0},
    };
    for (const auto& c : cases) {
        const u32 count = CountOf(c.radius, c.density);
        const u32 expected_counter = OriginalIncrements(count);
        for (const u32 start : {0u, 4000u, Budget, 1000000u}) {
            const Outcome o = RunOne(true, c.radius, c.density, start, MaxAtomicsPerEmitter);
            CHECK(o.finished);
            CHECK(o.atomics <= MaxAtomicsPerEmitter);
            CHECK(o.max_lane_atomics <= Budget + 2);
            CHECK(o.counter == start + expected_counter);
            const u32 room = start < Budget ? std::min(Budget - start, expected_counter) : 0;
            CHECK(o.writes.size() == room);
            if (!o.finished || o.counter != start + expected_counter || o.writes.size() != room) {
                std::printf("  %s start %u: count %#x finished %d atomics %llu counter %u writes %zu\n", c.name,
                            start, count, o.finished, static_cast<unsigned long long>(o.atomics), o.counter,
                            o.writes.size());
            }
        }
    }
    CHECK(CountOf(1.0e6f, 1000) == 0x7FFFFFFFu);
    CHECK(CountOf(inf, 1) == 0x7FFFFFFFu);
    CHECK(CountOf(nan, 3) == 0);
    CHECK(OriginalIncrements(0x7FFFFFFFu) == 63u * 33554432u);

    // Without the bound the same emitter is still appending far past that limit.
    const Outcome original = RunOne(false, 1.0e6f, 1000, 0, 16 * MaxAtomicsPerEmitter);
    CHECK(!original.finished);
    CHECK(original.atomics > 16 * MaxAtomicsPerEmitter);
}

// A full 1024-lane workgroup of oversized emitters sharing one counter (kept below a counter wrap).
void TestWorkgroupBounded() {
    Spawner s;
    CHECK(Optimization::BoundSpawnAppendLoops(s.program) == 1);
    Machine m{s.program};
    m.iteration_limit = MaxAtomicsPerEmitter;
    bool finished = true;
    u64 expected = 0;
    for (u32 emitter = 0; emitter < 16; ++emitter) {
        const s32 density = 10000000 + s32(emitter);
        SetEmitter(m, 1.0f, density);
        expected += OriginalIncrements(CountOf(1.0f, density));
        finished &= m.RunEmitter();
    }
    CHECK(finished);
    CHECK(expected > 16ull * 100000000);
    CHECK(expected < (1ull << 32));
    CHECK(m.atomics <= Budget + 2ull * 1024);
    CHECK(m.writes.size() == Budget);
    CHECK(m.counter == u32(expected));
}

std::string ReadSource(const char* relative) {
    std::ifstream stream(std::filesystem::path{ALN_SOURCE_DIR} / relative, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

size_t Occurrences(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// SPIR-V cached before the bound has another store name, so neither the preload nor the background warm-up can
// reach it; both take the name from the shader's stored info.
void TestStaleCacheBypassed() {
    using PipelineCachePolicy::ShaderBinaryName;
    const auto gds = Optimization::SpawnLoopBoundRevision(ComputeInfo(true));
    for (const u64 hash : {Spawner106, Spawner100, 0xc7ba9905ULL}) {
        for (const size_t perm : {size_t{0}, size_t{1}}) {
            CHECK(ShaderBinaryName(hash, perm, gds) != ShaderBinaryName(hash, perm, 0));
            CHECK(ShaderBinaryName(hash, perm, gds) != ShaderBinaryName(hash, perm, 1));
        }
    }
    CHECK(ShaderBinaryName(Spawner106, 0, 0) == "0x00000000e114750a_0");
    CHECK(ShaderBinaryName(Spawner106, 0, gds) == "0x00000000e114750a_0_r2");
    CHECK(ShaderBinaryName(0x03813bb4ULL, 2, Optimization::SpawnLoopBoundRevision(ComputeInfo(false))) ==
          "0x0000000003813bb4_2");

    const std::string serialization =
        ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp");
    CHECK(Occurrences(serialization, "\"{:#018x}_{}\"") == 0);
    CHECK(Occurrences(serialization, "BlobType::ShaderBinary") == 3);
    CHECK(Occurrences(serialization, "SpirvBlobName(") == 4);
    CHECK(serialization.find("std::string SpirvBlobName(const Shader::Info& info, size_t perm_idx) {\n"
                             "    return PipelineCachePolicy::ShaderBinaryName(info.pgm_hash, perm_idx,\n"
                             "                                                 "
                             "Shader::Optimization::SpawnLoopBoundRevision(info));") != std::string::npos);
    CHECK(serialization.find("SpirvBlobName(info, perm_idx)") != std::string::npos);
    CHECK(serialization.find("SpirvBlobName(program->info, perm_idx)") != std::string::npos);
    CHECK(serialization.find("SpirvBlobName(stage->program->info, stage->perm_idx)") != std::string::npos);

    const std::string cache = ReadSource("gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp");
    CHECK(cache.find("RegisterShaderBinary(std::move(build->binary), build->info, build->permutation);") !=
          std::string::npos);
    CHECK(cache.find("RegisterShaderBinary(std::move(spv), info, perm_idx);") != std::string::npos);
}

// Every translation runs the pass and a bounded loop is printed to the normal log, not a hidden log level.
void TestRecompilerContract() {
    const std::string recompiler = ReadSource("gpu/shadps4/shader_recompiler/recompiler.cpp");
    CHECK(Occurrences(recompiler, "Optimization::BoundSpawnAppendLoops(program)") == 1);
    CHECK(recompiler.find("if (const u32 bounded = Optimization::BoundSpawnAppendLoops(program)) {\n"
                          "        std::printf(\"%s\\n\", Optimization::SpawnLoopBoundNotice(info.pgm_hash, "
                          "bounded).c_str());") != std::string::npos);
    CHECK(recompiler.find("IsBoundedSpawnLoopShader") == std::string::npos);
    const size_t pass = recompiler.find("BoundSpawnAppendLoops(program)");
    const size_t dce = recompiler.rfind("DeadCodeEliminationPass(program)");
    CHECK(pass != std::string::npos && dce != std::string::npos && dce < pass);
}

} // namespace

int main() {
    TestRevision();
    TestNotice();
    TestPatternIsExact();
    TestBoundInIr();
    TestNormalCountsUnchanged();
    TestRangesAndSlots();
    TestIncrementPlacement(Variant::IncrementBesideAppend);
    TestIncrementPlacement(Variant::IncrementFirst);
    TestPathologicalBounded();
    TestWorkgroupBounded();
    TestStaleCacheBypassed();
    TestRecompilerContract();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
