/**
 * @file kernel/lib/ubsan.cpp
 * @brief Freestanding UBSan handler stubs (F-INFRA I-9)
 *
 * GCC -fsanitize=undefined instruments kernel code to CALL these handlers when
 * it detects undefined behaviour at runtime (signed overflow, shift out of
 * bounds, divrem overflow, out-of-bounds access, type mismatch, null member
 * call, ...). We provide freestanding definitions that route each hit into
 * kpanic (with backtrace + memstats), so UB becomes a loud, located panic
 * instead of silent miscompilation -- the single highest-value static/runtime
 * check before F4 SMP makes latent UB non-deterministic.
 *
 * GCC declares every __ubsan_handle_* as a BUILTIN with all-pointer args (e.g.
 * __ubsan_handle_shift_out_of_bounds(void*, void*, void*)); the integer value
 * operands are widened to void* at the instrumented call site. The definition
 * MUST match the builtin signature exactly -- a uintptr_t form (Clang's) is a
 * hard "definition ambiguates built-in declaration" error on GCC. This was
 * verified empirically against GCC 16.
 *
 * This file is compiled WITHOUT -fsanitize (CMake excludes it), and so are the
 * diagnostic-path files (kprintf/backtrace/diagnostics/exception_handlers), so
 * a UB hit never recurses through its own reporting path. Enable with
 * cmake -DCINUX_UBSAN=ON (off by default).
 *
 * @note The non-_abort (recoverable) variants are defined; GCC emits these under
 * the default -fsanitize-recover=undefined. The _abort variants are emitted
 * only with -fno-sanitize-recover=X; add them if a future build needs them.
 */

#include "kernel/lib/kprintf.hpp"

namespace {
/// Report the UBSan class and halt via the unified panic. The backtrace printed
/// by kpanic points at the instrumented call site -- the actual UB location.
[[gnu::noreturn]] void ubsan_trap(const char* kind) {
    cinux::lib::kpanic("UBSan: %s (undefined behaviour caught at runtime)", kind);
}
}  // namespace

extern "C" {

// --- 3-arg handlers: (data*, lhs, rhs) ---
void __ubsan_handle_shift_out_of_bounds(void*, void*, void*) {
    ubsan_trap("shift out of bounds");
}
void __ubsan_handle_add_overflow(void*, void*, void*) {
    ubsan_trap("add overflow");
}
void __ubsan_handle_sub_overflow(void*, void*, void*) {
    ubsan_trap("sub overflow");
}
void __ubsan_handle_mul_overflow(void*, void*, void*) {
    ubsan_trap("mul overflow");
}
void __ubsan_handle_divrem_overflow(void*, void*, void*) {
    ubsan_trap("divrem overflow");
}
void __ubsan_handle_pointer_overflow(void*, void*, void*) {
    ubsan_trap("pointer overflow");
}
void __ubsan_handle_implicit_conversion(void*, void*, void*) {
    ubsan_trap("implicit conversion");
}

// --- 2-arg handlers: (data*, value) ---
void __ubsan_handle_out_of_bounds(void*, void*) {
    ubsan_trap("out of bounds");
}
void __ubsan_handle_type_mismatch_v1(void*, void*) {
    ubsan_trap("type mismatch");
}
void __ubsan_handle_type_mismatch(void*, void*) {
    ubsan_trap("type mismatch");
}
void __ubsan_handle_negate_overflow(void*, void*) {
    ubsan_trap("negate overflow");
}
void __ubsan_handle_nonnull_arg(void*, void*) {
    ubsan_trap("nonnull argument");
}
void __ubsan_handle_nonnull_return(void*, void*) {
    ubsan_trap("nonnull return");
}
void __ubsan_handle_nonnull_return_v1(void*, void*) {
    ubsan_trap("nonnull return");
}
void __ubsan_handle_vla_bound_not_positive(void*, void*) {
    ubsan_trap("vla bound not positive");
}
void __ubsan_handle_float_cast_overflow(void*, void*) {
    ubsan_trap("float cast overflow");
}
void __ubsan_handle_load_invalid_value(void*, void*) {
    ubsan_trap("load invalid value");
}

// --- 4-arg handler: (data*, base, alignment, offset) ---
void __ubsan_handle_alignment_assumption(void*, void*, void*, void*) {
    ubsan_trap("alignment assumption");
}

// --- 1-arg handlers: (data*) ---
void __ubsan_handle_unreachable(void*) {
    ubsan_trap("unreachable executed");
}
void __ubsan_handle_missing_return(void*) {
    ubsan_trap("missing return");
}
void __ubsan_handle_invalid_builtin(void*) {
    ubsan_trap("invalid builtin");
}

}  // extern "C"
