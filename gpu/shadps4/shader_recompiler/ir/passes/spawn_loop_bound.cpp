// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/spawn_loop_bound.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
namespace {

using Node = IR::AbstractSyntaxNode;

bool IsImm32(const IR::Value& value, u32 imm) {
    return value.IsImmediate() && value.Type() == IR::Type::U32 && value.U32() == imm;
}

IR::Inst* InstOf(const IR::Value& value) {
    return value.IsImmediate() ? nullptr : value.Inst();
}

bool Reaches(const IR::Value& from, const IR::Inst* target) {
    const IR::Inst* start = InstOf(from);
    return start && IR::BreadthFirstSearch(start, [target](const IR::Inst* inst) -> std::optional<bool> {
                        return inst == target ? std::optional{true} : std::nullopt;
                    }).value_or(false);
}

struct Loop {
    size_t begin{};  // Loop node
    size_t end{};    // Repeat node
    IR::Block* header{};
    std::unordered_set<const IR::Block*> blocks;
};

std::optional<Loop> LoopEndingAt(const IR::Program& program, size_t repeat) {
    const auto& list = program.syntax_list;
    int depth = 0;
    for (size_t i = repeat; i-- > 0;) {
        if (list[i].type == Node::Type::Repeat) {
            ++depth;
        } else if (list[i].type == Node::Type::Loop) {
            if (depth-- != 0) {
                continue;
            }
            Loop loop{.begin = i, .end = repeat, .header = list[repeat].data.repeat.loop_header};
            loop.blocks.insert(loop.header);
            for (size_t n = i + 1; n < repeat; ++n) {
                if (list[n].type == Node::Type::Block) {
                    loop.blocks.insert(list[n].data.block);
                }
            }
            return loop;
        }
    }
    return std::nullopt;
}

struct Candidate {
    IR::Inst* counter{};    // header Phi of the iteration index
    IR::Inst* increment{};  // IAdd32(counter, 1)
    IR::Value bound;        // loop-invariant end of the lane's range
    IR::Inst* append{};     // GDS BufferAtomicIAdd32 adding 1
    IR::Inst* budget_test{};
    u32 budget{};
    IR::Inst* last{};       // the later of append and increment: the bound goes after it
};

// The append index compared against an immediate budget, as `budget > idx` or `idx < budget`.
std::optional<std::pair<IR::Inst*, u32>> BudgetTest(IR::Inst* append) {
    std::vector<IR::Inst*> values{append};
    for (const auto& use : append->Uses()) {
        if (use.user->GetOpcode() == IR::Opcode::IAdd32 && IsImm32(use.user->Arg(1), 0)) {
            values.push_back(use.user);
        }
    }
    for (IR::Inst* value : values) {
        for (const auto& use : value->Uses()) {
            IR::Inst* test = use.user;
            if (test->GetOpcode() == IR::Opcode::UGreaterThan32 && use.operand == 1 &&
                test->Arg(0).IsImmediate()) {
                return std::pair{test, test->Arg(0).U32()};
            }
            if (test->GetOpcode() == IR::Opcode::ULessThan32 && use.operand == 0 &&
                test->Arg(1).IsImmediate()) {
                return std::pair{test, test->Arg(1).U32()};
            }
        }
    }
    return std::nullopt;
}

bool IsGdsHandle(const IR::Program& program, const IR::Value& handle) {
    return handle.IsImmediate() && handle.Type() == IR::Type::U32 &&
           handle.U32() < program.info.buffers.size() &&
           program.info.buffers[handle.U32()].buffer_type == BufferType::GdsBuffer;
}

bool IsGdsAppend(const IR::Program& program, const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::BufferAtomicIAdd32 && IsGdsHandle(program, inst.Arg(0)) &&
           IsImm32(inst.Arg(2), 1);
}

// A spent budget stays spent only while nothing in the dispatch lowers or overwrites a counter.
// Other dispatches are ordered against this one: they all write the one GDS buffer.
bool GdsCountersOnlyGrow(const IR::Program& program) {
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : *block) {
            if (inst.NumArgs() == 0 || !IsGdsHandle(program, inst.Arg(0)) ||
                std::string_view{IR::NameOf(inst.GetOpcode())}.find("Buffer") == std::string_view::npos) {
                continue;
            }
            if (!IsGdsAppend(program, inst)) {
                return false;
            }
        }
    }
    return true;
}

// Matches: header Phi i; break unless (... && i < bound); one unconditional GDS append per
// iteration; every other side effect guarded by `idx < budget`; i += 1; nothing leaves the loop.
std::optional<Candidate> Match(const IR::Program& program, const Loop& loop) {
    const auto& list = program.syntax_list;
    const auto in_loop = [&](const IR::Inst* inst) {
        return loop.blocks.contains(inst->GetParent());
    };

    Candidate c{};
    for (IR::Inst& phi : *loop.header) {
        if (phi.GetOpcode() != IR::Opcode::Phi || phi.NumArgs() != 2) {
            continue;
        }
        for (size_t k = 0; k < 2; ++k) {
            IR::Inst* inc = InstOf(phi.Arg(k));
            const IR::Inst* init = InstOf(phi.Arg(1 - k));
            if (inc && inc->GetOpcode() == IR::Opcode::IAdd32 && inc->Arg(0) == IR::Value{&phi} &&
                IsImm32(inc->Arg(1), 1) && in_loop(inc) && (!init || !in_loop(init))) {
                c.counter = &phi;
                c.increment = inc;
            }
        }
    }
    if (!c.counter) {
        return std::nullopt;
    }

    // The first control node of the body must be the range check.
    size_t first_break = 0;
    for (size_t n = loop.begin + 1; n < loop.end && !first_break; ++n) {
        if (list[n].type == Node::Type::Break) {
            first_break = n;
        } else if (list[n].type != Node::Type::Block) {
            return std::nullopt;
        }
    }
    if (!first_break) {
        return std::nullopt;
    }
    const IR::Inst* cond = InstOf(list[first_break].data.break_node.cond);
    if (!cond || cond->GetOpcode() != IR::Opcode::ConditionRef) {
        return std::nullopt;
    }
    const IR::Inst* negated = InstOf(cond->Arg(0));
    if (!negated || negated->GetOpcode() != IR::Opcode::LogicalNot) {
        return std::nullopt;
    }
    IR::Inst* range = InstOf(negated->Arg(0));
    if (range && range->GetOpcode() == IR::Opcode::LogicalAnd) {
        range = InstOf(range->Arg(1));
    }
    if (!range || range->GetOpcode() != IR::Opcode::SLessThan32 ||
        range->Arg(0) != IR::Value{c.counter}) {
        return std::nullopt;
    }
    c.bound = range->Arg(1);
    if (const IR::Inst* bound = InstOf(c.bound); bound && in_loop(bound)) {
        return std::nullopt;
    }

    // The append must run exactly once per iteration: at loop level, outside any If or inner loop.
    std::vector<IR::Value> guards;
    int nested_loops = 0;
    size_t append_node = 0;
    size_t increment_node = 0;
    for (size_t n = loop.begin + 1; n < loop.end; ++n) {
        const auto& node = list[n];
        switch (node.type) {
        case Node::Type::If:
            guards.push_back(node.data.if_node.cond);
            break;
        case Node::Type::EndIf:
            if (guards.empty()) {
                return std::nullopt;
            }
            guards.pop_back();
            break;
        case Node::Type::Loop:
            ++nested_loops;
            break;
        case Node::Type::Repeat:
            --nested_loops;
            break;
        case Node::Type::Break:
            if (n != first_break) {
                return std::nullopt;
            }
            break;
        case Node::Type::Block:
            if (guards.empty() && nested_loops == 0) {
                if (node.data.block == c.increment->GetParent()) {
                    increment_node = n;
                }
            }
            for (IR::Inst& inst : *node.data.block) {
                if (guards.empty() && nested_loops == 0 && IsGdsAppend(program, inst)) {
                    if (c.append) {
                        return std::nullopt;
                    }
                    c.append = &inst;
                    append_node = n;
                }
            }
            break;
        default:
            return std::nullopt;
        }
    }
    if (!c.append || increment_node == 0) {
        return std::nullopt;
    }
    c.last = increment_node > append_node ? c.increment : c.append;
    if (increment_node == append_node) {
        const auto& block = *c.append->GetParent();
        for (const IR::Inst& inst : block) {
            if (&inst == c.append || &inst == c.increment) {
                c.last = &inst == c.append ? c.increment : c.append;
                break;
            }
        }
    }
    const auto test = BudgetTest(c.append);
    if (!test || test->second == 0) {
        return std::nullopt;
    }
    c.budget_test = test->first;
    c.budget = test->second;

    // Every other side effect must sit under the budget test, so skipped iterations do nothing.
    guards.clear();
    for (size_t n = loop.begin + 1; n < loop.end; ++n) {
        const auto& node = list[n];
        if (node.type == Node::Type::If) {
            guards.push_back(node.data.if_node.cond);
        } else if (node.type == Node::Type::EndIf) {
            guards.pop_back();
        } else if (node.type == Node::Type::Block) {
            const bool guarded = std::ranges::any_of(
                guards, [&](const IR::Value& g) { return Reaches(g, c.budget_test); });
            for (IR::Inst& inst : *node.data.block) {
                if (&inst == c.append || inst.GetOpcode() == IR::Opcode::ConditionRef ||
                    !inst.MayHaveSideEffects()) {
                    continue;
                }
                if (!guarded) {
                    return std::nullopt;
                }
            }
        }
    }

    // Skipped iterations must not feed anything after the loop.
    for (const IR::Block* block : loop.blocks) {
        for (const IR::Inst& inst : *block) {
            for (const auto& use : inst.Uses()) {
                if (!in_loop(use.user)) {
                    return std::nullopt;
                }
            }
        }
    }
    return c;
}

void Bound(const Candidate& c) {
    IR::Block* header = c.counter->GetParent();
    IR::Inst* step = &*header->PrependNewInst(header->begin(), IR::Opcode::Phi);
    step->SetFlags(IR::Type::U32);

    c.append->SetArg(2, IR::Value{step});
    c.increment->SetArg(1, IR::Value{step});

    auto after = IR::Block::InstructionList::s_iterator_to(*c.last);
    IR::IREmitter ir{*c.last->GetParent(), ++after};
    const IR::U32 next{IR::Value{c.increment}};
    const IR::U1 spent = ir.IGreaterThanEqual(IR::U32{IR::Value{c.append}}, ir.Imm32(c.budget), false);
    const IR::U32 rest = ir.ISub(IR::U32{c.bound}, next);
    const IR::Value step_next = ir.Select(spent, rest, ir.Imm32(1u));

    for (size_t k = 0; k < c.counter->NumArgs(); ++k) {
        const bool from_body = c.counter->Arg(k) == IR::Value{c.increment};
        step->AddPhiOperand(c.counter->PhiBlock(k), from_body ? step_next : IR::Value{1u});
    }
}

} // namespace

std::string SpawnLoopBoundNotice(u64 pgm_hash, u32 loops) {
    return fmt::format("Particle spawn loop bounded: cs {:#010x}, {} loop(s)", pgm_hash, loops);
}

u32 SpawnLoopBoundRevision(const Info& info) {
    const bool uses_gds = std::ranges::any_of(info.buffers, [](const BufferResource& buffer) {
        return buffer.buffer_type == BufferType::GdsBuffer;
    });
    return info.hw_stage == HwStage::Compute && uses_gds ? 2u : 0u;
}

u32 BoundSpawnAppendLoops(IR::Program& program) {
    if (program.info.hw_stage != HwStage::Compute || SpawnLoopBoundRevision(program.info) == 0 ||
        !GdsCountersOnlyGrow(program)) {
        return 0;
    }
    std::vector<Candidate> candidates;
    for (size_t n = 0; n < program.syntax_list.size(); ++n) {
        if (program.syntax_list[n].type != Node::Type::Repeat) {
            continue;
        }
        if (const auto loop = LoopEndingAt(program, n)) {
            if (auto c = Match(program, *loop)) {
                candidates.push_back(*c);
            }
        }
    }
    for (const auto& c : candidates) {
        Bound(c);
    }
    return u32(candidates.size());
}

} // namespace Shader::Optimization
