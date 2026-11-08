// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include "common/logging/classes.h"
#include "common/shader_crash_context.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    std::optional<Common::ShaderCrash::Scope> crash_scope;
    if (!Common::ShaderCrash::Active()) {
        crash_scope.emplace(fmt::format("{}", info.hw_stage), fmt::format("{}", info.sw_stage),
                            info.pgm_hash, 0);
    }

    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }

    // Clear any previous pooled data.
    pools.ReleaseContents();

    Common::ShaderCrash::SetPass("ControlFlowGraph");
    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    Shader::InjectClipDistanceAttributes(program, runtime_info);

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        Common::ShaderCrash::SetPass("LowerFp64ToFp32");
        Shader::Optimization::LowerFp64ToFp32(program);
    }
    Common::ShaderCrash::SetPass("SsaRewritePass");
    Shader::Optimization::SsaRewritePass(program);
    Common::ShaderCrash::SetPass("ConstantPropagationPass");
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Common::ShaderCrash::SetPass("ReadLaneEliminationPass");
    Shader::Optimization::ReadLaneEliminationPass(program);
    if (info.sw_stage == SwStage::TessellationControl) {
        Common::ShaderCrash::SetPass("TessellationPreprocess");
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Common::ShaderCrash::SetPass("HullShaderTransform");
        Shader::Optimization::HullShaderTransform(program, runtime_info);
    } else if (info.sw_stage == SwStage::TessellationEval) {
        Common::ShaderCrash::SetPass("TessellationPreprocess");
        Shader::Optimization::TessellationPreprocess(program, runtime_info);
        Common::ShaderCrash::SetPass("DomainShaderTransform");
        Shader::Optimization::DomainShaderTransform(program, runtime_info);
    }
    Common::ShaderCrash::SetPass("RingAccessElimination");
    Shader::Optimization::RingAccessElimination(program, runtime_info);
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    Common::ShaderCrash::SetPass("FlattenExtendedUserdataPass");
    Shader::Optimization::FlattenExtendedUserdataPass(program);
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    Common::ShaderCrash::SetPass("ResourcePatchingPass");
    Shader::Optimization::ResourcePatchingPass(program.info, resources, profile);
    Common::ShaderCrash::SetPass("LowerBufferFormatToRaw");
    Shader::Optimization::LowerBufferFormatToRaw(program);
    Common::ShaderCrash::SetPass("SharedMemorySimplifyPass");
    Shader::Optimization::SharedMemorySimplifyPass(program, profile);
    Common::ShaderCrash::SetPass("SharedMemoryToStoragePass");
    Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile);
    Common::ShaderCrash::SetPass("LowerUserClipPlanes");
    Shader::Optimization::LowerUserClipPlanes(program, runtime_info);
    Common::ShaderCrash::SetPass("PhiSimplificationPass");
    Shader::Optimization::PhiSimplificationPass(program);
    Common::ShaderCrash::SetPass("InverseBallotEliminationPass");
    Shader::Optimization::InverseBallotEliminationPass(program);
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    Common::ShaderCrash::SetPass("LowerPhisToRegsPass");
    Shader::Optimization::LowerPhisToRegsPass(program);

    Common::ShaderCrash::SetPass("Structurize");
    // Structurize control flow graph and create program.
    program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info);
    program.blocks = GenerateBlocks(program.syntax_list);
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);

    // Run optimization passes on structured graph
    Common::ShaderCrash::SetPass("SsaRepairPass");
    Shader::Optimization::SsaRepairPass(program);
    Common::ShaderCrash::SetPass("SsaRewritePass");
    Shader::Optimization::SsaRewritePass(program);
    Common::ShaderCrash::SetPass("SharedMemoryBarrierPass");
    Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile);
    Common::ShaderCrash::SetPass("DeadCodeEliminationPass");
    Shader::Optimization::DeadCodeEliminationPass(program);
    Common::ShaderCrash::SetPass("LowerWave64BallotPass");
    Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile);
    Common::ShaderCrash::SetPass("LowerHardwareIntrinsics");
    Shader::Optimization::LowerHardwareIntrinsics(program);
    Common::ShaderCrash::SetPass("ConstantPropagationPass");
    Shader::Optimization::ConstantPropagationPass(program.post_order_blocks);
    Common::ShaderCrash::SetPass("DeadCodeEliminationPass");
    Shader::Optimization::DeadCodeEliminationPass(program);
    if (info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix) {
        Common::ShaderCrash::SetPass("InterpolatedIntegerPass");
        Shader::Optimization::InterpolatedIntegerPass(program);
    }
    Common::ShaderCrash::SetPass("CollectShaderInfoPass");
    Shader::Optimization::CollectShaderInfoPass(program, profile);
    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader
