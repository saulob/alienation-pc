// GDS counter tests: the lane rank arithmetic around DataAppend/DataConsume results is removed from the matched
// operand only. Pure IR: no Vulkan, no window, no game files.
#include <cstdint>
#include <cstdio>

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/gds_lane_offset.h"

namespace {

using namespace Shader;

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

constexpr u32 MinusOne = 0xFFFFFFFFu;

struct Fixture {
    Common::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::IREmitter ir{block};
    IR::U1 exec{ir.GetExec()};

    IR::U32 Consume() {
        const IR::U32 result{ir.DataConsume(ir.Imm32(u32{0}))};
        result.Inst()->SetArg(1, exec);
        return result;
    }

    IR::U32 Append() {
        const IR::U32 result{ir.DataAppend(ir.Imm32(u32{0}))};
        result.Inst()->SetArg(1, exec);
        return result;
    }

    IR::Value Rank() {
        const IR::U64 ballot{ir.Ballot(exec)};
        const IR::Value parts{ir.UnpackUint2x32(ballot)};
        const IR::U32 hi{ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 1)}, ir.Imm32(u32{0}), true)};
        return ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 0)}, hi, false);
    }

    IR::Value AppendRankChain(const IR::U32& vx) {
        const IR::U64 ballot{ir.Ballot(exec)};
        const IR::Value parts{ir.UnpackUint2x32(ballot)};
        const IR::U32 hi{ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 1)}, vx, true)};
        return ir.MaskedBitCount(IR::U32{ir.CompositeExtract(parts, 0)}, hi, false);
    }
};

bool IsImm(const IR::Value& value, u32 expected) {
    return value.IsImmediate() && value.U32() == expected;
}

bool IsInst(const IR::Value& value, const IR::Value& expected) {
    return !value.IsImmediate() && value == expected;
}

void ConsumeSubRankThenMinusOne() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 vk{f.ir.ISub(vx, IR::U32{f.Rank()})};
    const IR::U32 idx{f.ir.IAdd(vk, f.ir.Imm32(MinusOne))};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(vk.Inst()->Arg(0), vx));
    CHECK(IsImm(vk.Inst()->Arg(1), 0u));
    CHECK(IsImm(idx.Inst()->Arg(1), MinusOne));
}

void ConsumeSubRankThenSubOne() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 vk{f.ir.ISub(vx, IR::U32{f.Rank()})};
    const IR::U32 idx{f.ir.ISub(vk, f.ir.Imm32(u32{1}))};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsImm(vk.Inst()->Arg(1), 0u));
    CHECK(IsImm(idx.Inst()->Arg(1), 1u));
}

void ConsumeMinusOneThenSubRank() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 vk{f.ir.IAdd(vx, f.ir.Imm32(MinusOne))};
    const IR::U32 idx{f.ir.ISub(vk, IR::U32{f.Rank()})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(idx.Inst()->Arg(0), vk));
    CHECK(IsImm(idx.Inst()->Arg(1), 0u));
}

void ConsumeZeroConstantPassesThrough() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 vk{f.ir.IAdd(vx, f.ir.Imm32(u32{0}))};
    const IR::U32 idx{f.ir.ISub(vk, IR::U32{f.Rank()})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsImm(vk.Inst()->Arg(1), 0u));
    CHECK(IsImm(idx.Inst()->Arg(1), 0u));
}

void ConsumeSubZeroThenRank() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 vk{f.ir.ISub(vx, f.ir.Imm32(u32{0}))};
    const IR::U32 idx{f.ir.ISub(vk, IR::U32{f.Rank()})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(idx.Inst()->Arg(0), vk));
    CHECK(IsImm(idx.Inst()->Arg(1), 0u));
}

void AppendAddRank() {
    Fixture f;
    const IR::U32 vx = f.Append();
    const IR::U32 idx{f.ir.IAdd(vx, IR::U32{f.Rank()})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(idx.Inst()->Arg(0), vx));
    CHECK(IsImm(idx.Inst()->Arg(1), 0u));
}

void AppendRankSeededWithResult() {
    Fixture f;
    const IR::U32 vx = f.Append();
    const IR::U32 lane{f.AppendRankChain(vx)};
    const IR::U32 use{f.ir.IMul(lane, f.ir.Imm32(u32{4}))};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(use.Inst()->Arg(0), vx));
}

void RankSharedByConsumeAndAppend() {
    Fixture f;
    const IR::Value rank = f.Rank();
    const IR::U32 consumed = f.Consume();
    const IR::U32 appended = f.Append();
    const IR::U32 consume_idx{f.ir.ISub(consumed, IR::U32{rank})};
    const IR::U32 append_idx{f.ir.IAdd(appended, IR::U32{rank})};
    const IR::U32 other_use{f.ir.IMul(IR::U32{rank}, f.ir.Imm32(u32{4}))};
    Optimization::RemoveAppendBufferLaneOffset(*consumed.Inst());
    Optimization::RemoveAppendBufferLaneOffset(*appended.Inst());
    CHECK(IsImm(consume_idx.Inst()->Arg(1), 0u));
    CHECK(IsImm(append_idx.Inst()->Arg(1), 0u));
    CHECK(IsInst(other_use.Inst()->Arg(0), rank));
}

void RankSharedByTwoConsumeUsers() {
    Fixture f;
    const IR::Value rank = f.Rank();
    const IR::U32 vx = f.Consume();
    const IR::U32 first{f.ir.ISub(vx, IR::U32{rank})};
    const IR::U32 second{f.ir.ISub(vx, IR::U32{rank})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsImm(first.Inst()->Arg(1), 0u));
    CHECK(IsImm(second.Inst()->Arg(1), 0u));
}

void UnrelatedUsersAreKept() {
    Fixture f;
    const IR::U32 vx = f.Consume();
    const IR::U32 scaled{f.ir.IMul(vx, f.ir.Imm32(u32{4}))};
    const IR::U32 idx{f.ir.ISub(vx, IR::U32{f.Rank()})};
    Optimization::RemoveAppendBufferLaneOffset(*vx.Inst());
    CHECK(IsInst(scaled.Inst()->Arg(0), vx));
    CHECK(IsImm(scaled.Inst()->Arg(1), 4u));
    CHECK(IsImm(idx.Inst()->Arg(1), 0u));
}

} // namespace

int main() {
    ConsumeSubRankThenMinusOne();
    ConsumeSubRankThenSubOne();
    ConsumeMinusOneThenSubRank();
    ConsumeZeroConstantPassesThrough();
    ConsumeSubZeroThenRank();
    AppendAddRank();
    AppendRankSeededWithResult();
    RankSharedByConsumeAndAppend();
    RankSharedByTwoConsumeUsers();
    UnrelatedUsersAreKept();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
