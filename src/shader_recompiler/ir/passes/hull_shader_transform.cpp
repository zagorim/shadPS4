// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <any>
#include <numeric>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

// TODO delelte
#include "common/config.h"
#include "common/io_file.h"
#include "common/path_util.h"

namespace Shader::Optimization {

/**
 * Tessellation shaders pass outputs to the next shader using LDS.
 * The Hull shader stage receives input control points stored in LDS.
 *
 * The LDS layout is:
 * - TCS inputs for patch 0
 * - TCS inputs for patch 1
 * - TCS inputs for patch 2
 * - ...
 * - TCS outputs for patch 0
 * - TCS outputs for patch 1
 * - TCS outputs for patch 2
 * - ...
 * - Per-patch TCS outputs for patch 0
 * - Per-patch TCS outputs for patch 1
 * - Per-patch TCS outputs for patch 2
 *
 * If the Hull stage does not write any new control points the driver will
 * optimize LDS layout so input and output control point spaces overlap.
 *
 * Tessellation factors are stored in the per-patch TCS output block
 * as well as a factor V# that is automatically bound by the driver.
 *
 * This pass attempts to resolve LDS accesses to attribute accesses and correctly
 * write to the tessellation factor tables. For the latter we replace the
 * buffer store instruction to factor writes according to their offset.
 *
 * LDS stores can either be output control point writes or per-patch data writes.
 * This is detected by looking at how the address is formed. In any case the calculation
 * will be of the form a * b + c. For output control points a = output_control_point_id
 * while for per-patch writes a = patch_id.
 *
 * Both patch_id and output_control_point_id are packed in VGPR1 by the driver and shader
 * uses V_BFE_U32 to extract them. We use the starting bit_pos to determine which is which.
 *
 * LDS reads are more tricky as amount of different calculations performed can vary.
 * The final result, if output control point space is distinct, is of the form:
 * patch_id * input_control_point_stride * num_control_points_per_input_patch + a
 * The value "a" can be anything in the range of [0, input_control_point_stride]
 *
 * This pass does not attempt to deduce the exact attribute referenced by "a" but rather
 * only using "a" itself index into input attributes. Those are defined as an array in the shader
 * layout (location = 0) in vec4[num_control_points_per_input_patch] attr[];
 * ...
 * float value = attr[a / in_stride][(a % in_stride) >> 4][(a & 0xF) >> 2];
 *
 * This requires knowing in_stride which is not provided to us by the guest.
 * To deduce it we perform a breadth first search on the arguments of a DS_READ*
 * looking for a buffer load with offset = 0. This will be the buffer holding tessellation
 * constants and it contains the value of in_stride we can read at compile time.
 *
 * NOTE: This pass must be run before constant propagation as it relies on relatively specific
 * pattern matching that might be mutated that that optimization pass.
 *
 * TODO: need to be careful about reading from output arrays at idx other than InvocationID
 * Need SPIRV OpControlBarrier
 * "Wait for all active invocations within the specified Scope to reach the current point of
 * execution."
 * Must be placed in uniform control flow
 */

// Bad pattern matching attempt
template <typename Derived>
struct MatchObject {
    inline bool DoMatch(IR::Value v) {
        return static_cast<Derived*>(this)->DoMatch(v);
    }
};

struct MatchValue : MatchObject<MatchValue> {
    MatchValue(IR::Value& return_val_) : return_val(return_val_) {}

    inline bool DoMatch(IR::Value v) {
        return_val = v;
        return true;
    }

private:
    IR::Value& return_val;
};

struct MatchIgnore : MatchObject<MatchIgnore> {
    MatchIgnore() {}

    inline bool DoMatch(IR::Value v) {
        return true;
    }
};

struct MatchImm : MatchObject<MatchImm> {
    MatchImm(IR::Value& v) : return_val(v) {}

    inline bool DoMatch(IR::Value v) {
        if (!v.IsImmediate()) {
            return false;
        }

        return_val = v;
        return true;
    }

private:
    IR::Value& return_val;
};

// Specific
struct MatchAttribute : MatchObject<MatchAttribute> {
    MatchAttribute(IR::Attribute attribute_) : attribute(attribute_) {}

