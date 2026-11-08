// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <string>
#include "common/assert.h"
#include "common/shader_crash_context.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/passes/gds_lane_offset.h"

namespace Shader::Optimization {
namespace {

constexpr size_t MaxReportedUsers = 8;

std::string DescribeInst(const IR::Inst* inst, int depth);

std::string Describe(const IR::Value& value, int depth) {
    if (value.IsEmpty()) {
        return "empty";
    }
    if (!value.IsImmediate()) {
        return DescribeInst(value.Inst(), depth);
    }
    switch (value.Type()) {
    case IR::Type::U32:
        return fmt::format("{:#x}", value.U32());
    case IR::Type::U1:
        return value.U1() ? "true" : "false";
    default:
        return "imm";
    }
}

std::string DescribeInst(const IR::Inst* inst, int depth) {
    std::string text{IR::NameOf(inst->GetOpcode())};
    if (depth > 0) {
        text += '(';
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            text += fmt::format("{}{}", i ? ", " : "", Describe(inst->Arg(i), depth - 1));
        }
        text += ')';
    }
    return text;
}

size_t IndexInBlock(const IR::Inst& target) {
    size_t index = 0;
    for (const IR::Inst& inst : *target.GetParent()) {
        if (&inst == &target) {
            break;
        }
        ++index;
    }
    return index;
}

[[noreturn]] void Fail(const char* reason, const IR::Inst& vx, const IR::Inst* user = nullptr) {
    std::string users;
    size_t count = 0;
    for (const auto& use : vx.Uses()) {
        if (count++ == MaxReportedUsers) {
            users += " ...";
            break;
        }
        users += fmt::format(" [{} operand {}]", DescribeInst(use.user, 1), use.operand);
    }
    Common::ShaderCrash::SetDetail(fmt::format(
        "{}; {} at block instruction {}, args {}; {} users:{}; offending user {}", reason,
        IR::NameOf(vx.GetOpcode()), IndexInBlock(vx), DescribeInst(&vx, 1), vx.Uses().size(),
        users, user ? DescribeInst(user, 1) : "none"));
    UNREACHABLE_MSG("{}", reason);
}

IR::Value IsMbcntWithExec(IR::Value value, bool is_hi, IR::U1 exec) {
    if (value.IsImmediate()) {
        return {};
    }
    const IR::Inst* inst = value.Inst();
    if (inst->GetOpcode() != IR::Opcode::MaskedBitCount32 || inst->Arg(0).IsImmediate() ||
        inst->Arg(2).U1() != is_hi) {
        return {};
    }
    IR::Inst* prod = inst->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::CompositeExtractU32x2 || prod->Arg(0).IsImmediate() ||
        prod->Arg(1).U32() != is_hi) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::UnpackUint2x32 || prod->Arg(0).IsImmediate()) {
        return {};
    }
    prod = prod->Arg(0).Inst();
    if (prod->GetOpcode() != IR::Opcode::Ballot || (prod->Arg(0) != exec && !is_hi)) {
        return {};
    }
    return inst->Arg(1);
}

bool IsAddOrSub(const IR::Inst* inst) {
    return inst->GetOpcode() == IR::Opcode::IAdd32 || inst->GetOpcode() == IR::Opcode::ISub32;
}

IR::Use FindUniqueUser(const IR::Inst& vx, IR::Inst* inst, auto&& pred) {
    IR::Use picked{};
    for (auto& use : inst->Uses()) {
        if (pred(use.user)) {
            if (picked.user) {
                Fail("Ambiguous lane offset chain", vx, use.user);
            }
            picked = use;
        }
    }
    if (!picked.user) {
        Fail("Lane offset chain has no add or subtract user", vx, inst);
    }
    return picked;
}

// x + 0 and x - 0 only pass the value through; a lane offset must follow in the chain.
bool IsIdentityOffset(const IR::Use& use, const IR::Value& other) {
    return other.IsImmediate() && other.U32() == 0u &&
           (use.user->GetOpcode() == IR::Opcode::IAdd32 || use.operand == 0);
}

} // namespace

void RemoveAppendBufferLaneOffset(IR::Inst& vx) {
    // ds_append       vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, vX
    // v_mbcnt_lo_u32_b32 idx, exec_lo, vY
    //
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // ds_append       vX gds
    // v_add_i32       idx, vcc, vX, vZ
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_sub_i32       vK, vcc, vX, vZ
    // v_subrev_i32    idx, vcc, 1, vK / v_add_i32    idx, vcc, -1, vK
    //
    // ds_consume      vX gds
    // v_mbcnt_hi_u32_b32 vY, exec_hi, 0
    // v_mbcnt_lo_u32_b32 vZ, exec_lo, vY
    // v_add_i32       vK, vcc, -1, vX
    // v_sub_i32       idx, vcc, vK, vZ
    //
    // The lane rank vZ may be shared by several GDS operations and other code, so only the
    // operand of the matched user is rewritten.
    IR::U1 exec{vx.Arg(1)};
    const bool is_consume = vx.GetOpcode() == IR::Opcode::DataConsume;
    const auto uses = vx.Uses();
    for (auto use : uses) {
        const auto& [user, operand] = use;
        if (user->GetOpcode() == IR::Opcode::MaskedBitCount32) {
            if (vx.GetOpcode() != IR::Opcode::DataAppend) {
                Fail("Lane count applied directly to a DataConsume result", vx, user);
            }
            IR::Inst* vy = user;
            auto vx_2 = IsMbcntWithExec(IR::Value{vy}, true, exec);
            if (vx_2.IsEmpty() || vx_2 != IR::Value{&vx}) {
                Fail("Unrecognized high lane count on DataAppend result", vx, user);
            }
            const auto [idx, idx_operand] = FindUniqueUser(vx, vy, [](const IR::Inst* inst) {
                return inst->GetOpcode() == IR::Opcode::MaskedBitCount32;
            });
            auto vy_2 = IsMbcntWithExec(IR::Value{idx}, false, exec);
            if (vy_2.IsEmpty() || vy_2 != IR::Value{vy}) {
                Fail("Unrecognized low lane count on DataAppend result", vx, idx);
            }
            idx->ReplaceUsesWithAndRemove(IR::Value{&vx});
            continue;
        }
        if (!IsAddOrSub(user)) {
            continue;
        }
        const auto other = user->Arg(1 - operand);
        if (other.IsImmediate()) {
            const bool allowed = other.U32() == 1u ||
                                 (other.U32() == std::numeric_limits<u32>::max() && is_consume) ||
                                 IsIdentityOffset(use, other);
            if (!allowed) {
                Fail("Unexpected constant offset to GDS counter result", vx, user);
            }
            use = FindUniqueUser(vx, user, IsAddOrSub);
        }

        auto vz = user->Arg(1 - operand);
        auto vy = IsMbcntWithExec(vz, false, exec);
        if (vy.IsEmpty() || vy.IsImmediate()) {
            Fail("Operand is not a lane rank", vx, user);
        }
        auto zero_const = IsMbcntWithExec(vy, true, exec);
        if (zero_const.IsEmpty() || !zero_const.IsImmediate() || zero_const.U32() != 0u) {
            Fail("Lane rank does not start from zero", vx, user);
        }
        user->SetArg(1 - operand, IR::Value{0u});
    }
}

} // namespace Shader::Optimization
