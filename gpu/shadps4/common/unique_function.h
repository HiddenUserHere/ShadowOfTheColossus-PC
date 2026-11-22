// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: small-buffer optimization (N2-94, W4), round 3 fixer.
//
// Two implementations live here, both always compiled:
//   - detail::HeapUniqueFunction: the pre-W4 layout, one std::unique_ptr to a virtual callable.
//     sizeof == sizeof(void*). Used when SOTC_UF_SBO is 0 (default), so every queue entry and ring slot
//     keeps its old size and the default build is byte-for-byte the old one.
//   - detail::InlineUniqueFunction: callables up to 64 bytes (alignment <= 16, nothrow move) live inside
//     the wrapper, larger ones on the heap. Used when SOTC_UF_SBO is 1.
// Common::UniqueFunction is an alias that selects one of them from SOTC_UF_SBO. Both classes stay
// compilable in any configuration, so a scratch test can exercise both paths in one translation unit.
//
// ODR / build-wide setting (N2-94 H08, round 3): SOTC_UF_SBO must have the same value in every
// translation unit of the build (set it once in CMake/build.sh, not per file). The two implementations
// are distinct class templates, so the alias makes a function whose signature takes a UniqueFunction
// mangle differently in a TU built with the other value: a mixed build fails at link time instead of
// silently mixing layouts. This does NOT protect class definitions that hold a UniqueFunction by
// value (for example a struct with a std::deque of them), which keep the same name in both modes;
// the build-wide setting is therefore still required (see core_requests).
//
// Coverage (N2-94 H08, W4, round 3):
//   item                                                  | done? | evidence
//   default-off layout: sizeof(UniqueFunction) == 8       | yes   | HeapUniqueFunction holds one std::unique_ptr (static_assert below)
//   inline storage: size <= 64 B, align <= 16             | yes   | InlineUniqueFunction::kInlineSize / kInlineAlign, `fits` in converting ctor
//   inline only when nothrow move-constructible           | yes   | std::is_nothrow_move_constructible_v<Holder<Functor>>
//   heap fallback for larger / throwing-move types        | yes   | Ops<Functor, false>: storage holds Holder*
//   per-type vtable: invoke / move / destroy (inline)     | yes   | VTable, Ops<Functor, Inline>::table
//   move ctor / move assign leave the source empty        | yes   | rhs.vtable = nullptr after the move; self-move guarded (inline); default (heap)
//   destroy runs exactly once (reset, dtor, assign)      | yes   | Reset() clears vtable after destroy (inline); unique_ptr (heap)
//   public API unchanged (ctors, operator(), bool)       | yes   | copy ops deleted; operator() const; explicit operator bool, both variants
//   non-const lvalue functors rejected (old: error)      | yes   | static_assert in both converting ctors (const lvalue aliasing kept as before)
//   SOTC_UF_SBO selects the variant (default 0)          | yes   | alias Common::UniqueFunction; macro value checked by static_assert
//   inline path compiled in default build too            | yes   | detail::InlineUniqueFunction is always defined, so a test can use it without -DSOTC_UF_SBO=1
//   ring memory (native_stream_ops.cpp kMaxPending)      | yes   | default: Slot 16 B, 2^19 entries as before; SBO on: 2^17 (see that file)
//   REA                                                   | n/a   | not required by this slice

#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#ifndef SOTC_UF_SBO
#define SOTC_UF_SBO 0
#endif
static_assert(SOTC_UF_SBO == 0 || SOTC_UF_SBO == 1, "SOTC_UF_SBO must be 0 or 1");

namespace Common {

/// True when the inline (small-buffer) variant is the Common::UniqueFunction alias. Default false.
inline constexpr bool kUniqueFunctionSbo = SOTC_UF_SBO != 0;

namespace detail {

/// Default variant (SOTC_UF_SBO=0): the pre-W4 wrapper, one pointer to a heap callable.
template <typename ResultType, typename... Args>
class HeapUniqueFunction {
    struct CallableBase {
        virtual ~CallableBase() = default;
        virtual ResultType Call(Args&&... args) = 0;
    };

    template <typename Functor>
    struct Callable final : CallableBase {
        explicit Callable(Functor&& functor_) : functor(std::forward<Functor>(functor_)) {}
        ResultType Call(Args&&... args) override {
            return functor(std::forward<Args>(args)...);
        }
        Functor functor;
    };

public:
    HeapUniqueFunction() = default;

    template <typename Functor>
    HeapUniqueFunction(Functor&& functor) {
        static_assert(!std::is_lvalue_reference_v<Functor> ||
                          std::is_const_v<std::remove_reference_t<Functor>>,
                      "UniqueFunction takes ownership: pass an rvalue (std::move the callable)");
        callable = std::make_unique<Callable<Functor>>(std::forward<Functor>(functor));
    }

    HeapUniqueFunction(HeapUniqueFunction&& rhs) noexcept = default;
    HeapUniqueFunction& operator=(HeapUniqueFunction&& rhs) noexcept = default;
    HeapUniqueFunction(const HeapUniqueFunction&) = delete;
    HeapUniqueFunction& operator=(const HeapUniqueFunction&) = delete;
    ~HeapUniqueFunction() = default;