    inline bool DoMatch(IR::Value v) {
        return v.Type() == IR::Type::Attribute && v.Attribute() == attribute;
    }

private:
    IR::Attribute attribute;
};

// Specific
struct MatchU32 : MatchObject<MatchU32> {
    MatchU32(u32 imm_) : imm(imm_) {}

    inline bool DoMatch(IR::Value v) {
        return v.Type() == IR::Type::U32 && v.U32() == imm;
    }

private:
    u32 imm;
};

template <IR::Opcode opcode, typename... Args>
struct MatchInstObject : MatchObject<MatchInstObject<opcode>> {
    static_assert(sizeof...(Args) == IR::NumArgsOf(opcode));
    MatchInstObject(Args&&... args) : pattern(std::forward_as_tuple(args...)) {}

    inline bool DoMatch(IR::Value v) {
        IR::Inst* inst = v.TryInstRecursive();
        if (!inst || inst->GetOpcode() != opcode) {
            return false;
        }

        bool matched = true;

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            // TODO how to short circuit
            ((matched = matched && std::get<Is>(pattern).DoMatch(inst->Arg(Is))), ...);
        }(std::make_index_sequence<sizeof...(Args)>{});

        return matched;
    }

private:
    using MatchArgs = std::tuple<Args&...>;
    MatchArgs pattern;
};

template <IR::Opcode opcode, typename... Args>
auto MakeInstPattern(Args&&... args) {
    return MatchInstObject<opcode, Args...>(std::forward<Args>(args)...);
}

template <typename T, typename U, typename V>
auto MakeIMadPattern(MatchObject<T>&& a, MatchObject<U>&& b, MatchObject<V>&& c) {
    return MakeInstPattern<IR::Opcode::IAdd32>(
        MakeInstPattern<IR::Opcode::IMul32>(
            MakeInstPattern<IR::Opcode::BitFieldSExtract>(std::forward<MatchObject<T>>(a),
                                                          MatchU32(0), MatchU32(24)),
            MakeInstPattern<IR::Opcode::BitFieldSExtract>(std::forward<MatchObject<U>>(b),
                                                          MatchU32(0), MatchU32(24))),
        std::forward<MatchObject<V>>(c));
}

// Represent address as sum of products
// Input control point:
//     PrimitiveId * input_cp_stride * #cp_per_input_patch + index * input_cp_stride + (attr# * 16 +
//     component)
// Output control point
//    #patches * input_cp_stride * #cp_per_input_patch + PrimitiveId * output_patch_stride +
//    InvocationID * output_cp_stride + (attr# * 16 + component)
// Per patch output:
//    #patches * input_cp_stride * #cp_per_input_patch + #patches * output_patch_stride +
//    + PrimitiveId * per_patch_output_stride + (attr# * 16 + component)

// Sort terms left to right
struct RingAddress {
    enum class Region : u32 { InputCP, OutputCP, PatchOutput };

    Region RegionKind() {
        if (is_passthrough) {
            DEBUG_ASSERT(region <= 1);
            return region == 1 ? Region::PatchOutput : Region::InputCP;
        }
        return Region(region);
    };
    IR::Value Index() {
        return index;
    };
    bool HasSimpleControlPointIndex();
    u32 AttributeByteOffset() {
        return attribute_byte_offset;
    };

    void WalkRingAccess(IR::Inst* ring_access) {
        ctx = {};
        linear_products.clear();
        terms.clear();
        linear_products.emplace_back();
        IR::Value addr = ring_access->Arg(0);
        terms.emplace_back(addr);
        Visit(addr);
        DEBUG_ASSERT(linear_products.size() == terms.size());
        GatherInfo();
    }

private:
    struct Context {
        bool within_mul{};
    } ctx;

