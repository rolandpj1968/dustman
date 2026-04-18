# Coding Standards

This document captures the C++ coding standards for dustman. It complements `.clang-format` (mechanical formatting) and — once it lands — `.clang-tidy` (lint enforcement) with the rules that tooling cannot express: what names look like, when to reach for which feature, how to lay out an expression that does too much on one line.

Rules are obeyed unless there is a concrete reason to deviate. When deviating, leave a comment explaining why — those are the comments that earn their keep.

## 1. Naming

- **Namespaces**: lowercase, short (`dustman`, `dustman::detail`).
- **Types** (classes, structs, enums, aliases): CamelCase (`TypeInfo`, `Visitor`, `Tracer`).
- **Smart-pointer-like types**: snake_case, matching std convention (`gc_ptr`, not `GcPtr`).
- **Functions, methods, variables**: snake_case (`visit`, `trace_trampoline`, `version_major`).
- **Macros**: `UPPER_SNAKE_CASE`, `DUSTMAN_` prefix (`DUSTMAN_VERSION_MAJOR`).
- **Template type parameters**: CamelCase — single letter (`T`, `U`) or meaningful name (`Field`, `Args`).
- **Internal implementation**: `detail` namespace (`dustman::detail`).

## 2. File structure

Each header starts with `#pragma once` on the first line. Include order, grouped with a blank line between groups:

1. The header's own-pair header (only in `.cpp` files — `foo.cpp` first includes `foo.hpp`).
2. Other dustman headers, alphabetical.
3. Third-party headers (Catch2, Google Benchmark).
4. C++ standard library.
5. C / POSIX headers.

IWYU ("include what you use"): every file includes what it references directly. Do not rely on transitive includes.

## 3. Whitespace and blank lines

- **Space before `{`** in brace-init: `Foo x {args}` (not `Foo x{args}`), `return Foo {args};`, `v.emplace_back(Foo {args})`. Applies consistently to declarations and expression-use.
- **Space after commas**, before the next argument.
- **No space inside** `()`, `[]`, `<>`: `std::vector<int> v`, not `std::vector < int > v`.
- **Blank line** between logical sections of a function, between constructor member-init list and body, between related groups in a class.

## 4. Expression complexity

- Break long or punctuation-dense expressions into named locals.

  Bad:
  ```cpp
  static_cast<T*>(obj)->~T();
  ```

  Good:
  ```cpp
  auto* t = static_cast<T*>(obj);
  t->~T();
  ```

- Never nest ternaries.
- A line with three or more adjacent punctuation characters (`->~`, `::*`, `.*&`, `&&*`) is a smell — split it.

## 5. Constness

- Parameters, locals, and member functions are `const` wherever possible.
- **West const**: `const T&`, matching the dominant std/Boost convention.
- Never cast away const.

## 6. Pointers and references

- Pointer and reference sigils are **left-aligned**: `T* p`, `T& r`. (Already enforced by `.clang-format`.)
- Prefer references to pointers where `null` is impossible.
- Always `nullptr`. Never `NULL` or `0`.

## 7. Type deduction

- Use `auto` only where the type is obvious from context: range-for over a named container, factory-and-initialise (`auto p = alloc<Foo>();`), iterators. Spell the type out otherwise.
- `auto*` for pointer deductions, to make the indirection explicit (`auto* t = static_cast<T*>(obj);`).

## 8. Constructors and initialisation

- **`explicit`** on every single-argument constructor unless implicit conversion is genuinely desired.
- **Brace-init** (`T x {…}`) as default; parenthesised construction only when brace-init collides with an initializer-list constructor (rare).
- **Trailing commas** in multi-line aggregate initialisers — reduces diff noise.

## 9. Functions

- **`noexcept`** on every function that cannot throw. Mandatory on destructors, move operations, and trampolines.
- **`[[nodiscard]]`** on functions whose return value is the point of the call.
- Return values by value unless profiling shows otherwise; trust the compiler's copy elision.

## 10. Enums

- Always `enum class`.
- Always specify the underlying type: `enum class Flags : std::uint32_t { … };`.

## 11. Error handling

- **Exceptions are forbidden in library code.** Library `.cpp` files are compiled with `-fno-exceptions`; headers do not throw, do not contain `try`/`catch`, and do not rely on RAII cleanup along exceptional paths.
- **Fatal conditions** (out-of-memory being the primary case) call a `dustman::detail::fatal_*()` helper which aborts. A future configurable hook may log or write a crash dump before aborting; the call sites do not change.
- Normal error paths use return types (`std::optional`, `std::pair<bool, T>`, custom error enums).
- This rule applies to library code only. **Tests use Catch2** and require exceptions to be enabled in their compilation units. Consumer code may use exceptions; consumer-type constructor throws may propagate through our inline templates, but they never enter our compiled `.cpp` files because our library never propagates exceptions across its own frames.
- A consequence: GC-managed consumer types are effectively required to be nothrow-constructible in practice. A throwing constructor under `-fno-exceptions` would `std::terminate`.

## 12. RTTI

- **Forbidden.** No `typeid`, no `dynamic_cast`.
- `dustman::TypeInfo` is our manual vtable; any dispatch on dynamic type goes through it.

## 13. Namespaces

- `using namespace` is **banned in headers**, discouraged in `.cpp` files.
- Spell names explicitly (`dustman::Visitor`) rather than aliasing — aliasing is for very long names only.
- Internal-only names live in `dustman::detail`.

## 14. Type aliases

- `using X = Y;` — never `typedef`.

## 15. Comments

- **None by policy** at this stage, to be back-filled when APIs stabilise. Churning APIs produce churning comments, which are pure waste.
- When comments do land, they explain **why**, not **what**. A comment that paraphrases the code has negative value.
- The design documents (`docs/design.md`, `docs/platform.md`, this file) carry the rationale; source files are the implementation.

## 16. Header-vs-source split

- **Header**: hot path (`alloc<T>`, `gc_ptr<T>`, `Tracer<T>`, safepoint poll, write barrier), templates, anything performance-critical.
- **Source** (`.cpp`): global state, platform primitives, slow paths. See [`docs/design.md`](design.md) for the full rationale.

## 17. Tests and benchmarks

- Each test case asserts one observable behaviour. Name tests after what they prove.
- **Both positive and negative tests**: what the code should do AND what it should not do.
- Benchmarks track real work (allocation, tracing, collection). A benchmark of an empty loop is a harness check, not a measurement.