    ResultType operator()(Args&&... args) const {
        return callable->Call(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return callable != nullptr;
    }

private:
    std::unique_ptr<CallableBase> callable;
};

static_assert(sizeof(HeapUniqueFunction<void>) == sizeof(void*),
              "default UniqueFunction must keep the pre-W4 pointer-sized layout");

/// Inline variant (SOTC_UF_SBO=1): callables up to 64 bytes live in the wrapper.
template <typename ResultType, typename... Args>
class InlineUniqueFunction {
    static constexpr std::size_t kInlineSize = 64;
    static constexpr std::size_t kInlineAlign = 16;

    /// Operations for one stored callable type, chosen once at construction. `move` move-constructs
    /// the object into `dst` and destroys the source (heap storage: only the pointer is transferred).
    /// `destroy` runs when the function is reset or destroyed.
    struct VTable {
        ResultType (*invoke)(void* storage, Args&&... args);
        void (*move)(void* dst, void* src) noexcept;
        void (*destroy)(void* storage) noexcept;
    };

    template <typename Functor>
    struct Holder {
        explicit Holder(Functor&& functor_) : functor(std::forward<Functor>(functor_)) {}
        ResultType operator()(Args&&... args) {
            return functor(std::forward<Args>(args)...);
        }
        Functor functor;
    };

    /// Inline (Inline = true): the Holder object itself sits in the storage.
    /// Heap (Inline = false): the storage holds a Holder* pointer.
    template <typename Functor, bool Inline>
    struct Ops {
        using H = Holder<Functor>;
        using HP = H*;

        static H* Get(void* storage) noexcept {
            if constexpr (Inline) {
                return std::launder(static_cast<H*>(storage));
            } else {
                return *std::launder(static_cast<HP*>(storage));
            }
        }

        static ResultType Invoke(void* storage, Args&&... args) {
            return (*Get(storage))(std::forward<Args>(args)...);
        }

        static void Move(void* dst, void* src) noexcept {
            if constexpr (Inline) {
                H* from = Get(src);
                ::new (dst) H(std::move(*from));
                from->~H();
            } else {
                ::new (dst) HP(*std::launder(static_cast<HP*>(src)));
            }
        }

        static void Destroy(void* storage) noexcept {
            if constexpr (Inline) {
                Get(storage)->~H();
            } else {
                delete Get(storage);
            }
        }

        static constexpr VTable table{&Invoke, &Move, &Destroy};
    };

public:
    InlineUniqueFunction() = default;

    template <typename Functor>
    InlineUniqueFunction(Functor&& functor) {
        // The old Callable<Functor> stored `Functor` and could not bind a non-const lvalue (the
        // move in its initializer made it an rvalue). Keep that set of accepted arguments and turn
        // the silent-aliasing case into a compile error. Const lvalues keep their old behaviour.
        static_assert(!std::is_lvalue_reference_v<Functor> ||
                          std::is_const_v<std::remove_reference_t<Functor>>,
                      "UniqueFunction takes ownership: pass an rvalue (std::move the callable)");
        using H = Holder<Functor>;
        using HP = H*;
        constexpr bool fits = sizeof(H) <= kInlineSize && alignof(H) <= kInlineAlign &&
                              std::is_nothrow_move_constructible_v<H>;
        if constexpr (fits) {
            ::new (static_cast<void*>(storage)) H(std::forward<Functor>(functor));
            vtable = &Ops<Functor, true>::table;
        } else {
            ::new (static_cast<void*>(storage)) HP(new H(std::forward<Functor>(functor)));
            vtable = &Ops<Functor, false>::table;
        }
    }

    InlineUniqueFunction& operator=(InlineUniqueFunction&& rhs) noexcept {
        if (this != &rhs) {
            Reset();
            if (rhs.vtable != nullptr) {
                rhs.vtable->move(storage, rhs.storage);
                vtable = rhs.vtable;
                rhs.vtable = nullptr;
            }
        }
        return *this;
    }

    InlineUniqueFunction(InlineUniqueFunction&& rhs) noexcept {
        if (rhs.vtable != nullptr) {
            rhs.vtable->move(storage, rhs.storage);
            vtable = rhs.vtable;
            rhs.vtable = nullptr;
        }
    }

    ~InlineUniqueFunction() {
        Reset();
    }

    InlineUniqueFunction& operator=(const InlineUniqueFunction&) = delete;
    InlineUniqueFunction(const InlineUniqueFunction&) = delete;

    ResultType operator()(Args&&... args) const {
        return vtable->invoke(storage, std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return vtable != nullptr;
    }

private:
    void Reset() noexcept {
        if (vtable != nullptr) {
            vtable->destroy(storage);
            vtable = nullptr;
        }
    }

    const VTable* vtable = nullptr;
    alignas(kInlineAlign) mutable unsigned char storage[kInlineSize];
};

} // namespace detail

/// General purpose function wrapper similar to std::function.
/// Unlike std::function, the captured values don't have to be copyable.
/// This class can be moved but not copied. Variant chosen by SOTC_UF_SBO (see the header comment).
template <typename ResultType, typename... Args>
using UniqueFunction = std::conditional_t<kUniqueFunctionSbo,
                                          detail::InlineUniqueFunction<ResultType, Args...>,
                                          detail::HeapUniqueFunction<ResultType, Args...>>;

} // namespace Common