    void Visit(IR::Value node) {
        IR::Value a, b, c;

        struct MulScope {
            Context saved_ctx;
            Context& ctx;

            MulScope(Context& ctx) : ctx(ctx), saved_ctx(ctx) {
                saved_ctx.within_mul = true;
                std::swap(ctx, saved_ctx);
            }
            ~MulScope() {
                std::swap(ctx, saved_ctx);
            }
        };

        if (MakeIMadPattern(MatchValue(a), MatchValue(b), MatchValue(c)).DoMatch(node)) {
            // v_mad_i32_i24 _, a, b, c
            {
                MulScope _{ctx};
                Visit(a);
                Visit(b);
            }

            linear_products.emplace_back();
            terms.emplace_back(c);
            Visit(c);
        } else if (MakeInstPattern<IR::Opcode::IMul32>(
                       MakeInstPattern<IR::Opcode::BitFieldSExtract>(MatchValue(a), MatchU32(0),
                                                                     MatchU32(24)),
                       MakeInstPattern<IR::Opcode::BitFieldSExtract>(MatchValue(b), MatchU32(0),
                                                                     MatchU32(24)))
                       .DoMatch(node)) {
            {
                MulScope _{ctx};
                Visit(a);
                Visit(b);
            }
            // v_mul_i32_i24 _, a, b
        } else if (MakeInstPattern<IR::Opcode::IAdd32>(MatchValue(a), MatchValue(b))
                       .DoMatch(node)) {
            DEBUG_ASSERT(!ctx.within_mul);
            // some add a + b
            if (!ctx.within_mul) {
                terms.back() = a;
            }
            Visit(a);
            if (!ctx.within_mul) {
                linear_products.emplace_back();
                terms.emplace_back(b);
            }
            Visit(b);
        } else if (MakeInstPattern<IR::Opcode::ShiftLeftLogical32>(MatchValue(a), MatchImm(b))
                       .DoMatch(node)) {
            linear_products.back().emplace_back(IR::Value(u32(2 << (b.U32() - 1))));
            Visit(a);
        } else if (MakeInstPattern<IR::Opcode::ReadConstBuffer>(
                       MatchIgnore(), MakeInstPattern<IR::Opcode::IAdd32>(MatchU32(0), MatchU32(2)))
                       .DoMatch(node)) {
            // #patches system value seems to be provided via V# at offset 2.
            linear_products.back().emplace_back(IR::Value(IR::Attribute::TcsNumPatches));
        } else if (MakeInstPattern<IR::Opcode::BitCastF32U32>(MatchValue(a)).DoMatch(node)) {
            return Visit(a);
        } else if (MakeInstPattern<IR::Opcode::BitCastU32F32>(MatchValue(a)).DoMatch(node)) {
            return Visit(a);
        } else {
            linear_products.back().emplace_back(node);
        }
    }

    void GatherInfo() {
        region = 0;
        attribute_byte_offset = 0;
        index = {};

        // Remove addends except for the attribute offset and possibly the
        // control point index calc
        boost::adaptors::filter(linear_products, [&](auto& term) {
            for (IR::Value& value : term) {
                if (value.Type() == IR::Type::Attribute) {
                    if (value.Attribute() == IR::Attribute::TcsNumPatches) {
                        // Terms that multiply by #patches should be meant to add the size
                        // of the input control point region or output control point region.
                        // So by counting them, we can infer what region the address is in.
                        // e.g. #patches * input_cp_stride * #cp_per_input_patch +
                        // #patches * output_patch_stride + ... -> PerPatchRegion
                        // #patches * input_cp_stride * #cp_per_input_patch + ... -> OutputCP
                        ++region;
                        return false;
                    } else if (value.Attribute() == IR::Attribute::PrimitiveId) {
                        return false;
                    }
                }
            }
            return true;
        });

        // For now assume we don't have any Output attribute reads

        // Look for some term with a dynamic index (should be the control point index)
        // Output writes: InvocationId
        // Input reads: arbitrary
        // Output reads: ???
        int count = 0;
        for (auto i = 0; i < linear_products.size(); i++) {
            auto& term = linear_products[i];
            // Remember this as the index term
            if (std::any_of(term.begin(), term.end(),
                            [&](const IR::Value& v) { return !v.IsImmediate(); })) {
                ASSERT_MSG(index.IsEmpty(),
                           "more than one non-immediate term in address calculation");
                index = terms[i];
            } else {
                // Otherwise assume it contributes to the attribute
                attribute_byte_offset +=
                    std::accumulate(term.begin(), term.end(), 0, [](u32 product, IR::Value& v) {
                        ASSERT(v.IsImmediate() && v.Type() == IR::Type::U32);
                        return product * v.U32();
                    });
            }
        }
    }

