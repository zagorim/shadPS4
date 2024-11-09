// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void EnsureThreadInitialized();

template <class ReturnType, class... FuncArgs, class... CallArgs>
ReturnType ExecuteGuest(PS4_SYSV_ABI ReturnType (*func)(FuncArgs...), CallArgs&&... args) {
    EnsureThreadInitialized();
    return func(std::forward<CallArgs>(args)...);
}

} // namespace Core
