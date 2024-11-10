// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/type.h"
#include "shader_recompiler/ir/value.h"

namespace Shader::IR {

Inst::Inst(IR::Opcode op_, u32 flags_) noexcept : op{op_}, flags{flags_} {
    if (op == Opcode::Phi) {
        std::construct_at(&phi_args);
    } else {
        std::construct_at(&args);
    }
}

Inst::Inst(const Inst& base) : op{base.op}, flags{base.flags} {
    if (base.op == Opcode::Phi) {
        throw NotImplementedException("Copying phi node");
    }
    std::construct_at(&args);
    const size_t num_args{base.NumArgs()};
    for (size_t index = 0; index < num_args; ++index) {
        SetArg(index, base.Arg(index));
    }
}

Inst::~Inst() {
    if (op == Opcode::Phi) {
        std::destroy_at(&phi_args);
    } else {
        std::destroy_at(&args);
    }
}

bool Inst::MayHaveSideEffects() const noexcept {
    switch (op) {
    case Opcode::Barrier:
    case Opcode::WorkgroupMemoryBarrier:
    case Opcode::DeviceMemoryBarrier:
    case Opcode::TcsOutputBarrier:
    case Opcode::ConditionRef:
    case Opcode::Reference:
    case Opcode::PhiMove:
    case Opcode::Prologue:
    case Opcode::Epilogue:
    case Opcode::Discard:
    case Opcode::DiscardCond:
    case Opcode::SetAttribute:
    case Opcode::SetPatch:
    case Opcode::StoreBufferU32:
    case Opcode::StoreBufferU32x2:
    case Opcode::StoreBufferU32x3:
    case Opcode::StoreBufferU32x4:
    case Opcode::StoreBufferFormatF32:
    case Opcode::BufferAtomicIAdd32:
    case Opcode::BufferAtomicSMin32:
    case Opcode::BufferAtomicUMin32:
    case Opcode::BufferAtomicSMax32:
    case Opcode::BufferAtomicUMax32:
    case Opcode::BufferAtomicInc32:
    case Opcode::BufferAtomicDec32:
    case Opcode::BufferAtomicAnd32:
    case Opcode::BufferAtomicOr32:
    case Opcode::BufferAtomicXor32:
    case Opcode::BufferAtomicSwap32:
    case Opcode::DataAppend:
    case Opcode::DataConsume:
    case Opcode::WriteSharedU128:
    case Opcode::WriteSharedU64:
    case Opcode::WriteSharedU32:
    case Opcode::SharedAtomicIAdd32:
    case Opcode::SharedAtomicSMin32:
    case Opcode::SharedAtomicUMin32:
    case Opcode::SharedAtomicSMax32:
    case Opcode::SharedAtomicUMax32:
    case Opcode::ImageWrite:
    case Opcode::ImageAtomicIAdd32:
    case Opcode::ImageAtomicSMin32:
    case Opcode::ImageAtomicUMin32:
    case Opcode::ImageAtomicSMax32:
    case Opcode::ImageAtomicUMax32:
    case Opcode::ImageAtomicInc32:
    case Opcode::ImageAtomicDec32:
    case Opcode::ImageAtomicAnd32:
    case Opcode::ImageAtomicOr32:
    case Opcode::ImageAtomicXor32:
    case Opcode::ImageAtomicExchange32:
    case Opcode::DebugPrint:
    case Opcode::EmitVertex:
    case Opcode::EmitPrimitive:
        return true;
    default:
        return false;
    }
}

bool Inst::AreAllArgsImmediates() const {
    if (op == Opcode::Phi) {
        UNREACHABLE_MSG("Testing for all arguments are immediates on phi instruction");
    }
    return std::all_of(args.begin(), args.begin() + NumArgs(),
                       [](const IR::Value& value) { return value.IsImmediate(); });
}

IR::Type Inst::Type() const {
    return TypeOf(op);
}

void Inst::SetArg(size_t index, Value value) {
    if (index >= NumArgs()) {
        throw InvalidArgument("Out of bounds argument index {} in opcode {}", index, op);
    }
    const IR::Value arg{Arg(index)};
    if (!arg.IsImmediate()) {
        UndoUse(arg.Inst(), index);
    }
    if (!value.IsImmediate()) {
        Use(value.Inst(), index);
    }
    if (op == Opcode::Phi) {
        phi_args[index].second = value;
    } else {
        args[index] = value;
    }
}

Block* Inst::PhiBlock(size_t index) const {
    if (op != Opcode::Phi) {
        UNREACHABLE_MSG("{} is not a Phi instruction", op);
    }
    if (index >= phi_args.size()) {
        throw InvalidArgument("Out of bounds argument index {} in phi instruction");
    }
    return phi_args[index].first;
}

void Inst::AddPhiOperand(Block* predecessor, const Value& value) {
    if (!value.IsImmediate()) {
        Use(value.Inst(), phi_args.size());
    }
    phi_args.emplace_back(predecessor, value);
}

void Inst::Invalidate() {
    ClearArgs();
    ASSERT(users.list.empty());
    ReplaceOpcode(Opcode::Void);
}

void Inst::ClearArgs() {
    if (op == Opcode::Phi) {
        for (auto i = 0; i < phi_args.size(); i++) {
            auto& pair = phi_args[i];
            IR::Value& value{pair.second};
            if (!value.IsImmediate()) {
                UndoUse(value.Inst(), i);
            }
        }
        phi_args.clear();
    } else {
        for (auto i = 0; i < args.size(); i++) {
            auto& value = args[i];
            if (!value.IsImmediate()) {
                UndoUse(value.Inst(), i);
            }
        }
        // Reset arguments to null
        // std::memset was measured to be faster on MSVC than std::ranges:fill
        std::memset(reinterpret_cast<char*>(&args), 0, sizeof(args));
    }
}

UseIterator Inst::UserList::UseBegin() {
    return UseIterator(list.begin(), list.end());
}

UseIterator Inst::UserList::UseEnd() {
    return UseIterator(list.end(), list.end());
}

boost::iterator_range<UseIterator> Inst::UserList::Uses() {
    return boost::make_iterator_range(UseBegin(), UseEnd());
}

void Inst::UserList::AddUse(IR::Inst* user, u32 operand) {
    DEBUG_ASSERT(operand < 31);
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const UserNode& user_node) { return user_node.user == user; });
    u32 operand_pos = 1 << operand;
    if (it == list.end()) {
        list.emplace_front(user, operand_pos);
    } else {
        DEBUG_ASSERT((it->operand_mask & operand_pos) == 0);
        it->operand_mask |= operand_pos;
    }
    ++num_uses;
}