    u32 region;

    u32 attribute_byte_offset;

    bool is_passthrough; // TODO figure out how to deduce passthrough
    IR::Value index;

    // address addends, in original nested IR
    boost::container::small_vector<IR::Value, 4> terms;

    // Each element is a linear representation of each term.
    // linear_products[i][0] * ... * linear_products[i][linear_products[i].size() - 1] should
    // represent terms[i]
    boost::container::small_vector<boost::container::small_vector<IR::Value, 4>, 4> linear_products;
};

void HullShaderTransform(const IR::Program& program, const RuntimeInfo& runtime_info) {
    auto dumpMatchingIR = [&](std::string phase) {
        std::string s = IR::DumpProgram(program);
        using namespace Common::FS;
        const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
        if (!std::filesystem::exists(dump_dir)) {
            std::filesystem::create_directories(dump_dir);
        }
        const auto filename =
            fmt::format("{}_{:#018x}.{}.ir.txt", program.info.stage, program.info.pgm_hash, phase);
        const auto file = IOFile{dump_dir / filename, FileAccessMode::Write};
        file.WriteString(s);
    };

    // Replace with intrinsics for easier pattern matching
    for (IR::Block* block : program.blocks) {

        for (auto it = block->Instructions().begin(); it != block->Instructions().end(); it++) {
            IR::Inst& inst = *it;
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::BitFieldUExtract: {
                if (MakeInstPattern<IR::Opcode::BitFieldUExtract>(
                        MakeInstPattern<IR::Opcode::GetAttributeU32>(
                            MatchAttribute(IR::Attribute::PackedHullInvocationInfo), MatchIgnore()),
                        MatchU32(0), MatchU32(8))
                        .DoMatch(IR::Value{&inst})) {
                    IR::IREmitter emit(*block, it);
                    IR::Value replacement = emit.GetAttributeU32(IR::Attribute::PrimitiveId);
                    inst.ReplaceUsesWithAndRemove(replacement);
                } else if (MakeInstPattern<IR::Opcode::BitFieldUExtract>(
                               MakeInstPattern<IR::Opcode::GetAttributeU32>(
                                   MatchAttribute(IR::Attribute::PackedHullInvocationInfo),
                                   MatchIgnore()),
                               MatchU32(8), MatchU32(5))
                               .DoMatch(IR::Value{&inst})) {
                    IR::IREmitter emit(*block, it);
                    IR::Value replacement = emit.GetAttributeU32(IR::Attribute::InvocationId);
                    inst.ReplaceUsesWithAndRemove(replacement);
                }
                break;
            }

            case IR::Opcode::BitFieldSExtract: {
                IR::Value offset{};
                if (MakeInstPattern<IR::Opcode::BitFieldSExtract>(
                        MakeInstPattern<IR::Opcode::ReadConstBuffer>(
                            MatchIgnore(),
                            MakeInstPattern<IR::Opcode::IAdd32>(MatchU32(0), MatchU32(0))),
                        MatchU32(19), MatchU32(2))
                        .DoMatch(IR::Value{&inst})) {
                    IR::IREmitter emit(*block, it);
                    IR::Value replacement = emit.GetAttributeU32(IR::Attribute::TcsInputCpStride);
                    inst.ReplaceUsesWithAndRemove(replacement);
                }
                break;
            }

            default:
                break;
            }
        }
    }

    dumpMatchingIR("mid_hull_tranform");

    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::StoreBufferU32:
            case IR::Opcode::StoreBufferU32x2:
            case IR::Opcode::StoreBufferU32x3:
            case IR::Opcode::StoreBufferU32x4: {
                const auto info = inst.Flags<IR::BufferInstInfo>();
                if (!info.globally_coherent) {
                    break;
                }
                const auto GetValue = [&](IR::Value data) -> IR::F32 {
                    if (auto* inst = data.TryInstRecursive();
                        inst && inst->GetOpcode() == IR::Opcode::BitCastU32F32) {
                        return IR::F32{inst->Arg(0)};
                    }
                    return ir.BitCast<IR::F32, IR::U32>(IR::U32{data});
                };
                const u32 num_dwords = u32(opcode) - u32(IR::Opcode::StoreBufferU32) + 1;
                const auto factor_idx = info.inst_offset.Value() >> 2;
                const IR::Value data = inst.Arg(2);
                inst.Invalidate();
                if (num_dwords == 1) {
                    ir.SetPatch(IR::PatchFactor(factor_idx), GetValue(data));
                    break;
                }
                auto* inst = data.TryInstRecursive();
                ASSERT(inst && (inst->GetOpcode() == IR::Opcode::CompositeConstructU32x2 ||
                                inst->GetOpcode() == IR::Opcode::CompositeConstructU32x3 ||
                                inst->GetOpcode() == IR::Opcode::CompositeConstructU32x4));
                for (s32 i = 0; i < num_dwords; i++) {
                    ir.SetPatch(IR::PatchFactor(factor_idx + i), GetValue(inst->Arg(i)));
                }
                break;
            }
            case IR::Opcode::WriteSharedU32:
            case IR::Opcode::WriteSharedU64:
            case IR::Opcode::WriteSharedU128: {
                RingAddress ring_address;
                ring_address.WalkRingAccess(&inst);

                const u32 num_dwords = opcode == IR::Opcode::WriteSharedU32
                                           ? 1
                                           : (opcode == IR::Opcode::WriteSharedU64 ? 2 : 4);
                const IR::Value data = inst.Arg(1);
                const auto [data_lo, data_hi] = [&] -> std::pair<IR::U32, IR::U32> {
                    if (num_dwords == 1) {
                        return {IR::U32{data}, IR::U32{}};
                    }
                    const auto* prod = data.InstRecursive();
                    return {IR::U32{prod->Arg(0)}, IR::U32{prod->Arg(1)}};
                }();

#if 0
                const IR::Inst* ds_offset = inst.Arg(0).InstRecursive();
                const u32 offset_dw = ds_offset->Arg(1).U32() >> 4; // should be >> 2?
                IR::Inst* prod = ds_offset->Arg(0).TryInstRecursive();
                ASSERT(prod && (prod->GetOpcode() == IR::Opcode::IAdd32 ||
                                prod->GetOpcode() == IR::Opcode::IMul32));
                if (prod->GetOpcode() == IR::Opcode::IAdd32) {
                    prod = prod->Arg(0).TryInstRecursive();
                    ASSERT(prod && prod->GetOpcode() == IR::Opcode::IMul32);
                }
                prod = prod->Arg(0).TryInstRecursive();
                ASSERT(prod && prod->GetOpcode() == IR::Opcode::BitFieldSExtract &&
                       prod->Arg(2).IsImmediate() && prod->Arg(2).U32() == 24);
                prod = prod->Arg(0).TryInstRecursive();
                ASSERT(prod && prod->GetOpcode() == IR::Opcode::BitFieldUExtract);
                const u32 bit_pos = prod->Arg(1).U32();

                ASSERT_MSG(bit_pos == 0 || bit_pos == 8, "Unknown bit extract pos {}", bit_pos);
                const bool is_patch_const = bit_pos == 0;
#endif

                const auto SetOutput = [&ir](IR::U32 value, u32 offset_dw,
                                             RingAddress::Region output_kind) {
                    const IR::F32 data = ir.BitCast<IR::F32, IR::U32>(value);
                    if (output_kind == RingAddress::Region::OutputCP) {
                        const u32 param = offset_dw >> 2;
                        const u32 comp = offset_dw & 3;
                        ir.SetAttribute(IR::Attribute::Param0 + param, data, comp);
                    } else {
                        assert(output_kind == RingAddress::Region::PatchOutput);
                        ir.SetPatch(IR::PatchGeneric(offset_dw), data);
                    }
                };

                u32 offset_dw = ring_address.AttributeByteOffset() >> 2;
                SetOutput(data_lo, offset_dw, ring_address.RegionKind());
                if (num_dwords > 1) {
                    SetOutput(data_hi, offset_dw + 1, ring_address.RegionKind());
                }
                inst.Invalidate();
                break;
            }

            case IR::Opcode::LoadSharedU32:
            case IR::Opcode::LoadSharedU64:
            case IR::Opcode::LoadSharedU128: {
                break;
            }

            default:
                break;
            }
        }
    }
}

} // namespace Shader::Optimization