void Inst::UserList::RemoveUse(IR::Inst* user, u32 operand) {
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const UserNode& user_node) { return user_node.user == user; });
    DEBUG_ASSERT(it != list.end());
    u32 operand_pos = 1 << operand;
    DEBUG_ASSERT((it->operand_mask & operand_pos) != 0);
    it->operand_mask &= ~operand_pos;
    if (it->operand_mask == 0) {
        list.erase(it);
    }
    --num_uses;
}

void Inst::ReplaceUsesWith(Value replacement, bool preserve) {
    // Copy since user->SetArg will mutate this->uses
    // Could also do temp_uses = std::move(uses) but more readable
    UserList temp_users = users;
    for (IR::Use use : temp_users.Uses()) {
        DEBUG_ASSERT(use.user->Arg(use.operand).Inst() == this);
        use.user->SetArg(use.operand, replacement);
    }
    Invalidate();
    if (preserve) {
        // Still useful to have Identity for indirection.
        // SSA pass would be more complicated without it
        ReplaceOpcode(Opcode::Identity);
        SetArg(0, replacement);
    }
}

void Inst::ReplaceOpcode(IR::Opcode opcode) {
    if (opcode == IR::Opcode::Phi) {
        UNREACHABLE_MSG("Cannot transition into Phi");
    }
    if (op == Opcode::Phi) {
        // Transition out of phi arguments into non-phi
        std::destroy_at(&phi_args);
        std::construct_at(&args);
    }
    op = opcode;
}

void Inst::Use(Inst* used, u32 operand) {
    used->users.AddUse(this, operand);
}

void Inst::UndoUse(Inst* used, u32 operand) {
    used->users.RemoveUse(this, operand);
}

UseIterator Inst::UseBegin() {
    return users.UseBegin();
}

UseIterator Inst::UseEnd() {
    return users.UseEnd();
}

boost::iterator_range<UseIterator> Inst::Uses() {
    return boost::make_iterator_range(UseBegin(), UseEnd());
}

} // namespace Shader::IR
