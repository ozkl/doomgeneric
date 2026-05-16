// ()  ()   ()   ()  ()  WebAssembly interpreter in a Header file (WAH)
// ()()() ()()() ()()()  Kang Seonghoon, version 2635 (2026-05), Public Domain
//  ()()  ()  () ()  ()  https://github.com/lifthrasiir/wah

#ifndef WAH_H
#define WAH_H

////////////////////////////////////////////////////////////////////////////////
// Public API //////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Macro: WAH_VERSION
//   The version of the WAH API. Incremented on any change to the API, including bug fixes.
//   Based on (fractional Gregorian year - 2000) * 100, with a liberal rounding.
#define WAH_VERSION -2635

// Macro: WAH_FORCE_PORTABLE [user-definable]
//   If defined, forces the interpreter to use portable C implementations
//   instead of platform-specific SIMD instructions.
//
// Macro: WAH_X86_64
// Macro: WAH_AARCH64
//   Defined automatically based on compiler macros if WAH_FORCE_PORTABLE is not defined.
#ifndef WAH_FORCE_PORTABLE
#if defined(__x86_64__) || defined(_M_X64)
#define WAH_X86_64
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define WAH_AARCH64
#include <arm_neon.h>
#endif
#endif

// Macro: WAH_DEBUG [user-definable]
//   If defined, enables internal correctness checks and debug logging.
//
// Macro: WAH_ASSERT(cond) [user-definable]
//   Assertion macro for internal correctness checks. Enabled when WAH_DEBUG is defined.
//   Can also be overriden to integrate with the user's logging/assertion system.
//
//   Assertions are only used for always-true conditions as a correctness check.
//   Any possible runtime error should be checked with WAH_ENSURE etc. instead.
//   Exception: Public interfaces without wah_error_t return type may use WAH_ASSERT as a last resort.
#ifndef WAH_ASSERT
#ifdef WAH_DEBUG
#define WAH_ASSERT(cond) assert(cond)
#else
#define WAH_ASSERT(cond) ((void)(cond))
#endif
#endif

// Macro: WAH_HAS_BUILTIN(x)
//   If evaluates to true, the compiler definitely supports the builtin function `__builtin_x`.
#ifdef __has_builtin
#define WAH_HAS_BUILTIN(x) __has_builtin(x)
#else
#define WAH_HAS_BUILTIN(x) 0
#endif

// Macro: WAH_HAS_FEATURE(x)
//   If evaluates to true, the compiler definitely supports the feature named as `x` as named by Clang.
#ifdef __has_feature
#define WAH_HAS_FEATURE(x) __has_feature(x)
#else
#define WAH_HAS_FEATURE(x) 0
#endif

// Macro: WAH_HAS_ATTRIBUTE(x)
//   If evaluates to true, the compiler definitely supports `__attribute__((x))` or `[[x]]`.
#ifdef __has_attribute
#define WAH_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#define WAH_HAS_ATTRIBUTE(x) 0
#endif

// Macro: WAH_ALIGNAS(N)
//   C99-compatible alias of `_Alignas` or `alignas` keyword.
#if defined(__cplusplus) && __cplusplus >= 201103L
#define WAH_ALIGNAS(N) alignas(N)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WAH_ALIGNAS(N) _Alignas(N)
#elif defined(_MSC_VER)
#define WAH_ALIGNAS(N) __declspec(align(N))
#elif defined(__GNUC__) || defined(__clang__)
#define WAH_ALIGNAS(N) __attribute__((aligned(N)))
#else
#define WAH_ALIGNAS(N)
#endif

// Macro: WAH_ALIGNOF(T)
//   C99-compatible alias of `_Alignof` or `alignof` operator.
#if defined(__cplusplus) && __cplusplus >= 201103L
#define WAH_ALIGNOF(T) alignof(T)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WAH_ALIGNOF(T) _Alignof(T)
#elif defined(_MSC_VER)
#define WAH_ALIGNOF(T) __alignof(T)
#elif defined(__GNUC__) || defined(__clang__)
#define WAH_ALIGNOF(T) __alignof__(T)
#else
#define WAH_ALIGNOF(T) offsetof(struct { char c; T t; }, t)
#endif

// Enum: wah_error_t
//   Error codes returned by most WAH functions.
//   Negative values indicate errors, non-negative values indicate status codes.
typedef enum {
    WAH_OK = 0,                            // Success, nothing special
    WAH_ERROR_INVALID_MAGIC_NUMBER = -1,   // Not a WebAssembly module at all
    WAH_ERROR_INVALID_VERSION = -2,        // Unsupported WebAssembly version
    WAH_ERROR_UNEXPECTED_EOF = -3,         // Unexpected end of input while parsing
    WAH_ERROR_MALFORMED = -4,              // Malformed WebAssembly module
    WAH_ERROR_TOO_LARGE = -5,              // Module is too large to process
    WAH_ERROR_OUT_OF_MEMORY = -6,          // Memory allocation failed
    WAH_ERROR_VALIDATION_FAILED = -7,      // Module failed validation (e.g. type errors)
    WAH_ERROR_TRAP = -8,                   // Runtime trap (e.g. division by zero, invalid memory access)
    WAH_ERROR_STACK_OVERFLOW = -9,         // Exceeded maximum call stack depth
    WAH_ERROR_MEMORY_OUT_OF_BOUNDS = -10,  // Accessed memory out of bounds
    WAH_ERROR_NOT_FOUND = -11,             // Import or export not found
    WAH_ERROR_MISUSE = -12,                // API misuse detected
    WAH_ERROR_BAD_SPEC = -13,              // Bad type specification in `wah_define_type` etc.
    WAH_ERROR_LINK_FAILED = -14,           // Link failed due to missing or incompatible module etc.
    WAH_ERROR_EXCEPTION = -15,             // Uncaught exception
    WAH_ERROR_DISABLED_FEATURE = -16,      // Used a feature not in WAH_COMPILED_FEATURES
    WAH_ERROR_MULTI_RETURN = -17,          // Would return multiple values if not disallowed
    WAH_STATUS_FUEL_EXHAUSTED = 2,         // Execution fuel exhausted, can be resumed
    WAH_STATUS_YIELDED = 3,                // Execution yielded by interrupt, can be resumed
} wah_error_t;

// Enum: wah_exec_state_t
//   Execution state of given module instance.
//
//   ```
//     +------------------+------------------------------------------+----------------+
//    \|/                 | cancel                                   | finish/cancel  | cancel
//   READY --start--> SUSPENDED --resume--> RUNNING --complete--> FINISHED            |
//                       /|\                 |   |                                    |
//                        +----yield/fuel----+   +-----error----> TRAPPED ------------+
//   ```
typedef enum {
    WAH_EXEC_READY = 0,  // Ready to start execution
    WAH_EXEC_RUNNING,    // Execution on the way
    WAH_EXEC_SUSPENDED,  // Execution suspended by interrupt or fuel exhausted, can be resumed
    WAH_EXEC_FINISHED,   // Execution finished, ready to start another execution
    WAH_EXEC_TRAPPED,    // Execution trapped
} wah_exec_state_t;

// Struct: wah_alloc_t
//   Custom allocator interface.
typedef struct {
    // Field: malloc
    //   Should allocate and return a block of memory of at least the given size,
    //   or return NULL on failure. `size` is guaranteed to be non-zero.
    void *(*malloc)(size_t size, void *userdata);
    // Field: realloc
    //   Should resize the given memory block to at least the given size,
    //   or return NULL on failure without deallocating the original block.
    //   `ptr` is guaranteed to be non-NULL and `size` is guaranteed to be non-zero.
    void *(*realloc)(void *ptr, size_t size, void *userdata);
    // Field: free
    //   Should deallocate the given memory block. `ptr` is guaranteed to be non-NULL.
    void (*free)(void *ptr, void *userdata);
    // Field: userdata
    //   Will be passed to the malloc/realloc/free functions.
    void *userdata;
} wah_alloc_t;

// Union: wah_v128_t
//   128-bit vector type.
typedef union {
    WAH_ALIGNAS(16)
    uint8_t u8[16]; uint16_t u16[8]; uint32_t u32[4]; uint64_t u64[2];
    int8_t i8[16]; int16_t i16[8]; int32_t i32[4]; int64_t i64[2];
    float f32[4]; double f64[2];
} wah_v128_t;

typedef char wah_v128_align_check_[WAH_ALIGNOF(wah_v128_t) >= 16 ? 1 : -1];

// Union: wah_value_t
//   Value type for WebAssembly values and references. The type is described elsewhere.
typedef union {
    int32_t i32;
    int64_t i64;
    float f32;
    double f64;
    wah_v128_t v128;

    // Field: ref
    //   Can hold any reference type (funcref, externref, structref, arrayref, exnref, ...).
    //   Pointers returned by `wah_gc_alloc_host` are always valid externrefs or anyrefs.
    //   Other types have private representations and shouldn't be directly used.
    void* ref;

    // Internal fields
#ifdef WAH_IMPLEMENTATION
    struct { void *sentinel; uint32_t func_idx; } _prefuncref; // const_expr funcref intermediate
#ifdef WAH_X86_64
    __m128i _m128i; __m128 _m128; __m128d _m128d;
#elif defined(WAH_AARCH64)
    uint8x16_t _u8x16;
#endif
#endif
} wah_value_t;

// Typedef: wah_type_t
//   Compact type representation. Non-negative values are type indices;
//   negative values are built-in types (based on their binary encodings).
//   Even values are non-nullable, odd values are nullable (for ref types).
typedef int32_t wah_type_t;

// Macros: WAH_TYPE_*
//   Built-in type values. Any other negative values are reserved.
#define WAH_TYPE_I32           -2   // i32
#define WAH_TYPE_I64           -4   // i64
#define WAH_TYPE_F32           -6   // f32
#define WAH_TYPE_F64           -8   // f64
#define WAH_TYPE_V128          -10  // v128
#define WAH_TYPE_PACKED_I8     -16  // i8 (packed only)
#define WAH_TYPE_PACKED_I16    -18  // i16 (packed only)
#define WAH_TYPE_FUNC          -32  // (ref func)
#define WAH_TYPE_FUNCREF       -31  // (ref null func) = funcref
#define WAH_TYPE_EXTERN        -34  // (ref extern)
#define WAH_TYPE_EXTERNREF     -33  // (ref null extern) = externref
#define WAH_TYPE_ANY           -36  // (ref any)
#define WAH_TYPE_ANYREF        -35  // (ref null any) = anyref
#define WAH_TYPE_EQ            -38  // (ref eq)
#define WAH_TYPE_EQREF         -37  // (ref null eq) = eqref
#define WAH_TYPE_I31           -40  // (ref i31)
#define WAH_TYPE_I31REF        -39  // (ref null i31) = i31ref
#define WAH_TYPE_STRUCT        -42  // (ref struct)
#define WAH_TYPE_STRUCTREF     -41  // (ref null struct) = structref
#define WAH_TYPE_ARRAY         -44  // (ref array)
#define WAH_TYPE_ARRAYREF      -43  // (ref null array) = arrayref
#define WAH_TYPE_EXN           -46  // (ref exn)
#define WAH_TYPE_EXNREF        -45  // (ref null exn) = exnref
#define WAH_TYPE_NOFUNC        -48  // (ref nofunc)
#define WAH_TYPE_NULLFUNCREF   -47  // (ref null nofunc) = nullfuncref
#define WAH_TYPE_NOEXTERN      -50  // (ref noextern)
#define WAH_TYPE_NULLEXTERNREF -49  // (ref null noextern) = nullexternref
#define WAH_TYPE_NONE          -52  // (ref none)
#define WAH_TYPE_NULLREF       -51  // (ref null none) = nullref
#define WAH_TYPE_NOEXN         -54  // (ref noexn)
#define WAH_TYPE_NULLEXNREF    -53  // (ref null noexn) = nullexnref

// Macro: WAH_TYPE_IS_NULLABLE(t)
//   Returns true if the given type is nullable (i.e. a reference type with nullability).
#define WAH_TYPE_IS_NULLABLE(t)  ((t) & 1)
// Macro: WAH_TYPE_AS_NULLABLE(t)
//   Converts the given type to its nullable version. Only valid for ref types.
#define WAH_TYPE_AS_NULLABLE(t)  ((t) | 1)
// Macro: WAH_TYPE_AS_NON_NULL(t)
//   Converts the given type to its non-nullable version. Only valid for ref types.
#define WAH_TYPE_AS_NON_NULL(t)  ((t) & ~(wah_type_t)1)

// Macro: WAH_TYPE_IS_PACKED(t)
//   Returns true if the given type is a packed integer type (i8 or i16).
#define WAH_TYPE_IS_PACKED(t)    ((t) == WAH_TYPE_PACKED_I8 || (t) == WAH_TYPE_PACKED_I16)
// Macro: WAH_TYPE_IS_REF(t)
//   Returns true if the given type is any kind of reference type (including nullref).
#define WAH_TYPE_IS_REF(t)       (((t) <= -31 && (t) >= -54) || (t) >= 0)

// Typedef: wah_features_t
//   Bitmap of supported WebAssembly features. See WAH_FEATURE_* macros below.
typedef uint64_t wah_features_t;

// Macros: WAH_FEATURE_*
//   Bit flags for supported WebAssembly proposals.
#define WAH_FEATURE_MVP              (UINT64_C(1) << 0)   // 1.0 MVP, always implied even when not given
#define WAH_FEATURE_MULTI_VALUE      (UINT64_C(1) << 1)   // Multi-value (2.0)
#define WAH_FEATURE_MUTABLE_GLOBALS  (UINT64_C(1) << 2)   // Mutable globals (2.0)
#define WAH_FEATURE_SIGN_EXT         (UINT64_C(1) << 3)   // Sign-extension operators (2.0)
#define WAH_FEATURE_NONTRAPPING_F2I  (UINT64_C(1) << 4)   // Non-trapping float-to-int conversions (2.0)
#define WAH_FEATURE_BULK_MEMORY      (UINT64_C(1) << 5)   // Bulk memory operations (2.0)
#define WAH_FEATURE_REF_TYPES        (UINT64_C(1) << 6)   // Reference types (2.0)
#define WAH_FEATURE_SIMD             (UINT64_C(1) << 7)   // 128-bit SIMD (2.0)
#define WAH_FEATURE_TAIL_CALL        (UINT64_C(1) << 8)   // Tail calls (3.0)
#define WAH_FEATURE_EXCEPTION        (UINT64_C(1) << 9)   // Exception handling (3.0)
#define WAH_FEATURE_GC               (UINT64_C(1) << 10)  // GC (3.0)
#define WAH_FEATURE_TYPED_FUNCREF    (UINT64_C(1) << 11)  // Typed funcrefs (3.0)
#define WAH_FEATURE_MEMORY64         (UINT64_C(1) << 12)  // Memory64 & table64 (3.0)
#define WAH_FEATURE_RELAXED_SIMD     (UINT64_C(1) << 13)  // Relaxed SIMD (3.0)

// Macro: WAH_FEATURE_WASM_V2
//   WebAssembly 2.0 features.
#define WAH_FEATURE_WASM_V2 ( \
    WAH_FEATURE_MVP | WAH_FEATURE_MULTI_VALUE | WAH_FEATURE_MUTABLE_GLOBALS | WAH_FEATURE_SIGN_EXT | \
    WAH_FEATURE_NONTRAPPING_F2I | WAH_FEATURE_BULK_MEMORY | WAH_FEATURE_REF_TYPES | WAH_FEATURE_SIMD)
// Macro: WAH_FEATURE_WASM_V3
//   WebAssembly 3.0 features.
#define WAH_FEATURE_WASM_V3 ( \
    WAH_FEATURE_WASM_V2 | WAH_FEATURE_TAIL_CALL | WAH_FEATURE_EXCEPTION | \
    WAH_FEATURE_GC | WAH_FEATURE_TYPED_FUNCREF | WAH_FEATURE_MEMORY64 | WAH_FEATURE_RELAXED_SIMD)
// Macro: WAH_FEATURE_ALL
//   A bitmap containing every supported feature.
#define WAH_FEATURE_ALL WAH_FEATURE_WASM_V3

// Macro: WAH_COMPILED_FEATURES [user-definable, default = WAH_FEATURE_ALL]
//   If set, restricts the compiled interpreter to only support the specified features.
#ifndef WAH_COMPILED_FEATURES
#define WAH_COMPILED_FEATURES WAH_FEATURE_ALL
#endif

// Macro: WAH_DEFAULT_FEATURES [user-definable, default = WAH_COMPILED_FEATURES]
//   If set, specifies the default enabled features for parsing and execution when not explicitly specified.
#ifndef WAH_DEFAULT_FEATURES
#define WAH_DEFAULT_FEATURES WAH_COMPILED_FEATURES
#endif

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_RELAXED_SIMD) && !((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)
#error "WAH_FEATURE_RELAXED_SIMD requires WAH_FEATURE_SIMD"
#endif
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC) && !((WAH_COMPILED_FEATURES) & WAH_FEATURE_REF_TYPES)
#error "WAH_FEATURE_GC requires WAH_FEATURE_REF_TYPES"
#endif
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_TYPED_FUNCREF) && !((WAH_COMPILED_FEATURES) & WAH_FEATURE_REF_TYPES)
#error "WAH_FEATURE_TYPED_FUNCREF requires WAH_FEATURE_REF_TYPES"
#endif

// Macro: WAH_FORCE_MUSTTAIL [user-definable]
// Macro: WAH_FORCE_COMPUTED_GOTO [user-definable]
// Macro: WAH_FORCE_SWITCH [user-definable]
//   If one of these is defined, forces the interpreter to use the corresponding dispatch method.
//   The interpreter otherwise tries the first dispatch scheme available for given compiler.
//   It is a undefined behavior that more than one of these macros are defined.
//
// Macro: WAH_USE_MUSTTAIL
//   Defined if the interpreter is using guaranteed tail call dispatch.
//   Available since GCC 15 and Clang 13. At least as fast as the computed goto dispatch.
//
// Macro: WAH_USE_COMPUTED_GOTO
//   Defined if the interpreter is using computed goto dispatch.
//   Available on GCC and Clang, but not MSVC. Much faster than the switch-based portable dispatch.
#if defined(WAH_FORCE_SWITCH)
// Force switch-based dispatch; skip musttail and computed goto.
#elif defined(WAH_FORCE_COMPUTED_GOTO)
#define WAH_USE_COMPUTED_GOTO
#elif defined(WAH_FORCE_MUSTTAIL)
#define WAH_USE_MUSTTAIL
#elif WAH_HAS_ATTRIBUTE(musttail)
#define WAH_USE_MUSTTAIL // clang 13+, GCC 15+
#elif defined(__GNUC__) || defined(__clang__)
#define WAH_USE_COMPUTED_GOTO
#endif

// Macro: WAH_BULK_CHECK_INTERVAL [user-definable, default = 16777216]
//   How many items have to be processed in a bulk operation (e.g. memory.copy, table.init)
//   before checking for fuel exhaustion and interrupts.
//
//   Fuel metering is still accurately done regardless of this interval, but higher values
//   may cause longer stalls on interrupts while lower values may cause more overhead
//   from checking too frequently. The default targets the interval around 1--10 milliseconds.
#ifndef WAH_BULK_CHECK_INTERVAL
#define WAH_BULK_CHECK_INTERVAL (1u << 24)
#endif

// Macro: WAH_BULK_ITEMS_PER_FUEL [user-definable, default = 4096]
//   When fuel metering is enabled, determines how many items can be processed per unit fuel
//   in bulk operations (e.g. memory.copy, table.init and so on).
//   This prevents excessively long opcode execution without consuming any fuel.
#ifndef WAH_BULK_ITEMS_PER_FUEL
#define WAH_BULK_ITEMS_PER_FUEL 4096
#endif

// Macro: WAH_NO_THREADS [user-definable]
//   If defined before including wah.h in the implementation translation unit, disables
//   threaded features. Currently this affects only the deadline timer: configuring
//   `wah_limits_t::deadline_us` will then fail with WAH_ERROR_DISABLED_FEATURE.
//
//   Auto-defined on platforms without Win32 or pthreads support. Define explicitly to
//   force a single-threaded build on platforms that would otherwise have threads.
#if !defined(WAH_NO_THREADS) && !defined(_WIN32) && !defined(__unix__) && !defined(__APPLE__) && !defined(_POSIX_THREADS)
#define WAH_NO_THREADS
#endif

// Enum: wah_comp_type_kind_t
//   The kind of a component type (function, struct or array).
typedef enum {
    // Variant: WAH_COMP_FUNC
    //   A function type.
    WAH_COMP_FUNC   = 0x60,
    // Variant: WAH_COMP_STRUCT
    //   A struct type.
    WAH_COMP_STRUCT = 0x5F,
    // Variant: WAH_COMP_ARRAY
    //   An array type.
    WAH_COMP_ARRAY  = 0x5E,
} wah_comp_type_kind_t;

// Macros: WAH_KIND_*
//   The entity kind of specific import or export.
#define WAH_KIND_FUNCTION 0  // Imported or exported function
#define WAH_KIND_TABLE    1  // Imported or exported table
#define WAH_KIND_MEMORY   2  // Imported or exported memory
#define WAH_KIND_GLOBAL   3  // Imported or exported global
#define WAH_KIND_TAG      4  // Imported or exported tag

// Struct: wah_type_desc_t
//   Describes a defined type.
typedef struct {
    // Field: kind
    //   The kind of the type, one of WAH_COMP_* macros.
    wah_comp_type_kind_t kind;
    // Field: is_final
    //   Set to true if the type is not open for subtyping.
    bool is_final;
    // Field: supertype
    //   The index of the supertype designated, or UINT32_MAX if none.
    uint32_t supertype;
    // Field: param_count
    // Field: result_count
    //   The number of parameters and results of the function type, or 0 otherwise.
    uint32_t param_count, result_count;
    // Field: param_types [borrowed]
    // Field: result_types [borrowed]
    //   The parameter and result types of the function type, or NULL otherwise.
    const wah_type_t *param_types, *result_types;
    // Field: field_count
    //   The number of fields of the struct or array type, or 0 otherwise.
    //   Note that arrays only have one implicit field representing the element type.
    uint32_t field_count;
    // Field: field_types [borrowed]
    //   The field types of the struct or array type, or NULL otherwise.
    const wah_type_t *field_types;
    // Field: field_mutables [borrowed]
    //   The mutability of fields of the struct type, or NULL otherwise.
    const bool *field_mutables;
} wah_type_desc_t;

// Struct: wah_func_desc_t
//   Describes a defined or imported function.
typedef struct {
    // Field: is_import
    //   Set to true if the function is imported, false if defined in the module.
    bool is_import;
    // Field: is_host
    //   Set to true if the function is a host function (i.e. implemented in C).
    bool is_host;
    // Field: type_index
    //   The index of the function type in the module's type section.
    uint32_t type_index;
    // Field: param_count
    // Field: result_count
    //   The number of parameters and results of the function type.
    uint32_t param_count, result_count;
    // Field: param_types [borrowed]
    // Field: result_types [borrowed]
    //   The parameter and result types of the function type.
    const wah_type_t *param_types, *result_types;
} wah_func_desc_t;

// Struct: wah_global_desc_t
//   Describes a defined or imported global variable.
typedef struct {
    // Field: type
    //   The type of the global variable.
    wah_type_t type;
    // Field: is_mutable
    //   Set to true if the global variable is mutable.
    bool is_mutable;
} wah_global_desc_t;

// Struct: wah_memory_desc_t
//   Describes a defined or imported linear memory.
typedef struct {
    // Field: addr_type
    //   The address type of the memory, either WAH_TYPE_I32 or WAH_TYPE_I64.
    wah_type_t addr_type;
    // Field: min_pages
    // Field: max_pages
    //   The initial and maximum size of the memory in WebAssembly pages (64KiB each).
    //   For `max_pages`, UINT64_MAX means no maximum limit.
    uint64_t min_pages, max_pages;
} wah_memory_desc_t;

// Struct: wah_table_desc_t
//   Describes a defined or imported table.
typedef struct {
    // Field: elem_type
    //   The type of elements in the table.
    wah_type_t elem_type;
    // Field: addr_type
    //   The address type of the table, either WAH_TYPE_I32 or WAH_TYPE_I64.
    wah_type_t addr_type;
    // Field: min_elements
    // Field: max_elements
    //   The initial and maximum number of elements in the table.
    //   For `max_elements`, UINT64_MAX means no maximum limit.
    uint64_t min_elements, max_elements;
} wah_table_desc_t;

// Struct: wah_tag_desc_t
//   Describes a defined or imported tag.
typedef struct {
    // Field: type_index
    //   The index of function that mirrors the tag type.
    uint32_t type_index;
    // Field: param_count
    //   The number of parameters of the tag type.
    uint32_t param_count;
    // Field: param_types [borrowed]
    //   The parameter types of the tag type.
    const wah_type_t *param_types;
} wah_tag_desc_t;

// Union: wah_extern_desc_t
//   Description of an imported or exported external entity.
typedef union {
    wah_func_desc_t func;
    wah_table_desc_t table;
    wah_memory_desc_t memory;
    wah_global_desc_t global;
    wah_tag_desc_t tag;
} wah_extern_desc_t;

// Struct: wah_import_desc_t
//   Describes an imported entity (function, table, memory, global or tag).
typedef struct {
    // Field: kind
    //   The kind of the imported entity, one of WAH_KIND_* macros.
    uint8_t kind;
    // Field: index
    //   The index of the imported entity.
    uint32_t index;
    // Field: module [borrowed]
    //   The zero-terminated name of the module from which the entity is imported.
    const char *module;
    // Field: module_len
    //   The length of the module name.
    size_t module_len;
    // Field: name [borrowed]
    //   The zero-terminated name of the entity to import.
    const char *name;
    // Field: name_len
    //   The length of the entity name.
    size_t name_len;
    // Field: u
    //   Contains one of wah_*_desc_t types depending on the `kind`.
    wah_extern_desc_t u;
} wah_import_desc_t;

// Struct: wah_export_desc_t
//   Describes an exported entity (function, table, memory, global or tag).
typedef struct {
    // Field: kind
    //   The kind of the exported entity, one of WAH_KIND_* macros.
    uint8_t kind;
    // Field: index
    //   The index of the exported entity.
    uint32_t index;
    // Field: name [borrowed]
    //   The zero-terminated name of the entity to export.
    const char *name;
    // Field: name_len
    //   The length of the entity name.
    size_t name_len;
    // Field: u
    //   Contains one of wah_*_desc_t types depending on the `kind`.
    wah_extern_desc_t u;
} wah_export_desc_t;

// Struct: wah_call_context_t
//   The context available to host functions when they are called.
typedef struct wah_call_context_s wah_call_context_t;

// Typedef: wah_func_t
//   Host can define a function of this type and export it to WebAssembly as a host function.
//
//   - ctx [in, borrowed]: Pointer to a `wah_call_context_t` struct for interacting
//     with the current execution. Valid only for the duration of the host call.
//   - userdata [in, borrowed]: The `userdata` field given on registration.
//
//   GC host boundary policy: GC-managed references passed to host functions
//   (via params or accessible through the exec context) are valid only for the
//   duration of the host call. Host code must NOT retain pointers to managed
//   objects after returning. Violating this policy causes undefined behavior
//   when the GC relocates or frees the referenced object.
typedef void (*wah_func_t)(wah_call_context_t *ctx, void *userdata);

// Typedef: wah_finalize_t
//   Can be given with `wah_func_t` to specify a finalizer function that will be called
//   when the host function is unregistered or when the module instance is deallocated.
//
//   - userdata [in, borrowed]: The `userdata` field given on registration.
typedef void (*wah_finalize_t)(void *userdata);

#if defined(__cplusplus) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif

// Struct: wah_module_t
//   Opaque type representing a parsed WebAssembly module. Immovable after initial linkage.
//
//   This type is meant to be stack-allocatable and has a fixed size for ABI compatibility.
typedef struct wah_module_s {
#ifndef WAH_IMPLEMENTATION
#ifdef __cplusplus
private:
#endif
    WAH_ALIGNAS(16) char reserved[384];
#else
    WAH_ALIGNAS(16)
    uint32_t type_count;
    uint32_t wasm_function_count;
    uint32_t code_count;
    uint32_t global_count;
    uint32_t memory_count;
    uint32_t table_count;
    uint32_t element_segment_count;
    uint32_t data_segment_count;
    uint32_t tag_count;
    uint32_t import_count;
    uint32_t export_count;

    uint32_t start_function_idx;
    bool has_start_function;
    bool has_data_count_section; // True if data count section was present
    uint32_t min_data_segment_count_required;

    struct wah_func_type_s *types;
    struct wah_type_def_s *type_defs;
    uint32_t types_cap;
    uint32_t *function_type_indices; // Index into the types array
    struct wah_code_body_s *code_bodies;
    struct wah_global_s *globals;
    struct wah_memory_type_s *memories;
    struct wah_table_type_s *tables;
    struct wah_element_segment_s *element_segments;
    struct wah_data_segment_s *data_segments;
    struct wah_tag_s *tags;
    wah_import_desc_t *imports;
    struct wah_export_s *exports;

    // Unified function table: functions[0..wasm_function_count) are WASM functions,
    // functions[wasm_function_count..local_function_count) are host functions.
    uint32_t local_function_count;
    uint32_t functions_cap;    // allocated capacity of functions[]
    struct wah_function_s *functions; // unified function array

    // Import section
    uint32_t import_function_count;
    uint32_t import_table_count;
    uint32_t import_memory_count;
    uint32_t import_global_count;
    uint32_t import_tag_count;

    struct wah_func_import_s *func_imports;
    struct wah_table_import_s *table_imports;
    struct wah_memory_import_s *memory_imports;
    struct wah_global_import_s *global_imports;
    struct wah_tag_import_s *tag_imports;

    // Dynamic export array growth
    uint32_t exports_cap;  // Capacity for dynamic export array growth

    // Repr metadata for GC types
    uint32_t repr_count;
    struct wah_repr_info_s **repr_infos;
    int32_t *typeidx_to_repr; // int32_t = wah_repr_t (private)
    struct wah_repr_set_s *type_cast_sets;

    // Canonical type map: canonical_map[i] = j means type i is canonically equal to type j
    uint32_t *canonical_map;

    // Bitmap of "declared" function indices (for ref.func validation).
    // Built at code-section parse time from exports, element segments, and global init exprs.
    uint8_t *declared_funcs;

    wah_alloc_t alloc;
    wah_features_t enabled_features;
    wah_features_t required_features;
    bool fuel_metering;

    void *reserved; // Ensure that at least one pimpl pointer can be added
#endif
} wah_module_t;

#ifdef WAH_IMPLEMENTATION
// Atomic flag for fast POLL check. Setters may be called from another thread;
// the interpreter hot loop only does a relaxed load.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
typedef atomic_int wah_poll_flag_t;
#define WAH_POLL_FLAG_LOAD(f)       atomic_load_explicit(&(f), memory_order_relaxed)
#define WAH_POLL_FLAG_STORE(f, v)   atomic_store_explicit(&(f), (v), memory_order_relaxed)
#elif defined(_MSC_VER)
typedef volatile long wah_poll_flag_t;
#define WAH_POLL_FLAG_LOAD(f)       (f)
#define WAH_POLL_FLAG_STORE(f, v)   _InterlockedExchange(&(f), (v))
#elif defined(__GNUC__)
typedef volatile int wah_poll_flag_t;
#define WAH_POLL_FLAG_LOAD(f)       __atomic_load_n(&(f), __ATOMIC_RELAXED)
#define WAH_POLL_FLAG_STORE(f, v)   __atomic_store_n(&(f), (v), __ATOMIC_RELAXED)
#else
typedef volatile int wah_poll_flag_t;
#define WAH_POLL_FLAG_LOAD(f)       (f)
#define WAH_POLL_FLAG_STORE(f, v)   ((f) = (v))
#endif
#endif

// Struct: wah_exec_context_t
//   Opaque and immovable type representing an instantiated module ready for execution.
//
//   This type is meant to be stack-allocatable and has a fixed size for ABI compatibility.
typedef struct wah_exec_context_s {
#ifndef WAH_IMPLEMENTATION
#ifdef __cplusplus
private:
#endif
    WAH_ALIGNAS(16) char reserved[384];
#else
    WAH_ALIGNAS(16)
    // Unified stack: values grow upward from stack_buffer, call frames grow
    // downward from stack_end. Overflow when the two regions meet.
    uint8_t *stack_buffer; // Base of the unified stack allocation
    uint64_t stack_buffer_size;     // Total bytes allocated for stack_buffer
    wah_value_t *value_stack;       // = (wah_value_t *)stack_buffer
    wah_value_t *sp;                // Value stack pointer (next free slot)
    struct wah_call_frame_s *frame_ptr; // Next free frame slot (grows downward from stack_end)
    uint32_t call_depth;            // Current call depth

    wah_poll_flag_t poll_flag;      // Fast path: non-zero means slow path needed
    wah_poll_flag_t interrupt_flag; // Cross-thread cooperative interruption

    wah_value_t *globals;           // Mutable global values
    uint32_t global_count;

    const struct wah_module_s *module;

    // Memory 0 fast path (kept in sync with memories[0].data/memories[0].size)
    uint8_t *memory_base; // Pointer to memory 0 (for i32_i32_mem0 ops; will be moved to register)
    uint64_t memory_size; // Size of memory 0 in bytes

    struct wah_memory_inst_s *memories; // Array[memory_count], memories[0].data kept in sync with memory_base/memory_size
    uint32_t memory_count;

    struct wah_table_inst_s *tables; // Array[table_count]
    uint32_t table_count;

    // Linkage support
    struct wah_linked_module_s *linked_modules;
    uint32_t linked_module_count;
    uint32_t linked_modules_cap;
    bool is_instantiated;

    // Runtime dispatch table (global function index space: imports + locals + hosts)
    struct wah_function_s *function_table;
    uint32_t function_table_count;

    // Tag instances (global tag index space: import tags + local tags)
    struct wah_tag_instance_s *tag_instances;
    uint32_t tag_instance_count;

    // Pending exception (set by throw, consumed by try_table catch or propagated)
    struct wah_exception_s *pending_exception;

    // Exception handler stack (try_table frames)
    struct wah_exception_handler_s *exception_handlers;
    uint32_t exception_handler_depth;

    // GC heap state (NULL when GC is not enabled)
    struct wah_gc_state_s *gc;

    int64_t fuel;
    uint64_t deadline_us;
    struct wah_timer_s *timer;

    uint64_t max_memory_bytes;      // UINT64_MAX = no limit; 0 is a valid limit
    uint64_t memory_bytes_committed; // current total: linear mem + tables + GC heap

    struct wah_exec_lifecycle_s {
        wah_exec_state_t state;
        wah_error_t stop_reason;
        uint32_t entry_result_count;
        uint32_t entry_param_count;
        const struct wah_function_s *entry_host_fn;
        wah_value_t *base_sp;
        uint32_t base_call_depth;
        uint32_t base_handler_depth;
        struct wah_call_frame_s *base_frame_ptr;
    } lifecycle;

    struct wah_type_check_cache_entry_s *type_check_cache;

    // Per-instance "dropped" state for element/data segments. The module is read-only
    // after parse, so segments cannot record dropped state on themselves -- otherwise
    // a single module reused across multiple instances would leak runtime state across
    // instances. These bitsets are sized to module->element_segment_count and
    // module->data_segment_count respectively. Declarative element segments are
    // pre-set during wah_instantiate().
    uint8_t *dropped_elem_segments;
    uint8_t *dropped_data_segments;

    wah_alloc_t alloc;
    wah_features_t enabled_features;

    void *reserved; // Ensure that at least one pimpl pointer can be added
#endif
} wah_exec_context_t;

#if defined(__cplusplus) && defined(__clang__)
#pragma clang diagnostic pop
#endif

typedef char wah_module_size_check_[sizeof(wah_module_t) <= 384 ? 1 : -1];
typedef char wah_module_align_check_[WAH_ALIGNOF(wah_module_t) == 16 ? 1 : -1];
typedef char wah_exec_context_size_check_[sizeof(wah_exec_context_t) <= 384 ? 1 : -1];
typedef char wah_exec_context_align_check_[WAH_ALIGNOF(wah_exec_context_t) == 16 ? 1 : -1];

// Function: wah_strerror
//   Converts an error code to a human-readable string.
//   Unlike API functions, the returned error string is not guaranteed to be stable.
const char *wah_strerror(wah_error_t err);

// Struct: wah_parse_options_t
//   Options for parsing a WebAssembly module.
typedef struct {
    // Field: features [default = WAH_DEFAULT_FEATURES if `wah_parse_options_t` is not given]
    //   Bitmap of WAH_FEATURE_* flags indicating which features to enable when parsing.
    wah_features_t features;
    // Field: enable_fuel_metering [default = false]
    //   Whether to enable fuel metering during execution.
    bool enable_fuel_metering;
    // Field: alloc [default = NULL]
    //   Custom allocator to be used for parsing and eventually by the returned module.
    //   The standard allocator is used if NULL is given.
    const wah_alloc_t *alloc;
} wah_parse_options_t;

// Function: wah_parse_module
//   Parses a WebAssembly binary into a `wah_module_t` struct.
//
//   - module [out, owned]: Pointer to an uninitialized `wah_module_t` struct.
//   - binary [in, borrowed]: Pointer to the WebAssembly binary data.
//   - binary_size [in]: Size of the WebAssembly binary data in bytes.
//   - options [in, borrowed, optional]: Parsing options. Can be NULL for defaults.
wah_error_t wah_parse_module(wah_module_t *module, const uint8_t *binary, size_t binary_size, const wah_parse_options_t *options);

// Functions: wah_module_*_count
//   Get the count of various entities in the module.
uint32_t wah_module_type_count(const wah_module_t *module);      // Count of defined types
uint32_t wah_module_function_count(const wah_module_t *module);  // Combined count of imported and defined functions
uint32_t wah_module_global_count(const wah_module_t *module);    // Combined count of imported and defined globals
uint32_t wah_module_memory_count(const wah_module_t *module);    // Combined count of imported and defined memories
uint32_t wah_module_table_count(const wah_module_t *module);     // Combined count of imported and defined tables
uint32_t wah_module_tag_count(const wah_module_t *module);       // Combined count of imported and defined tags
uint32_t wah_module_import_count(const wah_module_t *module);    // Total count of imports
uint32_t wah_module_export_count(const wah_module_t *module);    // Total count of exports

// Functions: wah_module_*
//   Get the description of various entities in the module by index.
//
//   - idx [in]: Index of the entity to query.
//   - out [out, borrowed]: Pointer to a description struct to be filled in.
//     Any borrowed pointers in `wah_*_desc_t` structs are only valid before the next API call.
wah_error_t wah_module_type(const wah_module_t *module, uint32_t idx, wah_type_desc_t *out);      // A defined type by index
wah_error_t wah_module_function(const wah_module_t *module, uint32_t idx, wah_func_desc_t *out);  // An imported or defined function by index
wah_error_t wah_module_global(const wah_module_t *module, uint32_t idx, wah_global_desc_t *out);  // An imported or defined global by index
wah_error_t wah_module_memory(const wah_module_t *module, uint32_t idx, wah_memory_desc_t *out);  // An imported or defined memory by index
wah_error_t wah_module_table(const wah_module_t *module, uint32_t idx, wah_table_desc_t *out);    // An imported or defined table by index
wah_error_t wah_module_tag(const wah_module_t *module, uint32_t idx, wah_tag_desc_t *out);        // An imported or defined tag by index
wah_error_t wah_module_import(const wah_module_t *module, uint32_t idx, wah_import_desc_t *out);  // An import by index
wah_error_t wah_module_export(const wah_module_t *module, size_t idx, wah_export_desc_t *out);    // An export by index

// Functions: wah_export_by_name
//   Get the description of an exported entity by name.
//
//   - name [in, borrowed]: Zero-terminated name of the export to query.
//   - out [out, borrowed]: Pointer to a description struct to be filled in.
wah_error_t wah_export_by_name(const wah_module_t *module, const char *name, wah_export_desc_t *out);

// Functions: wah_export_by_name_len
//   Get the description of an exported entity by name with a specified length.
//
//   - name [in, borrowed]: Name of the export to query. Can have embedded null bytes.
//   - name_len [in]: Length of the name.
//   - out [out, borrowed]: Pointer to a description struct to be filled in.
wah_error_t wah_export_by_name_len(const wah_module_t *module, const char *name, size_t name_len, wah_export_desc_t *out);

// --- Resource Limits ---

// Struct: wah_limits_t
//   Resource limits for execution. All limits are optional; 0 or false always mean unchanged,
//   which is the default when being initialized.
typedef struct wah_limits_s {
    // Field: max_stack_bytes [default = see below]
    //   Maximum total bytes for the unified value-call frame stack allocated prior to execution.
    //   The default value (1--2MB) is enough for 1024 nested calls with 64 stack values per each.
    //   (The actual recursion limit depends on stack usage of each call frame.)
    uint64_t max_stack_bytes;
    // Field: max_memory_bytes [default = UINT64_MAX (unlimited)]
    //   Maximum total bytes for linear memory, tables, and GC heap combined.
    //   Zero means the default (UINT64_MAX). Use `no_memory_bytes` to enforce a 0-byte limit.
    uint64_t max_memory_bytes;
    // Field: fuel [default = UINT64_MAX (unlimited)]
    //   Execution fuel. The interpreter will stop with WAH_STATUS_FUEL_EXHAUSTED when fuel runs out.
    //   Any WebAssembly opcode or ~WAH_BULK_ITEMS_PER_FUEL items handled consume a single fuel.
    //   Only applicable when `wah_parse_options_t::fuel_metering` was true for the primary module.
    uint64_t fuel;
    // Field: deadline_us [default = UINT64_MAX (no deadline)]
    //   Cooperative time limit in microseconds. Be aware that this is not precise nor deterministic.
    uint64_t deadline_us;
    // Field: no_memory_bytes [default = false]
    //   If true, enforces a 0-byte limit on all memory. Incompatible with any `max_memory_bytes` > 0.
    bool no_memory_bytes;
} wah_limits_t;

// Struct: wah_exec_options_t
//   Options for running WebAssembly modules.
typedef struct {
    // Field: limits
    //   Initial resource limits for execution. Can be set by `wah_set_limits` later.
    wah_limits_t limits;
    // Field: alloc
    //   Custom allocator to be used for execution. The standard allocator is used if NULL is given.
    //   Note that this is distinct from `wah_parse_options_t::alloc` and should be given separately.
    const wah_alloc_t *alloc;
} wah_exec_options_t;

// Function: wah_new_exec_context
//   Creates and initializes an execution context.
//
//   - exec_ctx [out, owned]: Pointer to an uninitialized `wah_exec_context_t` struct.
//   - module [in, borrowed]: Pointer to a parsed `wah_module_t` to execute.
//   - options [in, borrowed, optional]: Execution options. Can be NULL for defaults (all zeroed).
wah_error_t wah_new_exec_context(wah_exec_context_t *exec_ctx, const wah_module_t *module, const wah_exec_options_t *options);

// Function: wah_set_limits
//   Updates resource limits of an execution context.
//
//   - limits [in, borrowed]: Pointer to a `wah_limits_t` struct containing the new limits.
//     Any zero or false field is ignored (i.e. the corresponding limit is unchanged).
wah_error_t wah_set_limits(wah_exec_context_t *exec_ctx, const wah_limits_t *limits);

// Function: wah_get_limits
//   Retrieves the current resource limits of an execution context.
//
//   - out [out, borrowed]: Pointer to a `wah_limits_t` struct to be filled in with the current limits.
//     All fields are populated, except that 0 stands in for "unlimited" to match the default
//     initialization state and to keep get/set round-trips safe.
void wah_get_limits(const wah_exec_context_t *exec_ctx, wah_limits_t *out);

// Function: wah_free_exec_context
//   Frees resources of an execution context.
void wah_free_exec_context(wah_exec_context_t *exec_ctx);

// Function: wah_call
//   Calls a WebAssembly function by index.
//   Returns WAH_ERROR_MULTI_RETURN if the function has multiple return values.
//
//   - func_idx [in]: Index of the function to call. See `wah_export_desc_t.index`.
//   - params [in, borrowed, optional if param_count == 0]: Array of parameter values.
//     Must have at least `param_count` elements. Can be NULL if `param_count` is zero.
//   - param_count [in]: Number of parameters to pass.
//   - result [out, owned, optional]: Pointer to a `wah_value_t` to store the result.
//     Can be NULL if the result is not needed.
wah_error_t wah_call(wah_exec_context_t *exec_ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count, wah_value_t *result);

// Function: wah_call_multi
//   Calls a WebAssembly function by index with multiple return values.
//   Unlike wah_call, this function can receive multiple return values but truncates them
//   if the caller didn't provide enough space.
//
//   - func_idx [in]: Index of the function to call. See `wah_export_desc_t.index`.
//   - params [in, borrowed, optional if param_count == 0]: Array of parameter values.
//     Must have at least `param_count` elements. Can be NULL if `param_count` is zero.
//   - param_count [in]: Number of parameters to pass.
//   - results [out, owned, optional if max_result_count == 0]: Array to store the results.
//     Must have at least `max_result_count` elements. Can be NULL if `max_result_count` is zero.
//   - max_result_count [in]: Maximum number of results to store. Results will be truncated
//     if the function returns more than this number of values.
//   - actual_result_count [out, owned, optional]: Pointer to store the actual number of
//     results to be returned if `max_result_count` were large enough.
wah_error_t wah_call_multi(wah_exec_context_t *exec_ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count,
                           wah_value_t *results, uint32_t max_result_count, uint32_t *actual_result_count);

// Function: wah_call_by_name
//   Same as `wah_call`, but calls an exported WebAssembly function by name.
//
//   - name [in, borrowed]: Zero-terminated name of the export to call.
//   - params [in, borrowed, optional if param_count == 0]: Array of parameter values.
//     Must have at least `param_count` elements. Can be NULL if `param_count` is zero.
//   - param_count [in]: Number of parameters to pass.
//   - result [out, owned, optional]: Pointer to a `wah_value_t` to store the result.
//     Can be NULL if the result is not needed.
wah_error_t wah_call_by_name(wah_exec_context_t *exec_ctx, const char *name, const wah_value_t *params, uint32_t param_count, wah_value_t *result);

// Function: wah_set_fuel
//   Resets the available fuel for execution.
//
//   - fuel [in]: New fuel value. Can be set to INT64_MAX to effectively disable fuel exhaustion.
//
//   Returns WAH_ERROR_DISABLED_FEATURE if fuel metering is not enabled for the module
//   (`wah_parse_options_t::enable_fuel_metering` was false at parse time).
wah_error_t wah_set_fuel(wah_exec_context_t *ctx, int64_t fuel);

// Function: wah_get_fuel
//   Returns the remaining fuel for execution.
int64_t wah_get_fuel(const wah_exec_context_t *ctx);

// Function: wah_start
//   Sets up a call frame without executing.
//
//   - func_idx [in]: Index of the function to call. See `wah_export_desc_t::index`.
//   - params [in, borrowed, optional if param_count == 0]: Array of parameter values.
//     Must have at least `param_count` elements. Can be NULL if `param_count` is zero.
//   - param_count [in]: Number of parameters to pass.
wah_error_t wah_start(wah_exec_context_t *ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count);

// Function: wah_resume
//   Drives execution until completion, terminal error, or resumable stop.
wah_error_t wah_resume(wah_exec_context_t *ctx);

// Function: wah_finish
//   Extracts results after WAH_OK.
//
//   - results [out, owned, optional if max_result_count == 0]: Array to store the results.
//     Must have at least `max_result_count` elements. Can be NULL if `max_result_count` is zero.
//   - max_result_count [in]: Maximum number of results to store. Results will be truncated
//     if the function returns more than this number of values.
//   - actual_result_count [out, owned, optional]: Pointer to store the actual number of
//     results to be returned if `max_result_count` were large enough.
wah_error_t wah_finish(wah_exec_context_t *ctx, wah_value_t *results, uint32_t max_result_count, uint32_t *actual_result_count);

// Function: wah_cancel
//   Discards a suspended, finished or trapped activation.
//   After cancellation, the execution context can be safely destroyed or reused for another start.
void wah_cancel(wah_exec_context_t *ctx);

// Function: wah_is_suspended
//   Returns true if the execution context is currently waiting to be resumed.
bool wah_is_suspended(const wah_exec_context_t *ctx);

// Function: wah_exec_state
//   Returns the current execution state of the context.
wah_exec_state_t wah_exec_state(const wah_exec_context_t *ctx);

// Function: wah_free_module
//    Frees resources associated with a parsed module. The module pointer itself is not freed.
void wah_free_module(wah_module_t *module);

// --- Programmatically create modules ---

// Function: wah_new_module
//   Initializes an empty module for programmatic construction.
//
//   - alloc [in, borrowed, optional]: Custom allocator for constructing the module.
//     The standard allocator is used if NULL is given.
wah_error_t wah_new_module(wah_module_t *mod, const wah_alloc_t *alloc);

// Function: wah_define_type
//   Defines a new type in the module based on a type specification string and arguments.
//
//   The type specification is as follows:
//     fn (i32, f64) -> i32                // 2 arguments, 1 return
//     fn (i32, f64) -> (i32)              // Same as above
//     fn (i32, f64) -> (i32, f64)         // 2 arguments, 2 returns
//     fn (i32, f64) -> ()                 // 0 arguments, 0 returns
//     fn (i32, f64)                       // Same as above
//     struct { i32, mut funcref }         // 1 immutable field, 1 mutable field
//     struct { i32, mut ref null func, }  // Same as above, trailing comma is allowed
//     struct { i32, mut ref func }        // Same as above, but the second field is non-nullable now
//     array i32                           // Immutable array of i32
//     array mut anyref                    // Mutable array of anyref
//   A placeholder `%T` can go anywhere i32/f64/func/funcref/anyref/... are used above.
//   Its semantics strictly depend on its macro spelling (so `ref %T` accepts WAH_TYPE_FUNC
//   but no WAH_TYPE_FUNCREF). Packed types are only usable in struct fields and array elements.
//   In addition, prepending `fresh` ensures that the returned type is distinct from any other type;
//   by default structurally identical types are automatically merged.
//
//   - out_type [out]: Pointer to retrieve the (newly or previously) defined type index.
//   - spec [in, borrowed]: Type specification string as described above.
//   - ... [in]: Additional arguments for the type specification. `%T` accepts wah_type_t.
//
//   You can enable `WAH_DEBUG` if you receive `WAH_ERROR_BAD_SPEC`
//   to get more detailed error message via `WAH_LOG`.
wah_error_t wah_define_type(wah_module_t *mod, wah_type_t *out_type, const char *spec, ...);

// Function: wah_define_typev
//   Same as `wah_define_type` but with a `va_list` for arguments.
wah_error_t wah_define_typev(wah_module_t *mod, wah_type_t *out_type, const char *spec, va_list *args);

// Function: wah_export_func
//   Exports a host function from the module.
//
//   - name [in, borrowed]: Zero-terminated name of the export.
//   - types [in, borrowed]: Type specification string for the function signature.
//     Same as what `wah_define_type` accepts for function types, but without `%T`
//     and `fn` can be omitted as other types are forbidden in this context.
//   - func [in, consumed on success]: Function pointer to be called when the exported function is invoked.
//   - userdata [in, consumed on success]: User data pointer to be passed to the function on each call.
//   - finalize [in, consumed on success, optional]: Called with userdata when the module is freed.
//
//   `func`, `userdata` and `finalize` are consumed only on success; they remain caller-owned on failure.
wah_error_t wah_export_func(wah_module_t *mod, const char *name, const char *types, wah_func_t func, void *userdata, wah_finalize_t finalize);

// Function: wah_export_typed_func
//   Same as `wah_export_func` but with an explicit `wah_type_t` defined from `wah_define_type`.
//
//   - name [in, borrowed]: Zero-terminated name of the export.
//   - type [in]: Type index for the function signature, must be defined from `wah_define_type*`.
//   - func [in, consumed on success]: Function pointer to be called when the exported function is invoked.
//   - userdata [in, consumed on success]: User data pointer to be passed to the function on each call.
//   - finalize [in, consumed on success, optional]: Called with userdata when the module is freed.
//
//   `func`, `userdata` and `finalize` are consumed only on success; they remain caller-owned on failure.
wah_error_t wah_export_typed_func(wah_module_t *mod, const char *name, wah_type_t type, wah_func_t func, void *userdata, wah_finalize_t finalize);

// Function: wah_export_memory
//   Exports a new memory from the module.
//
//   - name [in, borrowed]: Zero-terminated name of the export.
//   - min_pages [in]: Initial memory size in WebAssembly pages (64KiB each).
//   - max_pages [in]: Optional maximum memory size in pages. Use UINT64_MAX for no maximum.
wah_error_t wah_export_memory(wah_module_t *mod, const char *name, uint64_t min_pages, uint64_t max_pages);

// Function: wah_export_global_*
//   Exports a new global variable from the module.
//
//   - name [in, borrowed]: Zero-terminated name of the export.
//   - is_mutable [in]: True if the global variable should be mutable, false if immutable.
//   - init_value [in]: Initial value of the global variable.
wah_error_t wah_export_global_i32(wah_module_t *mod, const char *name, bool is_mutable, int32_t init_value);  // Exports an i32 global variable
wah_error_t wah_export_global_i64(wah_module_t *mod, const char *name, bool is_mutable, int64_t init_value);  // Exports an i64 global variable
wah_error_t wah_export_global_f32(wah_module_t *mod, const char *name, bool is_mutable, float init_value);    // Exports an f32 global variable
wah_error_t wah_export_global_f64(wah_module_t *mod, const char *name, bool is_mutable, double init_value);   // Exports an f64 global variable
wah_error_t wah_export_global_v128(wah_module_t *mod, const char *name, bool is_mutable, const wah_v128_t *init_value); // Exports a v128 global variable

// --- Call context for host functions ---

// Function: wah_param_count
//   Returns the number of parameters passed to a host function.
//   Valid only during the host function call.
size_t wah_param_count(const wah_call_context_t *ctx);

// Function: wah_param_type
//   Returns the type of a parameter passed to a host function by index.
//   Valid only during the host function call.
//
//   - idx [in]: Index of the parameter to retrieve. Must be less than the number of parameters.
wah_type_t wah_param_type(const wah_call_context_t *ctx, size_t index);

// Functions: wah_param_*
//   Get parameters passed to a host function by index. Valid only during the host function call.
//
//   - idx [in]: Index of the parameter to retrieve. Must be less than the number of parameters.
int32_t wah_param_i32(const wah_call_context_t *ctx, size_t index);      // i32 parameter by index
int64_t wah_param_i64(const wah_call_context_t *ctx, size_t index);      // i64 parameter by index
float wah_param_f32(const wah_call_context_t *ctx, size_t index);        // f32 parameter by index
double wah_param_f64(const wah_call_context_t *ctx, size_t index);       // f64 parameter by index
wah_v128_t wah_param_v128(const wah_call_context_t *ctx, size_t index);  // v128 parameter by index

// Function: wah_param_ref
//   Get a `wah_gc_alloc_host`-compatible reference parameter passed to a host function by index.
//   Valid only during the host function call.
//
//   - idx [in]: Index of the parameter to retrieve. Must be less than the number of parameters.
void *wah_param_ref(const wah_call_context_t *ctx, size_t index);

// Function: wah_result_count
//   Returns the number of results expected for a host function.
//   Valid only during the host function call.
size_t wah_result_count(const wah_call_context_t *ctx);

// Function: wah_result_type
//   Returns the expected type of a result for a host function by index.
//   Valid only during the host function call.
//
//   - index [in]: Index of the result to retrieve. Must be less than the number of results.
wah_type_t wah_result_type(const wah_call_context_t *ctx, size_t index);

// Functions: wah_result_*
//   Set results for a host function by index. Valid only during the host function call.
//
//   - index [in]: Index of the result to set. Must be less than the number of results.
//   - value [in]: Value to set for the result.
void wah_result_i32(wah_call_context_t *ctx, size_t index, int32_t value);             // Sets i32 result by index
void wah_result_i64(wah_call_context_t *ctx, size_t index, int64_t value);             // Sets i64 result by index
void wah_result_f32(wah_call_context_t *ctx, size_t index, float value);               // Sets f32 result by index
void wah_result_f64(wah_call_context_t *ctx, size_t index, double value);              // Sets f64 result by index
void wah_result_v128(wah_call_context_t *ctx, size_t index, const wah_v128_t *value);  // Sets v128 result by index

// Function: wah_result_ref
//   Set a `wah_gc_alloc_host`-compatible reference result for a host function by index.
//   Valid only during the host function call.
//
//   - index [in]: Index of the result to set. Must be less than the number of results.
//   - value [in]: Value to set for the result. Can be NULL if the result type is nullable.
void wah_result_ref(wah_call_context_t *ctx, size_t index, void *value);

// Macros: wah_return_*
//   Convenience macros for setting a single return value at index 0.
//   Valid only during the host function call.
//
//   - value [in]: Value to set for the single return value.
#define wah_return_i32(ctx, value) wah_result_i32(ctx, 0, value)    // Sets a single i32 return value
#define wah_return_i64(ctx, value) wah_result_i64(ctx, 0, value)    // Sets a single i64 return value
#define wah_return_f32(ctx, value) wah_result_f32(ctx, 0, value)    // Sets a single f32 return value
#define wah_return_f64(ctx, value) wah_result_f64(ctx, 0, value)    // Sets a single f64 return value
#define wah_return_v128(ctx, value) wah_result_v128(ctx, 0, value)  // Sets a single v128 return value
#define wah_return_ref(ctx, value) wah_result_ref(ctx, 0, value)    // Sets a single reference return value

// Function: wah_trap
//   Traps from a host function with a specified reason.
//
//   - reason [in]: Error code to be propagated. Cannot be any non-error code like WAH_OK.
void wah_trap(wah_call_context_t *ctx, wah_error_t reason);

// --- Linkage ---

// Function: wah_link_module
//   Links a module into the primary module of the context under a specified name.
//   Can be called multiple times to link multiple modules.
//
//   - name [in, borrowed]: Name to link the module under. Must be unique among linked modules.
//   - mod [in, borrowed]: Module to link. Must outlive the execution context.
wah_error_t wah_link_module(wah_exec_context_t *ctx, const char *name, const wah_module_t *mod);

// Function: wah_link_context
//   Links another execution context into the primary module of the context under a specified name.
//   This allows sharing already-instantiated module instances, including runtime state
//   such as memories, tables, globals, and tags, between contexts.
//
//   - name [in, borrowed]: Name to link the context under. Must be unique among linked modules.
//   - linked_ctx [in, borrowed]: Execution context to link. Must outlive the primary context.
wah_error_t wah_link_context(wah_exec_context_t *ctx, const char *name, wah_exec_context_t *linked_ctx);

// Function: wah_instantiate
//   Instantiates the execution context, preparing it for execution.
//   This is optional since `wah_call` will automatically instantiate if it hasn't been done yet,
//   but it can be used as an early verification step after linking and before execution.
//
//   After instantiation, any `wah_link_*` calls are invalid since the module is finalized.
wah_error_t wah_instantiate(wah_exec_context_t *ctx);

// --- GC Management ---

// Enum: wah_gc_phase_t
//   GC phase for reporting and debugging purposes.
typedef enum {
    WAH_GC_PHASE_IDLE = 0,  // Not currently collecting
    WAH_GC_PHASE_MARK,      // Mark phase: tracing live objects from roots
    WAH_GC_PHASE_SWEEP,     // Sweep phase: reclaiming unreachable objects
} wah_gc_phase_t;

// Struct: wah_gc_heap_stats_t
//   GC heap statistics for reporting and debugging purposes.
typedef struct wah_gc_heap_stats_s {
    // Field: object_count
    //   Total number of objects currently allocated on the GC heap.
    uint32_t object_count;
    // Field: allocated_bytes
    //   Total bytes currently allocated on the GC heap for object payloads.
    size_t allocated_bytes;
    // Field: allocation_threshold
    //   The threshold in bytes at which the GC will be triggered.
    size_t allocation_threshold;
    // Field: phase
    //   Current GC phase. Will be WAH_GC_PHASE_IDLE if GC is not currently running.
    wah_gc_phase_t phase;

    // Field: total_collections
    //   Total number of GC collections so far. `WAH_DEBUG` is required.
    uint32_t total_collections;
    // Field: total_allocations
    //   Total number of allocations on the GC heap so far. `WAH_DEBUG` is required.
    uint32_t total_allocations;
    // Field: total_frees
    //   Total number of frees on the GC heap so far. `WAH_DEBUG` is required.
    uint32_t total_frees;
    // Field: total_polls
    //   Total number of GC polls so far. `WAH_DEBUG` is required.
    uint32_t total_polls;
} wah_gc_heap_stats_t;

// Function: wah_gc_start
//   Enables GC on the execution context. Can be called multiple times (idempotent).
//
//   This doesn't have to be called directly since GC will be automatically started when necessary.
//   One exception is to be able to use `wah_gc_alloc_host` before `wah_start` or `wah_call*`.
wah_error_t wah_gc_start(wah_exec_context_t *ctx);

// Function: wah_gc_heap_stats
//   Retrieves GC heap statistics for the execution context.
//
//   - stats [out, borrowed]: Pointer to a `wah_gc_heap_stats_t` struct to be filled.
void wah_gc_heap_stats(const wah_exec_context_t *ctx, wah_gc_heap_stats_t *stats);

// Function: wah_gc_heap_stats_from_host
//   Same as `wah_gc_heap_stats` but can be called from a host function call context.
void wah_gc_heap_stats_from_host(const wah_call_context_t *ctx, wah_gc_heap_stats_t *stats);

// Function: wah_gc_verify_heap
//   Verifies GC heap consistency for debugging. Logs errors via WAH_LOG in debug builds.
//
//   - returns: true if the heap is consistent, false if any inconsistency is detected.
bool wah_gc_verify_heap(const wah_exec_context_t *ctx);

// Function: wah_gc_alloc_host
//   Allocates an opaque host object on the GC heap.
//   This object goes into `wah_value_t::ref` and can be stored in `externref` or `anyref`.
//
//   - size [in]: Size of the host object payload in bytes.
//   - returns: Pointer to the payload of the allocated host object, or NULL on allocation failure.
void *wah_gc_alloc_host(wah_exec_context_t *ctx, size_t size);

// Function: wah_request_interrupt
//   Requests an interrupt from the interpreter. Calling this multiple times does nothing.
//   The interpreter will yield at the next POLL/TICK point. Safe to call from another thread.
void wah_request_interrupt(wah_exec_context_t *ctx);

// Function: wah_request_interrupt_from_host
//   Same as `wah_request_interrupt` but can be called from a host function call context.
void wah_request_interrupt_from_host(wah_call_context_t *ctx);

// Function: wah_is_interrupted
//   Checks if an interrupt has been requested for the execution context.
//   Safe to call from another thread.
bool wah_is_interrupted(const wah_exec_context_t *ctx);

////////////////////////////////////////////////////////////////////////////////

#ifdef __cplusplus
} // extern "C"
#endif

// Macro: WAH_IMPLEMENTATION [user-definable]
//   Define this in exactly one C file that includes this header file.
#ifdef WAH_IMPLEMENTATION

#ifdef __cplusplus
#error "WAH_IMPLEMENTATION is C-only; include wah.h without WAH_IMPLEMENTATION from C++."
#else

#include <string.h> // For memcpy, memset
#include <stdlib.h> // For malloc, free
#include <assert.h> // For assert
#include <stdint.h> // For INT32_MIN, INT32_MAX
#include <math.h> // For floating-point functions
#include <errno.h> // For ETIMEDOUT
#if defined(_MSC_VER)
#include <intrin.h> // For MSVC intrinsics
#pragma warning(disable: 4200) // flexible array member is standard C11
#endif

#ifndef WAH_NO_THREADS
#if defined(_WIN32)
#define WAH_HAS_THREADS 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOUSER
#define NOGDI
#include <windows.h>
#else
#define WAH_HAS_THREADS 1
#include <pthread.h>
#include <time.h>
#endif
#endif

////////////////////////////////////////////////////////////////////////////////
// Support macros //////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Adopted from https://stackoverflow.com/a/48045656
#if defined(_MSC_VER) && !defined(__clang__) && !defined(__cplusplus)
#define WAH_VA_OPT_SUPPORT(_) 0
#elif __cplusplus <= 201703 && defined(__GNUC__) && !defined(__clang__) && !defined(__EDG__)
#define WAH_VA_OPT_SUPPORT(_) 0
#else
#define WAH_VA_ARG1(x,y,...) y
#define WAH_VA_EMPTY(...) WAH_VA_ARG1(__VA_OPT__(,)0,1,)
#define WAH_VA_OPT_SUPPORT WAH_VA_EMPTY
#endif

#if defined(WAH_SANITIZE_UNDEFINED) || WAH_HAS_FEATURE(undefined_behavior_sanitizer)
#define WAH_UBSAN_ACTIVE 1
#else
#define WAH_UBSAN_ACTIVE 0
#endif

#if WAH_HAS_BUILTIN(__builtin_unreachable) || __GNUC__ * 100 + __GNUC_MINOR__ >= 405
#define WAH_UNREACHABLE() __builtin_unreachable()
#else
#define WAH_UNREACHABLE() abort()
#endif

#if WAH_HAS_ATTRIBUTE(always_inline)
#define WAH_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define WAH_ALWAYS_INLINE inline
#endif

#ifdef WAH_X86_64
#define WAH_IF_X86_64(then, ...) then

// Feature detection by compiler version
// Note: These indicate whether the compiler has the intrinsic/inline asm capability,
// not whether the target CPU supports it (that's checked at runtime).
#ifdef __GNUC__
#define WAH_GCC_VER (__GNUC__ * 100 + __GNUC_MINOR__)
#else
#define WAH_GCC_VER 0
#endif
#ifdef __clang__
#define WAH_CLANG_VER (__clang_major__ * 100 + __clang_minor__)
#else
#define WAH_CLANG_VER 0
#endif
#ifdef _MSC_VER
#define WAH_MSVC_VER _MSC_VER
#else
#define WAH_MSVC_VER 0
#endif
#define WAH_CHECK_COMPILER(gcc, clang, msvc) (WAH_GCC_VER >= gcc || WAH_CLANG_VER >= clang || WAH_MSVC_VER >= msvc)
#if WAH_CHECK_COMPILER(403, 206, 1500)
#define WAH_IF_SSSE3(...) __VA_ARGS__
#define WAH_IF_SSE41(...) __VA_ARGS__
#define WAH_IF_SSE42(...) __VA_ARGS__
#else
#define WAH_IF_SSSE3(...)
#define WAH_IF_SSE41(...)
#define WAH_IF_SSE42(...)
#endif
#if WAH_CHECK_COMPILER(404, 209, 1600)
#define WAH_IF_AVX(...) __VA_ARGS__
#else
#define WAH_IF_AVX(...)
#endif
#if WAH_CHECK_COMPILER(409, 303, 1800)
#define WAH_IF_AVX2(...) __VA_ARGS__
#else
#define WAH_IF_AVX2(...)
#endif
#if WAH_CHECK_COMPILER(700, 407, 1932)
#define WAH_IF_AVX512(...) __VA_ARGS__ // Includes AVX-512F/BITALG/BW/DQ/VL
#else
#define WAH_IF_AVX512(...)
#endif

#else
#define WAH_IF_X86_64(then, ...) __VA_ARGS__
#endif

#ifdef WAH_AARCH64
#define WAH_IF_AARCH64(then, ...) then
#else
#define WAH_IF_AARCH64(then, ...) __VA_ARGS__
#endif

#define WAH_TYPE_BOT -99 // Placeholder for bottom type in type checking

////////////////////////////////////////////////////////////////////////////////
// Feature configuration ///////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)
#define WAH_IF_SIMD(...) __VA_ARGS__
#else
#define WAH_IF_SIMD(...)
#endif
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
#define WAH_IF_MEMORY64(...) __VA_ARGS__
#else
#define WAH_IF_MEMORY64(...)
#endif
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
#define WAH_IF_GC(...) __VA_ARGS__
#else
#define WAH_IF_GC(...)
#endif

static inline wah_features_t wah_feature_closure(wah_features_t f) {
    if (f & WAH_FEATURE_RELAXED_SIMD) f |= WAH_FEATURE_SIMD;
    if (f & (WAH_FEATURE_GC | WAH_FEATURE_TYPED_FUNCREF)) f |= WAH_FEATURE_REF_TYPES;
    return f | WAH_FEATURE_MVP;
}

#define WAH_FEATURE_SHIFT_                   0 // Always available
#define WAH_FEATURE_SHIFT_MULTI_VALUE        1
#define WAH_FEATURE_SHIFT_MUTABLE_GLOBALS    2
#define WAH_FEATURE_SHIFT_SIGN_EXT           3
#define WAH_FEATURE_SHIFT_NONTRAPPING_F2I    4
#define WAH_FEATURE_SHIFT_BULK_MEMORY        5
#define WAH_FEATURE_SHIFT_REF_TYPES          6
#define WAH_FEATURE_SHIFT_SIMD               7
#define WAH_FEATURE_SHIFT_TAIL_CALL          8
#define WAH_FEATURE_SHIFT_EXCEPTION          9
#define WAH_FEATURE_SHIFT_GC                 10
#define WAH_FEATURE_SHIFT_TYPED_FUNCREF      11
#define WAH_FEATURE_SHIFT_MEMORY64           12
#define WAH_FEATURE_SHIFT_RELAXED_SIMD       13

static inline wah_error_t wah_require_feature(wah_module_t *module, int8_t shift) {
    if (shift == 0) return WAH_OK;
    wah_features_t bit = UINT64_C(1) << shift;
    module->required_features |= bit;
    return (module->enabled_features & bit) ? WAH_OK : WAH_ERROR_DISABLED_FEATURE;
}

////////////////////////////////////////////////////////////////////////////////
// Opcode listing //////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Opcode-remapping constants for compact lookup table.
#define WAH_FB 0xd7 // 0xFB 0x00..1E -> 0xD7..F5
#define WAH_FC 0xf6 // 0xFC 0x00..11 -> 0xF6..107
#define WAH_FD 0x108 // 0xFD 0x00..113 -> 0x108..21B
#define WAH_FE 0x21c // (sentinel)

typedef enum {
    WAH_OPCLASS_MISSING = 0, // Default value for non-existent opcodes
    WAH_OPCLASS_, // Each opcode is in its own class by default

    // No opcode arguments, validation consists of pops and pushes only
    WAH_OPCLASS__I_I, WAH_OPCLASS__I_L, WAH_OPCLASS__I_F, WAH_OPCLASS__I_D, WAH_OPCLASS__I_V, WAH_OPCLASS__II_I, WAH_OPCLASS__IV_V,
    WAH_OPCLASS__L_I, WAH_OPCLASS__L_L, WAH_OPCLASS__L_F, WAH_OPCLASS__L_D, WAH_OPCLASS__L_V, WAH_OPCLASS__LL_I, WAH_OPCLASS__LL_L, WAH_OPCLASS__LV_V,
    WAH_OPCLASS__F_I, WAH_OPCLASS__F_L, WAH_OPCLASS__F_F, WAH_OPCLASS__F_V, WAH_OPCLASS__FF_I, WAH_OPCLASS__FF_F,
    WAH_OPCLASS__D_I, WAH_OPCLASS__D_L, WAH_OPCLASS__D_D, WAH_OPCLASS__D_V, WAH_OPCLASS__DD_I, WAH_OPCLASS__DD_L, WAH_OPCLASS__DD_D,
    WAH_OPCLASS__V_I, WAH_OPCLASS__V_V, WAH_OPCLASS__VI_V, WAH_OPCLASS__VV_V, WAH_OPCLASS__VVV_V,

    // Opcodes with immediate arguments to be pasted into the preparsed code
    WAH_OPCLASS_B, WAH_OPCLASS_I, WAH_OPCLASS_II, WAH_OPCLASS_M, WAH_OPCLASS_MB,
} wah_opclass_t;

// --- WebAssembly Opcodes ---
#define WAH_OPCODES(X) \
    /* Control Flow Operators */ \
    X(UNREACHABLE,, 0x00,) X(NOP,, 0x01,) X(BLOCK,, 0x02,) X(LOOP,, 0x03,) X(IF,, 0x04,) X(ELSE,, 0x05,) X(END,, 0x0B,) \
    X(BR,, 0x0C,) X(BR_IF,, 0x0D,) X(BR_TABLE,, 0x0E,) X(RETURN,, 0x0F,) X(CALL,I, 0x10,) X(CALL_INDIRECT,II, 0x11,) \
    X(THROW,I, 0x08, EXCEPTION) X(THROW_REF,, 0x0A, EXCEPTION) \
    X(RETURN_CALL,I, 0x12, TAIL_CALL) X(RETURN_CALL_INDIRECT,II, 0x13, TAIL_CALL) \
    X(CALL_REF,I, 0x14, TYPED_FUNCREF) X(RETURN_CALL_REF,I, 0x15, TYPED_FUNCREF) \
    X(TRY_TABLE,, 0x1F, EXCEPTION) \
    \
    /* Parametric Operators */ \
    X(DROP,, 0x1A,) X(SELECT,, 0x1B,) X(SELECT_T,, 0x1C,) \
    \
    /* Variable Access */ \
    X(LOCAL_GET,I, 0x20,) X(LOCAL_SET,I, 0x21,) X(LOCAL_TEE,I, 0x22,) X(GLOBAL_GET,I, 0x23,) X(GLOBAL_SET,I, 0x24,) \
    \
    /* Table Operations */ \
    X(TABLE_GET,I, 0x25, REF_TYPES) X(TABLE_SET,I, 0x26, REF_TYPES) \
    X(TABLE_INIT,II, WAH_FC+0x0C, BULK_MEMORY) X(ELEM_DROP,I, WAH_FC+0x0D, BULK_MEMORY) \
    X(TABLE_COPY,II, WAH_FC+0x0E, BULK_MEMORY) X(TABLE_GROW,I, WAH_FC+0x0F, REF_TYPES) \
    X(TABLE_SIZE,I, WAH_FC+0x10, REF_TYPES) X(TABLE_FILL,I, WAH_FC+0x11, REF_TYPES) \
    \
    /* Memory Operators */ \
    X(I32_LOAD,M, 0x28,) X(I64_LOAD,M, 0x29,) X(F32_LOAD,M, 0x2A,) X(F64_LOAD,M, 0x2B,) \
    X(I32_LOAD8_S,M, 0x2C,) X(I32_LOAD8_U,M, 0x2D,) X(I32_LOAD16_S,M, 0x2E,) X(I32_LOAD16_U,M, 0x2F,) \
    X(I64_LOAD8_S,M, 0x30,) X(I64_LOAD8_U,M, 0x31,) X(I64_LOAD16_S,M, 0x32,) X(I64_LOAD16_U,M, 0x33,) \
    X(I64_LOAD32_S,M, 0x34,) X(I64_LOAD32_U,M, 0x35,) \
    X(I32_STORE,M, 0x36,) X(I64_STORE,M, 0x37,) X(F32_STORE,M, 0x38,) X(F64_STORE,M, 0x39,) \
    X(I32_STORE8,M, 0x3A,) X(I32_STORE16,M, 0x3B,) X(I64_STORE8,M, 0x3C,) X(I64_STORE16,M, 0x3D,) X(I64_STORE32,M, 0x3E,) \
    X(MEMORY_SIZE,I, 0x3F,) X(MEMORY_GROW,I, 0x40,) \
    X(MEMORY_INIT,II, WAH_FC+0x08, BULK_MEMORY) X(DATA_DROP,I, WAH_FC+0x09, BULK_MEMORY) \
    X(MEMORY_COPY,II, WAH_FC+0x0A, BULK_MEMORY) X(MEMORY_FILL,I, WAH_FC+0x0B, BULK_MEMORY) \
    \
    /* Constants */ \
    X(I32_CONST,, 0x41,) X(I64_CONST,, 0x42,) X(F32_CONST,, 0x43,) X(F64_CONST,, 0x44,) \
    \
    /* Comparison Operators */ \
    X(I32_EQZ,_I_I, 0x45,) X(I32_EQ,_II_I, 0x46,) X(I32_NE,_II_I, 0x47,) \
    X(I32_LT_S,_II_I, 0x48,) X(I32_LT_U,_II_I, 0x49,) X(I32_GT_S,_II_I, 0x4A,) X(I32_GT_U,_II_I, 0x4B,) \
    X(I32_LE_S,_II_I, 0x4C,) X(I32_LE_U,_II_I, 0x4D,) X(I32_GE_S,_II_I, 0x4E,) X(I32_GE_U,_II_I, 0x4F,) \
    X(I64_EQZ,_L_I, 0x50,) X(I64_EQ,_LL_I, 0x51,) X(I64_NE,_LL_I, 0x52,) \
    X(I64_LT_S,_LL_I, 0x53,) X(I64_LT_U,_LL_I, 0x54,) X(I64_GT_S,_LL_I, 0x55,) X(I64_GT_U,_LL_I, 0x56,) \
    X(I64_LE_S,_LL_I, 0x57,) X(I64_LE_U,_LL_I, 0x58,) X(I64_GE_S,_LL_I, 0x59,) X(I64_GE_U,_LL_I, 0x5A,) \
    X(F32_EQ,_FF_I, 0x5B,) X(F32_NE,_FF_I, 0x5C,) X(F32_LT,_FF_I, 0x5D,) X(F32_GT,_FF_I, 0x5E,) X(F32_LE,_FF_I, 0x5F,) X(F32_GE,_FF_I, 0x60,) \
    X(F64_EQ,_DD_I, 0x61,) X(F64_NE,_DD_I, 0x62,) X(F64_LT,_DD_I, 0x63,) X(F64_GT,_DD_I, 0x64,) X(F64_LE,_DD_I, 0x65,) X(F64_GE,_DD_I, 0x66,) \
    \
    /* Numeric Operators */ \
    X(I32_CLZ,_I_I, 0x67,) X(I32_CTZ,_I_I, 0x68,) X(I32_POPCNT,_I_I, 0x69,) X(I32_ADD,_II_I, 0x6A,) X(I32_SUB,_II_I, 0x6B,) \
    X(I32_MUL,_II_I, 0x6C,) X(I32_DIV_S,_II_I, 0x6D,) X(I32_DIV_U,_II_I, 0x6E,) X(I32_REM_S,_II_I, 0x6F,) X(I32_REM_U,_II_I, 0x70,) \
    X(I32_AND,_II_I, 0x71,) X(I32_OR,_II_I, 0x72,) X(I32_XOR,_II_I, 0x73,) \
    X(I32_SHL,_II_I, 0x74,) X(I32_SHR_S,_II_I, 0x75,) X(I32_SHR_U,_II_I, 0x76,) X(I32_ROTL,_II_I, 0x77,) X(I32_ROTR,_II_I, 0x78,) \
    X(I64_CLZ,_L_L, 0x79,) X(I64_CTZ,_L_L, 0x7A,) X(I64_POPCNT,_L_L, 0x7B,) X(I64_ADD,_LL_L, 0x7C,) X(I64_SUB,_LL_L, 0x7D,) \
    X(I64_MUL,_LL_L, 0x7E,) X(I64_DIV_S,_LL_L, 0x7F,) X(I64_DIV_U,_LL_L, 0x80,) X(I64_REM_S,_LL_L, 0x81,) X(I64_REM_U,_LL_L, 0x82,) \
    X(I64_AND,_LL_L, 0x83,) X(I64_OR,_LL_L, 0x84,) X(I64_XOR,_LL_L, 0x85,) \
    X(I64_SHL,_LL_L, 0x86,) X(I64_SHR_S,_LL_L, 0x87,) X(I64_SHR_U,_LL_L, 0x88,) X(I64_ROTL,_LL_L, 0x89,) X(I64_ROTR,_LL_L, 0x8A,) \
    X(F32_ABS,_F_F, 0x8B,) X(F32_NEG,_F_F, 0x8C,) X(F32_CEIL,_F_F, 0x8D,) X(F32_FLOOR,_F_F, 0x8E,) X(F32_TRUNC,_F_F, 0x8F,) X(F32_NEAREST,_F_F, 0x90,) \
    X(F32_SQRT,_F_F, 0x91,) X(F32_ADD,_FF_F, 0x92,) X(F32_SUB,_FF_F, 0x93,) X(F32_MUL,_FF_F, 0x94,) X(F32_DIV,_FF_F, 0x95,) \
    X(F32_MIN,_FF_F, 0x96,) X(F32_MAX,_FF_F, 0x97,) X(F32_COPYSIGN,_FF_F, 0x98,) \
    X(F64_ABS,_D_D, 0x99,) X(F64_NEG,_D_D, 0x9A,) X(F64_CEIL,_D_D, 0x9B,) X(F64_FLOOR,_D_D, 0x9C,) X(F64_TRUNC,_D_D, 0x9D,) X(F64_NEAREST,_D_D, 0x9E,) \
    X(F64_SQRT,_D_D, 0x9F,) X(F64_ADD,_DD_D, 0xA0,) X(F64_SUB,_DD_D, 0xA1,) X(F64_MUL,_DD_D, 0xA2,) X(F64_DIV,_DD_D, 0xA3,) \
    X(F64_MIN,_DD_D, 0xA4,) X(F64_MAX,_DD_D, 0xA5,) X(F64_COPYSIGN,_DD_D, 0xA6,) \
    \
    /* Conversion Operators */ \
    X(I32_WRAP_I64,_L_I, 0xA7,) \
    X(I32_TRUNC_F32_S,_F_I, 0xA8,) X(I32_TRUNC_F32_U,_F_I, 0xA9,) X(I32_TRUNC_F64_S,_D_I, 0xAA,) X(I32_TRUNC_F64_U,_D_I, 0xAB,) \
    X(I64_EXTEND_I32_S,_I_L, 0xAC,) X(I64_EXTEND_I32_U,_I_L, 0xAD,) \
    X(I64_TRUNC_F32_S,_F_L, 0xAE,) X(I64_TRUNC_F32_U,_F_L, 0xAF,) X(I64_TRUNC_F64_S,_D_L, 0xB0,) X(I64_TRUNC_F64_U,_D_L, 0xB1,) \
    X(F32_CONVERT_I32_S,_I_F, 0xB2,) X(F32_CONVERT_I32_U,_I_F, 0xB3,) \
    X(F32_CONVERT_I64_S,_L_F, 0xB4,) X(F32_CONVERT_I64_U,_L_F, 0xB5,) X(F32_DEMOTE_F64,, 0xB6,) \
    X(F64_CONVERT_I32_S,_I_D, 0xB7,) X(F64_CONVERT_I32_U,_I_D, 0xB8,) \
    X(F64_CONVERT_I64_S,_L_D, 0xB9,) X(F64_CONVERT_I64_U,_L_D, 0xBA,) X(F64_PROMOTE_F32,, 0xBB,) \
    X(I32_REINTERPRET_F32,_F_I, 0xBC,) X(I64_REINTERPRET_F64,_D_L, 0xBD,) \
    X(F32_REINTERPRET_I32,_I_F, 0xBE,) X(F64_REINTERPRET_I64,_L_D, 0xBF,) \
    X(I32_EXTEND8_S,_I_I, 0xC0, SIGN_EXT) X(I32_EXTEND16_S,_I_I, 0xC1, SIGN_EXT) \
    X(I64_EXTEND8_S,_L_L, 0xC2, SIGN_EXT) X(I64_EXTEND16_S,_L_L, 0xC3, SIGN_EXT) X(I64_EXTEND32_S,_L_L, 0xC4, SIGN_EXT) \
    X(I32_TRUNC_SAT_F32_S,_F_I, WAH_FC+0x00, NONTRAPPING_F2I) X(I32_TRUNC_SAT_F32_U,_F_I, WAH_FC+0x01, NONTRAPPING_F2I) \
    X(I32_TRUNC_SAT_F64_S,_D_I, WAH_FC+0x02, NONTRAPPING_F2I) X(I32_TRUNC_SAT_F64_U,_D_I, WAH_FC+0x03, NONTRAPPING_F2I) \
    X(I64_TRUNC_SAT_F32_S,_F_L, WAH_FC+0x04, NONTRAPPING_F2I) X(I64_TRUNC_SAT_F32_U,_F_L, WAH_FC+0x05, NONTRAPPING_F2I) \
    X(I64_TRUNC_SAT_F64_S,_D_L, WAH_FC+0x06, NONTRAPPING_F2I) X(I64_TRUNC_SAT_F64_U,_D_L, WAH_FC+0x07, NONTRAPPING_F2I) \
    \
    /* Reference Types */ \
    X(REF_NULL,, 0xD0, REF_TYPES) X(REF_IS_NULL,, 0xD1, REF_TYPES) X(REF_FUNC,, 0xD2, REF_TYPES) \
    X(REF_AS_NON_NULL,, 0xD4, TYPED_FUNCREF) X(BR_ON_NULL,, 0xD5, TYPED_FUNCREF) X(BR_ON_NON_NULL,, 0xD6, TYPED_FUNCREF) \
    \
    WAH_GC_OPCODES(X) WAH_SIMD_OPCODES(X)

#define WAH_GC_OPCODES(X) \
    X(REF_EQ,, 0xD3, GC) \
    X(STRUCT_NEW,, WAH_FB+0x00, GC) X(STRUCT_NEW_DEFAULT,, WAH_FB+0x01, GC) \
    X(STRUCT_GET,, WAH_FB+0x02, GC) X(STRUCT_GET_S,, WAH_FB+0x03, GC) X(STRUCT_GET_U,, WAH_FB+0x04, GC) \
    X(STRUCT_SET,, WAH_FB+0x05, GC) \
    X(ARRAY_NEW,, WAH_FB+0x06, GC) X(ARRAY_NEW_DEFAULT,, WAH_FB+0x07, GC) \
    X(ARRAY_NEW_FIXED,, WAH_FB+0x08, GC) \
    X(ARRAY_NEW_DATA,II, WAH_FB+0x09, GC) X(ARRAY_NEW_ELEM,II, WAH_FB+0x0A, GC) \
    X(ARRAY_GET,, WAH_FB+0x0B, GC) X(ARRAY_GET_S,, WAH_FB+0x0C, GC) X(ARRAY_GET_U,, WAH_FB+0x0D, GC) \
    X(ARRAY_SET,, WAH_FB+0x0E, GC) X(ARRAY_LEN,, WAH_FB+0x0F, GC) \
    X(ARRAY_FILL,I, WAH_FB+0x10, GC) X(ARRAY_COPY,, WAH_FB+0x11, GC) \
    X(ARRAY_INIT_DATA,II, WAH_FB+0x12, GC) X(ARRAY_INIT_ELEM,, WAH_FB+0x13, GC) \
    X(REF_TEST,, WAH_FB+0x14, GC) X(REF_TEST_NULL,, WAH_FB+0x15, GC) \
    X(REF_CAST,, WAH_FB+0x16, GC) X(REF_CAST_NULL,, WAH_FB+0x17, GC) \
    X(BR_ON_CAST,, WAH_FB+0x18, GC) X(BR_ON_CAST_FAIL,, WAH_FB+0x19, GC) \
    X(ANY_CONVERT_EXTERN,, WAH_FB+0x1A, GC) X(EXTERN_CONVERT_ANY,, WAH_FB+0x1B, GC) \
    X(REF_I31,, WAH_FB+0x1C, GC) X(I31_GET_S,, WAH_FB+0x1D, GC) X(I31_GET_U,, WAH_FB+0x1E, GC) \

#define WAH_SIMD_OPCODES(X) \
    /* Vector Memory Operators */ \
    X(V128_LOAD,M, WAH_FD+0x00, SIMD) \
    X(V128_LOAD8X8_S,M, WAH_FD+0x01, SIMD) X(V128_LOAD8X8_U,M, WAH_FD+0x02, SIMD) \
    X(V128_LOAD16X4_S,M, WAH_FD+0x03, SIMD) X(V128_LOAD16X4_U,M, WAH_FD+0x04, SIMD) \
    X(V128_LOAD32X2_S,M, WAH_FD+0x05, SIMD) X(V128_LOAD32X2_U,M, WAH_FD+0x06, SIMD) \
    X(V128_LOAD8_SPLAT,M, WAH_FD+0x07, SIMD) X(V128_LOAD16_SPLAT,M, WAH_FD+0x08, SIMD) \
    X(V128_LOAD32_SPLAT,M, WAH_FD+0x09, SIMD) X(V128_LOAD64_SPLAT,M, WAH_FD+0x0A, SIMD) \
    X(V128_LOAD32_ZERO,M, WAH_FD+0x5C, SIMD) X(V128_LOAD64_ZERO,M, WAH_FD+0x5D, SIMD) \
    X(V128_LOAD8_LANE,MB, WAH_FD+0x54, SIMD) X(V128_LOAD16_LANE,MB, WAH_FD+0x55, SIMD) \
    X(V128_LOAD32_LANE,MB, WAH_FD+0x56, SIMD) X(V128_LOAD64_LANE,MB, WAH_FD+0x57, SIMD) \
    X(V128_STORE8_LANE,MB, WAH_FD+0x58, SIMD) X(V128_STORE16_LANE,MB, WAH_FD+0x59, SIMD) \
    X(V128_STORE32_LANE,MB, WAH_FD+0x5A, SIMD) X(V128_STORE64_LANE,MB, WAH_FD+0x5B, SIMD) \
    X(V128_STORE,M, WAH_FD+0x0B, SIMD) \
    \
    /* Vector Lane Operations */ \
    X(I8X16_SHUFFLE,, WAH_FD+0x0D, SIMD) X(I8X16_SWIZZLE,_VV_V, WAH_FD+0x0E, SIMD) \
    X(I8X16_EXTRACT_LANE_S,B, WAH_FD+0x15, SIMD) X(I8X16_EXTRACT_LANE_U,B, WAH_FD+0x16, SIMD) X(I8X16_REPLACE_LANE,B, WAH_FD+0x17, SIMD) \
    X(I16X8_EXTRACT_LANE_S,B, WAH_FD+0x18, SIMD) X(I16X8_EXTRACT_LANE_U,B, WAH_FD+0x19, SIMD) X(I16X8_REPLACE_LANE,B, WAH_FD+0x1A, SIMD) \
    X(I32X4_EXTRACT_LANE,B, WAH_FD+0x1B, SIMD) X(I32X4_REPLACE_LANE,B, WAH_FD+0x1C, SIMD) \
    X(I64X2_EXTRACT_LANE,B, WAH_FD+0x1D, SIMD) X(I64X2_REPLACE_LANE,B, WAH_FD+0x1E, SIMD) \
    X(F32X4_EXTRACT_LANE,B, WAH_FD+0x1F, SIMD) X(F32X4_REPLACE_LANE,B, WAH_FD+0x20, SIMD) \
    X(F64X2_EXTRACT_LANE,B, WAH_FD+0x21, SIMD) X(F64X2_REPLACE_LANE,B, WAH_FD+0x22, SIMD) \
    X(I8X16_SPLAT,_I_V, WAH_FD+0x0F, SIMD) X(I16X8_SPLAT,_I_V, WAH_FD+0x10, SIMD) \
    X(I32X4_SPLAT,_I_V, WAH_FD+0x11, SIMD) X(I64X2_SPLAT,_L_V, WAH_FD+0x12, SIMD) \
    X(F32X4_SPLAT,_F_V, WAH_FD+0x13, SIMD) X(F64X2_SPLAT,_D_V, WAH_FD+0x14, SIMD) \
    \
    /* Constants */ \
    X(V128_CONST,, WAH_FD+0x0C, SIMD) \
    \
    /* Vector Numeric Operators */ \
    X(V128_NOT,_V_V, WAH_FD+0x4D, SIMD) X(V128_AND,_VV_V, WAH_FD+0x4E, SIMD) X(V128_ANDNOT,_VV_V, WAH_FD+0x4F, SIMD) \
    X(V128_OR,_VV_V, WAH_FD+0x50, SIMD) X(V128_XOR,_VV_V, WAH_FD+0x51, SIMD) \
    X(V128_BITSELECT,_VVV_V, WAH_FD+0x52, SIMD) X(V128_ANY_TRUE,_V_I, WAH_FD+0x53, SIMD) \
    \
    X(I8X16_ABS,_V_V, WAH_FD+0x60, SIMD) X(I8X16_NEG,_V_V, WAH_FD+0x61, SIMD) X(I8X16_POPCNT,_V_V, WAH_FD+0x62, SIMD) \
    X(I8X16_ALL_TRUE,_V_I, WAH_FD+0x63, SIMD) X(I8X16_BITMASK,_V_I, WAH_FD+0x64, SIMD) \
    X(I8X16_NARROW_I16X8_S,_VV_V, WAH_FD+0x65, SIMD) X(I8X16_NARROW_I16X8_U,_VV_V, WAH_FD+0x66, SIMD) \
    X(I8X16_SHL,_VI_V, WAH_FD+0x6B, SIMD) X(I8X16_SHR_S,_VI_V, WAH_FD+0x6C, SIMD) X(I8X16_SHR_U,_VI_V, WAH_FD+0x6D, SIMD) \
    X(I8X16_ADD,_VV_V, WAH_FD+0x6E, SIMD) X(I8X16_ADD_SAT_S,_VV_V, WAH_FD+0x6F, SIMD) X(I8X16_ADD_SAT_U,_VV_V, WAH_FD+0x70, SIMD) \
    X(I8X16_SUB,_VV_V, WAH_FD+0x71, SIMD) X(I8X16_SUB_SAT_S,_VV_V, WAH_FD+0x72, SIMD) X(I8X16_SUB_SAT_U,_VV_V, WAH_FD+0x73, SIMD) \
    X(I8X16_MIN_S,_VV_V, WAH_FD+0x76, SIMD) X(I8X16_MIN_U,_VV_V, WAH_FD+0x77, SIMD) \
    X(I8X16_MAX_S,_VV_V, WAH_FD+0x78, SIMD) X(I8X16_MAX_U,_VV_V, WAH_FD+0x79, SIMD) \
    X(I8X16_AVGR_U,_VV_V, WAH_FD+0x7B, SIMD) \
    \
    X(I16X8_EXTADD_PAIRWISE_I8X16_S,_V_V, WAH_FD+0x7C, SIMD) X(I16X8_EXTADD_PAIRWISE_I8X16_U,_V_V, WAH_FD+0x7D, SIMD) \
    X(I16X8_ABS,_V_V, WAH_FD+0x80, SIMD) X(I16X8_NEG,_V_V, WAH_FD+0x81, SIMD) X(I16X8_Q15MULR_SAT_S,_VV_V, WAH_FD+0x82, SIMD) \
    X(I16X8_ALL_TRUE,_V_I, WAH_FD+0x83, SIMD) X(I16X8_BITMASK,_V_I, WAH_FD+0x84, SIMD) \
    X(I16X8_NARROW_I32X4_S,_VV_V, WAH_FD+0x85, SIMD) X(I16X8_NARROW_I32X4_U,_VV_V, WAH_FD+0x86, SIMD) \
    X(I16X8_EXTEND_LOW_I8X16_S,_V_V, WAH_FD+0x87, SIMD) X(I16X8_EXTEND_HIGH_I8X16_S,_V_V, WAH_FD+0x88, SIMD) \
    X(I16X8_EXTEND_LOW_I8X16_U,_V_V, WAH_FD+0x89, SIMD) X(I16X8_EXTEND_HIGH_I8X16_U,_V_V, WAH_FD+0x8A, SIMD) \
    X(I16X8_SHL,_VI_V, WAH_FD+0x8B, SIMD) X(I16X8_SHR_S,_VI_V, WAH_FD+0x8C, SIMD) X(I16X8_SHR_U,_VI_V, WAH_FD+0x8D, SIMD) \
    X(I16X8_ADD,_VV_V, WAH_FD+0x8E, SIMD) X(I16X8_ADD_SAT_S,_VV_V, WAH_FD+0x8F, SIMD) X(I16X8_ADD_SAT_U,_VV_V, WAH_FD+0x90, SIMD) \
    X(I16X8_SUB,_VV_V, WAH_FD+0x91, SIMD) X(I16X8_SUB_SAT_S,_VV_V, WAH_FD+0x92, SIMD) X(I16X8_SUB_SAT_U,_VV_V, WAH_FD+0x93, SIMD) \
    X(I16X8_MUL,_VV_V, WAH_FD+0x95, SIMD) \
    X(I16X8_MIN_S,_VV_V, WAH_FD+0x96, SIMD) X(I16X8_MIN_U,_VV_V, WAH_FD+0x97, SIMD) \
    X(I16X8_MAX_S,_VV_V, WAH_FD+0x98, SIMD) X(I16X8_MAX_U,_VV_V, WAH_FD+0x99, SIMD) \
    X(I16X8_AVGR_U,_VV_V, WAH_FD+0x9B, SIMD) \
    X(I16X8_EXTMUL_LOW_I8X16_S,_VV_V, WAH_FD+0x9C, SIMD) X(I16X8_EXTMUL_HIGH_I8X16_S,_VV_V, WAH_FD+0x9D, SIMD) \
    X(I16X8_EXTMUL_LOW_I8X16_U,_VV_V, WAH_FD+0x9E, SIMD) X(I16X8_EXTMUL_HIGH_I8X16_U,_VV_V, WAH_FD+0x9F, SIMD) \
    \
    X(I32X4_EXTADD_PAIRWISE_I16X8_S,_V_V, WAH_FD+0x7E, SIMD) X(I32X4_EXTADD_PAIRWISE_I16X8_U,_V_V, WAH_FD+0x7F, SIMD) \
    X(I32X4_ABS,_V_V, WAH_FD+0xA0, SIMD) X(I32X4_NEG,_V_V, WAH_FD+0xA1, SIMD) \
    X(I32X4_ALL_TRUE,_V_I, WAH_FD+0xA3, SIMD) X(I32X4_BITMASK,_V_I, WAH_FD+0xA4, SIMD) \
    X(I32X4_EXTEND_LOW_I16X8_S,_V_V, WAH_FD+0xA7, SIMD) X(I32X4_EXTEND_HIGH_I16X8_S,_V_V, WAH_FD+0xA8, SIMD) \
    X(I32X4_EXTEND_LOW_I16X8_U,_V_V, WAH_FD+0xA9, SIMD) X(I32X4_EXTEND_HIGH_I16X8_U,_V_V, WAH_FD+0xAA, SIMD) \
    X(I32X4_SHL,_VI_V, WAH_FD+0xAB, SIMD) X(I32X4_SHR_S,_VI_V, WAH_FD+0xAC, SIMD) X(I32X4_SHR_U,_VI_V, WAH_FD+0xAD, SIMD) \
    X(I32X4_ADD,_VV_V, WAH_FD+0xAE, SIMD) X(I32X4_SUB,_VV_V, WAH_FD+0xB1, SIMD) X(I32X4_MUL,_VV_V, WAH_FD+0xB5, SIMD) \
    X(I32X4_MIN_S,_VV_V, WAH_FD+0xB6, SIMD) X(I32X4_MIN_U,_VV_V, WAH_FD+0xB7, SIMD) \
    X(I32X4_MAX_S,_VV_V, WAH_FD+0xB8, SIMD) X(I32X4_MAX_U,_VV_V, WAH_FD+0xB9, SIMD) \
    X(I32X4_DOT_I16X8_S,_VV_V, WAH_FD+0xBA, SIMD) \
    X(I32X4_EXTMUL_LOW_I16X8_S,_VV_V, WAH_FD+0xBC, SIMD) X(I32X4_EXTMUL_HIGH_I16X8_S,_VV_V, WAH_FD+0xBD, SIMD) \
    X(I32X4_EXTMUL_LOW_I16X8_U,_VV_V, WAH_FD+0xBE, SIMD) X(I32X4_EXTMUL_HIGH_I16X8_U,_VV_V, WAH_FD+0xBF, SIMD) \
    \
    X(I64X2_ABS,_V_V, WAH_FD+0xC0, SIMD) X(I64X2_NEG,_V_V, WAH_FD+0xC1, SIMD) \
    X(I64X2_ALL_TRUE,_V_I, WAH_FD+0xC3, SIMD) X(I64X2_BITMASK,_V_I, WAH_FD+0xC4, SIMD) \
    X(I64X2_EXTEND_LOW_I32X4_S,_V_V, WAH_FD+0xC7, SIMD) X(I64X2_EXTEND_HIGH_I32X4_S,_V_V, WAH_FD+0xC8, SIMD) \
    X(I64X2_EXTEND_LOW_I32X4_U,_V_V, WAH_FD+0xC9, SIMD) X(I64X2_EXTEND_HIGH_I32X4_U,_V_V, WAH_FD+0xCA, SIMD) \
    X(I64X2_SHL,_VI_V, WAH_FD+0xCB, SIMD) X(I64X2_SHR_S,_VI_V, WAH_FD+0xCC, SIMD) X(I64X2_SHR_U,_VI_V, WAH_FD+0xCD, SIMD) \
    X(I64X2_ADD,_VV_V, WAH_FD+0xCE, SIMD) X(I64X2_SUB,_VV_V, WAH_FD+0xD1, SIMD) X(I64X2_MUL,_VV_V, WAH_FD+0xD5, SIMD) \
    X(I64X2_EXTMUL_LOW_I32X4_S,_VV_V, WAH_FD+0xDC, SIMD) X(I64X2_EXTMUL_HIGH_I32X4_S,_VV_V, WAH_FD+0xDD, SIMD) \
    X(I64X2_EXTMUL_LOW_I32X4_U,_VV_V, WAH_FD+0xDE, SIMD) X(I64X2_EXTMUL_HIGH_I32X4_U,_VV_V, WAH_FD+0xDF, SIMD) \
    \
    X(F32X4_CEIL,_V_V, WAH_FD+0x67, SIMD) X(F32X4_FLOOR,_V_V, WAH_FD+0x68, SIMD) \
    X(F32X4_TRUNC,_V_V, WAH_FD+0x69, SIMD) X(F32X4_NEAREST,_V_V, WAH_FD+0x6A, SIMD) \
    X(F32X4_ABS,_V_V, WAH_FD+0xE0, SIMD) X(F32X4_NEG,_V_V, WAH_FD+0xE1, SIMD) X(F32X4_SQRT,_V_V, WAH_FD+0xE3, SIMD) \
    X(F32X4_ADD,_VV_V, WAH_FD+0xE4, SIMD) X(F32X4_SUB,_VV_V, WAH_FD+0xE5, SIMD) \
    X(F32X4_MUL,_VV_V, WAH_FD+0xE6, SIMD) X(F32X4_DIV,_VV_V, WAH_FD+0xE7, SIMD) \
    X(F32X4_MIN,_VV_V, WAH_FD+0xE8, SIMD) X(F32X4_MAX,_VV_V, WAH_FD+0xE9, SIMD) \
    X(F32X4_PMIN,_VV_V, WAH_FD+0xEA, SIMD) X(F32X4_PMAX,_VV_V, WAH_FD+0xEB, SIMD) \
    \
    X(F64X2_CEIL,_V_V, WAH_FD+0x74, SIMD) X(F64X2_FLOOR,_V_V, WAH_FD+0x75, SIMD) \
    X(F64X2_TRUNC,_V_V, WAH_FD+0x7A, SIMD) X(F64X2_NEAREST,_V_V, WAH_FD+0x94, SIMD) \
    X(F64X2_ABS,_V_V, WAH_FD+0xEC, SIMD) X(F64X2_NEG,_V_V, WAH_FD+0xED, SIMD) X(F64X2_SQRT,_V_V, WAH_FD+0xEF, SIMD) \
    X(F64X2_ADD,_VV_V, WAH_FD+0xF0, SIMD) X(F64X2_SUB,_VV_V, WAH_FD+0xF1, SIMD) \
    X(F64X2_MUL,_VV_V, WAH_FD+0xF2, SIMD) X(F64X2_DIV,_VV_V, WAH_FD+0xF3, SIMD) \
    X(F64X2_MIN,_VV_V, WAH_FD+0xF4, SIMD) X(F64X2_MAX,_VV_V, WAH_FD+0xF5, SIMD) \
    X(F64X2_PMIN,_VV_V, WAH_FD+0xF6, SIMD) X(F64X2_PMAX,_VV_V, WAH_FD+0xF7, SIMD) \
    \
    X(I32X4_TRUNC_SAT_F32X4_S,_V_V, WAH_FD+0xF8, SIMD) X(I32X4_TRUNC_SAT_F32X4_U,_V_V, WAH_FD+0xF9, SIMD) \
    X(F32X4_CONVERT_I32X4_S,_V_V, WAH_FD+0xFA, SIMD) X(F32X4_CONVERT_I32X4_U,_V_V, WAH_FD+0xFB, SIMD) \
    X(I32X4_TRUNC_SAT_F64X2_S_ZERO,_V_V, WAH_FD+0xFC, SIMD) X(I32X4_TRUNC_SAT_F64X2_U_ZERO,_V_V, WAH_FD+0xFD, SIMD) \
    X(F64X2_CONVERT_LOW_I32X4_S,_V_V, WAH_FD+0xFE, SIMD) X(F64X2_CONVERT_LOW_I32X4_U,_V_V, WAH_FD+0xFF, SIMD) \
    X(F32X4_DEMOTE_F64X2_ZERO,_V_V, WAH_FD+0x5E, SIMD) X(F64X2_PROMOTE_LOW_F32X4,_V_V, WAH_FD+0x5F, SIMD) \
    \
    /* Relaxed SIMD */ \
    X(I8X16_RELAXED_SWIZZLE,_VV_V, WAH_FD+0x100, RELAXED_SIMD) \
    X(I32X4_RELAXED_TRUNC_F32X4_S,_V_V, WAH_FD+0x101, RELAXED_SIMD) X(I32X4_RELAXED_TRUNC_F32X4_U,_V_V, WAH_FD+0x102, RELAXED_SIMD) \
    X(I32X4_RELAXED_TRUNC_F64X2_S,_V_V, WAH_FD+0x103, RELAXED_SIMD) X(I32X4_RELAXED_TRUNC_F64X2_U,_V_V, WAH_FD+0x104, RELAXED_SIMD) \
    X(F32X4_RELAXED_MADD,_VVV_V, WAH_FD+0x105, RELAXED_SIMD) X(F32X4_RELAXED_NMADD,_VVV_V, WAH_FD+0x106, RELAXED_SIMD) \
    X(F64X2_RELAXED_MADD,_VVV_V, WAH_FD+0x107, RELAXED_SIMD) X(F64X2_RELAXED_NMADD,_VVV_V, WAH_FD+0x108, RELAXED_SIMD) \
    X(I8X16_RELAXED_LANESELECT,_VVV_V, WAH_FD+0x109, RELAXED_SIMD) X(I16X8_RELAXED_LANESELECT,_VVV_V, WAH_FD+0x10A, RELAXED_SIMD) \
    X(I32X4_RELAXED_LANESELECT,_VVV_V, WAH_FD+0x10B, RELAXED_SIMD) X(I64X2_RELAXED_LANESELECT,_VVV_V, WAH_FD+0x10C, RELAXED_SIMD) \
    X(F32X4_RELAXED_MIN,_VV_V, WAH_FD+0x10D, RELAXED_SIMD) X(F32X4_RELAXED_MAX,_VV_V, WAH_FD+0x10E, RELAXED_SIMD) \
    X(F64X2_RELAXED_MIN,_VV_V, WAH_FD+0x10F, RELAXED_SIMD) X(F64X2_RELAXED_MAX,_VV_V, WAH_FD+0x110, RELAXED_SIMD) \
    X(I16X8_RELAXED_Q15MULR_S,_VV_V, WAH_FD+0x111, RELAXED_SIMD) \
    X(I16X8_RELAXED_DOT_I8X16_I7X16_S,_VV_V, WAH_FD+0x112, RELAXED_SIMD) \
    X(I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S,_VVV_V, WAH_FD+0x113, RELAXED_SIMD) \
    \
    /* Vector Comparison Operators */ \
    X(I8X16_EQ,_VV_V, WAH_FD+0x23, SIMD) X(I8X16_NE,_VV_V, WAH_FD+0x24, SIMD) \
    X(I8X16_LT_S,_VV_V, WAH_FD+0x25, SIMD) X(I8X16_LT_U,_VV_V, WAH_FD+0x26, SIMD) \
    X(I8X16_GT_S,_VV_V, WAH_FD+0x27, SIMD) X(I8X16_GT_U,_VV_V, WAH_FD+0x28, SIMD) \
    X(I8X16_LE_S,_VV_V, WAH_FD+0x29, SIMD) X(I8X16_LE_U,_VV_V, WAH_FD+0x2A, SIMD) \
    X(I8X16_GE_S,_VV_V, WAH_FD+0x2B, SIMD) X(I8X16_GE_U,_VV_V, WAH_FD+0x2C, SIMD) \
    X(I16X8_EQ,_VV_V, WAH_FD+0x2D, SIMD) X(I16X8_NE,_VV_V, WAH_FD+0x2E, SIMD) \
    X(I16X8_LT_S,_VV_V, WAH_FD+0x2F, SIMD) X(I16X8_LT_U,_VV_V, WAH_FD+0x30, SIMD) \
    X(I16X8_GT_S,_VV_V, WAH_FD+0x31, SIMD) X(I16X8_GT_U,_VV_V, WAH_FD+0x32, SIMD) \
    X(I16X8_LE_S,_VV_V, WAH_FD+0x33, SIMD) X(I16X8_LE_U,_VV_V, WAH_FD+0x34, SIMD) \
    X(I16X8_GE_S,_VV_V, WAH_FD+0x35, SIMD) X(I16X8_GE_U,_VV_V, WAH_FD+0x36, SIMD) \
    X(I32X4_EQ,_VV_V, WAH_FD+0x37, SIMD) X(I32X4_NE,_VV_V, WAH_FD+0x38, SIMD) \
    X(I32X4_LT_S,_VV_V, WAH_FD+0x39, SIMD) X(I32X4_LT_U,_VV_V, WAH_FD+0x3A, SIMD) \
    X(I32X4_GT_S,_VV_V, WAH_FD+0x3B, SIMD) X(I32X4_GT_U,_VV_V, WAH_FD+0x3C, SIMD) \
    X(I32X4_LE_S,_VV_V, WAH_FD+0x3D, SIMD) X(I32X4_LE_U,_VV_V, WAH_FD+0x3E, SIMD) \
    X(I32X4_GE_S,_VV_V, WAH_FD+0x3F, SIMD) X(I32X4_GE_U,_VV_V, WAH_FD+0x40, SIMD) \
    X(I64X2_EQ,_VV_V, WAH_FD+0xD6, SIMD) X(I64X2_NE,_VV_V, WAH_FD+0xD7, SIMD) \
    X(I64X2_LT_S,_VV_V, WAH_FD+0xD8, SIMD) X(I64X2_GT_S,_VV_V, WAH_FD+0xD9, SIMD) \
    X(I64X2_LE_S,_VV_V, WAH_FD+0xDA, SIMD) X(I64X2_GE_S,_VV_V, WAH_FD+0xDB, SIMD) \
    X(F32X4_EQ,_VV_V, WAH_FD+0x41, SIMD) X(F32X4_NE,_VV_V, WAH_FD+0x42, SIMD) \
    X(F32X4_LT,_VV_V, WAH_FD+0x43, SIMD) X(F32X4_GT,_VV_V, WAH_FD+0x44, SIMD) \
    X(F32X4_LE,_VV_V, WAH_FD+0x45, SIMD) X(F32X4_GE,_VV_V, WAH_FD+0x46, SIMD) \
    X(F64X2_EQ,_VV_V, WAH_FD+0x47, SIMD) X(F64X2_NE,_VV_V, WAH_FD+0x48, SIMD) \
    X(F64X2_LT,_VV_V, WAH_FD+0x49, SIMD) X(F64X2_GT,_VV_V, WAH_FD+0x4A, SIMD) \
    X(F64X2_LE,_VV_V, WAH_FD+0x4B, SIMD) X(F64X2_GE,_VV_V, WAH_FD+0x4C, SIMD)

#define WAH_I32_MEM0_OPCODES_M(X) \
    X(I32_LOAD,i32_mem0) X(I64_LOAD,i32_mem0) X(F32_LOAD,i32_mem0) X(F64_LOAD,i32_mem0) \
    X(I32_LOAD8_S,i32_mem0) X(I32_LOAD8_U,i32_mem0) X(I32_LOAD16_S,i32_mem0) X(I32_LOAD16_U,i32_mem0) \
    X(I64_LOAD8_S,i32_mem0) X(I64_LOAD8_U,i32_mem0) X(I64_LOAD16_S,i32_mem0) X(I64_LOAD16_U,i32_mem0) \
    X(I64_LOAD32_S,i32_mem0) X(I64_LOAD32_U,i32_mem0) \
    X(I32_STORE,i32_mem0) X(I64_STORE,i32_mem0) X(F32_STORE,i32_mem0) X(F64_STORE,i32_mem0) \
    X(I32_STORE8,i32_mem0) X(I32_STORE16,i32_mem0) X(I64_STORE8,i32_mem0) X(I64_STORE16,i32_mem0) X(I64_STORE32,i32_mem0) \
    WAH_IF_SIMD( \
        X(V128_LOAD,i32_mem0) \
        X(V128_LOAD8X8_S,i32_mem0) X(V128_LOAD8X8_U,i32_mem0) \
        X(V128_LOAD16X4_S,i32_mem0) X(V128_LOAD16X4_U,i32_mem0) \
        X(V128_LOAD32X2_S,i32_mem0) X(V128_LOAD32X2_U,i32_mem0) \
        X(V128_LOAD8_SPLAT,i32_mem0) X(V128_LOAD16_SPLAT,i32_mem0) X(V128_LOAD32_SPLAT,i32_mem0) X(V128_LOAD64_SPLAT,i32_mem0) \
        X(V128_LOAD32_ZERO,i32_mem0) X(V128_LOAD64_ZERO,i32_mem0) \
        X(V128_STORE,i32_mem0) \
    )
#define WAH_I32_MEM0_OPCODES_MB(X) \
    WAH_IF_SIMD( \
        X(V128_LOAD8_LANE,i32_mem0) X(V128_LOAD16_LANE,i32_mem0) X(V128_LOAD32_LANE,i32_mem0) X(V128_LOAD64_LANE,i32_mem0) \
        X(V128_STORE8_LANE,i32_mem0) X(V128_STORE16_LANE,i32_mem0) X(V128_STORE32_LANE,i32_mem0) X(V128_STORE64_LANE,i32_mem0) \
    )

// i64-addressed memory opcodes (non-mem0)
#define WAH_I64_MEM_OPCODES_M(X) \
    WAH_IF_MEMORY64( \
        X(I32_LOAD,i64) X(I64_LOAD,i64) X(F32_LOAD,i64) X(F64_LOAD,i64) \
        X(I32_LOAD8_S,i64) X(I32_LOAD8_U,i64) X(I32_LOAD16_S,i64) X(I32_LOAD16_U,i64) \
        X(I64_LOAD8_S,i64) X(I64_LOAD8_U,i64) X(I64_LOAD16_S,i64) X(I64_LOAD16_U,i64) \
        X(I64_LOAD32_S,i64) X(I64_LOAD32_U,i64) \
        X(I32_STORE,i64) X(I64_STORE,i64) X(F32_STORE,i64) X(F64_STORE,i64) \
        X(I32_STORE8,i64) X(I32_STORE16,i64) X(I64_STORE8,i64) X(I64_STORE16,i64) X(I64_STORE32,i64) \
        WAH_IF_SIMD( \
            X(V128_LOAD,i64) \
            X(V128_LOAD8X8_S,i64) X(V128_LOAD8X8_U,i64) \
            X(V128_LOAD16X4_S,i64) X(V128_LOAD16X4_U,i64) \
            X(V128_LOAD32X2_S,i64) X(V128_LOAD32X2_U,i64) \
            X(V128_LOAD8_SPLAT,i64) X(V128_LOAD16_SPLAT,i64) X(V128_LOAD32_SPLAT,i64) X(V128_LOAD64_SPLAT,i64) \
            X(V128_LOAD32_ZERO,i64) X(V128_LOAD64_ZERO,i64) \
            X(V128_STORE,i64) \
        ) \
    )
#define WAH_I64_MEM_OPCODES_MB(X) \
    WAH_IF_MEMORY64(WAH_IF_SIMD( \
        X(V128_LOAD8_LANE,i64) X(V128_LOAD16_LANE,i64) X(V128_LOAD32_LANE,i64) X(V128_LOAD64_LANE,i64) \
        X(V128_STORE8_LANE,i64) X(V128_STORE16_LANE,i64) X(V128_STORE32_LANE,i64) X(V128_STORE64_LANE,i64) \
    ))

// i64-addressed memory 0 fast-path opcodes
#define WAH_I64_MEM0_OPCODES_M(X) \
    WAH_IF_MEMORY64( \
        X(I32_LOAD,i64_mem0) X(I64_LOAD,i64_mem0) X(F32_LOAD,i64_mem0) X(F64_LOAD,i64_mem0) \
        X(I32_LOAD8_S,i64_mem0) X(I32_LOAD8_U,i64_mem0) X(I32_LOAD16_S,i64_mem0) X(I32_LOAD16_U,i64_mem0) \
        X(I64_LOAD8_S,i64_mem0) X(I64_LOAD8_U,i64_mem0) X(I64_LOAD16_S,i64_mem0) X(I64_LOAD16_U,i64_mem0) \
        X(I64_LOAD32_S,i64_mem0) X(I64_LOAD32_U,i64_mem0) \
        X(I32_STORE,i64_mem0) X(I64_STORE,i64_mem0) X(F32_STORE,i64_mem0) X(F64_STORE,i64_mem0) \
        X(I32_STORE8,i64_mem0) X(I32_STORE16,i64_mem0) X(I64_STORE8,i64_mem0) X(I64_STORE16,i64_mem0) X(I64_STORE32,i64_mem0) \
        WAH_IF_SIMD( \
            X(V128_LOAD,i64_mem0) \
            X(V128_LOAD8X8_S,i64_mem0) X(V128_LOAD8X8_U,i64_mem0) \
            X(V128_LOAD16X4_S,i64_mem0) X(V128_LOAD16X4_U,i64_mem0) \
            X(V128_LOAD32X2_S,i64_mem0) X(V128_LOAD32X2_U,i64_mem0) \
            X(V128_LOAD8_SPLAT,i64_mem0) X(V128_LOAD16_SPLAT,i64_mem0) X(V128_LOAD32_SPLAT,i64_mem0) X(V128_LOAD64_SPLAT,i64_mem0) \
            X(V128_LOAD32_ZERO,i64_mem0) X(V128_LOAD64_ZERO,i64_mem0) \
            X(V128_STORE,i64_mem0) \
        ) \
    )
#define WAH_I64_MEM0_OPCODES_MB(X) \
    WAH_IF_MEMORY64(WAH_IF_SIMD( \
        X(V128_LOAD8_LANE,i64_mem0) X(V128_LOAD16_LANE,i64_mem0) X(V128_LOAD32_LANE,i64_mem0) X(V128_LOAD64_LANE,i64_mem0) \
        X(V128_STORE8_LANE,i64_mem0) X(V128_STORE16_LANE,i64_mem0) X(V128_STORE32_LANE,i64_mem0) X(V128_STORE64_LANE,i64_mem0) \
    ))

// i64-addressed memory.size/grow/fill/init/copy opcodes (non-mem0 and mem0)
#define WAH_I64_MEM_SIZE_OPCODES(X) \
    WAH_IF_MEMORY64( \
        X(MEMORY_SIZE,i64) X(MEMORY_GROW,i64) \
        X(MEMORY_FILL,i64) X(MEMORY_INIT,i64) X(MEMORY_COPY,i32_to_i64) X(MEMORY_COPY,i64_to_i32) X(MEMORY_COPY,i64_to_i64) \
    )

// i64-indexed table opcodes
#define WAH_I64_TABLE_OPCODES(X) \
    WAH_IF_MEMORY64( \
        X(TABLE_GET,i64) X(TABLE_SET,i64) X(TABLE_SIZE,i64) \
        X(TABLE_GROW,i64) X(TABLE_FILL,i64) X(TABLE_COPY,i32_to_i64) X(TABLE_COPY,i64_to_i32) X(TABLE_COPY,i64_to_i64) X(TABLE_INIT,i64) \
        X(CALL_INDIRECT,i64) X(RETURN_CALL_INDIRECT,i64) \
    )

#ifdef WAH_X86_64

// Single-version opcodes (only one optimized implementation)
#define WAH_X86_64_EXTRA_OPCODES_SINGLE(X) \
    X(I8X16_ABS,ssse3) X(I16X8_ABS,ssse3) X(I32X4_ABS,ssse3) \
    X(I16X8_Q15MULR_SAT_S,ssse3) X(I16X8_RELAXED_Q15MULR_S,ssse3) X(I8X16_RELAXED_SWIZZLE,ssse3) \
    X(I8X16_MIN_S,sse41) X(I8X16_MAX_S,sse41) X(I16X8_MIN_U,sse41) X(I16X8_MAX_U,sse41) \
    X(I32X4_MIN_S,sse41) X(I32X4_MIN_U,sse41) X(I32X4_MAX_S,sse41) X(I32X4_MAX_U,sse41) \
    X(I32X4_MUL,sse41) \
    X(I16X8_EXTEND_LOW_I8X16_S,sse41) X(I16X8_EXTEND_HIGH_I8X16_S,sse41) \
    X(I16X8_EXTEND_LOW_I8X16_U,sse41) X(I16X8_EXTEND_HIGH_I8X16_U,sse41) \
    X(I32X4_EXTEND_LOW_I16X8_S,sse41) X(I32X4_EXTEND_HIGH_I16X8_S,sse41) \
    X(I32X4_EXTEND_LOW_I16X8_U,sse41) X(I32X4_EXTEND_HIGH_I16X8_U,sse41) \
    X(I64X2_EXTEND_LOW_I32X4_S,sse41) X(I64X2_EXTEND_HIGH_I32X4_S,sse41) \
    X(I64X2_EXTEND_LOW_I32X4_U,sse41) X(I64X2_EXTEND_HIGH_I32X4_U,sse41) \
    X(I16X8_EXTMUL_LOW_I8X16_S,sse41) X(I16X8_EXTMUL_HIGH_I8X16_S,sse41) \
    X(I16X8_EXTMUL_LOW_I8X16_U,sse41) X(I16X8_EXTMUL_HIGH_I8X16_U,sse41) \
    X(I32X4_EXTMUL_LOW_I16X8_S,sse41) X(I32X4_EXTMUL_HIGH_I16X8_S,sse41) \
    X(I32X4_EXTMUL_LOW_I16X8_U,sse41) X(I32X4_EXTMUL_HIGH_I16X8_U,sse41) \
    X(I64X2_EXTMUL_LOW_I32X4_S,sse41) X(I64X2_EXTMUL_HIGH_I32X4_S,sse41) \
    X(I64X2_EXTMUL_LOW_I32X4_U,sse41) X(I64X2_EXTMUL_HIGH_I32X4_U,sse41) \
    X(I64X2_EQ,sse41) X(I64X2_NE,sse41) \
    X(I32X4_TRUNC_SAT_F32X4_S,sse41) \
    X(F32X4_CEIL,sse41) X(F32X4_FLOOR,sse41) X(F32X4_TRUNC,sse41) X(F32X4_NEAREST,sse41) \
    X(F64X2_CEIL,sse41) X(F64X2_FLOOR,sse41) X(F64X2_TRUNC,sse41) X(F64X2_NEAREST,sse41) \
    X(I64X2_GT_S,sse42) X(I64X2_LT_S,sse42) \
    X(I64X2_LE_S,sse42) X(I64X2_GE_S,sse42) \
    X(I8X16_LT_U,avx512bw_vl) X(I64X2_MUL,avx512dq_vl) \
    X(F32X4_CONVERT_I32X4_U,avx512f_vl) X(F64X2_CONVERT_LOW_I32X4_U,avx512f_vl) \
    X(V128_BITSELECT,avx512f_vl)

// Multi-version opcodes (multiple implementations with priority ordering)
// These are handled manually in wah_x86_64_opcode function
#define WAH_X86_64_EXTRA_OPCODES_MULTI(X) \
    X(I8X16_POPCNT,avx2) X(I8X16_POPCNT,avx512bitalg_vl) \
    X(I32X4_TRUNC_SAT_F32X4_U,sse41) X(I32X4_TRUNC_SAT_F32X4_U,avx512f_vl)

#endif

#define WAH_EXTRA_OPCODES(X) \
    WAH_I32_MEM0_OPCODES_M(X) WAH_I32_MEM0_OPCODES_MB(X) \
    WAH_I64_MEM_OPCODES_M(X) WAH_I64_MEM_OPCODES_MB(X) \
    WAH_I64_MEM0_OPCODES_M(X) WAH_I64_MEM0_OPCODES_MB(X) \
    WAH_I64_MEM_SIZE_OPCODES(X) \
    WAH_I64_TABLE_OPCODES(X) \
    WAH_IF_SIMD(WAH_IF_X86_64(WAH_X86_64_EXTRA_OPCODES_SINGLE(X) WAH_X86_64_EXTRA_OPCODES_MULTI(X)))

#define WAH_GC_INTERNAL_OPCODES(X) \
    X(STRUCT_GET_S8) X(STRUCT_GET_S16) X(STRUCT_GET_U8) X(STRUCT_GET_U16) \
    X(STRUCT_GET_32) X(STRUCT_GET_64) X(STRUCT_GET_128) \
    X(STRUCT_SET_8) X(STRUCT_SET_16) X(STRUCT_SET_32) X(STRUCT_SET_64) X(STRUCT_SET_128) \
    X(ARRAY_GET_S8) X(ARRAY_GET_S16) X(ARRAY_GET_U8) X(ARRAY_GET_U16) \
    X(ARRAY_GET_32) X(ARRAY_GET_64) X(ARRAY_GET_128) \
    X(ARRAY_SET_8) X(ARRAY_SET_16) X(ARRAY_SET_32) X(ARRAY_SET_64) X(ARRAY_SET_128)

#define WAH_INTERNAL_OPCODES(X) \
    X(POLL) X(METER) X(TICK) \
    X(END_TRY_TABLE) \
    X(REF_FUNC_CONST) \
    X(GLOBAL_GET_INDIRECT) \
    X(GLOBAL_SET_INDIRECT) \
    WAH_IF_GC(WAH_GC_INTERNAL_OPCODES(X))

typedef enum {
#define WAH_OPCODE_INIT(name, cls, val, feat) WAH_OP_##name = val,
#define WAH_EXTRA_OPCODE_INIT(name, suffix) WAH_OP_##name##_##suffix,
#define WAH_INTERNAL_OPCODE_INIT(name) WAH_OP_##name,
    WAH_OPCODES(WAH_OPCODE_INIT)
    WAH_LAST_OPCODE = WAH_FE - 1, // To make the first extra opcode have value WAH_FE
    WAH_INTERNAL_OPCODES(WAH_INTERNAL_OPCODE_INIT)
    WAH_EXTRA_OPCODES(WAH_EXTRA_OPCODE_INIT)
    WAH_NUM_OPCODES,
#undef WAH_OPCODE_INIT
#undef WAH_EXTRA_OPCODE_INIT
#undef WAH_INTERNAL_OPCODE_INIT
} wah_opcode_t;

static const uint8_t wah_opclasses[WAH_FE] = {
#define WAH_OPCLASS_INIT(name, cls, val, feat) [WAH_OP_##name] = WAH_OPCLASS_##cls,
    WAH_OPCODES(WAH_OPCLASS_INIT)
#undef WAH_OPCLASS_INIT
};

static const uint8_t wah_opcode_features[WAH_FE] = {
#define WAH_OPCODE_FEATURE_INIT(name, cls, val, feat) [WAH_OP_##name] = WAH_FEATURE_SHIFT_##feat,
    WAH_OPCODES(WAH_OPCODE_FEATURE_INIT)
#undef WAH_OPCODE_FEATURE_INIT
};

////////////////////////////////////////////////////////////////////////////////
// Architecture-specific wrappers //////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#ifdef WAH_X86_64

#ifdef __GNUC__
#include <cpuid.h>
#else
#include <intrin.h>
#endif

// SSE2/SSSE3/SSE4.1 inline assembly wrappers for GCC/Clang (must be declared before use)
#ifdef __GNUC__
#define WAH_ASM_UNARY_M128I(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a) { __m128i res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#define WAH_ASM_UNARY_M128(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128 a) { __m128 res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#define WAH_ASM_UNARY_M128D(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128d a) { __m128d res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#define WAH_ASM_BINARY_M128I(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a, __m128i b) \
        { __m128i res; __asm__(insn " %2, %0" : "=x"(res) : "0"(a), "x"(b)); return res; }
#define WAH_ASM_BINARY_M128I_3OP(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a, __m128i b) \
        { __m128i res; __asm__(insn " %2, %1, %0" : "=x"(res) : "x"(a), "x"(b)); return res; }
#define WAH_ASM_BINARY_M128(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128 a, __m128 b) \
        { __m128 res; __asm__(insn " %2, %0" : "=x"(res) : "0"(a), "x"(b)); return res; }
#define WAH_ASM_BINARY_M128_IMM(intrin, insn, imm) \
    static WAH_ALWAYS_INLINE __m128 wah##intrin##imm(__m128 a, __m128 b) \
        { __m128 res; __asm__(insn " $" #imm ", %2, %1, %0" : "=x"(res) : "x"(a), "x"(b)); return res; }
#define WAH_ASM_BINARY_M128D(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128d a, __m128d b) \
        { __m128d res; __asm__(insn " %2, %0" : "=x"(res) : "0"(a), "x"(b)); return res; }
#define WAH_ASM_BINARY_M128D_IMM(intrin, insn, imm) \
    static WAH_ALWAYS_INLINE __m128d wah##intrin##imm(__m128d a, __m128d b) \
        { __m128d res; __asm__(insn " $" #imm ", %2, %1, %0" : "=x"(res) : "x"(a), "x"(b)); return res; }
#define WAH_ASM_CONV_M128I_TO_M128(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128i a) { __m128 res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#define WAH_ASM_CONV_M128I_TO_M128D(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128i a) { __m128d res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#define WAH_ASM_CONV_M128_TO_M128I(intrin, insn) \
    static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128 a) { __m128i res; __asm__(insn " %1, %0" : "=x"(res) : "x"(a)); return res; }
#else
#define WAH_ASM_UNARY_M128I(intrin, insn) static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a) { return intrin(a); }
#define WAH_ASM_UNARY_M128(intrin, insn) static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128 a) { return intrin(a); }
#define WAH_ASM_UNARY_M128D(intrin, insn) static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128d a) { return intrin(a); }
#define WAH_ASM_BINARY_M128I(intrin, insn) static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a, __m128i b) { return intrin(a, b); }
#define WAH_ASM_BINARY_M128I_3OP(intrin, insn) static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128i a, __m128i b) { return intrin(a, b); }
#define WAH_ASM_BINARY_M128(intrin, insn) static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128 a, __m128 b) { return intrin(a, b); }
#define WAH_ASM_BINARY_M128_IMM(intrin, insn, imm) static WAH_ALWAYS_INLINE __m128 wah##intrin##imm(__m128 a, __m128 b) { return intrin(a, b, imm); }
#define WAH_ASM_BINARY_M128D(intrin, insn) static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128d a, __m128d b) { return intrin(a, b); }
#define WAH_ASM_BINARY_M128D_IMM(intrin, insn, imm) static WAH_ALWAYS_INLINE __m128d wah##intrin##imm(__m128d a, __m128d b) { return intrin(a, b, imm); }
#define WAH_ASM_CONV_M128I_TO_M128(intrin, insn) static WAH_ALWAYS_INLINE __m128 wah##intrin(__m128i a) { return intrin(a); }
#define WAH_ASM_CONV_M128I_TO_M128D(intrin, insn) static WAH_ALWAYS_INLINE __m128d wah##intrin(__m128i a) { return intrin(a); }
#define WAH_ASM_CONV_M128_TO_M128I(intrin, insn) static WAH_ALWAYS_INLINE __m128i wah##intrin(__m128 a) { return intrin(a); }
#endif

// SSE2 wrappers (cmpord used for NaN detection immune to compiler optimizations)
WAH_ASM_BINARY_M128(_mm_cmpord_ps, "cmpordps")
WAH_ASM_BINARY_M128D(_mm_cmpord_pd, "cmpordpd")

// SSSE3 wrappers
WAH_IF_SSSE3(
    WAH_ASM_UNARY_M128I(_mm_abs_epi8, "pabsb")
    WAH_ASM_UNARY_M128I(_mm_abs_epi16, "pabsw")
    WAH_ASM_UNARY_M128I(_mm_abs_epi32, "pabsd")
    WAH_ASM_BINARY_M128I(_mm_mulhrs_epi16, "pmulhrsw")
    WAH_ASM_BINARY_M128I(_mm_shuffle_epi8, "pshufb")
    WAH_ASM_BINARY_M128I(_mm_maddubs_epi16, "pmaddubsw")
)

#define _mm_round_ps_trunc(a) _mm_round_ps((a), _MM_FROUND_TO_ZERO)
#define _mm_round_ps_nearest(a) _mm_round_ps((a), _MM_FROUND_TO_NEAREST_INT)
#define _mm_round_pd_trunc(a) _mm_round_pd((a), _MM_FROUND_TO_ZERO)
#define _mm_round_pd_nearest(a) _mm_round_pd((a), _MM_FROUND_TO_NEAREST_INT)

WAH_IF_SSE41(
    // SSE4.1 wrappers - min/max
    WAH_ASM_BINARY_M128I(_mm_min_epi8, "pminsb")
    WAH_ASM_BINARY_M128I(_mm_max_epi8, "pmaxsb")
    WAH_ASM_BINARY_M128I(_mm_min_epu16, "pminuw")
    WAH_ASM_BINARY_M128I(_mm_max_epu16, "pmaxuw")
    WAH_ASM_BINARY_M128I(_mm_min_epi32, "pminsd")
    WAH_ASM_BINARY_M128I(_mm_max_epi32, "pmaxsd")
    WAH_ASM_BINARY_M128I(_mm_min_epu32, "pminud")
    WAH_ASM_BINARY_M128I(_mm_max_epu32, "pmaxud")

    // SSE4.1 wrappers - extend
    WAH_ASM_UNARY_M128I(_mm_cvtepi8_epi16, "pmovsxbw")
    WAH_ASM_UNARY_M128I(_mm_cvtepu8_epi16, "pmovzxbw")
    WAH_ASM_UNARY_M128I(_mm_cvtepi16_epi32, "pmovsxwd")
    WAH_ASM_UNARY_M128I(_mm_cvtepu16_epi32, "pmovzxwd")
    WAH_ASM_UNARY_M128I(_mm_cvtepi32_epi64, "pmovsxdq")
    WAH_ASM_UNARY_M128I(_mm_cvtepu32_epi64, "pmovzxdq")

    // SSE4.1 wrappers - compare/pack/mul
    WAH_ASM_BINARY_M128I(_mm_cmpeq_epi64, "pcmpeqq")
    WAH_ASM_BINARY_M128I(_mm_cmpgt_epi64, "pcmpgtq")
    WAH_ASM_BINARY_M128I(_mm_packus_epi32, "packusdw")
    WAH_ASM_BINARY_M128I(_mm_mullo_epi32, "pmulld")
    WAH_ASM_BINARY_M128I(_mm_mul_epi32, "pmuldq")

    // SSE4.1 wrappers - float round with immediate operand
    WAH_ASM_UNARY_M128(_mm_ceil_ps, "roundps $0x2,")
    WAH_ASM_UNARY_M128(_mm_floor_ps, "roundps $0x1,")
    WAH_ASM_UNARY_M128(_mm_round_ps_trunc, "roundps $0x3,")
    WAH_ASM_UNARY_M128(_mm_round_ps_nearest, "roundps $0x0,")

    WAH_ASM_UNARY_M128D(_mm_ceil_pd, "roundpd $0x2,")
    WAH_ASM_UNARY_M128D(_mm_floor_pd, "roundpd $0x1,")
    WAH_ASM_UNARY_M128D(_mm_round_pd_trunc, "roundpd $0x3,")
    WAH_ASM_UNARY_M128D(_mm_round_pd_nearest, "roundpd $0x0,")
)

// SSE4.1 wrapper with implicit xmm0 operand (pblendvb)
#ifdef __GNUC__
WAH_IF_SSE41(
    static WAH_ALWAYS_INLINE __m128i wah_mm_blendv_epi8(__m128i a, __m128i b, __m128i mask) {
        __m128i res = a;
        __asm__ __volatile__("movdqa %2, %%xmm0; pblendvb %1, %0"
            : "+x"(res)
            : "x"(b), "x"(mask)
            : "xmm0");
        return res;
    }
)
#else
WAH_IF_SSE41(
    static WAH_ALWAYS_INLINE __m128i wah_mm_blendv_epi8(__m128i a, __m128i b, __m128i mask) {
        return _mm_blendv_epi8(a, b, mask);
    }
)
#endif

WAH_IF_AVX512(
    // AVX-512 popcount wrappers
    WAH_ASM_UNARY_M128I(_mm_popcnt_epi8, "vpopcntb")

    // AVX-512 unsigned conversion wrappers (i32x4 -> f32x4/f64x2)
    WAH_ASM_CONV_M128I_TO_M128(_mm_cvtepu32_ps, "vcvtudq2ps")
    WAH_ASM_CONV_M128I_TO_M128D(_mm_cvtepu32_pd, "vcvtudq2pd")

    // AVX-512VL truncate-to-unsigned wrappers (f32x4 -> i32x4, with saturation)
    WAH_ASM_CONV_M128_TO_M128I(_mm_cvttps_epu32, "vcvttps2udq")

    // AVX-512DQ+VL 64x64->64 multiply
    WAH_ASM_BINARY_M128I_3OP(_mm_mullo_epi64, "vpmullq")
)

// AVX-512F+VL ternary logic wrapper (vpternlogd)
// Specialized for V128_BITSELECT: (a & c) | (b & ~c), imm8 = 0xE4
#ifdef __GNUC__
WAH_IF_AVX512(
    static WAH_ALWAYS_INLINE __m128i wah_mm_ternarylogic_epi32_bitselect(__m128i a, __m128i b, __m128i c) {
        __m128i res = a;
        __asm__("vpternlogd $0xE4, %2, %1, %0" : "+x"(res) : "x"(b), "x"(c));
        return res;
    }
)
#else
WAH_IF_AVX512(
    static WAH_ALWAYS_INLINE __m128i wah_mm_ternarylogic_epi32_bitselect(__m128i a, __m128i b, __m128i c) {
        return _mm_ternarylogic_epi32(a, b, c, 0xE4);
    }
)
#endif

#undef WAH_ASM_UNARY_M128I
#undef WAH_ASM_BINARY_M128I
#undef WAH_ASM_BINARY_M128I_3OP
#undef WAH_ASM_UNARY_M128
#undef WAH_ASM_BINARY_M128
#undef WAH_ASM_BINARY_M128_IMM
#undef WAH_ASM_UNARY_M128D
#undef WAH_ASM_BINARY_M128D
#undef WAH_ASM_BINARY_M128D_IMM
#undef WAH_ASM_CONV_M128I_TO_M128
#undef WAH_ASM_CONV_M128I_TO_M128D
#undef WAH_ASM_CONV_M128_TO_M128I

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)

typedef struct {
    uint32_t ssse3 : 1, sse41 : 1, sse42 : 1, avx2 : 1;
    uint32_t avx512f_vl : 1, avx512dq_vl : 1, avx512bw_vl : 1, avx512bitalg_vl : 1;
} wah_x86_64_features_t;

static WAH_ALWAYS_INLINE uint64_t wah_x86_64_xgetbv(uint32_t xcr) {
#if defined(_MSC_VER)
    return _xgetbv(xcr);
#else
    uint32_t eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(xcr));
    return ((uint64_t)edx << 32) | eax;
#endif
}

static wah_x86_64_features_t wah_x86_64_features(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_basic_leaf;
#if defined(_MSC_VER)
    int cpu_info[4];
    __cpuid(cpu_info, 0);
    max_basic_leaf = (uint32_t)cpu_info[0];
    if (max_basic_leaf < 1) return (wah_x86_64_features_t){0};
    __cpuid(cpu_info, 1);
    eax = (uint32_t)cpu_info[0];
    ebx = (uint32_t)cpu_info[1];
    ecx = (uint32_t)cpu_info[2];
    edx = (uint32_t)cpu_info[3];
#else
    __cpuid(0, max_basic_leaf, ebx, ecx, edx);
    if (max_basic_leaf < 1) return (wah_x86_64_features_t){0};
    __cpuid(1, eax, ebx, ecx, edx);
#endif

    uint32_t has_osxsave = (ecx >> 27) & 1;
    uint32_t has_avx = (ecx >> 28) & 1;
    uint32_t avx_usable = 0, avx512_usable = 0;
    if (has_osxsave && has_avx) {
        uint64_t xcr0 = wah_x86_64_xgetbv(0);
        avx_usable = (xcr0 & 0x6) == 0x6;
        avx512_usable = avx_usable && (xcr0 & 0xE0) == 0xE0;
    }

    uint32_t leaf7_ebx = 0, leaf7_ecx = 0;
    if (max_basic_leaf >= 7) {
#if defined(_MSC_VER)
        __cpuidex(cpu_info, 7, 0);
        leaf7_ebx = (uint32_t)cpu_info[1];
        leaf7_ecx = (uint32_t)cpu_info[2];
#else
        __cpuid_count(7, 0, eax, leaf7_ebx, leaf7_ecx, edx);
#endif
    }

    uint32_t avx512vl = avx512_usable && ((leaf7_ebx >> 31) & 1);
    return (wah_x86_64_features_t){
        .ssse3 = (ecx >> 9) & 1,
        .sse41 = (ecx >> 19) & 1,
        .sse42 = (ecx >> 20) & 1,
        .avx2 = avx_usable && ((leaf7_ebx >> 5) & 1),
        .avx512f_vl = avx512vl && ((leaf7_ebx >> 16) & 1),
        .avx512dq_vl = avx512vl && ((leaf7_ebx >> 17) & 1),
        .avx512bw_vl = avx512vl && ((leaf7_ebx >> 30) & 1),
        .avx512bitalg_vl = avx512vl && ((leaf7_ecx >> 12) & 1),
    };
}

static wah_opcode_t wah_x86_64_opcode(wah_opcode_t opcode, wah_x86_64_features_t features) {
    switch (opcode) {
        // Single-version opcodes
        #define WAH_X86_64_OPCODE_CASE(name, suffix) case WAH_OP_##name: if (features.suffix) return WAH_OP_##name##_##suffix; break;
        WAH_X86_64_EXTRA_OPCODES_SINGLE(WAH_X86_64_OPCODE_CASE)
        #undef WAH_X86_64_OPCODE_CASE

        // Multi-version opcodes with priority ordering
        case WAH_OP_I8X16_POPCNT:
            if (features.avx512bitalg_vl) return WAH_OP_I8X16_POPCNT_avx512bitalg_vl;
            if (features.avx2) return WAH_OP_I8X16_POPCNT_avx2;
            break;
        case WAH_OP_I32X4_TRUNC_SAT_F32X4_U:
            if (features.avx512f_vl) return WAH_OP_I32X4_TRUNC_SAT_F32X4_U_avx512f_vl;
            if (features.sse41) return WAH_OP_I32X4_TRUNC_SAT_F32X4_U_sse41;
            break;
        default: ; // pass through
    }
    return opcode;
}

#endif // WAH_FEATURE_SIMD

#endif

////////////////////////////////////////////////////////////////////////////////
// Internal API ////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#define WAH_TYIDX(t)               ((uint32_t)(t) >> 1)
#define WAH_TYPE_FROM_IDX(i, null) ((wah_type_t)((i) * 2 + !!(null)))
#define WAH_MAX_TYPE_INDEX         ((uint32_t)INT32_MAX >> 1)

struct wah_call_context_s {
    struct wah_exec_context_s *exec;

    size_t nparams, nresults;
    const wah_value_t *params;
    wah_value_t *results;
    const wah_type_t *param_types, *result_types;

    wah_error_t trap_reason;
};

// --- Repr Metadata ---
typedef int32_t wah_repr_t;

typedef enum {
    WAH_REPR_STRUCT = 1,
    WAH_REPR_ARRAY,
} wah_repr_type_t;

typedef struct {
    uint32_t offset;
    wah_repr_t repr_id;
} wah_repr_field_t;

typedef struct wah_repr_info_s {
    wah_repr_type_t type;
    uint32_t typeidx;
    uint32_t size;
    uint32_t count;
    wah_repr_field_t fields[];
} wah_repr_info_t;

#define WAH_REPR_NONE   ((wah_repr_t)-0x40)
#define WAH_REPR_REF    ((wah_repr_t)-0x41)
#define WAH_REPR_HOST   ((wah_repr_t)-0x42)

typedef struct {
    uint32_t length;
    uint32_t reserved; // To make the following data aligned for multiples of 8 bytes
} wah_gc_array_body_t;

static inline bool wah_repr_field_is_ref(const wah_repr_field_t *f) {
    return f->repr_id == WAH_REPR_REF || f->repr_id >= 0;
}

typedef struct wah_repr_set_s {
    uint32_t word_count;
    uint64_t *bits;
    bool accepts_i31;
} wah_repr_set_t;

#define WAH_NO_SUPERTYPE UINT32_MAX

typedef struct wah_type_def_s {
    wah_comp_type_kind_t kind;
    bool is_final;
    uint32_t supertype;
    uint32_t field_count;
    wah_type_t *field_types;
    bool *field_mutables;
    uint32_t rec_group_start;
    uint32_t rec_group_size;
} wah_type_def_t;

#define WAH_TYPE_CHECK_CACHE_SIZE 64

// --- GC State ---
#define WAH_GC_TAG_MARK ((uintptr_t)0x1)
#define WAH_GC_TAG_AUX  ((uintptr_t)0x2)
#define WAH_GC_TAG_MASK ((uintptr_t)0x3)

#define WAH_I31_TAG     ((uintptr_t)0x1)
static inline bool wah_ref_is_i31(const void *ref) {
    return ((uintptr_t)ref & WAH_I31_TAG) != 0;
}
static inline void *wah_ref_make_i31(int32_t value) {
    return (void *)(((uintptr_t)(uint32_t)(value & 0x7FFFFFFF) << 1) | WAH_I31_TAG);
}
static inline int32_t wah_ref_i31_get_u(const void *ref) {
    return (int32_t)((uintptr_t)ref >> 1) & 0x7FFFFFFF;
}
static inline int32_t wah_ref_i31_get_s(const void *ref) {
    int32_t val = wah_ref_i31_get_u(ref);
    return (val & 0x40000000) ? (int32_t)(val | 0x80000000u) : val;
}

typedef struct wah_gc_object_s {
    struct wah_gc_object_s *next_tagged;
    wah_repr_t repr_id;
    uint32_t size_bytes;
    const struct wah_module_s *module;
} wah_gc_object_t;

static inline wah_gc_object_t *wah_gc_next(const wah_gc_object_t *obj) {
    return (wah_gc_object_t *)((uintptr_t)obj->next_tagged & ~WAH_GC_TAG_MASK);
}

static inline void wah_gc_set_next(wah_gc_object_t *obj, wah_gc_object_t *next) {
    obj->next_tagged = (struct wah_gc_object_s *)((uintptr_t)next | ((uintptr_t)obj->next_tagged & WAH_GC_TAG_MASK));
}

static inline bool wah_gc_marked(const wah_gc_object_t *obj) {
    return ((uintptr_t)obj->next_tagged & WAH_GC_TAG_MARK) != 0;
}

static inline void wah_gc_set_mark(wah_gc_object_t *obj, bool marked) {
    if (marked)
        obj->next_tagged = (struct wah_gc_object_s *)((uintptr_t)obj->next_tagged | WAH_GC_TAG_MARK);
    else
        obj->next_tagged = (struct wah_gc_object_s *)((uintptr_t)obj->next_tagged & ~WAH_GC_TAG_MARK);
}

typedef struct wah_gc_state_s {
    wah_gc_object_t *all_objects;
    wah_gc_object_t *sweep_cursor;
    wah_gc_phase_t phase;
    uint32_t object_count;
    size_t allocated_bytes;
    size_t allocation_threshold;
    bool gc_pending;
#ifdef WAH_DEBUG
    uint32_t total_collections;
    uint32_t total_allocations;
    uint32_t total_frees;
    uint32_t total_polls;
#endif
} wah_gc_state_t;

typedef struct wah_memory_inst_s {
    uint8_t *data;
    uint64_t size;
    uint64_t max_pages;
    bool is_imported;
    struct wah_exec_context_s *import_ctx;
    uint32_t import_idx;
} wah_memory_inst_t;

typedef struct wah_table_inst_s {
    wah_value_t *entries;
    uint64_t size;
    uint64_t max_size;
    bool is_imported;
    struct wah_exec_context_s *import_ctx;
    uint32_t import_idx;
} wah_table_inst_t;

typedef struct wah_linked_module_s {
    const char *name;
    const wah_module_t *module;
    struct wah_exec_context_s *ctx;
    bool owns_ctx;
} wah_linked_module_t;

// Returns the object header from a payload pointer.
static inline wah_gc_object_t *wah_gc_header(void *payload) {
    return (wah_gc_object_t *)((uint8_t *)payload - sizeof(wah_gc_object_t));
}
// Returns the payload pointer from an object header.
static inline void *wah_gc_payload(wah_gc_object_t *obj) {
    return (uint8_t *)obj + sizeof(wah_gc_object_t);
}
static inline uint32_t wah_gc_struct_alloc_size(const wah_repr_info_t *info) {
    return (uint32_t)(sizeof(wah_gc_object_t) + info->size);
}
static inline size_t wah_gc_array_alloc_size(const wah_repr_info_t *info, uint32_t length) {
    return sizeof(wah_gc_object_t) + sizeof(wah_gc_array_body_t) + (size_t)info->size * length;
}
// Visitor callback for root enumeration. Called once per live reference slot.
// slot points to the wah_value_t containing the reference; type is its declared type.
typedef void (*wah_gc_ref_visitor_t)(wah_value_t *slot, void *userdata);

// --- Repr Lookup ---
static inline wah_repr_t wah_module_typeidx_to_repr(const wah_module_t *module, uint32_t typeidx) {
    if (!module->typeidx_to_repr || typeidx >= module->type_count) return WAH_REPR_NONE;
    return module->typeidx_to_repr[typeidx];
}

static inline const wah_repr_info_t *wah_repr_info_get(const wah_module_t *module, wah_repr_t repr_id) {
    if (repr_id < 0 || (uint32_t)repr_id >= module->repr_count) return NULL;
    return module->repr_infos[repr_id];
}

static inline uint32_t wah_repr_info_typeidx(const wah_module_t *module, wah_repr_t repr_id) {
    const wah_repr_info_t *info = wah_repr_info_get(module, repr_id);
    return info ? info->typeidx : (uint32_t)-1;
}

// --- Memory Structure ---
#define WAH_WASM_PAGE_SIZE 65536 // 64 KB

typedef struct wah_memory_type_s {
    wah_type_t addr_type;     // WAH_TYPE_I32 or WAH_TYPE_I64
    uint64_t min_pages, max_pages; // max_pages: UINT64_MAX if no maximum
} wah_memory_type_t;

typedef struct {
    uint8_t *bytecode;
    uint32_t bytecode_size;
    uint8_t *operand_ref_map;
    uint32_t operand_ref_map_size;
} wah_parsed_code_t;

// --- WebAssembly Table Structures ---
typedef struct wah_table_type_s {
    wah_type_t elem_type;
    wah_type_t addr_type;     // WAH_TYPE_I32 or WAH_TYPE_I64
    uint64_t min_elements;
    uint64_t max_elements; // UINT64_MAX if no maximum
    wah_parsed_code_t init_expr;
} wah_table_type_t;

typedef struct wah_data_segment_s {
    uint32_t flags;
    uint32_t memory_idx; // Only for active segments (flags & 0x02)
    wah_parsed_code_t offset_expr; // Preparsed offset expression for active segments
    uint32_t data_len;
    const uint8_t *data; // Pointer to the raw data bytes within the WASM binary
} wah_data_segment_t;

// Unified function entry used in wah_module_t::functions[].
// For WASM functions only kind is meaningful; the rest is zero.
// For host functions all fields are valid.
typedef struct wah_function_s {
    wah_gc_object_t fake_header;
    bool is_host;

    // Global function index (for reference types)
    uint32_t global_idx;  // Index in the global function index space [imports, local, host]

    // Host function fields (only valid when is_host == true)
    char *name;           // owned
    wah_func_t func;
    void *userdata;
    wah_finalize_t finalize;
    size_t nparams, nresults;
    wah_type_t *param_types, *result_types; // owned

    // WASM function fields (only valid when is_host == false)
    uint32_t local_idx;                    // local index within fn_module->code_bodies[]
    const struct wah_module_s *fn_module;  // owning module (NULL means ctx->module)
    struct wah_exec_context_s *fn_ctx;     // owning exec context (NULL means current ctx)
} wah_function_t;

typedef struct wah_export_s {
    const char *name;
    size_t name_len;
    uint8_t kind; // WASM export kind (0=func, 1=table, 2=mem, 3=global)
    uint32_t index; // Index into the respective module array (functions, tables, etc.)
} wah_export_t;

// --- Operand Stack ---
typedef struct {
    wah_value_t *data; // Dynamically allocated based on function requirements
    uint32_t sp; // Stack pointer
    uint32_t capacity; // Allocated capacity
} wah_stack_t;

// --- Type Stack for Validation ---
#define WAH_MAX_TYPE_STACK_SIZE 1024 // Maximum size of the type stack for validation
typedef struct {
    wah_type_t data[WAH_MAX_TYPE_STACK_SIZE];
    uint32_t sp; // Stack pointer
} wah_type_stack_t;

// --- Execution Context ---

// Represents a single function call's state on the call stack.
typedef struct wah_call_frame_s {
    const uint8_t *bytecode_ip;  // Instruction pointer into the parsed bytecode
    const struct wah_code_body_s *code; // The function body being executed
    wah_value_t *locals;         // Pointer to this frame's locals in the value stack
    uint32_t func_idx;           // Local index of the function being executed (debug)
    uint32_t result_count;       // Number of return values (used by RETURN/END)
    const struct wah_module_s *module; // The module this function belongs to (for cross-module calls)
    wah_value_t *frame_globals;  // Pointer to the globals array for this frame's module
    uint32_t ref_map_offset;     // Byte offset into parsed_code.operand_ref_map for current POLL point
    struct wah_function_s *frame_function_table; // Function table for this frame
    uint32_t frame_function_table_count; // Number of entries in frame_function_table
    struct wah_exec_context_s *frame_ctx; // Exec context for this frame
} wah_call_frame_t;

// The main context for the entire WebAssembly interpretation.
#define WAH_DEFAULT_MAX_CALL_DEPTH 1024
#define WAH_DEFAULT_VALUE_STACK_SIZE (64 * 1024)
#define WAH_MAX_LOCAL_COUNT WAH_DEFAULT_VALUE_STACK_SIZE
// Default unified stack: space for WAH_DEFAULT_VALUE_STACK_SIZE values + WAH_DEFAULT_MAX_CALL_DEPTH frames
#define WAH_DEFAULT_STACK_BYTES \
    ((uint64_t)WAH_DEFAULT_VALUE_STACK_SIZE * sizeof(wah_value_t) + \
     (uint64_t)WAH_DEFAULT_MAX_CALL_DEPTH * sizeof(wah_call_frame_t))

// Frame indexing: frame at depth d in a downward-growing call stack.
// frame_ptr points to the topmost (most recently pushed) frame.
// frame_ptr[0] = newest frame, frame_ptr[call_depth-1] = oldest frame.
#define WAH_FRAME(ctx, d) ((ctx)->frame_ptr[(ctx)->call_depth - 1 - (d)])

// --- Function Type ---
typedef struct wah_func_type_s {
    uint32_t param_count;
    uint32_t result_count;
    wah_type_t *param_types;
    wah_type_t *result_types;
} wah_func_type_t;

// --- Imports ---
typedef struct wah_import_name_s {
    char *module;
    size_t module_len;
    char *field;
    size_t field_len;
} wah_import_name_t;

typedef struct wah_func_import_s {
    wah_import_name_t name;
    uint32_t type_index;
} wah_func_import_t;

typedef struct wah_table_import_s {
    wah_import_name_t name;
    wah_table_type_t type;
} wah_table_import_t;

typedef struct wah_memory_import_s {
    wah_import_name_t name;
    wah_memory_type_t type;
} wah_memory_import_t;

typedef struct wah_global_import_s {
    wah_import_name_t name;
    wah_type_t type;
    bool is_mutable;
} wah_global_import_t;

typedef struct wah_tag_import_s {
    wah_import_name_t name;
    uint32_t type_index;
} wah_tag_import_t;

typedef struct wah_tag_s {
    uint32_t type_index;
} wah_tag_t;

typedef struct wah_tag_instance_s {
    uint32_t type_index;
    const struct wah_tag_instance_s *identity;
} wah_tag_instance_t;

typedef struct wah_exception_s {
    wah_gc_object_t header;
    const wah_tag_instance_t *tag_identity;
    uint32_t tag_index;
    uint32_t value_count;
} wah_exception_t;

// GC payloads are not expected to be aligned to multiples of 16 bytes, so memcpy is required.
static inline void wah_exception_get_value(wah_exception_t *exc, uint32_t i, wah_value_t *out) {
    memcpy(out, (uint8_t *)exc + sizeof(wah_exception_t) + i * sizeof(wah_value_t), sizeof(wah_value_t));
}
static inline void wah_exception_set_value(wah_exception_t *exc, uint32_t i, const wah_value_t *val) {
    memcpy((uint8_t *)exc + sizeof(wah_exception_t) + i * sizeof(wah_value_t), val, sizeof(wah_value_t));
}
static inline wah_type_t *wah_exception_value_types(wah_exception_t *exc) {
    return (wah_type_t *)((uint8_t *)exc + sizeof(wah_exception_t) + exc->value_count * sizeof(wah_value_t));
}
static inline wah_exception_t *wah_exception_from_ref(void *ref) {
    return (wah_exception_t *)wah_gc_header(ref);
}
static inline void *wah_exception_to_ref(wah_exception_t *exc) {
    return wah_gc_payload(&exc->header);
}

typedef struct wah_exception_handler_s {
    uint32_t call_depth;
    wah_value_t *sp_base;
    const uint8_t *catch_table;
    uint32_t catch_count;
    const uint8_t *bytecode_base;
    struct wah_tag_instance_s *handler_tag_instances;
    uint32_t handler_tag_instance_count;
} wah_exception_handler_t;

#define WAH_CATCH_KIND_CATCH     0
#define WAH_CATCH_KIND_CATCH_REF 1
#define WAH_CATCH_KIND_CATCH_ALL 2
#define WAH_CATCH_KIND_CATCH_ALL_REF 3

// --- Code Body Structure ---
typedef struct wah_code_body_s {
    uint32_t local_count;
    wah_type_t *local_types; // Array of types for local variables
    uint32_t code_size;
    const uint8_t *code; // Pointer to the raw instruction bytes within the WASM binary
    uint32_t max_stack_depth; // Maximum operand stack depth required
    uint32_t max_frame_slots; // local_count + max_stack_depth (preflight budget)
    wah_parsed_code_t parsed_code; // Pre-parsed opcodes and arguments for optimized execution
} wah_code_body_t;

// --- WebAssembly Element Segment Structure ---
typedef struct wah_element_segment_s {
    bool is_active;           // true for active (flags&3 == 0 or 2), false for passive
    bool is_declarative;      // true for declarative segments (dropped at parse time, no elem data stored)
    uint32_t table_idx;       // For active modes
    uint32_t num_elems;
    bool is_expr_elem;        // true if elem* are expressions, false if func indices
    wah_type_t elem_type;     // declared element reference type (funcref, externref, or concrete type)

    // offset expression (for active modes) - preparsed for evaluation
    wah_parsed_code_t offset_expr;

    // elem* data
    union {
        uint32_t *func_indices;              // For is_expr_elem == false
        struct {
            uint8_t **bytecodes;                // Array of pointers to each expression
            uint32_t *bytecode_sizes;           // Array of sizes for each expression
        } expr;
    } u;
} wah_element_segment_t;

// --- Global Variable Structure ---
typedef struct wah_global_s {
    wah_type_t type;
    bool is_mutable;
    wah_parsed_code_t init_expr; // Preparsed const expression (evaluated at instantiation)
} wah_global_t;

// --- Analyzer Mode ---
typedef enum {
    WAH_ANALYZE_FUNC_BODY,
    WAH_ANALYZE_CONST_EXPR,
} wah_analyze_mode_t;

// --- Validation Context ---
#define WAH_MAX_CONTROL_DEPTH 256
typedef struct {
    wah_opcode_t opcode;
    uint32_t type_stack_sp; // Type stack pointer at the start of the block
    wah_func_type_t block_type; // For if/block/loop
    bool else_found; // For if blocks
    bool is_unreachable; // True if this control frame is currently unreachable
    uint32_t stack_height; // Stack height at the beginning of the block
    uint32_t local_init_save_offset; // Offset into local_init_stack for saved state
} wah_validation_control_frame_t;

typedef struct {
    wah_analyze_mode_t mode;
    wah_type_stack_t type_stack;
    const wah_func_type_t *func_type; // Type of the function being validated
    wah_module_t *module; // Reference to the module for global/function lookups
    uint32_t total_locals; // Total number of locals (params + declared locals)
    uint32_t current_stack_depth; // Current stack depth during validation
    uint32_t max_stack_depth; // Maximum stack depth seen during validation
    bool is_unreachable; // True if the current code path is unreachable
    uint32_t max_global_idx; // For const_expr mode: globals before this index only

    // Control flow validation stack
    wah_validation_control_frame_t control_stack[WAH_MAX_CONTROL_DEPTH];
    uint32_t control_sp;

    // Local initialization tracking for non-defaultable locals
    uint8_t *local_inits; // Per-local init state: 1=initialized, 0=not
    uint8_t *local_init_stack; // Saved states for control frames
    uint32_t local_init_stack_used; // Current usage in local_init_stack
    uint32_t num_non_defaultable; // Number of non-defaultable locals
} wah_validation_context_t;

// --- Analyzed-Code IR (structured output of raw-Wasm decoding + validation) ---

typedef enum {
    WAH_INSTR_FLAG_NONE          = 0,
    WAH_INSTR_FLAG_POLL          = 1 << 0,
    WAH_INSTR_FLAG_UNREACHABLE   = 1 << 1,
    WAH_INSTR_FLAG_LOWERING_HINT = 1 << 2,
} wah_instr_flags_t;

typedef struct {
    uint32_t align;
    uint32_t memidx;
    uint64_t offset;
} wah_memarg_t;

typedef struct {
    uint16_t opcode;
    uint16_t flags;
    union {
        uint32_t u32;
        int32_t i32;
        int64_t i64;
        uint32_t f32_bits;
        uint64_t f64_bits;
        uint8_t v128[16];
        struct { uint32_t a, b; } u32_u32;
        wah_memarg_t memarg;
        struct { wah_memarg_t memarg; uint8_t lane; } memarg_lane;
        struct { uint32_t label_idx; uint32_t target_symbol; uint32_t keep; uint32_t drop; } branch;
        struct {
            uint32_t target_count;
            uint32_t *target_symbols;
            uint32_t default_symbol;
            uint32_t keep;
            uint32_t *drops;
        } br_table;
        struct { int32_t heap_type; } ref_cast;
        struct {
            uint8_t cast_flags;
            uint32_t target_symbol;
            wah_type_t src_type, dst_type;
            int32_t dst_heap_type;
            uint32_t keep;
            uint32_t drop;
        } br_on_cast;
        struct { uint32_t type_idx; uint32_t field_idx; } type_field;
        struct { uint32_t type_idx; uint32_t length; } type_length;
        struct {
            uint32_t catch_count;
            struct { uint8_t kind; uint32_t tag_idx; uint32_t target_symbol; } *catches;
        } try_table;
    } imm;
} wah_decoded_instr_t;

typedef struct {
    wah_analyze_mode_t mode;

    wah_decoded_instr_t *instrs;
    uint32_t instr_count;
    uint32_t instrs_cap;

    uint8_t *operand_ref_map;
    uint32_t operand_ref_map_size;

    uint32_t max_stack_depth;
} wah_analyzed_code_t;

typedef struct wah_type_check_cache_entry_s {
    const struct wah_module_s *sub_module;
    const struct wah_module_s *sup_module;
    wah_type_t sub_type;
    wah_type_t sup_type;
    bool is_subtype;
    bool valid;
} wah_type_check_cache_entry_t;

// -- Exceptions --
#define WAH_MAX_EXCEPTION_HANDLER_DEPTH 64

// -- Function References --
#define WAH_FUNCREF_FAKE_HEADER { .next_tagged = NULL, .repr_id = WAH_TYPE_BOT, .size_bytes = 0 }

// Sentinel used by wah_value_t._prefuncref to distinguish ref.func 0 from ref.null.
// A prefuncref with .sentinel == wah_funcref_sentinel is a valid function reference;
// .ref == NULL means null. This sentinel is never executed.
static wah_function_t wah_funcref_sentinel[1] = {{ .fake_header = WAH_FUNCREF_FAKE_HEADER }};

static inline void *wah_func_to_ref(wah_function_t *fn) {
    return wah_gc_payload(&fn->fake_header);
}
static inline wah_function_t *wah_ref_to_func(void *ref) {
    return (wah_function_t *)wah_gc_header(ref);
}

// -- Forward declarations --

// Const expression functions
static wah_error_t wah_eval_const_expr(wah_exec_context_t *ctx, const uint8_t *bytecode, uint32_t bytecode_size, wah_value_t *result);

// Helpers to look up types across import+local index spaces
static inline uint32_t wah_func_index_limit(const wah_module_t *m) { return m->import_function_count + m->wasm_function_count; }
static inline uint32_t wah_table_index_limit(const wah_module_t *m) { return m->import_table_count + m->table_count; }
static inline uint32_t wah_memory_index_limit(const wah_module_t *m) { return m->import_memory_count + m->memory_count; }
static inline uint32_t wah_global_index_limit(const wah_module_t *m) { return m->import_global_count + m->global_count; }

static inline const wah_table_type_t *wah_table_type(const wah_module_t *m, uint32_t idx) {
    if (idx < m->import_table_count) return &m->table_imports[idx].type;
    return &m->tables[idx - m->import_table_count];
}
static inline const wah_memory_type_t *wah_memory_type(const wah_module_t *m, uint32_t idx) {
    if (idx < m->import_memory_count) return &m->memory_imports[idx].type;
    return &m->memories[idx - m->import_memory_count];
}
static inline wah_type_t wah_global_type(const wah_module_t *m, uint32_t idx) {
    if (idx < m->import_global_count) return m->global_imports[idx].type;
    return m->globals[idx - m->import_global_count].type;
}
static inline bool wah_global_is_mutable(const wah_module_t *m, uint32_t idx) {
    if (idx < m->import_global_count) return m->global_imports[idx].is_mutable;
    return m->globals[idx - m->import_global_count].is_mutable;
}
static inline uint32_t wah_tag_type_index(const wah_module_t *m, uint32_t idx) {
    if (idx < m->import_tag_count) return m->tag_imports[idx].type_index;
    return m->tags[idx - m->import_tag_count].type_index;
}

// Per-instance segment-dropped helpers. The bitsets live on the exec context
// (sized to module->element_segment_count / module->data_segment_count) so the
// module itself stays read-only after parse and can be reused across instances.
static inline bool wah_elem_seg_is_dropped(const wah_exec_context_t *ctx, uint32_t idx) {
    return ctx->dropped_elem_segments != NULL &&
           (ctx->dropped_elem_segments[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}
static inline void wah_elem_seg_mark_dropped(wah_exec_context_t *ctx, uint32_t idx) {
    if (ctx->dropped_elem_segments) {
        ctx->dropped_elem_segments[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    }
}
static inline bool wah_data_seg_is_dropped(const wah_exec_context_t *ctx, uint32_t idx) {
    return ctx->dropped_data_segments != NULL &&
           (ctx->dropped_data_segments[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}
static inline void wah_data_seg_mark_dropped(wah_exec_context_t *ctx, uint32_t idx) {
    if (ctx->dropped_data_segments) {
        ctx->dropped_data_segments[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    }
}
// Effective length helpers (return 0 if dropped, else the segment's full length).
static inline uint32_t wah_elem_seg_num_elems(const wah_exec_context_t *ctx, uint32_t idx) {
    if (wah_elem_seg_is_dropped(ctx, idx)) return 0;
    return ctx->module->element_segments[idx].num_elems;
}
static inline uint32_t wah_data_seg_data_len(const wah_exec_context_t *ctx, uint32_t idx) {
    if (wah_data_seg_is_dropped(ctx, idx)) return 0;
    return ctx->module->data_segments[idx].data_len;
}

// Helper function to duplicate a string
static inline char* wah_strdup(const char* s, const wah_alloc_t *alloc) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)alloc->malloc(len + 1, alloc->userdata);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

////////////////////////////////////////////////////////////////////////////////
// Error checking and memory allocation ////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#ifdef WAH_DEBUG
#include <stdio.h>
#if WAH_VA_OPT_SUPPORT(?)
#define WAH_LOG(fmt, ...) printf("(%d) " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__)
#else
#define WAH_LOG(fmt, ...) printf("(%d) " fmt "\n", __LINE__, ##__VA_ARGS__) // common extension
#endif
#else
#define WAH_LOG(fmt, ...) (void)(0)
#endif

// --- Helper Macros ---
#define WAH_CHECK(expr) do { \
    wah_error_t _err = (expr); \
    if (_err != WAH_OK) { WAH_LOG("WAH_CHECK(%s) failed due to: %s", #expr, wah_strerror(_err)); return _err; } \
} while(0)

#define WAH_CHECK_GOTO(expr, label) do { \
    err = (expr); \
    if (err != WAH_OK) { WAH_LOG("WAH_CHECK_GOTO(%s, %s) failed due to: %s", #expr, #label, wah_strerror(err)); goto label; } \
} while(0)

#define WAH_ENSURE(cond, error) do { \
    if (!(cond)) { WAH_LOG("WAH_ENSURE(%s, %s) failed", #cond, #error); return (error); } \
} while(0)

#define WAH_ENSURE_GOTO(cond, error, label) do { \
    if (!(cond)) { err = (error); WAH_LOG("WAH_ENSURE_GOTO(%s, %s, %s) failed", #cond, #error, #label); goto label; } \
} while(0)

static void *wah_stdc_malloc(size_t size, void *userdata) { (void)userdata; return malloc(size); }
static void *wah_stdc_realloc(void *ptr, size_t size, void *userdata) { (void)userdata; return realloc(ptr, size); }
static void wah_stdc_free(void *ptr, void *userdata) { (void)userdata; free(ptr); }

static inline bool wah_alloc_valid(const wah_alloc_t *a) {
    return !a || (a->malloc && a->realloc && a->free);
}

static inline wah_alloc_t wah_resolve_alloc(const wah_alloc_t *a) {
    if (a && a->malloc && a->realloc && a->free) return *a;
    return (wah_alloc_t){ wah_stdc_malloc, wah_stdc_realloc, wah_stdc_free, NULL };
}

static inline void wah_free(const wah_alloc_t *a, void *ptr) {
    if (!ptr) return;
    a->free(ptr, a->userdata);
}

static inline wah_error_t wah_malloc(const wah_alloc_t *a, size_t count, size_t elemsize, void** out_ptr) {
    *out_ptr = NULL;
    if (count == 0) return WAH_OK;
    if (elemsize != 0 && count > SIZE_MAX / elemsize) return WAH_ERROR_OUT_OF_MEMORY;
    *out_ptr = a->malloc(count * elemsize, a->userdata);
    return *out_ptr ? WAH_OK : WAH_ERROR_OUT_OF_MEMORY;
}

static inline wah_error_t wah_realloc(const wah_alloc_t *a, size_t count, size_t elemsize, void** p_ptr) {
    if (count == 0) {
        wah_free(a, *p_ptr);
        *p_ptr = NULL;
        return WAH_OK;
    }
    if (elemsize != 0 && count > SIZE_MAX / elemsize) return WAH_ERROR_OUT_OF_MEMORY;
    size_t size = count * elemsize;
    void* new_ptr = *p_ptr ? a->realloc(*p_ptr, size, a->userdata) : a->malloc(size, a->userdata);
    if (!new_ptr) return WAH_ERROR_OUT_OF_MEMORY;
    *p_ptr = new_ptr;
    return WAH_OK;
}

#define WAH_MALLOC(ptr) WAH_MALLOC_ARRAY(ptr, 1)
#define WAH_MALLOC_GOTO(ptr, label) WAH_MALLOC_ARRAY_GOTO(ptr, 1, label)

#define WAH_MALLOC_ARRAY(ptr, count) do { \
        void *_alloc_ptr; \
        wah_error_t _alloc_err = wah_malloc(alloc, (count), sizeof(*(ptr)), &_alloc_ptr); \
        if (_alloc_err != WAH_OK) { \
            WAH_LOG("WAH_MALLOC_ARRAY(%s, %s) failed due to OOM", #ptr, #count); \
            return _alloc_err; \
        } \
        (ptr) = _alloc_ptr; \
    } while (0)

#define WAH_REALLOC_ARRAY(ptr, count) do { \
        void *_alloc_ptr = (ptr); \
        wah_error_t _alloc_err = wah_realloc(alloc, (count), sizeof(*(ptr)), &_alloc_ptr); \
        if (_alloc_err != WAH_OK) { \
            WAH_LOG("WAH_REALLOC_ARRAY(%s, %s) failed due to OOM", #ptr, #count); \
            return _alloc_err; \
        } \
        (ptr) = _alloc_ptr; \
    } while (0)

#define WAH_MALLOC_ARRAY_GOTO(ptr, count, label) do { \
        void* _alloc_ptr; \
        err = wah_malloc(alloc, (count), sizeof(*(ptr)), &_alloc_ptr); \
        if (err != WAH_OK) { \
            WAH_LOG("WAH_MALLOC_ARRAY_GOTO(%s, %s, %s) failed due to OOM", #ptr, #count, #label); \
            goto label; \
        } \
        (ptr) = _alloc_ptr; \
    } while (0)

#define WAH_REALLOC_ARRAY_GOTO(ptr, count, label) do { \
        void* _alloc_ptr = ptr; \
        err = wah_realloc(alloc, (count), sizeof(*(ptr)), &_alloc_ptr); \
        if (err != WAH_OK) { \
            WAH_LOG("WAH_REALLOC_ARRAY_GOTO(%s, %s, %s) failed due to OOM", #ptr, #count, #label); \
            goto label; \
        } \
        (ptr) = _alloc_ptr; \
    } while (0)

#define WAH_ENSURE_CAP(arr, needed) do { \
        size_t _cap_needed_size = (needed); \
        WAH_ENSURE(_cap_needed_size <= UINT32_MAX, WAH_ERROR_TOO_LARGE); \
        uint32_t _cap_needed = (uint32_t)_cap_needed_size; \
        if (_cap_needed > arr##_cap) { \
            uint32_t _nc = arr##_cap == 0 ? 8 : arr##_cap; \
            while (_nc < _cap_needed) { \
                WAH_ENSURE(_nc <= UINT32_MAX / 2, WAH_ERROR_TOO_LARGE); \
                _nc *= 2; \
            } \
            WAH_REALLOC_ARRAY(arr, _nc); \
            arr##_cap = _nc; \
        } \
    } while (0)

#define WAH_ENSURE_CAP_GOTO(arr, needed, label) do { \
        size_t _cap_needed_size = (needed); \
        WAH_ENSURE_GOTO(_cap_needed_size <= UINT32_MAX, WAH_ERROR_TOO_LARGE, label); \
        uint32_t _cap_needed = (uint32_t)_cap_needed_size; \
        if (_cap_needed > arr##_cap) { \
            uint32_t _nc = arr##_cap == 0 ? 8 : arr##_cap; \
            while (_nc < _cap_needed) { \
                WAH_ENSURE_GOTO(_nc <= UINT32_MAX / 2, WAH_ERROR_TOO_LARGE, label); \
                _nc *= 2; \
            } \
            WAH_REALLOC_ARRAY_GOTO(arr, _nc, label); \
            arr##_cap = _nc; \
        } \
    } while (0)

const char *wah_strerror(wah_error_t err) {
    switch (err) {
        case WAH_OK: return "Success";
        case WAH_ERROR_INVALID_MAGIC_NUMBER: return "Invalid WASM magic number";
        case WAH_ERROR_INVALID_VERSION: return "Invalid WASM version";
        case WAH_ERROR_UNEXPECTED_EOF: return "Unexpected end of file";
        case WAH_ERROR_MALFORMED: return "Malformed binary encoding";
        case WAH_ERROR_TOO_LARGE: return "Exceeding implementation limits (or value too large)";
        case WAH_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case WAH_ERROR_VALIDATION_FAILED: return "Validation failed";
        case WAH_ERROR_TRAP: return "Runtime trap";
        case WAH_ERROR_STACK_OVERFLOW: return "Stack overflow";
        case WAH_ERROR_MEMORY_OUT_OF_BOUNDS: return "Memory access out of bounds";
        case WAH_ERROR_NOT_FOUND: return "Item not found";
        case WAH_ERROR_MISUSE: return "API misused: invalid arguments";
        case WAH_ERROR_BAD_SPEC: return "Invalid DSL spec (turn on WAH_DEBUG for more info)";
        case WAH_ERROR_LINK_FAILED: return "Link failed (import not found or incompatible)";
        case WAH_ERROR_EXCEPTION: return "Uncaught exception";
        case WAH_ERROR_DISABLED_FEATURE: return "Feature not enabled";
        case WAH_ERROR_MULTI_RETURN: return "Function would return multiple values (use wah_call_multi)";
        case WAH_STATUS_FUEL_EXHAUSTED: return "Fuel exhausted";
        case WAH_STATUS_YIELDED: return "Execution yielded";
        default: return "Unknown error";
    }
}

////////////////////////////////////////////////////////////////////////////////
// Primitives and fallbacks ////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// WebAssembly canonical NaN bit patterns
//
// Note that WebAssembly prior to 3.0 had *two* canonical NaNs varying only by their signs.
// These are canonical in terms of the WebAssembly 3.0 deterministic profile, which needs a positive sign.
static const union { uint32_t i; float f; } WAH_CANONICAL_NAN32 = { .i = 0x7fc00000U };
static const union { uint64_t i; double f; } WAH_CANONICAL_NAN64 = { .i = 0x7ff8000000000000ULL };

static inline float wah_canonicalize_f32(float val) { return val == val ? val : WAH_CANONICAL_NAN32.f; }
static inline double wah_canonicalize_f64(double val) { return val == val ? val : WAH_CANONICAL_NAN64.f; }

#ifdef WAH_X86_64
// Some libms, notably MSVCRT, don't have correctly rounding sqrt[f] implementations!!
static inline float wah_sqrtf(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }
static inline double wah_sqrt(double v) { double r; _mm_store_sd(&r, _mm_sqrt_sd(_mm_set_sd(v), _mm_set_sd(v))); return r; }
#else
static inline float wah_sqrtf(float v) { return wah_canonicalize_f32(sqrtf(v)); }
static inline double wah_sqrt(double v) { return wah_canonicalize_f64(sqrt(v)); }
#endif

static inline float wah_ceilf(float v) { return wah_canonicalize_f32(ceilf(v)); }
static inline double wah_ceil(double v) { return wah_canonicalize_f64(ceil(v)); }
static inline float wah_floorf(float v) { return wah_canonicalize_f32(floorf(v)); }
static inline double wah_floor(double v) { return wah_canonicalize_f64(floor(v)); }
static inline float wah_truncf(float v) { return wah_canonicalize_f32(truncf(v)); }
static inline double wah_trunc(double v) { return wah_canonicalize_f64(trunc(v)); }

// C fmin/fmax don't match Wasm semantics for signed zeros:
// Wasm requires min(+0,-0)=-0 and max(+0,-0)=+0, but C leaves this unspecified.
// When both operands compare equal, bitwise OR of sign bits picks -0 (for min)
// and bitwise AND picks +0 (for max).
static inline float wah_minf(float a, float b) {
    if (a != a || b != b) return WAH_CANONICAL_NAN32.f;
    if (a < b) return a;
    if (b < a) return b;
    union { float f; uint32_t u; } ua = {a}, ub = {b};
    ua.u |= ub.u; return ua.f;
}
static inline float wah_maxf(float a, float b) {
    if (a != a || b != b) return WAH_CANONICAL_NAN32.f;
    if (a > b) return a;
    if (b > a) return b;
    union { float f; uint32_t u; } ua = {a}, ub = {b};
    ua.u &= ub.u; return ua.f;
}
static inline double wah_min(double a, double b) {
    if (a != a || b != b) return WAH_CANONICAL_NAN64.f;
    if (a < b) return a;
    if (b < a) return b;
    union { double f; uint64_t u; } ua = {a}, ub = {b};
    ua.u |= ub.u; return ua.f;
}
static inline double wah_max(double a, double b) {
    if (a != a || b != b) return WAH_CANONICAL_NAN64.f;
    if (a > b) return a;
    if (b > a) return b;
    union { double f; uint64_t u; } ua = {a}, ub = {b};
    ua.u &= ub.u; return ua.f;
}

#ifdef WAH_X86_64 // SSE2

static inline __m128 wah_canonicalize_f32x4_sse2(__m128 v) {
    __m128 nan_mask = wah_mm_cmpord_ps(v, v);
    return _mm_or_ps(_mm_and_ps(nan_mask, v), _mm_andnot_ps(nan_mask, _mm_set1_ps(WAH_CANONICAL_NAN32.f)));
}

static inline __m128d wah_canonicalize_f64x2_sse2(__m128d v) {
    __m128d nan_mask = wah_mm_cmpord_pd(v, v);
    return _mm_or_pd(_mm_and_pd(nan_mask, v), _mm_andnot_pd(nan_mask, _mm_set1_pd(WAH_CANONICAL_NAN64.f)));
}

#endif

#ifdef WAH_AARCH64 // NEON

static WAH_ALWAYS_INLINE uint8x16_t wah_canonicalize_f32x4_neon(uint8x16_t v) {
    uint32x4_t bits = vreinterpretq_u32_u8(v);
    uint32x4_t abs_bits = vandq_u32(bits, vdupq_n_u32(UINT32_C(0x7fffffff)));
    uint32x4_t nan_mask = vcgtq_u32(abs_bits, vdupq_n_u32(UINT32_C(0x7f800000)));
    return vreinterpretq_u8_u32(vbslq_u32(nan_mask, vdupq_n_u32(WAH_CANONICAL_NAN32.i), bits));
}

static WAH_ALWAYS_INLINE uint8x16_t wah_canonicalize_f64x2_neon(uint8x16_t v) {
    uint64x2_t bits = vreinterpretq_u64_u8(v);
    uint64x2_t abs_bits = vandq_u64(bits, vdupq_n_u64(UINT64_C(0x7fffffffffffffff)));
    uint64x2_t nan_mask = vcgtq_u64(abs_bits, vdupq_n_u64(UINT64_C(0x7ff0000000000000)));
    return vreinterpretq_u8_u64(vbslq_u64(nan_mask, vdupq_n_u64(WAH_CANONICAL_NAN64.i), bits));
}

#endif

// --- Integer Utility Functions ---

// popcnt
static WAH_ALWAYS_INLINE uint32_t wah_popcount_u32(uint32_t n) {
#if WAH_HAS_BUILTIN(__builtin_popcount) || defined(__GNUC__)
    return __builtin_popcount(n);
#elif defined(_MSC_VER)
    return __popcnt(n);
#else
    // Generic software implementation
    uint32_t count = 0;
    while (n > 0) {
        n &= (n - 1);
        count++;
    }
    return count;
#endif
}

static WAH_ALWAYS_INLINE uint64_t wah_popcount_u64(uint64_t n) {
#if WAH_HAS_BUILTIN(__builtin_popcountll) || defined(__GNUC__)
    return __builtin_popcountll(n);
#elif defined(_MSC_VER)
    return __popcnt64(n);
#else
    // Generic software implementation
    uint64_t count = 0;
    while (n > 0) {
        n &= (n - 1);
        count++;
    }
    return count;
#endif
}

static WAH_ALWAYS_INLINE uint8_t wah_popcount_u8(uint8_t n) {
    uint8_t count = 0;
    while (n > 0) {
        n &= (n - 1);
        count++;
    }
    return count;
}

// clz (count leading zeros)
static WAH_ALWAYS_INLINE uint32_t wah_clz_u32(uint32_t n) {
#if WAH_HAS_BUILTIN(__builtin_clz) || defined(__GNUC__)
    return n == 0 ? 32 : __builtin_clz(n);
#elif defined(_MSC_VER)
    unsigned long index;
    if (_BitScanReverse(&index, n)) {
        return 31 - index;
    } else {
        return 32; // All bits are zero
    }
#else
    // Generic software implementation
    if (n == 0) return 32;
    uint32_t count = 0;
    if (n <= 0x0000FFFF) { count += 16; n <<= 16; }
    if (n <= 0x00FFFFFF) { count += 8; n <<= 8; }
    if (n <= 0x0FFFFFFF) { count += 4; n <<= 4; }
    if (n <= 0x3FFFFFFF) { count += 2; n <<= 2; }
    if (n <= 0x7FFFFFFF) { count += 1; }
    return count;
#endif
}

static WAH_ALWAYS_INLINE uint64_t wah_clz_u64(uint64_t n) {
#if WAH_HAS_BUILTIN(__builtin_clzll) || defined(__GNUC__)
    return n == 0 ? 64 : __builtin_clzll(n);
#elif defined(_MSC_VER)
    unsigned long index;
    if (_BitScanReverse64(&index, n)) {
        return 63 - index;
    } else {
        return 64; // All bits are zero
    }
#else
    // Generic software implementation
    if (n == 0) return 64;
    uint64_t count = 0;
    if (n <= 0x00000000FFFFFFFFULL) { count += 32; n <<= 32; }
    if (n <= 0x0000FFFFFFFFFFFFULL) { count += 16; n <<= 16; }
    if (n <= 0x00FFFFFFFFFFFFFFULL) { count += 8; n <<= 8; }
    if (n <= 0x0FFFFFFFFFFFFFFFULL) { count += 4; n <<= 4; }
    if (n <= 0x3FFFFFFFFFFFFFFFULL) { count += 2; n <<= 2; }
    if (n <= 0x7FFFFFFFFFFFFFFFULL) { count += 1; }
    return count;
#endif
}

// ctz (count trailing zeros)
static WAH_ALWAYS_INLINE uint32_t wah_ctz_u32(uint32_t n) {
#if WAH_HAS_BUILTIN(__builtin_ctz) || defined(__GNUC__)
    return n == 0 ? 32 : __builtin_ctz(n);
#elif defined(_MSC_VER)
    unsigned long index;
    if (_BitScanForward(&index, n)) {
        return index;
    } else {
        return 32; // All bits are zero
    }
#else
    // Generic software implementation
    if (n == 0) return 32;
    uint32_t count = 0;
    while ((n & 1) == 0) {
        n >>= 1;
        count++;
    }
    return count;
#endif
}

static WAH_ALWAYS_INLINE uint64_t wah_ctz_u64(uint64_t n) {
#if WAH_HAS_BUILTIN(__builtin_ctzll) || defined(__GNUC__)
    return n == 0 ? 64 : __builtin_ctzll(n);
#elif defined(_MSC_VER)
    unsigned long index;
    if (_BitScanForward64(&index, n)) {
        return index;
    } else {
        return 64; // All bits are zero
    }
#else
    // Generic software implementation
    if (n == 0) return 64;
    uint64_t count = 0;
    while ((n & 1) == 0) {
        n >>= 1;
        count++;
    }
    return count;
#endif
}

// rotl (rotate left)
static WAH_ALWAYS_INLINE uint32_t wah_rotl_u32(uint32_t n, uint32_t shift) {
#if WAH_HAS_BUILTIN(__builtin_rotateleft32)
    return __builtin_rotateleft32(n, shift);
#elif defined(_MSC_VER)
    return _rotl(n, shift);
#else
    shift &= 31; // Ensure shift is within 0-31
    if (shift == 0) return n;
    return (n << shift) | (n >> (32 - shift));
#endif
}

static WAH_ALWAYS_INLINE uint64_t wah_rotl_u64(uint64_t n, uint64_t shift) {
#if WAH_HAS_BUILTIN(__builtin_rotateleft64)
    return __builtin_rotateleft64(n, shift);
#elif defined(_MSC_VER)
    return _rotl64(n, (int)shift);
#else
    shift &= 63; // Ensure shift is within 0-63
    if (shift == 0) return n;
    return (n << shift) | (n >> (64 - shift));
#endif
}

// rotr (rotate right)
static WAH_ALWAYS_INLINE uint32_t wah_rotr_u32(uint32_t n, uint32_t shift) {
#if WAH_HAS_BUILTIN(__builtin_rotateright32)
    return __builtin_rotateright32(n, shift);
#elif defined(_MSC_VER)
    return _rotr(n, shift);
#else
    shift &= 31; // Ensure shift is within 0-31
    if (shift == 0) return n;
    return (n >> shift) | (n << (32 - shift));
#endif
}

static WAH_ALWAYS_INLINE uint64_t wah_rotr_u64(uint64_t n, uint64_t shift) {
#if WAH_HAS_BUILTIN(__builtin_rotateright64)
    return __builtin_rotateright64(n, shift);
#elif defined(_MSC_VER)
    return _rotr64(n, (int)shift);
#else
    shift &= 63; // Ensure shift is within 0-63
    if (shift == 0) return n;
    return (n >> shift) | (n << (64 - shift));
#endif
}

// Helper function for Q15 multiplication with rounding and saturation
static WAH_ALWAYS_INLINE wah_v128_t wah_q15mulr_sat_s(wah_v128_t a, wah_v128_t b) {
    wah_v128_t result;
    for (int i = 0; i < 8; ++i) {
        int32_t prod = (int32_t)a.i16[i] * b.i16[i];
        int32_t res = (prod + 16384) >> 15; // Rounding and shift
        if (res > 32767) res = 32767;
        if (res < -32768) res = -32768;
        result.i16[i] = (int16_t)res;
    }
    return result;
}

#ifdef WAH_X86_64

static WAH_ALWAYS_INLINE int32_t wah_i16x8_bitmask_sse2(__m128i v) {
    int32_t mask = _mm_movemask_epi8(v) & 0xAAAA; // extract high bit of each byte, keep even bytes (MSB of each i16)
    mask = (mask >> 1) & 0x5555; // shift to bit positions 0,2,4,...
    mask = (mask | (mask >> 1)) & 0x3333;
    mask = (mask | (mask >> 2)) & 0x0f0f;
    return (mask | (mask >> 4)) & 0x00ff;
}

static WAH_ALWAYS_INLINE int32_t wah_i32x4_bitmask_sse2(__m128i v) {
    return _mm_movemask_ps(_mm_castsi128_ps(v));
}

static WAH_ALWAYS_INLINE int32_t wah_i64x2_bitmask_sse2(__m128i v) {
    return _mm_movemask_pd(_mm_castsi128_pd(v));
}

#endif

// abs/neg as bitwise operations (wasm spec requires NaN payload preservation)
static WAH_ALWAYS_INLINE float wah_fabsf(float f) {
    union { float f; uint32_t i; } u = { .f = f };
    u.i &= 0x7fffffffU;
    return u.f;
}
static WAH_ALWAYS_INLINE double wah_fabs(double d) {
    union { double f; uint64_t i; } u = { .f = d };
    u.i &= 0x7fffffffffffffffULL;
    return u.f;
}

// nearest (round to nearest, ties to even)
// XXX: Clang doesn't support __builtin_roundeven(f) without recent enough -march, so we opt in for known archs
static WAH_ALWAYS_INLINE float wah_nearest_f32(float f) {
#if WAH_HAS_BUILTIN(__builtin_roundevenf) && defined(__clang__) && defined(__SSE4_1__)
    return __builtin_roundevenf(f);
#else
    if (isnan(f)) return WAH_CANONICAL_NAN32.f;
    if (isinf(f) || f == 0.0f) return f;
    float rounded = roundf(f);
    if (fabsf(f - rounded) == 0.5f && ((long long)rounded % 2) != 0) {
        float adjusted = rounded - copysignf(1.0f, f);
        return adjusted == 0.0f ? copysignf(0.0f, f) : adjusted;
    }
    return rounded;
#endif
}

static WAH_ALWAYS_INLINE double wah_nearest_f64(double d) {
#if WAH_HAS_BUILTIN(__builtin_roundeven) && defined(__clang__) && defined(__SSE4_1__)
    return __builtin_roundeven(d);
#else
    if (isnan(d)) return WAH_CANONICAL_NAN64.f;
    if (isinf(d) || d == 0.0) return d;
    double rounded = round(d);
    if (fabs(d - rounded) == 0.5 && ((long long)rounded % 2) != 0) {
        double adjusted = rounded - copysign(1.0, d);
        return adjusted == 0.0 ? copysign(0.0, d) : adjusted;
    }
    return rounded;
#endif
}

// Helper functions for floating-point to integer truncations with trap handling
#define DEFINE_TRUNC_F2I(N, fty, T, ity, lo, hi, call) \
static WAH_ALWAYS_INLINE wah_error_t wah_trunc_f##N##_to_##T(fty val, ity *result) { \
    if (isnan(val) || isinf(val)) return WAH_ERROR_TRAP; \
    fty tv = call(val); \
    if (tv < (lo) || tv >= (hi)) return WAH_ERROR_TRAP; \
    *result = (ity)tv; \
    return WAH_OK; \
}

DEFINE_TRUNC_F2I(32, float,  i32,  int32_t,  (float)INT32_MIN,  (float) INT32_MAX + 1.0f, truncf)
DEFINE_TRUNC_F2I(32, float,  u32, uint32_t,                 0,  (float)UINT32_MAX + 1.0f, truncf)
DEFINE_TRUNC_F2I(64, double, i32,  int32_t, (double)INT32_MIN, (double) INT32_MAX + 1.0,  trunc)
DEFINE_TRUNC_F2I(64, double, u32, uint32_t,                 0, (double)UINT32_MAX + 1.0,  trunc)
DEFINE_TRUNC_F2I(32, float,  i64,  int64_t,  (float)INT64_MIN,  (float) INT64_MAX + 1.0f, truncf)
DEFINE_TRUNC_F2I(32, float,  u64, uint64_t,                 0,  (float)UINT64_MAX + 1.0f, truncf)
DEFINE_TRUNC_F2I(64, double, i64,  int64_t, (double)INT64_MIN, (double) INT64_MAX + 1.0,  trunc)
DEFINE_TRUNC_F2I(64, double, u64, uint64_t,                 0, (double)UINT64_MAX + 1.0,  trunc)

// Helper functions for floating-point to integer truncations with saturation
#define DEFINE_TRUNC_SAT_F2I(N, fty, T, ity, min_val, max_val, call) \
static WAH_ALWAYS_INLINE ity wah_trunc_sat_f##N##_to_##T(fty val) { \
    if (isnan(val)) return 0; \
    if (val <= (fty)min_val) return min_val; \
    if (val >= (fty)max_val) return max_val; \
    return (ity)call(val); \
}

DEFINE_TRUNC_SAT_F2I(32, float,  i32,  int32_t,  INT32_MIN,  INT32_MAX, truncf)
DEFINE_TRUNC_SAT_F2I(32, float,  u32, uint32_t,          0, UINT32_MAX, truncf)
DEFINE_TRUNC_SAT_F2I(64, double, i32,  int32_t,  INT32_MIN,  INT32_MAX, trunc)
DEFINE_TRUNC_SAT_F2I(64, double, u32, uint32_t,          0, UINT32_MAX, trunc)
DEFINE_TRUNC_SAT_F2I(32, float,  i64,  int64_t,  INT64_MIN,  INT64_MAX, truncf)
DEFINE_TRUNC_SAT_F2I(32, float,  u64, uint64_t,          0, UINT64_MAX, truncf)
DEFINE_TRUNC_SAT_F2I(64, double, i64,  int64_t,  INT64_MIN,  INT64_MAX, trunc)
DEFINE_TRUNC_SAT_F2I(64, double, u64, uint64_t,          0, UINT64_MAX, trunc)

static WAH_ALWAYS_INLINE int8_t wah_trunc_sat_i16_to_i8(int16_t val) {
    if (val < -128) return -128;
    if (val > 127) return 127;
    return (int8_t)val;
}

static WAH_ALWAYS_INLINE uint8_t wah_trunc_sat_i16_to_u8(int16_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

static WAH_ALWAYS_INLINE int16_t wah_trunc_sat_i32_to_i16(int32_t val) {
    if (val < -32768) return -32768;
    if (val > 32767) return 32767;
    return (int16_t)val;
}

static WAH_ALWAYS_INLINE uint16_t wah_trunc_sat_i32_to_u16(int32_t val) {
    if (val < 0) return 0;
    if (val > 65535) return 65535;
    return (uint16_t)val;
}

static WAH_ALWAYS_INLINE int16_t WAH_MIN_S_16(int16_t a, int16_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE uint16_t WAH_MIN_U_16(uint16_t a, uint16_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE int16_t WAH_MAX_S_16(int16_t a, int16_t b) { return a > b ? a : b; }
static WAH_ALWAYS_INLINE uint16_t WAH_MAX_U_16(uint16_t a, uint16_t b) { return a > b ? a : b; }

static WAH_ALWAYS_INLINE int32_t WAH_MIN_S_32(int32_t a, int32_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE uint32_t WAH_MIN_U_32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE int32_t WAH_MAX_S_32(int32_t a, int32_t b) { return a > b ? a : b; }
static WAH_ALWAYS_INLINE uint32_t WAH_MAX_U_32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static WAH_ALWAYS_INLINE int8_t WAH_MIN_S_8(int8_t a, int8_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE uint8_t WAH_MIN_U_8(uint8_t a, uint8_t b) { return a < b ? a : b; }
static WAH_ALWAYS_INLINE int8_t WAH_MAX_S_8(int8_t a, int8_t b) { return a > b ? a : b; }
static WAH_ALWAYS_INLINE uint8_t WAH_MAX_U_8(uint8_t a, uint8_t b) { return a > b ? a : b; }

static WAH_ALWAYS_INLINE float wah_pminf(float a, float b) { return b < a ? b : a; }
static WAH_ALWAYS_INLINE float wah_pmaxf(float a, float b) { return a < b ? b : a; }
static WAH_ALWAYS_INLINE double wah_pmin(double a, double b) { return b < a ? b : a; }
static WAH_ALWAYS_INLINE double wah_pmax(double a, double b) { return a < b ? b : a; }

#ifdef WAH_X86_64

static WAH_ALWAYS_INLINE __m128 wah_f32x4_pmin_sse2(__m128 a, __m128 b) { return _mm_min_ps(b, a); }
static WAH_ALWAYS_INLINE __m128 wah_f32x4_pmax_sse2(__m128 a, __m128 b) { return _mm_max_ps(b, a); }
static WAH_ALWAYS_INLINE __m128d wah_f64x2_pmin_sse2(__m128d a, __m128d b) { return _mm_min_pd(b, a); }
static WAH_ALWAYS_INLINE __m128d wah_f64x2_pmax_sse2(__m128d a, __m128d b) { return _mm_max_pd(b, a); }

static WAH_ALWAYS_INLINE __m128 wah_f32x4_min_sse2(__m128 a, __m128 b) {
    __m128 nan_mask = _mm_or_ps(_mm_cmpunord_ps(a, a), _mm_cmpunord_ps(b, b));
    __m128 canon_nan = _mm_set1_ps(WAH_CANONICAL_NAN32.f);
    __m128 min_ab = _mm_min_ps(a, b);
    __m128 min_ba = _mm_min_ps(b, a);
    __m128 result = _mm_or_ps(min_ab, min_ba);
    return _mm_or_ps(_mm_andnot_ps(nan_mask, result), _mm_and_ps(nan_mask, canon_nan));
}
static WAH_ALWAYS_INLINE __m128 wah_f32x4_max_sse2(__m128 a, __m128 b) {
    __m128 nan_mask = _mm_or_ps(_mm_cmpunord_ps(a, a), _mm_cmpunord_ps(b, b));
    __m128 canon_nan = _mm_set1_ps(WAH_CANONICAL_NAN32.f);
    __m128 max_ab = _mm_max_ps(a, b);
    __m128 max_ba = _mm_max_ps(b, a);
    __m128 result = _mm_and_ps(max_ab, max_ba);
    return _mm_or_ps(_mm_andnot_ps(nan_mask, result), _mm_and_ps(nan_mask, canon_nan));
}
static WAH_ALWAYS_INLINE __m128d wah_f64x2_min_sse2(__m128d a, __m128d b) {
    __m128d nan_mask = _mm_or_pd(_mm_cmpunord_pd(a, a), _mm_cmpunord_pd(b, b));
    __m128d canon_nan = _mm_set1_pd(WAH_CANONICAL_NAN64.f);
    __m128d min_ab = _mm_min_pd(a, b);
    __m128d min_ba = _mm_min_pd(b, a);
    __m128d result = _mm_or_pd(min_ab, min_ba);
    return _mm_or_pd(_mm_andnot_pd(nan_mask, result), _mm_and_pd(nan_mask, canon_nan));
}
static WAH_ALWAYS_INLINE __m128d wah_f64x2_max_sse2(__m128d a, __m128d b) {
    __m128d nan_mask = _mm_or_pd(_mm_cmpunord_pd(a, a), _mm_cmpunord_pd(b, b));
    __m128d canon_nan = _mm_set1_pd(WAH_CANONICAL_NAN64.f);
    __m128d max_ab = _mm_max_pd(a, b);
    __m128d max_ba = _mm_max_pd(b, a);
    __m128d result = _mm_and_pd(max_ab, max_ba);
    return _mm_or_pd(_mm_andnot_pd(nan_mask, result), _mm_and_pd(nan_mask, canon_nan));
}

#define DEFINE_CMP_U(wasm_type, cmp, const, N) \
static WAH_ALWAYS_INLINE __m128i wah_##wasm_type##_##cmp##_u_sse2(__m128i a, __m128i b) { \
    __m128i sign_bit = _mm_set1_epi##N(const); \
    __m128i a_flipped = _mm_xor_si128(a, sign_bit); \
    __m128i b_flipped = _mm_xor_si128(b, sign_bit); \
    return _mm_cmp##cmp##_epi##N(a_flipped, b_flipped); \
}

DEFINE_CMP_U(i8x16, lt, 0x80, 8)
DEFINE_CMP_U(i8x16, gt, 0x80, 8)
DEFINE_CMP_U(i16x8, lt, 0x8000, 16)
DEFINE_CMP_U(i16x8, gt, 0x8000, 16)
DEFINE_CMP_U(i32x4, lt, 0x80000000, 32)
DEFINE_CMP_U(i32x4, gt, 0x80000000, 32)

static WAH_ALWAYS_INLINE __m128i wah_v128_not_sse2(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi8(0xff)); }
static WAH_ALWAYS_INLINE __m128i wah_v128_andnot_rev_sse2(__m128i a, __m128i b) { return _mm_andnot_si128(b, a); }

// I64X2 multiply using SSE2
// Computes low 64 bits of 64x64 multiplication (what WebAssembly requires)
static WAH_ALWAYS_INLINE __m128i wah_i64x2_mul_sse2(__m128i a, __m128i b) {
    __m128i a_hi = _mm_srli_epi64(a, 32);
    __m128i b_hi = _mm_srli_epi64(b, 32);
    __m128i prod_ll = _mm_mul_epu32(a, b);
    __m128i prod_hl = _mm_mul_epu32(a_hi, b);
    __m128i prod_lh = _mm_mul_epu32(a, b_hi);
    __m128i cross = _mm_add_epi64(prod_hl, prod_lh);
    __m128i cross_shifted = _mm_slli_epi64(cross, 32);
    return _mm_add_epi64(prod_ll, cross_shifted);
}

// I8X16 popcount using SSE2
// Counts set bits in each of 16 bytes
static WAH_ALWAYS_INLINE __m128i wah_i8x16_popcnt_sse2(__m128i v) {
    // Parallel popcount: count bits in pairs, then nibbles, then bytes

    // Step 1: Count bits in pairs (2 bits)
    // For each 2-bit pair, add the bits together
    __m128i m1 = _mm_set1_epi8(0x55);  // 01010101
    __m128i v1 = _mm_and_si128(v, m1);                     // Even-position bits
    __m128i v2 = _mm_and_si128(_mm_srli_epi64(v, 1), m1);   // Odd-position bits
    __m128i pair_count = _mm_add_epi8(v1, v2);              // Count in each pair (0-2)

    // Step 2: Count bits in nibbles (4 bits)
    // Add adjacent 2-bit counts
    __m128i m2 = _mm_set1_epi8(0x33);  // 00110011
    __m128i pc_low = _mm_and_si128(pair_count, m2);
    __m128i pc_high = _mm_and_si128(_mm_srli_epi64(pair_count, 2), m2);
    __m128i nibble_count = _mm_add_epi8(pc_low, pc_high);  // Count in each nibble (0-4)

    // Step 3: Count bits in bytes (8 bits)
    // Add adjacent nibble counts
    __m128i nc_low = _mm_and_si128(nibble_count, _mm_set1_epi8(0x0f));
    __m128i nc_high = _mm_and_si128(_mm_srli_epi64(nibble_count, 4), _mm_set1_epi8(0x0f));
    __m128i byte_count = _mm_add_epi8(nc_low, nc_high);  // Count in each byte (0-8)

    return byte_count;
}

// abs(x) = (x ^ mask) - mask where mask = (x < 0) ? 0xFF : 0x00
static WAH_ALWAYS_INLINE __m128i wah_i8x16_abs_sse2(__m128i x) {
    __m128i mask = _mm_cmplt_epi8(x, _mm_setzero_si128());
    return _mm_sub_epi8(_mm_xor_si128(x, mask), mask);
}
static WAH_ALWAYS_INLINE __m128i wah_i16x8_abs_sse2(__m128i x) {
    __m128i mask = _mm_cmplt_epi16(x, _mm_setzero_si128());
    return _mm_sub_epi16(_mm_xor_si128(x, mask), mask);
}
static WAH_ALWAYS_INLINE __m128i wah_i32x4_abs_sse2(__m128i x) {
    __m128i mask = _mm_cmplt_epi32(x, _mm_setzero_si128());
    return _mm_sub_epi32(_mm_xor_si128(x, mask), mask);
}
static WAH_ALWAYS_INLINE __m128i wah_i64x2_abs_sse2(__m128i x) { // For 64-bit: if MSB is set, negate: (~x + 1)
    __m128i mask = _mm_srai_epi32(x, 31);  // Get sign bit of each 64-bit element (as 32-bit pairs)
    __m128i mask64 = _mm_shuffle_epi32(mask, _MM_SHUFFLE(3, 3, 1, 1));  // Spread sign to both 32-bit parts
    __m128i neg = _mm_sub_epi64(_mm_xor_si128(x, mask64), mask64);
    __m128i is_neg = _mm_cmpeq_epi32(mask, _mm_set1_epi32(0xFFFFFFFF));
    __m128i is_neg64 = _mm_shuffle_epi32(is_neg, _MM_SHUFFLE(3, 3, 1, 1));
    return _mm_or_si128(_mm_and_si128(neg, is_neg64), _mm_andnot_si128(is_neg64, x));
}

// SSE2 implementations for SSE4.1 SIMD instructions

static WAH_ALWAYS_INLINE __m128i wah_i8x16_min_s_sse2(__m128i a, __m128i b) {
    __m128i mask = _mm_cmplt_epi8(a, b);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}
static WAH_ALWAYS_INLINE __m128i wah_i8x16_max_s_sse2(__m128i a, __m128i b) {
    __m128i mask = _mm_cmpgt_epi8(a, b);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

static WAH_ALWAYS_INLINE __m128i wah_i16x8_min_u_sse2(__m128i a, __m128i b) {
    __m128i a_xor = _mm_xor_si128(a, _mm_set1_epi16(INT16_MIN));
    __m128i b_xor = _mm_xor_si128(b, _mm_set1_epi16(INT16_MIN));
    __m128i mask = _mm_cmplt_epi16(a_xor, b_xor);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}
static WAH_ALWAYS_INLINE __m128i wah_i16x8_max_u_sse2(__m128i a, __m128i b) {
    __m128i a_xor = _mm_xor_si128(a, _mm_set1_epi16(INT16_MIN));
    __m128i b_xor = _mm_xor_si128(b, _mm_set1_epi16(INT16_MIN));
    __m128i mask = _mm_cmpgt_epi16(a_xor, b_xor);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

static WAH_ALWAYS_INLINE __m128i wah_i32x4_mul_sse2(__m128i a, __m128i b) {
    // i32x4 mul: multiply each 32-bit lane, keep low 32 bits of result
    // SSE2's _mm_mul_epi32 only works on 2 elements at a time, so we do it in pairs

    // Multiply a[0,2] * b[0,2]
    __m128i prod02 = _mm_mul_epu32(a, b);
    // prod02 = [a0*b0 (64-bit), a2*b2 (64-bit)]

    // Multiply a[1,3] * b[1,3] - need to shuffle first
    __m128i a_13 = _mm_shuffle_epi32(a, _MM_SHUFFLE(3, 3, 1, 1));
    __m128i b_13 = _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 3, 1, 1));
    __m128i prod13 = _mm_mul_epu32(a_13, b_13);
    // prod13 = [a1*b1 (64-bit), a3*b3 (64-bit)]

    // Interleave low 32 bits to get final order: [a0*b0, a1*b1, a2*b2, a3*b3]
    return _mm_unpacklo_epi32(prod02, prod13);
}

static WAH_ALWAYS_INLINE __m128i wah_i32x4_min_s_sse2(__m128i a, __m128i b) {
    __m128i mask = _mm_cmplt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}
static WAH_ALWAYS_INLINE __m128i wah_i32x4_max_s_sse2(__m128i a, __m128i b) {
    __m128i mask = _mm_cmpgt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

static WAH_ALWAYS_INLINE __m128i wah_i32x4_min_u_sse2(__m128i a, __m128i b) {
    __m128i a_xor = _mm_xor_si128(a, _mm_set1_epi32(0x80000000));
    __m128i b_xor = _mm_xor_si128(b, _mm_set1_epi32(0x80000000));
    __m128i mask = _mm_cmplt_epi32(a_xor, b_xor);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}
static WAH_ALWAYS_INLINE __m128i wah_i32x4_max_u_sse2(__m128i a, __m128i b) {
    __m128i a_xor = _mm_xor_si128(a, _mm_set1_epi32(0x80000000));
    __m128i b_xor = _mm_xor_si128(b, _mm_set1_epi32(0x80000000));
    __m128i mask = _mm_cmpgt_epi32(a_xor, b_xor);
    return _mm_or_si128(_mm_and_si128(mask, a), _mm_andnot_si128(mask, b));
}

static WAH_ALWAYS_INLINE __m128 wah_f32x4_convert_i32x4_u_sse2(__m128i a) {
    __m128i lo16 = _mm_and_si128(a, _mm_set1_epi32(0xFFFF));
    __m128i hi16 = _mm_srli_epi32(a, 16);
    __m128 lo_f = _mm_cvtepi32_ps(lo16);
    __m128 hi_f = _mm_cvtepi32_ps(hi16);
    return _mm_add_ps(lo_f, _mm_mul_ps(hi_f, _mm_set1_ps(65536.0f)));
}

static WAH_ALWAYS_INLINE __m128i wah_i16x8_narrow_i32x4_u_sse2(__m128i a, __m128i b) {
    __m128i max_val = _mm_set1_epi32(0xFFFF);
    __m128i a_neg = _mm_srai_epi32(a, 31);
    __m128i b_neg = _mm_srai_epi32(b, 31);
    __m128i a_gt = _mm_cmpgt_epi32(a, max_val);
    __m128i b_gt = _mm_cmpgt_epi32(b, max_val);
    __m128i ac = _mm_andnot_si128(a_neg, _mm_or_si128(_mm_and_si128(a_gt, max_val), _mm_andnot_si128(a_gt, a)));
    __m128i bc = _mm_andnot_si128(b_neg, _mm_or_si128(_mm_and_si128(b_gt, max_val), _mm_andnot_si128(b_gt, b)));
    ac = _mm_and_si128(ac, max_val);
    bc = _mm_and_si128(bc, max_val);
    __m128i bias = _mm_set1_epi32(0x8000);
    __m128i result = _mm_packs_epi32(_mm_sub_epi32(ac, bias), _mm_sub_epi32(bc, bias));
    return _mm_add_epi16(result, _mm_set1_epi16(INT16_MIN));
}

static WAH_ALWAYS_INLINE int32_t wah_i64x2_all_true_sse2(__m128i v) {
    __m128i eq = _mm_cmpeq_epi32(v, _mm_setzero_si128());
    __m128i eq64 = _mm_and_si128(eq, _mm_shuffle_epi32(eq, _MM_SHUFFLE(2, 3, 0, 1)));
    return (_mm_movemask_epi8(eq64) & 0x0101) == 0;
}

// Since SSE2 has no byte shift, we unpack to 16-bit, shift, and repack
static WAH_ALWAYS_INLINE __m128i wah_i8x16_shl_sse2(__m128i a, int32_t count) {
    count &= 7;  // Mask to 0-7
    if (count == 0) return a;
    __m128i zero = _mm_setzero_si128();
    __m128i a_lo = _mm_unpacklo_epi8(a, zero);   // [a0, a1, ..., a7] as 16-bit
    __m128i a_hi = _mm_unpackhi_epi8(a, zero);   // [a8, a9, ..., a15] as 16-bit
    __m128i lo = _mm_slli_epi16(a_lo, count);
    __m128i hi = _mm_slli_epi16(a_hi, count);
    // Mask to get only low 8 bits of each 16-bit element
    __m128i mask = _mm_set1_epi16(0xFF);
    lo = _mm_and_si128(lo, mask);
    hi = _mm_and_si128(hi, mask);
    return _mm_packus_epi16(lo, hi);
}
static WAH_ALWAYS_INLINE __m128i wah_i8x16_shr_s_sse2(__m128i a, int32_t count) {
    count &= 7;
    if (count == 0) return a;
    __m128i sign = _mm_cmpgt_epi8(_mm_setzero_si128(), a);
    __m128i a_lo = _mm_unpacklo_epi8(a, sign);
    __m128i a_hi = _mm_unpackhi_epi8(a, sign);
    __m128i lo = _mm_srai_epi16(a_lo, count);
    __m128i hi = _mm_srai_epi16(a_hi, count);
    return _mm_packs_epi16(lo, hi);
}
static WAH_ALWAYS_INLINE __m128i wah_i8x16_shr_u_sse2(__m128i a, int32_t count) {
    count &= 7;  // Mask to 0-7
    if (count == 0) return a;
    __m128i zero = _mm_setzero_si128();
    __m128i a_lo = _mm_unpacklo_epi8(a, zero);
    __m128i a_hi = _mm_unpackhi_epi8(a, zero);
    __m128i lo = _mm_srli_epi16(a_lo, count);
    __m128i hi = _mm_srli_epi16(a_hi, count);
    return _mm_packus_epi16(lo, hi);
}

// I16X8 relaxed dot: i8x16 (signed) * i7x16, sum adjacent pairs -> i16x8
// Deterministic profile: treat i7 operand as signed i8 directly.
static WAH_ALWAYS_INLINE __m128i wah_i16x8_relaxed_dot_i8x16_i7x16_s_sse2(__m128i a, __m128i b) {
    __m128i zero = _mm_setzero_si128();
    __m128i a_sign = _mm_cmplt_epi8(a, zero);
    __m128i b_sign = _mm_cmplt_epi8(b, zero);
    __m128i a_lo = _mm_unpacklo_epi8(a, a_sign);
    __m128i a_hi = _mm_unpackhi_epi8(a, a_sign);
    __m128i b_lo = _mm_unpacklo_epi8(b, b_sign);
    __m128i b_hi = _mm_unpackhi_epi8(b, b_sign);
    // pmaddwd: result[i] = a[2i]*b[2i] + a[2i+1]*b[2i+1] as i32
    __m128i dot_lo = _mm_madd_epi16(a_lo, b_lo);   // 4 i32 values from bytes 0-7
    __m128i dot_hi = _mm_madd_epi16(a_hi, b_hi);   // 4 i32 values from bytes 8-15
    // Pack i32 -> i16; max |result| = 127*63*2 = 16002 < 32767, no saturation
    return _mm_packs_epi32(dot_lo, dot_hi);
}

// I32X4 relaxed dot-add: sum 4 consecutive (i8*i7) products per lane, add accumulator
// Uses shuffle trick for horizontal add (avoids SSSE3 phaddd).
static WAH_ALWAYS_INLINE __m128i wah_i32x4_relaxed_dot_i8x16_i7x16_add_s_sse2(__m128i a, __m128i b, __m128i c) {
    __m128i zero = _mm_setzero_si128();
    __m128i a_sign = _mm_cmplt_epi8(a, zero);
    __m128i b_sign = _mm_cmplt_epi8(b, zero);
    __m128i a_lo = _mm_unpacklo_epi8(a, a_sign);
    __m128i a_hi = _mm_unpackhi_epi8(a, a_sign);
    __m128i b_lo = _mm_unpacklo_epi8(b, b_sign);
    __m128i b_hi = _mm_unpackhi_epi8(b, b_sign);
    // dots_lo[i] = a[2i]*b[2i] + a[2i+1]*b[2i+1] as i32 (4 results from bytes 0-7)
    // dots_hi[i] = a[8+2i]*b[8+2i] + a[9+2i]*b[9+2i] as i32 (4 results from bytes 8-15)
    __m128i dots_lo = _mm_madd_epi16(a_lo, b_lo);
    __m128i dots_hi = _mm_madd_epi16(a_hi, b_hi);
    // Horizontal sum of adjacent pairs using float shuffle (SSE1, no SSSE3 required):
    // shuffle_ps(a, b, {0,2,0,2}) = [a[0],a[2],b[0],b[2]] (even lanes)
    // shuffle_ps(a, b, {1,3,1,3}) = [a[1],a[3],b[1],b[3]] (odd lanes)
    // sum = [a[0]+a[1], a[2]+a[3], b[0]+b[1], b[2]+b[3]]
    __m128i evens = _mm_castps_si128(_mm_shuffle_ps(
        _mm_castsi128_ps(dots_lo), _mm_castsi128_ps(dots_hi), _MM_SHUFFLE(2,0,2,0)));
    __m128i odds  = _mm_castps_si128(_mm_shuffle_ps(
        _mm_castsi128_ps(dots_lo), _mm_castsi128_ps(dots_hi), _MM_SHUFFLE(3,1,3,1)));
    return _mm_add_epi32(_mm_add_epi32(evens, odds), c);
}

WAH_IF_SSE41(
    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extend_high_i8x16_s_sse41(__m128i a) {
        // Shuffle bytes 8-15 to low positions: [8,9,10,11,12,13,14,15, 0x80,...]
        return wah_mm_cvtepi8_epi16(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extend_high_i8x16_u_sse41(__m128i a) {
        return wah_mm_cvtepu8_epi16(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extend_high_i16x8_s_sse41(__m128i a) {
        // Shuffle words 4-7 (bytes 8-15) to low positions
        return wah_mm_cvtepi16_epi32(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extend_high_i16x8_u_sse41(__m128i a) {
        return wah_mm_cvtepu16_epi32(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extend_high_i32x4_s_sse41(__m128i a) {
        // Shuffle dwords 2-3 (bytes 8-15) to low positions
        return wah_mm_cvtepi32_epi64(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extend_high_i32x4_u_sse41(__m128i a) {
        return wah_mm_cvtepu32_epi64(wah_mm_shuffle_epi8(a, _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8)));
    }

    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extmul_low_i8x16_s_sse41(__m128i a, __m128i b) {
        return _mm_mullo_epi16(wah_mm_cvtepi8_epi16(a), wah_mm_cvtepi8_epi16(b));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extmul_low_i8x16_u_sse41(__m128i a, __m128i b) {
        return _mm_mullo_epi16(wah_mm_cvtepu8_epi16(a), wah_mm_cvtepu8_epi16(b));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extmul_high_i8x16_s_sse41(__m128i a, __m128i b) {
        __m128i hi_shuf = _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8);
        return _mm_mullo_epi16(
            wah_mm_cvtepi8_epi16(wah_mm_shuffle_epi8(a, hi_shuf)),
            wah_mm_cvtepi8_epi16(wah_mm_shuffle_epi8(b, hi_shuf)));
    }
    static WAH_ALWAYS_INLINE __m128i wah_i16x8_extmul_high_i8x16_u_sse41(__m128i a, __m128i b) {
        __m128i hi_shuf = _mm_set_epi8(0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80, 15,14,13,12,11,10,9,8);
        return _mm_mullo_epi16(
            wah_mm_cvtepu8_epi16(wah_mm_shuffle_epi8(a, hi_shuf)),
            wah_mm_cvtepu8_epi16(wah_mm_shuffle_epi8(b, hi_shuf)));
    }

    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extmul_low_i16x8_s_sse41(__m128i a, __m128i b) {
        __m128i a_ext = wah_mm_cvtepi16_epi32(a);
        __m128i b_ext = wah_mm_cvtepi16_epi32(b);
        return wah_mm_mullo_epi32(a_ext, b_ext);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extmul_low_i16x8_u_sse41(__m128i a, __m128i b) {
        __m128i a_ext = wah_mm_cvtepu16_epi32(a);
        __m128i b_ext = wah_mm_cvtepu16_epi32(b);
        return wah_mm_mullo_epi32(a_ext, b_ext);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extmul_high_i16x8_s_sse41(__m128i a, __m128i b) {
        __m128i a_ext = wah_mm_cvtepi16_epi32(_mm_srli_si128(a, 8));
        __m128i b_ext = wah_mm_cvtepi16_epi32(_mm_srli_si128(b, 8));
        return wah_mm_mullo_epi32(a_ext, b_ext);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i32x4_extmul_high_i16x8_u_sse41(__m128i a, __m128i b) {
        __m128i a_ext = wah_mm_cvtepu16_epi32(_mm_srli_si128(a, 8));
        __m128i b_ext = wah_mm_cvtepu16_epi32(_mm_srli_si128(b, 8));
        return wah_mm_mullo_epi32(a_ext, b_ext);
    }

    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extmul_low_i32x4_s_sse41(__m128i a, __m128i b) {
        __m128i prod0 = wah_mm_mul_epi32(a, b);
        __m128i prod1 = wah_mm_mul_epi32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
        return _mm_unpacklo_epi64(prod0, prod1);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extmul_low_i32x4_u_sse41(__m128i a, __m128i b) {
        __m128i prod0 = _mm_mul_epu32(a, b);
        __m128i prod1 = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(b, 32));
        return _mm_unpacklo_epi64(prod0, prod1);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extmul_high_i32x4_s_sse41(__m128i a, __m128i b) {
        __m128i a_hi = _mm_srli_si128(a, 8);
        __m128i b_hi = _mm_srli_si128(b, 8);
        __m128i prod0 = wah_mm_mul_epi32(a_hi, b_hi);
        __m128i prod1 = wah_mm_mul_epi32(_mm_srli_epi64(a_hi, 32), _mm_srli_epi64(b_hi, 32));
        return _mm_unpacklo_epi64(prod0, prod1);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_extmul_high_i32x4_u_sse41(__m128i a, __m128i b) {
        __m128i a_hi = _mm_srli_si128(a, 8);
        __m128i b_hi = _mm_srli_si128(b, 8);
        __m128i prod0 = _mm_mul_epu32(a_hi, b_hi);
        __m128i prod1 = _mm_mul_epu32(_mm_srli_epi64(a_hi, 32), _mm_srli_epi64(b_hi, 32));
        return _mm_unpacklo_epi64(prod0, prod1);
    }

    static WAH_ALWAYS_INLINE __m128i wah_i32x4_trunc_sat_f32x4_s_sse41(__m128 a) {
        // First truncate (no saturation)
        __m128i truncated = _mm_cvttps_epi32(a);

        // Check for NaN and create a mask
        __m128 cmp_nan = _mm_cmpunord_ps(a, a);

        // Check for overflow - use >= and <= to handle exact boundary values
        __m128 cmp_gt = _mm_cmpge_ps(a, _mm_set1_ps((float)INT32_MAX));
        __m128 cmp_lt = _mm_cmple_ps(a, _mm_set1_ps((float)INT32_MIN));

        // Select: if NaN -> 0, if >= max -> INT32_MAX, if <= min -> INT32_MIN, else truncated
        __m128i result = truncated;
        result = wah_mm_blendv_epi8(result, _mm_setzero_si128(), _mm_castps_si128(cmp_nan));
        result = wah_mm_blendv_epi8(result, _mm_set1_epi32(INT32_MAX), _mm_castps_si128(cmp_gt));
        result = wah_mm_blendv_epi8(result, _mm_set1_epi32(INT32_MIN), _mm_castps_si128(cmp_lt));
        return result;
    }

    static WAH_ALWAYS_INLINE __m128i wah_i32x4_trunc_sat_f32x4_u_sse41(__m128 a) {
        // Check for NaN
        __m128 cmp_nan = _mm_cmpunord_ps(a, a);

        // Check for negative
        __m128 cmp_lt = _mm_cmplt_ps(a, _mm_setzero_ps());

        // Check for overflow (>= UINT32_MAX)
        __m128 cmp_gt = _mm_cmpge_ps(a, _mm_set1_ps((float)UINT32_MAX));

        // Truncate
        __m128i truncated = _mm_cvttps_epi32(a);

        // Select: if NaN or < 0 -> 0, if > max -> UINT32_MAX, else truncated
        __m128i result = truncated;
        __m128i invalid = _mm_or_si128(_mm_castps_si128(cmp_nan), _mm_castps_si128(cmp_lt));
        result = wah_mm_blendv_epi8(result, _mm_setzero_si128(), invalid);
        result = wah_mm_blendv_epi8(result, _mm_set1_epi32(UINT32_MAX), _mm_castps_si128(cmp_gt));
        return result;
    }
)

WAH_IF_SSE42(
    // SSE4.2 64-bit comparison wrappers (pcmpgtq)
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_lt_s_sse42(__m128i a, __m128i b) {
        return wah_mm_cmpgt_epi64(b, a);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_gt_s_sse42(__m128i a, __m128i b) {
        return wah_mm_cmpgt_epi64(a, b);
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_le_s_sse42(__m128i a, __m128i b) {
        return wah_v128_not_sse2(wah_mm_cmpgt_epi64(a, b)); // a <= b  is  NOT(a > b)
    }
    static WAH_ALWAYS_INLINE __m128i wah_i64x2_ge_s_sse42(__m128i a, __m128i b) {
        return wah_v128_not_sse2(wah_mm_cmpgt_epi64(b, a)); // a >= b  is  NOT(b > a)
    }
)

WAH_IF_AVX2(
    // AVX2 optimized popcount using SSSE3 pshufb lookup table
    static WAH_ALWAYS_INLINE __m128i wah_i8x16_popcnt_avx2(__m128i v) {
        // Split bytes into nibbles, look up popcount for each
        __m128i low_nibbles = _mm_and_si128(v, _mm_set1_epi8(0x0F));
        __m128i high_nibbles = _mm_and_si128(_mm_srli_epi16(v, 4), _mm_set1_epi8(0x0F));

        // Popcount lookup table (for nibbles 0-15)
        __m128i popcnt_table = _mm_set_epi8(4,3,3,2,3,2,2,1,3,2,2,1,2,1,1,0);

        // Look up popcount for each nibble
        __m128i low_popcnt = wah_mm_shuffle_epi8(popcnt_table, low_nibbles);
        __m128i high_popcnt = wah_mm_shuffle_epi8(popcnt_table, high_nibbles);

        // Add the two popcounts together
        return _mm_add_epi8(low_popcnt, high_popcnt);
    }
)

WAH_IF_AVX512(
    // AVX-512VL HIGH conversion helpers (shuffle high 64 bits then convert)
    static WAH_ALWAYS_INLINE __m128 wah_f32x4_convert_i32x4_u_high_avx512(__m128i a) {
        __m128i a_high = _mm_srli_si128(a, 8);
        return wah_mm_cvtepu32_ps(a_high);
    }

    static WAH_ALWAYS_INLINE __m128d wah_f64x2_convert_high_i32x4_u_avx512(__m128i a) {
        __m128i a_high = _mm_srli_si128(a, 8);
        return wah_mm_cvtepu32_pd(a_high);
    }
)

// AVX-512BW+VL unsigned byte comparison (vpcmpub -> k -> vpmovm2b)
// SSE2 lacks unsigned byte comparison, so this is genuinely useful unlike
// signed eq/gt which SSE2 already handles with pcmpeqb/pcmpgtb.
#if defined(__GNUC__)
WAH_IF_AVX512(
    // GCC rejects explicit k-register operands/clobbers for non-AVX-512 targets even when
    // the instruction itself is only reached via runtime dispatch. We therefore keep the
    // AVX-512 opcodes in the asm string but expose only XMM operands to the compiler.
    // This works in practice because the hidden mask register is fully internal to the asm:
    // inputs and outputs stay in vector registers, and generic x86-64 code generation does
    // not otherwise allocate k-registers when AVX-512 is not enabled for the containing TU.
    static WAH_ALWAYS_INLINE __m128i wah_mm_cmplt_epu8_avx512(__m128i a, __m128i b) {
        __m128i result;
        __asm__("vpcmpub $1, %[b], %[a], %%k1\n\t vpmovm2b %%k1, %[result]"
            : [result] "=x" (result) : [a] "x" (a), [b] "x" (b));
        return result;
    }
)
#else
WAH_IF_AVX512(
    static WAH_ALWAYS_INLINE __m128i wah_mm_cmplt_epu8_avx512(__m128i a, __m128i b) {
        return _mm_mask_blend_epi8(_mm_cmplt_epu8_mask(a, b), _mm_setzero_si128(), _mm_set1_epi8(-1));
    }
)
#endif

#endif // defined WAH_X86_64

#ifdef WAH_AARCH64 // NEON helper functions

// NEON lacks same-type reinterpret; provide identity macro for u8.
#define vreinterpretq_u8_u8(x) (x)

// Macro-generated typed wrappers for integer SIMD operations.
// NEON uses typed vectors, but _u8x16 stores everything as uint8x16_t.
// These wrappers reinterpret to the correct type, call the NEON intrinsic, and reinterpret back.

#define WAH_NEON_BIN(neon_fn, lane_t) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_##neon_fn##_##lane_t(uint8x16_t a, uint8x16_t b) { \
        return vreinterpretq_u8_##lane_t( \
            neon_fn##_##lane_t(vreinterpretq_##lane_t##_u8(a), vreinterpretq_##lane_t##_u8(b))); \
    }
#define WAH_NEON_UNA(neon_fn, lane_t) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_##neon_fn##_##lane_t(uint8x16_t a) { \
        return vreinterpretq_u8_##lane_t(neon_fn##_##lane_t(vreinterpretq_##lane_t##_u8(a))); \
    }
#define WAH_NEON_CMP(neon_fn, lane_t, ret_t) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_##neon_fn##_##lane_t(uint8x16_t a, uint8x16_t b) { \
        return vreinterpretq_u8_##ret_t( \
            neon_fn##_##lane_t(vreinterpretq_##lane_t##_u8(a), vreinterpretq_##lane_t##_u8(b))); \
    }

// Signed wrapping NEON ops are expressed via unsigned lanes to keep UBSan quiet.
#define WAH_NEON_WRAP_BIN(neon_fn, s_lane, u_lane) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_##neon_fn##_##s_lane(uint8x16_t a, uint8x16_t b) { \
        return vreinterpretq_u8_##u_lane( \
            neon_fn##_##u_lane(vreinterpretq_##u_lane##_u8(a), vreinterpretq_##u_lane##_u8(b))); \
    }
#define WAH_NEON_WRAP_NEG(s_lane, u_lane) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_vnegq_##s_lane(uint8x16_t a) { \
        return vreinterpretq_u8_##u_lane(vsubq_##u_lane(vdupq_n_##u_lane(0), vreinterpretq_##u_lane##_u8(a))); \
    }
#if WAH_UBSAN_ACTIVE
#define WAH_NEON_WRAP_ABS(s_lane, u_lane, uvec_t) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_vabsq_##s_lane(uint8x16_t a) { \
        uvec_t mask = vcltq_##s_lane(vreinterpretq_##s_lane##_u8(a), vdupq_n_##s_lane(0)); \
        return vreinterpretq_u8_##u_lane(vsubq_##u_lane(veorq_##u_lane(vreinterpretq_##u_lane##_u8(a), mask), mask)); \
    }
#else
#define WAH_NEON_WRAP_ABS(s_lane, u_lane, uvec_t) \
    static WAH_ALWAYS_INLINE uint8x16_t wah_vabsq_##s_lane(uint8x16_t a) { \
        return vreinterpretq_u8_##s_lane(vabsq_##s_lane(vreinterpretq_##s_lane##_u8(a))); \
    }
#endif
WAH_NEON_WRAP_BIN(vaddq,  s8,  u8) WAH_NEON_WRAP_BIN(vsubq,  s8,  u8)
WAH_NEON_WRAP_BIN(vaddq, s16, u16) WAH_NEON_WRAP_BIN(vsubq, s16, u16) WAH_NEON_WRAP_BIN(vmulq, s16, u16)
WAH_NEON_WRAP_BIN(vaddq, s32, u32) WAH_NEON_WRAP_BIN(vsubq, s32, u32) WAH_NEON_WRAP_BIN(vmulq, s32, u32)
WAH_NEON_WRAP_BIN(vaddq, s64, u64) WAH_NEON_WRAP_BIN(vsubq, s64, u64)
WAH_NEON_WRAP_NEG( s8,  u8) WAH_NEON_WRAP_NEG(s16, u16)
WAH_NEON_WRAP_NEG(s32, u32) WAH_NEON_WRAP_NEG(s64, u64)
WAH_NEON_WRAP_ABS( s8,  u8, uint8x16_t) WAH_NEON_WRAP_ABS(s16, u16, uint16x8_t)
WAH_NEON_WRAP_ABS(s32, u32, uint32x4_t) WAH_NEON_WRAP_ABS(s64, u64, uint64x2_t)
#undef WAH_NEON_WRAP_BIN
#undef WAH_NEON_WRAP_NEG
#undef WAH_NEON_WRAP_ABS

// --- Arithmetic ---
WAH_NEON_BIN(vqaddq,  s8) WAH_NEON_BIN(vqsubq,  s8)
WAH_NEON_BIN(vaddq,   u8) WAH_NEON_BIN(vsubq,   u8) WAH_NEON_BIN(vqaddq,  u8) WAH_NEON_BIN(vqsubq,  u8)
WAH_NEON_BIN(vqaddq, s16) WAH_NEON_BIN(vqsubq, s16)
WAH_NEON_BIN(vaddq,  u16) WAH_NEON_BIN(vsubq,  u16) WAH_NEON_BIN(vqaddq, u16) WAH_NEON_BIN(vqsubq, u16)
WAH_NEON_BIN(vaddq,  u32) WAH_NEON_BIN(vsubq,  u32)                           WAH_NEON_BIN(vmulq,  u32)
WAH_NEON_BIN(vaddq,  u64) WAH_NEON_BIN(vsubq,  u64)

// --- Compare (returns unsigned result type, not the input type) ---
WAH_NEON_CMP(vceqq,  s8,  u8) WAH_NEON_CMP(vcgtq,  s8,  u8) WAH_NEON_CMP(vcltq,  s8,  u8)
WAH_NEON_CMP(vceqq,  u8,  u8) WAH_NEON_CMP(vcgtq,  u8,  u8) WAH_NEON_CMP(vcltq,  u8,  u8)
WAH_NEON_CMP(vceqq, s16, u16) WAH_NEON_CMP(vcgtq, s16, u16) WAH_NEON_CMP(vcltq, s16, u16)
WAH_NEON_CMP(vceqq, u16, u16) WAH_NEON_CMP(vcgtq, u16, u16) WAH_NEON_CMP(vcltq, u16, u16)
WAH_NEON_CMP(vceqq, s32, u32) WAH_NEON_CMP(vcgtq, s32, u32) WAH_NEON_CMP(vcltq, s32, u32)
WAH_NEON_CMP(vceqq, u32, u32) WAH_NEON_CMP(vcgtq, u32, u32) WAH_NEON_CMP(vcltq, u32, u32)
WAH_NEON_CMP(vceqq, s64, u64) WAH_NEON_CMP(vcgtq, s64, u64) WAH_NEON_CMP(vcltq, s64, u64)

// --- Min/max ---
WAH_NEON_BIN(vminq,  s8) WAH_NEON_BIN(vmaxq,  s8)
WAH_NEON_BIN(vminq,  u8) WAH_NEON_BIN(vmaxq,  u8)
WAH_NEON_BIN(vminq, s16) WAH_NEON_BIN(vmaxq, s16)
WAH_NEON_BIN(vminq, u16) WAH_NEON_BIN(vmaxq, u16)
WAH_NEON_BIN(vminq, s32) WAH_NEON_BIN(vmaxq, s32)
WAH_NEON_BIN(vminq, u32) WAH_NEON_BIN(vmaxq, u32)

// --- Float/double arithmetic ---
WAH_NEON_BIN(vaddq,  f32)      WAH_NEON_BIN(vaddq,  f64)
WAH_NEON_BIN(vsubq,  f32)      WAH_NEON_BIN(vsubq,  f64)
WAH_NEON_BIN(vmulq,  f32)      WAH_NEON_BIN(vmulq,  f64)
WAH_NEON_BIN(vdivq,  f32)      WAH_NEON_BIN(vdivq,  f64)
WAH_NEON_CMP(vceqq,  f32, u32) WAH_NEON_CMP(vceqq,  f64, u64)
WAH_NEON_CMP(vcltq,  f32, u32) WAH_NEON_CMP(vcltq,  f64, u64)
WAH_NEON_CMP(vcleq,  f32, u32) WAH_NEON_CMP(vcleq,  f64, u64)
WAH_NEON_CMP(vcgtq,  f32, u32) WAH_NEON_CMP(vcgtq,  f64, u64)
WAH_NEON_CMP(vcgeq,  f32, u32) WAH_NEON_CMP(vcgeq,  f64, u64)
WAH_NEON_BIN(vminq,  f32)      WAH_NEON_BIN(vminq,  f64)
WAH_NEON_BIN(vmaxq,  f32)      WAH_NEON_BIN(vmaxq,  f64)
WAH_NEON_UNA(vabsq,  f32)      WAH_NEON_UNA(vabsq,  f64)
WAH_NEON_UNA(vnegq,  f32)      WAH_NEON_UNA(vnegq,  f64)
WAH_NEON_UNA(vsqrtq, f32)      WAH_NEON_UNA(vsqrtq, f64)
WAH_NEON_UNA(vrndpq, f32)      WAH_NEON_UNA(vrndpq, f64)
WAH_NEON_UNA(vrndmq, f32)      WAH_NEON_UNA(vrndmq, f64)
WAH_NEON_UNA(vrndq,  f32)      WAH_NEON_UNA(vrndq,  f64)
WAH_NEON_UNA(vrndnq, f32)      WAH_NEON_UNA(vrndnq, f64)

#undef WAH_NEON_BIN
#undef WAH_NEON_CMP
#undef WAH_NEON_UNA

// --- Special-case helpers ---

// Bitmask: extract sign bits into packed integer
static WAH_ALWAYS_INLINE int32_t wah_bitmask_i8x16_neon(uint8x16_t a) {
    static const uint8_t lut[16] = {1,2,4,8,16,32,64,128, 1,2,4,8,16,32,64,128};
    uint8x16_t mask = vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(a), 7));
    uint8x16_t weighted = vandq_u8(mask, vld1q_u8(lut));
    uint64x2_t acc = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(weighted)));
    return (int32_t)(vgetq_lane_u64(acc, 0) | (vgetq_lane_u64(acc, 1) << 8));
}
static WAH_ALWAYS_INLINE int32_t wah_bitmask_i16x8_neon(uint8x16_t a) {
    int16x8_t v = vreinterpretq_s16_u8(a);
    uint16x8_t bits = vcltzq_s16(v);
    static const uint16_t lut[8] = {1,2,4,8,16,32,64,128};
    uint16x8_t weights = vld1q_u16(lut);
    uint16x8_t weighted = vandq_u16(bits, weights);
    uint32x4_t pairs = vpaddlq_u16(weighted);
    uint64x2_t quads = vpaddlq_u32(pairs);
    return (int32_t)(vgetq_lane_u64(quads, 0) | vgetq_lane_u64(quads, 1));
}
static WAH_ALWAYS_INLINE int32_t wah_bitmask_i32x4_neon(uint8x16_t a) {
    int32x4_t v = vreinterpretq_s32_u8(a);
    uint32x4_t bits = vcltzq_s32(v);
    static const uint32_t lut[4] = {1,2,4,8};
    uint32x4_t weights = vld1q_u32(lut);
    uint32x4_t weighted = vandq_u32(bits, weights);
    uint64x2_t pairs = vpaddlq_u32(weighted);
    return (int32_t)(vgetq_lane_u64(pairs, 0) | vgetq_lane_u64(pairs, 1));
}
static WAH_ALWAYS_INLINE int32_t wah_bitmask_i64x2_neon(uint8x16_t a) {
    int64x2_t v = vreinterpretq_s64_u8(a);
    uint64x2_t bits = vcltzq_s64(v);
    return (int32_t)((vgetq_lane_u64(bits, 0) & 1) | (vgetq_lane_u64(bits, 1) & 2));
}

// All-true: check all lanes nonzero
static WAH_ALWAYS_INLINE int32_t wah_all_true_i8x16_neon(uint8x16_t a) {
    return (int32_t)(vminvq_u8(a) != 0);
}
static WAH_ALWAYS_INLINE int32_t wah_all_true_i16x8_neon(uint8x16_t a) {
    return (int32_t)(vminvq_u16(vreinterpretq_u16_u8(a)) != 0);
}
static WAH_ALWAYS_INLINE int32_t wah_all_true_i32x4_neon(uint8x16_t a) {
    return (int32_t)(vminvq_u32(vreinterpretq_u32_u8(a)) != 0);
}
static WAH_ALWAYS_INLINE int32_t wah_all_true_i64x2_neon(uint8x16_t a) {
    uint64x2_t v = vreinterpretq_u64_u8(a);
    return (int32_t)(vgetq_lane_u64(v, 0) != 0 && vgetq_lane_u64(v, 1) != 0);
}

// Any-true
static WAH_ALWAYS_INLINE int32_t wah_any_true_neon(uint8x16_t a) {
    return (int32_t)(vmaxvq_u8(a) != 0);
}

// ANDNOT (a & ~b)
static WAH_ALWAYS_INLINE uint8x16_t wah_v128_andnot_neon(uint8x16_t a, uint8x16_t b) {
    return vandq_u8(a, vmvnq_u8(b));
}

// i8x16 shifts (NEON handles byte shifts natively)
static WAH_ALWAYS_INLINE uint8x16_t wah_i8x16_shl_neon(uint8x16_t a, int32_t count) {
    return vshlq_u8(a, vdupq_n_s8((int8_t)(count & 7)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i8x16_shr_s_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_s8(vshlq_s8(vreinterpretq_s8_u8(a), vdupq_n_s8((int8_t)(-(count & 7)))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i8x16_shr_u_neon(uint8x16_t a, int32_t count) {
    return vshlq_u8(a, vdupq_n_s8((int8_t)(-(count & 7))));
}

// i16x8 shifts
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_shl_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u16(vshlq_u16(vreinterpretq_u16_u8(a), vdupq_n_s16((int16_t)(count & 15))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_shr_s_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_s16(vshlq_s16(vreinterpretq_s16_u8(a), vdupq_n_s16((int16_t)(-(count & 15)))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_shr_u_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u16(vshlq_u16(vreinterpretq_u16_u8(a), vdupq_n_s16((int16_t)(-(count & 15)))));
}

// i32x4 shifts
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_shl_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u32(vshlq_u32(vreinterpretq_u32_u8(a), vdupq_n_s32(count & 31)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_shr_s_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_s32(vshlq_s32(vreinterpretq_s32_u8(a), vdupq_n_s32(-(count & 31))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_shr_u_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u32(vshlq_u32(vreinterpretq_u32_u8(a), vdupq_n_s32(-(count & 31))));
}

// i64x2 shifts
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_shl_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u64(vshlq_u64(vreinterpretq_u64_u8(a), vdupq_n_s64(count & 63)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_shr_u_neon(uint8x16_t a, int32_t count) {
    return vreinterpretq_u8_u64(vshlq_u64(vreinterpretq_u64_u8(a), vdupq_n_s64(-(count & 63))));
}

// Narrow operations
static WAH_ALWAYS_INLINE uint8x16_t wah_i8x16_narrow_i16x8_s_neon(uint8x16_t a, uint8x16_t b) {
    int16x8_t va = vreinterpretq_s16_u8(a), vb = vreinterpretq_s16_u8(b);
    return vreinterpretq_u8_s8(vcombine_s8(vqmovn_s16(va), vqmovn_s16(vb)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i8x16_narrow_i16x8_u_neon(uint8x16_t a, uint8x16_t b) {
    int16x8_t va = vreinterpretq_s16_u8(a), vb = vreinterpretq_s16_u8(b);
    return vcombine_u8(vqmovun_s16(va), vqmovun_s16(vb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_narrow_i32x4_s_neon(uint8x16_t a, uint8x16_t b) {
    int32x4_t va = vreinterpretq_s32_u8(a), vb = vreinterpretq_s32_u8(b);
    return vreinterpretq_u8_s16(vcombine_s16(vqmovn_s32(va), vqmovn_s32(vb)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_narrow_i32x4_u_neon(uint8x16_t a, uint8x16_t b) {
    int32x4_t va = vreinterpretq_s32_u8(a), vb = vreinterpretq_s32_u8(b);
    return vreinterpretq_u8_u16(vcombine_u16(vqmovun_s32(va), vqmovun_s32(vb)));
}

// Extend operations
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_extend_low_i8x16_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s16(vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_extend_high_i8x16_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s16(vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_extend_low_i8x16_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u16(vmovl_u8(vget_low_u8(a)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i16x8_extend_high_i8x16_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u16(vmovl_u8(vget_high_u8(a)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_extend_low_i16x8_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s32(vmovl_s16(vget_low_s16(vreinterpretq_s16_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_extend_high_i16x8_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s32(vmovl_s16(vget_high_s16(vreinterpretq_s16_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_extend_low_i16x8_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u32(vmovl_u16(vget_low_u16(vreinterpretq_u16_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_extend_high_i16x8_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u32(vmovl_u16(vget_high_u16(vreinterpretq_u16_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_extend_low_i32x4_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s64(vmovl_s32(vget_low_s32(vreinterpretq_s32_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_extend_high_i32x4_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_s64(vmovl_s32(vget_high_s32(vreinterpretq_s32_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_extend_low_i32x4_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u64(vmovl_u32(vget_low_u32(vreinterpretq_u32_u8(a))));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_i64x2_extend_high_i32x4_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_u64(vmovl_u32(vget_high_u32(vreinterpretq_u32_u8(a))));
}

// Extmul operations
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_low_s8_neon(uint8x16_t a, uint8x16_t b) {
    int16x8_t ea = vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(a)));
    int16x8_t eb = vmovl_s8(vget_low_s8(vreinterpretq_s8_u8(b)));
    return vreinterpretq_u8_s16(vmulq_s16(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_high_s8_neon(uint8x16_t a, uint8x16_t b) {
    int16x8_t ea = vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(a)));
    int16x8_t eb = vmovl_s8(vget_high_s8(vreinterpretq_s8_u8(b)));
    return vreinterpretq_u8_s16(vmulq_s16(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_low_u8_neon(uint8x16_t a, uint8x16_t b) {
    uint16x8_t ea = vmovl_u8(vget_low_u8(a));
    uint16x8_t eb = vmovl_u8(vget_low_u8(b));
    return vreinterpretq_u8_u16(vmulq_u16(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_high_u8_neon(uint8x16_t a, uint8x16_t b) {
    uint16x8_t ea = vmovl_u8(vget_high_u8(a));
    uint16x8_t eb = vmovl_u8(vget_high_u8(b));
    return vreinterpretq_u8_u16(vmulq_u16(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_low_s16_neon(uint8x16_t a, uint8x16_t b) {
    int32x4_t ea = vmovl_s16(vget_low_s16(vreinterpretq_s16_u8(a)));
    int32x4_t eb = vmovl_s16(vget_low_s16(vreinterpretq_s16_u8(b)));
    return vreinterpretq_u8_s32(vmulq_s32(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_high_s16_neon(uint8x16_t a, uint8x16_t b) {
    int32x4_t ea = vmovl_s16(vget_high_s16(vreinterpretq_s16_u8(a)));
    int32x4_t eb = vmovl_s16(vget_high_s16(vreinterpretq_s16_u8(b)));
    return vreinterpretq_u8_s32(vmulq_s32(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_low_u16_neon(uint8x16_t a, uint8x16_t b) {
    uint32x4_t ea = vmovl_u16(vget_low_u16(vreinterpretq_u16_u8(a)));
    uint32x4_t eb = vmovl_u16(vget_low_u16(vreinterpretq_u16_u8(b)));
    return vreinterpretq_u8_u32(vmulq_u32(ea, eb));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extmul_high_u16_neon(uint8x16_t a, uint8x16_t b) {
    uint32x4_t ea = vmovl_u16(vget_high_u16(vreinterpretq_u16_u8(a)));
    uint32x4_t eb = vmovl_u16(vget_high_u16(vreinterpretq_u16_u8(b)));
    return vreinterpretq_u8_u32(vmulq_u32(ea, eb));
}
// Extadd pairwise
static WAH_ALWAYS_INLINE uint8x16_t wah_extadd_pairwise_s8_neon(uint8x16_t a) {
    return vreinterpretq_u8_s16(vpaddlq_s8(vreinterpretq_s8_u8(a)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extadd_pairwise_u8_neon(uint8x16_t a) {
    return vreinterpretq_u8_u16(vpaddlq_u8(a));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extadd_pairwise_s16_neon(uint8x16_t a) {
    return vreinterpretq_u8_s32(vpaddlq_s16(vreinterpretq_s16_u8(a)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_extadd_pairwise_u16_neon(uint8x16_t a) {
    return vreinterpretq_u8_u32(vpaddlq_u16(vreinterpretq_u16_u8(a)));
}

// Average unsigned
static WAH_ALWAYS_INLINE uint8x16_t wah_avgr_u16_neon(uint8x16_t a, uint8x16_t b) {
    return vreinterpretq_u8_u16(vrhaddq_u16(vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(b)));
}

// i32x4_trunc_sat_f32x4_u (no direct NEON intrinsic)
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_trunc_sat_f32x4_u_neon(uint8x16_t v) {
    float32x4_t fv = vreinterpretq_f32_u8(v);
    float32x4_t clamped = vbslq_f32(vcgeq_f32(fv, vdupq_n_f32(0)), fv, vdupq_n_f32(0));
    uint32x4_t overflow = vcgtq_f32(clamped, vdupq_n_f32(4294967296.0f));
    uint32x4_t result = vcvtq_u32_f32(clamped);
    result = vbslq_u32(overflow, vdupq_n_u32(0xFFFFFFFF), result);
    return vreinterpretq_u8_u32(result);
}

// Wasm f32x4.min/max: canonical NaN if either is NaN, -0/+0 handling via OR/AND
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_min_neon(uint8x16_t va, uint8x16_t vb) {
    float32x4_t a = vreinterpretq_f32_u8(va), b = vreinterpretq_f32_u8(vb);
    uint32x4_t not_nan = vandq_u32(vceqq_f32(a, a), vceqq_f32(b, b));
    float32x4_t result = vreinterpretq_f32_u32(vorrq_u32(
        vreinterpretq_u32_f32(vminq_f32(a, b)), vreinterpretq_u32_f32(vminq_f32(b, a))));
    return vreinterpretq_u8_f32(vbslq_f32(not_nan, result, vdupq_n_f32(WAH_CANONICAL_NAN32.f)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_max_neon(uint8x16_t va, uint8x16_t vb) {
    float32x4_t a = vreinterpretq_f32_u8(va), b = vreinterpretq_f32_u8(vb);
    uint32x4_t not_nan = vandq_u32(vceqq_f32(a, a), vceqq_f32(b, b));
    float32x4_t result = vreinterpretq_f32_u32(vandq_u32(
        vreinterpretq_u32_f32(vmaxq_f32(a, b)), vreinterpretq_u32_f32(vmaxq_f32(b, a))));
    return vreinterpretq_u8_f32(vbslq_f32(not_nan, result, vdupq_n_f32(WAH_CANONICAL_NAN32.f)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f64x2_min_neon(uint8x16_t va, uint8x16_t vb) {
    float64x2_t a = vreinterpretq_f64_u8(va), b = vreinterpretq_f64_u8(vb);
    uint64x2_t not_nan = vandq_u64(vceqq_f64(a, a), vceqq_f64(b, b));
    float64x2_t result = vreinterpretq_f64_u64(vorrq_u64(
        vreinterpretq_u64_f64(vminq_f64(a, b)), vreinterpretq_u64_f64(vminq_f64(b, a))));
    return vreinterpretq_u8_f64(vbslq_f64(not_nan, result, vdupq_n_f64(WAH_CANONICAL_NAN64.f)));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f64x2_max_neon(uint8x16_t va, uint8x16_t vb) {
    float64x2_t a = vreinterpretq_f64_u8(va), b = vreinterpretq_f64_u8(vb);
    uint64x2_t not_nan = vandq_u64(vceqq_f64(a, a), vceqq_f64(b, b));
    float64x2_t result = vreinterpretq_f64_u64(vandq_u64(
        vreinterpretq_u64_f64(vmaxq_f64(a, b)), vreinterpretq_u64_f64(vmaxq_f64(b, a))));
    return vreinterpretq_u8_f64(vbslq_f64(not_nan, result, vdupq_n_f64(WAH_CANONICAL_NAN64.f)));
}

// pmin(a, b) = b < a ? b : a;  pmax(a, b) = a < b ? b : a
// NaN comparisons are false, so NaN operands pass through as `a`.
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_pmin_neon(uint8x16_t va, uint8x16_t vb) {
    float32x4_t a = vreinterpretq_f32_u8(va), b = vreinterpretq_f32_u8(vb);
    return vreinterpretq_u8_f32(vbslq_f32(vcltq_f32(b, a), b, a));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_pmax_neon(uint8x16_t va, uint8x16_t vb) {
    float32x4_t a = vreinterpretq_f32_u8(va), b = vreinterpretq_f32_u8(vb);
    return vreinterpretq_u8_f32(vbslq_f32(vcltq_f32(a, b), b, a));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f64x2_pmin_neon(uint8x16_t va, uint8x16_t vb) {
    float64x2_t a = vreinterpretq_f64_u8(va), b = vreinterpretq_f64_u8(vb);
    return vreinterpretq_u8_f64(vbslq_f64(vcltq_f64(b, a), b, a));
}
static WAH_ALWAYS_INLINE uint8x16_t wah_f64x2_pmax_neon(uint8x16_t va, uint8x16_t vb) {
    float64x2_t a = vreinterpretq_f64_u8(va), b = vreinterpretq_f64_u8(vb);
    return vreinterpretq_u8_f64(vbslq_f64(vcltq_f64(a, b), b, a));
}

// Float convert i32x4 signed
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_convert_i32x4_s_neon(uint8x16_t a) {
    return vreinterpretq_u8_f32(vcvtq_f32_s32(vreinterpretq_s32_u8(a)));
}

// Float convert i32x4 unsigned
static WAH_ALWAYS_INLINE uint8x16_t wah_f32x4_convert_i32x4_u_neon(uint8x16_t a) {
    return vreinterpretq_u8_f32(vcvtq_f32_u32(vreinterpretq_u32_u8(a)));
}

// I32X4_TRUNC_SAT_F32X4_S
static WAH_ALWAYS_INLINE uint8x16_t wah_i32x4_trunc_sat_f32x4_s_neon(uint8x16_t v) {
    return vreinterpretq_u8_s32(vcvtq_s32_f32(vreinterpretq_f32_u8(v)));
}

#endif // defined WAH_AARCH64

////////////////////////////////////////////////////////////////////////////////
// Memory management ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static bool wah_budget_check(const wah_exec_context_t *ctx, uint64_t additional) {
    if (ctx->max_memory_bytes == UINT64_MAX) return true;
    if (ctx->memory_bytes_committed > ctx->max_memory_bytes) return false;
    return additional <= ctx->max_memory_bytes - ctx->memory_bytes_committed;
}

static inline wah_error_t wah_table_byte_size(uint64_t elements, uint64_t *out_bytes) {
#if WAH_HAS_BUILTIN(__builtin_mul_overflow)
    uint64_t bytes;
    if (__builtin_mul_overflow(elements, (uint64_t)sizeof(wah_value_t), &bytes)) {
        return WAH_ERROR_TOO_LARGE;
    }
#else
    if (elements > UINT64_MAX / sizeof(wah_value_t)) return WAH_ERROR_TOO_LARGE;
    uint64_t bytes = elements * sizeof(wah_value_t);
#endif
    if (bytes > SIZE_MAX) return WAH_ERROR_TOO_LARGE;
    *out_bytes = bytes;
    return WAH_OK;
}

static void wah_budget_charge(wah_exec_context_t *ctx, uint64_t bytes) {
    ctx->memory_bytes_committed += bytes;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static void wah_budget_release(wah_exec_context_t *ctx, uint64_t bytes) {
    WAH_ASSERT(ctx->memory_bytes_committed >= bytes);
    ctx->memory_bytes_committed -= bytes;
}
#endif

////////////////////////////////////////////////////////////////////////////////
// Subtyping ///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static inline wah_error_t wah_repr_set_init(wah_repr_set_t *set, uint32_t repr_count, const wah_alloc_t *alloc) {
    *set = (wah_repr_set_t){0};
    set->word_count = (repr_count + 63) / 64;
    if (set->word_count == 0) return WAH_OK;
    wah_error_t err = wah_malloc(alloc, set->word_count, sizeof(set->bits[0]), (void **)&set->bits);
    if (err != WAH_OK) return err;
    memset(set->bits, 0, (size_t)set->word_count * sizeof(set->bits[0]));
    return WAH_OK;
}

static inline wah_error_t wah_repr_set_resize(wah_repr_set_t *set, uint32_t repr_count, const wah_alloc_t *alloc) {
    uint32_t new_word_count = (repr_count + 63) / 64;
    if (new_word_count == set->word_count) return WAH_OK;
    if (new_word_count == 0) {
        wah_free(alloc, set->bits);
        set->bits = NULL;
        set->word_count = 0;
        return WAH_OK;
    }
    uint64_t *new_bits = set->bits;
    wah_error_t err = wah_realloc(alloc, new_word_count, sizeof(set->bits[0]), (void **)&new_bits);
    if (err != WAH_OK) return err;
    if (new_word_count > set->word_count) {
        memset(new_bits + set->word_count, 0, (size_t)(new_word_count - set->word_count) * sizeof(new_bits[0]));
    }
    set->bits = new_bits;
    set->word_count = new_word_count;
    return WAH_OK;
}

static inline void wah_repr_set_add(wah_repr_set_t *set, wah_repr_t repr_id) {
    if (!set || repr_id < 0) return;
    uint32_t word = (uint32_t)repr_id / 64;
    if (word >= set->word_count || !set->bits) return;
    set->bits[word] |= UINT64_C(1) << ((uint32_t)repr_id & 63);
}

static inline bool wah_repr_set_contains(const wah_repr_set_t *set, wah_repr_t repr_id) {
    if (!set || repr_id < 0) return false;
    uint32_t word = (uint32_t)repr_id / 64;
    if (word >= set->word_count || !set->bits) return false;
    return (set->bits[word] & (UINT64_C(1) << ((uint32_t)repr_id & 63))) != 0;
}

static inline bool wah_type_accepts_repr(const wah_module_t *module, uint32_t typeidx, wah_repr_t repr_id) {
    if (!module->type_cast_sets || typeidx >= module->type_count) return false;
    return wah_repr_set_contains(&module->type_cast_sets[typeidx], repr_id);
}

// Helper function to validate if an actual type matches an expected type, considering WAH_TYPE_BOT
static inline wah_comp_type_kind_t wah_type_def_kind(const wah_module_t *m, wah_type_t t) {
    if (!m || !m->type_defs || t < 0 || WAH_TYIDX(t) >= m->type_count) return (wah_comp_type_kind_t)0;
    return m->type_defs[WAH_TYIDX(t)].kind;
}

static inline bool wah_type_is_subtype(wah_type_t sub, wah_type_t sup, const wah_module_t *module) {
    if (sub == sup || sub == WAH_TYPE_BOT) return true;
    if (WAH_TYPE_IS_NULLABLE(sub) && !WAH_TYPE_IS_NULLABLE(sup)) return false;
    sub = WAH_TYPE_AS_NON_NULL(sub);
    sup = WAH_TYPE_AS_NON_NULL(sup);
    if (sub == sup) return true;

    // concrete sub -> concrete sup: check canonical equivalence then walk supertype chain
    if (sub >= 0 && sup >= 0) {
        if (!module || !module->type_defs) return false;
        uint32_t sub_idx = WAH_TYIDX(sub);
        uint32_t sup_idx = WAH_TYIDX(sup);
        if (sub_idx >= module->type_count || sup_idx >= module->type_count) return false;
        if (module->canonical_map) {
            uint32_t csub = module->canonical_map[sub_idx];
            uint32_t csup = module->canonical_map[sup_idx];
            if (csub == csup) return true;
        }
        uint32_t t = sub_idx;
        while (t != WAH_NO_SUPERTYPE) {
            uint32_t st = module->type_defs[t].supertype;
            if (st == WAH_NO_SUPERTYPE) break;
            if (st == sup_idx) return true;
            if (module->canonical_map && module->canonical_map[st] == module->canonical_map[sup_idx])
                return true;
            t = st;
        }
        return false;
    }

    // concrete sub -> abstract sup
    if (sub >= 0) {
        wah_comp_type_kind_t kind = wah_type_def_kind(module, sub);
        switch (sup) {
            case WAH_TYPE_ANY:    return kind == WAH_COMP_STRUCT || kind == WAH_COMP_ARRAY;
            case WAH_TYPE_EQ:     return kind == WAH_COMP_STRUCT || kind == WAH_COMP_ARRAY;
            case WAH_TYPE_STRUCT: return kind == WAH_COMP_STRUCT;
            case WAH_TYPE_ARRAY:  return kind == WAH_COMP_ARRAY;
            case WAH_TYPE_FUNC:   return kind == WAH_COMP_FUNC;
            default: return false;
        }
    }

    // abstract sub -> concrete sup (bottom types)
    if (sup >= 0) {
        wah_comp_type_kind_t kind = wah_type_def_kind(module, sup);
        switch (sub) {
            case WAH_TYPE_NONE: return kind == WAH_COMP_STRUCT || kind == WAH_COMP_ARRAY;
            case WAH_TYPE_NOFUNC: return kind == WAH_COMP_FUNC;
            default: return false;
        }
    }

    // abstract sub -> abstract sup
    if (!WAH_TYPE_IS_REF(sub) || !WAH_TYPE_IS_REF(sup)) return false;

    switch (sup) {
        case WAH_TYPE_ANY:
            return sub != WAH_TYPE_EXTERN && sub != WAH_TYPE_EXN && sub != WAH_TYPE_NOEXTERN && sub != WAH_TYPE_NOEXN
                && sub != WAH_TYPE_FUNC && sub != WAH_TYPE_NOFUNC;
        case WAH_TYPE_EQ:
            return sub == WAH_TYPE_I31 || sub == WAH_TYPE_STRUCT || sub == WAH_TYPE_ARRAY || sub == WAH_TYPE_NONE;
        case WAH_TYPE_STRUCT:
        case WAH_TYPE_ARRAY:
        case WAH_TYPE_I31:
            return sub == WAH_TYPE_NONE;
        case WAH_TYPE_FUNC:
            return sub == WAH_TYPE_NOFUNC;
        case WAH_TYPE_EXTERN:
            return sub == WAH_TYPE_NOEXTERN;
        case WAH_TYPE_EXN:
            return sub == WAH_TYPE_NOEXN;
        default:
            return false;
    }
}

static inline wah_type_t wah_type_hierarchy_top(wah_type_t t, const wah_module_t *module) {
    t = WAH_TYPE_AS_NON_NULL(t);
    if (t >= 0) {
        switch (wah_type_def_kind(module, t)) {
            case WAH_COMP_STRUCT: case WAH_COMP_ARRAY: return WAH_TYPE_ANY;
            case WAH_COMP_FUNC: return WAH_TYPE_FUNC;
            default: return WAH_TYPE_BOT;
        }
    }
    switch (t) {
        case WAH_TYPE_ANY: case WAH_TYPE_EQ: case WAH_TYPE_I31:
        case WAH_TYPE_STRUCT: case WAH_TYPE_ARRAY: case WAH_TYPE_NONE:
            return WAH_TYPE_ANY;
        case WAH_TYPE_FUNC: case WAH_TYPE_NOFUNC:
            return WAH_TYPE_FUNC;
        case WAH_TYPE_EXTERN: case WAH_TYPE_NOEXTERN:
            return WAH_TYPE_EXTERN;
        case WAH_TYPE_EXN: case WAH_TYPE_NOEXN:
            return WAH_TYPE_EXN;
        default:
            return WAH_TYPE_BOT;
    }
}

static wah_type_t wah_canonicalize_type_ref(wah_type_t t, uint32_t rec_start, uint32_t rec_size, const uint32_t *canonical_map, uint32_t type_count) {
    if (t < 0) return t;
    uint32_t idx = WAH_TYIDX(t);
    if (idx >= rec_start && idx < rec_start + rec_size) return t;
    if (idx >= type_count) return t;
    return WAH_TYPE_FROM_IDX(canonical_map[idx], WAH_TYPE_IS_NULLABLE(t));
}

// Compare two canonicalized type refs during structural equivalence.
// Returns: 1 = equal, 0 = not equal, -1 = need further check (both external and same)
static inline bool wah_canon_ref_eq(wah_type_t ta, uint32_t rga_s, uint32_t rga_n,
                                    wah_type_t tb, uint32_t rgb_s, uint32_t rgb_n) {
    if (WAH_TYPE_IS_NULLABLE(ta) != WAH_TYPE_IS_NULLABLE(tb)) return false;
    uint32_t ia = WAH_TYIDX(ta), ib = WAH_TYIDX(tb);
    bool a_in = (ta >= 0 && ia >= rga_s && ia < rga_s + rga_n);
    bool b_in = (tb >= 0 && ib >= rgb_s && ib < rgb_s + rgb_n);
    if (a_in && b_in) return (ia - rga_s) == (ib - rgb_s);
    if (a_in != b_in) return false;
    return ta == tb;
}

static bool wah_types_structurally_equal(const wah_module_t *module, uint32_t a, uint32_t b, const uint32_t *canonical_map) {
    const wah_type_def_t *td_a = &module->type_defs[a];
    const wah_type_def_t *td_b = &module->type_defs[b];
    if (td_a->kind != td_b->kind) return false;
    if (td_a->is_final != td_b->is_final) return false;

    uint32_t rg_a_start = td_a->rec_group_start, rg_a_size = td_a->rec_group_size;
    uint32_t rg_b_start = td_b->rec_group_start, rg_b_size = td_b->rec_group_size;

    // Compare supertypes (canonicalized)
    if (td_a->supertype == WAH_NO_SUPERTYPE) {
        if (td_b->supertype != WAH_NO_SUPERTYPE) return false;
    } else {
        if (td_b->supertype == WAH_NO_SUPERTYPE) return false;
        wah_type_t sa = wah_canonicalize_type_ref(WAH_TYPE_FROM_IDX(td_a->supertype, 0), rg_a_start, rg_a_size, canonical_map, module->type_count);
        wah_type_t sb = wah_canonicalize_type_ref(WAH_TYPE_FROM_IDX(td_b->supertype, 0), rg_b_start, rg_b_size, canonical_map, module->type_count);
        if (!wah_canon_ref_eq(sa, rg_a_start, rg_a_size, sb, rg_b_start, rg_b_size)) return false;
    }

    if (td_a->kind == WAH_COMP_FUNC) {
        const wah_func_type_t *ft_a = &module->types[a];
        const wah_func_type_t *ft_b = &module->types[b];
        if (ft_a->param_count != ft_b->param_count) return false;
        if (ft_a->result_count != ft_b->result_count) return false;
        for (uint32_t j = 0; j < ft_a->param_count; ++j) {
            wah_type_t ta = wah_canonicalize_type_ref(ft_a->param_types[j], rg_a_start, rg_a_size, canonical_map, module->type_count);
            wah_type_t tb = wah_canonicalize_type_ref(ft_b->param_types[j], rg_b_start, rg_b_size, canonical_map, module->type_count);
            if (!wah_canon_ref_eq(ta, rg_a_start, rg_a_size, tb, rg_b_start, rg_b_size)) return false;
        }
        for (uint32_t j = 0; j < ft_a->result_count; ++j) {
            wah_type_t ta = wah_canonicalize_type_ref(ft_a->result_types[j], rg_a_start, rg_a_size, canonical_map, module->type_count);
            wah_type_t tb = wah_canonicalize_type_ref(ft_b->result_types[j], rg_b_start, rg_b_size, canonical_map, module->type_count);
            if (!wah_canon_ref_eq(ta, rg_a_start, rg_a_size, tb, rg_b_start, rg_b_size)) return false;
        }
    } else {
        if (td_a->field_count != td_b->field_count) return false;
        for (uint32_t j = 0; j < td_a->field_count; ++j) {
            wah_type_t ta = wah_canonicalize_type_ref(td_a->field_types[j], rg_a_start, rg_a_size, canonical_map, module->type_count);
            wah_type_t tb = wah_canonicalize_type_ref(td_b->field_types[j], rg_b_start, rg_b_size, canonical_map, module->type_count);
            if (!wah_canon_ref_eq(ta, rg_a_start, rg_a_size, tb, rg_b_start, rg_b_size)) return false;
            if (td_a->field_mutables[j] != td_b->field_mutables[j]) return false;
        }
    }
    return true;
}

#define WAH_MAX_TYPE_RECURSION_DEPTH 256

static bool wah_cross_module_rec_group_eq(const wah_module_t *ma, uint32_t rga_s, uint32_t rga_n,
                                          const wah_module_t *mb, uint32_t rgb_s, uint32_t rgb_n,
                                          uint32_t depth);

static bool wah_cross_module_ref_in_recgroup(const wah_module_t *ma, wah_type_t ta, uint32_t rga_s, uint32_t rga_n,
                                             const wah_module_t *mb, wah_type_t tb, uint32_t rgb_s, uint32_t rgb_n,
                                             uint32_t depth) {
    if (ta == tb && ta < 0) return true;
    if (ta < 0 || tb < 0) return ta == tb;
    if (WAH_TYPE_IS_NULLABLE(ta) != WAH_TYPE_IS_NULLABLE(tb)) return false;
    uint32_t ua = WAH_TYIDX(ta), ub = WAH_TYIDX(tb);
    uint32_t ca = ma->canonical_map ? ma->canonical_map[ua] : ua;
    uint32_t cb = mb->canonical_map ? mb->canonical_map[ub] : ub;
    bool a_in = (ca >= rga_s && ca < rga_s + rga_n);
    bool b_in = (cb >= rgb_s && cb < rgb_s + rgb_n);
    if (a_in && b_in) return (ca - rga_s) == (cb - rgb_s);
    if (a_in || b_in) return false;
    if (ma == mb && ca == cb) return true;
    if (depth >= WAH_MAX_TYPE_RECURSION_DEPTH) return false;
    const wah_type_def_t *da = &ma->type_defs[ca];
    const wah_type_def_t *db = &mb->type_defs[cb];
    if (da->rec_group_size != db->rec_group_size) return false;
    if (ca - da->rec_group_start != cb - db->rec_group_start) return false;
    return wah_cross_module_rec_group_eq(ma, da->rec_group_start, da->rec_group_size,
                                         mb, db->rec_group_start, db->rec_group_size, depth + 1);
}

static bool wah_cross_module_type_eq_in_recgroup(const wah_module_t *ma, uint32_t ia, uint32_t rga_s, uint32_t rga_n,
                                                 const wah_module_t *mb, uint32_t ib, uint32_t rgb_s, uint32_t rgb_n,
                                                 uint32_t depth) {
    const wah_type_def_t *da = &ma->type_defs[ia];
    const wah_type_def_t *db = &mb->type_defs[ib];
    if (da->kind != db->kind) return false;
    if (da->is_final != db->is_final) return false;

    // Compare supertypes
    if (da->supertype == WAH_NO_SUPERTYPE) {
        if (db->supertype != WAH_NO_SUPERTYPE) return false;
    } else {
        if (db->supertype == WAH_NO_SUPERTYPE) return false;
        if (!wah_cross_module_ref_in_recgroup(ma, WAH_TYPE_FROM_IDX(da->supertype, 0), rga_s, rga_n,
                                              mb, WAH_TYPE_FROM_IDX(db->supertype, 0), rgb_s, rgb_n, depth))
            return false;
    }

    if (da->kind == WAH_COMP_FUNC) {
        const wah_func_type_t *fa = &ma->types[ia];
        const wah_func_type_t *fb = &mb->types[ib];
        if (fa->param_count != fb->param_count || fa->result_count != fb->result_count) return false;
        for (uint32_t j = 0; j < fa->param_count; ++j) {
            if (!wah_cross_module_ref_in_recgroup(ma, fa->param_types[j], rga_s, rga_n,
                                                  mb, fb->param_types[j], rgb_s, rgb_n, depth)) return false;
        }
        for (uint32_t j = 0; j < fa->result_count; ++j) {
            if (!wah_cross_module_ref_in_recgroup(ma, fa->result_types[j], rga_s, rga_n,
                                                  mb, fb->result_types[j], rgb_s, rgb_n, depth)) return false;
        }
    } else {
        if (da->field_count != db->field_count) return false;
        for (uint32_t j = 0; j < da->field_count; ++j) {
            if (!wah_cross_module_ref_in_recgroup(ma, da->field_types[j], rga_s, rga_n,
                                                  mb, db->field_types[j], rgb_s, rgb_n, depth)) return false;
            if (da->field_mutables[j] != db->field_mutables[j]) return false;
        }
    }
    return true;
}

static bool wah_cross_module_rec_group_eq(const wah_module_t *ma, uint32_t rga_s, uint32_t rga_n,
                                          const wah_module_t *mb, uint32_t rgb_s, uint32_t rgb_n,
                                          uint32_t depth) {
    if (rga_n != rgb_n) return false;
    if (ma == mb && rga_s == rgb_s) return true;
    for (uint32_t k = 0; k < rga_n; ++k) {
        if (!wah_cross_module_type_eq_in_recgroup(ma, rga_s + k, rga_s, rga_n,
                                                  mb, rgb_s + k, rgb_s, rgb_n, depth))
            return false;
    }
    return true;
}

static bool wah_cross_module_type_ref_eq(const wah_module_t *ma, wah_type_t ta,
                                         const wah_module_t *mb, wah_type_t tb) {
    if (ta == tb && ta < 0) return true;
    if (ta < 0 || tb < 0) return ta == tb;
    if (WAH_TYPE_IS_NULLABLE(ta) != WAH_TYPE_IS_NULLABLE(tb)) return false;

    uint32_t ua = WAH_TYIDX(ta), ub = WAH_TYIDX(tb);
    uint32_t ca = ma->canonical_map ? ma->canonical_map[ua] : ua;
    uint32_t cb = mb->canonical_map ? mb->canonical_map[ub] : ub;

    if (ma == mb && ca == cb) return true;

    const wah_type_def_t *da = &ma->type_defs[ca];
    const wah_type_def_t *db = &mb->type_defs[cb];

    uint32_t rga_s = da->rec_group_start, rga_n = da->rec_group_size;
    uint32_t rgb_s = db->rec_group_start, rgb_n = db->rec_group_size;

    if (rga_n != rgb_n) return false;
    if (ca - rga_s != cb - rgb_s) return false;

    return wah_cross_module_rec_group_eq(ma, rga_s, rga_n, mb, rgb_s, rgb_n, 0);
}

static bool wah_cross_module_subtype(const wah_module_t *sub_m, wah_type_t sub_t,
                                     const wah_module_t *sup_m, wah_type_t sup_t) {
    if (sub_t == sup_t && sub_m == sup_m) return true;
    if (WAH_TYPE_IS_NULLABLE(sub_t) && !WAH_TYPE_IS_NULLABLE(sup_t)) return false;
    if (wah_cross_module_type_ref_eq(sub_m, sub_t, sup_m, sup_t)) return true;
    if (sub_t < 0 || sup_t < 0) return wah_type_is_subtype(sub_t, sup_t, sub_m);
    // Strip nullability for structural comparison (non-null <: nullable is valid)
    wah_type_t sub_nn = WAH_TYPE_AS_NON_NULL(sub_t);
    wah_type_t sup_nn = WAH_TYPE_AS_NON_NULL(sup_t);
    if (sub_nn != sub_t || sup_nn != sup_t) {
        if (wah_cross_module_type_ref_eq(sub_m, sub_nn, sup_m, sup_nn)) return true;
    }
    // Walk supertype chain of sub_t
    uint32_t t = WAH_TYIDX(sub_t);
    while (t != WAH_NO_SUPERTYPE) {
        if (sub_m->type_defs[t].supertype == WAH_NO_SUPERTYPE) break;
        t = sub_m->type_defs[t].supertype;
        if (wah_cross_module_type_ref_eq(sub_m, WAH_TYPE_FROM_IDX(t, 0), sup_m, sup_nn)) return true;
    }
    return false;
}

static inline uint32_t wah_type_check_cache_slot(const wah_module_t *sub_m, wah_type_t sub_t,
                                                 const wah_module_t *sup_m, wah_type_t sup_t) {
    uintptr_t h = (uintptr_t)sub_m >> 4;
    h ^= ((uintptr_t)sup_m >> 9) * UINT64_C(0x9e3779b97f4a7c15);
    h ^= (uintptr_t)(uint32_t)sub_t * UINT64_C(0xbf58476d1ce4e5b9);
    h ^= (uintptr_t)(uint32_t)sup_t * UINT64_C(0x94d049bb133111eb);
    return (uint32_t)(h % WAH_TYPE_CHECK_CACHE_SIZE);
}

static bool wah_cross_module_subtype_cached(wah_exec_context_t *ctx,
                                            const wah_module_t *sub_m, wah_type_t sub_t,
                                            const wah_module_t *sup_m, wah_type_t sup_t) {
    if (!ctx) return wah_cross_module_subtype(sub_m, sub_t, sup_m, sup_t);
    uint32_t slot = wah_type_check_cache_slot(sub_m, sub_t, sup_m, sup_t);
    wah_type_check_cache_entry_t *entry = &ctx->type_check_cache[slot];
    if (entry->valid && entry->sub_module == sub_m && entry->sup_module == sup_m &&
        entry->sub_type == sub_t && entry->sup_type == sup_t) {
        return entry->is_subtype;
    }
    bool is_subtype = wah_cross_module_subtype(sub_m, sub_t, sup_m, sup_t);
    *entry = (wah_type_check_cache_entry_t){
        .sub_module = sub_m, .sup_module = sup_m,
        .sub_type = sub_t, .sup_type = sup_t,
        .is_subtype = is_subtype, .valid = true,
    };
    return is_subtype;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_error_t wah_field_type_layout(wah_type_t ft, uint32_t *out_size, wah_repr_t *out_repr) {
    *out_repr = WAH_REPR_NONE;
    switch (ft) {
        case WAH_TYPE_PACKED_I8:  *out_size = 1; return WAH_OK;
        case WAH_TYPE_PACKED_I16: *out_size = 2; return WAH_OK;
        case WAH_TYPE_I32:
        case WAH_TYPE_F32:        *out_size = 4; return WAH_OK;
        case WAH_TYPE_I64:
        case WAH_TYPE_F64:        *out_size = 8; return WAH_OK;
        case WAH_TYPE_V128:       *out_size = 16; return WAH_OK;
        default:
            if (WAH_TYPE_IS_REF(ft)) {
                *out_size = sizeof(void *);
                *out_repr = WAH_REPR_REF;
                return WAH_OK;
            }
            return WAH_ERROR_VALIDATION_FAILED;
    }
}

typedef struct {
    uint32_t *offsets;
    uint32_t count;
    uint32_t cap;
} wah_layout_slot_list_t;

static inline uint32_t wah_layout_slot_index(uint32_t size) {
    switch (size) {
        case 1: return 0;
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: WAH_ASSERT(0 && "invalid layout slot size"); return 0;
    }
}

static wah_error_t wah_layout_slot_push(wah_layout_slot_list_t slots[4], uint32_t size, uint32_t offset, const wah_alloc_t *alloc) {
    WAH_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);
    WAH_ASSERT((offset & (size - 1)) == 0);
    wah_layout_slot_list_t *list = &slots[wah_layout_slot_index(size)];
    if (list->count == list->cap) {
        if (list->cap > UINT32_MAX / 2) return WAH_ERROR_TOO_LARGE;
        uint32_t new_cap = list->cap ? list->cap * 2 : 8;
        if (new_cap < list->count + 1) return WAH_ERROR_TOO_LARGE;
        void *new_offsets = list->offsets;
        WAH_CHECK(wah_realloc(alloc, new_cap, sizeof(list->offsets[0]), &new_offsets));
        list->offsets = (uint32_t *)new_offsets;
        list->cap = new_cap;
    }
    uint32_t i = list->count;
    while (i > 0 && list->offsets[i - 1] > offset) {
        list->offsets[i] = list->offsets[i - 1];
        --i;
    }
    list->offsets[i] = offset;
    list->count++;
    return WAH_OK;
}

static bool wah_layout_slot_pop(wah_layout_slot_list_t slots[4], uint32_t size, uint32_t *out_offset) {
    wah_layout_slot_list_t *list = &slots[wah_layout_slot_index(size)];
    if (list->count == 0) return false;
    *out_offset = list->offsets[0];
    memmove(list->offsets, list->offsets + 1, (size_t)(list->count - 1) * sizeof(list->offsets[0]));
    list->count--;
    return true;
}

static wah_error_t wah_layout_split_slot(wah_layout_slot_list_t slots[4], uint32_t offset,
                                         uint32_t from_size, uint32_t used_size, const wah_alloc_t *alloc) {
    while (from_size / 2 >= used_size) {
        from_size /= 2;
        WAH_CHECK(wah_layout_slot_push(slots, from_size, offset + from_size, alloc));
    }
    return WAH_OK;
}

static wah_error_t wah_layout_record_tail_slots(wah_layout_slot_list_t slots[4], uint32_t base,
                                                uint32_t used_size, const wah_alloc_t *alloc) {
    WAH_ASSERT(used_size == 1 || used_size == 2 || used_size == 4 || used_size == 8 || used_size == 16);
    for (uint32_t size = 8; size >= used_size && size > 0; size /= 2) {
        WAH_CHECK(wah_layout_slot_push(slots, size, base + size, alloc));
        if (size == 1) break;
    }
    return WAH_OK;
}

static void wah_layout_free_slots(wah_layout_slot_list_t slots[4], const wah_alloc_t *alloc) {
    for (uint32_t i = 0; i < 4; ++i) wah_free(alloc, slots[i].offsets);
}

static wah_error_t wah_validate_struct_repr_info(const wah_type_def_t *td, const wah_repr_info_t *info, const wah_alloc_t *alloc) {
#ifdef WAH_DEBUG
    uint64_t field_bytes = 0;
    uint8_t *used = NULL;
    WAH_ENSURE(td && info && info->type == WAH_REPR_STRUCT, WAH_ERROR_MISUSE);
    if (info->size > 0 && info->size <= 4096) {
        WAH_MALLOC_ARRAY(used, info->size);
        memset(used, 0, info->size);
    }
    for (uint32_t i = 0; i < td->field_count; ++i) {
        uint32_t field_size;
        wah_repr_t field_repr;
        WAH_CHECK(wah_field_type_layout(td->field_types[i], &field_size, &field_repr));
        WAH_ENSURE(field_size > 0 && field_size <= 16 && (field_size & (field_size - 1)) == 0, WAH_ERROR_VALIDATION_FAILED);
        WAH_ENSURE((info->fields[i].offset & (field_size - 1)) == 0, WAH_ERROR_VALIDATION_FAILED);
        WAH_ENSURE(info->fields[i].offset <= info->size && field_size <= info->size - info->fields[i].offset, WAH_ERROR_VALIDATION_FAILED);
        WAH_ENSURE(info->fields[i].repr_id == field_repr, WAH_ERROR_VALIDATION_FAILED);
        field_bytes += field_size;
        if (used) {
            for (uint32_t j = 0; j < field_size; ++j) {
                WAH_ENSURE(!used[info->fields[i].offset + j], WAH_ERROR_VALIDATION_FAILED);
                used[info->fields[i].offset + j] = 1;
            }
        }
    }
    WAH_ENSURE((uint64_t)info->size >= field_bytes && (uint64_t)info->size - field_bytes <= 15, WAH_ERROR_VALIDATION_FAILED);
    wah_free(alloc, used);
#else
    (void)td; (void)info; (void)alloc;
#endif
    return WAH_OK;
}

static wah_error_t wah_build_struct_repr_info(uint32_t typeidx, const wah_type_def_t *td,
                                              wah_repr_info_t **out_info, const wah_alloc_t *alloc) {
    wah_error_t err = WAH_OK;
    wah_layout_slot_list_t slots[4] = {{0}};
    wah_repr_field_t *fields = NULL;
    wah_repr_info_t *info = NULL;
    uint32_t frontier = 0;
    uint32_t live_end = 0;

    WAH_ENSURE(td && out_info && td->kind == WAH_COMP_STRUCT, WAH_ERROR_MISUSE);
    *out_info = NULL;
    if (td->field_count > 0) WAH_MALLOC_ARRAY_GOTO(fields, td->field_count, cleanup);

    for (uint32_t i = 0; i < td->field_count; ++i) {
        uint32_t field_size;
        wah_repr_t field_repr;
        uint32_t offset = 0;
        WAH_CHECK_GOTO(wah_field_type_layout(td->field_types[i], &field_size, &field_repr), cleanup);
        WAH_ENSURE_GOTO(field_size > 0 && field_size <= 16 && (field_size & (field_size - 1)) == 0,
                        WAH_ERROR_VALIDATION_FAILED, cleanup);

        if (field_size == 16) {
            offset = frontier;
            WAH_ENSURE_GOTO(frontier <= UINT32_MAX - 16, WAH_ERROR_TOO_LARGE, cleanup);
            frontier += 16;
        } else if (!wah_layout_slot_pop(slots, field_size, &offset)) {
            uint32_t slot_size = field_size * 2;
            while (slot_size <= 8 && !wah_layout_slot_pop(slots, slot_size, &offset)) slot_size *= 2;
            if (slot_size <= 8) {
                WAH_CHECK_GOTO(wah_layout_split_slot(slots, offset, slot_size, field_size, alloc), cleanup);
            } else {
                offset = frontier;
                WAH_ENSURE_GOTO(frontier <= UINT32_MAX - 16, WAH_ERROR_TOO_LARGE, cleanup);
                frontier += 16;
                WAH_CHECK_GOTO(wah_layout_record_tail_slots(slots, offset, field_size, alloc), cleanup);
            }
        }

        fields[i] = (wah_repr_field_t){ .offset = offset, .repr_id = field_repr };
        if (offset > UINT32_MAX - field_size) { err = WAH_ERROR_TOO_LARGE; goto cleanup; }
        if (live_end < offset + field_size) live_end = offset + field_size;
    }

    size_t max_fields = (SIZE_MAX - sizeof(wah_repr_info_t)) / sizeof(wah_repr_field_t);
    if (max_fields < UINT32_MAX && (size_t)td->field_count > max_fields) {
        err = WAH_ERROR_TOO_LARGE;
        goto cleanup;
    }
    size_t info_size = sizeof(wah_repr_info_t) + td->field_count * sizeof(wah_repr_field_t);
    WAH_CHECK_GOTO(wah_malloc(alloc, 1, info_size, (void **)&info), cleanup);
    info->type = WAH_REPR_STRUCT;
    info->typeidx = typeidx;
    info->size = live_end;
    info->count = td->field_count;
    if (td->field_count > 0) memcpy(info->fields, fields, td->field_count * sizeof(wah_repr_field_t));
    WAH_CHECK_GOTO(wah_validate_struct_repr_info(td, info, alloc), cleanup);
    *out_info = info;
    info = NULL;

cleanup:
    wah_free(alloc, info);
    wah_free(alloc, fields);
    wah_layout_free_slots(slots, alloc);
    return err;
}

static wah_error_t wah_module_alloc_repr(wah_module_t *module, uint32_t typeidx, const wah_repr_info_t *info, wah_repr_t *out_repr_id) {
    const wah_alloc_t *alloc = &module->alloc;
    WAH_ENSURE(module && info && out_repr_id, WAH_ERROR_MISUSE);
    WAH_ENSURE(typeidx < module->type_count, WAH_ERROR_MISUSE);
    WAH_ENSURE(module->typeidx_to_repr[typeidx] == WAH_REPR_NONE, WAH_ERROR_MISUSE);

    uint32_t new_id = module->repr_count;
    uint32_t new_count = new_id + 1;

    WAH_CHECK(wah_realloc(alloc, new_count, sizeof(wah_repr_info_t *), (void **)&module->repr_infos));

    size_t info_size = sizeof(wah_repr_info_t) + info->count * sizeof(wah_repr_field_t);
    uint8_t *_alloc_bytes;
    WAH_MALLOC_ARRAY(_alloc_bytes, info_size);
    wah_repr_info_t *copy = (wah_repr_info_t *)_alloc_bytes;
    memcpy(copy, info, info_size);
    copy->typeidx = typeidx;

    module->repr_infos[new_id] = copy;
    module->repr_count = new_count;
    module->typeidx_to_repr[typeidx] = (wah_repr_t)new_id;
    *out_repr_id = (wah_repr_t)new_id;
    return WAH_OK;
}
#endif // WAH_FEATURE_GC

static void wah_module_clear_type_metadata(wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    wah_free(alloc, module->canonical_map);
    module->canonical_map = NULL;
    wah_free(alloc, module->typeidx_to_repr);
    module->typeidx_to_repr = NULL;
    if (module->repr_infos) {
        for (uint32_t i = 0; i < module->repr_count; ++i) wah_free(alloc, module->repr_infos[i]);
        wah_free(alloc, module->repr_infos);
    }
    module->repr_infos = NULL;
    module->repr_count = 0;
    if (module->type_cast_sets) {
        for (uint32_t i = 0; i < module->type_count; ++i) wah_free(alloc, module->type_cast_sets[i].bits);
        wah_free(alloc, module->type_cast_sets);
    }
    module->type_cast_sets = NULL;
}

static wah_error_t wah_module_build_type_metadata(wah_module_t *module) {
    wah_error_t err;
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t *canonical_map = NULL;
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    wah_repr_info_t *info = NULL;
#endif

    wah_module_clear_type_metadata(module);
    if (module->type_count == 0) return WAH_OK;

    WAH_MALLOC_ARRAY_GOTO(canonical_map, module->type_count, cleanup);
    for (uint32_t i = 0; i < module->type_count; ++i) canonical_map[i] = i;

    for (uint32_t i = 0; i < module->type_count; ++i) {
        wah_type_def_t *td_i = &module->type_defs[i];
        if (i != td_i->rec_group_start) continue;
        uint32_t rg_size_i = td_i->rec_group_size;
        for (uint32_t j = 0; j < i; ++j) {
            wah_type_def_t *td_j = &module->type_defs[j];
            if (j != td_j->rec_group_start) continue;
            if (td_j->rec_group_size != rg_size_i) continue;
            bool match = true;
            for (uint32_t k = 0; k < rg_size_i && match; ++k) {
                match = wah_types_structurally_equal(module, i + k, j + k, canonical_map);
            }
            if (match) {
                for (uint32_t k = 0; k < rg_size_i; ++k) canonical_map[i + k] = j + k;
                break;
            }
        }
    }

    for (uint32_t i = 0; i < module->type_count; ++i) {
        wah_type_def_t *td = &module->type_defs[i];
        if (td->supertype == WAH_NO_SUPERTYPE) continue;
        wah_type_def_t *super_td = &module->type_defs[td->supertype];

        #define WAH_SUBTYPE_CHECK(sub_t, sup_t) ( \
            (sub_t) == (sup_t) || \
            ((sub_t) >= 0 && (sup_t) >= 0 && canonical_map[WAH_TYIDX(sub_t)] == canonical_map[WAH_TYIDX(sup_t)]) || \
            wah_type_is_subtype((sub_t), (sup_t), module) || \
            ((sub_t) >= 0 && (sup_t) >= 0 && wah_type_is_subtype(WAH_TYPE_FROM_IDX(canonical_map[WAH_TYIDX(sub_t)], WAH_TYPE_IS_NULLABLE(sub_t)), \
                                                                 WAH_TYPE_FROM_IDX(canonical_map[WAH_TYIDX(sup_t)], WAH_TYPE_IS_NULLABLE(sup_t)), module)) \
        )

        if (td->kind == WAH_COMP_FUNC) {
            wah_func_type_t *sub_ft = &module->types[i];
            wah_func_type_t *sup_ft = &module->types[td->supertype];
            WAH_ENSURE_GOTO(sub_ft->param_count == sup_ft->param_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            WAH_ENSURE_GOTO(sub_ft->result_count == sup_ft->result_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            for (uint32_t j = 0; j < sub_ft->param_count; ++j) {
                WAH_ENSURE_GOTO(WAH_SUBTYPE_CHECK(sup_ft->param_types[j], sub_ft->param_types[j]), WAH_ERROR_VALIDATION_FAILED, cleanup);
            }
            for (uint32_t j = 0; j < sub_ft->result_count; ++j) {
                WAH_ENSURE_GOTO(WAH_SUBTYPE_CHECK(sub_ft->result_types[j], sup_ft->result_types[j]), WAH_ERROR_VALIDATION_FAILED, cleanup);
            }
        } else if (td->kind == WAH_COMP_STRUCT) {
            WAH_ENSURE_GOTO(td->field_count >= super_td->field_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            for (uint32_t j = 0; j < super_td->field_count; ++j) {
                if (super_td->field_mutables[j]) {
                    WAH_ENSURE_GOTO(td->field_mutables[j], WAH_ERROR_VALIDATION_FAILED, cleanup);
                    wah_type_t st = super_td->field_types[j], tt = td->field_types[j];
                    WAH_ENSURE_GOTO(st == tt || (st >= 0 && tt >= 0 && canonical_map[WAH_TYIDX(st)] == canonical_map[WAH_TYIDX(tt)]), WAH_ERROR_VALIDATION_FAILED, cleanup);
                } else {
                    WAH_ENSURE_GOTO(!td->field_mutables[j], WAH_ERROR_VALIDATION_FAILED, cleanup);
                    WAH_ENSURE_GOTO(WAH_SUBTYPE_CHECK(td->field_types[j], super_td->field_types[j]), WAH_ERROR_VALIDATION_FAILED, cleanup);
                }
            }
        } else if (td->kind == WAH_COMP_ARRAY) {
            WAH_ENSURE_GOTO(td->field_count == 1 && super_td->field_count == 1, WAH_ERROR_VALIDATION_FAILED, cleanup);
            if (super_td->field_mutables[0]) {
                WAH_ENSURE_GOTO(td->field_mutables[0], WAH_ERROR_VALIDATION_FAILED, cleanup);
                wah_type_t st = super_td->field_types[0], tt = td->field_types[0];
                WAH_ENSURE_GOTO(st == tt || (st >= 0 && tt >= 0 && canonical_map[WAH_TYIDX(st)] == canonical_map[WAH_TYIDX(tt)]), WAH_ERROR_VALIDATION_FAILED, cleanup);
            } else {
                WAH_ENSURE_GOTO(!td->field_mutables[0], WAH_ERROR_VALIDATION_FAILED, cleanup);
                WAH_ENSURE_GOTO(WAH_SUBTYPE_CHECK(td->field_types[0], super_td->field_types[0]), WAH_ERROR_VALIDATION_FAILED, cleanup);
            }
        }
        #undef WAH_SUBTYPE_CHECK
    }

    WAH_MALLOC_ARRAY_GOTO(module->typeidx_to_repr, module->type_count, cleanup);
    for (uint32_t i = 0; i < module->type_count; ++i) module->typeidx_to_repr[i] = WAH_REPR_NONE;

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    for (uint32_t i = 0; i < module->type_count; ++i) {
        wah_type_def_t *td = &module->type_defs[i];
        if (td->kind == WAH_COMP_STRUCT) {
            WAH_CHECK_GOTO(wah_build_struct_repr_info(i, td, &info, alloc), cleanup);
            wah_repr_t repr_id;
            WAH_CHECK_GOTO(wah_module_alloc_repr(module, i, info, &repr_id), cleanup);
            wah_free(alloc, info); info = NULL;
        } else if (td->kind == WAH_COMP_ARRAY) {
            uint32_t elem_size;
            wah_repr_t elem_repr;
            WAH_CHECK_GOTO(wah_field_type_layout(td->field_types[0], &elem_size, &elem_repr), cleanup);
            size_t info_size = sizeof(wah_repr_info_t) + sizeof(wah_repr_field_t);
            WAH_CHECK_GOTO(wah_malloc(alloc, 1, info_size, (void **)&info), cleanup);
            info->type = WAH_REPR_ARRAY;
            info->typeidx = i;
            info->size = elem_size;
            info->count = 1;
            info->fields[0] = (wah_repr_field_t){ .offset = 0, .repr_id = elem_repr };
            wah_repr_t repr_id;
            WAH_CHECK_GOTO(wah_module_alloc_repr(module, i, info, &repr_id), cleanup);
            wah_free(alloc, info); info = NULL;
        }
    }

    WAH_MALLOC_ARRAY_GOTO(module->type_cast_sets, module->type_count, cleanup);
    memset(module->type_cast_sets, 0, (size_t)module->type_count * sizeof(module->type_cast_sets[0]));
    for (uint32_t i = 0; i < module->type_count; ++i) {
        WAH_CHECK_GOTO(wah_repr_set_init(&module->type_cast_sets[i], module->repr_count, alloc), cleanup);
    }
    for (uint32_t i = 0; i < module->type_count; ++i) {
        wah_repr_t repr_id = module->typeidx_to_repr[i];
        if (repr_id == WAH_REPR_NONE) continue;
        uint32_t t = i;
        while (t != WAH_NO_SUPERTYPE) {
            wah_repr_set_add(&module->type_cast_sets[canonical_map[t]], repr_id);
            t = module->type_defs[t].supertype;
        }
    }
    for (uint32_t i = 0; i < module->type_count; ++i) {
        uint32_t canonical = canonical_map[i];
        if (canonical == i) continue;
        if (module->type_cast_sets[i].word_count > 0) {
            memcpy(module->type_cast_sets[i].bits,
                   module->type_cast_sets[canonical].bits,
                   (size_t)module->type_cast_sets[i].word_count * sizeof(module->type_cast_sets[i].bits[0]));
        }
    }
#endif

    module->canonical_map = canonical_map;
    return WAH_OK;

cleanup:
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    wah_free(alloc, info);
#endif
    wah_free(alloc, canonical_map);
    wah_module_clear_type_metadata(module);
    return err;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static void wah_module_rollback_last_type_metadata(wah_module_t *module, uint32_t idx, bool cast_slot_ready) {
    const wah_alloc_t *alloc = &module->alloc;
    if (cast_slot_ready && module->type_cast_sets && idx < module->type_count) {
        wah_free(alloc, module->type_cast_sets[idx].bits);
        module->type_cast_sets[idx] = (wah_repr_set_t){0};
    }
    if (module->typeidx_to_repr && idx < module->type_count && module->typeidx_to_repr[idx] >= 0) {
        uint32_t repr_id = (uint32_t)module->typeidx_to_repr[idx];
        if (repr_id + 1 == module->repr_count && module->repr_infos) {
            wah_free(alloc, module->repr_infos[repr_id]);
            module->repr_infos[repr_id] = NULL;
            module->repr_count--;
        }
        module->typeidx_to_repr[idx] = WAH_REPR_NONE;
    }
}
#endif

static wah_error_t wah_module_add_latest_type_metadata(wah_module_t *module) {
    wah_error_t err;
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t idx;
    void *new_ptr;
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    wah_repr_info_t *info = NULL;
    wah_repr_t repr_id = WAH_REPR_NONE;
    bool cast_slot_ready = false;
#endif

    WAH_ASSERT(module->type_count > 0);
    idx = module->type_count - 1;

    new_ptr = module->typeidx_to_repr;
    WAH_CHECK(wah_realloc(alloc, module->type_count, sizeof(module->typeidx_to_repr[0]), &new_ptr));
    module->typeidx_to_repr = (int32_t *)new_ptr;
    module->typeidx_to_repr[idx] = WAH_REPR_NONE;

    new_ptr = module->canonical_map;
    WAH_CHECK(wah_realloc(alloc, module->type_count, sizeof(module->canonical_map[0]), &new_ptr));
    module->canonical_map = (uint32_t *)new_ptr;
    module->canonical_map[idx] = idx;

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    uint32_t target_repr_count = module->repr_count;
    wah_type_def_t *td = &module->type_defs[idx];

    if (td->kind == WAH_COMP_STRUCT) {
        WAH_CHECK_GOTO(wah_build_struct_repr_info(idx, td, &info, alloc), cleanup);
        target_repr_count++;
    } else if (td->kind == WAH_COMP_ARRAY) {
        uint32_t elem_size;
        wah_repr_t elem_repr;
        WAH_CHECK_GOTO(wah_field_type_layout(td->field_types[0], &elem_size, &elem_repr), cleanup);
        size_t info_size = sizeof(wah_repr_info_t) + sizeof(wah_repr_field_t);
        WAH_CHECK_GOTO(wah_malloc(alloc, 1, info_size, (void **)&info), cleanup);
        info->type = WAH_REPR_ARRAY;
        info->typeidx = idx;
        info->size = elem_size;
        info->count = 1;
        info->fields[0] = (wah_repr_field_t){ .offset = 0, .repr_id = elem_repr };
        target_repr_count++;
    }

    if (info) {
        WAH_CHECK_GOTO(wah_module_alloc_repr(module, idx, info, &repr_id), cleanup);
    }

    new_ptr = module->type_cast_sets;
    WAH_CHECK_GOTO(wah_realloc(alloc, module->type_count, sizeof(module->type_cast_sets[0]), &new_ptr), cleanup);
    module->type_cast_sets = (wah_repr_set_t *)new_ptr;
    module->type_cast_sets[idx] = (wah_repr_set_t){0};
    cast_slot_ready = true;
    for (uint32_t i = 0; i + 1 < module->type_count; ++i) {
        WAH_CHECK_GOTO(wah_repr_set_resize(&module->type_cast_sets[i], target_repr_count, alloc), cleanup);
    }
    WAH_CHECK_GOTO(wah_repr_set_init(&module->type_cast_sets[idx], target_repr_count, alloc), cleanup);

    if (repr_id != WAH_REPR_NONE) wah_repr_set_add(&module->type_cast_sets[idx], repr_id);
#endif

    err = WAH_OK;
    goto done;

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
cleanup:
    wah_module_rollback_last_type_metadata(module, idx, cast_slot_ready);
#endif
done:
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    wah_free(alloc, info);
#endif
    return err;
}

////////////////////////////////////////////////////////////////////////////////
// Parsing utilities ///////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Helper to read a uint8_t from a byte array in little-endian format
static inline uint8_t wah_read_u8_le(const uint8_t *ptr) {
    return ptr[0];
}

// Helper to write a uint8_t to a byte array in little-endian format
static inline void wah_write_u8_le(uint8_t *ptr, uint8_t val) {
    ptr[0] = val;
}

// Little-endian detection for direct memory access optimization.
// On little-endian platforms with unaligned access support (x86/x64, AArch64),
// memcpy-based loads/stores compile to single instructions.
#if defined(WAH_X86_64) || defined(WAH_AARCH64)
#if defined(_MSC_VER) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define WAH_NATIVE_LE
#endif
#endif

// Helper to read a uint16_t from a byte array in little-endian format
static inline uint16_t wah_read_u16_le(const uint8_t *ptr) {
#ifdef WAH_NATIVE_LE
    uint16_t val; memcpy(&val, ptr, sizeof(val)); return val;
#else
    return ((uint16_t)ptr[0] << 0) |
           ((uint16_t)ptr[1] << 8);
#endif
}

// Helper to write a uint16_t to a byte array in little-endian format
static inline void wah_write_u16_le(uint8_t *ptr, uint16_t val) {
#ifdef WAH_NATIVE_LE
    memcpy(ptr, &val, sizeof(val));
#else
    ptr[0] = (uint8_t)(val >> 0);
    ptr[1] = (uint8_t)(val >> 8);
#endif
}

// Helper to read a uint32_t from a byte array in little-endian format
static inline uint32_t wah_read_u32_le(const uint8_t *ptr) {
#ifdef WAH_NATIVE_LE
    uint32_t val; memcpy(&val, ptr, sizeof(val)); return val;
#else
    return ((uint32_t)ptr[0] << 0) |
           ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[3] << 24);
#endif
}

// Helper to read a uint64_t from a byte array in little-endian format
static inline uint64_t wah_read_u64_le(const uint8_t *ptr) {
#ifdef WAH_NATIVE_LE
    uint64_t val; memcpy(&val, ptr, sizeof(val)); return val;
#else
    return ((uint64_t)ptr[0] << 0) |
           ((uint64_t)ptr[1] << 8) |
           ((uint64_t)ptr[2] << 16) |
           ((uint64_t)ptr[3] << 24) |
           ((uint64_t)ptr[4] << 32) |
           ((uint64_t)ptr[5] << 40) |
           ((uint64_t)ptr[6] << 48) |
           ((uint64_t)ptr[7] << 56);
#endif
}

// Helper to write a uint32_t to a byte array in little-endian format
static inline void wah_write_u32_le(uint8_t *ptr, uint32_t val) {
#ifdef WAH_NATIVE_LE
    memcpy(ptr, &val, sizeof(val));
#else
    ptr[0] = (uint8_t)(val >> 0);
    ptr[1] = (uint8_t)(val >> 8);
    ptr[2] = (uint8_t)(val >> 16);
    ptr[3] = (uint8_t)(val >> 24);
#endif
}

// Helper to write a uint64_t to a byte array in little-endian format
static inline void wah_write_u64_le(uint8_t *ptr, uint64_t val) {
#ifdef WAH_NATIVE_LE
    memcpy(ptr, &val, sizeof(val));
#else
    ptr[0] = (uint8_t)(val >> 0);
    ptr[1] = (uint8_t)(val >> 8);
    ptr[2] = (uint8_t)(val >> 16);
    ptr[3] = (uint8_t)(val >> 24);
    ptr[4] = (uint8_t)(val >> 32);
    ptr[5] = (uint8_t)(val >> 40);
    ptr[6] = (uint8_t)(val >> 48);
    ptr[7] = (uint8_t)(val >> 56);
#endif
}

// Helper to read a float from a byte array in little-endian format
static inline float wah_read_f32_le(const uint8_t *ptr) {
    union { uint32_t i; float f; } u = { .i = wah_read_u32_le(ptr) };
    return u.f;
}

// Helper to read a double from a byte array in little-endian format
static inline double wah_read_f64_le(const uint8_t *ptr) {
    union { uint64_t i; double d; } u = { .i = wah_read_u64_le(ptr) };
    return u.d;
}

// Helper to write a float to a byte array in little-endian format
static inline void wah_write_f32_le(uint8_t *ptr, float val) {
    union { uint32_t i; float f; } u = { .f = val };
    wah_write_u32_le(ptr, u.i);
}

// Helper to write a double to a byte array in little-endian format
static inline void wah_write_f64_le(uint8_t *ptr, double val) {
    union { uint64_t i; double f; } u = { .f = val };
    wah_write_u64_le(ptr, u.i);
}

// --- LEB128 Decoding ---
// Helper function to decode an unsigned LEB128 integer
static inline wah_error_t wah_decode_uleb128(const uint8_t **ptr, const uint8_t *end, uint32_t *result) {
    uint64_t val = 0;
    uint32_t shift = 0;
    uint8_t byte;

    for (int i = 0; i < 5; ++i) { // Max 5 bytes for a 32-bit value
        WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
        byte = *(*ptr)++;
        val |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            WAH_ENSURE(val <= UINT32_MAX, WAH_ERROR_TOO_LARGE);
            *result = (uint32_t)val;
            return WAH_OK;
        }
        shift += 7;
    }
    // If we get here, it means the 5th byte had the continuation bit set.
    return WAH_ERROR_TOO_LARGE;
}

// Helper function to decode an unsigned LEB128 integer (64-bit)
static inline wah_error_t wah_decode_uleb128_64(const uint8_t **ptr, const uint8_t *end, uint64_t *result) {
    uint64_t val = 0;
    uint32_t shift = 0;
    uint8_t byte;
    for (int i = 0; i < 10; ++i) { // Max 10 bytes for a 64-bit value
        WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
        byte = *(*ptr)++;
        val |= (uint64_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            if (i == 9 && byte > 1) return WAH_ERROR_TOO_LARGE;
            *result = val;
            return WAH_OK;
        }
        shift += 7;
    }
    return WAH_ERROR_TOO_LARGE;
}

// Helper function to decode a signed LEB128 integer (32-bit)
static inline wah_error_t wah_decode_sleb128_32(const uint8_t **ptr, const uint8_t *end, int32_t *result) {
    uint64_t val = 0;
    uint32_t shift = 0;
    uint8_t byte;

    for (int i = 0; i < 5; ++i) { // Max 5 bytes for a 32-bit value
        WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
        byte = *(*ptr)++;
        val |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            // Sign extend
            if (shift < 64) {
                uint64_t sign_bit = 1ULL << (shift - 1);
                if ((val & sign_bit) != 0) {
                    val |= ~0ULL << shift;
                }
            }
            WAH_ENSURE((int64_t)val >= INT32_MIN && (int64_t)val <= INT32_MAX, WAH_ERROR_TOO_LARGE);
            *result = (int32_t)val;
            return WAH_OK;
        }
    }
    // If we get here, it means the 5th byte had the continuation bit set.
    return WAH_ERROR_TOO_LARGE;
}

// Helper function to decode a signed LEB128 integer (64-bit)
static inline wah_error_t wah_decode_sleb128_64(const uint8_t **ptr, const uint8_t *end, int64_t *result) {
    uint64_t val = 0;
    uint32_t shift = 0;
    uint8_t byte;

    do {
        WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
        WAH_ENSURE(shift < 64, WAH_ERROR_TOO_LARGE);
        byte = *(*ptr)++;
        val |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);

    if (shift == 70) {
        WAH_ENSURE(byte == 0x00 || byte == 0x7F, WAH_ERROR_TOO_LARGE);
    } else if (shift < 64) {
        uint64_t sign_bit = 1ULL << (shift - 1);
        if ((val & sign_bit) != 0) {
            val |= ~0ULL << shift;
        }
    }
    *result = (int64_t)val;
    return WAH_OK;
}

// Helper function to decode WebAssembly 3.0 memarg
// WebAssembly 3.0 memarg format: align:u32 [memidx:u32] offset:u64
// If align is 0..63, memidx is assumed to be 0
// If align is 64..127, memidx is present and align is adjusted by subtracting 64
static inline wah_error_t wah_decode_memarg(const uint8_t **ptr, const uint8_t *end, wah_memarg_t *memarg) {
    WAH_CHECK(wah_decode_uleb128(ptr, end, &memarg->align));
    WAH_ENSURE(memarg->align < 128, WAH_ERROR_MALFORMED);
    memarg->memidx = 0;
    // WebAssembly 3.0: if align >= 64, memidx is present
    if (memarg->align >= 64) {
        WAH_CHECK(wah_decode_uleb128(ptr, end, &memarg->memidx));
        memarg->align -= 64; // adjust align to actual value
    }
    WAH_CHECK(wah_decode_uleb128_64(ptr, end, &memarg->offset));
    return WAH_OK;
}

static wah_error_t wah_decode_opcode(const uint8_t **ptr, const uint8_t *end, uint16_t *opcode_val) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t first_byte = *(*ptr)++;
    uint32_t sub_opcode_val;

    switch (first_byte) {
        case 0xFB:
            WAH_CHECK(wah_decode_uleb128(ptr, end, &sub_opcode_val));
            WAH_ENSURE(sub_opcode_val < WAH_FC - WAH_FB, WAH_ERROR_VALIDATION_FAILED);
            *opcode_val = WAH_FB + (uint16_t)sub_opcode_val;
            break;
        case 0xFC:
            WAH_CHECK(wah_decode_uleb128(ptr, end, &sub_opcode_val));
            WAH_ENSURE(sub_opcode_val < WAH_FD - WAH_FC, WAH_ERROR_VALIDATION_FAILED);
            *opcode_val = WAH_FC + (uint16_t)sub_opcode_val;
            break;
        case 0xFD:
            WAH_CHECK(wah_decode_uleb128(ptr, end, &sub_opcode_val));
            WAH_ENSURE(sub_opcode_val < WAH_FE - WAH_FD, WAH_ERROR_VALIDATION_FAILED);
            *opcode_val = WAH_FD + (uint16_t)sub_opcode_val;
            break;
        default:
            WAH_ENSURE(first_byte < WAH_FB, WAH_ERROR_MALFORMED);
            *opcode_val = (uint16_t)first_byte;
            break;
    }
    return WAH_OK;
}

// Helper function to decode a raw byte representing a value type into a wah_type_t
static inline bool wah_decode_abstract_heap_byte(uint8_t byte, wah_type_t *out_type) {
    switch (byte) {
        case 0x70: *out_type = WAH_TYPE_FUNC;       return true;
        case 0x6F: *out_type = WAH_TYPE_EXTERN;     return true;
        case 0x6E: *out_type = WAH_TYPE_ANY;        return true;
        case 0x6D: *out_type = WAH_TYPE_EQ;         return true;
        case 0x6C: *out_type = WAH_TYPE_I31;        return true;
        case 0x6B: *out_type = WAH_TYPE_STRUCT;     return true;
        case 0x6A: *out_type = WAH_TYPE_ARRAY;      return true;
        case 0x69: *out_type = WAH_TYPE_EXN;        return true;
        case 0x74: *out_type = WAH_TYPE_NOEXN;      return true;
        case 0x73: *out_type = WAH_TYPE_NOFUNC;     return true;
        case 0x72: *out_type = WAH_TYPE_NOEXTERN;   return true;
        case 0x71: *out_type = WAH_TYPE_NONE;       return true;
        default: return false;
    }
}

static wah_error_t wah_decode_heap_type(const uint8_t **ptr, const uint8_t *end,
                                        wah_type_t *out_type) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    if (wah_decode_abstract_heap_byte(**ptr, out_type)) {
        (*ptr)++;
        return WAH_OK;
    }
    int32_t idx;
    WAH_CHECK(wah_decode_sleb128_32(ptr, end, &idx));
    WAH_ENSURE(idx >= 0, WAH_ERROR_MALFORMED);
    WAH_ENSURE((uint32_t)idx <= WAH_MAX_TYPE_INDEX, WAH_ERROR_TOO_LARGE);
    *out_type = WAH_TYPE_FROM_IDX(idx, 0);
    return WAH_OK;
}

static wah_error_t wah_decode_ref_type(const uint8_t **ptr, const uint8_t *end, wah_type_t *out_type) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t byte = *(*ptr)++;
    if (wah_decode_abstract_heap_byte(byte, out_type)) {
        *out_type = WAH_TYPE_AS_NULLABLE(*out_type);
        return WAH_OK;
    }
    if (byte == 0x63) {
        WAH_CHECK(wah_decode_heap_type(ptr, end, out_type));
        *out_type = WAH_TYPE_AS_NULLABLE(*out_type);
        return WAH_OK;
    }
    if (byte == 0x64) {
        WAH_CHECK(wah_decode_heap_type(ptr, end, out_type));
        return WAH_OK;
    }
    return WAH_ERROR_MALFORMED;
}

static wah_error_t wah_decode_val_type(const uint8_t **ptr, const uint8_t *end, wah_type_t *out_type) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t byte = **ptr;
    switch (byte) {
        case 0x7F: *out_type = WAH_TYPE_I32; (*ptr)++; return WAH_OK;
        case 0x7E: *out_type = WAH_TYPE_I64; (*ptr)++; return WAH_OK;
        case 0x7D: *out_type = WAH_TYPE_F32; (*ptr)++; return WAH_OK;
        case 0x7C: *out_type = WAH_TYPE_F64; (*ptr)++; return WAH_OK;
        case 0x7B: *out_type = WAH_TYPE_V128; (*ptr)++; return WAH_OK;
        default: return wah_decode_ref_type(ptr, end, out_type);
    }
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_error_t wah_decode_storage_type(const uint8_t **ptr, const uint8_t *end,
                                            wah_type_t *out_type) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t byte = **ptr;
    switch (byte) {
        case 0x78: *out_type = WAH_TYPE_PACKED_I8; (*ptr)++; return WAH_OK;
        case 0x77: *out_type = WAH_TYPE_PACKED_I16; (*ptr)++; return WAH_OK;
        default: return wah_decode_val_type(ptr, end, out_type);
    }
}
#endif // WAH_FEATURE_GC

// Helper function to decode a ULEB128 count and validate it against remaining section size
static inline wah_error_t wah_decode_and_validate_count(const uint8_t **ptr, const uint8_t *section_end, uint32_t *count, uint32_t min_bytes_per_item) {
    WAH_CHECK(wah_decode_uleb128(ptr, section_end, count));

    // Check for potential integer overflow before multiplication
    if (min_bytes_per_item > 0 && *count > (UINT32_MAX / min_bytes_per_item)) {
        return WAH_ERROR_TOO_LARGE; // Count * min_bytes_per_item would overflow
    }

    // Ensure that the declared count of items does not exceed the remaining section size.
    // This prevents excessively large allocations and potential hangs/OOM.
    // We use min_bytes_per_item to make a more accurate check.
    if ((uint64_t)*count * min_bytes_per_item > (uint64_t)(section_end - *ptr)) {
        return WAH_ERROR_MALFORMED;
    }
    return WAH_OK;
}

// Helper function to validate if a byte sequence is valid UTF-8
static inline bool wah_is_valid_utf8(const char *s, size_t len) {
    const unsigned char *bytes = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned char byte = bytes[i];
        if (byte < 0x80) { // 1-byte sequence (0xxxxxxx)
            i++;
            continue;
        }
        if ((byte & 0xE0) == 0xC0) { // 2-byte sequence (110xxxxx 10xxxxxx)
            if (i + 1 >= len || (bytes[i+1] & 0xC0) != 0x80 || (byte & 0xFE) == 0xC0) return false; // Overlong encoding
            i += 2;
            continue;
        }
        if ((byte & 0xF0) == 0xE0) { // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 >= len || (bytes[i+1] & 0xC0) != 0x80 || (bytes[i+2] & 0xC0) != 0x80 ||
                (byte == 0xE0 && bytes[i+1] < 0xA0) || // Overlong encoding
                (byte == 0xED && bytes[i+1] >= 0xA0)) return false; // Surrogate pair
            i += 3;
            continue;
        }
        if ((byte & 0xF8) == 0xF0) { // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (byte > 0xF4 || i + 3 >= len || (bytes[i+1] & 0xC0) != 0x80 || (bytes[i+2] & 0xC0) != 0x80 || (bytes[i+3] & 0xC0) != 0x80 ||
                (byte == 0xF0 && bytes[i+1] < 0x90) || // Overlong encoding
                (byte == 0xF4 && bytes[i+1] >= 0x90)) return false; // Codepoint > 0x10FFFF
            i += 4;
            continue;
        }
        return false; // Invalid start byte
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Validation and lowering /////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static inline wah_error_t wah_type_stack_push(wah_type_stack_t *stack, wah_type_t type) {
    WAH_ENSURE(stack->sp < WAH_MAX_TYPE_STACK_SIZE, WAH_ERROR_TOO_LARGE);
    stack->data[stack->sp] = type;
    stack->sp++;
    return WAH_OK;
}

static inline wah_error_t wah_type_stack_pop(wah_type_stack_t *stack, wah_type_t *out_type) {
    WAH_ENSURE(stack->sp > 0, WAH_ERROR_VALIDATION_FAILED);
    --stack->sp;
    *out_type = stack->data[stack->sp];
    return WAH_OK;
}

static inline wah_error_t wah_validate_type_match(wah_type_t actual, wah_type_t expected, const wah_module_t *module) {
    if (actual == WAH_TYPE_BOT) return WAH_OK;
    WAH_ENSURE(wah_type_is_subtype(WAH_TYPE_AS_NON_NULL(actual), WAH_TYPE_AS_NON_NULL(expected), module), WAH_ERROR_VALIDATION_FAILED);
    if (!WAH_TYPE_IS_NULLABLE(expected)) {
        WAH_ENSURE(!WAH_TYPE_IS_NULLABLE(actual), WAH_ERROR_VALIDATION_FAILED);
    }
    return WAH_OK;
}

// Stack depth tracking helpers
static inline uint32_t wah_validation_block_base_height(const wah_validation_context_t *vctx) {
    if (vctx->control_sp == 0) return 0;
    return vctx->control_stack[vctx->control_sp - 1].stack_height;
}

static inline wah_error_t wah_validation_push_type(wah_validation_context_t *vctx, wah_type_t type) {
    WAH_CHECK(wah_type_stack_push(&vctx->type_stack, type));
    vctx->current_stack_depth++;
    if (vctx->current_stack_depth > vctx->max_stack_depth) {
        vctx->max_stack_depth = vctx->current_stack_depth;
    }
    return WAH_OK;
}

static inline wah_error_t wah_validation_pop_type(wah_validation_context_t *vctx, wah_type_t *out_type) {
    uint32_t base = wah_validation_block_base_height(vctx);
    if (vctx->current_stack_depth <= base) {
        WAH_ENSURE(vctx->is_unreachable, WAH_ERROR_VALIDATION_FAILED);
        *out_type = WAH_TYPE_BOT;
        return WAH_OK;
    }
    WAH_CHECK(wah_type_stack_pop(&vctx->type_stack, out_type));
    vctx->current_stack_depth--;
    return WAH_OK;
}

static inline wah_error_t wah_validation_pop_and_match_type(wah_validation_context_t *vctx, wah_type_t expected_type) {
    wah_type_t actual_type;
    WAH_CHECK(wah_validation_pop_type(vctx, &actual_type));
    return wah_validate_type_match(actual_type, expected_type, vctx->module);
}

static inline wah_error_t wah_validation_pop_field_value(wah_validation_context_t *vctx, wah_type_t field_type) {
    wah_type_t ut = WAH_TYPE_IS_PACKED(field_type) ? WAH_TYPE_I32 : field_type;
    return wah_validation_pop_and_match_type(vctx, ut);
}

static wah_error_t wah_validation_pop_func_params(wah_validation_context_t *vctx, const wah_func_type_t *ft) {
    for (int32_t j = ft->param_count - 1; j >= 0; --j) {
        WAH_CHECK(wah_validation_pop_and_match_type(vctx, ft->param_types[j]));
    }
    return WAH_OK;
}

static wah_error_t wah_validation_push_func_results(wah_validation_context_t *vctx, const wah_func_type_t *ft) {
    for (uint32_t j = 0; j < ft->result_count; ++j) {
        WAH_CHECK(wah_validation_push_type(vctx, ft->result_types[j]));
    }
    return WAH_OK;
}

static wah_error_t wah_validation_check_return_types(const wah_validation_context_t *vctx, const wah_func_type_t *ft) {
    WAH_ENSURE(ft->result_count == vctx->func_type->result_count, WAH_ERROR_VALIDATION_FAILED);
    for (uint32_t j = 0; j < ft->result_count; ++j) {
        WAH_CHECK(wah_validate_type_match(ft->result_types[j], vctx->func_type->result_types[j], vctx->module));
    }
    return WAH_OK;
}

static inline void wah_validation_mark_unreachable(wah_validation_context_t *vctx) {
    uint32_t base = wah_validation_block_base_height(vctx);
    if (vctx->current_stack_depth > base) {
        vctx->type_stack.sp -= (vctx->current_stack_depth - base);
        vctx->current_stack_depth = base;
    }
    vctx->is_unreachable = true;
}

static wah_error_t wah_validation_decode_block_type(const uint8_t **code_ptr, const uint8_t *code_end,
                                                     wah_validation_context_t *vctx, wah_func_type_t *bt) {
    const wah_alloc_t *alloc = &vctx->module->alloc;
    *bt = (wah_func_type_t){0};
    WAH_ENSURE(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t block_type_peek = **code_ptr;

    if (block_type_peek == 0x40) {
        (*code_ptr)++;
    } else if (block_type_peek >= 0x63 && block_type_peek <= 0x7F) {
        wah_type_t result_type;
        WAH_CHECK(wah_decode_val_type(code_ptr, code_end, &result_type));
        WAH_ENSURE(result_type < 0 || WAH_TYIDX(result_type) < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
        bt->result_count = 1;
        WAH_MALLOC_ARRAY(bt->result_types, 1);
        bt->result_types[0] = result_type;
    } else {
        int32_t block_type_val;
        WAH_CHECK(wah_decode_sleb128_32(code_ptr, code_end, &block_type_val));
        WAH_ENSURE(block_type_val >= 0, WAH_ERROR_MALFORMED);
        uint32_t type_idx = (uint32_t)block_type_val;
        WAH_ENSURE(type_idx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
        const wah_func_type_t* referenced_type = &vctx->module->types[type_idx];

        bt->param_count = referenced_type->param_count;
        if (bt->param_count > 0) {
            WAH_MALLOC_ARRAY(bt->param_types, bt->param_count);
            memcpy(bt->param_types, referenced_type->param_types, sizeof(wah_type_t) * bt->param_count);
        }

        bt->result_count = referenced_type->result_count;
        if (bt->result_count > 0) {
            WAH_MALLOC_ARRAY(bt->result_types, bt->result_count);
            memcpy(bt->result_types, referenced_type->result_types, sizeof(wah_type_t) * bt->result_count);
        }
    }
    return WAH_OK;
}

static wah_error_t wah_analyzed_append_end(wah_analyzed_code_t *ac, const wah_alloc_t *alloc) {
    WAH_ENSURE_CAP(ac->instrs, ac->instr_count + 1);
    ac->instrs[ac->instr_count] = (wah_decoded_instr_t){ .opcode = WAH_OP_END };
    ac->instr_count++;
    return WAH_OK;
}

// Resolve a branch label index to its target result types and stack height.
// label_idx == control_sp targets the implicit function frame (equivalent to return).
static void wah_validation_resolve_br_target(const wah_validation_context_t *vctx, uint32_t label_idx,
                                              uint32_t *out_result_count, const wah_type_t **out_result_types,
                                              uint32_t *out_stack_height) {
    if (label_idx == vctx->control_sp) {
        *out_result_count = vctx->func_type->result_count;
        if (out_result_types) *out_result_types = vctx->func_type->result_types;
        if (out_stack_height) *out_stack_height = 0;
    } else {
        const wah_validation_control_frame_t *frame = &vctx->control_stack[vctx->control_sp - 1 - label_idx];
        if (frame->opcode == WAH_OP_LOOP) {
            *out_result_count = frame->block_type.param_count;
            if (out_result_types) *out_result_types = frame->block_type.param_types;
        } else {
            *out_result_count = frame->block_type.result_count;
            if (out_result_types) *out_result_types = frame->block_type.result_types;
        }
        if (out_stack_height) *out_stack_height = frame->stack_height;
    }
}

static wah_error_t wah_declare_func(wah_module_t *m, uint32_t func_idx) {
    if (!m->declared_funcs) {
        const wah_alloc_t *alloc = &m->alloc;
        uint32_t total = wah_func_index_limit(m);
        if (total == 0) return WAH_OK;
        uint32_t bytes = (total + 7) / 8;
        WAH_MALLOC_ARRAY(m->declared_funcs, bytes);
        memset(m->declared_funcs, 0, bytes);
    }
    m->declared_funcs[func_idx / 8] |= (uint8_t)(1u << (func_idx % 8));
    return WAH_OK;
}

static inline bool wah_is_func_declared(const wah_module_t *m, uint32_t func_idx) {
    if (!m->declared_funcs) return false;
    return (m->declared_funcs[func_idx / 8] & (1u << (func_idx % 8))) != 0;
}

static inline const wah_func_type_t *wah_resolve_func_type(const wah_module_t *m, uint32_t func_idx) {
    if (func_idx < m->import_function_count) {
        return &m->types[m->func_imports[func_idx].type_index];
    }
    return &m->types[m->function_type_indices[func_idx - m->import_function_count]];
}

// Validation helper function that handles a single opcode
static wah_error_t wah_validate_opcode(uint16_t opcode_val, const uint8_t **code_ptr, const uint8_t *code_end,
                                       wah_validation_context_t *vctx, wah_code_body_t* code_body, wah_analyzed_code_t *ac) {
#define WAH_TYPE__(T) T // So that POP(_(T)) and PUSH(_(T)) work
#define POP(T) WAH_CHECK(wah_validation_pop_and_match_type(vctx, WAH_TYPE_##T))
#define POP_INTO(x) WAH_CHECK(wah_validation_pop_type(vctx, x))
#define PUSH(T) WAH_CHECK(wah_validation_push_type(vctx, WAH_TYPE_##T))

#define EMIT_INSTR_EX(op, ...) do { \
    WAH_ENSURE_CAP(ac->instrs, ac->instr_count + 1); \
    wah_decoded_instr_t *_di = &ac->instrs[ac->instr_count++]; \
    *_di = (wah_decoded_instr_t){ .opcode = (op), .flags = vctx->is_unreachable ? WAH_INSTR_FLAG_UNREACHABLE : 0 }; \
    __VA_ARGS__; \
} while (0)

#define EMIT_SIMPLE() EMIT_INSTR_EX(opcode_val, (void)0)

    const wah_alloc_t *alloc = &vctx->module->alloc;

    WAH_ENSURE(wah_opclasses[opcode_val] != WAH_OPCLASS_MISSING, WAH_ERROR_MALFORMED);

    if (vctx->mode == WAH_ANALYZE_CONST_EXPR) {
        switch (opcode_val) {
            case WAH_OP_I32_CONST: case WAH_OP_I64_CONST: case WAH_OP_F32_CONST:
            case WAH_OP_F64_CONST: case WAH_OP_V128_CONST:
            case WAH_OP_I32_ADD: case WAH_OP_I32_SUB: case WAH_OP_I32_MUL:
            case WAH_OP_I64_ADD: case WAH_OP_I64_SUB: case WAH_OP_I64_MUL:
            case WAH_OP_REF_NULL: case WAH_OP_REF_FUNC: case WAH_OP_REF_I31:
            case WAH_OP_GLOBAL_GET:
            case WAH_OP_STRUCT_NEW: case WAH_OP_STRUCT_NEW_DEFAULT:
            case WAH_OP_ARRAY_NEW: case WAH_OP_ARRAY_NEW_DEFAULT:
            case WAH_OP_ARRAY_NEW_FIXED:
            case WAH_OP_ANY_CONVERT_EXTERN: case WAH_OP_EXTERN_CONVERT_ANY:
                break;
            default:
                return WAH_ERROR_VALIDATION_FAILED;
        }
    }

    // Note: WAH_OPCLASS__AB_C should POP(B) first, then POP(A) and PUSH(C)!
    switch (wah_opclasses[opcode_val]) {
        case WAH_OPCLASS__I_I:   POP(I32); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__I_L:   POP(I32); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__I_F:   POP(I32); PUSH(F32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__I_D:   POP(I32); PUSH(F64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__I_V:   POP(I32); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__II_I:  POP(I32); POP(I32); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__IV_V:  POP(V128); POP(I32); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__L_I:   POP(I64); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__L_L:   POP(I64); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__L_F:   POP(I64); PUSH(F32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__L_D:   POP(I64); PUSH(F64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__L_V:   POP(I64); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__LL_I:  POP(I64); POP(I64); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__LL_L:  POP(I64); POP(I64); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__LV_V:  POP(V128); POP(I64); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__F_I:   POP(F32); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__F_L:   POP(F32); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__F_F:   POP(F32); PUSH(F32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__F_V:   POP(F32); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__FF_I:  POP(F32); POP(F32); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__FF_F:  POP(F32); POP(F32); PUSH(F32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__D_I:   POP(F64); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__D_L:   POP(F64); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__D_D:   POP(F64); PUSH(F64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__D_V:   POP(F64); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__DD_I:  POP(F64); POP(F64); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__DD_L:  POP(F64); POP(F64); PUSH(I64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__DD_D:  POP(F64); POP(F64); PUSH(F64); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__V_I:   POP(V128); PUSH(I32); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__V_V:   POP(V128); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__VI_V:  POP(I32); POP(V128); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__VV_V:  POP(V128); POP(V128); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
        case WAH_OPCLASS__VVV_V: POP(V128); POP(V128); POP(V128); PUSH(V128); EMIT_SIMPLE(); return WAH_OK;
    }

    switch (opcode_val) {
#define LOAD_OP(T, max_lg_align) { \
            wah_memarg_t memarg; \
            WAH_CHECK(wah_decode_memarg(code_ptr, code_end, &memarg)); \
            WAH_ENSURE(memarg.align <= max_lg_align, WAH_ERROR_VALIDATION_FAILED); \
            WAH_ENSURE(memarg.memidx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED); \
            wah_type_t addr_type = wah_memory_type(vctx->module, memarg.memidx)->addr_type; \
            if (addr_type == WAH_TYPE_I32) WAH_ENSURE(memarg.offset <= 0xFFFFFFFFU, WAH_ERROR_VALIDATION_FAILED); \
            POP(_(addr_type)); PUSH(T); \
            EMIT_INSTR_EX(opcode_val, _di->imm.memarg = memarg); \
            break; \
        }

#define STORE_OP(T, max_lg_align) { \
            wah_memarg_t memarg; \
            WAH_CHECK(wah_decode_memarg(code_ptr, code_end, &memarg)); \
            WAH_ENSURE(memarg.align <= max_lg_align, WAH_ERROR_VALIDATION_FAILED); \
            WAH_ENSURE(memarg.memidx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED); \
            wah_type_t addr_type = wah_memory_type(vctx->module, memarg.memidx)->addr_type; \
            if (addr_type == WAH_TYPE_I32) WAH_ENSURE(memarg.offset <= 0xFFFFFFFFU, WAH_ERROR_VALIDATION_FAILED); \
            POP(T); POP(_(addr_type)); \
            EMIT_INSTR_EX(opcode_val, _di->imm.memarg = memarg); \
            break; \
        }

#define V128_LANE_OP(max_lg_align, is_load) { \
            wah_memarg_t memarg; \
            WAH_CHECK(wah_decode_memarg(code_ptr, code_end, &memarg)); \
            WAH_ENSURE(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF); \
            uint8_t lane_idx = *(*code_ptr)++; \
            WAH_ENSURE(memarg.align <= max_lg_align, WAH_ERROR_VALIDATION_FAILED); \
            WAH_ENSURE(lane_idx < (16 >> max_lg_align), WAH_ERROR_VALIDATION_FAILED); \
            WAH_ENSURE(memarg.memidx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED); \
            wah_type_t addr_type = wah_memory_type(vctx->module, memarg.memidx)->addr_type; \
            if (addr_type == WAH_TYPE_I32) WAH_ENSURE(memarg.offset <= 0xFFFFFFFFU, WAH_ERROR_VALIDATION_FAILED); \
            POP(V128); POP(_(addr_type)); if (is_load) PUSH(V128); \
            EMIT_INSTR_EX(opcode_val, _di->imm.memarg_lane.memarg = memarg; _di->imm.memarg_lane.lane = lane_idx); \
            break; \
        }

#define EXTRACT_LANE_OP(SCALAR_TYPE, LANE_COUNT) { \
            POP(V128); PUSH(SCALAR_TYPE); \
            WAH_ENSURE(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF); \
            uint8_t lane_idx = *(*code_ptr)++; \
            WAH_ENSURE(lane_idx < LANE_COUNT, WAH_ERROR_VALIDATION_FAILED); \
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = lane_idx); \
            break; \
        }

#define REPLACE_LANE_OP(SCALAR_TYPE, LANE_COUNT) { \
            POP(SCALAR_TYPE); POP(V128); PUSH(V128); \
            WAH_ENSURE(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF); \
            uint8_t lane_idx = *(*code_ptr)++; \
            WAH_ENSURE(lane_idx < LANE_COUNT, WAH_ERROR_VALIDATION_FAILED); \
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = lane_idx); \
            break; \
        }

        case WAH_OP_I32_LOAD: LOAD_OP(I32, 2)
        case WAH_OP_I64_LOAD: LOAD_OP(I64, 3)
        case WAH_OP_F32_LOAD: LOAD_OP(F32, 2)
        case WAH_OP_F64_LOAD: LOAD_OP(F64, 3)
        case WAH_OP_I32_LOAD8_S: case WAH_OP_I32_LOAD8_U: LOAD_OP(I32, 0)
        case WAH_OP_I32_LOAD16_S: case WAH_OP_I32_LOAD16_U: LOAD_OP(I32, 1)
        case WAH_OP_I64_LOAD8_S: case WAH_OP_I64_LOAD8_U: LOAD_OP(I64, 0)
        case WAH_OP_I64_LOAD16_S: case WAH_OP_I64_LOAD16_U: LOAD_OP(I64, 1)
        case WAH_OP_I64_LOAD32_S: case WAH_OP_I64_LOAD32_U: LOAD_OP(I64, 2)
        case WAH_OP_I32_STORE: STORE_OP(I32, 2)
        case WAH_OP_I64_STORE: STORE_OP(I64, 3)
        case WAH_OP_F32_STORE: STORE_OP(F32, 2)
        case WAH_OP_F64_STORE: STORE_OP(F64, 3)
        case WAH_OP_I32_STORE8: STORE_OP(I32, 0)
        case WAH_OP_I32_STORE16: STORE_OP(I32, 1)
        case WAH_OP_I64_STORE8: STORE_OP(I64, 0)
        case WAH_OP_I64_STORE16: STORE_OP(I64, 1)
        case WAH_OP_I64_STORE32: STORE_OP(I64, 2)
        case WAH_OP_V128_LOAD: LOAD_OP(V128, 4)
        case WAH_OP_V128_LOAD8X8_S: case WAH_OP_V128_LOAD8X8_U: LOAD_OP(V128, 3)
        case WAH_OP_V128_LOAD16X4_S: case WAH_OP_V128_LOAD16X4_U: LOAD_OP(V128, 3)
        case WAH_OP_V128_LOAD32X2_S: case WAH_OP_V128_LOAD32X2_U: LOAD_OP(V128, 3)
        case WAH_OP_V128_LOAD8_SPLAT: LOAD_OP(V128, 0)
        case WAH_OP_V128_LOAD16_SPLAT: LOAD_OP(V128, 1)
        case WAH_OP_V128_LOAD32_SPLAT: LOAD_OP(V128, 2)
        case WAH_OP_V128_LOAD64_SPLAT: LOAD_OP(V128, 3)
        case WAH_OP_V128_LOAD32_ZERO: LOAD_OP(V128, 2)
        case WAH_OP_V128_LOAD64_ZERO: LOAD_OP(V128, 3)
        case WAH_OP_V128_STORE: STORE_OP(V128, 4)

        case WAH_OP_V128_LOAD8_LANE: V128_LANE_OP(0, true)
        case WAH_OP_V128_LOAD16_LANE: V128_LANE_OP(1, true)
        case WAH_OP_V128_LOAD32_LANE: V128_LANE_OP(2, true)
        case WAH_OP_V128_LOAD64_LANE: V128_LANE_OP(3, true)

        case WAH_OP_V128_STORE8_LANE: V128_LANE_OP(0, false)
        case WAH_OP_V128_STORE16_LANE: V128_LANE_OP(1, false)
        case WAH_OP_V128_STORE32_LANE: V128_LANE_OP(2, false)
        case WAH_OP_V128_STORE64_LANE: V128_LANE_OP(3, false)

        /* Vector Lane Operations */
        case WAH_OP_I8X16_SHUFFLE: {
            POP(V128); POP(V128); PUSH(V128);
            WAH_ENSURE(code_end - *code_ptr >= 16, WAH_ERROR_UNEXPECTED_EOF);
            for (int i = 0; i < 16; i++) WAH_ENSURE((*code_ptr)[i] < 32, WAH_ERROR_VALIDATION_FAILED);
            EMIT_INSTR_EX(opcode_val, memcpy(_di->imm.v128, *code_ptr, 16));
            *code_ptr += 16;
            break;
        }

        case WAH_OP_I8X16_EXTRACT_LANE_S:
        case WAH_OP_I8X16_EXTRACT_LANE_U: EXTRACT_LANE_OP(I32, 16)
        case WAH_OP_I8X16_REPLACE_LANE: REPLACE_LANE_OP(I32, 16)
        case WAH_OP_I16X8_EXTRACT_LANE_S:
        case WAH_OP_I16X8_EXTRACT_LANE_U: EXTRACT_LANE_OP(I32, 8)
        case WAH_OP_I16X8_REPLACE_LANE: REPLACE_LANE_OP(I32, 8)
        case WAH_OP_I32X4_EXTRACT_LANE: EXTRACT_LANE_OP(I32, 4)
        case WAH_OP_I32X4_REPLACE_LANE: REPLACE_LANE_OP(I32, 4)
        case WAH_OP_I64X2_EXTRACT_LANE: EXTRACT_LANE_OP(I64, 2)
        case WAH_OP_I64X2_REPLACE_LANE: REPLACE_LANE_OP(I64, 2)
        case WAH_OP_F32X4_EXTRACT_LANE: EXTRACT_LANE_OP(F32, 4)
        case WAH_OP_F32X4_REPLACE_LANE: REPLACE_LANE_OP(F32, 4)
        case WAH_OP_F64X2_EXTRACT_LANE: EXTRACT_LANE_OP(F64, 2)
        case WAH_OP_F64X2_REPLACE_LANE: REPLACE_LANE_OP(F64, 2)

#undef LOAD_OP
#undef STORE_OP
#undef V128_LANE_OP
#undef EXTRACT_LANE_OP
#undef REPLACE_LANE_OP

        case WAH_OP_MEMORY_SIZE: {
            uint32_t mem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &mem_idx));
            WAH_ENSURE(mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            PUSH(_(wah_memory_type(vctx->module, mem_idx)->addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = mem_idx);
            break;
        }
        case WAH_OP_MEMORY_GROW: {
            uint32_t mem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &mem_idx));
            WAH_ENSURE(mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            wah_type_t addr_type = wah_memory_type(vctx->module, mem_idx)->addr_type;
            POP(_(addr_type)); PUSH(_(addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = mem_idx);
            break;
        }
        case WAH_OP_MEMORY_FILL: {
            uint32_t mem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &mem_idx));
            WAH_ENSURE(mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            wah_type_t addr_type = wah_memory_type(vctx->module, mem_idx)->addr_type;
            POP(_(addr_type)); POP(I32); POP(_(addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = mem_idx);
            break;
        }
        case WAH_OP_MEMORY_INIT: {
            uint32_t data_idx, mem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &data_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &mem_idx));
            WAH_ENSURE(mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            wah_type_t addr_type = wah_memory_type(vctx->module, mem_idx)->addr_type;
            POP(I32); POP(I32); POP(_(addr_type));
            WAH_ENSURE(data_idx < UINT32_MAX, WAH_ERROR_VALIDATION_FAILED);
            if (data_idx + 1 > vctx->module->min_data_segment_count_required) {
                vctx->module->min_data_segment_count_required = data_idx + 1;
            }
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = data_idx; _di->imm.u32_u32.b = mem_idx);
            break;
        }
        case WAH_OP_DATA_DROP: {
            uint32_t data_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &data_idx));
            WAH_ENSURE(data_idx < UINT32_MAX, WAH_ERROR_VALIDATION_FAILED);
            if (data_idx + 1 > vctx->module->min_data_segment_count_required) {
                vctx->module->min_data_segment_count_required = data_idx + 1;
            }
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = data_idx);
            break;
        }
        case WAH_OP_MEMORY_COPY: {
            uint32_t dest_mem_idx, src_mem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &dest_mem_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &src_mem_idx));
            WAH_ENSURE(dest_mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(src_mem_idx < wah_memory_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            wah_type_t dst_at = wah_memory_type(vctx->module, dest_mem_idx)->addr_type;
            wah_type_t src_at = wah_memory_type(vctx->module, src_mem_idx)->addr_type;
            wah_type_t n_type = (dst_at == WAH_TYPE_I64 && src_at == WAH_TYPE_I64) ? WAH_TYPE_I64 : WAH_TYPE_I32;
            POP(_(n_type)); POP(_(src_at)); POP(_(dst_at));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = dest_mem_idx; _di->imm.u32_u32.b = src_mem_idx);
            break;
        }
        case WAH_OP_CALL: {
            uint32_t func_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &func_idx));
            WAH_ENSURE(func_idx < wah_func_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_func_type_t *called_func_type = wah_resolve_func_type(vctx->module, func_idx);
            WAH_CHECK(wah_validation_pop_func_params(vctx, called_func_type));
            WAH_CHECK(wah_validation_push_func_results(vctx, called_func_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = func_idx);
            break;
        }
        case WAH_OP_CALL_INDIRECT: {
            uint32_t type_idx, table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &type_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(wah_type_is_subtype(wah_table_type(vctx->module, table_idx)->elem_type,
                                           WAH_TYPE_FUNCREF, vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(type_idx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            POP(_(wah_table_type(vctx->module, table_idx)->addr_type));
            const wah_func_type_t *expected_func_type = &vctx->module->types[type_idx];
            WAH_CHECK(wah_validation_pop_func_params(vctx, expected_func_type));
            WAH_CHECK(wah_validation_push_func_results(vctx, expected_func_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = type_idx; _di->imm.u32_u32.b = table_idx);
            break;
        }
        case WAH_OP_CALL_REF: {
            uint32_t type_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &type_idx));
            WAH_ENSURE(type_idx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[type_idx].kind == WAH_COMP_FUNC, WAH_ERROR_VALIDATION_FAILED);
            const wah_func_type_t *expected_func_type = &vctx->module->types[type_idx];
            POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(type_idx, 0))));
            WAH_CHECK(wah_validation_pop_func_params(vctx, expected_func_type));
            WAH_CHECK(wah_validation_push_func_results(vctx, expected_func_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = type_idx);
            break;
        }
        case WAH_OP_RETURN_CALL: {
            uint32_t func_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &func_idx));
            WAH_ENSURE(func_idx < wah_func_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_func_type_t *called_func_type = wah_resolve_func_type(vctx->module, func_idx);
            WAH_CHECK(wah_validation_check_return_types(vctx, called_func_type));
            WAH_CHECK(wah_validation_pop_func_params(vctx, called_func_type));
            wah_validation_mark_unreachable(vctx);
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = func_idx);
            return WAH_OK;
        }
        case WAH_OP_RETURN_CALL_INDIRECT: {
            uint32_t type_idx, table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &type_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(wah_type_is_subtype(wah_table_type(vctx->module, table_idx)->elem_type,
                                           WAH_TYPE_FUNCREF, vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(type_idx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            POP(_(wah_table_type(vctx->module, table_idx)->addr_type));
            const wah_func_type_t *expected_func_type = &vctx->module->types[type_idx];
            WAH_CHECK(wah_validation_check_return_types(vctx, expected_func_type));
            WAH_CHECK(wah_validation_pop_func_params(vctx, expected_func_type));
            wah_validation_mark_unreachable(vctx);
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = type_idx; _di->imm.u32_u32.b = table_idx);
            return WAH_OK;
        }
        case WAH_OP_RETURN_CALL_REF: {
            uint32_t type_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &type_idx));
            WAH_ENSURE(type_idx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[type_idx].kind == WAH_COMP_FUNC, WAH_ERROR_VALIDATION_FAILED);
            const wah_func_type_t *expected_func_type = &vctx->module->types[type_idx];
            POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(type_idx, 0))));
            WAH_CHECK(wah_validation_check_return_types(vctx, expected_func_type));
            WAH_CHECK(wah_validation_pop_func_params(vctx, expected_func_type));
            wah_validation_mark_unreachable(vctx);
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = type_idx);
            return WAH_OK;
        }
        case WAH_OP_LOCAL_GET:
        case WAH_OP_LOCAL_SET:
        case WAH_OP_LOCAL_TEE: {
            uint32_t local_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &local_idx));

            WAH_ENSURE(local_idx < vctx->total_locals, WAH_ERROR_VALIDATION_FAILED);

            wah_type_t expected_type;
            if (local_idx < vctx->func_type->param_count) {
                expected_type = vctx->func_type->param_types[local_idx];
            } else {
                uint32_t li = local_idx - vctx->func_type->param_count;
                expected_type = code_body->local_types[li];
            }

            if (opcode_val == WAH_OP_LOCAL_GET) {
                if (vctx->local_inits && !vctx->local_inits[local_idx] && !vctx->is_unreachable)
                    return WAH_ERROR_VALIDATION_FAILED;
                PUSH(_(expected_type));
            } else if (opcode_val == WAH_OP_LOCAL_SET) {
                POP(_(expected_type));
                if (vctx->local_inits) vctx->local_inits[local_idx] = 1;
            } else { // WAH_OP_LOCAL_TEE
                POP(_(expected_type)); PUSH(_(expected_type));
                if (vctx->local_inits) vctx->local_inits[local_idx] = 1;
            }
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = local_idx);
            break;
        }

        case WAH_OP_GLOBAL_GET: {
            uint32_t global_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &global_idx));
            WAH_ENSURE(global_idx < wah_global_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            if (vctx->mode == WAH_ANALYZE_CONST_EXPR) {
                WAH_ENSURE(global_idx < vctx->max_global_idx, WAH_ERROR_VALIDATION_FAILED);
                WAH_ENSURE(!wah_global_is_mutable(vctx->module, global_idx), WAH_ERROR_VALIDATION_FAILED);
            }
            PUSH(_(wah_global_type(vctx->module, global_idx)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = global_idx);
            break;
        }
        case WAH_OP_GLOBAL_SET: {
            uint32_t global_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &global_idx));
            WAH_ENSURE(global_idx < wah_global_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(wah_global_is_mutable(vctx->module, global_idx), WAH_ERROR_VALIDATION_FAILED);
            POP(_(wah_global_type(vctx->module, global_idx)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = global_idx);
            break;
        }
        case WAH_OP_TABLE_GET: {
            uint32_t table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_table_type_t *tt = wah_table_type(vctx->module, table_idx);
            POP(_(tt->addr_type)); PUSH(_(tt->elem_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = table_idx);
            break;
        }
        case WAH_OP_TABLE_SET: {
            uint32_t table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_table_type_t *tt = wah_table_type(vctx->module, table_idx);
            POP(_(tt->elem_type)); POP(_(tt->addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = table_idx);
            break;
        }
        case WAH_OP_TABLE_SIZE: {
            uint32_t table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            PUSH(_(wah_table_type(vctx->module, table_idx)->addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = table_idx);
            break;
        }
        case WAH_OP_TABLE_GROW: {
            uint32_t table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_table_type_t *tt = wah_table_type(vctx->module, table_idx);
            POP(_(tt->addr_type)); POP(_(tt->elem_type)); PUSH(_(tt->addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = table_idx);
            break;
        }
        case WAH_OP_TABLE_FILL: {
            uint32_t table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_table_type_t *tt = wah_table_type(vctx->module, table_idx);
            POP(_(tt->addr_type)); POP(_(tt->elem_type)); POP(_(tt->addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = table_idx);
            break;
        }
        case WAH_OP_TABLE_COPY: {
            uint32_t dst_table_idx, src_table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &dst_table_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &src_table_idx));
            WAH_ENSURE(dst_table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(src_table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_table_type_t *dst_tt = wah_table_type(vctx->module, dst_table_idx);
            const wah_table_type_t *src_tt = wah_table_type(vctx->module, src_table_idx);
            wah_type_t dst_addr_type = dst_tt->addr_type;
            wah_type_t src_addr_type = src_tt->addr_type;
            WAH_CHECK(wah_validate_type_match(src_tt->elem_type, dst_tt->elem_type, vctx->module));
            wah_type_t size_type = (dst_addr_type == WAH_TYPE_I64 && src_addr_type == WAH_TYPE_I64)
                ? WAH_TYPE_I64 : WAH_TYPE_I32;
            POP(_(size_type)); POP(_(src_addr_type)); POP(_(dst_addr_type));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = dst_table_idx; _di->imm.u32_u32.b = src_table_idx);
            break;
        }
        case WAH_OP_TABLE_INIT: {
            uint32_t elem_idx, table_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &elem_idx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &table_idx));
            WAH_ENSURE(elem_idx < vctx->module->element_segment_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(table_idx < wah_table_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            const wah_element_segment_t *seg = &vctx->module->element_segments[elem_idx];
            const wah_table_type_t *tt = wah_table_type(vctx->module, table_idx);
            WAH_CHECK(wah_validate_type_match(seg->elem_type, tt->elem_type, vctx->module));
            wah_type_t taddr = wah_table_type(vctx->module, table_idx)->addr_type;
            POP(I32); POP(I32); POP(_(taddr));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32_u32.a = elem_idx; _di->imm.u32_u32.b = table_idx);
            break;
        }
        case WAH_OP_ELEM_DROP: {
            uint32_t elem_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &elem_idx));
            WAH_ENSURE(elem_idx < vctx->module->element_segment_count, WAH_ERROR_VALIDATION_FAILED);
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = elem_idx);
            break;
        }

        case WAH_OP_I32_CONST: {
            int32_t val;
            WAH_CHECK(wah_decode_sleb128_32(code_ptr, code_end, &val));
            PUSH(I32);
            EMIT_INSTR_EX(opcode_val, _di->imm.i32 = val);
            break;
        }
        case WAH_OP_I64_CONST: {
            int64_t val;
            WAH_CHECK(wah_decode_sleb128_64(code_ptr, code_end, &val));
            PUSH(I64);
            EMIT_INSTR_EX(opcode_val, _di->imm.i64 = val);
            break;
        }
        case WAH_OP_F32_CONST: {
            WAH_ENSURE(code_end - *code_ptr >= 4, WAH_ERROR_UNEXPECTED_EOF);
            EMIT_INSTR_EX(opcode_val, memcpy(&_di->imm.f32_bits, *code_ptr, 4));
            *code_ptr += 4;
            PUSH(F32); break;
        }
        case WAH_OP_F64_CONST: {
            WAH_ENSURE(code_end - *code_ptr >= 8, WAH_ERROR_UNEXPECTED_EOF);
            EMIT_INSTR_EX(opcode_val, memcpy(&_di->imm.f64_bits, *code_ptr, 8));
            *code_ptr += 8;
            PUSH(F64); break;
        }
        case WAH_OP_V128_CONST: {
            WAH_ENSURE(code_end - *code_ptr >= 16, WAH_ERROR_UNEXPECTED_EOF);
            EMIT_INSTR_EX(opcode_val, memcpy(_di->imm.v128, *code_ptr, 16));
            *code_ptr += 16;
            PUSH(V128); break;
        }

        case WAH_OP_F32_DEMOTE_F64:  POP(F64); PUSH(F32); EMIT_SIMPLE(); break;
        case WAH_OP_F64_PROMOTE_F32: POP(F32); PUSH(F64); EMIT_SIMPLE(); break;

        // Parametric operations
        case WAH_OP_DROP: {
            wah_type_t type; POP_INTO(&type);
            EMIT_SIMPLE();
            return WAH_OK;
        }

        case WAH_OP_SELECT: {
            wah_type_t b_type, a_type; POP(I32); POP_INTO(&b_type); POP_INTO(&a_type);
            WAH_ENSURE(a_type == b_type || a_type == WAH_TYPE_BOT || b_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t resolved = (a_type != WAH_TYPE_BOT) ? a_type : b_type;
            if (resolved != WAH_TYPE_BOT) {
                WAH_ENSURE(resolved >= WAH_TYPE_V128 && resolved <= WAH_TYPE_I32, WAH_ERROR_VALIDATION_FAILED);
            }
            EMIT_SIMPLE();
            return wah_validation_push_type(vctx, resolved);
        }
        case WAH_OP_SELECT_T: {
            uint32_t vec_len;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &vec_len));
            WAH_ENSURE(vec_len == 1, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t sel_type;
            WAH_CHECK(wah_decode_val_type(code_ptr, code_end, &sel_type));
            WAH_ENSURE(sel_type < 0 || WAH_TYIDX(sel_type) < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            POP(I32); POP(_(sel_type)); POP(_(sel_type));
            EMIT_SIMPLE();
            return wah_validation_push_type(vctx, sel_type);
        }

        // Control flow operations
        case WAH_OP_NOP: break; // No opcode emitted
        case WAH_OP_UNREACHABLE: {
            wah_validation_mark_unreachable(vctx);
            EMIT_SIMPLE();
            break;
        }

        case WAH_OP_BLOCK:
        case WAH_OP_LOOP:
        case WAH_OP_IF: {
            wah_error_t err = WAH_OK;
            bool frame_pushed = false;
            wah_validation_control_frame_t* frame = NULL;
            wah_type_t cond_type;
            if (opcode_val == WAH_OP_IF) {
                POP_INTO(&cond_type);
                WAH_CHECK(wah_validate_type_match(cond_type, WAH_TYPE_I32, vctx->module));
            }

            WAH_ENSURE(vctx->control_sp < WAH_MAX_CONTROL_DEPTH, WAH_ERROR_TOO_LARGE);
            frame = &vctx->control_stack[vctx->control_sp++];
            frame_pushed = true;
            frame->opcode = (wah_opcode_t)opcode_val;
            frame->else_found = false;
            frame->is_unreachable = vctx->is_unreachable;
            wah_func_type_t* bt = &frame->block_type;
            WAH_CHECK_GOTO(wah_validation_decode_block_type(code_ptr, code_end, vctx, bt), cleanup_block);

            for (int32_t i = bt->param_count - 1; i >= 0; --i) {
                WAH_CHECK_GOTO(wah_validation_pop_and_match_type(vctx, bt->param_types[i]), cleanup_block);
            }

            frame->stack_height = vctx->current_stack_depth;
            frame->type_stack_sp = vctx->type_stack.sp;

            if (vctx->local_inits) {
                frame->local_init_save_offset = vctx->local_init_stack_used;
                memcpy(vctx->local_init_stack + vctx->local_init_stack_used,
                       vctx->local_inits, vctx->total_locals);
                vctx->local_init_stack_used += vctx->total_locals;
            }

            for (uint32_t i = 0; i < bt->param_count; ++i) {
                WAH_CHECK_GOTO(wah_validation_push_type(vctx, bt->param_types[i]), cleanup_block);
            }
            vctx->is_unreachable = false;
            if (ac) {
                WAH_ENSURE_CAP_GOTO(ac->instrs, ac->instr_count + 1, cleanup_block);
                wah_decoded_instr_t *_di = &ac->instrs[ac->instr_count++];
                *_di = (wah_decoded_instr_t){ .opcode = opcode_val, .flags = 0 };
            }
            return WAH_OK;

cleanup_block:
            if (frame) {
                wah_free(alloc, frame->block_type.param_types);
                wah_free(alloc, frame->block_type.result_types);
                frame->block_type = (wah_func_type_t){0};
            }
            if (frame_pushed && vctx->control_sp > 0) vctx->control_sp--;
            return err;
        }
        case WAH_OP_ELSE: {
            WAH_ENSURE(vctx->control_sp > 0, WAH_ERROR_VALIDATION_FAILED);
            wah_validation_control_frame_t* frame = &vctx->control_stack[vctx->control_sp - 1];
            WAH_ENSURE(frame->opcode == WAH_OP_IF && !frame->else_found, WAH_ERROR_VALIDATION_FAILED);
            frame->else_found = true;

            for (int32_t i = frame->block_type.result_count - 1; i >= 0; --i) POP(_(frame->block_type.result_types[i]));
            WAH_ENSURE(vctx->current_stack_depth == frame->stack_height, WAH_ERROR_VALIDATION_FAILED);

            vctx->type_stack.sp = frame->type_stack_sp;
            vctx->current_stack_depth = frame->type_stack_sp;

            if (vctx->local_inits) {
                memcpy(vctx->local_inits,
                       vctx->local_init_stack + frame->local_init_save_offset,
                       vctx->total_locals);
            }

            for (uint32_t i = 0; i < frame->block_type.param_count; ++i) PUSH(_(frame->block_type.param_types[i]));
            vctx->is_unreachable = false;
            EMIT_SIMPLE();
            return WAH_OK;
        }
        case WAH_OP_END: {
            if (vctx->control_sp == 0) {
                for (int32_t j = vctx->func_type->result_count - 1; j >= 0; --j) POP(_(vctx->func_type->result_types[j]));
                WAH_ENSURE(vctx->current_stack_depth == 0, WAH_ERROR_VALIDATION_FAILED);
                vctx->is_unreachable = false;
                EMIT_SIMPLE();
                return WAH_OK;
            }

            wah_validation_control_frame_t* frame = &vctx->control_stack[vctx->control_sp - 1];

            if (frame->opcode == WAH_OP_IF && !frame->else_found) {
                WAH_ENSURE(frame->block_type.param_count == frame->block_type.result_count, WAH_ERROR_VALIDATION_FAILED);
                for (uint32_t i = 0; i < frame->block_type.param_count; ++i) {
                    WAH_ENSURE(frame->block_type.param_types[i] == frame->block_type.result_types[i], WAH_ERROR_VALIDATION_FAILED);
                }
            }

            for (int32_t i = frame->block_type.result_count - 1; i >= 0; --i) POP(_(frame->block_type.result_types[i]));
            WAH_ENSURE(vctx->current_stack_depth == frame->stack_height, WAH_ERROR_VALIDATION_FAILED);

            // Reset stack to the state before the block
            vctx->type_stack.sp = frame->type_stack_sp;
            vctx->current_stack_depth = frame->type_stack_sp;

            if (vctx->local_inits) {
                memcpy(vctx->local_inits,
                       vctx->local_init_stack + frame->local_init_save_offset,
                       vctx->total_locals);
                vctx->local_init_stack_used = frame->local_init_save_offset;
            }

            vctx->control_sp--;
            // Restore the unreachable state from the parent control frame
            if (vctx->control_sp > 0) {
                vctx->is_unreachable = vctx->control_stack[vctx->control_sp - 1].is_unreachable;
            } else {
                vctx->is_unreachable = false;
            }

            // Push final results of the block with actual types (not ANY)
            for (uint32_t i = 0; i < frame->block_type.result_count; ++i) {
                WAH_CHECK(wah_type_stack_push(&vctx->type_stack, frame->block_type.result_types[i]));
                vctx->current_stack_depth++;
                if (vctx->current_stack_depth > vctx->max_stack_depth) {
                    vctx->max_stack_depth = vctx->current_stack_depth;
                }
            }

            // Free memory allocated for the block type in the control frame
            wah_free(alloc, frame->block_type.param_types);
            wah_free(alloc, frame->block_type.result_types);
            frame->block_type.param_types = NULL;
            frame->block_type.result_types = NULL;
            EMIT_SIMPLE();
            return WAH_OK;
        }

        case WAH_OP_BR:
        case WAH_OP_BR_IF: {
            bool is_br_if = opcode_val == WAH_OP_BR_IF;
            uint32_t label_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &label_idx));
            WAH_ENSURE(label_idx <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED);

            if (is_br_if) POP(I32);

            uint32_t br_result_count;
            const wah_type_t *br_result_types;
            uint32_t br_stack_height;
            wah_validation_resolve_br_target(vctx, label_idx, &br_result_count, &br_result_types, &br_stack_height);

            uint32_t adj_keep = 0, adj_drop = 0;
            if (!vctx->is_unreachable) {
                if (!is_br_if) {
                    uint32_t block_floor = (vctx->control_sp > 0) ? vctx->control_stack[vctx->control_sp - 1].stack_height : 0;
                    WAH_ENSURE(vctx->current_stack_depth >= br_stack_height + br_result_count, WAH_ERROR_VALIDATION_FAILED);
                    WAH_ENSURE(vctx->current_stack_depth >= block_floor + br_result_count, WAH_ERROR_VALIDATION_FAILED);
                } else {
                    WAH_ENSURE(vctx->current_stack_depth >= br_result_count, WAH_ERROR_VALIDATION_FAILED);
                }
                adj_keep = br_result_count;
                adj_drop = vctx->current_stack_depth - br_stack_height - br_result_count;
            }

            for (int32_t i = br_result_count - 1; i >= 0; --i) POP(_(br_result_types[i]));
            if (is_br_if) {
                for (uint32_t i = 0; i < br_result_count; ++i) PUSH(_(br_result_types[i]));
            } else {
                wah_validation_mark_unreachable(vctx);
            }
            EMIT_INSTR_EX(opcode_val,
                _di->imm.branch.label_idx = label_idx; _di->imm.branch.target_symbol = 0;
                _di->imm.branch.keep = adj_keep; _di->imm.branch.drop = adj_drop);
            return WAH_OK;
        }

        case WAH_OP_BR_TABLE: {
            wah_error_t err = WAH_OK;
            uint32_t num_targets;
            WAH_CHECK(wah_decode_and_validate_count(code_ptr, code_end, &num_targets, 1));
            WAH_ENSURE(num_targets < UINT32_MAX, WAH_ERROR_TOO_LARGE);
            WAH_ENSURE((uint64_t)num_targets + 1 <= (uint64_t)(code_end - *code_ptr), WAH_ERROR_MALFORMED);

            uint32_t* label_indices = NULL;
            uint32_t *bt_drops = NULL;
            WAH_MALLOC_ARRAY_GOTO(label_indices, num_targets + 1, cleanup_br_table);

            for (uint32_t i = 0; i < num_targets; ++i) {
                WAH_CHECK_GOTO(wah_decode_uleb128(code_ptr, code_end, &label_indices[i]), cleanup_br_table);
                WAH_ENSURE_GOTO(label_indices[i] <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED, cleanup_br_table);
            }

            WAH_CHECK_GOTO(wah_decode_uleb128(code_ptr, code_end, &label_indices[num_targets]), cleanup_br_table);
            WAH_ENSURE_GOTO(label_indices[num_targets] <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED, cleanup_br_table);

            WAH_CHECK_GOTO(wah_validation_pop_and_match_type(vctx, WAH_TYPE_I32), cleanup_br_table);

            uint32_t default_result_count;
            const wah_type_t *default_result_types;
            uint32_t default_stack_height;
            wah_validation_resolve_br_target(vctx, label_indices[num_targets],
                                              &default_result_count, &default_result_types, &default_stack_height);

            for (uint32_t i = 0; i < num_targets; ++i) {
                uint32_t cur_result_count;
                wah_validation_resolve_br_target(vctx, label_indices[i], &cur_result_count, NULL, NULL);
                WAH_ENSURE_GOTO(cur_result_count == default_result_count, WAH_ERROR_VALIDATION_FAILED, cleanup_br_table);
            }

            WAH_MALLOC_ARRAY_GOTO(bt_drops, num_targets + 1, cleanup_br_table);
            uint32_t bt_keep = 0;
            if (!vctx->is_unreachable) {
                bt_keep = default_result_count;
                for (uint32_t i = 0; i < num_targets; ++i) {
                    uint32_t target_stack_height, dummy_count;
                    wah_validation_resolve_br_target(vctx, label_indices[i], &dummy_count, NULL, &target_stack_height);
                    uint32_t d = vctx->current_stack_depth - target_stack_height - default_result_count;
                    bt_drops[i] = d;
                }
                uint32_t dd = vctx->current_stack_depth - default_stack_height - default_result_count;
                bt_drops[num_targets] = dd;
            } else {
                for (uint32_t i = 0; i <= num_targets; ++i) bt_drops[i] = 0;
            }

            wah_type_t *popped_types = NULL;
            if (default_result_count > 0) {
                WAH_MALLOC_ARRAY_GOTO(popped_types, default_result_count, cleanup_br_table_popped);
            }
            for (int32_t i = default_result_count - 1; i >= 0; --i) {
                WAH_CHECK_GOTO(wah_validation_pop_type(vctx, &popped_types[i]), cleanup_br_table_popped);
                WAH_CHECK_GOTO(wah_validate_type_match(popped_types[i], default_result_types[i], vctx->module), cleanup_br_table_popped);
            }
            for (uint32_t i = 0; i < num_targets; ++i) {
                uint32_t cur_result_count;
                const wah_type_t *cur_result_types;
                wah_validation_resolve_br_target(vctx, label_indices[i],
                                                 &cur_result_count, &cur_result_types, NULL);
                for (uint32_t j = 0; j < default_result_count; ++j) {
                    WAH_CHECK_GOTO(wah_validate_type_match(popped_types[j], cur_result_types[j], vctx->module), cleanup_br_table_popped);
                }
            }
            err = WAH_OK;
        cleanup_br_table_popped:
            wah_free(alloc, popped_types);
            if (err != WAH_OK) goto cleanup_br_table;

            wah_validation_mark_unreachable(vctx);
            EMIT_INSTR_EX(opcode_val, {
                _di->imm.br_table.target_count = num_targets;
                _di->imm.br_table.default_symbol = label_indices[num_targets];
                _di->imm.br_table.target_symbols = NULL;
                _di->imm.br_table.keep = bt_keep;
                _di->imm.br_table.drops = bt_drops;
                bt_drops = NULL;
                if (num_targets > 0) {
                    WAH_MALLOC_ARRAY_GOTO(_di->imm.br_table.target_symbols, num_targets, cleanup_br_table);
                    for (uint32_t _k = 0; _k < num_targets; _k++) _di->imm.br_table.target_symbols[_k] = label_indices[_k];
                }
            });
            err = WAH_OK;
        cleanup_br_table:
            wah_free(alloc, bt_drops);
            wah_free(alloc, label_indices);
            return err;
        }

        case WAH_OP_RETURN: {
            if (!vctx->is_unreachable) {
                uint32_t block_floor = (vctx->control_sp > 0) ? vctx->control_stack[vctx->control_sp - 1].stack_height : 0;
                WAH_ENSURE(vctx->current_stack_depth >= block_floor + vctx->func_type->result_count, WAH_ERROR_VALIDATION_FAILED);
            }
            for (int32_t j = vctx->func_type->result_count - 1; j >= 0; --j) POP(_(vctx->func_type->result_types[j]));
            wah_validation_mark_unreachable(vctx);
            EMIT_SIMPLE();
            return WAH_OK;
        }

        case WAH_OP_REF_NULL: {
            wah_type_t ref_type;
            WAH_CHECK(wah_decode_heap_type(code_ptr, code_end, &ref_type));
            if (ref_type >= 0) {
                WAH_ENSURE(WAH_TYIDX(ref_type) < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            }

            PUSH(_(WAH_TYPE_AS_NULLABLE(ref_type)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = (uint32_t)(int32_t)ref_type);
            break;
        }

        case WAH_OP_REF_IS_NULL: {
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(WAH_TYPE_IS_REF(ref_type) || ref_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);
            PUSH(I32);
            EMIT_SIMPLE();
            break;
        }

        case WAH_OP_REF_FUNC: {
            uint32_t func_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &func_idx));
            WAH_ENSURE(func_idx < wah_func_index_limit(vctx->module), WAH_ERROR_VALIDATION_FAILED);
            if (vctx->mode == WAH_ANALYZE_CONST_EXPR) {
                WAH_CHECK(wah_declare_func(vctx->module, func_idx));
            } else {
                WAH_ENSURE(wah_is_func_declared(vctx->module, func_idx), WAH_ERROR_VALIDATION_FAILED);
            }
            const wah_func_type_t *ft = wah_resolve_func_type(vctx->module, func_idx);
            uint32_t type_idx = (uint32_t)(ft - vctx->module->types);
            PUSH(_(WAH_TYPE_FROM_IDX(type_idx, 0)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = func_idx);
            break;
        }

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
        case WAH_OP_STRUCT_NEW:
        case WAH_OP_STRUCT_NEW_DEFAULT: {
            uint32_t typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_STRUCT, WAH_ERROR_VALIDATION_FAILED);
            if (opcode_val == WAH_OP_STRUCT_NEW) {
                for (uint32_t j = td->field_count; j > 0; --j)
                    WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[j - 1]));
            } else {
                for (uint32_t j = 0; j < td->field_count; ++j) {
                    wah_type_t ft = td->field_types[j];
                    WAH_ENSURE(!WAH_TYPE_IS_REF(ft) || WAH_TYPE_IS_NULLABLE(ft), WAH_ERROR_VALIDATION_FAILED);
                }
            }
            PUSH(_(WAH_TYPE_FROM_IDX(typeidx, 0)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = typeidx);
            break;
        }
        case WAH_OP_STRUCT_GET: case WAH_OP_STRUCT_GET_S: case WAH_OP_STRUCT_GET_U: {
            uint32_t typeidx, fieldidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &fieldidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_STRUCT, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(fieldidx < td->field_count, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t ft = td->field_types[fieldidx];
            if (opcode_val == WAH_OP_STRUCT_GET) {
                WAH_ENSURE(!WAH_TYPE_IS_PACKED(ft), WAH_ERROR_VALIDATION_FAILED);
            } else {
                WAH_ENSURE(WAH_TYPE_IS_PACKED(ft), WAH_ERROR_VALIDATION_FAILED);
            }
            POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            PUSH(_(WAH_TYPE_IS_PACKED(ft) ? WAH_TYPE_I32 : ft));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_field.type_idx = typeidx; _di->imm.type_field.field_idx = fieldidx);
            break;
        }
        case WAH_OP_STRUCT_SET: {
            uint32_t typeidx, fieldidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &fieldidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_STRUCT, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(fieldidx < td->field_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(td->field_mutables[fieldidx], WAH_ERROR_VALIDATION_FAILED);
            WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[fieldidx]));
            POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_field.type_idx = typeidx; _di->imm.type_field.field_idx = fieldidx);
            break;
        }
        case WAH_OP_ARRAY_NEW:
        case WAH_OP_ARRAY_NEW_DEFAULT: {
            uint32_t typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            POP(I32);
            if (opcode_val == WAH_OP_ARRAY_NEW) {
                WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[0]));
            } else {
                wah_type_t ft = td->field_types[0];
                WAH_ENSURE(!WAH_TYPE_IS_REF(ft) || WAH_TYPE_IS_NULLABLE(ft), WAH_ERROR_VALIDATION_FAILED);
            }
            PUSH(_(WAH_TYPE_FROM_IDX(typeidx, 0)));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = typeidx);
            break;
        }
        case WAH_OP_ARRAY_NEW_FIXED: {
            uint32_t typeidx, length;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &length));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            if (!vctx->is_unreachable) {
                uint32_t base = wah_validation_block_base_height(vctx);
                WAH_ENSURE(length <= vctx->current_stack_depth - base, WAH_ERROR_VALIDATION_FAILED);
                for (uint32_t j = 0; j < length; ++j)
                    WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[0]));
            }
            PUSH(_(WAH_TYPE_FROM_IDX(typeidx, 0)));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_length.type_idx = typeidx; _di->imm.type_length.length = length);
            break;
        }
        case WAH_OP_ARRAY_GET: case WAH_OP_ARRAY_GET_S: case WAH_OP_ARRAY_GET_U: {
            uint32_t typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t et = td->field_types[0];
            if (opcode_val == WAH_OP_ARRAY_GET) {
                WAH_ENSURE(!WAH_TYPE_IS_PACKED(et), WAH_ERROR_VALIDATION_FAILED);
            } else {
                WAH_ENSURE(WAH_TYPE_IS_PACKED(et), WAH_ERROR_VALIDATION_FAILED);
            }
            POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            PUSH(_(WAH_TYPE_IS_PACKED(et) ? WAH_TYPE_I32 : et));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = typeidx);
            break;
        }
        case WAH_OP_ARRAY_SET: {
            uint32_t typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(td->field_mutables[0], WAH_ERROR_VALIDATION_FAILED);
            WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[0]));
            POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = typeidx);
            break;
        }
        case WAH_OP_ARRAY_LEN: {
            wah_type_t rt; POP_INTO(&rt);
            WAH_ENSURE(rt == WAH_TYPE_BOT ||
                       wah_type_is_subtype(rt, WAH_TYPE_ARRAYREF, vctx->module),
                       WAH_ERROR_VALIDATION_FAILED);
            PUSH(I32);
            EMIT_SIMPLE();
            break;
        }
        case WAH_OP_ARRAY_NEW_DATA: case WAH_OP_ARRAY_NEW_ELEM: {
            uint32_t typeidx, segidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &segidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[typeidx].kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            if (opcode_val == WAH_OP_ARRAY_NEW_DATA) {
                WAH_ENSURE(!WAH_TYPE_IS_REF(vctx->module->type_defs[typeidx].field_types[0]), WAH_ERROR_VALIDATION_FAILED);
                WAH_ENSURE(segidx < vctx->module->data_segment_count, WAH_ERROR_VALIDATION_FAILED);
            } else {
                WAH_ENSURE(segidx < vctx->module->element_segment_count, WAH_ERROR_VALIDATION_FAILED);
                WAH_CHECK(wah_validate_type_match(vctx->module->element_segments[segidx].elem_type,
                    vctx->module->type_defs[typeidx].field_types[0], vctx->module));
            }
            POP(I32); POP(I32); PUSH(_(WAH_TYPE_FROM_IDX(typeidx, 0)));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_length.type_idx = typeidx; _di->imm.type_length.length = segidx);
            break;
        }
        case WAH_OP_ARRAY_FILL: {
            uint32_t typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(td->field_mutables[0], WAH_ERROR_VALIDATION_FAILED);
            POP(I32);
            WAH_CHECK(wah_validation_pop_field_value(vctx, td->field_types[0]));
            POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = typeidx);
            break;
        }
        case WAH_OP_ARRAY_COPY: {
            uint32_t dst_typeidx, src_typeidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &dst_typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &src_typeidx));
            WAH_ENSURE(dst_typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[dst_typeidx].kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[dst_typeidx].field_mutables[0], WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(src_typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(vctx->module->type_defs[src_typeidx].kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *dst_td = &vctx->module->type_defs[dst_typeidx];
            const wah_type_def_t *src_td = &vctx->module->type_defs[src_typeidx];
            WAH_CHECK(wah_validate_type_match(src_td->field_types[0],
                WAH_TYPE_AS_NULLABLE(dst_td->field_types[0]), vctx->module));
            POP(I32); POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(src_typeidx, 0))));
            POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(dst_typeidx, 0))));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_length.type_idx = dst_typeidx; _di->imm.type_length.length = src_typeidx);
            break;
        }
        case WAH_OP_ARRAY_INIT_DATA: case WAH_OP_ARRAY_INIT_ELEM: {
            uint32_t typeidx, segidx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &typeidx));
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &segidx));
            WAH_ENSURE(typeidx < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *td = &vctx->module->type_defs[typeidx];
            WAH_ENSURE(td->kind == WAH_COMP_ARRAY, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(td->field_mutables[0], WAH_ERROR_VALIDATION_FAILED);
            if (opcode_val == WAH_OP_ARRAY_INIT_DATA) {
                WAH_ENSURE(!WAH_TYPE_IS_REF(td->field_types[0]), WAH_ERROR_VALIDATION_FAILED);
                WAH_ENSURE(segidx < vctx->module->data_segment_count, WAH_ERROR_VALIDATION_FAILED);
            } else {
                WAH_ENSURE(WAH_TYPE_IS_REF(td->field_types[0]), WAH_ERROR_VALIDATION_FAILED);
                WAH_ENSURE(segidx < vctx->module->element_segment_count, WAH_ERROR_VALIDATION_FAILED);
                WAH_CHECK(wah_validate_type_match(vctx->module->element_segments[segidx].elem_type,
                    td->field_types[0], vctx->module));
            }
            POP(I32); POP(I32); POP(I32); POP(_(WAH_TYPE_AS_NULLABLE(WAH_TYPE_FROM_IDX(typeidx, 0))));
            EMIT_INSTR_EX(opcode_val, _di->imm.type_length.type_idx = typeidx; _di->imm.type_length.length = segidx);
            break;
        }

        case WAH_OP_REF_EQ: {
            POP(EQREF); POP(EQREF); PUSH(I32);
            EMIT_SIMPLE();
            break;
        }
        case WAH_OP_REF_AS_NON_NULL: {
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(WAH_TYPE_IS_REF(ref_type) || ref_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);
            PUSH(_(ref_type == WAH_TYPE_BOT ? WAH_TYPE_BOT : WAH_TYPE_AS_NON_NULL(ref_type)));
            EMIT_SIMPLE();
            break;
        }
        case WAH_OP_BR_ON_NULL: case WAH_OP_BR_ON_NON_NULL: {
            uint32_t label_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &label_idx));
            WAH_ENSURE(label_idx <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(WAH_TYPE_IS_REF(ref_type) || ref_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);

            uint32_t br_result_count;
            const wah_type_t *br_result_types;
            uint32_t br_stack_height;
            wah_validation_resolve_br_target(vctx, label_idx,
                &br_result_count, &br_result_types, &br_stack_height);

            uint32_t prefix_result_count = br_result_count;
            if (opcode_val == WAH_OP_BR_ON_NON_NULL) {
                WAH_ENSURE(br_result_count >= 1, WAH_ERROR_VALIDATION_FAILED);
                wah_type_t non_null_ref_type = (ref_type == WAH_TYPE_BOT) ? WAH_TYPE_BOT : WAH_TYPE_AS_NON_NULL(ref_type);
                WAH_ENSURE(wah_type_is_subtype(non_null_ref_type, br_result_types[br_result_count - 1], vctx->module),
                           WAH_ERROR_VALIDATION_FAILED);
                prefix_result_count--;
            }

            uint32_t adj_keep = 0, adj_drop = 0;
            if (!vctx->is_unreachable) {
                WAH_ENSURE(vctx->current_stack_depth >= br_stack_height + prefix_result_count, WAH_ERROR_VALIDATION_FAILED);
                adj_keep = br_result_count;
                adj_drop = vctx->current_stack_depth - br_stack_height - prefix_result_count;
            }

            for (int32_t i = (int32_t)prefix_result_count - 1; i >= 0; --i) POP(_(br_result_types[i]));
            for (uint32_t i = 0; i < prefix_result_count; ++i) PUSH(_(br_result_types[i]));
            if (opcode_val == WAH_OP_BR_ON_NULL) {
                PUSH(_(ref_type == WAH_TYPE_BOT ? WAH_TYPE_BOT : WAH_TYPE_AS_NON_NULL(ref_type)));
            }
            EMIT_INSTR_EX(opcode_val,
                _di->imm.branch.label_idx = label_idx; _di->imm.branch.target_symbol = 0;
                _di->imm.branch.keep = adj_keep; _di->imm.branch.drop = adj_drop);
            break;
        }

        case WAH_OP_REF_TEST_NULL: case WAH_OP_REF_TEST:
        case WAH_OP_REF_CAST_NULL: case WAH_OP_REF_CAST: {
            wah_type_t heap_type; WAH_CHECK(wah_decode_heap_type(code_ptr, code_end, &heap_type));
            if (heap_type >= 0) {
                WAH_ENSURE(WAH_TYIDX(heap_type) < vctx->module->type_count, WAH_ERROR_VALIDATION_FAILED);
            }
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(WAH_TYPE_IS_REF(ref_type) || ref_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);
            if (ref_type != WAH_TYPE_BOT) {
                wah_type_t src_top = wah_type_hierarchy_top(ref_type, vctx->module);
                wah_type_t tgt_top = wah_type_hierarchy_top(heap_type, vctx->module);
                WAH_ENSURE(src_top != WAH_TYPE_BOT && src_top == tgt_top, WAH_ERROR_VALIDATION_FAILED);
            }
            if (opcode_val == WAH_OP_REF_TEST_NULL || opcode_val == WAH_OP_REF_TEST) {
                PUSH(I32);
            } else {
                wah_type_t cast_type = WAH_TYPE_IS_REF(heap_type) ? heap_type :
                    (heap_type >= 0 ? heap_type : WAH_TYPE_ANYREF);
                if (opcode_val == WAH_OP_REF_CAST_NULL) cast_type = WAH_TYPE_AS_NULLABLE(cast_type);
                PUSH(_(cast_type));
            }
            EMIT_INSTR_EX(opcode_val, _di->imm.ref_cast.heap_type = (int32_t)heap_type);
            break;
        }

        case WAH_OP_BR_ON_CAST: case WAH_OP_BR_ON_CAST_FAIL: {
            WAH_ENSURE(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF);
            uint8_t cast_flags = *(*code_ptr)++;
            uint32_t label_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &label_idx));
            wah_type_t ht1, ht2;
            WAH_CHECK(wah_decode_heap_type(code_ptr, code_end, &ht1));
            WAH_CHECK(wah_decode_heap_type(code_ptr, code_end, &ht2));
            WAH_ENSURE(wah_type_is_subtype(ht2, ht1, vctx->module), WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(label_idx <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED);
            bool src_nullable = (cast_flags & 0x01) != 0;
            bool dst_nullable = (cast_flags & 0x02) != 0;
            WAH_ENSURE(!dst_nullable || src_nullable, WAH_ERROR_VALIDATION_FAILED);
            wah_type_t src_type = WAH_TYPE_IS_REF(ht1) ? ht1 : (ht1 >= 0 ? ht1 : WAH_TYPE_ANYREF);
            wah_type_t dst_type = WAH_TYPE_IS_REF(ht2) ? ht2 : (ht2 >= 0 ? ht2 : WAH_TYPE_ANYREF);
            if (src_nullable) src_type = WAH_TYPE_AS_NULLABLE(src_type);
            if (dst_nullable) dst_type = WAH_TYPE_AS_NULLABLE(dst_type);
            POP(_(src_type));

            uint32_t br_result_count;
            const wah_type_t *br_result_types;
            uint32_t br_stack_height;
            wah_validation_resolve_br_target(vctx, label_idx, &br_result_count, &br_result_types, &br_stack_height);
            WAH_ENSURE(br_result_count >= 1, WAH_ERROR_VALIDATION_FAILED);

            wah_type_t br_type;
            if (opcode_val == WAH_OP_BR_ON_CAST) {
                br_type = dst_type;
            } else {
                br_type = dst_nullable ? WAH_TYPE_AS_NON_NULL(src_type) : src_type;
            }
            WAH_CHECK(wah_validate_type_match(br_type, br_result_types[br_result_count - 1], vctx->module));

            uint32_t prefix_result_count = br_result_count - 1;
            uint32_t adj_keep = 0, adj_drop = 0;
            if (!vctx->is_unreachable) {
                WAH_ENSURE(vctx->current_stack_depth >= br_stack_height + prefix_result_count, WAH_ERROR_VALIDATION_FAILED);
                adj_keep = br_result_count;
                adj_drop = vctx->current_stack_depth - br_stack_height - prefix_result_count;
                if (br_result_count > 1) {
                    for (uint32_t k = 0; k < prefix_result_count; ++k) {
                        uint32_t stack_pos = vctx->type_stack.sp - 1 - k;
                        WAH_ENSURE(vctx->type_stack.data[stack_pos] == br_result_types[prefix_result_count - 1 - k],
                                   WAH_ERROR_VALIDATION_FAILED);
                    }
                }
            }

            if (opcode_val == WAH_OP_BR_ON_CAST) {
                PUSH(_(dst_nullable ? WAH_TYPE_AS_NON_NULL(src_type) : src_type));
            } else {
                PUSH(_(dst_type));
            }
            EMIT_INSTR_EX(opcode_val,
                _di->imm.br_on_cast.cast_flags = cast_flags; _di->imm.br_on_cast.target_symbol = label_idx;
                _di->imm.br_on_cast.src_type = ht1; _di->imm.br_on_cast.dst_type = ht2;
                _di->imm.br_on_cast.dst_heap_type = (int32_t)ht2;
                _di->imm.br_on_cast.keep = adj_keep; _di->imm.br_on_cast.drop = adj_drop);
            break;
        }

        case WAH_OP_ANY_CONVERT_EXTERN: {
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(ref_type == WAH_TYPE_BOT ||
                       wah_type_hierarchy_top(ref_type, vctx->module) == WAH_TYPE_EXTERN,
                       WAH_ERROR_VALIDATION_FAILED);
            PUSH(_(WAH_TYPE_IS_NULLABLE(ref_type) ? WAH_TYPE_ANYREF : WAH_TYPE_ANY));
            break; // No opcode emitted
        }
        case WAH_OP_EXTERN_CONVERT_ANY: {
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(ref_type == WAH_TYPE_BOT ||
                       wah_type_hierarchy_top(ref_type, vctx->module) == WAH_TYPE_ANY,
                       WAH_ERROR_VALIDATION_FAILED);
            PUSH(_(WAH_TYPE_IS_NULLABLE(ref_type) ? WAH_TYPE_EXTERNREF : WAH_TYPE_EXTERN));
            break; // No opcode emitted
        }
        case WAH_OP_REF_I31: {
            POP(I32); PUSH(I31);
            EMIT_SIMPLE();
            break;
        }
        case WAH_OP_I31_GET_S:
        case WAH_OP_I31_GET_U: {
            POP(I31REF);
            PUSH(I32);
            EMIT_SIMPLE();
            break;
        }
#endif // WAH_FEATURE_GC (validation)

        case WAH_OP_THROW: {
            uint32_t tag_idx;
            WAH_CHECK(wah_decode_uleb128(code_ptr, code_end, &tag_idx));
            uint32_t total_tags = vctx->module->import_tag_count + vctx->module->tag_count;
            WAH_ENSURE(tag_idx < total_tags, WAH_ERROR_VALIDATION_FAILED);
            uint32_t type_idx = tag_idx < vctx->module->import_tag_count
                ? vctx->module->tag_imports[tag_idx].type_index
                : vctx->module->tags[tag_idx - vctx->module->import_tag_count].type_index;
            const wah_func_type_t *tag_type = &vctx->module->types[type_idx];
            WAH_CHECK(wah_validation_pop_func_params(vctx, tag_type));
            wah_validation_mark_unreachable(vctx);
            EMIT_INSTR_EX(opcode_val, _di->imm.u32 = tag_idx);
            break;
        }

        case WAH_OP_THROW_REF: {
            wah_type_t ref_type; POP_INTO(&ref_type);
            WAH_ENSURE(WAH_TYPE_AS_NON_NULL(ref_type) == WAH_TYPE_EXN ||
                       WAH_TYPE_AS_NON_NULL(ref_type) == WAH_TYPE_NOEXN ||
                       ref_type == WAH_TYPE_BOT, WAH_ERROR_VALIDATION_FAILED);
            wah_validation_mark_unreachable(vctx);
            EMIT_SIMPLE();
            break;
        }

        case WAH_OP_TRY_TABLE: {
            wah_error_t err = WAH_OK;
            WAH_ENSURE(vctx->control_sp < WAH_MAX_CONTROL_DEPTH, WAH_ERROR_TOO_LARGE);
            typedef struct { uint8_t kind; uint32_t tag_idx; uint32_t label_idx; } wah_catch_entry_t;
            wah_catch_entry_t *catch_entries = NULL;
            wah_validation_control_frame_t* frame = &vctx->control_stack[vctx->control_sp];
            frame->opcode = (wah_opcode_t)opcode_val;
            frame->else_found = false;
            frame->is_unreachable = vctx->is_unreachable;
            wah_func_type_t* bt = &frame->block_type;
            WAH_CHECK_GOTO(wah_validation_decode_block_type(code_ptr, code_end, vctx, bt), cleanup_try_table);

            uint32_t catch_count;
            WAH_CHECK_GOTO(wah_decode_and_validate_count(code_ptr, code_end, &catch_count, 2), cleanup_try_table);

            if (catch_count > 0) {
                WAH_MALLOC_ARRAY_GOTO(catch_entries, catch_count, cleanup_try_table);
            }

            for (uint32_t ci = 0; ci < catch_count; ci++) {
                WAH_ENSURE_GOTO(*code_ptr < code_end, WAH_ERROR_UNEXPECTED_EOF, cleanup_try_table);
                uint8_t catch_kind = *(*code_ptr)++;
                WAH_ENSURE_GOTO(catch_kind <= 3, WAH_ERROR_VALIDATION_FAILED, cleanup_try_table);
                catch_entries[ci].kind = catch_kind;
                catch_entries[ci].tag_idx = 0;
                if (catch_kind == 0 || catch_kind == 1) {
                    WAH_CHECK_GOTO(wah_decode_uleb128(code_ptr, code_end, &catch_entries[ci].tag_idx), cleanup_try_table);
                    uint32_t total_tags = vctx->module->import_tag_count + vctx->module->tag_count;
                    WAH_ENSURE_GOTO(catch_entries[ci].tag_idx < total_tags, WAH_ERROR_VALIDATION_FAILED, cleanup_try_table);
                }
                WAH_CHECK_GOTO(wah_decode_uleb128(code_ptr, code_end, &catch_entries[ci].label_idx), cleanup_try_table);
                WAH_ENSURE_GOTO(catch_entries[ci].label_idx <= vctx->control_sp, WAH_ERROR_VALIDATION_FAILED, cleanup_try_table);
            }

            vctx->control_sp++;

            for (uint32_t ci = 0; ci < catch_count; ci++) {
                uint32_t adjusted_label = catch_entries[ci].label_idx + 1;
                uint32_t br_result_count;
                const wah_type_t *br_result_types;
                wah_validation_resolve_br_target(vctx, adjusted_label,
                    &br_result_count, &br_result_types, NULL);

                uint32_t tag_param_count = 0;
                const wah_type_t *tag_param_types = NULL;
                if (catch_entries[ci].kind == 0 || catch_entries[ci].kind == 1) {
                    uint32_t tidx = catch_entries[ci].tag_idx;
                    uint32_t type_idx = (tidx < vctx->module->import_tag_count)
                        ? vctx->module->tag_imports[tidx].type_index
                        : vctx->module->tags[tidx - vctx->module->import_tag_count].type_index;
                    const wah_func_type_t *tag_type = &vctx->module->types[type_idx];
                    tag_param_count = tag_type->param_count;
                    tag_param_types = tag_type->param_types;
                }

                bool has_exnref = (catch_entries[ci].kind == 1 || catch_entries[ci].kind == 3);
                uint32_t expected_count = tag_param_count + (has_exnref ? 1 : 0);
                WAH_ENSURE_GOTO(br_result_count == expected_count, WAH_ERROR_VALIDATION_FAILED, cleanup_try_table);
                for (uint32_t j = 0; j < tag_param_count; j++) {
                    WAH_CHECK_GOTO(wah_validate_type_match(
                        tag_param_types[j], br_result_types[j], vctx->module), cleanup_try_table);
                }
                if (has_exnref) {
                    WAH_CHECK_GOTO(wah_validate_type_match(
                        WAH_TYPE_EXN, br_result_types[br_result_count - 1], vctx->module), cleanup_try_table);
                }
            }

            WAH_ENSURE_GOTO(vctx->current_stack_depth >= bt->param_count, WAH_ERROR_VALIDATION_FAILED, cleanup_try_table);
            for (uint32_t i = 0; i < bt->param_count; ++i) {
                wah_type_t actual_type = vctx->type_stack.data[vctx->type_stack.sp - bt->param_count + i];
                WAH_CHECK_GOTO(wah_validate_type_match(actual_type, bt->param_types[i], vctx->module), cleanup_try_table);
            }

            frame->stack_height = vctx->current_stack_depth - bt->param_count;
            frame->type_stack_sp = vctx->type_stack.sp - bt->param_count;

            if (vctx->local_inits) {
                frame->local_init_save_offset = vctx->local_init_stack_used;
                memcpy(vctx->local_init_stack + vctx->local_init_stack_used,
                       vctx->local_inits, vctx->total_locals);
                vctx->local_init_stack_used += vctx->total_locals;
            }

            EMIT_INSTR_EX(opcode_val, {
                _di->imm.try_table.catch_count = catch_count;
                _di->imm.try_table.catches = NULL;
                if (catch_count > 0) {
                    WAH_MALLOC_ARRAY_GOTO(_di->imm.try_table.catches, catch_count, cleanup_try_table);
                    for (uint32_t ci = 0; ci < catch_count; ci++) {
                        _di->imm.try_table.catches[ci].kind = catch_entries[ci].kind;
                        _di->imm.try_table.catches[ci].tag_idx = catch_entries[ci].tag_idx;
                        _di->imm.try_table.catches[ci].target_symbol = catch_entries[ci].label_idx;
                    }
                }
            });
            wah_free(alloc, catch_entries);
            return WAH_OK;

cleanup_try_table:
            wah_free(alloc, catch_entries);
            wah_free(alloc, bt->param_types);
            wah_free(alloc, bt->result_types);
            *bt = (wah_func_type_t){0};
            return err;
        }

        default:
            return WAH_ERROR_MALFORMED;
    }

#undef PUSH
#undef POP
#undef POP_INTO
#undef EMIT_INSTR_EX
#undef EMIT_SIMPLE

    return WAH_OK;
}

// Lowering function that consumes analyzed IR instead of raw Wasm bytes
static wah_error_t wah_lower_analyzed_code(const wah_module_t* module, const wah_analyzed_code_t *ac, wah_parsed_code_t *parsed_code) {
    const wah_alloc_t *alloc = &module->alloc;
    bool emit_poll = (ac->mode == WAH_ANALYZE_FUNC_BODY);
    wah_error_t err = WAH_OK;
    uint8_t *saved_ref_map = parsed_code->operand_ref_map;
    uint32_t saved_ref_map_size = parsed_code->operand_ref_map_size;
    *parsed_code = (wah_parsed_code_t){ .operand_ref_map = saved_ref_map, .operand_ref_map_size = saved_ref_map_size };

    // --- Control frame for single-pass backpatching ---
    typedef struct {
        wah_opcode_t opcode;
        uint32_t continuation_offset; // for LOOP: offset of loop header in buf
        uint32_t if_false_patch;      // for IF: buf offset of the false-branch placeholder
        uint32_t *patch_offsets;      // positions needing backpatch with this frame's end offset
        uint32_t patch_count;
        uint32_t patch_offsets_cap;
    } wah_lower_cf_t;

    // --- Growable buffer ---
    uint8_t *buf = NULL;
    uint32_t buf_size = 0;
    uint32_t buf_cap = 0;

    // --- Function-end patches (for branches targeting function scope) ---
    uint32_t *func_end_patches = NULL;
    uint32_t func_end_patch_count = 0;
    uint32_t func_end_patches_cap = 0;

    wah_lower_cf_t control_stack[WAH_MAX_CONTROL_DEPTH];
    uint32_t control_sp = 0;

    // --- Fuel metering types and variables (before any goto cleanup) ---
    typedef struct {
        uint32_t meter_offset;
        uint32_t next_fast_offset;
        uint32_t instr_record_start;
        uint16_t cost;
        uint16_t instr_record_count;
    } wah_meter_chunk_t;
    typedef struct {
        uint32_t fast_offset;
        uint32_t byte_length;
    } wah_meter_instr_record_t;
    wah_meter_chunk_t *meter_chunks = NULL;
    uint32_t meter_chunk_count = 0, meter_chunks_cap = 0;
    wah_meter_instr_record_t *meter_instr_records = NULL;
    uint32_t meter_instr_record_count = 0, meter_instr_records_cap = 0;

    // Initialize buffer
    buf_cap = 256;
    WAH_MALLOC_ARRAY_GOTO(buf, buf_cap, cleanup);

    #define WAH_LOWER_ENSURE(n) do { \
        WAH_ENSURE_CAP_GOTO(buf, buf_size + (n), cleanup); \
    } while (0)

    #define WAH_LOWER_U8(v) do { \
        WAH_LOWER_ENSURE(1); \
        buf[buf_size++] = (uint8_t)(v); \
    } while (0)

    #define WAH_LOWER_U16(v) do { \
        WAH_LOWER_ENSURE(sizeof(uint16_t)); \
        wah_write_u16_le(buf + buf_size, (v)); \
        buf_size += sizeof(uint16_t); \
    } while (0)

    #define WAH_LOWER_U32(v) do { \
        WAH_LOWER_ENSURE(sizeof(uint32_t)); \
        wah_write_u32_le(buf + buf_size, (v)); \
        buf_size += sizeof(uint32_t); \
    } while (0)

    #define WAH_LOWER_U64(v) do { \
        WAH_LOWER_ENSURE(sizeof(uint64_t)); \
        wah_write_u64_le(buf + buf_size, (v)); \
        buf_size += sizeof(uint64_t); \
    } while (0)

    #define WAH_LOWER_MEM(p, n) do { \
        WAH_LOWER_ENSURE(n); \
        memcpy(buf + buf_size, (p), (n)); \
        buf_size += (n); \
    } while (0)

    #define WAH_LOWER_PATCH_U32(off, v) do { \
        wah_write_u32_le(buf + (off), (v)); \
    } while (0)

    #define WAH_LOWER_ADD_PATCH_AT(cf_ptr, pos) do { \
        wah_lower_cf_t *_cf = (cf_ptr); \
        WAH_ENSURE_CAP_GOTO(_cf->patch_offsets, _cf->patch_count + 1, cleanup); \
        _cf->patch_offsets[_cf->patch_count++] = (pos); \
    } while (0)

    #define WAH_LOWER_ADD_PATCH(cf_ptr) WAH_LOWER_ADD_PATCH_AT(cf_ptr, buf_size)

    #define WAH_LOWER_ADD_FUNC_END_PATCH() do { \
        WAH_ENSURE_CAP_GOTO(func_end_patches, func_end_patch_count + 1, cleanup); \
        func_end_patches[func_end_patch_count++] = buf_size; \
    } while (0)

    #define WAH_LOWER_APPLY_PATCHES(offsets, count, target) do { \
        for (uint32_t _pi = 0; _pi < (count); _pi++) { \
            WAH_LOWER_PATCH_U32((offsets)[_pi], (target)); \
        } \
    } while (0)

    #define WAH_LOWER_RESOLVE_BRANCH(relative_depth) do { \
        uint32_t _rd = (relative_depth); \
        if (_rd == control_sp) { \
            WAH_LOWER_ADD_FUNC_END_PATCH(); \
            WAH_LOWER_U32(0); \
        } else { \
            WAH_ASSERT(_rd < control_sp && "validation should have verified relative depth"); \
            wah_lower_cf_t *_target = &control_stack[control_sp - 1 - _rd]; \
            if (_target->opcode == WAH_OP_LOOP) { \
                WAH_LOWER_U32(_target->continuation_offset); \
            } else { \
                WAH_LOWER_ADD_PATCH(_target); \
                WAH_LOWER_U32(0); \
            } \
        } \
    } while (0)

    #define WAH_LOWER_PUSH_FRAME(op) do { \
        WAH_ASSERT(control_sp < WAH_MAX_CONTROL_DEPTH && "validation should have verified control stack size"); \
        control_stack[control_sp++] = (wah_lower_cf_t){ .opcode = (op), .continuation_offset = buf_size }; \
    } while (0)

    #define WAH_LOWER_FINISH_FRAME() do { \
        WAH_ASSERT(control_sp > 0); \
        wah_lower_cf_t *_cf = &control_stack[--control_sp]; \
        uint32_t _end_pos = buf_size; \
        WAH_LOWER_APPLY_PATCHES(_cf->patch_offsets, _cf->patch_count, _end_pos); \
        if (_cf->opcode == WAH_OP_IF) { \
            WAH_LOWER_PATCH_U32(_cf->if_false_patch, _end_pos); \
        } \
        wah_free(alloc, _cf->patch_offsets); \
        *_cf = (wah_lower_cf_t){0}; \
    } while (0)

    #if defined(WAH_X86_64) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)
    wah_x86_64_features_t features = wah_x86_64_features();
    #endif

    uint32_t poll_ref_cursor = 0;

    // --- Fuel metering state ---
    bool emit_meter = emit_poll && module->fuel_metering;
    bool meter_need_new_chunk = emit_meter;
    uint16_t meter_current_cost = 0;

    // If this changes, update test_meter_chunk_reset_before_polled_loop().
    #define WAH_METER_MAX_CHUNK_COST 100

    #define WAH_METER_START_CHUNK() do { \
        if (emit_meter && meter_need_new_chunk) { \
            WAH_ENSURE_CAP_GOTO(meter_chunks, meter_chunk_count + 1, cleanup); \
            meter_chunks[meter_chunk_count] = (wah_meter_chunk_t){ \
                .meter_offset = buf_size, \
                .instr_record_start = meter_instr_record_count, \
            }; \
            WAH_LOWER_U16(WAH_OP_METER); \
            WAH_LOWER_U16(0); \
            WAH_LOWER_U32(0); \
            meter_current_cost = 0; \
            meter_need_new_chunk = false; \
        } \
    } while (0)

    #define WAH_METER_RECORD_INSTR_START() \
        uint32_t _meter_instr_start = buf_size

    #define WAH_METER_RECORD_INSTR_END() do { \
        if (emit_meter) { \
            WAH_ENSURE_CAP_GOTO(meter_instr_records, meter_instr_record_count + 1, cleanup); \
            meter_instr_records[meter_instr_record_count++] = (wah_meter_instr_record_t){ \
                .fast_offset = _meter_instr_start, .byte_length = buf_size - _meter_instr_start \
            }; \
            meter_current_cost++; \
        } \
    } while (0)

    #define WAH_METER_END_CHUNK() do { \
        if (emit_meter && meter_current_cost > 0) { \
            wah_write_u16_le(buf + meter_chunks[meter_chunk_count].meter_offset + sizeof(uint16_t), meter_current_cost); \
            meter_chunks[meter_chunk_count].cost = meter_current_cost; \
            meter_chunks[meter_chunk_count].instr_record_count = (uint16_t)(meter_instr_record_count - meter_chunks[meter_chunk_count].instr_record_start); \
            meter_chunks[meter_chunk_count].next_fast_offset = buf_size; \
            meter_chunk_count++; \
            meter_current_cost = 0; \
            meter_need_new_chunk = true; \
        } \
    } while (0)

    #define WAH_EMIT_POLL() do { \
        WAH_LOWER_U16(WAH_OP_POLL); \
        WAH_LOWER_U32(poll_ref_cursor); \
        if (parsed_code->operand_ref_map && poll_ref_cursor < parsed_code->operand_ref_map_size) { \
            uint16_t _cnt = wah_read_u16_le(parsed_code->operand_ref_map + poll_ref_cursor); \
            uint32_t _bmp_words = (_cnt + 15) / 16; \
            const uint8_t *_bmp = parsed_code->operand_ref_map + poll_ref_cursor + sizeof(uint16_t); \
            uint32_t _refs = 0; \
            for (uint32_t _k = 0; _k < _bmp_words; _k++) { \
                uint16_t _w = wah_read_u16_le(_bmp + _k * sizeof(uint16_t)); \
                while (_w) { _refs++; _w &= _w - 1; } \
            } \
            poll_ref_cursor += sizeof(uint16_t) * (1 + _bmp_words) + _refs * sizeof(wah_type_t); \
        } \
    } while (0)

    // --- Single pass: emit bytecode with backpatching ---
    for (uint32_t ii = 0; ii < ac->instr_count; ii++) {
        const wah_decoded_instr_t *instr = &ac->instrs[ii];
        uint16_t opcode = instr->opcode;

        if (opcode == WAH_OP_BLOCK || opcode == WAH_OP_LOOP) {
            uint32_t continuation_offset = buf_size;
            if (emit_poll && (instr->flags & WAH_INSTR_FLAG_POLL)) {
                if (opcode == WAH_OP_LOOP) WAH_METER_END_CHUNK();
                WAH_EMIT_POLL();
                if (opcode == WAH_OP_LOOP) meter_need_new_chunk = emit_meter;
            }
            WAH_METER_START_CHUNK();
            WAH_METER_RECORD_INSTR_START();
            WAH_LOWER_PUSH_FRAME((wah_opcode_t)opcode);
            if (opcode == WAH_OP_LOOP) {
                control_stack[control_sp - 1].continuation_offset = continuation_offset;
            }
            WAH_METER_RECORD_INSTR_END();
            continue;
        }
        if (opcode == WAH_OP_TRY_TABLE) {
            uint32_t try_catch_count = instr->imm.try_table.catch_count;

            WAH_METER_START_CHUNK();
            WAH_METER_RECORD_INSTR_START();
            WAH_LOWER_U16(WAH_OP_TRY_TABLE);
            WAH_LOWER_U32(try_catch_count);

            for (uint32_t ci = 0; ci < try_catch_count; ci++) {
                uint8_t catch_kind = instr->imm.try_table.catches[ci].kind;
                WAH_LOWER_U8(catch_kind);
                uint32_t tag_idx = instr->imm.try_table.catches[ci].tag_idx;
                WAH_LOWER_U32(tag_idx);
                uint32_t label_depth = instr->imm.try_table.catches[ci].target_symbol;
                WAH_ASSERT(label_depth <= control_sp);
                WAH_LOWER_RESOLVE_BRANCH(label_depth);
            }

            WAH_LOWER_PUSH_FRAME(WAH_OP_TRY_TABLE);
            WAH_METER_RECORD_INSTR_END();
            continue;
        }
        if (opcode == WAH_OP_END) {
            if (control_sp > 0) {
                WAH_METER_START_CHUNK();
                WAH_METER_RECORD_INSTR_START();
                if (control_stack[control_sp - 1].opcode == WAH_OP_TRY_TABLE) {
                    WAH_LOWER_U16(WAH_OP_END_TRY_TABLE);
                }
                WAH_LOWER_FINISH_FRAME();
                WAH_METER_RECORD_INSTR_END();
                continue;
            }
            // Function-level END
            WAH_METER_START_CHUNK();
            WAH_METER_RECORD_INSTR_START();
            WAH_LOWER_U16(WAH_OP_END);
            WAH_METER_RECORD_INSTR_END();
            WAH_METER_END_CHUNK();
            uint32_t func_end_pos = buf_size - sizeof(uint16_t);
            WAH_LOWER_APPLY_PATCHES(func_end_patches, func_end_patch_count, func_end_pos);
            continue;
        }

        if (emit_poll && (instr->flags & WAH_INSTR_FLAG_POLL)) {
            WAH_EMIT_POLL();
        }

        WAH_METER_START_CHUNK();
        WAH_METER_RECORD_INSTR_START();

        #if defined(WAH_X86_64) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)
        uint16_t native_opcode = wah_x86_64_opcode(opcode, features);
        #else
        uint16_t native_opcode = opcode;
        #endif

        WAH_LOWER_U16(native_opcode);

        switch (wah_opclasses[opcode]) {
            case WAH_OPCLASS_B: {
                WAH_LOWER_U8(instr->imm.u32);
                break;
            }
            case WAH_OPCLASS_I: {
                uint32_t a = instr->imm.u32;
                // Redirect global.get/set to indirect variants for imported mutable globals
                if ((opcode == WAH_OP_GLOBAL_GET || opcode == WAH_OP_GLOBAL_SET)
                    && a < module->import_global_count && module->global_imports[a].is_mutable) {
                    uint16_t new_opcode = (opcode == WAH_OP_GLOBAL_GET)
                        ? WAH_OP_GLOBAL_GET_INDIRECT : WAH_OP_GLOBAL_SET_INDIRECT;
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_opcode);
                }
                #if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
                // Redirect memory.size/memory.grow/memory.fill to i64 variants when needed
                if ((opcode == WAH_OP_MEMORY_SIZE || opcode == WAH_OP_MEMORY_GROW || opcode == WAH_OP_MEMORY_FILL)
                    && a < wah_memory_index_limit(module) && wah_memory_type(module, a)->addr_type == WAH_TYPE_I64) {
                    uint16_t new_opcode;
                    if (opcode == WAH_OP_MEMORY_SIZE) new_opcode = WAH_OP_MEMORY_SIZE_i64;
                    else if (opcode == WAH_OP_MEMORY_GROW) new_opcode = WAH_OP_MEMORY_GROW_i64;
                    else new_opcode = WAH_OP_MEMORY_FILL_i64;
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_opcode);
                }
                // Redirect table ops to i64 variants for table64
                if ((opcode == WAH_OP_TABLE_GET || opcode == WAH_OP_TABLE_SET ||
                     opcode == WAH_OP_TABLE_SIZE || opcode == WAH_OP_TABLE_GROW || opcode == WAH_OP_TABLE_FILL)
                    && a < wah_table_index_limit(module) && wah_table_type(module, a)->addr_type == WAH_TYPE_I64) {
                    uint16_t new_opcode;
                    if (opcode == WAH_OP_TABLE_GET) new_opcode = WAH_OP_TABLE_GET_i64;
                    else if (opcode == WAH_OP_TABLE_SET) new_opcode = WAH_OP_TABLE_SET_i64;
                    else if (opcode == WAH_OP_TABLE_SIZE) new_opcode = WAH_OP_TABLE_SIZE_i64;
                    else if (opcode == WAH_OP_TABLE_GROW) new_opcode = WAH_OP_TABLE_GROW_i64;
                    else new_opcode = WAH_OP_TABLE_FILL_i64;
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_opcode);
                }
                #endif // WAH_FEATURE_MEMORY64
                WAH_LOWER_U32(a);
                break;
            }
            case WAH_OPCLASS_II: {
                uint32_t a = instr->imm.u32_u32.a;
                uint32_t b = instr->imm.u32_u32.b;
                #if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
                #define WAH_LOWER_MEM_I64(idx) ((idx) < wah_memory_index_limit(module) && wah_memory_type(module, (idx))->addr_type == WAH_TYPE_I64)
                #define WAH_LOWER_TAB_I64(idx) ((idx) < wah_table_index_limit(module) && wah_table_type(module, (idx))->addr_type == WAH_TYPE_I64)
                #define WAH_LOWER_REWRITE_IF(cond, new_op) do { if (cond) wah_write_u16_le(buf + buf_size - sizeof(uint16_t), (new_op)); } while (0)
                switch (opcode) {
                    case WAH_OP_MEMORY_INIT:   WAH_LOWER_REWRITE_IF(WAH_LOWER_MEM_I64(b), WAH_OP_MEMORY_INIT_i64); break;
                    case WAH_OP_MEMORY_COPY: {
                        bool da = WAH_LOWER_MEM_I64(a), db = WAH_LOWER_MEM_I64(b);
                        if (da && db)       WAH_LOWER_REWRITE_IF(true, WAH_OP_MEMORY_COPY_i64_to_i64);
                        else if (da && !db) WAH_LOWER_REWRITE_IF(true, WAH_OP_MEMORY_COPY_i32_to_i64);
                        else if (!da && db) WAH_LOWER_REWRITE_IF(true, WAH_OP_MEMORY_COPY_i64_to_i32);
                        break;
                    }
                    case WAH_OP_TABLE_COPY: {
                        bool da = WAH_LOWER_TAB_I64(a), db = WAH_LOWER_TAB_I64(b);
                        if (da && db)       WAH_LOWER_REWRITE_IF(true, WAH_OP_TABLE_COPY_i64_to_i64);
                        else if (da && !db) WAH_LOWER_REWRITE_IF(true, WAH_OP_TABLE_COPY_i32_to_i64);
                        else if (!da && db) WAH_LOWER_REWRITE_IF(true, WAH_OP_TABLE_COPY_i64_to_i32);
                        break;
                    }
                    case WAH_OP_TABLE_INIT:    WAH_LOWER_REWRITE_IF(WAH_LOWER_TAB_I64(b), WAH_OP_TABLE_INIT_i64); break;
                    case WAH_OP_CALL_INDIRECT: WAH_LOWER_REWRITE_IF(WAH_LOWER_TAB_I64(b), WAH_OP_CALL_INDIRECT_i64); break;
                    case WAH_OP_RETURN_CALL_INDIRECT: WAH_LOWER_REWRITE_IF(WAH_LOWER_TAB_I64(b), WAH_OP_RETURN_CALL_INDIRECT_i64); break;
                    default: break;
                }
                #undef WAH_LOWER_REWRITE_IF
                #undef WAH_LOWER_TAB_I64
                #undef WAH_LOWER_MEM_I64
                #endif // WAH_FEATURE_MEMORY64
                WAH_LOWER_U32(a);
                WAH_LOWER_U32(b);
                break;
            }
// Shared memory opcode rewriting: redirect base opcode to address-class variant
#define WAH_LOWER_MEM_REWRITE(MEM0_OPCODES, I64_MEM0_OPCODES, I64_MEM_OPCODES) \
    do { \
        bool _is_i64 = wah_memory_type(module, memidx)->addr_type == WAH_TYPE_I64; \
        if (memidx == 0 && !_is_i64) { \
            switch (opcode) { \
                MEM0_OPCODES(WAH_LOWER_MEM_CASE) \
            } \
            wah_write_u16_le(buf + buf_size - sizeof(uint16_t), opcode); \
        } else if (memidx == 0 && _is_i64) { \
            switch (opcode) { \
                I64_MEM0_OPCODES(WAH_LOWER_MEM_CASE) \
            } \
            wah_write_u16_le(buf + buf_size - sizeof(uint16_t), opcode); \
        } else if (_is_i64) { \
            switch (opcode) { \
                I64_MEM_OPCODES(WAH_LOWER_MEM_CASE) \
            } \
            wah_write_u16_le(buf + buf_size - sizeof(uint16_t), opcode); \
            WAH_LOWER_U32(memidx); \
        } else { \
            WAH_LOWER_U32(memidx); \
        } \
    } while (0)
#define WAH_LOWER_MEM_CASE(name, suffix) case WAH_OP_##name: opcode = WAH_OP_##name##_##suffix; break;

            case WAH_OPCLASS_M: {
                uint32_t memidx = instr->imm.memarg.memidx;
                uint64_t offset = instr->imm.memarg.offset;
                WAH_LOWER_MEM_REWRITE(WAH_I32_MEM0_OPCODES_M, WAH_I64_MEM0_OPCODES_M, WAH_I64_MEM_OPCODES_M);
                WAH_LOWER_U64(offset);
                break;
            }
            case WAH_OPCLASS_MB: {
                uint32_t memidx = instr->imm.memarg_lane.memarg.memidx;
                uint64_t offset = instr->imm.memarg_lane.memarg.offset;
                uint8_t lane_idx = instr->imm.memarg_lane.lane;
                WAH_LOWER_MEM_REWRITE(WAH_I32_MEM0_OPCODES_MB, WAH_I64_MEM0_OPCODES_MB, WAH_I64_MEM_OPCODES_MB);
                WAH_LOWER_U64(offset);
                WAH_LOWER_U8(lane_idx);
                break;
            }
            default: switch (opcode) {
                case WAH_OP_IF: {
                    uint32_t false_patch_pos = buf_size;
                    WAH_LOWER_U32(0);
                    WAH_LOWER_PUSH_FRAME(WAH_OP_IF);
                    control_stack[control_sp - 1].if_false_patch = false_patch_pos;
                    break;
                }
                case WAH_OP_ELSE: {
                    WAH_ASSERT(control_sp > 0 && control_stack[control_sp - 1].opcode == WAH_OP_IF && "validation should have verified ELSE is inside IF");
                    wah_lower_cf_t *cf = &control_stack[control_sp - 1];
                    uint32_t else_end_patch = buf_size;
                    WAH_LOWER_U32(0);
                    WAH_LOWER_PATCH_U32(cf->if_false_patch, buf_size);
                    cf->opcode = WAH_OP_ELSE;
                    cf->if_false_patch = 0;
                    WAH_LOWER_ADD_PATCH_AT(cf, else_end_patch);
                    break;
                }
                case WAH_OP_BR: case WAH_OP_BR_IF:
                case WAH_OP_BR_ON_NULL: case WAH_OP_BR_ON_NON_NULL: {
                    uint32_t relative_depth = instr->imm.branch.label_idx;
                    WAH_LOWER_RESOLVE_BRANCH(relative_depth);
                    WAH_LOWER_U32(instr->imm.branch.keep);
                    WAH_LOWER_U32(instr->imm.branch.drop);
                    break;
                }
                case WAH_OP_BR_ON_CAST: case WAH_OP_BR_ON_CAST_FAIL: {
                    uint8_t flags = instr->imm.br_on_cast.cast_flags;
                    uint32_t relative_depth = instr->imm.br_on_cast.target_symbol;
                    wah_type_t ht2 = instr->imm.br_on_cast.dst_type;
                    WAH_LOWER_U8(flags);
                    WAH_LOWER_RESOLVE_BRANCH(relative_depth);
                    WAH_LOWER_U32((uint32_t)ht2);
                    WAH_LOWER_U32(instr->imm.br_on_cast.keep);
                    WAH_LOWER_U32(instr->imm.br_on_cast.drop);
                    break;
                }
                case WAH_OP_BR_TABLE: {
                    uint32_t num_targets = instr->imm.br_table.target_count;
                    WAH_LOWER_U32(num_targets);
                    WAH_LOWER_U32(instr->imm.br_table.keep);
                    for (uint32_t i = 0; i < num_targets + 1; ++i) {
                        uint32_t relative_depth;
                        if (i < num_targets) {
                            relative_depth = instr->imm.br_table.target_symbols[i];
                        } else {
                            relative_depth = instr->imm.br_table.default_symbol;
                        }
                        WAH_LOWER_RESOLVE_BRANCH(relative_depth);
                        uint32_t bt_drop = instr->imm.br_table.drops ? instr->imm.br_table.drops[i] : 0;
                        WAH_LOWER_U32(bt_drop);
                    }
                    break;
                }
                case WAH_OP_I32_CONST: {
                    WAH_LOWER_U32((uint32_t)instr->imm.i32);
                    break;
                }
                case WAH_OP_I64_CONST: {
                    WAH_LOWER_U64((uint64_t)instr->imm.i64);
                    break;
                }
                case WAH_OP_F32_CONST: {
                    WAH_LOWER_U32(instr->imm.f32_bits);
                    break;
                }
                case WAH_OP_F64_CONST: {
                    WAH_LOWER_U64(instr->imm.f64_bits);
                    break;
                }
                case WAH_OP_V128_CONST: case WAH_OP_I8X16_SHUFFLE: {
                    WAH_LOWER_MEM(instr->imm.v128, 16);
                    break;
                }
                case WAH_OP_SELECT_T: {
                    // Rewrite SELECT_T to SELECT: overwrite the opcode we just emitted
                    buf_size -= sizeof(uint16_t);
                    WAH_LOWER_U16(WAH_OP_SELECT);
                    break;
                }
                case WAH_OP_REF_FUNC: {
                    if (ac->mode == WAH_ANALYZE_CONST_EXPR) {
                        wah_write_u16_le(buf + buf_size - sizeof(uint16_t), WAH_OP_REF_FUNC_CONST);
                    }
                    WAH_LOWER_U32(instr->imm.u32);
                    break;
                }
                case WAH_OP_REF_TEST_NULL: case WAH_OP_REF_TEST:
                case WAH_OP_REF_CAST_NULL: case WAH_OP_REF_CAST: {
                    WAH_LOWER_U32((uint32_t)instr->imm.ref_cast.heap_type);
                    break;
                }
                #if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
                case WAH_OP_STRUCT_NEW: case WAH_OP_STRUCT_NEW_DEFAULT:
                case WAH_OP_ARRAY_NEW: case WAH_OP_ARRAY_NEW_DEFAULT:
                case WAH_OP_ARRAY_FILL: {
                    WAH_LOWER_U32(instr->imm.u32);
                    break;
                }
                case WAH_OP_STRUCT_GET: case WAH_OP_STRUCT_GET_S: case WAH_OP_STRUCT_GET_U: {
                    uint32_t typeidx = instr->imm.type_field.type_idx;
                    uint32_t fieldidx = instr->imm.type_field.field_idx;
                    wah_type_t ft = module->type_defs[typeidx].field_types[fieldidx];
                    const wah_repr_info_t *ri = module->repr_infos[module->typeidx_to_repr[typeidx]];
                    uint16_t new_op;
                    if (opcode == WAH_OP_STRUCT_GET_S) {
                        new_op = (ft == WAH_TYPE_PACKED_I8) ? WAH_OP_STRUCT_GET_S8 : WAH_OP_STRUCT_GET_S16;
                    } else if (opcode == WAH_OP_STRUCT_GET_U) {
                        new_op = (ft == WAH_TYPE_PACKED_I8) ? WAH_OP_STRUCT_GET_U8 : WAH_OP_STRUCT_GET_U16;
                    } else {
                        switch (ft) {
                            case WAH_TYPE_I32: case WAH_TYPE_F32: new_op = WAH_OP_STRUCT_GET_32; break;
                            case WAH_TYPE_I64: case WAH_TYPE_F64: new_op = WAH_OP_STRUCT_GET_64; break;
                            case WAH_TYPE_V128: new_op = WAH_OP_STRUCT_GET_128; break;
                            default: new_op = (sizeof(void *) >= 8) ? WAH_OP_STRUCT_GET_64 : WAH_OP_STRUCT_GET_32; break;
                        }
                    }
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_op);
                    WAH_LOWER_U32(ri->fields[fieldidx].offset);
                    break;
                }
                case WAH_OP_STRUCT_SET: {
                    uint32_t typeidx = instr->imm.type_field.type_idx;
                    uint32_t fieldidx = instr->imm.type_field.field_idx;
                    wah_type_t ft = module->type_defs[typeidx].field_types[fieldidx];
                    const wah_repr_info_t *ri = module->repr_infos[module->typeidx_to_repr[typeidx]];
                    uint16_t new_op;
                    switch (ft) {
                        case WAH_TYPE_PACKED_I8: new_op = WAH_OP_STRUCT_SET_8; break;
                        case WAH_TYPE_PACKED_I16: new_op = WAH_OP_STRUCT_SET_16; break;
                        case WAH_TYPE_I32: case WAH_TYPE_F32: new_op = WAH_OP_STRUCT_SET_32; break;
                        case WAH_TYPE_I64: case WAH_TYPE_F64: new_op = WAH_OP_STRUCT_SET_64; break;
                        case WAH_TYPE_V128: new_op = WAH_OP_STRUCT_SET_128; break;
                        default: new_op = (sizeof(void *) >= 8) ? WAH_OP_STRUCT_SET_64 : WAH_OP_STRUCT_SET_32; break;
                    }
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_op);
                    WAH_LOWER_U32(ri->fields[fieldidx].offset);
                    break;
                }
                case WAH_OP_ARRAY_GET: case WAH_OP_ARRAY_GET_S: case WAH_OP_ARRAY_GET_U: {
                    uint32_t typeidx = instr->imm.u32;
                    wah_type_t et = module->type_defs[typeidx].field_types[0];
                    uint16_t new_op;
                    if (opcode == WAH_OP_ARRAY_GET_S) {
                        new_op = (et == WAH_TYPE_PACKED_I8) ? WAH_OP_ARRAY_GET_S8 : WAH_OP_ARRAY_GET_S16;
                    } else if (opcode == WAH_OP_ARRAY_GET_U) {
                        new_op = (et == WAH_TYPE_PACKED_I8) ? WAH_OP_ARRAY_GET_U8 : WAH_OP_ARRAY_GET_U16;
                    } else {
                        switch (et) {
                            case WAH_TYPE_I32: case WAH_TYPE_F32: new_op = WAH_OP_ARRAY_GET_32; break;
                            case WAH_TYPE_I64: case WAH_TYPE_F64: new_op = WAH_OP_ARRAY_GET_64; break;
                            case WAH_TYPE_V128: new_op = WAH_OP_ARRAY_GET_128; break;
                            default: new_op = (sizeof(void *) >= 8) ? WAH_OP_ARRAY_GET_64 : WAH_OP_ARRAY_GET_32; break;
                        }
                    }
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_op);
                    break;
                }
                case WAH_OP_ARRAY_SET: {
                    uint32_t typeidx = instr->imm.u32;
                    wah_type_t et = module->type_defs[typeidx].field_types[0];
                    uint16_t new_op;
                    switch (et) {
                        case WAH_TYPE_PACKED_I8: new_op = WAH_OP_ARRAY_SET_8; break;
                        case WAH_TYPE_PACKED_I16: new_op = WAH_OP_ARRAY_SET_16; break;
                        case WAH_TYPE_I32: case WAH_TYPE_F32: new_op = WAH_OP_ARRAY_SET_32; break;
                        case WAH_TYPE_I64: case WAH_TYPE_F64: new_op = WAH_OP_ARRAY_SET_64; break;
                        case WAH_TYPE_V128: new_op = WAH_OP_ARRAY_SET_128; break;
                        default: new_op = (sizeof(void *) >= 8) ? WAH_OP_ARRAY_SET_64 : WAH_OP_ARRAY_SET_32; break;
                    }
                    wah_write_u16_le(buf + buf_size - sizeof(uint16_t), new_op);
                    break;
                }
                case WAH_OP_ARRAY_NEW_FIXED:
                case WAH_OP_ARRAY_NEW_DATA: case WAH_OP_ARRAY_NEW_ELEM: {
                    WAH_LOWER_U32(instr->imm.type_length.type_idx);
                    WAH_LOWER_U32(instr->imm.type_length.length);
                    break;
                }
                case WAH_OP_ARRAY_INIT_ELEM: {
                    WAH_LOWER_U32(instr->imm.type_length.length);
                    break;
                }
                #endif
            }
        }

        WAH_METER_RECORD_INSTR_END();

        if (emit_meter && meter_current_cost > 0) {
            switch (opcode) {
                case WAH_OP_BR: case WAH_OP_BR_IF:
                case WAH_OP_BR_ON_NULL: case WAH_OP_BR_ON_NON_NULL:
                case WAH_OP_BR_ON_CAST: case WAH_OP_BR_ON_CAST_FAIL:
                case WAH_OP_BR_TABLE:
                case WAH_OP_CALL: case WAH_OP_CALL_INDIRECT: case WAH_OP_CALL_REF:
                case WAH_OP_RETURN_CALL: case WAH_OP_RETURN_CALL_INDIRECT:
                case WAH_OP_RETURN: case WAH_OP_RETURN_CALL_REF:
                case WAH_OP_IF: case WAH_OP_ELSE:
                case WAH_OP_THROW: case WAH_OP_THROW_REF:
                WAH_IF_MEMORY64(case WAH_OP_CALL_INDIRECT_i64: case WAH_OP_RETURN_CALL_INDIRECT_i64:)
                    if (meter_current_cost >= WAH_METER_MAX_CHUNK_COST) {
                        WAH_METER_END_CHUNK();
                    }
                    break;
                default:
                    break;
            }
        }
    }

    // --- Phase 2: append slow-path islands ---
    if (emit_meter && meter_chunk_count > 0) {
        for (uint32_t ci = 0; ci < meter_chunk_count; ci++) {
            wah_meter_chunk_t *chunk = &meter_chunks[ci];
            uint32_t slow_island_start = buf_size;
            wah_write_u32_le(buf + chunk->meter_offset + sizeof(uint16_t) + sizeof(uint16_t), slow_island_start);

            uint32_t rec_start = chunk->instr_record_start;
            uint32_t rec_end = rec_start + chunk->instr_record_count;
            for (uint32_t ri = rec_start; ri < rec_end; ri++) {
                wah_meter_instr_record_t *rec = &meter_instr_records[ri];
                uint32_t tick_offset = buf_size;
                WAH_LOWER_U16(WAH_OP_TICK);
                WAH_LOWER_U32(tick_offset);
                if (rec->byte_length > 0) {
                    WAH_LOWER_ENSURE(rec->byte_length);
                    memcpy(buf + buf_size, buf + rec->fast_offset, rec->byte_length);
                    buf_size += rec->byte_length;
                }
            }

            // Emit fall-through jump to next fast chunk if last instruction can fall through
            if (chunk->instr_record_count > 0) {
                wah_meter_instr_record_t *last_rec = &meter_instr_records[rec_end - 1];
                bool needs_fallthrough = true;
                if (last_rec->byte_length >= sizeof(uint16_t)) {
                    uint16_t last_op = wah_read_u16_le(buf + last_rec->fast_offset);
                    switch (last_op) {
                        case WAH_OP_BR: case WAH_OP_RETURN:
                        case WAH_OP_RETURN_CALL: case WAH_OP_RETURN_CALL_INDIRECT: case WAH_OP_RETURN_CALL_REF:
                        case WAH_OP_BR_TABLE: case WAH_OP_UNREACHABLE:
                        case WAH_OP_THROW: case WAH_OP_THROW_REF:
                        case WAH_OP_END:
                        WAH_IF_MEMORY64(case WAH_OP_RETURN_CALL_INDIRECT_i64:)
                            needs_fallthrough = false;
                            break;
                        default: break;
                    }
                }
                if (needs_fallthrough) {
                    WAH_LOWER_U16(WAH_OP_ELSE);
                    WAH_LOWER_U32(chunk->next_fast_offset);
                }
            }
        }
    }

    #undef WAH_METER_END_CHUNK
    #undef WAH_METER_RECORD_INSTR_END
    #undef WAH_METER_RECORD_INSTR_START
    #undef WAH_METER_START_CHUNK
    #undef WAH_METER_MAX_CHUNK_COST
    #undef WAH_EMIT_POLL
    #undef WAH_LOWER_FINISH_FRAME
    #undef WAH_LOWER_PUSH_FRAME
    #undef WAH_LOWER_RESOLVE_BRANCH
    #undef WAH_LOWER_APPLY_PATCHES
    #undef WAH_LOWER_ADD_FUNC_END_PATCH
    #undef WAH_LOWER_ADD_PATCH
    #undef WAH_LOWER_ADD_PATCH_AT
    #undef WAH_LOWER_PATCH_U32
    #undef WAH_LOWER_MEM
    #undef WAH_LOWER_U64
    #undef WAH_LOWER_U32
    #undef WAH_LOWER_U16
    #undef WAH_LOWER_U8
    #undef WAH_LOWER_ENSURE

    // Transfer buffer ownership to parsed_code
    parsed_code->bytecode = buf;
    parsed_code->bytecode_size = buf_size;
    buf = NULL;

cleanup:
    wah_free(alloc, buf);
    wah_free(alloc, func_end_patches);
    wah_free(alloc, meter_chunks);
    wah_free(alloc, meter_instr_records);
    for (uint32_t i = 0; i < control_sp; i++) {
        wah_free(alloc, control_stack[i].patch_offsets);
    }
    if (err != WAH_OK && parsed_code->bytecode) {
        wah_free(alloc, parsed_code->bytecode);
        parsed_code->bytecode = NULL;
        parsed_code->bytecode_size = 0;
    }
    return err;
}

// Shared analysis loop for function bodies and const expressions.
// Decodes opcodes, validates, captures ref-map/poll (function body only),
// appends terminal END, and finalizes analyzed-code metadata.
static wah_error_t wah_analyze_stream(
    const uint8_t **code_ptr,
    const uint8_t *code_end,
    wah_validation_context_t *vctx,
    wah_code_body_t *code_body,       // NULL for const_expr
    wah_type_t expected_type,          // const_expr only: required result type
    wah_analyzed_code_t *ac
) {
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &vctx->module->alloc;
    uint8_t *ref_map = NULL;
    uint32_t ref_map_size = 0, ref_map_cap = 0;
    bool is_func_body = (code_body != NULL);

    #define WAH_CAPTURE_REF_MAP() do { \
        uint32_t depth = vctx->is_unreachable ? 0 : vctx->type_stack.sp; \
        uint32_t _words = (depth + 15) / 16; \
        uint32_t _ref_count = 0; \
        for (uint32_t _j = 0; _j < depth; _j++) { \
            if (WAH_TYPE_IS_REF(vctx->type_stack.data[_j])) _ref_count++; \
        } \
        uint32_t _entry_bytes = sizeof(uint16_t) * (1 + _words) + _ref_count * sizeof(wah_type_t); \
        WAH_ENSURE_CAP_GOTO(ref_map, ref_map_size + _entry_bytes, cleanup); \
        wah_write_u16_le(ref_map + ref_map_size, (uint16_t)depth); \
        memset(ref_map + ref_map_size + sizeof(uint16_t), 0, _words * sizeof(uint16_t)); \
        uint8_t *_type_ptr = ref_map + ref_map_size + sizeof(uint16_t) * (1 + _words); \
        for (uint32_t _j = 0; _j < depth; _j++) { \
            if (WAH_TYPE_IS_REF(vctx->type_stack.data[_j])) { \
                uint32_t _byte_off = ref_map_size + sizeof(uint16_t) + (_j / 16) * sizeof(uint16_t); \
                uint16_t _bits = wah_read_u16_le(ref_map + _byte_off); \
                _bits |= (uint16_t)(1u << (_j % 16)); \
                wah_write_u16_le(ref_map + _byte_off, _bits); \
                wah_write_u32_le(_type_ptr, (uint32_t)vctx->type_stack.data[_j]); \
                _type_ptr += sizeof(wah_type_t); \
            } \
        } \
        ref_map_size += _entry_bytes; \
    } while (0)

    bool is_first_instr = true;
    uint16_t poll_flags = 0;

    while (*code_ptr < code_end) {
        uint16_t opcode_val;
        WAH_CHECK_GOTO(wah_decode_opcode(code_ptr, code_end, &opcode_val), cleanup);
        WAH_CHECK_GOTO(wah_require_feature(vctx->module, wah_opcode_features[opcode_val]), cleanup);

        if (is_func_body && is_first_instr) {
            WAH_CAPTURE_REF_MAP();
            poll_flags = WAH_INSTR_FLAG_POLL;
            is_first_instr = false;
        }

        if (opcode_val == WAH_OP_END) {
            if (is_func_body) {
                if (vctx->control_sp == 0) {
                    for (int32_t j = vctx->func_type->result_count - 1; j >= 0; --j) {
                        WAH_CHECK_GOTO(wah_validation_pop_and_match_type(vctx, vctx->func_type->result_types[j]), cleanup);
                    }
                    WAH_ENSURE_GOTO(vctx->current_stack_depth == 0, WAH_ERROR_VALIDATION_FAILED, cleanup);
                    WAH_ENSURE_GOTO(*code_ptr == code_end, WAH_ERROR_VALIDATION_FAILED, cleanup);
                    WAH_CHECK_GOTO(wah_analyzed_append_end(ac, &vctx->module->alloc), cleanup);
                    goto done;
                }
            } else {
                WAH_ENSURE_GOTO(vctx->current_stack_depth == 1, WAH_ERROR_VALIDATION_FAILED, cleanup);
                wah_type_t actual = vctx->type_stack.data[0];
                WAH_ENSURE_GOTO(actual == WAH_TYPE_BOT || wah_type_is_subtype(actual, expected_type, vctx->module),
                    WAH_ERROR_VALIDATION_FAILED, cleanup);
                WAH_CHECK_GOTO(wah_analyzed_append_end(ac, &vctx->module->alloc), cleanup);
                goto done;
            }
        }

        if (is_func_body) {
            if (opcode_val == WAH_OP_CALL || opcode_val == WAH_OP_CALL_INDIRECT || opcode_val == WAH_OP_CALL_REF) {
                WAH_CAPTURE_REF_MAP();
                poll_flags = WAH_INSTR_FLAG_POLL;
            } else if (opcode_val == WAH_OP_RETURN_CALL || opcode_val == WAH_OP_RETURN_CALL_INDIRECT || opcode_val == WAH_OP_RETURN_CALL_REF) {
                poll_flags = WAH_INSTR_FLAG_POLL;
            }
        }

        uint32_t instr_count_before = ac->instr_count;
        WAH_CHECK_GOTO(wah_validate_opcode(opcode_val, code_ptr, code_end, vctx, code_body, ac), cleanup);

        if (is_func_body) {
            if (poll_flags && ac->instr_count > instr_count_before) {
                ac->instrs[instr_count_before].flags |= poll_flags;
                poll_flags = 0;
            }
            if (opcode_val == WAH_OP_LOOP) {
                WAH_CAPTURE_REF_MAP();
                if (ac->instr_count > 0) {
                    ac->instrs[ac->instr_count - 1].flags |= WAH_INSTR_FLAG_POLL;
                }
            }
        }
    }
    err = is_func_body ? WAH_ERROR_MALFORMED : WAH_ERROR_UNEXPECTED_EOF;
    goto cleanup;

done:
    if (is_func_body) {
        WAH_ENSURE_GOTO(vctx->control_sp == 0, WAH_ERROR_VALIDATION_FAILED, cleanup);
    }
    ac->mode = vctx->mode;
    ac->max_stack_depth = vctx->max_stack_depth;
    if (is_func_body) {
        ac->operand_ref_map = ref_map;
        ac->operand_ref_map_size = ref_map_size;
        ref_map = NULL;
    }

cleanup:
    #undef WAH_CAPTURE_REF_MAP
    if (err != WAH_OK) {
        for (uint32_t i = 0; i < vctx->control_sp; i++) {
            wah_free(alloc, vctx->control_stack[i].block_type.param_types);
            wah_free(alloc, vctx->control_stack[i].block_type.result_types);
            vctx->control_stack[i].block_type = (wah_func_type_t){0};
        }
        vctx->control_sp = 0;
    }
    wah_free(alloc, ref_map);
    return err;
}

static void wah_free_analyzed_code(wah_analyzed_code_t *ac, const wah_alloc_t *alloc) {
    if (!ac) return;
    if (ac->instrs) {
        for (uint32_t i = 0; i < ac->instr_count; i++) {
            wah_decoded_instr_t *instr = &ac->instrs[i];
            switch (instr->opcode) {
                case WAH_OP_BR_TABLE:
                    wah_free(alloc, instr->imm.br_table.target_symbols);
                    wah_free(alloc, instr->imm.br_table.drops);
                    break;
                case WAH_OP_TRY_TABLE:
                    wah_free(alloc, instr->imm.try_table.catches);
                    break;
                default:
                    break;
            }
        }
        wah_free(alloc, ac->instrs);
    }
    wah_free(alloc, ac->operand_ref_map);
    *ac = (wah_analyzed_code_t){0};
}

// Validates, analyzes, and lowers a const expression in one call.
static wah_error_t wah_compile_const_expr(
    const uint8_t **ptr,
    const uint8_t *section_end,
    wah_type_t expected_type,
    wah_module_t *module,
    uint32_t max_global_idx,
    wah_parsed_code_t *out
) {
    wah_validation_context_t vctx = { .mode = WAH_ANALYZE_CONST_EXPR, .module = module, .max_global_idx = max_global_idx };
    wah_analyzed_code_t ac = {0};
    wah_error_t err = WAH_OK;

    WAH_CHECK_GOTO(wah_analyze_stream(ptr, section_end, &vctx, NULL, expected_type, &ac), cleanup);

    if (out) {
        err = wah_lower_analyzed_code(module, &ac, out);
    }

cleanup:
    wah_free_analyzed_code(&ac, &module->alloc);
    return err;
}

static void wah_free_parsed_code(wah_parsed_code_t *parsed_code, const wah_alloc_t *alloc) {
    if (!parsed_code) return;
    wah_free(alloc, parsed_code->bytecode);
    parsed_code->bytecode = NULL;
    parsed_code->bytecode_size = 0;
    wah_free(alloc, parsed_code->operand_ref_map);
    parsed_code->operand_ref_map = NULL;
    parsed_code->operand_ref_map_size = 0;
}

static void wah_free_code_bodies(wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    if (!module->code_bodies) return;
    for (uint32_t i = 0; i < module->code_count; ++i) {
        wah_free(alloc, module->code_bodies[i].local_types);
        wah_free_parsed_code(&module->code_bodies[i].parsed_code, alloc);
    }
    wah_free(alloc, module->code_bodies);
    module->code_bodies = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Parsing /////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static wah_error_t wah_parse_name(const uint8_t **ptr, const uint8_t *section_end,
                                  char **out_name, size_t *out_len, const wah_alloc_t *alloc) {
    uint32_t name_len = 0;
    char *name_copy = NULL;

    *out_name = NULL;
    *out_len = 0;

    WAH_CHECK(wah_decode_uleb128(ptr, section_end, &name_len));
    WAH_ENSURE(*ptr + name_len <= section_end, WAH_ERROR_UNEXPECTED_EOF);
    WAH_ENSURE(wah_is_valid_utf8((const char *)*ptr, name_len), WAH_ERROR_MALFORMED);

    WAH_MALLOC_ARRAY(name_copy, (size_t)name_len + 1);
    memcpy(name_copy, *ptr, name_len);
    name_copy[name_len] = '\0';

    *ptr += name_len;
    *out_name = name_copy;
    *out_len = (size_t)name_len;
    return WAH_OK;
}

// Helper to calculate effective address with overflow and bounds checking
static inline wah_error_t wah_check_effective_addr(uint64_t addr, uint64_t offset, uint64_t size, uint64_t memory_size, uint64_t *effective_addr) {
    uint64_t ea = addr + offset;
    if (ea < addr || size > memory_size || ea > memory_size - size) return WAH_ERROR_MEMORY_OUT_OF_BOUNDS;
    *effective_addr = ea;
    return WAH_OK;
}

static inline bool wah_u64_range_in_bounds(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

// Helper to read a section header
static wah_error_t wah_read_section_header(const uint8_t **ptr, const uint8_t *end, uint8_t *id, uint32_t *size) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    *id = *(*ptr)++;
    return wah_decode_uleb128(ptr, end, size);
}

// --- Internal Section Parsing Functions ---

static wah_error_t wah_type_section_ensure_capacity(wah_module_t *module, uint32_t needed) {
    const wah_alloc_t *alloc = &module->alloc;
    if (needed <= module->types_cap) return WAH_OK;
    WAH_ENSURE_CAP(module->types, needed);
    WAH_REALLOC_ARRAY(module->type_defs, module->types_cap);
    return WAH_OK;
}

static void wah_type_section_init_slot(wah_module_t *module, uint32_t idx) {
    module->types[idx] = (wah_func_type_t){0};
    module->type_defs[idx] = (wah_type_def_t){ .kind = WAH_COMP_FUNC, .is_final = true, .supertype = WAH_NO_SUPERTYPE };
}

static wah_error_t wah_parse_func_type(const uint8_t **ptr, const uint8_t *end, wah_func_type_t *ft, const wah_alloc_t *alloc) {
    uint32_t param_count;
    WAH_CHECK(wah_decode_and_validate_count(ptr, end, &param_count, 1));
    ft->param_count = param_count;
    WAH_MALLOC_ARRAY(ft->param_types, param_count);
    for (uint32_t j = 0; j < param_count; ++j) {
        WAH_CHECK(wah_decode_val_type(ptr, end, &ft->param_types[j]));
    }

    uint32_t result_count;
    WAH_CHECK(wah_decode_and_validate_count(ptr, end, &result_count, 1));
    ft->result_count = result_count;
    WAH_MALLOC_ARRAY(ft->result_types, result_count);
    for (uint32_t j = 0; j < result_count; ++j) {
        WAH_CHECK(wah_decode_val_type(ptr, end, &ft->result_types[j]));
    }
    return WAH_OK;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_error_t wah_parse_struct_type(const uint8_t **ptr, const uint8_t *end, wah_type_def_t *td, const wah_alloc_t *alloc) {
    uint32_t field_count;
    WAH_CHECK(wah_decode_and_validate_count(ptr, end, &field_count, 2));
    td->field_count = field_count;
    WAH_MALLOC_ARRAY(td->field_types, field_count);
    WAH_MALLOC_ARRAY(td->field_mutables, field_count);
    if (field_count) {
        memset(td->field_mutables, 0, field_count * sizeof(bool));
    }
    for (uint32_t j = 0; j < field_count; ++j) {
        WAH_CHECK(wah_decode_storage_type(ptr, end, &td->field_types[j]));
        WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
        uint8_t mut = *(*ptr)++;
        WAH_ENSURE(mut <= 1, WAH_ERROR_MALFORMED);
        td->field_mutables[j] = (mut == 1);
    }
    return WAH_OK;
}

static wah_error_t wah_parse_array_type(const uint8_t **ptr, const uint8_t *end, wah_type_def_t *td, const wah_alloc_t *alloc) {
    td->field_count = 1;
    WAH_MALLOC_ARRAY(td->field_types, 1);
    WAH_MALLOC_ARRAY(td->field_mutables, 1);
    WAH_CHECK(wah_decode_storage_type(ptr, end, &td->field_types[0]));
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t mut = *(*ptr)++;
    WAH_ENSURE(mut <= 1, WAH_ERROR_MALFORMED);
    td->field_mutables[0] = (mut == 1);
    return WAH_OK;
}

#endif // WAH_FEATURE_GC

static wah_error_t wah_parse_composite_type(const uint8_t **ptr, const uint8_t *end,
                                            wah_func_type_t *ft, wah_type_def_t *td, const wah_alloc_t *alloc) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t tag = *(*ptr)++;
    switch (tag) {
        case 0x60:
            td->kind = WAH_COMP_FUNC;
            return wah_parse_func_type(ptr, end, ft, alloc);
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
        case 0x5F:
            td->kind = WAH_COMP_STRUCT;
            return wah_parse_struct_type(ptr, end, td, alloc);
        case 0x5E:
            td->kind = WAH_COMP_ARRAY;
            return wah_parse_array_type(ptr, end, td, alloc);
#else
        case 0x5F: case 0x5E:
            return WAH_ERROR_DISABLED_FEATURE;
#endif
        default:
            return WAH_ERROR_MALFORMED;
    }
}

static wah_error_t wah_parse_sub_type(const uint8_t **ptr, const uint8_t *end, uint32_t current_typeidx,
                                      wah_func_type_t *ft, wah_type_def_t *td, const wah_alloc_t *alloc) {
    WAH_ENSURE(*ptr < end, WAH_ERROR_UNEXPECTED_EOF);
    uint8_t tag = **ptr;

    if (tag == 0x50 || tag == 0x4F) {
        (*ptr)++;
        td->is_final = (tag == 0x4F);

        uint32_t super_count;
        WAH_CHECK(wah_decode_uleb128(ptr, end, &super_count));
        WAH_ENSURE(super_count <= 1, WAH_ERROR_MALFORMED);
        if (super_count == 1) {
            uint32_t super_idx;
            WAH_CHECK(wah_decode_uleb128(ptr, end, &super_idx));
            WAH_ENSURE(super_idx < current_typeidx, WAH_ERROR_VALIDATION_FAILED);
            td->supertype = super_idx;
        }
        return wah_parse_composite_type(ptr, end, ft, td, alloc);
    }

    td->is_final = true;
    td->supertype = WAH_NO_SUPERTYPE;
    return wah_parse_composite_type(ptr, end, ft, td, alloc);
}

static wah_error_t wah_parse_type_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t rec_count;
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &rec_count, 1));

    module->type_count = 0;
    module->types_cap = 0;
    module->types = NULL;
    module->type_defs = NULL;

    for (uint32_t r = 0; r < rec_count; ++r) {
        WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
        uint8_t tag = **ptr;

        if (tag == 0x4E) {
            WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_GC));
            (*ptr)++;
            uint32_t group_count;
            WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &group_count, 1));
            uint32_t group_start = module->type_count;
            for (uint32_t g = 0; g < group_count; ++g) {
                uint32_t idx = module->type_count;
                WAH_CHECK(wah_type_section_ensure_capacity(module, idx + 1));
                wah_type_section_init_slot(module, idx);
                ++module->type_count;
                WAH_CHECK(wah_parse_sub_type(ptr, section_end, idx,
                                             &module->types[idx], &module->type_defs[idx], alloc));
                if (module->type_defs[idx].kind == WAH_COMP_STRUCT || module->type_defs[idx].kind == WAH_COMP_ARRAY
                        || module->type_defs[idx].supertype != WAH_NO_SUPERTYPE || !module->type_defs[idx].is_final) {
                    WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_GC));
                }
            }
            for (uint32_t g = 0; g < group_count; ++g) {
                module->type_defs[group_start + g].rec_group_start = group_start;
                module->type_defs[group_start + g].rec_group_size = group_count;
            }
        } else {
            uint32_t idx = module->type_count;
            WAH_CHECK(wah_type_section_ensure_capacity(module, idx + 1));
            wah_type_section_init_slot(module, idx);
            ++module->type_count;
            WAH_CHECK(wah_parse_sub_type(ptr, section_end, idx,
                                         &module->types[idx], &module->type_defs[idx], alloc));
            if (module->type_defs[idx].kind == WAH_COMP_STRUCT || module->type_defs[idx].kind == WAH_COMP_ARRAY
                    || module->type_defs[idx].supertype != WAH_NO_SUPERTYPE || !module->type_defs[idx].is_final) {
                WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_GC));
            }
            module->type_defs[idx].rec_group_start = idx;
            module->type_defs[idx].rec_group_size = 1;
        }
    }

    for (uint32_t i = 0; i < module->type_count; ++i) {
        wah_type_def_t *td_scope = &module->type_defs[i];
        uint32_t scope_limit = td_scope->rec_group_start + td_scope->rec_group_size;
        #define WAH_VALIDATE_HEAP_TYPE_IDX(t) do { \
            if ((t) >= 0 && WAH_TYIDX(t) >= scope_limit) return WAH_ERROR_VALIDATION_FAILED; \
        } while (0)
        wah_func_type_t *ft = &module->types[i];
        for (uint32_t j = 0; j < ft->param_count; ++j) WAH_VALIDATE_HEAP_TYPE_IDX(ft->param_types[j]);
        for (uint32_t j = 0; j < ft->result_count; ++j) WAH_VALIDATE_HEAP_TYPE_IDX(ft->result_types[j]);
        wah_type_def_t *td = &module->type_defs[i];
        if (td->supertype != WAH_NO_SUPERTYPE) {
            WAH_ENSURE(td->supertype < i, WAH_ERROR_VALIDATION_FAILED);
            const wah_type_def_t *super_td = &module->type_defs[td->supertype];
            WAH_ENSURE(!super_td->is_final, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(td->kind == super_td->kind, WAH_ERROR_VALIDATION_FAILED);
        }
        if (td->kind == WAH_COMP_STRUCT || td->kind == WAH_COMP_ARRAY) {
            for (uint32_t j = 0; j < td->field_count; ++j) WAH_VALIDATE_HEAP_TYPE_IDX(td->field_types[j]);
        }
    }
    #undef WAH_VALIDATE_HEAP_TYPE_IDX

    return wah_module_build_type_metadata(module);
}

static wah_error_t wah_parse_function_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 1));

    module->wasm_function_count = count;
    WAH_MALLOC_ARRAY(module->function_type_indices, count);

    for (uint32_t i = 0; i < count; ++i) {
        WAH_CHECK(wah_decode_uleb128(ptr, section_end, &module->function_type_indices[i]));
        WAH_ENSURE(module->function_type_indices[i] < module->type_count, WAH_ERROR_VALIDATION_FAILED);
    }
    return WAH_OK;
}

static inline bool wah_name_matches(const char *name, size_t name_len, const char *expected, size_t expected_len) {
    return name_len == expected_len && memcmp(name, expected, name_len) == 0;
}

static bool wah_find_linked_module(
    const wah_exec_context_t *ctx,
    const wah_import_name_t *name,
    const wah_module_t **out_module,
    wah_exec_context_t **out_linked_ctx,
    uint32_t *out_linked_idx
) {
    WAH_ASSERT(ctx);
    WAH_ASSERT(name);
    if (out_module) *out_module = NULL;
    if (out_linked_ctx) *out_linked_ctx = NULL;
    if (out_linked_idx) *out_linked_idx = 0;

    for (uint32_t i = 0; i < ctx->linked_module_count; ++i) {
        const char *linked_name = ctx->linked_modules[i].name;
        if (!wah_name_matches(linked_name, strlen(linked_name), name->module, name->module_len)) continue;
        if (out_module) *out_module = ctx->linked_modules[i].module;
        if (out_linked_ctx) *out_linked_ctx = ctx->linked_modules[i].ctx;
        if (out_linked_idx) *out_linked_idx = i;
        return true;
    }

    return false;
}

static const wah_export_t *wah_find_export(const wah_module_t *module, uint8_t kind, const wah_import_name_t *name) {
    for (uint32_t i = 0; i < module->export_count; ++i) {
        const wah_export_t *exp = &module->exports[i];
        if (exp->kind != kind) continue;
        if (wah_name_matches(exp->name, exp->name_len, name->field, name->field_len)) return exp;
    }

    return NULL;
}

static wah_error_t wah_parse_local_decls(const uint8_t **ptr, const uint8_t *body_end,
                                         wah_code_body_t *body, uint32_t type_count, const wah_alloc_t *alloc) {
    uint32_t num_entries;
    WAH_CHECK(wah_decode_and_validate_count(ptr, body_end, &num_entries, 2));

    uint32_t total = 0;
    const uint8_t *scan = *ptr;
    for (uint32_t j = 0; j < num_entries; ++j) {
        uint32_t n; wah_type_t t;
        WAH_CHECK(wah_decode_uleb128(&scan, body_end, &n));
        WAH_CHECK(wah_decode_val_type(&scan, body_end, &t));
        WAH_ENSURE(UINT32_MAX - total >= n, WAH_ERROR_TOO_LARGE);
        total += n;
        WAH_ENSURE(total <= WAH_MAX_LOCAL_COUNT, WAH_ERROR_TOO_LARGE);
    }

    body->local_count = total;
    WAH_MALLOC_ARRAY(body->local_types, total);

    uint32_t idx = 0;
    for (uint32_t j = 0; j < num_entries; ++j) {
        uint32_t n; wah_type_t t;
        WAH_CHECK(wah_decode_uleb128(ptr, body_end, &n));
        WAH_CHECK(wah_decode_val_type(ptr, body_end, &t));
        WAH_ENSURE(t < 0 || WAH_TYIDX(t) < type_count, WAH_ERROR_VALIDATION_FAILED);
        for (uint32_t k = 0; k < n; ++k) {
            body->local_types[idx] = t;
            idx++;
        }
    }
    return WAH_OK;
}

static wah_error_t wah_parse_code_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &module->alloc;
    wah_validation_context_t vctx = {0};
    wah_analyzed_code_t ac = {0};

    uint32_t count;
    // A code body entry requires at least 3 bytes (body_size, num_locals, END opcode).
    WAH_CHECK_GOTO(wah_decode_and_validate_count(ptr, section_end, &count, 3), cleanup);
    WAH_ENSURE_GOTO(count == module->wasm_function_count, WAH_ERROR_MALFORMED, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->code_bodies, count, cleanup);
    module->code_count = 0;

    for (uint32_t i = 0; i < count; ++i) {
        module->code_bodies[i].local_types = NULL;
        module->code_bodies[i].parsed_code = (wah_parsed_code_t){0};
        ++module->code_count;

        uint32_t body_size;
        WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &body_size), cleanup);
        WAH_ENSURE_GOTO(body_size <= (size_t)(section_end - *ptr), WAH_ERROR_MALFORMED, cleanup);
        const uint8_t *code_body_end = *ptr + body_size;

        WAH_CHECK_GOTO(wah_parse_local_decls(ptr, code_body_end, &module->code_bodies[i], module->type_count, alloc), cleanup);

        module->code_bodies[i].code_size = (uint32_t)(code_body_end - *ptr);
        module->code_bodies[i].code = *ptr;

        const wah_func_type_t *func_type = &module->types[module->function_type_indices[i]];
        WAH_ENSURE_GOTO(UINT32_MAX - func_type->param_count >= module->code_bodies[i].local_count,
                        WAH_ERROR_TOO_LARGE, cleanup);
        WAH_ENSURE_GOTO(func_type->param_count + module->code_bodies[i].local_count <= WAH_MAX_LOCAL_COUNT,
                        WAH_ERROR_TOO_LARGE, cleanup);
        vctx = (wah_validation_context_t){
            .module = module,
            .func_type = func_type,
            .total_locals = func_type->param_count + module->code_bodies[i].local_count,
        };

        // Set up local initialization tracking for non-defaultable locals
        wah_free(alloc, vctx.local_inits); vctx.local_inits = NULL;
        wah_free(alloc, vctx.local_init_stack); vctx.local_init_stack = NULL;
        vctx.local_init_stack_used = 0;
        vctx.num_non_defaultable = 0;

        uint32_t tl = vctx.total_locals;
        uint32_t pc = func_type->param_count;
        for (uint32_t li = 0; li < module->code_bodies[i].local_count; ++li) {
            wah_type_t lt = module->code_bodies[i].local_types[li];
            if (WAH_TYPE_IS_REF(lt) && !WAH_TYPE_IS_NULLABLE(lt)) ++vctx.num_non_defaultable;
        }
        if (vctx.num_non_defaultable > 0) {
            WAH_MALLOC_ARRAY_GOTO(vctx.local_inits, tl, cleanup);
            memset(vctx.local_inits, 1, pc); // params are initialized
            memset(vctx.local_inits + pc, 0, tl - pc); // declared locals start uninitialized
            // Mark defaultable locals as always initialized
            for (uint32_t li = 0; li < module->code_bodies[i].local_count; ++li) {
                wah_type_t lt = module->code_bodies[i].local_types[li];
                if (!WAH_TYPE_IS_REF(lt) || WAH_TYPE_IS_NULLABLE(lt)) vctx.local_inits[pc + li] = 1;
            }
            WAH_MALLOC_ARRAY_GOTO(vctx.local_init_stack, (size_t)tl * WAH_MAX_CONTROL_DEPTH, cleanup);
        }

        wah_free_analyzed_code(&ac, alloc);
        ac = (wah_analyzed_code_t){0};

        const uint8_t *code_ptr = module->code_bodies[i].code;
        const uint8_t *code_end = code_ptr + module->code_bodies[i].code_size;
        WAH_CHECK_GOTO(wah_analyze_stream(&code_ptr, code_end, &vctx, &module->code_bodies[i], 0, &ac), cleanup);

        module->code_bodies[i].parsed_code.operand_ref_map = ac.operand_ref_map;
        module->code_bodies[i].parsed_code.operand_ref_map_size = ac.operand_ref_map_size;
        ac.operand_ref_map = NULL;
        module->code_bodies[i].max_stack_depth = ac.max_stack_depth;
        module->code_bodies[i].max_frame_slots = module->code_bodies[i].local_count + ac.max_stack_depth;

        WAH_CHECK_GOTO(wah_lower_analyzed_code(module, &ac, &module->code_bodies[i].parsed_code), cleanup);
        wah_free_analyzed_code(&ac, alloc);
        wah_free(alloc, vctx.local_inits);
        wah_free(alloc, vctx.local_init_stack);
        vctx.local_inits = NULL;
        vctx.local_init_stack = NULL;

        *ptr = code_body_end;
    }
    err = WAH_OK;

cleanup:
    wah_free_analyzed_code(&ac, alloc);
    wah_free(alloc, vctx.local_inits);
    wah_free(alloc, vctx.local_init_stack);
    if (err != WAH_OK) {
        for (int32_t j = vctx.control_sp - 1; j >= 0; --j) {
            wah_validation_control_frame_t* frame = &vctx.control_stack[j];
            wah_free(alloc, frame->block_type.param_types);
            wah_free(alloc, frame->block_type.result_types);
        }
        wah_free_code_bodies(module);
    }
    return err;
}

static wah_error_t wah_parse_global_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    // A global entry requires at least 3 bytes (type, is_mutable, init_expr end).
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 3));

    module->global_count = 0;
    WAH_MALLOC_ARRAY(module->globals, count);

    for (uint32_t i = 0; i < count; ++i) {
        module->globals[i] = (wah_global_t){0};
        ++module->global_count;

        wah_type_t global_declared_type;
        WAH_CHECK(wah_decode_val_type(ptr, section_end, &global_declared_type));
        module->globals[i].type = global_declared_type;

        // Mutability
        WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
        uint8_t mut_byte = *(*ptr)++;
        WAH_ENSURE(mut_byte <= 1, WAH_ERROR_MALFORMED);
        module->globals[i].is_mutable = (mut_byte == 1);

        WAH_CHECK(wah_compile_const_expr(ptr, section_end, global_declared_type, module,
                                         module->import_global_count + i, &module->globals[i].init_expr));
    }
    return WAH_OK;
}

static wah_error_t wah_parse_memory_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    // A memory entry requires at least 2 bytes (flags, min_pages_uleb128).
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 2));

    module->memory_count = count;
    if (count > 0) {
        WAH_MALLOC_ARRAY(module->memories, count);

        for (uint32_t i = 0; i < count; ++i) {
            WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
            uint8_t flags = *(*ptr)++; // Flags for memory type

            // Determine address type from flags
            if (flags == 0x00 || flags == 0x01) {
                module->memories[i].addr_type = WAH_TYPE_I32;
            } else if (flags == 0x04 || flags == 0x05) {
                WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_MEMORY64));
                module->memories[i].addr_type = WAH_TYPE_I64;
            } else {
                return WAH_ERROR_MALFORMED; // Unknown memory type flags
            }

            if (flags == 0x04 || flags == 0x05) {
                WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &module->memories[i].min_pages));
                if (flags & 0x01) {
                    WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &module->memories[i].max_pages));
                } else {
                    module->memories[i].max_pages = UINT64_MAX;
                }
            } else {
                uint64_t min64, max64;
                WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &min64));
                module->memories[i].min_pages = min64;
                if (flags & 0x01) {
                    WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &max64));
                    module->memories[i].max_pages = max64;
                } else {
                    module->memories[i].max_pages = UINT64_MAX;
                }
            }

            uint64_t page_limit = (module->memories[i].addr_type == WAH_TYPE_I32) ? 65536ULL : (1ULL << 48);
            WAH_ENSURE(module->memories[i].min_pages <= page_limit, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(module->memories[i].max_pages <= page_limit || module->memories[i].max_pages == UINT64_MAX, WAH_ERROR_VALIDATION_FAILED);
            WAH_ENSURE(module->memories[i].min_pages <= module->memories[i].max_pages, WAH_ERROR_VALIDATION_FAILED);
        }
    }
    return WAH_OK;
}

static wah_error_t wah_parse_table_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    // A table entry requires at least 3 bytes (elem_type, flags, min_elements_uleb128).
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 3));

    module->table_count = 0;
    if (count > 0) {
        WAH_MALLOC_ARRAY(module->tables, count);

        for (uint32_t i = 0; i < count; ++i) {
            module->tables[i] = (wah_table_type_t){0};
            module->table_count++;

            WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
            bool has_init_expr = (**ptr == 0x40);
            if (has_init_expr) {
                (*ptr)++;
                WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
                WAH_ENSURE(**ptr == 0x00, WAH_ERROR_MALFORMED);
                (*ptr)++;
            }

            wah_type_t elem_type;
            WAH_CHECK(wah_decode_ref_type(ptr, section_end, &elem_type));
            module->tables[i].elem_type = elem_type;

            WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
            uint8_t flags = *(*ptr)++;
            WAH_ENSURE(flags == 0x00 || flags == 0x01 || flags == 0x04 || flags == 0x05,
                       WAH_ERROR_MALFORMED);

            if (flags & 0x04) {
                WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_MEMORY64));
                module->tables[i].addr_type = WAH_TYPE_I64;
            } else {
                module->tables[i].addr_type = WAH_TYPE_I32;
            }

            if (flags & 0x04) {
                WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &module->tables[i].min_elements));
                if (flags & 0x01) {
                    WAH_CHECK(wah_decode_uleb128_64(ptr, section_end, &module->tables[i].max_elements));
                } else {
                    module->tables[i].max_elements = UINT64_MAX;
                }
            } else {
                uint32_t min32;
                WAH_CHECK(wah_decode_uleb128(ptr, section_end, &min32));
                module->tables[i].min_elements = min32;
                if (flags & 0x01) {
                    uint32_t max32;
                    WAH_CHECK(wah_decode_uleb128(ptr, section_end, &max32));
                    module->tables[i].max_elements = max32;
                } else {
                    module->tables[i].max_elements = UINT64_MAX;
                }
            }
            WAH_ENSURE(module->tables[i].min_elements <= module->tables[i].max_elements,
                       WAH_ERROR_VALIDATION_FAILED);

            if (has_init_expr) {
                WAH_CHECK(wah_compile_const_expr(ptr, section_end, elem_type, module, wah_global_index_limit(module), &module->tables[i].init_expr));
            } else if (!WAH_TYPE_IS_NULLABLE(elem_type)) {
                return WAH_ERROR_VALIDATION_FAILED;
            }
        }
    }
    return WAH_OK;
}

static wah_error_t wah_parse_tag_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    WAH_CHECK(wah_require_feature(module, WAH_FEATURE_SHIFT_EXCEPTION));
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count = 0;
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 2));
    WAH_MALLOC_ARRAY_GOTO(module->tags, count, cleanup);
    module->tag_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
        uint8_t attribute = *(*ptr)++;
        WAH_ENSURE_GOTO(attribute == 0, WAH_ERROR_MALFORMED, cleanup);
        WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &module->tags[i].type_index), cleanup);
        WAH_ENSURE_GOTO(module->tags[i].type_index < module->type_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
        WAH_ENSURE_GOTO(module->types[module->tags[i].type_index].result_count == 0, WAH_ERROR_VALIDATION_FAILED, cleanup);
    }
    return WAH_OK;
cleanup:
    wah_free(alloc, module->tags);
    module->tags = NULL;
    module->tag_count = 0;
    return err;
}

static wah_error_t wah_parse_custom_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    (void)module;
    uint32_t name_len;
    WAH_CHECK(wah_decode_uleb128(ptr, section_end, &name_len));
    WAH_ENSURE(*ptr + name_len <= section_end, WAH_ERROR_UNEXPECTED_EOF);
    WAH_ENSURE(wah_is_valid_utf8((const char *)*ptr, name_len), WAH_ERROR_MALFORMED);
    *ptr = section_end;
    return WAH_OK;
}

static wah_error_t wah_parse_import_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count = 0;
    uint32_t import_func_count = 0;
    uint32_t import_table_count = 0;
    uint32_t import_memory_count = 0;
    uint32_t import_global_count = 0;
    uint32_t import_tag_count = 0;

    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 4));

    if (count == 0) return WAH_OK;

    module->import_count = count;
    WAH_MALLOC_ARRAY_GOTO(module->imports, count, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->func_imports, count, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->table_imports, count, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->memory_imports, count, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->global_imports, count, cleanup);
    WAH_MALLOC_ARRAY_GOTO(module->tag_imports, count, cleanup);

    for (uint32_t i = 0; i < count; ++i) {
        // Parse import name (module_name + field_name)
        wah_import_name_t imp_name = {0};
        WAH_CHECK_GOTO(wah_parse_name(ptr, section_end, &imp_name.module, &imp_name.module_len, alloc), cleanup);
        err = wah_parse_name(ptr, section_end, &imp_name.field, &imp_name.field_len, alloc);
        if (err != WAH_OK) {
            wah_free(alloc, imp_name.module);
            WAH_LOG("wah_parse_name(ptr, section_end, &imp_name.field, &imp_name.field_len) failed due to: %s", wah_strerror(err));
            goto cleanup;
        }

        // kind
        if (*ptr >= section_end) {
            wah_free(alloc, imp_name.module);
            wah_free(alloc, imp_name.field);
            err = WAH_ERROR_UNEXPECTED_EOF;
            goto cleanup;
        }
        uint8_t kind = *(*ptr)++;
        module->imports[i] = (wah_import_desc_t) {
            .kind = kind,
            .module = imp_name.module, .module_len = imp_name.module_len,
            .name = imp_name.field, .name_len = imp_name.field_len,
        };

        if (kind == WAH_KIND_FUNCTION) {
            // Function import
            uint32_t type_index;
            wah_func_import_t *fi = &module->func_imports[import_func_count];
            fi->name = imp_name;
            module->imports[i].index = import_func_count;
            import_func_count++;
            WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &type_index), cleanup);
            WAH_ENSURE_GOTO(type_index < module->type_count, WAH_ERROR_VALIDATION_FAILED, cleanup);

            fi->type_index = type_index;
        } else if (kind == WAH_KIND_TABLE) {
            // Table import
            wah_table_import_t *ti = &module->table_imports[import_table_count];
            ti->name = imp_name;
            module->imports[i].index = import_table_count;
            import_table_count++;

            WAH_CHECK_GOTO(wah_decode_ref_type(ptr, section_end, &ti->type.elem_type), cleanup);
            WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
            uint8_t flags = *(*ptr)++;
            ti->type.addr_type = (flags & 0x04) ? WAH_TYPE_I64 : WAH_TYPE_I32;
            if (flags & 0x04) {
                WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &ti->type.min_elements), cleanup);
                if (flags & 0x01) {
                    WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &ti->type.max_elements), cleanup);
                } else {
                    ti->type.max_elements = UINT64_MAX;
                }
            } else {
                uint32_t min32;
                WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &min32), cleanup);
                ti->type.min_elements = min32;
                if (flags & 0x01) {
                    uint32_t max32;
                    WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &max32), cleanup);
                    ti->type.max_elements = max32;
                } else {
                    ti->type.max_elements = UINT64_MAX;
                }
            }
            WAH_ENSURE_GOTO(ti->type.min_elements <= ti->type.max_elements, WAH_ERROR_VALIDATION_FAILED, cleanup);
        } else if (kind == WAH_KIND_MEMORY) {
            // Memory import
            wah_memory_import_t *mi = &module->memory_imports[import_memory_count];
            mi->name = imp_name;
            module->imports[i].index = import_memory_count;
            import_memory_count++;

            WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
            uint8_t flags = *(*ptr)++;
            if (flags == 0x00 || flags == 0x01) {
                mi->type.addr_type = WAH_TYPE_I32;
            } else if (flags == 0x04 || flags == 0x05) {
                mi->type.addr_type = WAH_TYPE_I64;
            } else {
                err = WAH_ERROR_MALFORMED;
                goto cleanup;
            }
            if (flags == 0x04 || flags == 0x05) {
                WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &mi->type.min_pages), cleanup);
                if (flags & 0x01) {
                    WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &mi->type.max_pages), cleanup);
                } else {
                    mi->type.max_pages = UINT64_MAX;
                }
            } else {
                uint64_t min64;
                WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &min64), cleanup);
                mi->type.min_pages = min64;
                if (flags & 0x01) {
                    uint64_t max64;
                    WAH_CHECK_GOTO(wah_decode_uleb128_64(ptr, section_end, &max64), cleanup);
                    mi->type.max_pages = max64;
                } else {
                    mi->type.max_pages = UINT64_MAX;
                }
            }
            uint64_t page_limit = (mi->type.addr_type == WAH_TYPE_I32) ? 65536ULL : (1ULL << 48);
            WAH_ENSURE_GOTO(mi->type.min_pages <= page_limit, WAH_ERROR_VALIDATION_FAILED, cleanup);
            WAH_ENSURE_GOTO(mi->type.max_pages <= page_limit || mi->type.max_pages == UINT64_MAX, WAH_ERROR_VALIDATION_FAILED, cleanup);
            WAH_ENSURE_GOTO(mi->type.min_pages <= mi->type.max_pages, WAH_ERROR_VALIDATION_FAILED, cleanup);
        } else if (kind == WAH_KIND_GLOBAL) {
            // Global import
            wah_global_import_t *gi = &module->global_imports[import_global_count];
            gi->name = imp_name;
            module->imports[i].index = import_global_count;
            import_global_count++;

            WAH_CHECK_GOTO(wah_decode_val_type(ptr, section_end, &gi->type), cleanup);
            WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
            uint8_t mut_byte = *(*ptr)++;
            WAH_ENSURE_GOTO(mut_byte <= 1, WAH_ERROR_MALFORMED, cleanup);
            gi->is_mutable = (mut_byte == 1);
        } else if (kind == WAH_KIND_TAG) {
            // Tag import
            wah_tag_import_t *tgi = &module->tag_imports[import_tag_count];
            tgi->name = imp_name;
            module->imports[i].index = import_tag_count;
            import_tag_count++;

            WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
            uint8_t attribute = *(*ptr)++;
            WAH_ENSURE_GOTO(attribute == 0, WAH_ERROR_MALFORMED, cleanup);
            WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &tgi->type_index), cleanup);
            WAH_ENSURE_GOTO(tgi->type_index < module->type_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            WAH_ENSURE_GOTO(module->types[tgi->type_index].result_count == 0, WAH_ERROR_VALIDATION_FAILED, cleanup);
        } else {
            wah_free(alloc, imp_name.module);
            wah_free(alloc, imp_name.field);
            err = WAH_ERROR_MALFORMED;
            goto cleanup;
        }
    }

    module->import_function_count = import_func_count;
    module->import_table_count = import_table_count;
    module->import_memory_count = import_memory_count;
    module->import_global_count = import_global_count;
    module->import_tag_count = import_tag_count;
    return WAH_OK;

cleanup:
#define WAH_FREE_IMPORTS(arr, count) do { \
    if (arr) { \
        for (uint32_t i_ = 0; i_ < (count); i_++) { \
            wah_free(alloc, (arr)[i_].name.module); \
            wah_free(alloc, (arr)[i_].name.field); \
        } \
        wah_free(alloc, arr); \
        (arr) = NULL; \
    } \
} while(0)
    WAH_FREE_IMPORTS(module->func_imports, import_func_count);
    WAH_FREE_IMPORTS(module->table_imports, import_table_count);
    WAH_FREE_IMPORTS(module->memory_imports, import_memory_count);
    WAH_FREE_IMPORTS(module->global_imports, import_global_count);
    WAH_FREE_IMPORTS(module->tag_imports, import_tag_count);
#undef WAH_FREE_IMPORTS
    wah_free(alloc, module->imports);
    module->imports = NULL;
    module->import_count = 0;
    module->import_function_count = 0;
    module->import_table_count = 0;
    module->import_memory_count = 0;
    module->import_global_count = 0;
    module->import_tag_count = 0;
    return err;
}

static wah_error_t wah_parse_export_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count = 0;
    // An export entry requires at least 3 bytes (name_len, kind, index).
    WAH_CHECK_GOTO(wah_decode_and_validate_count(ptr, section_end, &count, 3), cleanup);

    module->export_count = 0; // Doubles as how many entries have been initialized (for cleanup)
    module->exports_cap = count;  // For parsed modules, capacity equals count
    WAH_MALLOC_ARRAY_GOTO(module->exports, count, cleanup);

    for (uint32_t i = 0; i < count; ++i) {
        wah_export_t *export_entry = &module->exports[i];
        *export_entry = (wah_export_t){0};
        ++module->export_count;

        WAH_CHECK_GOTO(wah_parse_name(ptr, section_end, (char **)&export_entry->name, &export_entry->name_len, alloc), cleanup);

        // Check for duplicate export names
        for (uint32_t j = 0; j < i; ++j) {
            if (module->exports[j].name_len == export_entry->name_len &&
                strncmp(module->exports[j].name, export_entry->name, export_entry->name_len) == 0) {
                err = WAH_ERROR_VALIDATION_FAILED; // Duplicate export name
                goto cleanup;
            }
        }

        // Export kind
        WAH_ENSURE_GOTO(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF, cleanup);
        export_entry->kind = *(*ptr)++;

        // Export index
        WAH_CHECK_GOTO(wah_decode_uleb128(ptr, section_end, &export_entry->index), cleanup);

        // Basic validation of index based on kind
        switch (export_entry->kind) {
            case WAH_KIND_FUNCTION:
                WAH_ENSURE_GOTO(export_entry->index < wah_func_index_limit(module), WAH_ERROR_VALIDATION_FAILED, cleanup);
                WAH_CHECK_GOTO(wah_declare_func(module, export_entry->index), cleanup);
                break;
            case WAH_KIND_TABLE:
                WAH_ENSURE_GOTO(export_entry->index < wah_table_index_limit(module), WAH_ERROR_VALIDATION_FAILED, cleanup);
                break;
            case WAH_KIND_MEMORY:
                WAH_ENSURE_GOTO(export_entry->index < wah_memory_index_limit(module), WAH_ERROR_VALIDATION_FAILED, cleanup);
                break;
            case WAH_KIND_GLOBAL:
                WAH_ENSURE_GOTO(export_entry->index < wah_global_index_limit(module), WAH_ERROR_VALIDATION_FAILED, cleanup);
                break;
            case WAH_KIND_TAG:
                WAH_ENSURE_GOTO(export_entry->index < module->import_tag_count + module->tag_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
                break;
            default:
                err = WAH_ERROR_MALFORMED; // Unknown export kind
                goto cleanup;
        }
    }

cleanup:
    if (err != WAH_OK) {
        if (module->exports) {
            // Free names that were already allocated
            for (uint32_t k = 0; k < module->export_count; ++k) {
                if (module->exports[k].name) {
                    wah_free(alloc, (void*)module->exports[k].name);
                }
            }
            wah_free(alloc, module->exports);
            module->exports = NULL;
            module->export_count = 0;
        }
    }
    return err;
}

static wah_error_t wah_parse_start_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    WAH_CHECK(wah_decode_uleb128(ptr, section_end, &module->start_function_idx));
    WAH_ENSURE(module->start_function_idx < wah_func_index_limit(module), WAH_ERROR_VALIDATION_FAILED);

    uint32_t type_idx;
    if (module->start_function_idx < module->import_function_count) {
        type_idx = module->func_imports[module->start_function_idx].type_index;
    } else {
        type_idx = module->function_type_indices[module->start_function_idx - module->import_function_count];
    }
    WAH_ENSURE(type_idx < module->type_count, WAH_ERROR_VALIDATION_FAILED);
    const wah_func_type_t *ft = &module->types[type_idx];
    WAH_ENSURE(ft->param_count == 0 && ft->result_count == 0, WAH_ERROR_VALIDATION_FAILED);

    module->has_start_function = true;
    return WAH_OK;
}

static wah_error_t wah_parse_element_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    // An element segment requires at least 1 byte (flags)
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 1));

    module->element_segment_count = 0;
    if (count > 0) {
        WAH_MALLOC_ARRAY(module->element_segments, count);

        for (uint32_t i = 0; i < count; ++i) {
            wah_element_segment_t *segment = &module->element_segments[i];
            *segment = (wah_element_segment_t){0};
            ++module->element_segment_count;

            uint32_t flags;
            WAH_CHECK(wah_decode_uleb128(ptr, section_end, &flags));

            uint32_t mode = flags & 3;
            bool is_expr_elem = (flags & 4) != 0;

            segment->is_active = (mode == 0 || mode == 2);
            segment->is_expr_elem = is_expr_elem;

            // Parse based on mode
            if (mode == 0) {
                // Active: table 0, offset_expr, elem* (funcref type implied)
                WAH_ENSURE(wah_table_index_limit(module) > 0, WAH_ERROR_VALIDATION_FAILED);
                segment->table_idx = 0;
            } else if (mode == 1) {
                // Passive: elemkind/reftype, elem*
                segment->is_active = false;
            } else if (mode == 2) {
                // Active: tableidx, offset_expr, elemkind/reftype, elem*
                WAH_CHECK(wah_decode_uleb128(ptr, section_end, &segment->table_idx));
                WAH_ENSURE(segment->table_idx < wah_table_index_limit(module), WAH_ERROR_VALIDATION_FAILED);
            } else { // mode == 3
                // Declarative: elemkind/reftype, elem* (dropped after validation)
                segment->is_declarative = true;
            }

            // For active modes, parse offset_expr
            if (segment->is_active) {
                wah_type_t offset_type = WAH_TYPE_I32;
                if (segment->table_idx < wah_table_index_limit(module) &&
                    wah_table_type(module, segment->table_idx)->addr_type == WAH_TYPE_I64) {
                    offset_type = WAH_TYPE_I64;
                }
                WAH_CHECK(wah_compile_const_expr(ptr, section_end, offset_type, module, wah_global_index_limit(module), &segment->offset_expr));
            }

            // Parse elemkind/reftype
            // In mode 0, elemkind/reftype is implicitly funcref (not present in binary)
            // In modes 1,2,3, we need to read the elemkind/reftype byte
            segment->elem_type = WAH_TYPE_FUNCREF;
            if (mode > 0) {
                if (is_expr_elem) {
                    WAH_CHECK(wah_decode_ref_type(ptr, section_end, &segment->elem_type));
                    WAH_ENSURE(WAH_TYPE_IS_REF(segment->elem_type), WAH_ERROR_VALIDATION_FAILED);
                } else {
                    WAH_ENSURE(*ptr < section_end, WAH_ERROR_UNEXPECTED_EOF);
                    uint8_t elemkind = *(*ptr)++;
                    WAH_ENSURE(elemkind == 0x00, WAH_ERROR_MALFORMED);
                }
            }

            // For active segments, validate element type is subtype of table element type
            if (segment->is_active) {
                const wah_table_type_t *tt = wah_table_type(module, segment->table_idx);
                WAH_CHECK(wah_validate_type_match(segment->elem_type, tt->elem_type, module));
            }

            // Parse num_elems
            uint32_t num_elems;
            WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &num_elems, 1));

            // If declarative, validate and skip elem* (no elem data is stored).
            if (segment->is_declarative) {
                for (uint32_t j = 0; j < num_elems; ++j) {
                    if (is_expr_elem) {
                        WAH_CHECK(wah_compile_const_expr(ptr, section_end, segment->elem_type, module,
                                                         wah_global_index_limit(module), NULL));
                    } else {
                        uint32_t funcidx;
                        WAH_CHECK(wah_decode_uleb128(ptr, section_end, &funcidx));
                        WAH_ENSURE(funcidx < wah_func_index_limit(module), WAH_ERROR_VALIDATION_FAILED);
                        WAH_CHECK(wah_declare_func(module, funcidx));
                    }
                }
                continue; // Don't store elem data for declarative segments
            }

            // Parse elem* based on is_expr_elem
            segment->num_elems = 0;
            if (num_elems > 0) {
                if (!is_expr_elem) {
                    // elem* are function indices
                    WAH_MALLOC_ARRAY(segment->u.func_indices, num_elems);
                    for (uint32_t j = 0; j < num_elems; ++j) {
                        ++segment->num_elems;
                        WAH_CHECK(wah_decode_uleb128(ptr, section_end, &segment->u.func_indices[j]));
                        WAH_ENSURE(segment->u.func_indices[j] < wah_func_index_limit(module), WAH_ERROR_VALIDATION_FAILED);
                        WAH_CHECK(wah_declare_func(module, segment->u.func_indices[j]));
                    }
                } else {
                    WAH_MALLOC_ARRAY(segment->u.expr.bytecodes, num_elems);
                    WAH_MALLOC_ARRAY(segment->u.expr.bytecode_sizes, num_elems);

                    for (uint32_t j = 0; j < num_elems; ++j) {
                        segment->u.expr.bytecodes[j] = NULL;
                        ++segment->num_elems;

                        wah_parsed_code_t parsed_expr = {0};
                        WAH_CHECK(wah_compile_const_expr(ptr, section_end, segment->elem_type, module, wah_global_index_limit(module), &parsed_expr));

                        segment->u.expr.bytecode_sizes[j] = parsed_expr.bytecode_size;
                        segment->u.expr.bytecodes[j] = parsed_expr.bytecode;
                    }
                }
            }
        }
    }
    return WAH_OK;
}

static wah_error_t wah_parse_data_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    const wah_alloc_t *alloc = &module->alloc;
    uint32_t count;
    // A data segment requires at least 2 bytes (flags, data_len_uleb128).
    WAH_CHECK(wah_decode_and_validate_count(ptr, section_end, &count, 2));

    // If a datacount section was present, validate that the data section count matches.
    if (module->has_data_count_section) {
        WAH_ENSURE(count == module->data_segment_count, WAH_ERROR_MALFORMED);
    }

    if (count > 0) {
        WAH_MALLOC_ARRAY(module->data_segments, count);

        module->data_segment_count = 0; // Doubles as how many entries have been initialized (for cleanup)
        for (uint32_t i = 0; i < count; ++i) {
            wah_data_segment_t *segment = &module->data_segments[i];
            *segment = (wah_data_segment_t){0};
            ++module->data_segment_count;

            WAH_CHECK(wah_decode_uleb128(ptr, section_end, &segment->flags));

            if (segment->flags == 0x00) { // Active segment, memory index 0
                segment->memory_idx = 0;
            } else if (segment->flags == 0x01) { // Passive segment
                // No memory index or offset expression for passive segments
            } else if (segment->flags == 0x02) { // Active segment, with memory index
                WAH_CHECK(wah_decode_uleb128(ptr, section_end, &segment->memory_idx));
                WAH_ENSURE(segment->memory_idx < wah_memory_index_limit(module), WAH_ERROR_VALIDATION_FAILED);
            } else {
                return WAH_ERROR_MALFORMED; // Unknown data segment flags
            }

            if (segment->flags == 0x00 || segment->flags == 0x02) { // Active segments have offset expression
                WAH_ENSURE(segment->memory_idx < wah_memory_index_limit(module), WAH_ERROR_VALIDATION_FAILED);
                wah_type_t mem_addr_type = wah_memory_type(module, segment->memory_idx)->addr_type;

                WAH_CHECK(wah_compile_const_expr(ptr, section_end, mem_addr_type, module,
                                                 wah_global_index_limit(module), &segment->offset_expr));
            }

            WAH_CHECK(wah_decode_uleb128(ptr, section_end, &segment->data_len));

            WAH_ENSURE(*ptr + segment->data_len <= section_end, WAH_ERROR_UNEXPECTED_EOF);
            if (segment->data_len > 0) {
                uint8_t *data_copy;
                WAH_MALLOC_ARRAY(data_copy, segment->data_len);
                memcpy(data_copy, *ptr, segment->data_len);
                segment->data = data_copy;
            }
            *ptr += segment->data_len;
        }
    }
    return WAH_OK;
}

static wah_error_t wah_parse_datacount_section(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module) {
    WAH_CHECK(wah_decode_uleb128(ptr, section_end, &module->data_segment_count));
    module->has_data_count_section = true;
    return WAH_OK;
}

// Global array of section handlers, indexed by the section ID
static const struct wah_section_handler_s {
    int8_t order; // Expected order of the section (0 for custom, 1 for Type, etc.)
    wah_error_t (*parser_func)(const uint8_t **ptr, const uint8_t *section_end, wah_module_t *module);
} wah_section_handlers[] = {
    [0]  = { .order = 0,  .parser_func = wah_parse_custom_section },
    [1]  = { .order = 1,  .parser_func = wah_parse_type_section },
    [2]  = { .order = 2,  .parser_func = wah_parse_import_section },
    [3]  = { .order = 3,  .parser_func = wah_parse_function_section },
    [4]  = { .order = 4,  .parser_func = wah_parse_table_section },
    [5]  = { .order = 5,  .parser_func = wah_parse_memory_section },
    [13] = { .order = 6,  .parser_func = wah_parse_tag_section },
    [6]  = { .order = 7,  .parser_func = wah_parse_global_section },
    [7]  = { .order = 8,  .parser_func = wah_parse_export_section },
    [8]  = { .order = 9,  .parser_func = wah_parse_start_section },
    [9]  = { .order = 10, .parser_func = wah_parse_element_section },
    [12] = { .order = 11, .parser_func = wah_parse_datacount_section },
    [10] = { .order = 12, .parser_func = wah_parse_code_section },
    [11] = { .order = 13, .parser_func = wah_parse_data_section },
};

wah_error_t wah_parse_module(wah_module_t *module, const uint8_t *binary, size_t binary_size, const wah_parse_options_t *options) {
    wah_error_t err = WAH_OK;
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(binary, WAH_ERROR_MISUSE);
    WAH_ENSURE(binary_size >= 8, WAH_ERROR_UNEXPECTED_EOF);
    WAH_ENSURE(wah_alloc_valid(options ? options->alloc : NULL), WAH_ERROR_MISUSE);

    *module = (wah_module_t){0}; // Initialize module struct
    module->alloc = wah_resolve_alloc(options ? options->alloc : NULL);
    const wah_alloc_t *alloc = &module->alloc;

    wah_features_t requested = options && options->features ? options->features : (WAH_DEFAULT_FEATURES);
    module->enabled_features = wah_feature_closure(requested) & (WAH_COMPILED_FEATURES);
    module->required_features = 0;
    module->fuel_metering = options && options->enable_fuel_metering;

    const uint8_t *ptr = binary;
    const uint8_t *end = binary + binary_size;

    // For section order validation
    int8_t last_parsed_order = 0; // Start with 0, as Type section is 1. Custom sections are 0 in map.

    // 1. Check Magic Number
    uint32_t magic = wah_read_u32_le(ptr);
    ptr += 4;
    WAH_ENSURE(magic == 0x6D736100, WAH_ERROR_INVALID_MAGIC_NUMBER);

    // 2. Check Version
    WAH_ENSURE(ptr + 4 <= end, WAH_ERROR_UNEXPECTED_EOF);
    uint32_t version = wah_read_u32_le(ptr);
    ptr += 4;
    WAH_ENSURE(version == 0x01, WAH_ERROR_INVALID_VERSION);

    // 3. Parse Sections
    while (ptr < end) {
        uint8_t section_id;
        uint32_t section_size;
        WAH_CHECK_GOTO(wah_read_section_header(&ptr, end, &section_id, &section_size), cleanup_parse);

        // Get section handler from lookup table
        WAH_ENSURE_GOTO(section_id < sizeof(wah_section_handlers) / sizeof(*wah_section_handlers), WAH_ERROR_MALFORMED, cleanup_parse);
        const struct wah_section_handler_s *handler = &wah_section_handlers[section_id];

        // Section order validation
        if (section_id != 0) { // Custom sections do not affect the order
            WAH_ENSURE_GOTO(handler->order > last_parsed_order, WAH_ERROR_MALFORMED, cleanup_parse);
            last_parsed_order = handler->order;
        }

        const uint8_t *section_payload_end = ptr + section_size;
        WAH_ENSURE_GOTO(section_payload_end <= end, WAH_ERROR_UNEXPECTED_EOF, cleanup_parse);

        WAH_LOG("Parsing section ID: %d, size: %u", section_id, section_size);
        WAH_CHECK_GOTO(handler->parser_func(&ptr, section_payload_end, module), cleanup_parse);

        // Ensure we consumed exactly the section_size bytes
        WAH_ENSURE_GOTO(ptr == section_payload_end, WAH_ERROR_MALFORMED, cleanup_parse);
    }

    // After all sections are parsed, validate that wasm_function_count matches code_count
    WAH_ENSURE_GOTO(module->wasm_function_count == module->code_count, WAH_ERROR_MALFORMED, cleanup_parse);

    // Validate data segment references
    if (module->min_data_segment_count_required > 0) {
        WAH_ENSURE_GOTO(module->has_data_count_section, WAH_ERROR_MALFORMED, cleanup_parse);
    }
    WAH_ENSURE_GOTO(module->data_segment_count >= module->min_data_segment_count_required, WAH_ERROR_VALIDATION_FAILED, cleanup_parse);

    // If a data count section was present, ensure data segments were actually allocated.
    if (module->has_data_count_section && module->data_segment_count > 0) {
        WAH_ENSURE_GOTO(module->data_segments != NULL, WAH_ERROR_MALFORMED, cleanup_parse);
    }

    // Validate heap type indices across all sections
    #define WAH_CHECK_TYPE_INDICES(count, t) do { \
        for (uint32_t i = 0; i < (count); ++i) { \
            int32_t _t = (int32_t)(t); \
            if (_t >= 0 && WAH_TYIDX(_t) >= module->type_count) { err = WAH_ERROR_VALIDATION_FAILED; goto cleanup_parse; } \
        } \
    } while (0)
    WAH_CHECK_TYPE_INDICES(module->global_count, module->globals[i].type);
    WAH_CHECK_TYPE_INDICES(module->table_count, module->tables[i].elem_type);
    WAH_CHECK_TYPE_INDICES(module->import_global_count, module->global_imports[i].type);
    WAH_CHECK_TYPE_INDICES(module->import_table_count, module->table_imports[i].type.elem_type);
    WAH_CHECK_TYPE_INDICES(module->element_segment_count, module->element_segments[i].elem_type);
    #undef WAH_CHECK_TYPE_INDICES

    // Feature checks.
    bool uses_v128 = false, uses_multi_value = false, uses_mut_import = false;
    for (uint32_t i = 0; i < module->type_count; ++i) {
        const wah_func_type_t *ft = &module->types[i];
        if (ft->result_count > 1) uses_multi_value = true;
        for (uint32_t j = 0; j < ft->param_count; ++j) if (ft->param_types[j] == WAH_TYPE_V128) uses_v128 = true;
        for (uint32_t j = 0; j < ft->result_count; ++j) if (ft->result_types[j] == WAH_TYPE_V128) uses_v128 = true;
    }
    for (uint32_t i = 0; i < module->global_count; ++i) {
        if (module->globals[i].type == WAH_TYPE_V128) uses_v128 = true;
    }
    for (uint32_t i = 0; i < module->import_global_count; ++i) {
        if (module->global_imports[i].type == WAH_TYPE_V128) uses_v128 = true;
        if (module->global_imports[i].is_mutable) uses_mut_import = true;
    }
    if (uses_v128) WAH_CHECK_GOTO(wah_require_feature(module, WAH_FEATURE_SHIFT_SIMD), cleanup_parse);
    if (uses_multi_value) WAH_CHECK_GOTO(wah_require_feature(module, WAH_FEATURE_SHIFT_MULTI_VALUE), cleanup_parse);
    if (uses_mut_import) WAH_CHECK_GOTO(wah_require_feature(module, WAH_FEATURE_SHIFT_MUTABLE_GLOBALS), cleanup_parse);

    // Build the unified functions[] array for the WASM functions.
    // Host functions may be appended later via wah_export_func().
    // local_idx, global_idx, and fn_module are determined entirely by the module
    // and are set here at parse time so that module->functions[] is read-only after
    // parse. This allows a single wah_module_t to be safely reused across multiple
    // wah_exec_context_t instances. fn_ctx remains 0 (filled in lazily per-instance).
    if (module->wasm_function_count > 0) {
        WAH_MALLOC_ARRAY_GOTO(module->functions, module->wasm_function_count, cleanup_parse);
        for (uint32_t i = 0; i < module->wasm_function_count; i++) {
            module->functions[i] = (wah_function_t){
                .fake_header = (wah_gc_object_t)WAH_FUNCREF_FAKE_HEADER,
                .local_idx = i,
                .global_idx = module->import_function_count + i,
                .fn_module = module,
            };
        }
        module->functions_cap = module->wasm_function_count;
    }
    module->local_function_count = module->wasm_function_count;

    return WAH_OK;

cleanup_parse:
    wah_free_module(module);
    return err;
}

// Helper to free element segment data
static void wah_free_element_segment_data(wah_element_segment_t *segment, const wah_alloc_t *alloc) {
    if (!segment->is_expr_elem) {
        wah_free(alloc, segment->u.func_indices);
        segment->u.func_indices = NULL;
    } else {
        if (segment->u.expr.bytecodes) {
            for (uint32_t i = 0; i < segment->num_elems; ++i) {
                wah_free(alloc, (void*)segment->u.expr.bytecodes[i]);
            }
            wah_free(alloc, segment->u.expr.bytecodes);
            segment->u.expr.bytecodes = NULL;
        }
        if (segment->u.expr.bytecode_sizes) {
            wah_free(alloc, segment->u.expr.bytecode_sizes);
            segment->u.expr.bytecode_sizes = NULL;
        }
    }
    wah_free_parsed_code(&segment->offset_expr, alloc);
}

////////////////////////////////////////////////////////////////////////////////
// Garbage collection //////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static inline void wah_recompute_poll_flag(wah_exec_context_t *ctx);

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)

#define WAH_GC_DEFAULT_THRESHOLD (256 * 1024)

static void wah_gc_free_all_objects(wah_exec_context_t *ctx, wah_gc_state_t *gc) {
    const wah_alloc_t *alloc = &ctx->alloc;
    wah_gc_object_t *obj = gc->all_objects;
    while (obj) {
        wah_gc_object_t *next = wah_gc_next(obj);
        wah_free(alloc, obj);
        obj = next;
    }
    gc->all_objects = NULL;
    gc->sweep_cursor = NULL;
    gc->object_count = 0;
    gc->allocated_bytes = 0;
}

wah_error_t wah_gc_start(wah_exec_context_t *ctx) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    if (ctx->gc) return WAH_OK;
    const wah_alloc_t *alloc = &ctx->alloc;
    WAH_MALLOC(ctx->gc);
    *ctx->gc = (wah_gc_state_t){ .allocation_threshold = WAH_GC_DEFAULT_THRESHOLD };
    return WAH_OK;
}

static void wah_gc_end(wah_exec_context_t *ctx) {
    if (!ctx->gc) return;
    wah_budget_release(ctx, ctx->gc->allocated_bytes);
    wah_gc_free_all_objects(ctx, ctx->gc);
    wah_free(&ctx->alloc, ctx->gc);
    ctx->gc = NULL;
}

typedef char wah_gc_align_check_[(sizeof(wah_gc_object_t) % 2 == 0) ? 1 : -1];

static void *wah_gc_alloc(wah_exec_context_t *ctx, const wah_module_t *module, wah_repr_t repr_id, uint32_t payload_size) {
    wah_gc_state_t *gc = ctx->gc;
    if (!gc) return NULL;

    if (payload_size > UINT32_MAX - sizeof(wah_gc_object_t)) return NULL;
    uint32_t total = (uint32_t)sizeof(wah_gc_object_t) + payload_size;
    if (!wah_budget_check(ctx, total)) return NULL;
    wah_gc_object_t *obj = NULL;
    if (wah_malloc(&ctx->alloc, total, 1, (void **)&obj) != WAH_OK) return NULL;
    WAH_ASSERT(((uintptr_t)obj & WAH_I31_TAG) == 0 && "malloc returned unaligned pointer");
    memset(obj, 0, total);

    obj->repr_id = repr_id;
    obj->size_bytes = total;
    obj->module = module;
    wah_gc_set_next(obj, gc->all_objects);
    gc->all_objects = obj;
    gc->object_count++;
    gc->allocated_bytes += total;
    wah_budget_charge(ctx, total);
#ifdef WAH_DEBUG
    gc->total_allocations++;
#endif

    if (gc->allocated_bytes >= gc->allocation_threshold) {
        gc->gc_pending = true;
        WAH_POLL_FLAG_STORE(ctx->poll_flag, 1);
    }

    return wah_gc_payload(obj);
}

static void *wah_gc_alloc_struct(wah_exec_context_t *ctx, const wah_module_t *module, wah_repr_t repr_id, const wah_repr_info_t *info) {
    WAH_ASSERT(info && info->type == WAH_REPR_STRUCT);
    return wah_gc_alloc(ctx, module, repr_id, info->size);
}

static void *wah_gc_alloc_array(wah_exec_context_t *ctx, const wah_module_t *module, wah_repr_t repr_id, const wah_repr_info_t *info, uint32_t length) {
    WAH_ASSERT(info && info->type == WAH_REPR_ARRAY);
    uint32_t elem_size = info->size;
    uint64_t payload64 = (uint64_t)sizeof(wah_gc_array_body_t) + (uint64_t)elem_size * length;
    if (payload64 > UINT32_MAX) return NULL;
    void *payload = wah_gc_alloc(ctx, module, repr_id, (uint32_t)payload64);
    if (payload) {
        wah_gc_array_body_t *body = (wah_gc_array_body_t *)payload;
        body->length = length;
    }
    return payload;
}

void *wah_gc_alloc_host(wah_exec_context_t *ctx, size_t size) {
    if (!ctx) return NULL;
    if (size > UINT32_MAX) return NULL;
    return wah_gc_alloc(ctx, NULL, WAH_REPR_HOST, (uint32_t)size);
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_EXCEPTION) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_exception_t *wah_gc_alloc_exception(wah_exec_context_t *ctx, uint32_t value_count) {
    uint32_t fields_size = (uint32_t)(sizeof(wah_exception_t) - sizeof(wah_gc_object_t));
    uint64_t payload64 = (uint64_t)fields_size + (uint64_t)value_count * (sizeof(wah_value_t) + sizeof(wah_type_t));
    if (payload64 > UINT32_MAX) return NULL;
    void *p = wah_gc_alloc(ctx, NULL, WAH_TYPE_EXN, (uint32_t)payload64);
    if (!p) return NULL;
    return (wah_exception_t *)wah_gc_header(p);
}
#endif

static inline void wah_gc_store_field(wah_type_t ft, uint8_t *addr, const wah_value_t *val) {
    switch (ft) {
        case WAH_TYPE_PACKED_I8:  *(uint8_t *)addr = (uint8_t)val->i32; break;
        case WAH_TYPE_PACKED_I16: *(uint16_t *)addr = (uint16_t)val->i32; break;
        case WAH_TYPE_I32: case WAH_TYPE_F32: *(uint32_t *)addr = val->i32; break;
        case WAH_TYPE_I64: case WAH_TYPE_F64: *(uint64_t *)addr = val->i64; break;
        case WAH_TYPE_V128: memcpy(addr, val, sizeof(wah_v128_t)); break;
        default:
            WAH_ASSERT(WAH_TYPE_IS_REF(ft));
            *(void **)addr = val->ref;
            break;
    }
}

static inline void wah_gc_load_field(wah_type_t ft, const uint8_t *addr, wah_value_t *val) {
    *val = (wah_value_t){0};
    switch (ft) {
        case WAH_TYPE_PACKED_I8:  val->i32 = *(uint8_t *)addr; break;
        case WAH_TYPE_PACKED_I16: val->i32 = *(uint16_t *)addr; break;
        case WAH_TYPE_I32: case WAH_TYPE_F32: val->i32 = *(int32_t *)addr; break;
        case WAH_TYPE_I64: case WAH_TYPE_F64: val->i64 = *(int64_t *)addr; break;
        case WAH_TYPE_V128: memcpy(val, addr, sizeof(wah_v128_t)); break;
        default:
            WAH_ASSERT(WAH_TYPE_IS_REF(ft));
            val->ref = *(void **)addr;
            break;
    }
}

static inline int32_t wah_gc_load_packed(wah_type_t ft, const uint8_t *addr, bool sign_extend) {
    if (ft == WAH_TYPE_PACKED_I8)
        return sign_extend ? (int32_t)(int8_t)*(uint8_t *)addr : (int32_t)*(uint8_t *)addr;
    return sign_extend ? (int32_t)(int16_t)*(uint16_t *)addr : (int32_t)*(uint16_t *)addr;
}

// Write barrier stubs.
static inline void wah_ref_store_global(wah_exec_context_t *ctx, uint32_t idx, wah_value_t val) {
    ctx->globals[idx] = val;
}
static inline void wah_ref_store_global_slot(wah_value_t *slot, wah_value_t val) {
    *slot = val;
}
static inline void wah_ref_store_table(wah_exec_context_t *ctx, uint32_t table_idx, uint64_t elem_idx, wah_value_t val) {
    ctx->tables[table_idx].entries[elem_idx] = val;
}

static void wah_gc_enumerate_roots(wah_exec_context_t *ctx, wah_gc_ref_visitor_t visitor, void *userdata) {
    if (!visitor) return;

    // 1. Parameters and locals in each call frame
    for (uint32_t d = 0; d < ctx->call_depth; d++) {
        wah_call_frame_t *frame = &WAH_FRAME(ctx, d);
        const wah_code_body_t *code = frame->code;
        if (!code) continue;
        const wah_module_t *fmod = frame->module;
        const wah_func_type_t *ftype = &fmod->types[fmod->function_type_indices[frame->func_idx]];
        // 1a. Parameters (slots [locals .. locals + param_count))
        for (uint32_t i = 0; i < ftype->param_count; i++) {
            if (WAH_TYPE_IS_REF(ftype->param_types[i])) {
                visitor(&frame->locals[i], userdata);
            }
        }
        // 1b. Declared locals (slots [locals + param_count .. + param_count + local_count))
        for (uint32_t i = 0; i < code->local_count; i++) {
            if (WAH_TYPE_IS_REF(code->local_types[i])) {
                visitor(&frame->locals[ftype->param_count + i], userdata);
            }
        }

        // 1c. Operand stack slots (using ref map from POLL points)
        wah_value_t *operand_base = frame->locals + ftype->param_count + code->local_count;
        const uint8_t *oref_map = code->parsed_code.operand_ref_map;
        uint32_t rm_byte_offset = frame->ref_map_offset;

        // If bytecode_ip points to a POLL (frame suspended at a call site),
        // read the ref map offset from the POLL operands directly.
        const uint8_t *ip = frame->bytecode_ip;
        if (ip && wah_read_u16_le(ip) == WAH_OP_POLL) {
            rm_byte_offset = wah_read_u32_le(ip + sizeof(uint16_t));
        }

        if (oref_map && rm_byte_offset < code->parsed_code.operand_ref_map_size) {
            uint16_t rm_count = wah_read_u16_le(oref_map + rm_byte_offset);
            // The ref map describes the post-POLL type stack. Clamp to the
            // actual operand stack depth to handle frames suspended mid-call
            // (where callee results haven't been pushed yet).
            wah_value_t *next_frame_base = (d < ctx->call_depth - 1) ? WAH_FRAME(ctx, d + 1).locals : ctx->sp;
            uint32_t actual_depth = (next_frame_base > operand_base)
                ? (uint32_t)(next_frame_base - operand_base) : 0;
            if (rm_count > actual_depth) rm_count = (uint16_t)actual_depth;

            const uint8_t *bits = oref_map + rm_byte_offset + sizeof(uint16_t);
            for (uint16_t i = 0; i < rm_count; i++) {
                uint16_t word = wah_read_u16_le(bits + (i / 16) * sizeof(uint16_t));
                if (word & (1u << (i % 16))) {
                    visitor(&operand_base[i], userdata);
                }
            }
        }
    }

    // 2. Globals (primary module + linked modules)
    const wah_module_t *module = ctx->module;
    uint32_t primary_globals = wah_global_index_limit(module);
    for (uint32_t i = 0; i < primary_globals; i++) {
        wah_type_t gt = wah_global_type(module, i);
        if (WAH_TYPE_IS_REF(gt)) {
            // Imported mutable globals store an indirection pointer in .ref,
            // not a GC object. Dereference to visit the actual value.
            if (i < module->import_global_count && module->global_imports[i].is_mutable) {
                visitor((wah_value_t *)ctx->globals[i].ref, userdata);
            } else {
                visitor(&ctx->globals[i], userdata);
            }
        }
    }

    uint32_t g_offset = primary_globals;
    for (uint32_t m = 0; m < ctx->linked_module_count; m++) {
        const wah_module_t *linked = ctx->linked_modules[m].module;
        for (uint32_t k = 0; k < linked->global_count; k++) {
            wah_type_t gt = linked->globals[k].type;
            if (WAH_TYPE_IS_REF(gt)) {
                visitor(&ctx->globals[g_offset + k], userdata);
            }
        }
        g_offset += linked->global_count;
    }

    // 3. Table elements
    for (uint32_t t = 0; t < ctx->table_count; t++) {
        const wah_table_type_t *tt = wah_table_type(module, t);
        if (WAH_TYPE_IS_REF(tt->elem_type)) {
            for (uint64_t e = 0; e < ctx->tables[t].size; e++) {
                visitor(&ctx->tables[t].entries[e], userdata);
            }
        }
    }

    // 4. Pending exception (in-flight between throw and catch)
    if (ctx->pending_exception) {
        wah_value_t exc_val;
        exc_val.ref = wah_exception_to_ref(ctx->pending_exception);
        visitor(&exc_val, userdata);
    }
}

static inline bool wah_gc_gray(const wah_gc_object_t *obj) {
    return ((uintptr_t)obj->next_tagged & WAH_GC_TAG_AUX) != 0;
}

static inline void wah_gc_set_gray(wah_gc_object_t *obj, bool gray) {
    if (gray)
        obj->next_tagged = (struct wah_gc_object_s *)((uintptr_t)obj->next_tagged | WAH_GC_TAG_AUX);
    else
        obj->next_tagged = (struct wah_gc_object_s *)((uintptr_t)obj->next_tagged & ~WAH_GC_TAG_AUX);
}

static void wah_gc_mark_object(wah_gc_object_t *obj, const wah_module_t *module) {
    (void)module;
    if (!obj || wah_gc_marked(obj)) return;
    wah_gc_set_mark(obj, true);
    wah_gc_set_gray(obj, true);
}

static void wah_gc_mark_ref(void *ref, const wah_module_t *module) {
    if (!ref || wah_ref_is_i31(ref)) return;
    wah_gc_mark_object(wah_gc_header(ref), module);
}

static void wah_gc_scan_object(wah_gc_object_t *obj, const wah_module_t *module) {
    wah_repr_t repr_id = obj->repr_id;
    if (repr_id == WAH_TYPE_EXN) {
        wah_exception_t *exc = (wah_exception_t *)obj;
        wah_type_t *types = wah_exception_value_types(exc);
        for (uint32_t i = 0; i < exc->value_count; i++) {
            if (!WAH_TYPE_IS_REF(types[i])) continue;
            wah_value_t v;
            wah_exception_get_value(exc, i, &v);
            if (!v.ref || wah_ref_is_i31(v.ref)) continue;
            wah_gc_mark_ref(v.ref, module);
        }
        return;
    }
    if (repr_id < 0) return;
    const wah_module_t *obj_mod = obj->module ? obj->module : module;
    const wah_repr_info_t *info = wah_repr_info_get(obj_mod, repr_id);
    if (!info) return;

    uint8_t *payload = (uint8_t *)wah_gc_payload(obj);
    if (info->type == WAH_REPR_STRUCT) {
        for (uint32_t i = 0; i < info->count; ++i) {
            if (!wah_repr_field_is_ref(&info->fields[i])) continue;
            void **ref = (void **)(payload + info->fields[i].offset);
            if (*ref) wah_gc_mark_ref(*ref, obj_mod);
        }
    } else if (info->type == WAH_REPR_ARRAY) {
        if (info->count == 0) return;
        if (!wah_repr_field_is_ref(&info->fields[0])) return;
        uint32_t elem_size = info->size;
        uint32_t *length_ptr = (uint32_t *)payload;
        uint32_t length = *length_ptr;
        uint8_t *elems = payload + sizeof(wah_gc_array_body_t);
        for (uint32_t i = 0; i < length; ++i) {
            void **ref = (void **)(elems + i * elem_size + info->fields[0].offset);
            if (*ref) wah_gc_mark_ref(*ref, obj_mod);
        }
    }
}

static void wah_gc_drain_gray(wah_gc_state_t *gc, const wah_module_t *module) {
    bool found_gray;
    do {
        found_gray = false;
        for (wah_gc_object_t *obj = gc->all_objects; obj; obj = wah_gc_next(obj)) {
            if (wah_gc_gray(obj)) {
                wah_gc_set_gray(obj, false);
                found_gray = true;
                wah_gc_scan_object(obj, module);
            }
        }
    } while (found_gray);
}

static void wah_gc_mark_visitor(wah_value_t *slot, void *userdata) {
    void *ref = slot->ref;
    if (!ref || wah_ref_is_i31(ref)) return;
    wah_gc_mark_object(wah_gc_header(ref), (const wah_module_t *)userdata);
}

static void wah_gc_step_mark(wah_exec_context_t *ctx) {
    wah_gc_state_t *gc = ctx->gc;

    // Clear all marks and gray bits
    for (wah_gc_object_t *obj = gc->all_objects; obj; obj = wah_gc_next(obj)) {
        wah_gc_set_mark(obj, false);
        wah_gc_set_gray(obj, false);
    }

    // Mark from roots (marks objects as gray)
    wah_gc_enumerate_roots(ctx, wah_gc_mark_visitor, (void *)ctx->module);

    // Iteratively scan gray objects until no more remain
    wah_gc_drain_gray(gc, ctx->module);

    gc->phase = WAH_GC_PHASE_SWEEP;
    gc->sweep_cursor = NULL;
}

static void wah_gc_step_sweep(wah_exec_context_t *ctx) {
    wah_gc_state_t *gc = ctx->gc;
    wah_gc_object_t *prev_obj = NULL;
    wah_gc_object_t *obj = gc->all_objects;
    while (obj) {
        wah_gc_object_t *next = wah_gc_next(obj);
        if (!wah_gc_marked(obj)) {
            if (prev_obj)
                wah_gc_set_next(prev_obj, next);
            else
                gc->all_objects = next;
            gc->allocated_bytes -= obj->size_bytes;
            gc->object_count--;
            wah_budget_release(ctx, obj->size_bytes);
#ifdef WAH_DEBUG
            gc->total_frees++;
#endif
            wah_free(&ctx->alloc, obj);
        } else {
            prev_obj = obj;
        }
        obj = next;
    }

    gc->phase = WAH_GC_PHASE_IDLE;
    gc->allocation_threshold = gc->allocated_bytes * 2;
    if (gc->allocation_threshold < WAH_GC_DEFAULT_THRESHOLD) {
        gc->allocation_threshold = WAH_GC_DEFAULT_THRESHOLD;
    }
}

static void wah_gc_step(wah_exec_context_t *ctx) {
    wah_gc_state_t *gc = ctx->gc;
    if (!gc) return;

    switch (gc->phase) {
        case WAH_GC_PHASE_IDLE:
            gc->phase = WAH_GC_PHASE_MARK;
#ifdef WAH_DEBUG
            gc->total_collections++;
#endif
            wah_gc_step_mark(ctx);
            wah_gc_step_sweep(ctx);
            break;
        case WAH_GC_PHASE_MARK:
            wah_gc_step_mark(ctx);
            wah_gc_step_sweep(ctx);
            break;
        case WAH_GC_PHASE_SWEEP:
            wah_gc_step_sweep(ctx);
            break;
    }
}

void wah_gc_heap_stats(const wah_exec_context_t *ctx, wah_gc_heap_stats_t *stats) {
    if (!stats) return;
    if (!ctx || !ctx->gc) {
        *stats = (wah_gc_heap_stats_t){0};
        return;
    }
    *stats = (wah_gc_heap_stats_t){
        .object_count = ctx->gc->object_count, .allocated_bytes = ctx->gc->allocated_bytes,
        .allocation_threshold = ctx->gc->allocation_threshold, .phase = ctx->gc->phase,
#ifdef WAH_DEBUG
        .total_collections = ctx->gc->total_collections, .total_allocations = ctx->gc->total_allocations,
        .total_frees = ctx->gc->total_frees, .total_polls = ctx->gc->total_polls,
#endif
    };
}

void wah_gc_heap_stats_from_host(const wah_call_context_t *ctx, wah_gc_heap_stats_t *stats) {
    WAH_ASSERT(ctx && "Call context is NULL");
    if (!ctx) {
        if (stats) *stats = (wah_gc_heap_stats_t){0};
        return;
    }
    wah_gc_heap_stats(ctx->exec, stats);
}

bool wah_gc_verify_heap(const wah_exec_context_t *ctx) {
    if (!ctx) return false;
    if (!ctx->gc) return true;
    const wah_gc_state_t *gc = ctx->gc;

    uint32_t counted = 0;
    size_t counted_bytes = 0;
    for (wah_gc_object_t *obj = gc->all_objects; obj; obj = wah_gc_next(obj)) {
        counted++;
        counted_bytes += obj->size_bytes;
        if (counted > gc->object_count + 1) {
            WAH_LOG("GC verify: object list longer than object_count (%u)", gc->object_count);
            return false;
        }
    }
    if (counted != gc->object_count) {
        WAH_LOG("GC verify: counted %u objects but object_count is %u", counted, gc->object_count);
        return false;
    }
    if (counted_bytes != gc->allocated_bytes) {
        WAH_LOG("GC verify: counted %zu bytes but allocated_bytes is %zu", counted_bytes, gc->allocated_bytes);
        return false;
    }
    return true;
}

#else // !WAH_FEATURE_GC

wah_error_t wah_gc_start(wah_exec_context_t *ctx) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    return WAH_OK;
}
static void wah_gc_end(wah_exec_context_t *ctx) { (void)ctx; }
static void wah_gc_step(wah_exec_context_t *ctx) { (void)ctx; }
void wah_gc_heap_stats(const wah_exec_context_t *ctx, wah_gc_heap_stats_t *stats) {
    (void)ctx;
    if (stats) *stats = (wah_gc_heap_stats_t){0};
}
void wah_gc_heap_stats_from_host(const wah_call_context_t *ctx, wah_gc_heap_stats_t *stats) {
    (void)ctx;
    if (stats) *stats = (wah_gc_heap_stats_t){0};
}
bool wah_gc_verify_heap(const wah_exec_context_t *ctx) { return ctx != NULL; }
void *wah_gc_alloc_host(wah_exec_context_t *ctx, size_t size) {
    (void)ctx; (void)size;
    return NULL;
}

static inline void wah_ref_store_global(wah_exec_context_t *ctx, uint32_t idx, wah_value_t val) {
    ctx->globals[idx] = val;
}
static inline void wah_ref_store_global_slot(wah_value_t *slot, wah_value_t val) {
    *slot = val;
}
static inline void wah_ref_store_table(wah_exec_context_t *ctx, uint32_t table_idx, uint64_t elem_idx, wah_value_t val) {
    ctx->tables[table_idx].entries[elem_idx] = val;
}

#endif // WAH_FEATURE_GC

////////////////////////////////////////////////////////////////////////////////
// Timer thread ////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static inline void wah_recompute_poll_flag(wah_exec_context_t *ctx) {
    int pending = WAH_POLL_FLAG_LOAD(ctx->interrupt_flag) != 0;
    pending = pending || (ctx->gc && ctx->gc->gc_pending);
    WAH_POLL_FLAG_STORE(ctx->poll_flag, pending ? 1 : 0);
}

void wah_request_interrupt(wah_exec_context_t *ctx) {
    if (!ctx) return;
    WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 1);
    WAH_POLL_FLAG_STORE(ctx->poll_flag, 1);
}

void wah_request_interrupt_from_host(wah_call_context_t *ctx) {
    if (!ctx) return;
    wah_request_interrupt(ctx->exec);
}

bool wah_is_interrupted(const wah_exec_context_t *ctx) {
    return ctx && WAH_POLL_FLAG_LOAD(ctx->interrupt_flag) != 0;
}

#ifndef WAH_NO_THREADS
typedef struct wah_timer_s {
    wah_exec_context_t *ctx;
    uint64_t deadline_us;
    wah_poll_flag_t cancelled;
    wah_poll_flag_t armed;
#if defined(_WIN32)
    HANDLE thread;
    HANDLE event;
    HANDLE timer;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
} wah_timer_t;

static void wah_timer_fire(wah_timer_t *timer) {
    wah_request_interrupt(timer->ctx);
}

#if defined(_WIN32)
static DWORD WINAPI wah_timer_main(LPVOID arg) {
    wah_timer_t *timer = (wah_timer_t *)arg;
    for (;;) {
        WaitForSingleObject(timer->event, INFINITE);
        ResetEvent(timer->event);
        if (WAH_POLL_FLAG_LOAD(timer->cancelled)) break;
        if (!WAH_POLL_FLAG_LOAD(timer->armed)) continue;

        uint64_t ticks_100ns = timer->deadline_us * 10;
        if (ticks_100ns == 0) ticks_100ns = 1;
        LARGE_INTEGER due_time;
        due_time.QuadPart = -(LONGLONG)ticks_100ns;
        if (!SetWaitableTimer(timer->timer, &due_time, 0, NULL, NULL, FALSE)) {
            WAH_POLL_FLAG_STORE(timer->armed, 0);
            wah_timer_fire(timer);
            continue;
        }

        HANDLE handles[2] = { timer->event, timer->timer };
        DWORD rc = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (WAH_POLL_FLAG_LOAD(timer->cancelled)) break;
        if (rc == WAIT_OBJECT_0) {
            CancelWaitableTimer(timer->timer);
            ResetEvent(timer->event);
            continue;
        }
        if (rc == WAIT_OBJECT_0 + 1 && WAH_POLL_FLAG_LOAD(timer->armed)) {
            WAH_POLL_FLAG_STORE(timer->armed, 0);
            wah_timer_fire(timer);
        }
    }
    return 0;
}
#else
#if defined(__APPLE__)
static uint64_t wah_timer_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

static void *wah_timer_main(void *arg) {
    wah_timer_t *timer = (wah_timer_t *)arg;
    pthread_mutex_lock(&timer->mutex);
    for (;;) {
        while (!WAH_POLL_FLAG_LOAD(timer->cancelled) && !WAH_POLL_FLAG_LOAD(timer->armed)) {
            pthread_cond_wait(&timer->cond, &timer->mutex);
        }
        if (WAH_POLL_FLAG_LOAD(timer->cancelled)) break;

#if defined(__APPLE__)
        uint64_t start_us = wah_timer_now_us();
        uint64_t deadline_us = timer->deadline_us;
        int rc = 0;
        while (!WAH_POLL_FLAG_LOAD(timer->cancelled) && WAH_POLL_FLAG_LOAD(timer->armed) && rc != ETIMEDOUT) {
            uint64_t now_us = wah_timer_now_us();
            uint64_t elapsed_us = now_us - start_us;
            if (elapsed_us >= deadline_us) {
                rc = ETIMEDOUT;
                break;
            }
            uint64_t remaining_us = deadline_us - elapsed_us;
            struct timespec rel = { .tv_sec = (time_t)(remaining_us / 1000000),
                                    .tv_nsec = (long)((remaining_us % 1000000) * 1000) };
            rc = pthread_cond_timedwait_relative_np(&timer->cond, &timer->mutex, &rel);
        }
#else
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(timer->deadline_us / 1000000);
        deadline.tv_nsec += (long)((timer->deadline_us % 1000000) * 1000);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        int rc = 0;
        while (!WAH_POLL_FLAG_LOAD(timer->cancelled) && WAH_POLL_FLAG_LOAD(timer->armed) && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&timer->cond, &timer->mutex, &deadline);
        }
#endif
        if (WAH_POLL_FLAG_LOAD(timer->cancelled)) break;
        if (WAH_POLL_FLAG_LOAD(timer->armed) && rc == ETIMEDOUT) {
            WAH_POLL_FLAG_STORE(timer->armed, 0);
            pthread_mutex_unlock(&timer->mutex);
            wah_timer_fire(timer);
            pthread_mutex_lock(&timer->mutex);
        }
    }
    pthread_mutex_unlock(&timer->mutex);
    return NULL;
}
#endif

static wah_error_t wah_new_timer(wah_exec_context_t *ctx) {
    if (ctx->timer) return WAH_OK;
    const wah_alloc_t *alloc = &ctx->alloc;
    wah_timer_t *timer = NULL;
    WAH_CHECK(wah_malloc(alloc, 1, sizeof(*timer), (void **)&timer));
    *timer = (wah_timer_t){ .ctx = ctx, .deadline_us = ctx->deadline_us };
#if defined(_WIN32)
    timer->event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!timer->event) goto cleanup;
    timer->timer = CreateWaitableTimerA(NULL, TRUE, NULL);
    if (!timer->timer) goto cleanup_event;
    timer->thread = CreateThread(NULL, 0, wah_timer_main, timer, 0, NULL);
    if (!timer->thread) goto cleanup_timer;
    ctx->timer = timer;
    return WAH_OK;
cleanup_timer:
    CloseHandle(timer->timer);
cleanup_event:
    CloseHandle(timer->event);
cleanup:
    wah_free(alloc, timer);
    return WAH_ERROR_OUT_OF_MEMORY;
#else
    pthread_condattr_t attr;
    if (pthread_mutex_init(&timer->mutex, NULL) != 0) goto cleanup;
    if (pthread_condattr_init(&attr) != 0) goto cleanup_mutex;
#if defined(CLOCK_MONOTONIC) && !defined(__APPLE__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    if (pthread_cond_init(&timer->cond, &attr) != 0) goto cleanup_attr;
    pthread_condattr_destroy(&attr);
    if (pthread_create(&timer->thread, NULL, wah_timer_main, timer) != 0) goto cleanup_cond;
    ctx->timer = timer;
    return WAH_OK;
cleanup_cond:
    pthread_cond_destroy(&timer->cond);
    goto cleanup_mutex;
cleanup_attr:
    pthread_condattr_destroy(&attr);
cleanup_mutex:
    pthread_mutex_destroy(&timer->mutex);
cleanup:
    wah_free(alloc, timer);
    return WAH_ERROR_OUT_OF_MEMORY;
#endif
}

static void wah_timer_set_armed(wah_exec_context_t *ctx, bool armed) {
    wah_timer_t *timer = ctx->timer;
    if (!timer) return;
    if (armed && ctx->deadline_us == 0) return;
#if defined(_WIN32)
    if (armed) timer->deadline_us = ctx->deadline_us;
    WAH_POLL_FLAG_STORE(timer->armed, armed ? 1 : 0);
    if (!armed) CancelWaitableTimer(timer->timer);
    SetEvent(timer->event);
#else
    pthread_mutex_lock(&timer->mutex);
    if (armed) timer->deadline_us = ctx->deadline_us;
    WAH_POLL_FLAG_STORE(timer->armed, armed ? 1 : 0);
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
#endif
}

static void wah_free_timer(wah_exec_context_t *ctx) {
    wah_timer_t *timer = ctx->timer;
    if (!timer) return;
#if defined(_WIN32)
    WAH_POLL_FLAG_STORE(timer->cancelled, 1);
    SetEvent(timer->event);
    WaitForSingleObject(timer->thread, INFINITE);
    CloseHandle(timer->thread);
    CloseHandle(timer->timer);
    CloseHandle(timer->event);
#else
    pthread_mutex_lock(&timer->mutex);
    WAH_POLL_FLAG_STORE(timer->cancelled, 1);
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->mutex);
    pthread_join(timer->thread, NULL);
    pthread_cond_destroy(&timer->cond);
    pthread_mutex_destroy(&timer->mutex);
#endif
    wah_free(&ctx->alloc, timer);
    ctx->timer = NULL;
}
#else
static wah_error_t wah_new_timer(wah_exec_context_t *ctx) { (void)ctx; return WAH_ERROR_DISABLED_FEATURE; }
static void wah_timer_set_armed(wah_exec_context_t *ctx, bool armed) { (void)ctx; (void)armed; }
static void wah_free_timer(wah_exec_context_t *ctx) { (void)ctx; }
#endif

////////////////////////////////////////////////////////////////////////////////
// Interpreter loop ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static wah_error_t wah_alloc_unified_stack(wah_exec_context_t *exec_ctx, uint64_t stack_bytes) {
    const wah_alloc_t *alloc = &exec_ctx->alloc;
    if (stack_bytes == 0) stack_bytes = WAH_DEFAULT_STACK_BYTES;
    if (stack_bytes > SIZE_MAX) return WAH_ERROR_TOO_LARGE;
    exec_ctx->stack_buffer_size = stack_bytes;
    WAH_CHECK(wah_malloc(alloc, (size_t)stack_bytes, 1, (void **)&exec_ctx->stack_buffer));
    exec_ctx->value_stack = (wah_value_t *)exec_ctx->stack_buffer;
    exec_ctx->sp = exec_ctx->value_stack;
    exec_ctx->call_depth = 0;
    // Align frame_ptr to wah_call_frame_t boundary at the top of the buffer
    uintptr_t buf_end = (uintptr_t)exec_ctx->stack_buffer + (size_t)stack_bytes;
    buf_end &= ~(uintptr_t)(WAH_ALIGNOF(wah_call_frame_t) - 1);
    exec_ctx->frame_ptr = (wah_call_frame_t *)buf_end;
    return WAH_OK;
}

wah_error_t wah_new_exec_context(wah_exec_context_t *exec_ctx, const wah_module_t *module, const wah_exec_options_t *options) {
    wah_limits_t default_limits = {0};
    const wah_limits_t *limits = options ? &options->limits : &default_limits;
    WAH_ENSURE(exec_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(wah_alloc_valid(options ? options->alloc : NULL), WAH_ERROR_MISUSE);
    *exec_ctx = (wah_exec_context_t){ .is_instantiated = false, .alloc = wah_resolve_alloc(options ? options->alloc : NULL) };
    wah_error_t err = WAH_OK;
    const wah_alloc_t *alloc = &exec_ctx->alloc;

    WAH_ENSURE(!(limits->no_memory_bytes && limits->max_memory_bytes > 0), WAH_ERROR_MISUSE);
    if (limits->no_memory_bytes) {
        exec_ctx->max_memory_bytes = 0;
    } else if (limits->max_memory_bytes > 0) {
        exec_ctx->max_memory_bytes = limits->max_memory_bytes;
    } else {
        exec_ctx->max_memory_bytes = UINT64_MAX;
    }
    exec_ctx->memory_bytes_committed = 0;

    WAH_MALLOC_ARRAY_GOTO(exec_ctx->exception_handlers, WAH_MAX_EXCEPTION_HANDLER_DEPTH, cleanup);
    memset(exec_ctx->exception_handlers, 0, sizeof(wah_exception_handler_t) * WAH_MAX_EXCEPTION_HANDLER_DEPTH);
    WAH_MALLOC_ARRAY_GOTO(exec_ctx->type_check_cache, WAH_TYPE_CHECK_CACHE_SIZE, cleanup);
    memset(exec_ctx->type_check_cache, 0, sizeof(wah_type_check_cache_entry_t) * WAH_TYPE_CHECK_CACHE_SIZE);

    WAH_CHECK_GOTO(wah_alloc_unified_stack(exec_ctx, limits->max_stack_bytes), cleanup);

    WAH_ENSURE_GOTO(module->global_count <= UINT32_MAX - module->import_global_count, WAH_ERROR_TOO_LARGE, cleanup);
    uint32_t total_globals = wah_global_index_limit(module);
    if (total_globals > 0) {
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->globals, total_globals, cleanup);
        memset(exec_ctx->globals, 0, sizeof(wah_value_t) * total_globals);
    }
    exec_ctx->global_count = total_globals;

    exec_ctx->module = module;
    exec_ctx->enabled_features = module->enabled_features;
    if (limits->fuel != 0) {
        WAH_ENSURE_GOTO(module->fuel_metering, WAH_ERROR_DISABLED_FEATURE, cleanup);
        exec_ctx->fuel = limits->fuel <= INT64_MAX ? (int64_t)limits->fuel : INT64_MAX;
    } else {
        exec_ctx->fuel = INT64_MAX;
    }
    if (limits->deadline_us > 0) {
        WAH_CHECK_GOTO(wah_new_timer(exec_ctx), cleanup);
        exec_ctx->deadline_us = limits->deadline_us;
    }

    WAH_ENSURE_GOTO(module->memory_count <= UINT32_MAX - module->import_memory_count, WAH_ERROR_TOO_LARGE, cleanup);
    uint32_t total_memories = wah_memory_index_limit(module);
    if (total_memories > 0) {
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->memories, total_memories, cleanup);
        exec_ctx->memory_count = 0;

        for (uint32_t i = 0; i < total_memories; ++i) {
            const wah_memory_type_t *mt = wah_memory_type(module, i);
            uint64_t page_limit = (mt->addr_type == WAH_TYPE_I32) ? 65536ULL : (1ULL << 48);
            uint64_t max_p = mt->max_pages;
            if (max_p > page_limit) max_p = page_limit;
            exec_ctx->memories[exec_ctx->memory_count++] = (wah_memory_inst_t){ .max_pages = max_p };
        }

        // Import memory slots are left zero-initialized; wah_instantiate() fills them in.
        // Only allocate local memories (starting at offset import_memory_count).
        for (uint32_t i = 0; i < module->memory_count; ++i) {
            uint32_t slot = module->import_memory_count + i;
            WAH_ENSURE_GOTO(module->memories[i].min_pages <= SIZE_MAX / WAH_WASM_PAGE_SIZE, WAH_ERROR_TOO_LARGE, cleanup);
            uint64_t byte_size = module->memories[i].min_pages * (uint64_t)WAH_WASM_PAGE_SIZE;
            WAH_ENSURE_GOTO(wah_budget_check(exec_ctx, byte_size), WAH_ERROR_TOO_LARGE, cleanup);
            exec_ctx->memories[slot].size = byte_size;
            if (byte_size > 0) {
                WAH_MALLOC_ARRAY_GOTO(exec_ctx->memories[slot].data, byte_size, cleanup);
                memset(exec_ctx->memories[slot].data, 0, byte_size);
            }
            wah_budget_charge(exec_ctx, byte_size);
        }
        if (exec_ctx->memory_count > 0) {
            exec_ctx->memory_base = exec_ctx->memories[0].data;
            exec_ctx->memory_size = exec_ctx->memories[0].size;
        }
    }

    WAH_ENSURE_GOTO(module->table_count <= UINT32_MAX - module->import_table_count, WAH_ERROR_TOO_LARGE, cleanup);
    uint32_t total_tables = wah_table_index_limit(module);
    if (total_tables > 0) {
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->tables, total_tables, cleanup);
        exec_ctx->table_count = 0;

        // Import table slots are left zero-initialized; wah_instantiate() fills them in.
        for (uint32_t i = 0; i < module->import_table_count; ++i) {
            exec_ctx->tables[exec_ctx->table_count++] = (wah_table_inst_t){0};
        }
        // Only allocate local tables (starting at offset import_table_count).
        for (uint32_t i = 0; i < module->table_count; ++i) {
            uint32_t slot = exec_ctx->table_count;
            uint64_t min_elements = module->tables[i].min_elements;
            uint64_t table_bytes = 0;
            WAH_CHECK_GOTO(wah_table_byte_size(min_elements, &table_bytes), cleanup);
            WAH_ENSURE_GOTO(wah_budget_check(exec_ctx, table_bytes), WAH_ERROR_TOO_LARGE, cleanup);
            exec_ctx->tables[slot] = (wah_table_inst_t){ .size = min_elements, .max_size = module->tables[i].max_elements };
            if (min_elements > 0) {
                WAH_MALLOC_ARRAY_GOTO(exec_ctx->tables[slot].entries, min_elements, cleanup);
                memset(exec_ctx->tables[slot].entries, 0, (size_t)table_bytes);
            }
            wah_budget_charge(exec_ctx, table_bytes);
            exec_ctx->table_count++;
        }
    }

    // Build the runtime tag_instances (global tag index space: imports + locals).
    WAH_ENSURE_GOTO(module->tag_count <= UINT32_MAX - module->import_tag_count, WAH_ERROR_TOO_LARGE, cleanup);
    uint32_t total_tags = module->import_tag_count + module->tag_count;
    if (total_tags > 0) {
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->tag_instances, total_tags, cleanup);
        exec_ctx->tag_instance_count = 0;
        for (uint32_t i = 0; i < module->import_tag_count; i++) {
            exec_ctx->tag_instances[exec_ctx->tag_instance_count++] =
                (wah_tag_instance_t){ .type_index = module->tag_imports[i].type_index };
        }
        for (uint32_t i = 0; i < module->tag_count; i++) {
            uint32_t slot = exec_ctx->tag_instance_count++;
            exec_ctx->tag_instances[slot] =
                (wah_tag_instance_t){ .type_index = module->tags[i].type_index, .identity = &exec_ctx->tag_instances[slot] };
        }
    }

    // Build the runtime function_table (global index space: imports + locals + hosts).
    // Import slots are zero-initialized here; wah_instantiate() fills them in.
    // Local/host slots are shallow copies of module->functions[]; module->functions[]
    // is fully initialized at parse time (local_idx, global_idx, fn_module), so the
    // copy needs only one per-instance fixup: fn_ctx for non-host wasm functions, so
    // funcrefs into this table dispatch correctly even when handled by a third-party
    // context that has not directly linked us.
    uint32_t import_count = module->import_function_count;
    WAH_ENSURE_GOTO(module->local_function_count <= UINT32_MAX - import_count, WAH_ERROR_TOO_LARGE, cleanup);
    uint32_t table_size = import_count + module->local_function_count;
    exec_ctx->function_table_count = table_size;
    if (table_size > 0) {
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->function_table, table_size, cleanup);
        // Initialize import slots (will be filled during import resolution)
        for (uint32_t i = 0; i < import_count; i++) {
            exec_ctx->function_table[i] = (wah_function_t){ .fake_header = (wah_gc_object_t)WAH_FUNCREF_FAKE_HEADER };
        }
        // Copy local/host functions at offset import_count
        for (uint32_t i = 0; i < module->local_function_count; i++) {
            exec_ctx->function_table[import_count + i] = module->functions[i];
            if (!module->functions[i].is_host) {
                exec_ctx->function_table[import_count + i].fn_ctx = exec_ctx;
            }
        }
    }

    // Per-instance segment-dropped bitsets. Pre-mark declarative element segments
    // as dropped so runtime readers don't need to check is_declarative separately.
    if (module->element_segment_count > 0) {
        size_t bytes = (module->element_segment_count + 7) / 8;
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->dropped_elem_segments, bytes, cleanup);
        memset(exec_ctx->dropped_elem_segments, 0, bytes);
        for (uint32_t i = 0; i < module->element_segment_count; ++i) {
            if (module->element_segments[i].is_declarative) {
                wah_elem_seg_mark_dropped(exec_ctx, i);
            }
        }
    }
    if (module->data_segment_count > 0) {
        size_t bytes = (module->data_segment_count + 7) / 8;
        WAH_MALLOC_ARRAY_GOTO(exec_ctx->dropped_data_segments, bytes, cleanup);
        memset(exec_ctx->dropped_data_segments, 0, bytes);
    }

    return WAH_OK;

cleanup:
    if (err != WAH_OK) wah_free_exec_context(exec_ctx);
    return err;
}

wah_error_t wah_set_limits(wah_exec_context_t *exec_ctx, const wah_limits_t *limits) {
    WAH_ENSURE(exec_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(limits, WAH_ERROR_MISUSE);
    const wah_alloc_t *alloc = &exec_ctx->alloc;
    WAH_ENSURE(exec_ctx->lifecycle.state == WAH_EXEC_READY, WAH_ERROR_MISUSE);
    WAH_ENSURE(exec_ctx->call_depth == 0 && exec_ctx->sp == exec_ctx->value_stack, WAH_ERROR_MISUSE);

    if (limits->max_stack_bytes != 0 && limits->max_stack_bytes != exec_ctx->stack_buffer_size) {
        uint64_t new_size = limits->max_stack_bytes;
        if (new_size > SIZE_MAX) return WAH_ERROR_TOO_LARGE;
        uint8_t *new_buf = NULL;
        WAH_CHECK(wah_malloc(alloc, (size_t)new_size, 1, (void **)&new_buf));
        wah_free(alloc, exec_ctx->stack_buffer);
        exec_ctx->stack_buffer = new_buf;
        exec_ctx->stack_buffer_size = new_size;
        exec_ctx->value_stack = (wah_value_t *)new_buf;
        exec_ctx->sp = exec_ctx->value_stack;
        uintptr_t buf_end = (uintptr_t)new_buf + (size_t)new_size;
        buf_end &= ~(uintptr_t)(WAH_ALIGNOF(wah_call_frame_t) - 1);
        exec_ctx->frame_ptr = (wah_call_frame_t *)buf_end;
    }

    WAH_ENSURE(!(limits->no_memory_bytes && limits->max_memory_bytes > 0), WAH_ERROR_MISUSE);
    if (limits->no_memory_bytes || limits->max_memory_bytes > 0) {
        uint64_t new_max_mem = limits->no_memory_bytes ? 0 : limits->max_memory_bytes;
        if (new_max_mem < exec_ctx->memory_bytes_committed) {
            return WAH_ERROR_TOO_LARGE;
        }
        exec_ctx->max_memory_bytes = new_max_mem;
    }

    if (limits->fuel != 0) {
        WAH_ENSURE(exec_ctx->module->fuel_metering, WAH_ERROR_DISABLED_FEATURE);
        exec_ctx->fuel = limits->fuel <= INT64_MAX ? (int64_t)limits->fuel : INT64_MAX;
    }

    if (limits->deadline_us != 0) {
        WAH_CHECK(wah_new_timer(exec_ctx));
        exec_ctx->deadline_us = limits->deadline_us;
    }

    return WAH_OK;
}

void wah_get_limits(const wah_exec_context_t *exec_ctx, wah_limits_t *out) {
    if (!out) return;
    if (!exec_ctx) {
        *out = (wah_limits_t){0};
        return;
    }
    uint64_t mm = exec_ctx->max_memory_bytes;
    *out = (wah_limits_t){
        .max_stack_bytes = exec_ctx->stack_buffer_size,
        .max_memory_bytes = (mm == UINT64_MAX) ? 0 : mm,
        .no_memory_bytes = (mm == 0),
        .fuel = (exec_ctx->fuel >= 0 && exec_ctx->fuel < INT64_MAX) ? (uint64_t)exec_ctx->fuel : 0,
        .deadline_us = exec_ctx->deadline_us,
    };
}

void wah_free_exec_context(wah_exec_context_t *exec_ctx) {
    if (!exec_ctx) return;
    wah_alloc_t alloc_storage = wah_resolve_alloc(&exec_ctx->alloc);
    const wah_alloc_t *alloc = &alloc_storage;
    wah_free_timer(exec_ctx);
    exec_ctx->lifecycle = (struct wah_exec_lifecycle_s){0};
    wah_free(alloc, exec_ctx->stack_buffer);
    wah_free(alloc, exec_ctx->exception_handlers);
    wah_free(alloc, exec_ctx->type_check_cache);
    wah_free(alloc, exec_ctx->dropped_elem_segments);
    wah_free(alloc, exec_ctx->dropped_data_segments);
    wah_free(alloc, exec_ctx->globals);
    if (exec_ctx->memories) {
        for (uint32_t i = 0; i < exec_ctx->memory_count; ++i) {
            if (!exec_ctx->memories[i].is_imported) {
                wah_free(alloc, exec_ctx->memories[i].data);
            }
        }
        wah_free(alloc, exec_ctx->memories);
    }
    wah_free(alloc, exec_ctx->function_table);
    if (exec_ctx->tables) {
        for (uint32_t i = 0; i < exec_ctx->table_count; ++i) {
            if (!exec_ctx->tables[i].is_imported) {
                wah_free(alloc, exec_ctx->tables[i].entries);
            }
        }
        wah_free(alloc, exec_ctx->tables);
    }

    wah_free(alloc, exec_ctx->tag_instances);

    // Free linked modules
    if (exec_ctx->linked_modules) {
        for (uint32_t i = 0; i < exec_ctx->linked_module_count; ++i) {
            wah_free(alloc, (void*)exec_ctx->linked_modules[i].name);
            if (exec_ctx->linked_modules[i].owns_ctx) {
                wah_free(alloc, exec_ctx->linked_modules[i].ctx->tag_instances);
                wah_free(alloc, exec_ctx->linked_modules[i].ctx->dropped_elem_segments);
                wah_free(alloc, exec_ctx->linked_modules[i].ctx->dropped_data_segments);
                wah_free(alloc, exec_ctx->linked_modules[i].ctx);
            }
        }
        wah_free(alloc, exec_ctx->linked_modules);
    }

    wah_gc_end(exec_ctx);

    *exec_ctx = (wah_exec_context_t){0};
}

wah_error_t wah_set_fuel(wah_exec_context_t *ctx, int64_t fuel) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->module, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->module->fuel_metering, WAH_ERROR_DISABLED_FEATURE);
    ctx->fuel = fuel;
    return WAH_OK;
}

int64_t wah_get_fuel(const wah_exec_context_t *ctx) {
    if (!ctx) return 0;
    return ctx->fuel;
}

static wah_error_t wah_poll_handler(wah_exec_context_t *ctx) {
    wah_gc_state_t *gc = ctx->gc;

#ifdef WAH_DEBUG
    if (gc) gc->total_polls++;
#endif

    if (WAH_POLL_FLAG_LOAD(ctx->interrupt_flag)) {
        WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 0);
        wah_recompute_poll_flag(ctx);
        return WAH_STATUS_YIELDED;
    }

    if (!gc) {
        wah_recompute_poll_flag(ctx);
        return WAH_OK;
    }

    if (gc->gc_pending) {
        gc->gc_pending = false;
        wah_gc_step(ctx);
    }

    wah_recompute_poll_flag(ctx);
    return WAH_OK;
}

static inline bool wah_bulk_should_interrupt(const wah_exec_context_t *ctx) {
    return WAH_POLL_FLAG_LOAD(ctx->interrupt_flag) != 0;
}

static inline uint64_t wah_bulk_fuel_limit(const wah_exec_context_t *ctx, uint64_t chunk) {
    if (!ctx->module->fuel_metering) return chunk;
    if (ctx->fuel < 0) return 0;
    uint64_t fuel_u = (uint64_t)ctx->fuel + 1;
    if (fuel_u > UINT64_MAX / WAH_BULK_ITEMS_PER_FUEL) return chunk;
    uint64_t affordable = fuel_u * WAH_BULK_ITEMS_PER_FUEL;
    return chunk < affordable ? chunk : affordable;
}

static inline void wah_bulk_fuel_charge(wah_exec_context_t *ctx, uint64_t chunk) {
    if (ctx->module->fuel_metering && chunk > 0)
        ctx->fuel -= (int64_t)((chunk + WAH_BULK_ITEMS_PER_FUEL - 1) / WAH_BULK_ITEMS_PER_FUEL);
}

static inline bool wah_bulk_should_stop(const wah_exec_context_t *ctx) {
    return WAH_POLL_FLAG_LOAD(ctx->interrupt_flag) != 0 ||
           (ctx->module->fuel_metering && ctx->fuel < 0);
}

// Yield from a bulk op: rewind IP to instruction start, return appropriate status.
// Caller must have already pushed remaining args onto sp before this.
#define WAH_BULK_YIELD(instr_start) do { \
    if (ctx->module->fuel_metering && ctx->fuel < 0) { \
        err = WAH_STATUS_FUEL_EXHAUSTED; \
    } else { \
        WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 0); \
        wah_recompute_poll_flag(ctx); \
        err = WAH_STATUS_YIELDED; \
    } \
    bytecode_ip = (instr_start); \
    goto cleanup; \
} while (0)

// Check for interrupt or fuel exhaustion at the end of a bulk-op chunk.
#define WAH_BULK_CHECK(done, total) \
    ((done) < (total) && \
     ((((done) & (WAH_BULK_CHECK_INTERVAL - 1)) == 0 && wah_bulk_should_interrupt(ctx)) || \
      (ctx->module->fuel_metering && ctx->fuel < 0)))

// Pushes a new call frame. This is an internal helper.
// local_idx: index into fn_module->code_bodies[] for the function body.
// result_count: number of return values (stored in frame for RETURN/END).
// fn_ctx: owning exec context of the function (NULL means use linked_modules lookup).
// value_top: actual top of the value stack region (may differ from ctx->sp during interpretation).
static inline wah_error_t wah_bind_frame_module(
    wah_exec_context_t *ctx, wah_call_frame_t *frame,
    const wah_module_t *fn_module, wah_exec_context_t *fn_ctx
) {
    if (fn_module == ctx->module) {
        frame->frame_globals = ctx->globals;
        frame->frame_function_table = ctx->function_table;
        frame->frame_function_table_count = ctx->function_table_count;
        frame->frame_ctx = ctx;
    } else if (fn_ctx && fn_ctx != ctx) {
        frame->frame_globals = fn_ctx->globals;
        frame->frame_function_table = fn_ctx->function_table;
        frame->frame_function_table_count = fn_ctx->function_table_count;
        frame->frame_ctx = fn_ctx;
    } else {
        uint32_t offset = wah_global_index_limit(ctx->module);
        bool found = false;
        for (uint32_t i = 0; i < ctx->linked_module_count; i++) {
            if (ctx->linked_modules[i].module == fn_module) {
                if (ctx->linked_modules[i].ctx) {
                    frame->frame_globals = ctx->linked_modules[i].ctx->globals;
                    frame->frame_function_table = ctx->linked_modules[i].ctx->function_table;
                    frame->frame_function_table_count = ctx->linked_modules[i].ctx->function_table_count;
                    frame->frame_ctx = ctx->linked_modules[i].ctx;
                } else {
                    frame->frame_globals = offset ? ctx->globals + offset : ctx->globals;
                    frame->frame_function_table = ctx->function_table;
                    frame->frame_function_table_count = ctx->function_table_count;
                    frame->frame_ctx = ctx;
                }
                found = true;
                break;
            }
            offset += ctx->linked_modules[i].module->global_count;
        }
        if (!found) {
            for (uint32_t i = 0; i < ctx->linked_module_count && !found; i++) {
                wah_exec_context_t *lctx = ctx->linked_modules[i].ctx;
                if (!lctx) continue;
                uint32_t loffset = wah_global_index_limit(lctx->module);
                for (uint32_t j = 0; j < lctx->linked_module_count; j++) {
                    if (lctx->linked_modules[j].module == fn_module) {
                        if (lctx->linked_modules[j].ctx) {
                            frame->frame_globals = lctx->linked_modules[j].ctx->globals;
                            frame->frame_function_table = lctx->linked_modules[j].ctx->function_table;
                            frame->frame_function_table_count = lctx->linked_modules[j].ctx->function_table_count;
                            frame->frame_ctx = lctx->linked_modules[j].ctx;
                        } else {
                            frame->frame_globals = lctx->globals + loffset;
                            frame->frame_function_table = lctx->function_table;
                            frame->frame_function_table_count = lctx->function_table_count;
                            frame->frame_ctx = lctx;
                        }
                        found = true;
                        break;
                    }
                    loffset += lctx->linked_modules[j].module->global_count;
                }
            }
        }
        WAH_ENSURE(found, WAH_ERROR_TRAP);
    }
    return WAH_OK;
}

static inline wah_error_t wah_init_wasm_frame(
    wah_exec_context_t *ctx, wah_call_frame_t *frame,
    const wah_module_t *fn_module, uint32_t local_idx, const wah_code_body_t *code_body,
    wah_value_t *locals, uint32_t result_count, wah_exec_context_t *fn_ctx
) {
    frame->code = code_body;
    frame->bytecode_ip = code_body->parsed_code.bytecode;
    frame->locals = locals;
    frame->func_idx = local_idx;
    frame->result_count = result_count;
    frame->module = fn_module;
    frame->ref_map_offset = 0;
    return wah_bind_frame_module(ctx, frame, fn_module, fn_ctx);
}

static wah_error_t wah_push_frame(
    wah_exec_context_t *ctx, const wah_module_t *fn_module, uint32_t local_idx, wah_value_t *locals,
    uint32_t result_count, wah_exec_context_t *fn_ctx, const wah_value_t *value_top
) {
    wah_call_frame_t *new_frame_ptr = ctx->frame_ptr - 1;
    WAH_ENSURE((const uint8_t *)value_top <= (const uint8_t *)new_frame_ptr, WAH_ERROR_STACK_OVERFLOW);

    const wah_code_body_t *code_body = &fn_module->code_bodies[local_idx];
    ctx->frame_ptr = new_frame_ptr;
    ctx->call_depth++;
    wah_call_frame_t *frame = new_frame_ptr;

    return wah_init_wasm_frame(ctx, frame, fn_module, local_idx, code_body, locals, result_count, fn_ctx);
}

#define RELOAD_FRAME() \
    do { \
        if (ctx->call_depth == 0) goto cleanup; \
        frame = ctx->frame_ptr; \
        bytecode_ip = frame->bytecode_ip; \
        bytecode_base = frame->code->parsed_code.bytecode; \
        fctx = frame->frame_ctx; \
    } while (0)

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_EXCEPTION) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_error_t wah_throw_exception(wah_exec_context_t *ctx, wah_exception_t *exc) {
    ctx->pending_exception = exc;

    while (ctx->exception_handler_depth > 0) {
        wah_exception_handler_t *handler = &ctx->exception_handlers[ctx->exception_handler_depth - 1];

        while (ctx->call_depth > handler->call_depth) {
            ctx->call_depth--;
            ctx->frame_ptr++;
        }

        const uint8_t *catch_ptr = handler->catch_table;
        for (uint32_t ci = 0; ci < handler->catch_count; ci++) {
            uint8_t catch_kind = *catch_ptr++;
            uint32_t catch_tag_idx = wah_read_u32_le(catch_ptr); catch_ptr += sizeof(uint32_t);
            uint32_t catch_offset = wah_read_u32_le(catch_ptr); catch_ptr += sizeof(uint32_t);

            bool match = false;
            if (catch_kind == WAH_CATCH_KIND_CATCH || catch_kind == WAH_CATCH_KIND_CATCH_REF) {
                if (catch_tag_idx < handler->handler_tag_instance_count &&
                    handler->handler_tag_instances[catch_tag_idx].identity == exc->tag_identity) {
                    match = true;
                }
            } else {
                match = true;
            }

            if (match) {
                ctx->sp = handler->sp_base;
                ctx->exception_handler_depth--;

                if (catch_kind == WAH_CATCH_KIND_CATCH || catch_kind == WAH_CATCH_KIND_CATCH_REF) {
                    for (uint32_t i = 0; i < exc->value_count; i++) {
                        wah_exception_get_value(exc, i, ctx->sp++);
                    }
                }
                if (catch_kind == WAH_CATCH_KIND_CATCH_REF || catch_kind == WAH_CATCH_KIND_CATCH_ALL_REF) {
                    (*ctx->sp++).ref = wah_exception_to_ref(exc);
                }
                ctx->pending_exception = NULL;

                if (ctx->call_depth > 0) {
                    ctx->frame_ptr->bytecode_ip = handler->bytecode_base + catch_offset;
                }
                return WAH_OK;
            }
        }

        ctx->exception_handler_depth--;
    }

    return WAH_ERROR_EXCEPTION;
}
#endif // WAH_FEATURE_EXCEPTION && WAH_FEATURE_GC

static inline bool wah_ref_test_heap_type(wah_exec_context_t *ctx, wah_value_t ref_val, wah_type_t target) {
    void *ref = ref_val.ref;
    if (ref == NULL) return false;

    wah_type_t ht = WAH_TYPE_AS_NON_NULL(target);
    if (wah_ref_is_i31(ref)) {
        // EXTERN is included because WAH uses identity representation for
        // any.convert_extern / extern.convert_any. Validation ensures ref.test
        // extern can only be applied to values in the extern hierarchy, so an
        // i31 reaching this point was already converted via extern.convert_any.
        return ht == WAH_TYPE_ANY || ht == WAH_TYPE_EXTERN || ht == WAH_TYPE_EQ || ht == WAH_TYPE_I31;
    }

    wah_gc_object_t *hdr = wah_gc_header(ref);
    wah_repr_t repr_id = hdr->repr_id;

    if (target >= 0) {
        uint32_t target_idx = WAH_TYIDX(target);
        const wah_module_t *obj_mod = hdr->module;
        if (obj_mod == ctx->module || obj_mod == NULL) {
            if (wah_type_accepts_repr(ctx->module, target_idx, repr_id))
                return true;
        } else if (repr_id >= 0) {
            uint32_t obj_typeidx = wah_repr_info_typeidx(obj_mod, repr_id);
            if (obj_typeidx != (uint32_t)-1)
                return wah_cross_module_subtype_cached(ctx, obj_mod, WAH_TYPE_FROM_IDX(obj_typeidx, 0),
                                                       ctx->module, target);
        }
        if (repr_id == WAH_TYPE_BOT &&
            ctx->module->type_defs && target_idx < ctx->module->type_count &&
            ctx->module->type_defs[target_idx].kind == WAH_COMP_FUNC) {
            const wah_function_t *fn = wah_ref_to_func(ref);
            const wah_module_t *fn_module = fn->fn_module ? fn->fn_module : ctx->module;
            if (!fn->is_host && fn->local_idx < fn_module->wasm_function_count) {
                uint32_t fn_type = fn_module->function_type_indices[fn->local_idx];
                return wah_cross_module_subtype_cached(ctx, fn_module, WAH_TYPE_FROM_IDX(fn_type, 0),
                                                       ctx->module, target);
            }
        }
        return false;
    }

    switch (ht) {
        // ANY and EXTERN share a case: identity-based extern conversions (see i31 comment above).
        case WAH_TYPE_ANY: case WAH_TYPE_EXTERN: return repr_id != WAH_TYPE_BOT;
        case WAH_TYPE_EQ: return repr_id >= 0;
        case WAH_TYPE_STRUCT: {
            if (repr_id < 0) return false;
            const wah_module_t *obj_mod = hdr->module;
            return obj_mod && (uint32_t)repr_id < obj_mod->repr_count &&
                   obj_mod->repr_infos[repr_id]->type == WAH_REPR_STRUCT;
        }
        case WAH_TYPE_ARRAY: {
            if (repr_id < 0) return false;
            const wah_module_t *obj_mod = hdr->module;
            return obj_mod && (uint32_t)repr_id < obj_mod->repr_count &&
                   obj_mod->repr_infos[repr_id]->type == WAH_REPR_ARRAY;
        }
        case WAH_TYPE_FUNC: return repr_id == WAH_TYPE_BOT;
        case WAH_TYPE_I31: default: return false;
    }
}

// Internal helper: Call a host function with given context
static wah_error_t wah_call_host_function_internal(
    wah_exec_context_t *exec_ctx,
    const wah_function_t *fn,
    const wah_value_t *params,
    uint32_t param_count,
    wah_value_t *results
) {
    wah_call_context_t call_ctx = {
        .exec = exec_ctx,
        .nparams = param_count, .params = params,
        .nresults = fn->nresults, .results = results,
        .param_types = fn->param_types, .result_types = fn->result_types,
        .trap_reason = WAH_OK,
    };

    WAH_ASSERT(!exec_ctx->gc || exec_ctx->gc->phase == WAH_GC_PHASE_IDLE);
    fn->func(&call_ctx, fn->userdata);
    WAH_ASSERT(!exec_ctx->gc || exec_ctx->gc->phase == WAH_GC_PHASE_IDLE);

    WAH_ENSURE(call_ctx.trap_reason == WAH_OK, call_ctx.trap_reason);

    return WAH_OK;
}

// --- Chunked bulk-op helpers for bounded interruption ---
// Each returns the number of elements processed. If < size, interrupted.

static uint64_t wah_bulk_table_fill(wah_exec_context_t *ctx, wah_exec_context_t *fctx, uint32_t table_idx,
                                    uint64_t offset, wah_value_t val, uint64_t size) {
    for (uint64_t done = 0; done < size; ) {
        uint64_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = wah_bulk_fuel_limit(ctx, chunk);
        for (uint64_t i = 0; i < chunk; ++i)
            wah_ref_store_table(fctx, table_idx, offset + done + i, val);
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

static uint64_t wah_bulk_table_copy(wah_exec_context_t *ctx, wah_exec_context_t *fctx,
                                    uint32_t dst_table_idx, uint64_t dst_offset,
                                    uint32_t src_table_idx, uint64_t src_offset, uint64_t size) {
    if (src_table_idx == dst_table_idx && dst_offset == src_offset) return size;
    bool backward = (src_table_idx == dst_table_idx && dst_offset > src_offset && dst_offset < src_offset + size);
    for (uint64_t done = 0; done < size; ) {
        uint64_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = wah_bulk_fuel_limit(ctx, chunk);
        if (backward) {
            uint64_t tail = size - done;
            for (uint64_t j = 0; j < chunk; ++j)
                wah_ref_store_table(fctx, dst_table_idx, dst_offset + tail - 1 - j,
                                    fctx->tables[src_table_idx].entries[src_offset + tail - 1 - j]);
        } else {
            for (uint64_t j = 0; j < chunk; ++j)
                wah_ref_store_table(fctx, dst_table_idx, dst_offset + done + j,
                                    fctx->tables[src_table_idx].entries[src_offset + done + j]);
        }
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

// Returns elements processed. Sets *out_err on trap (negative). If WAH_OK and returned < size, stopped.
static uint32_t wah_bulk_table_init(wah_exec_context_t *ctx, wah_exec_context_t *fctx, uint32_t table_idx, uint64_t dst_offset,
                                    const wah_element_segment_t *segment,
                                    uint32_t src_offset, uint32_t size, wah_error_t *out_err) {
    *out_err = WAH_OK;
    for (uint32_t done = 0; done < size; ) {
        uint32_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = (uint32_t)wah_bulk_fuel_limit(ctx, chunk);
        for (uint32_t j = 0; j < chunk; ++j) {
            uint32_t i = done + j;
            wah_value_t store_val;
            if (!segment->is_expr_elem) {
                uint32_t gfi = segment->u.func_indices[src_offset + i];
                WAH_ASSERT(gfi < fctx->function_table_count);
                wah_function_t *fn = &fctx->function_table[gfi];
                if (!fn->is_host && fn->fn_ctx == NULL && fn->fn_module == fctx->module) fn->fn_ctx = fctx;
                store_val.ref = wah_func_to_ref(fn);
            } else {
                wah_value_t elem_val;
                if (src_offset + i >= segment->num_elems) { *out_err = WAH_ERROR_TRAP; return done + j; }
                wah_error_t e = wah_eval_const_expr(fctx, segment->u.expr.bytecodes[src_offset + i],
                                                    segment->u.expr.bytecode_sizes[src_offset + i], &elem_val);
                if (e != WAH_OK) { *out_err = e; return done + j; }
                if (elem_val.ref == wah_func_to_ref(wah_funcref_sentinel)) {
                    uint32_t gfi = elem_val._prefuncref.func_idx;
                    WAH_ASSERT(gfi < fctx->function_table_count);
                    wah_function_t *fn = &fctx->function_table[gfi];
                    if (!fn->is_host && fn->fn_ctx == NULL && fn->fn_module == fctx->module) fn->fn_ctx = fctx;
                    store_val.ref = wah_func_to_ref(fn);
                } else {
                    store_val = elem_val;
                }
            }
            wah_ref_store_table(fctx, table_idx, dst_offset + i, store_val);
        }
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static uint32_t wah_bulk_array_fill(wah_exec_context_t *ctx, wah_type_t et, uint8_t *elems,
                                     uint32_t offset, uint32_t size, uint32_t elem_size, const wah_value_t *fill_val) {
    for (uint32_t done = 0; done < size; ) {
        uint32_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = (uint32_t)wah_bulk_fuel_limit(ctx, chunk);
        for (uint32_t i = 0; i < chunk; i++)
            wah_gc_store_field(et, elems + (offset + done + i) * elem_size, fill_val);
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

static uint32_t wah_bulk_array_init_elem(wah_exec_context_t *ctx, uint8_t *elems, uint32_t dst_offset,
                                          const wah_element_segment_t *seg, uint32_t src_offset,
                                          uint32_t size, wah_error_t *out_err) {
    *out_err = WAH_OK;
    for (uint32_t done = 0; done < size; ) {
        uint32_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = (uint32_t)wah_bulk_fuel_limit(ctx, chunk);
        for (uint32_t j = 0; j < chunk; j++) {
            uint32_t i = done + j;
            if (!seg->is_expr_elem) {
                uint32_t fidx = seg->u.func_indices[src_offset + i];
                WAH_ASSERT(fidx < ctx->function_table_count);
                ((void **)(elems))[dst_offset + i] = wah_func_to_ref(&ctx->function_table[fidx]);
            } else {
                wah_value_t ev;
                wah_error_t e = wah_eval_const_expr(ctx, seg->u.expr.bytecodes[src_offset + i],
                    seg->u.expr.bytecode_sizes[src_offset + i], &ev);
                if (e != WAH_OK) { *out_err = e; return done + j; }
                ((void **)(elems))[dst_offset + i] = ev.ref;
            }
        }
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}
#endif // WAH_FEATURE_GC (bulk array helpers)

static uint64_t wah_memory_fill_internal(wah_exec_context_t *ctx,
                                          uint8_t *mem_data, uint64_t dst, uint8_t val, uint64_t size) {
    for (uint64_t done = 0; done < size; ) {
        uint64_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = wah_bulk_fuel_limit(ctx, chunk);
        memset(mem_data + dst + done, val, (size_t)chunk);
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

static uint64_t wah_memory_init_internal(wah_exec_context_t *ctx,
                                          uint8_t *mem_data, uint64_t dest_offset,
                                          const uint8_t *src_data, uint32_t src_offset, uint32_t size) {
    for (uint32_t done = 0; done < size; ) {
        uint32_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = (uint32_t)wah_bulk_fuel_limit(ctx, chunk);
        memcpy(mem_data + dest_offset + done, src_data + src_offset + done, chunk);
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

static uint64_t wah_memory_copy_internal(wah_exec_context_t *ctx,
                                      uint8_t *dest_data, uint64_t dest, uint8_t *src_data, uint64_t src,
                                      uint64_t size, bool backward) {
    for (uint64_t done = 0; done < size; ) {
        uint64_t chunk = size - done < WAH_BULK_CHECK_INTERVAL ? size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = wah_bulk_fuel_limit(ctx, chunk);
        if (backward) {
            uint64_t tail = size - done;
            memmove(dest_data + dest + tail - chunk, src_data + src + tail - chunk, (size_t)chunk);
        } else {
            memmove(dest_data + dest + done, src_data + src + done, (size_t)chunk);
        }
        done += chunk;
        wah_bulk_fuel_charge(ctx, chunk);
        if (done < size && wah_bulk_should_stop(ctx)) return done;
    }
    return size;
}

static wah_error_t wah_table_grow_internal(
    wah_exec_context_t *ctx, wah_exec_context_t *fctx, uint32_t table_idx, uint64_t delta, wah_value_t init_val,
    uint64_t *old_size, bool *grew
) {
    *grew = false;
    *old_size = fctx->tables[table_idx].size;

    uint64_t new_size = *old_size + delta;
    if (new_size < *old_size || new_size > fctx->tables[table_idx].max_size) {
        return WAH_OK;
    }

    if (delta > SIZE_MAX / sizeof(wah_value_t) || new_size > SIZE_MAX / sizeof(wah_value_t)) {
        return WAH_OK;
    }
    uint64_t delta_bytes = delta * sizeof(wah_value_t);
    if (!wah_budget_check(ctx, delta_bytes)) {
        return WAH_OK;
    }
    if (fctx->tables[table_idx].import_ctx && fctx->tables[table_idx].is_imported) {
        if (!wah_budget_check(fctx->tables[table_idx].import_ctx, delta_bytes)) {
            return WAH_OK;
        }
    }

    wah_exec_context_t *owner = (fctx->tables[table_idx].is_imported && fctx->tables[table_idx].import_ctx)
        ? fctx->tables[table_idx].import_ctx : ctx;
    const wah_alloc_t *grow_alloc = &owner->alloc;
    wah_value_t *new_table = NULL;
    wah_error_t err = wah_malloc(grow_alloc, (size_t)new_size, sizeof(wah_value_t), (void **)&new_table);
    if (err != WAH_OK) return err;

    if (*old_size > 0) {
        memcpy(new_table, fctx->tables[table_idx].entries, sizeof(wah_value_t) * *old_size);
    }
    for (uint64_t i = *old_size; i < new_size; ++i) {
        new_table[i] = init_val;
    }

    wah_value_t *old_entries = fctx->tables[table_idx].entries;
    fctx->tables[table_idx].entries = new_table;
    fctx->tables[table_idx].size = new_size;
    wah_budget_charge(ctx, delta_bytes);

    if (fctx->tables[table_idx].import_ctx) {
        wah_exec_context_t *src = fctx->tables[table_idx].import_ctx;
        uint32_t src_idx = fctx->tables[table_idx].import_idx;
        if (fctx->tables[table_idx].is_imported) {
            wah_budget_charge(src, delta_bytes);
            src->tables[src_idx].is_imported = true;
        }
        src->tables[src_idx].entries = new_table;
        src->tables[src_idx].size = new_size;
    }
    wah_free(grow_alloc, old_entries);
    fctx->tables[table_idx].is_imported = false;

    *grew = true;
    return WAH_OK;
}

static bool wah_memory_grow_internal(
    wah_exec_context_t *ctx, wah_exec_context_t *fctx, uint32_t mem_idx,
    uint64_t pages_to_grow, uint64_t *old_pages
) {
    *old_pages = fctx->memories[mem_idx].size / WAH_WASM_PAGE_SIZE;
    uint64_t new_pages = *old_pages + pages_to_grow;

    if (new_pages < *old_pages || new_pages > fctx->memories[mem_idx].max_pages || new_pages > SIZE_MAX / WAH_WASM_PAGE_SIZE) {
        return false;
    }

    size_t new_memory_size = (size_t)new_pages * WAH_WASM_PAGE_SIZE;
    uint64_t delta_bytes = new_memory_size - fctx->memories[mem_idx].size;
    if (!wah_budget_check(ctx, delta_bytes)) {
        return false;
    }
    if (fctx->memories[mem_idx].import_ctx && fctx->memories[mem_idx].is_imported) {
        if (!wah_budget_check(fctx->memories[mem_idx].import_ctx, delta_bytes)) {
            return false;
        }
    }

    wah_exec_context_t *owner = (fctx->memories[mem_idx].is_imported && fctx->memories[mem_idx].import_ctx)
        ? fctx->memories[mem_idx].import_ctx : fctx;
    void *memory_data = fctx->memories[mem_idx].data;
    if (wah_realloc(&owner->alloc, new_memory_size, sizeof(*fctx->memories[mem_idx].data), &memory_data) != WAH_OK) {
        return false;
    }
    fctx->memories[mem_idx].data = (uint8_t *)memory_data;

    if (new_memory_size > fctx->memories[mem_idx].size) {
        memset(fctx->memories[mem_idx].data + fctx->memories[mem_idx].size, 0, new_memory_size - fctx->memories[mem_idx].size);
    }

    wah_budget_charge(ctx, delta_bytes);
    fctx->memories[mem_idx].size = (uint64_t)new_memory_size;
    if (fctx->memories[mem_idx].import_ctx) {
        wah_exec_context_t *src = fctx->memories[mem_idx].import_ctx;
        uint32_t src_idx = fctx->memories[mem_idx].import_idx;
        if (fctx->memories[mem_idx].is_imported) {
            wah_budget_charge(src, delta_bytes);
            src->memories[src_idx].is_imported = true;
        }
        src->memories[src_idx].data = fctx->memories[mem_idx].data;
        src->memories[src_idx].size = fctx->memories[mem_idx].size;
        if (src_idx == 0) {
            src->memory_base = src->memories[0].data;
            src->memory_size = src->memories[0].size;
        }
    }
    fctx->memories[mem_idx].is_imported = false;
    if (mem_idx == 0) {
        fctx->memory_base = fctx->memories[0].data;
        fctx->memory_size = fctx->memories[0].size;
    }

    return true;
}

// Enforce deterministic fp operations.
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC push_options
    #pragma GCC optimize("-ffp-contract=off")
    #pragma GCC diagnostic push
    // GCC 15 warns that locals' addresses may escape through musttail calls, but
    // WAH_NEXT() only passes values (pointers into ctx/frame/bytecode), never &local.
    #if __GNUC__ >= 15
    #pragma GCC diagnostic ignored "-Wmaybe-musttail-local-addr"
    #endif
    #define WAH_PRAGMA_FP_CONTRACT_OFF()
#elif defined(_MSC_VER)
    #pragma float_control(push)
    #pragma fp_contract(off)
    #define WAH_PRAGMA_FP_CONTRACT_OFF()
#else // Clang etc.
    #define WAH_PRAGMA_FP_CONTRACT_OFF() _Pragma("STDC FP_CONTRACT OFF")
#endif

#if defined(WAH_USE_MUSTTAIL) // --- Tail recursion dispatch ---

#define WAH_RUN(opcode) \
    static wah_error_t wah_run_##opcode(wah_exec_context_t *ctx, wah_call_frame_t *frame, \
                                        const uint8_t *bytecode_ip, const uint8_t *bytecode_base, \
                                        wah_value_t *sp, wah_exec_context_t *fctx, wah_error_t err)

#define WAH_NEXT() do { \
        (void)bytecode_base; (void)err; \
        __attribute__((musttail)) return wah_run_single(ctx, frame, bytecode_ip, bytecode_base, sp, fctx, err); \
    } while (0)

#define WAH_CLEANUP() goto cleanup; cleanup: do { \
        /* Before returning, store the final IP and SP back into the (potentially last) frame */ \
        if (ctx->call_depth > 0) { \
            frame->bytecode_ip = bytecode_ip; \
        } \
        ctx->sp = sp; \
        return (err); \
    } while (0)

static wah_error_t wah_run_single(wah_exec_context_t *ctx, wah_call_frame_t *frame,
                                  const uint8_t *bytecode_ip, const uint8_t *bytecode_base,
                                  wah_value_t *sp, wah_exec_context_t *fctx, wah_error_t err);

#elif defined(WAH_USE_COMPUTED_GOTO) // --- Computed GOTO dispatch ---

static wah_error_t wah_run_interpreter(wah_exec_context_t *ctx) {
    wah_error_t err = WAH_OK;

    // These are pointers to the current frame's state for faster access.
    wah_call_frame_t *frame = ctx->frame_ptr;
    const uint8_t *bytecode_ip = frame->bytecode_ip;
    const uint8_t *bytecode_base = frame->code->parsed_code.bytecode;
    wah_value_t *sp = ctx->sp;
    wah_exec_context_t *fctx = frame->frame_ctx;   // Module-local context for table/memory access

    // Computed goto jump table
    static const void* wah_opcode_labels[] = {
#define WAH_OPCODE_LABEL(name, cls, val, feat) [WAH_OP_##name] = &&wah_op_##name,
#define WAH_EXTRA_OPCODE_LABEL(name, suffix) [WAH_OP_##name##_##suffix] = &&wah_op_##name##_##suffix,
#define WAH_INTERNAL_OPCODE_LABEL(name) [WAH_OP_##name] = &&wah_op_##name,
        WAH_OPCODES(WAH_OPCODE_LABEL)
        WAH_INTERNAL_OPCODES(WAH_INTERNAL_OPCODE_LABEL)
        WAH_EXTRA_OPCODES(WAH_EXTRA_OPCODE_LABEL)
#undef WAH_OPCODE_LABEL
#undef WAH_EXTRA_OPCODE_LABEL
#undef WAH_INTERNAL_OPCODE_LABEL
    };

    goto *wah_opcode_labels[wah_read_u16_le(bytecode_ip)];

    #define WAH_RUN(opcode) wah_op_##opcode: bytecode_ip += sizeof(uint16_t);
    #define WAH_NEXT() goto *wah_opcode_labels[wah_read_u16_le(bytecode_ip)]
    #define WAH_CLEANUP() goto cleanup

#else // --- Default switch-based interpreter ---

static wah_error_t wah_run_interpreter(wah_exec_context_t *ctx) {
    wah_error_t err = WAH_OK;

    // These are pointers to the current frame's state for faster access.
    wah_call_frame_t *frame = ctx->frame_ptr;
    const uint8_t *bytecode_ip = frame->bytecode_ip;
    const uint8_t *bytecode_base = frame->code->parsed_code.bytecode;
    wah_value_t *sp = ctx->sp;  // Stack pointer for faster access
    wah_exec_context_t *fctx = frame->frame_ctx;   // Module-local context for table/memory access

    while (1) {
        uint16_t opcode = wah_read_u16_le(bytecode_ip);
        bytecode_ip += sizeof(uint16_t);

        switch (opcode) {

        #define WAH_RUN(opcode) break; case WAH_OP_##opcode:
        #define WAH_NEXT() break
        #define WAH_CLEANUP() goto cleanup

#endif

//------------------------------------------------------------------------------
#define WAH_NEVER_RUN(opcode) WAH_RUN(opcode) { (void)bytecode_base; WAH_UNREACHABLE(); WAH_NEXT(); }

WAH_NEVER_RUN(NOP) // Removed by preparser
WAH_NEVER_RUN(BLOCK) // Converted to bytecode offsets
WAH_NEVER_RUN(LOOP) // Converted to bytecode offsets
WAH_NEVER_RUN(SELECT_T) // Identical to WAH_RUN(SELECT) in run time

WAH_RUN(POLL) {
    // TODO: this unconditional store could be deferred to the poll_flag branch
    // (read via bytecode_ip - sizeof(uint32_t) after advancing), but
    // host functions currently call wah_gc_enumerate_roots which relies on
    // ref_map_offset being up-to-date outside of poll_handler. See test_gc.c.
    frame->ref_map_offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);

    if (WAH_POLL_FLAG_LOAD(ctx->poll_flag)) {
        frame->bytecode_ip = bytecode_ip;
        ctx->sp = sp;
        WAH_CHECK_GOTO(wah_poll_handler(ctx), cleanup);
        sp = ctx->sp;
        frame = ctx->frame_ptr;
        bytecode_ip = frame->bytecode_ip;
        bytecode_base = frame->code->parsed_code.bytecode;
        fctx = frame->frame_ctx;
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(METER) {
    uint16_t cost = wah_read_u16_le(bytecode_ip);
    bytecode_ip += sizeof(uint16_t);
    uint32_t slow_offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    ctx->fuel -= cost;
    if (ctx->fuel < 0) {
        ctx->fuel += cost;
        bytecode_ip = bytecode_base + slow_offset;
    }
    WAH_NEXT();
}

WAH_RUN(TICK) {
    uint32_t resume_offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    ctx->fuel--;
    if (ctx->fuel < 0) {
        err = WAH_STATUS_FUEL_EXHAUSTED;
    } else if (WAH_POLL_FLAG_LOAD(ctx->interrupt_flag)) {
        WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 0);
        wah_recompute_poll_flag(ctx);
        err = WAH_STATUS_YIELDED;
    }
    if (err != WAH_OK) {
        bytecode_ip = bytecode_base + resume_offset;
        frame->bytecode_ip = bytecode_ip;
        ctx->sp = sp;
        WAH_CLEANUP();
    }
    WAH_NEXT();
}

WAH_RUN(IF) {
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    if ((*--sp).i32 == 0) {
        bytecode_ip = bytecode_base + offset;
    }
    WAH_NEXT();
}

WAH_RUN(ELSE) { // This is an unconditional jump
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    bytecode_ip = bytecode_base + offset;
    WAH_NEXT();
}

#define WAH_DROP_KEEP(keep_, drop_) do { \
    uint32_t keep__ = (keep_); \
    uint32_t drop__ = (drop_); \
    if (drop__ > 0) { \
        wah_value_t *dst__ = sp - keep__ - drop__; \
        if (keep__ > 0) memmove(dst__, sp - keep__, keep__ * sizeof(wah_value_t)); \
        sp -= drop__; \
    } \
} while (0)

WAH_RUN(BR) {
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    WAH_DROP_KEEP(keep, drop);
    bytecode_ip = bytecode_base + offset;
    WAH_NEXT();
}

WAH_RUN(BR_IF) {
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    if ((*--sp).i32 != 0) {
        WAH_DROP_KEEP(keep, drop);
        bytecode_ip = bytecode_base + offset;
    }
    WAH_NEXT();
}

WAH_RUN(BR_TABLE) {
    uint32_t index = (uint32_t)(*--sp).i32;
    uint32_t num_targets = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);

    uint32_t target_idx = (index < num_targets) ? index : num_targets;
    uint32_t target_offset = wah_read_u32_le(bytecode_ip + target_idx * 2 * sizeof(uint32_t));
    uint32_t drop = wah_read_u32_le(bytecode_ip + target_idx * 2 * sizeof(uint32_t) + sizeof(uint32_t));
    WAH_DROP_KEEP(keep, drop);
    bytecode_ip = bytecode_base + target_offset;
    WAH_NEXT();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_EXCEPTION) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)

WAH_RUN(TRY_TABLE) {
    uint32_t catch_count_val = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    WAH_ENSURE_GOTO(ctx->exception_handler_depth < WAH_MAX_EXCEPTION_HANDLER_DEPTH, WAH_ERROR_STACK_OVERFLOW, cleanup);
    wah_exception_handler_t *handler = &ctx->exception_handlers[ctx->exception_handler_depth++];
    handler->call_depth = ctx->call_depth;
    handler->sp_base = sp;
    handler->catch_table = bytecode_ip;
    handler->catch_count = catch_count_val;
    handler->bytecode_base = bytecode_base;
    handler->handler_tag_instances = fctx->tag_instances;
    handler->handler_tag_instance_count = fctx->tag_instance_count;
    for (uint32_t ci = 0; ci < catch_count_val; ci++) {
        bytecode_ip += 1; // catch_kind
        bytecode_ip += sizeof(uint32_t); // tag_idx
        bytecode_ip += sizeof(uint32_t); // offset
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(END_TRY_TABLE) {
    WAH_ASSERT(ctx->exception_handler_depth > 0);
    ctx->exception_handler_depth--;
    WAH_NEXT();
}

WAH_RUN(THROW) {
    uint32_t tag_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    WAH_ASSERT(tag_idx < fctx->tag_instance_count);

    const wah_tag_instance_t *tag_inst = &fctx->tag_instances[tag_idx];
    const wah_module_t *cur_module = frame->module ? frame->module : ctx->module;
    uint32_t type_idx = tag_inst->type_index;
    const wah_func_type_t *tag_type = &cur_module->types[type_idx];
    uint32_t value_count = tag_type->param_count;

    wah_exception_t *exc = wah_gc_alloc_exception(ctx, value_count);
    WAH_ENSURE_GOTO(exc != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    exc->tag_identity = tag_inst->identity;
    exc->tag_index = tag_idx;
    exc->value_count = value_count;
    if (value_count > 0) {
        wah_type_t *exc_types = wah_exception_value_types(exc);
        for (uint32_t i = 0; i < value_count; i++) {
            wah_exception_set_value(exc, i, &sp[-(int32_t)value_count + (int32_t)i]);
            exc_types[i] = tag_type->param_types[i];
        }
        sp -= value_count;
    }

    frame->bytecode_ip = bytecode_ip;
    ctx->sp = sp;
    err = wah_throw_exception(ctx, exc);
    if (err != WAH_OK) goto cleanup;
    sp = ctx->sp;
    RELOAD_FRAME();
    WAH_NEXT();

    WAH_CLEANUP();
}

WAH_RUN(THROW_REF) {
    wah_value_t exnref_val = *--sp;
    WAH_ENSURE_GOTO(exnref_val.ref != NULL, WAH_ERROR_TRAP, cleanup);

    wah_exception_t *exc = wah_exception_from_ref(exnref_val.ref);

    frame->bytecode_ip = bytecode_ip;
    ctx->sp = sp;
    err = wah_throw_exception(ctx, exc);
    if (err != WAH_OK) goto cleanup;
    sp = ctx->sp;
    RELOAD_FRAME();
    WAH_NEXT();

    WAH_CLEANUP();
}

#else // !WAH_FEATURE_EXCEPTION || !WAH_FEATURE_GC
WAH_NEVER_RUN(TRY_TABLE) WAH_NEVER_RUN(END_TRY_TABLE) WAH_NEVER_RUN(THROW) WAH_NEVER_RUN(THROW_REF)
#endif // WAH_FEATURE_EXCEPTION && WAH_FEATURE_GC

WAH_RUN(I32_CONST) { (*sp++).i32 = (int32_t)wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); WAH_NEXT(); }
WAH_RUN(I64_CONST) { (*sp++).i64 = (int64_t)wah_read_u64_le(bytecode_ip); bytecode_ip += sizeof(uint64_t); WAH_NEXT(); }
WAH_RUN(F32_CONST) { (*sp++).f32 = wah_read_f32_le(bytecode_ip); bytecode_ip += sizeof(float); WAH_NEXT(); }
WAH_RUN(F64_CONST) { (*sp++).f64 = wah_read_f64_le(bytecode_ip); bytecode_ip += sizeof(double); WAH_NEXT(); }
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)
WAH_RUN(V128_CONST) {
    memcpy(&(*sp++).v128, bytecode_ip, sizeof(wah_v128_t));
    bytecode_ip += sizeof(wah_v128_t);
    WAH_NEXT();
}
#endif

WAH_RUN(REF_NULL) {
    (*sp++).ref = NULL;
    WAH_NEXT();
}

WAH_RUN(REF_IS_NULL) {
    wah_value_t ref_val = *--sp;

    // Determine if reference is null based on type
    // For now, we'll check both funcref and externref
    // In a complete implementation, we'd need to track the actual type of each stack value
    int32_t is_null = (ref_val.ref == NULL);
    (*sp++).i32 = is_null;
    WAH_NEXT();
}

WAH_RUN(REF_FUNC) {
    uint32_t func_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);

    WAH_ASSERT(func_idx < frame->frame_function_table_count && "validation should have verified function index");
    (*sp++).ref = wah_func_to_ref(&frame->frame_function_table[func_idx]);
    WAH_NEXT();
}

WAH_RUN(REF_FUNC_CONST) {
    uint32_t func_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    *sp++ = (wah_value_t){
        ._prefuncref = { .sentinel = wah_func_to_ref(wah_funcref_sentinel), .func_idx = func_idx }
    };
    WAH_NEXT();
}

WAH_RUN(GLOBAL_GET_INDIRECT) {
    uint32_t global_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    *sp++ = *(wah_value_t *)frame->frame_globals[global_idx].ref;
    WAH_NEXT();
}

WAH_RUN(GLOBAL_SET_INDIRECT) {
    uint32_t global_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    wah_ref_store_global_slot((wah_value_t *)frame->frame_globals[global_idx].ref, *--sp);
    WAH_NEXT();
}

WAH_RUN(REF_AS_NON_NULL) {
    WAH_ENSURE_GOTO(sp[-1].ref != NULL, WAH_ERROR_TRAP, cleanup);
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(BR_ON_NULL) {
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    if (sp[-1].ref == NULL) {
        --sp;
        WAH_DROP_KEEP(keep, drop);
        bytecode_ip = bytecode_base + offset;
    }
    WAH_NEXT();
}

WAH_RUN(BR_ON_NON_NULL) {
    uint32_t offset = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    if (sp[-1].ref != NULL) {
        WAH_DROP_KEEP(keep, drop);
        bytecode_ip = bytecode_base + offset;
    } else {
        --sp;
    }
    WAH_NEXT();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)

WAH_RUN(REF_TEST) {
    wah_type_t heap_type = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(int32_t);
    wah_value_t ref_val = *--sp;
    bool result = wah_ref_test_heap_type(fctx, ref_val, heap_type);
    (*sp++).i32 = result ? 1 : 0;
    WAH_NEXT();
}

WAH_RUN(REF_TEST_NULL) {
    wah_type_t heap_type = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(int32_t);
    wah_value_t ref_val = *--sp;
    bool result = (ref_val.ref == NULL) || wah_ref_test_heap_type(fctx, ref_val, heap_type);
    (*sp++).i32 = result ? 1 : 0;
    WAH_NEXT();
}

WAH_RUN(REF_CAST) {
    wah_type_t heap_type = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(int32_t);
    wah_value_t ref_val = sp[-1];
    WAH_ENSURE_GOTO(ref_val.ref != NULL && wah_ref_test_heap_type(fctx, ref_val, heap_type),
                     WAH_ERROR_TRAP, cleanup);
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(REF_CAST_NULL) {
    wah_type_t heap_type = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(int32_t);
    wah_value_t ref_val = sp[-1];
    WAH_ENSURE_GOTO(ref_val.ref == NULL || wah_ref_test_heap_type(fctx, ref_val, heap_type),
                     WAH_ERROR_TRAP, cleanup);
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(REF_EQ) {
    wah_value_t b = *--sp;
    wah_value_t a = *--sp;
    (*sp++).i32 = (a.ref == b.ref) ? 1 : 0;
    WAH_NEXT();
}

WAH_RUN(BR_ON_CAST) {
    uint8_t cast_flags = *bytecode_ip++;
    uint32_t offset = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_type_t target_ht = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(int32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_value_t ref_val = sp[-1];
    bool matches;
    if (ref_val.ref == NULL) {
        matches = (cast_flags & 0x02) != 0;
    } else {
        matches = wah_ref_test_heap_type(fctx, ref_val, target_ht);
    }
    if (matches) {
        WAH_DROP_KEEP(keep, drop);
        bytecode_ip = bytecode_base + offset;
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(BR_ON_CAST_FAIL) {
    uint8_t cast_flags = *bytecode_ip++;
    uint32_t offset = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_type_t target_ht = (wah_type_t)(int32_t)wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(int32_t);
    uint32_t keep = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t drop = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_value_t ref_val = sp[-1];
    bool matches;
    if (ref_val.ref == NULL) {
        matches = (cast_flags & 0x02) != 0;
    } else {
        matches = wah_ref_test_heap_type(fctx, ref_val, target_ht);
    }
    if (!matches) {
        WAH_DROP_KEEP(keep, drop);
        bytecode_ip = bytecode_base + offset;
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(STRUCT_NEW) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_struct(ctx, fctx->module, repr_id, info);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    uint8_t *payload = (uint8_t *)obj;
    const wah_type_def_t *td = &fctx->module->type_defs[typeidx];
    for (uint32_t i = td->field_count; i > 0; --i) {
        wah_value_t val = *--sp;
        wah_gc_store_field(td->field_types[i - 1], payload + info->fields[i - 1].offset, &val);
    }
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(STRUCT_NEW_DEFAULT) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_struct(ctx, fctx->module, repr_id, info);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

#define WAH_STRUCT_GET_BODY(load) { \
    uint32_t offset = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); \
    void *obj = (--sp)->ref; \
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup); \
    uint8_t *addr = (uint8_t *)obj + offset; \
    wah_value_t val = {0}; \
    load; \
    *sp++ = val; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}
#define WAH_STRUCT_SET_BODY(store) { \
    uint32_t offset = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); \
    wah_value_t val = *--sp; \
    void *obj = (--sp)->ref; \
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup); \
    uint8_t *addr = (uint8_t *)obj + offset; \
    store; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_NEVER_RUN(STRUCT_GET) WAH_NEVER_RUN(STRUCT_GET_S) WAH_NEVER_RUN(STRUCT_GET_U) WAH_NEVER_RUN(STRUCT_SET)
WAH_RUN(STRUCT_GET_S8)  WAH_STRUCT_GET_BODY(val.i32 = (int32_t)(int8_t)*(uint8_t *)addr)
WAH_RUN(STRUCT_GET_S16) WAH_STRUCT_GET_BODY(val.i32 = (int32_t)(int16_t)*(uint16_t *)addr)
WAH_RUN(STRUCT_GET_U8)  WAH_STRUCT_GET_BODY(val.i32 = (int32_t)*(uint8_t *)addr)
WAH_RUN(STRUCT_GET_U16) WAH_STRUCT_GET_BODY(val.i32 = (int32_t)*(uint16_t *)addr)
WAH_RUN(STRUCT_GET_32)  WAH_STRUCT_GET_BODY(val.i32 = *(int32_t *)addr)
WAH_RUN(STRUCT_GET_64)  WAH_STRUCT_GET_BODY(val.i64 = *(int64_t *)addr)
WAH_RUN(STRUCT_GET_128) WAH_STRUCT_GET_BODY(memcpy(&val, addr, sizeof(wah_v128_t)))
WAH_RUN(STRUCT_SET_8)   WAH_STRUCT_SET_BODY(*(uint8_t *)addr = (uint8_t)val.i32)
WAH_RUN(STRUCT_SET_16)  WAH_STRUCT_SET_BODY(*(uint16_t *)addr = (uint16_t)val.i32)
WAH_RUN(STRUCT_SET_32)  WAH_STRUCT_SET_BODY(*(int32_t *)addr = val.i32)
WAH_RUN(STRUCT_SET_64)  WAH_STRUCT_SET_BODY(*(int64_t *)addr = val.i64)
WAH_RUN(STRUCT_SET_128) WAH_STRUCT_SET_BODY(memcpy(addr, &val, sizeof(wah_v128_t)))

WAH_RUN(ARRAY_NEW) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t length = (uint32_t)(--sp)->i32;
    wah_value_t init_val = *--sp;
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_array(ctx, fctx->module, repr_id, info, length);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    uint8_t *elems = (uint8_t *)obj + sizeof(wah_gc_array_body_t);
    wah_type_t et = fctx->module->type_defs[typeidx].field_types[0];
    for (uint32_t i = 0; i < length; i++)
        wah_gc_store_field(et, elems + i * info->size, &init_val);
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_NEW_DEFAULT) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t length = (uint32_t)(--sp)->i32;
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_array(ctx, fctx->module, repr_id, info, length);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_NEW_FIXED) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t length = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_array(ctx, fctx->module, repr_id, info, length);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    uint8_t *elems = (uint8_t *)obj + sizeof(wah_gc_array_body_t);
    wah_type_t et = fctx->module->type_defs[typeidx].field_types[0];
    for (uint32_t i = length; i > 0; --i) {
        wah_value_t val = *--sp;
        wah_gc_store_field(et, elems + (i - 1) * info->size, &val);
    }
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

#define WAH_ARRAY_GET_BODY(elem_size, load) { \
    uint32_t idx = (uint32_t)(--sp)->i32; \
    void *obj = (--sp)->ref; \
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup); \
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj; \
    WAH_ENSURE_GOTO(idx < body->length, WAH_ERROR_TRAP, cleanup); \
    uint8_t *addr = (uint8_t *)body + sizeof(wah_gc_array_body_t) + (size_t)idx * (elem_size); \
    wah_value_t val = {0}; \
    load; \
    *sp++ = val; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}
#define WAH_ARRAY_SET_BODY(elem_size, store) { \
    wah_value_t val = *--sp; \
    uint32_t idx = (uint32_t)(--sp)->i32; \
    void *obj = (--sp)->ref; \
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup); \
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj; \
    WAH_ENSURE_GOTO(idx < body->length, WAH_ERROR_TRAP, cleanup); \
    uint8_t *addr = (uint8_t *)body + sizeof(wah_gc_array_body_t) + (size_t)idx * (elem_size); \
    store; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_NEVER_RUN(ARRAY_GET) WAH_NEVER_RUN(ARRAY_GET_S) WAH_NEVER_RUN(ARRAY_GET_U) WAH_NEVER_RUN(ARRAY_SET)
WAH_RUN(ARRAY_GET_S8)  WAH_ARRAY_GET_BODY(1,  val.i32 = (int32_t)(int8_t)*(uint8_t *)addr)
WAH_RUN(ARRAY_GET_S16) WAH_ARRAY_GET_BODY(2,  val.i32 = (int32_t)(int16_t)*(uint16_t *)addr)
WAH_RUN(ARRAY_GET_U8)  WAH_ARRAY_GET_BODY(1,  val.i32 = (int32_t)*(uint8_t *)addr)
WAH_RUN(ARRAY_GET_U16) WAH_ARRAY_GET_BODY(2,  val.i32 = (int32_t)*(uint16_t *)addr)
WAH_RUN(ARRAY_GET_32)  WAH_ARRAY_GET_BODY(4,  val.i32 = *(int32_t *)addr)
WAH_RUN(ARRAY_GET_64)  WAH_ARRAY_GET_BODY(8,  val.i64 = *(int64_t *)addr)
WAH_RUN(ARRAY_GET_128) WAH_ARRAY_GET_BODY(16, memcpy(&val, addr, sizeof(wah_v128_t)))
WAH_RUN(ARRAY_SET_8)   WAH_ARRAY_SET_BODY(1,  *(uint8_t *)addr = (uint8_t)val.i32)
WAH_RUN(ARRAY_SET_16)  WAH_ARRAY_SET_BODY(2,  *(uint16_t *)addr = (uint16_t)val.i32)
WAH_RUN(ARRAY_SET_32)  WAH_ARRAY_SET_BODY(4,  *(int32_t *)addr = val.i32)
WAH_RUN(ARRAY_SET_64)  WAH_ARRAY_SET_BODY(8,  *(int64_t *)addr = val.i64)
WAH_RUN(ARRAY_SET_128) WAH_ARRAY_SET_BODY(16, memcpy(addr, &val, sizeof(wah_v128_t)))

WAH_RUN(ARRAY_LEN) {
    void *obj = sp[-1].ref;
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup);
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj;
    sp[-1].i32 = (int32_t)body->length;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_NEW_DATA) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t dataidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    uint32_t offset = (uint32_t)(--sp)->i32;
    WAH_ASSERT(dataidx < fctx->module->data_segment_count);
    const wah_data_segment_t *seg = &fctx->module->data_segments[dataidx];
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    uint32_t esz = info->size;
    uint32_t seg_len = wah_data_seg_data_len(fctx, dataidx);
    WAH_ENSURE_GOTO((uint64_t)offset + (uint64_t)size * esz <= seg_len, WAH_ERROR_TRAP, cleanup);
    void *obj = wah_gc_alloc_array(ctx, fctx->module, repr_id, info, size);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    uint8_t *elems = (uint8_t *)obj + sizeof(wah_gc_array_body_t);
    memcpy(elems, seg->data + offset, (size_t)size * esz);
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_NEW_ELEM) {
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t elemidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    uint32_t offset = (uint32_t)(--sp)->i32;
    WAH_ASSERT(elemidx < fctx->module->element_segment_count);
    const wah_element_segment_t *seg = &fctx->module->element_segments[elemidx];
    WAH_ENSURE_GOTO(!wah_elem_seg_is_dropped(fctx, elemidx), WAH_ERROR_TRAP, cleanup);
    WAH_ENSURE_GOTO((uint64_t)offset + size <= seg->num_elems, WAH_ERROR_TRAP, cleanup);
    wah_repr_t repr_id = fctx->module->typeidx_to_repr[typeidx];
    const wah_repr_info_t *info = fctx->module->repr_infos[repr_id];
    void *obj = wah_gc_alloc_array(ctx, fctx->module, repr_id, info, size);
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_OUT_OF_MEMORY, cleanup);
    uint8_t *elems = (uint8_t *)obj + sizeof(wah_gc_array_body_t);
    for (uint32_t i = 0; i < size; i++) {
        if (!seg->is_expr_elem) {
            uint32_t fidx = seg->u.func_indices[offset + i];
            WAH_ASSERT(fidx < ctx->function_table_count);
            ((void **)elems)[i] = wah_func_to_ref(&ctx->function_table[fidx]);
        } else {
            wah_value_t ev;
            WAH_CHECK_GOTO(wah_eval_const_expr(ctx, seg->u.expr.bytecodes[offset + i],
                seg->u.expr.bytecode_sizes[offset + i], &ev), cleanup);
            ((void **)elems)[i] = ev.ref;
        }
    }
    (*sp++).ref = obj;
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_FILL) {
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t);
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    wah_value_t fill_val = *--sp;
    uint32_t offset = (uint32_t)(--sp)->i32;
    void *obj = (--sp)->ref;
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup);
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj;
    WAH_ENSURE_GOTO((uint64_t)offset + size <= body->length, WAH_ERROR_TRAP, cleanup);
    {
        uint8_t *elems = (uint8_t *)body + sizeof(wah_gc_array_body_t);
        const wah_gc_object_t *arr_hdr = wah_gc_header(obj);
        const wah_repr_info_t *info = arr_hdr->module->repr_infos[arr_hdr->repr_id];
        wah_type_t et = fctx->module->type_defs[typeidx].field_types[0];
        uint32_t done = wah_bulk_array_fill(ctx, et, elems, offset, size, info->size, &fill_val);
        if (done < size) {
            (*sp++).ref = obj;
            (*sp++).i32 = (int32_t)(offset + done);
            *sp++ = fill_val;
            (*sp++).i32 = (int32_t)(size - done);
            WAH_BULK_YIELD(instr_start);
        }
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_COPY) {
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    uint32_t src_offset = (uint32_t)(--sp)->i32;
    void *src_obj = (--sp)->ref;
    uint32_t dst_offset = (uint32_t)(--sp)->i32;
    void *dst_obj = (--sp)->ref;
    WAH_ENSURE_GOTO(src_obj != NULL && dst_obj != NULL, WAH_ERROR_TRAP, cleanup);
    wah_gc_array_body_t *src_body = (wah_gc_array_body_t *)src_obj;
    wah_gc_array_body_t *dst_body = (wah_gc_array_body_t *)dst_obj;
    WAH_ENSURE_GOTO((uint64_t)src_offset + size <= src_body->length, WAH_ERROR_TRAP, cleanup);
    WAH_ENSURE_GOTO((uint64_t)dst_offset + size <= dst_body->length, WAH_ERROR_TRAP, cleanup);
    const wah_gc_object_t *src_hdr = wah_gc_header(src_obj);
    const wah_gc_object_t *dst_hdr = wah_gc_header(dst_obj);
    const wah_repr_info_t *src_info = src_hdr->module->repr_infos[src_hdr->repr_id];
    const wah_repr_info_t *dst_info = dst_hdr->module->repr_infos[dst_hdr->repr_id];
    uint32_t esz = src_info->size;
    WAH_ASSERT(esz == dst_info->size);
    {
        uint8_t *src_elems = (uint8_t *)src_body + sizeof(wah_gc_array_body_t);
        uint8_t *dst_elems = (uint8_t *)dst_body + sizeof(wah_gc_array_body_t);
        size_t byte_size = (size_t)size * esz;
        bool backward = (dst_obj == src_obj && dst_offset > src_offset && dst_offset < src_offset + size);
        for (size_t done = 0; done < byte_size; ) {
            size_t chunk = byte_size - done < WAH_BULK_CHECK_INTERVAL ? byte_size - done : WAH_BULK_CHECK_INTERVAL;
            chunk = (size_t)wah_bulk_fuel_limit(ctx, (uint64_t)chunk);
            if (chunk > esz) chunk -= chunk % esz; else chunk = esz;
            if (chunk > byte_size - done) chunk = byte_size - done;
            if (backward) {
                size_t tail = byte_size - done;
                memmove(dst_elems + (size_t)dst_offset * esz + tail - chunk,
                        src_elems + (size_t)src_offset * esz + tail - chunk, chunk);
            } else {
                memmove(dst_elems + (size_t)dst_offset * esz + done,
                        src_elems + (size_t)src_offset * esz + done, chunk);
            }
            done += chunk;
            wah_bulk_fuel_charge(ctx, (uint64_t)chunk);
            if (done < byte_size && wah_bulk_should_stop(ctx)) {
                uint32_t elem_done = (uint32_t)(done / esz);
                if (backward) {
                    (*sp++).ref = dst_obj;
                    (*sp++).i32 = (int32_t)dst_offset;
                    (*sp++).ref = src_obj;
                    (*sp++).i32 = (int32_t)src_offset;
                    (*sp++).i32 = (int32_t)(size - elem_done);
                } else {
                    (*sp++).ref = dst_obj;
                    (*sp++).i32 = (int32_t)(dst_offset + elem_done);
                    (*sp++).ref = src_obj;
                    (*sp++).i32 = (int32_t)(src_offset + elem_done);
                    (*sp++).i32 = (int32_t)(size - elem_done);
                }
                WAH_BULK_YIELD(instr_start);
            }
        }
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_INIT_DATA) {
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t);
    uint32_t typeidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t dataidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    uint32_t src_offset = (uint32_t)(--sp)->i32;
    uint32_t dst_offset = (uint32_t)(--sp)->i32;
    void *obj = (--sp)->ref;
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup);
    WAH_ASSERT(dataidx < fctx->module->data_segment_count);
    const wah_data_segment_t *seg = &fctx->module->data_segments[dataidx];
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj;
    WAH_ENSURE_GOTO((uint64_t)dst_offset + size <= body->length, WAH_ERROR_TRAP, cleanup);
    const wah_repr_info_t *info = fctx->module->repr_infos[fctx->module->typeidx_to_repr[typeidx]];
    uint32_t esz = info->size;
    uint32_t seg_len = wah_data_seg_data_len(fctx, dataidx);
    WAH_ENSURE_GOTO((uint64_t)src_offset + (uint64_t)size * esz <= seg_len, WAH_ERROR_TRAP, cleanup);
    uint8_t *elems = (uint8_t *)body + sizeof(wah_gc_array_body_t);
    size_t byte_size = (size_t)size * esz;
    for (size_t done = 0; done < byte_size; ) {
        size_t chunk = byte_size - done < WAH_BULK_CHECK_INTERVAL ? byte_size - done : WAH_BULK_CHECK_INTERVAL;
        chunk = (size_t)wah_bulk_fuel_limit(ctx, (uint64_t)chunk);
        if (chunk > esz) chunk -= chunk % esz; else chunk = esz;
        if (chunk > byte_size - done) chunk = byte_size - done;
        memcpy(elems + (size_t)dst_offset * esz + done, seg->data + src_offset + done, chunk);
        done += chunk;
        wah_bulk_fuel_charge(ctx, (uint64_t)chunk);
        if (done < byte_size && wah_bulk_should_stop(ctx)) {
            uint32_t elem_done = (uint32_t)(done / esz);
            (*sp++).ref = obj;
            (*sp++).i32 = (int32_t)(dst_offset + elem_done);
            (*sp++).i32 = (int32_t)(src_offset + done);
            (*sp++).i32 = (int32_t)(size - elem_done);
            WAH_BULK_YIELD(instr_start);
        }
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(ARRAY_INIT_ELEM) {
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t);
    uint32_t elemidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t);
    uint32_t size = (uint32_t)(--sp)->i32;
    uint32_t src_offset = (uint32_t)(--sp)->i32;
    uint32_t dst_offset = (uint32_t)(--sp)->i32;
    void *obj = (--sp)->ref;
    WAH_ENSURE_GOTO(obj != NULL, WAH_ERROR_TRAP, cleanup);
    WAH_ASSERT(elemidx < fctx->module->element_segment_count);
    const wah_element_segment_t *seg = &fctx->module->element_segments[elemidx];
    uint32_t seg_len = wah_elem_seg_num_elems(fctx, elemidx);
    wah_gc_array_body_t *body = (wah_gc_array_body_t *)obj;
    WAH_ENSURE_GOTO((uint64_t)dst_offset + size <= body->length, WAH_ERROR_TRAP, cleanup);
    WAH_ENSURE_GOTO((uint64_t)src_offset + size <= seg_len, WAH_ERROR_TRAP, cleanup);
    uint8_t *elems = (uint8_t *)body + sizeof(wah_gc_array_body_t);
    wah_error_t init_err;
    uint32_t done = wah_bulk_array_init_elem(ctx, elems, dst_offset, seg, src_offset, size, &init_err);
    if (init_err != WAH_OK) { err = init_err; goto cleanup; }
    if (done < size) {
        (*sp++).ref = obj;
        (*sp++).i32 = (int32_t)(dst_offset + done);
        (*sp++).i32 = (int32_t)(src_offset + done);
        (*sp++).i32 = (int32_t)(size - done);
        WAH_BULK_YIELD(instr_start);
    }
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(REF_I31) {
    sp[-1].ref = wah_ref_make_i31(sp[-1].i32);
    WAH_NEXT();
}

WAH_RUN(I31_GET_S) {
    WAH_ENSURE_GOTO(sp[-1].ref != NULL, WAH_ERROR_TRAP, cleanup);
    sp[-1].i32 = wah_ref_i31_get_s(sp[-1].ref);
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(I31_GET_U) {
    WAH_ENSURE_GOTO(sp[-1].ref != NULL, WAH_ERROR_TRAP, cleanup);
    sp[-1].i32 = wah_ref_i31_get_u(sp[-1].ref);
    WAH_NEXT();
    WAH_CLEANUP();
}

// Two hierarchies share the same representation so these are no-ops
WAH_NEVER_RUN(ANY_CONVERT_EXTERN) WAH_NEVER_RUN(EXTERN_CONVERT_ANY)

#else // !WAH_FEATURE_GC

#define WAH_GC_NEVER_RUN(name, ...) WAH_NEVER_RUN(name)
WAH_GC_OPCODES(WAH_GC_NEVER_RUN)
#undef WAH_GC_NEVER_RUN

#endif // WAH_FEATURE_GC

WAH_RUN(LOCAL_GET) {
    uint32_t local_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    *sp++ = frame->locals[local_idx];
    WAH_NEXT();
}

WAH_RUN(LOCAL_SET) {
    uint32_t local_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    frame->locals[local_idx] = *--sp;
    WAH_NEXT();
}

WAH_RUN(LOCAL_TEE) {
    uint32_t local_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    wah_value_t val = sp[-1];
    frame->locals[local_idx] = val;
    WAH_NEXT();
}

WAH_RUN(GLOBAL_GET) {
    uint32_t global_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    *sp++ = frame->frame_globals[global_idx];
    WAH_NEXT();
}

WAH_RUN(GLOBAL_SET) {
    uint32_t global_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    wah_ref_store_global_slot(&frame->frame_globals[global_idx], *--sp);
    WAH_NEXT();
}

#define WAH_TABLE_GET_IMPL(N) { \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint64_t elem_idx = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    WAH_ENSURE_GOTO(elem_idx < fctx->tables[table_idx].size, WAH_ERROR_TRAP, cleanup); \
    *sp++ = fctx->tables[table_idx].entries[elem_idx]; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_TABLE_SET_IMPL(N) { \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    wah_value_t val = *--sp; \
    uint64_t elem_idx = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    WAH_ENSURE_GOTO(elem_idx < fctx->tables[table_idx].size, WAH_ERROR_TRAP, cleanup); \
    wah_ref_store_table(fctx, table_idx, elem_idx, val); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_TABLE_SIZE_IMPL(N) { \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    (*sp++).i##N = (int##N##_t)fctx->tables[table_idx].size; \
    WAH_NEXT(); \
}

#define WAH_TABLE_GROW_IMPL(N) { \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    int64_t delta = (int64_t)(*--sp).i##N; \
    wah_value_t init_val = *--sp; \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    \
    if (delta < 0) { \
        (*sp++).i##N = -1; \
    } else { \
        uint64_t old_size; \
        bool grew; \
        WAH_CHECK_GOTO(wah_table_grow_internal(ctx, fctx, table_idx, (uint64_t)delta, init_val, &old_size, &grew), cleanup); \
        (*sp++).i##N = grew ? (int##N##_t)old_size : -1; \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_TABLE_FILL_IMPL(N) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint64_t size = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    wah_value_t val = *--sp; \
    uint64_t offset = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(offset, size, fctx->tables[table_idx].size), WAH_ERROR_TRAP, cleanup); \
    uint64_t done = wah_bulk_table_fill(ctx, fctx, table_idx, offset, val, size); \
    if (done < size) { \
        (*sp++).i##N = (int##N##_t)(offset + done); \
        *sp++ = val; \
        (*sp++).i##N = (int##N##_t)(size - done); \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_TABLE_COPY_IMPL(N,M,Z) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t dst_table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint32_t src_table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint64_t size = (uint64_t)(uint##Z##_t)(*--sp).i##Z; \
    uint64_t src_offset = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    uint64_t dst_offset = (uint64_t)(uint##M##_t)(*--sp).i##M; \
    WAH_ASSERT(src_table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    WAH_ASSERT(dst_table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(src_offset, size, fctx->tables[src_table_idx].size), WAH_ERROR_TRAP, cleanup); \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(dst_offset, size, fctx->tables[dst_table_idx].size), WAH_ERROR_TRAP, cleanup); \
    uint64_t done = wah_bulk_table_copy(ctx, fctx, dst_table_idx, dst_offset, src_table_idx, src_offset, size); \
    if (done < size) { \
        bool backward = (src_table_idx == dst_table_idx && dst_offset > src_offset && dst_offset < src_offset + size); \
        if (backward) { \
            (*sp++).i##M = (int##M##_t)dst_offset; \
            (*sp++).i##N = (int##N##_t)src_offset; \
        } else { \
            (*sp++).i##M = (int##M##_t)(dst_offset + done); \
            (*sp++).i##N = (int##N##_t)(src_offset + done); \
        } \
        (*sp++).i##Z = (int##Z##_t)(size - done); \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_TABLE_INIT_IMPL(N) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t elem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint32_t table_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint32_t size = (*--sp).i32; \
    uint32_t src_offset = (*--sp).i32; \
    uint64_t dst_offset = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    WAH_ASSERT(elem_idx < fctx->module->element_segment_count && "validation didn't catch out-of-bound element segment index"); \
    WAH_ASSERT(table_idx < fctx->table_count && "validation didn't catch out-of-bound table index"); \
    const wah_element_segment_t *segment = &fctx->module->element_segments[elem_idx]; \
    if (wah_elem_seg_is_dropped(fctx, elem_idx)) { \
        WAH_ENSURE_GOTO(size == 0 && src_offset == 0, WAH_ERROR_TRAP, cleanup); \
        WAH_ENSURE_GOTO(dst_offset <= fctx->tables[table_idx].size, WAH_ERROR_TRAP, cleanup); \
        WAH_NEXT(); \
    } \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(src_offset, size, segment->num_elems), WAH_ERROR_TRAP, cleanup); \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(dst_offset, size, fctx->tables[table_idx].size), WAH_ERROR_TRAP, cleanup); \
    wah_error_t init_err; \
    uint32_t done = wah_bulk_table_init(ctx, fctx, table_idx, dst_offset, segment, src_offset, size, &init_err); \
    if (init_err != WAH_OK) { err = init_err; goto cleanup; } \
    if (done < size) { \
        (*sp++).i##N = (int##N##_t)(dst_offset + done); \
        (*sp++).i32 = src_offset + done; \
        (*sp++).i32 = size - done; \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_RUN(TABLE_GET) WAH_TABLE_GET_IMPL(32)
WAH_RUN(TABLE_SET) WAH_TABLE_SET_IMPL(32)
WAH_RUN(TABLE_SIZE) WAH_TABLE_SIZE_IMPL(32)
WAH_RUN(TABLE_GROW) WAH_TABLE_GROW_IMPL(32)
WAH_RUN(TABLE_FILL) WAH_TABLE_FILL_IMPL(32)
WAH_RUN(TABLE_COPY) WAH_TABLE_COPY_IMPL(32,32,32)
WAH_RUN(TABLE_INIT) WAH_TABLE_INIT_IMPL(32)

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
WAH_RUN(TABLE_GET_i64) WAH_TABLE_GET_IMPL(64)
WAH_RUN(TABLE_SET_i64) WAH_TABLE_SET_IMPL(64)
WAH_RUN(TABLE_SIZE_i64) WAH_TABLE_SIZE_IMPL(64)
WAH_RUN(TABLE_GROW_i64) WAH_TABLE_GROW_IMPL(64)
WAH_RUN(TABLE_FILL_i64) WAH_TABLE_FILL_IMPL(64)
WAH_RUN(TABLE_COPY_i32_to_i64) WAH_TABLE_COPY_IMPL(32,64,32)
WAH_RUN(TABLE_COPY_i64_to_i32) WAH_TABLE_COPY_IMPL(64,32,32)
WAH_RUN(TABLE_COPY_i64_to_i64) WAH_TABLE_COPY_IMPL(64,64,64)
WAH_RUN(TABLE_INIT_i64) WAH_TABLE_INIT_IMPL(64)
#endif // WAH_FEATURE_MEMORY64 (i64 table ops)

WAH_RUN(ELEM_DROP) {
    uint32_t elem_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    WAH_ASSERT(elem_idx < fctx->module->element_segment_count && "validation didn't catch out-of-bound element segment index");

    wah_elem_seg_mark_dropped(fctx, elem_idx);
    WAH_NEXT();
}

// Inline host call: place results above params, call, move results down.
#define WAH_CALL_HOST_INLINE(the_fn) \
    do { \
        size_t nparams = (the_fn)->nparams; \
        size_t nresults = (the_fn)->nresults; \
        WAH_ASSERT(sp >= ctx->value_stack + nparams && "validation bug"); \
        wah_value_t *param_vals = sp - nparams; \
        wah_value_t *result_vals = sp; \
        WAH_ENSURE_GOTO((uint8_t *)(result_vals + nresults) <= (uint8_t *)ctx->frame_ptr, WAH_ERROR_STACK_OVERFLOW, cleanup); \
        memset(result_vals, 0, sizeof(wah_value_t) * nresults); \
        frame->bytecode_ip = bytecode_ip; \
        ctx->sp = sp; \
        WAH_CHECK_GOTO(wah_call_host_function_internal(ctx, (the_fn), param_vals, (uint32_t)nparams, result_vals), cleanup); \
        if (nresults > 0) { \
            memmove(param_vals, result_vals, sizeof(wah_value_t) * nresults); \
        } \
        sp = param_vals + nresults; \
    } while (0)

// Inline wasm call: push frame, init locals.
#define WAH_CALL_WASM_INLINE(fn_module_, local_idx_, func_type_, fn_ctx_) \
    do { \
        const wah_code_body_t *called_code_ = &(fn_module_)->code_bodies[local_idx_]; \
        wah_value_t *new_locals_ = sp - (func_type_)->param_count; \
        wah_value_t *preflight_top_ = new_locals_ + (func_type_)->param_count + called_code_->max_frame_slots; \
        frame->bytecode_ip = bytecode_ip; \
        WAH_CHECK_GOTO(wah_push_frame(ctx, (fn_module_), (local_idx_), new_locals_, (func_type_)->result_count, (fn_ctx_), preflight_top_), cleanup); \
        uint32_t num_locals_ = called_code_->local_count; \
        if (num_locals_ > 0) { \
            memset(sp, 0, sizeof(wah_value_t) * num_locals_); \
            sp += num_locals_; \
        } \
        RELOAD_FRAME(); \
    } while (0)

// Common type-check + dispatch for call_indirect family.
// CALL_HOST / CALL_WASM are invoked with (actual_fn) / (fn_module, local_idx, actual_func_type) in scope.
#define WAH_INDIRECT_BODY(func_table_idx, CALL_HOST, CALL_WASM) \
    WAH_ENSURE_GOTO(func_table_idx < fctx->tables[table_idx].size, WAH_ERROR_TRAP, cleanup); \
    void *_fn_ref = fctx->tables[table_idx].entries[func_table_idx].ref; \
    WAH_ENSURE_GOTO(_fn_ref != NULL, WAH_ERROR_TRAP, cleanup); \
    WAH_ASSERT(_fn_ref != wah_func_to_ref(wah_funcref_sentinel) && "prefuncref stored in table without conversion to funcref"); \
    const wah_function_t *actual_fn = wah_ref_to_func(_fn_ref); \
    WAH_REF_BODY(actual_fn, CALL_HOST, CALL_WASM)

// Common type-check + dispatch for call_ref family.
// Expects actual_fn, type_idx in scope. CALL_HOST / CALL_WASM same convention as WAH_INDIRECT_BODY.
#define WAH_REF_BODY(actual_fn, CALL_HOST, CALL_WASM) \
    const wah_func_type_t *expected_func_type = &fctx->module->types[type_idx]; \
    if ((actual_fn)->is_host) { \
        WAH_ENSURE_GOTO(expected_func_type->param_count == (actual_fn)->nparams && \
                        expected_func_type->result_count == (actual_fn)->nresults, WAH_ERROR_TRAP, cleanup); \
        { bool _types_ok = true; \
        for (uint32_t i = 0; i < expected_func_type->param_count; ++i) { \
            if (!wah_type_is_subtype(expected_func_type->param_types[i], (actual_fn)->param_types[i], fctx->module)) \
                { _types_ok = false; break; } \
        } \
        for (uint32_t i = 0; _types_ok && i < expected_func_type->result_count; ++i) { \
            if (!wah_type_is_subtype((actual_fn)->result_types[i], expected_func_type->result_types[i], fctx->module)) \
                { _types_ok = false; break; } \
        } \
        WAH_ENSURE_GOTO(_types_ok, WAH_ERROR_TRAP, cleanup); } \
        CALL_HOST; \
    } else { \
        const wah_module_t *fn_module = (actual_fn)->fn_module ? (actual_fn)->fn_module : fctx->module; \
        wah_exec_context_t *fn_ctx = (actual_fn)->fn_ctx; \
        uint32_t local_idx = (actual_fn)->local_idx; \
        uint32_t actual_type_idx = fn_module->function_type_indices[local_idx]; \
        const wah_func_type_t *actual_func_type = &fn_module->types[actual_type_idx]; \
        WAH_ENSURE_GOTO(wah_cross_module_subtype_cached(ctx, fn_module, WAH_TYPE_FROM_IDX(actual_type_idx, 0), \
                                                        fctx->module, WAH_TYPE_FROM_IDX(type_idx, 0)), WAH_ERROR_TRAP, cleanup); \
        CALL_WASM; \
    }

WAH_RUN(CALL) {
    uint32_t called_func_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);

    WAH_ASSERT(called_func_idx < frame->frame_function_table_count && "validation didn't catch out-of-bound function index");
    const wah_function_t *called_fn = &frame->frame_function_table[called_func_idx];

    if (called_fn->is_host) {
        WAH_CALL_HOST_INLINE(called_fn);
    } else {
        const wah_module_t *fn_module = called_fn->fn_module;
        if (!fn_module) fn_module = frame->module;
        uint32_t local_idx = called_fn->local_idx;
        const wah_func_type_t *called_func_type = &fn_module->types[fn_module->function_type_indices[local_idx]];
        WAH_CALL_WASM_INLINE(fn_module, local_idx, called_func_type, NULL);
    }

    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(CALL_INDIRECT) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t table_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t func_table_idx = (uint32_t)(*--sp).i32;
    WAH_ASSERT(type_idx < fctx->module->type_count);
    WAH_ASSERT(table_idx < fctx->table_count);
    WAH_INDIRECT_BODY(func_table_idx,
        WAH_CALL_HOST_INLINE(actual_fn),
        WAH_CALL_WASM_INLINE(fn_module, local_idx, actual_func_type, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
WAH_RUN(CALL_INDIRECT_i64) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t table_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint64_t func_table_idx = (uint64_t)(*--sp).i64;
    WAH_ASSERT(type_idx < fctx->module->type_count);
    WAH_ASSERT(table_idx < fctx->table_count);
    WAH_INDIRECT_BODY(func_table_idx,
        WAH_CALL_HOST_INLINE(actual_fn),
        WAH_CALL_WASM_INLINE(fn_module, local_idx, actual_func_type, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}
#endif // WAH_FEATURE_MEMORY64

WAH_RUN(CALL_REF) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    void *_fn_ref = (*--sp).ref;
    WAH_ENSURE_GOTO(_fn_ref != NULL, WAH_ERROR_TRAP, cleanup);
    WAH_ASSERT(_fn_ref != wah_func_to_ref(wah_funcref_sentinel) && "prefuncref stored without conversion to funcref");
    const wah_function_t *actual_fn = wah_ref_to_func(_fn_ref);
    WAH_REF_BODY(actual_fn,
        WAH_CALL_HOST_INLINE(actual_fn),
        WAH_CALL_WASM_INLINE(fn_module, local_idx, actual_func_type, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_TAIL_CALL)

// Tail-call helper: reuse current frame for a wasm-to-wasm call.
// Moves params to current frame's locals area, updates frame in-place.
#define WAH_TAIL_CALL_WASM(fn_module_, local_idx_, param_count_, result_count_, fn_ctx_) \
    do { \
        const wah_module_t *tc_module = (fn_module_); \
        wah_exec_context_t *tc_ctx = (fn_ctx_); \
        uint32_t tc_local_idx = (local_idx_); \
        uint32_t tc_param_count = (param_count_); \
        const wah_code_body_t *tc_code = &tc_module->code_bodies[tc_local_idx]; \
        wah_value_t *tc_params_src = sp - tc_param_count; \
        wah_value_t *tc_locals_dst = frame->locals; \
        WAH_ENSURE_GOTO((uint8_t *)(tc_locals_dst + tc_param_count + tc_code->max_frame_slots) <= (uint8_t *)ctx->frame_ptr, \
                        WAH_ERROR_STACK_OVERFLOW, cleanup); \
        if (tc_param_count > 0) { \
            memmove(tc_locals_dst, tc_params_src, sizeof(wah_value_t) * tc_param_count); \
        } \
        sp = tc_locals_dst + tc_param_count; \
        uint32_t tc_num_locals = tc_code->local_count; \
        if (tc_num_locals > 0) { \
            memset(sp, 0, sizeof(wah_value_t) * tc_num_locals); \
            sp += tc_num_locals; \
        } \
        WAH_CHECK_GOTO(wah_init_wasm_frame(ctx, frame, tc_module, tc_local_idx, tc_code, tc_locals_dst, (result_count_), tc_ctx), cleanup); \
        bytecode_ip = frame->bytecode_ip; \
        bytecode_base = frame->code->parsed_code.bytecode; \
        fctx = frame->frame_ctx; \
    } while (0)

// Tail-call helper for host functions: perform return, then call host function in caller context.
#define WAH_TAIL_CALL_HOST(called_fn_) \
    do { \
        const wah_function_t *tc_fn = (called_fn_); \
        size_t tc_nparams = tc_fn->nparams; \
        size_t tc_nresults = tc_fn->nresults; \
        wah_value_t *tc_params_src = sp - tc_nparams; \
        wah_value_t *tc_locals_dst = frame->locals; \
        if (tc_nparams > 0) { \
            memmove(tc_locals_dst, tc_params_src, sizeof(wah_value_t) * tc_nparams); \
        } \
        sp = tc_locals_dst + tc_nparams; \
        ctx->call_depth--; \
        ctx->frame_ptr++; \
        wah_value_t *tc_param_vals = sp - tc_nparams; \
        wah_value_t *tc_result_vals = sp; \
        WAH_ENSURE_GOTO((uint8_t *)(tc_result_vals + tc_nresults) <= (uint8_t *)ctx->frame_ptr, WAH_ERROR_STACK_OVERFLOW, cleanup); \
        memset(tc_result_vals, 0, sizeof(wah_value_t) * tc_nresults); \
        frame->bytecode_ip = bytecode_ip; \
        ctx->sp = sp; \
        WAH_CHECK_GOTO(wah_call_host_function_internal(ctx, tc_fn, tc_param_vals, (uint32_t)tc_nparams, tc_result_vals), cleanup); \
        if (tc_nresults > 0) { \
            memmove(tc_param_vals, tc_result_vals, sizeof(wah_value_t) * tc_nresults); \
        } \
        sp = tc_param_vals + tc_nresults; \
        RELOAD_FRAME(); \
    } while (0)

WAH_RUN(RETURN_CALL) {
    uint32_t called_func_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);

    while (ctx->exception_handler_depth > 0 &&
           ctx->exception_handlers[ctx->exception_handler_depth - 1].call_depth >= ctx->call_depth) {
        ctx->exception_handler_depth--;
    }

    WAH_ASSERT(called_func_idx < frame->frame_function_table_count && "validation didn't catch out-of-bound function index");
    const wah_function_t *called_fn = &frame->frame_function_table[called_func_idx];

    if (called_fn->is_host) {
        WAH_TAIL_CALL_HOST(called_fn);
    } else {
        const wah_module_t *fn_module = called_fn->fn_module;
        if (!fn_module) fn_module = frame->module;
        uint32_t local_idx = called_fn->local_idx;
        const wah_func_type_t *called_func_type = &fn_module->types[fn_module->function_type_indices[local_idx]];
        WAH_TAIL_CALL_WASM(fn_module, local_idx, called_func_type->param_count, called_func_type->result_count, called_fn->fn_ctx);
    }

    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(RETURN_CALL_INDIRECT) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t table_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t func_table_idx = (uint32_t)(*--sp).i32;
    WAH_ASSERT(type_idx < fctx->module->type_count);
    WAH_ASSERT(table_idx < fctx->table_count);
    while (ctx->exception_handler_depth > 0 &&
           ctx->exception_handlers[ctx->exception_handler_depth - 1].call_depth >= ctx->call_depth) {
        ctx->exception_handler_depth--;
    }
    WAH_INDIRECT_BODY(func_table_idx,
        WAH_TAIL_CALL_HOST(actual_fn),
        WAH_TAIL_CALL_WASM(fn_module, local_idx, actual_func_type->param_count, actual_func_type->result_count, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
WAH_RUN(RETURN_CALL_INDIRECT_i64) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint32_t table_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint64_t func_table_idx = (uint64_t)(*--sp).i64;
    WAH_ASSERT(type_idx < fctx->module->type_count);
    WAH_ASSERT(table_idx < fctx->table_count);
    while (ctx->exception_handler_depth > 0 &&
           ctx->exception_handlers[ctx->exception_handler_depth - 1].call_depth >= ctx->call_depth) {
        ctx->exception_handler_depth--;
    }
    WAH_INDIRECT_BODY(func_table_idx,
        WAH_TAIL_CALL_HOST(actual_fn),
        WAH_TAIL_CALL_WASM(fn_module, local_idx, actual_func_type->param_count, actual_func_type->result_count, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}
#endif // WAH_FEATURE_MEMORY64

WAH_RUN(RETURN_CALL_REF) {
    uint32_t type_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    void *_fn_ref = (*--sp).ref;
    WAH_ENSURE_GOTO(_fn_ref != NULL, WAH_ERROR_TRAP, cleanup);
    WAH_ASSERT(_fn_ref != wah_func_to_ref(wah_funcref_sentinel) && "prefuncref stored without conversion to funcref");
    const wah_function_t *actual_fn = wah_ref_to_func(_fn_ref);
    while (ctx->exception_handler_depth > 0 &&
           ctx->exception_handlers[ctx->exception_handler_depth - 1].call_depth >= ctx->call_depth) {
        ctx->exception_handler_depth--;
    }
    WAH_REF_BODY(actual_fn,
        WAH_TAIL_CALL_HOST(actual_fn),
        WAH_TAIL_CALL_WASM(fn_module, local_idx, actual_func_type->param_count, actual_func_type->result_count, fn_ctx))
    WAH_NEXT();
    WAH_CLEANUP();
}

#else // !WAH_FEATURE_TAIL_CALL
WAH_NEVER_RUN(RETURN_CALL) WAH_NEVER_RUN(RETURN_CALL_INDIRECT)
WAH_IF_MEMORY64(WAH_NEVER_RUN(RETURN_CALL_INDIRECT_i64))
WAH_NEVER_RUN(RETURN_CALL_REF)
#endif // WAH_FEATURE_TAIL_CALL

#define WAH_RETURN_CURRENT_FRAME() do { \
    while (ctx->exception_handler_depth > 0 && \
           ctx->exception_handlers[ctx->exception_handler_depth - 1].call_depth >= ctx->call_depth) { \
        ctx->exception_handler_depth--; \
    } \
    uint32_t results_to_keep_ = frame->result_count; \
    wah_value_t *results_src_ = sp - results_to_keep_; \
    sp = frame->locals; \
    ctx->call_depth--; \
    ctx->frame_ptr++; \
    if (results_to_keep_ == 1) { \
        *sp++ = *results_src_; \
    } else if (results_to_keep_ > 0) { \
        memmove(sp, results_src_, sizeof(wah_value_t) * results_to_keep_); \
        sp += results_to_keep_; \
    } \
    RELOAD_FRAME(); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
} while (0)

WAH_RUN(RETURN) {
    WAH_RETURN_CURRENT_FRAME();
}

WAH_RUN(END) { // End of function
    WAH_RETURN_CURRENT_FRAME();
}

#define BINOP_I(N,op) { sp[-2].i##N = (int##N##_t)((uint##N##_t)sp[-2].i##N op (uint##N##_t)sp[-1].i##N); sp--; WAH_NEXT(); }
#define CMP_I_S(N,op) { sp[-2].i32 = sp[-2].i##N op sp[-1].i##N ? 1 : 0; sp--; WAH_NEXT(); }
#define CMP_I_U(N,op) { sp[-2].i32 = (uint##N##_t)sp[-2].i##N op (uint##N##_t)sp[-1].i##N ? 1 : 0; sp--; WAH_NEXT(); }
#define BINOP_F(N,op) { sp[-2].f##N = wah_canonicalize_f##N(sp[-2].f##N op sp[-1].f##N); sp--; WAH_NEXT(); }
#define CMP_F(N,op)   { sp[-2].i32 = sp[-2].f##N op sp[-1].f##N ? 1 : 0; sp--; WAH_NEXT(); }
#define UNOP_I_FN(N,fn)  { sp[-1].i##N = (int##N##_t)fn((uint##N##_t)sp[-1].i##N); WAH_NEXT(); }
#define BINOP_I_FN(N,fn) { sp[-2].i##N = (int##N##_t)fn((uint##N##_t)sp[-2].i##N, (uint##N##_t)sp[-1].i##N); sp--; WAH_NEXT(); }
#define UNOP_F_FN(N,fn)  { sp[-1].f##N = wah_canonicalize_f##N(fn(sp[-1].f##N)); WAH_NEXT(); }
#define BINOP_F_FN(N,fn) { sp[-2].f##N = wah_canonicalize_f##N(fn(sp[-2].f##N, sp[-1].f##N)); sp--; WAH_NEXT(); }

#define NUM_OPS(N,_F) \
    WAH_RUN(I##N##_CLZ) UNOP_I_FN(N, wah_clz_u##N) \
    WAH_RUN(I##N##_CTZ) UNOP_I_FN(N, wah_ctz_u##N) \
    WAH_RUN(I##N##_POPCNT) UNOP_I_FN(N, wah_popcount_u##N) \
    WAH_RUN(I##N##_ADD) BINOP_I(N,+) \
    WAH_RUN(I##N##_SUB) BINOP_I(N,-) \
    WAH_RUN(I##N##_MUL) BINOP_I(N,*) \
    WAH_RUN(I##N##_DIV_S) {  \
        WAH_ENSURE_GOTO(sp[-1].i##N != 0, WAH_ERROR_TRAP, cleanup); \
        WAH_ENSURE_GOTO(sp[-2].i##N != INT##N##_MIN || sp[-1].i##N != -1, WAH_ERROR_TRAP, cleanup); \
        sp[-2].i##N /= sp[-1].i##N; sp--; WAH_NEXT(); \
        WAH_CLEANUP(); \
    } \
    WAH_RUN(I##N##_DIV_U) {  \
        WAH_ENSURE_GOTO(sp[-1].i##N != 0, WAH_ERROR_TRAP, cleanup); \
        sp[-2].i##N = (int##N##_t)((uint##N##_t)sp[-2].i##N / (uint##N##_t)sp[-1].i##N); \
        sp--; \
        WAH_NEXT(); \
        WAH_CLEANUP(); \
    } \
    WAH_RUN(I##N##_REM_S) {  \
        WAH_ENSURE_GOTO(sp[-1].i##N != 0, WAH_ERROR_TRAP, cleanup); \
        if (sp[-2].i##N == INT##N##_MIN && sp[-1].i##N == -1) sp[-2].i##N = 0; else sp[-2].i##N %= sp[-1].i##N; \
        sp--; \
        WAH_NEXT(); \
        WAH_CLEANUP(); \
    } \
    WAH_RUN(I##N##_REM_U) {  \
        WAH_ENSURE_GOTO(sp[-1].i##N != 0, WAH_ERROR_TRAP, cleanup); \
        sp[-2].i##N = (int##N##_t)((uint##N##_t)sp[-2].i##N % (uint##N##_t)sp[-1].i##N); \
        sp--; \
        WAH_NEXT(); \
        WAH_CLEANUP(); \
    } \
    WAH_RUN(I##N##_AND) BINOP_I(N,&) \
    WAH_RUN(I##N##_OR)  BINOP_I(N,|) \
    WAH_RUN(I##N##_XOR) BINOP_I(N,^) \
    WAH_RUN(I##N##_SHL) { sp[-2].i##N = (int##N##_t)((uint##N##_t)sp[-2].i##N << (sp[-1].i##N & (N-1))); sp--; WAH_NEXT(); } \
    WAH_RUN(I##N##_SHR_S) { sp[-2].i##N >>= (sp[-1].i##N & (N-1)); sp--; WAH_NEXT(); } \
    WAH_RUN(I##N##_SHR_U) { sp[-2].i##N = (int##N##_t)((uint##N##_t)sp[-2].i##N >> (sp[-1].i##N & (N-1))); sp--; WAH_NEXT(); } \
    WAH_RUN(I##N##_ROTL) BINOP_I_FN(N, wah_rotl_u##N) \
    WAH_RUN(I##N##_ROTR) BINOP_I_FN(N, wah_rotr_u##N) \
    \
    WAH_RUN(I##N##_EQ)   CMP_I_S(N,==) \
    WAH_RUN(I##N##_NE)   CMP_I_S(N,!=) \
    WAH_RUN(I##N##_LT_S) CMP_I_S(N,<) \
    WAH_RUN(I##N##_LT_U) CMP_I_U(N,<) \
    WAH_RUN(I##N##_GT_S) CMP_I_S(N,>) \
    WAH_RUN(I##N##_GT_U) CMP_I_U(N,>) \
    WAH_RUN(I##N##_LE_S) CMP_I_S(N,<=) \
    WAH_RUN(I##N##_LE_U) CMP_I_U(N,<=) \
    WAH_RUN(I##N##_GE_S) CMP_I_S(N,>=) \
    WAH_RUN(I##N##_GE_U) CMP_I_U(N,>=) \
    WAH_RUN(I##N##_EQZ) { sp[-1].i32 = (sp[-1].i##N == 0) ? 1 : 0; WAH_NEXT(); } \
    \
    WAH_RUN(F##N##_ABS) { sp[-1].f##N = wah_fabs##_F(sp[-1].f##N); WAH_NEXT(); } \
    WAH_RUN(F##N##_NEG) { sp[-1].f##N = -(sp[-1].f##N); WAH_NEXT(); } \
    WAH_RUN(F##N##_CEIL) UNOP_F_FN(N, ceil##_F) \
    WAH_RUN(F##N##_FLOOR) UNOP_F_FN(N, floor##_F) \
    WAH_RUN(F##N##_TRUNC) UNOP_F_FN(N, trunc##_F) \
    WAH_RUN(F##N##_NEAREST) UNOP_F_FN(N, wah_nearest_f##N) \
    WAH_RUN(F##N##_SQRT) UNOP_F_FN(N, wah_sqrt##_F) \
    WAH_RUN(F##N##_ADD) BINOP_F(N,+) \
    WAH_RUN(F##N##_SUB) BINOP_F(N,-) \
    WAH_RUN(F##N##_MUL) BINOP_F(N,*) \
    WAH_RUN(F##N##_DIV) BINOP_F(N,/) /* Let hardware handle division by zero (NaN/inf) */ \
    WAH_RUN(F##N##_EQ) CMP_F(N,==) \
    WAH_RUN(F##N##_NE) CMP_F(N,!=) \
    WAH_RUN(F##N##_LT) CMP_F(N,<) \
    WAH_RUN(F##N##_GT) CMP_F(N,>) \
    WAH_RUN(F##N##_LE) CMP_F(N,<=) \
    WAH_RUN(F##N##_GE) CMP_F(N,>=) \
    WAH_RUN(F##N##_MIN) BINOP_F_FN(N, wah_min##_F) \
    WAH_RUN(F##N##_MAX) BINOP_F_FN(N, wah_max##_F) \
    WAH_RUN(F##N##_COPYSIGN) { sp[-2].f##N = copysign##_F(sp[-2].f##N, sp[-1].f##N); sp--; WAH_NEXT(); }

#define WAH_SP_ADDR_I32 ((uint64_t)(uint32_t)(*--sp).i32)
#define WAH_SP_ADDR_I64 ((uint64_t)(*--sp).i64)

#define LOAD_OP(mem0, addr_expr, N, T, value_field, cast) { \
    uint32_t memidx = 0; \
    if (!(mem0)) { memidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); } \
    uint64_t offset = wah_read_u64_le(bytecode_ip); \
    bytecode_ip += sizeof(uint64_t); \
    uint64_t addr = (addr_expr); \
    uint64_t effective_addr; \
    if (!(mem0)) { WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); } \
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, N/8, \
        (mem0) ? fctx->memory_size : fctx->memories[memidx].size, &effective_addr), cleanup); \
    (*sp++).value_field = cast wah_read_##T##_le( \
        ((mem0) ? fctx->memory_base : fctx->memories[memidx].data) + effective_addr); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define STORE_OP(mem0, addr_expr, N, T, value_field, value_type, cast) { \
    uint32_t memidx = 0; \
    if (!(mem0)) { memidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); } \
    uint64_t offset = wah_read_u64_le(bytecode_ip); \
    bytecode_ip += sizeof(uint64_t); \
    value_type val = (*--sp).value_field; \
    uint64_t addr = (addr_expr); \
    uint64_t effective_addr; \
    if (!(mem0)) { WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); } \
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, N/8, \
        (mem0) ? fctx->memory_size : fctx->memories[memidx].size, &effective_addr), cleanup); \
    wah_write_##T##_le( \
        ((mem0) ? fctx->memory_base : fctx->memories[memidx].data) + effective_addr, cast (val)); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define CONVERT(from_field, cast, to_field) { \
    sp[-1].to_field = cast (sp[-1].from_field); \
    WAH_NEXT(); \
}
#define CONVERT_CHECK(from_field, call, ty, cast, to_field) { \
    ty res; \
    WAH_CHECK_GOTO(call(sp[-1].from_field, &res), cleanup); \
    sp[-1].to_field = cast (res); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}
#define REINTERPRET(from_field, from_ty, to_field, to_ty) { \
    union { from_ty from; to_ty to; } u = { .from = sp[-1].from_field }; \
    sp[-1].to_field = u.to; \
    WAH_NEXT(); \
}

NUM_OPS(32,f)
NUM_OPS(64,)

WAH_RUN(I32_LOAD) LOAD_OP(0, WAH_SP_ADDR_I32, 32, u32, i32, (int32_t))
WAH_RUN(I64_LOAD) LOAD_OP(0, WAH_SP_ADDR_I32, 64, u64, i64, (int64_t))
WAH_RUN(F32_LOAD) LOAD_OP(0, WAH_SP_ADDR_I32, 32, f32, f32, )
WAH_RUN(F64_LOAD) LOAD_OP(0, WAH_SP_ADDR_I32, 64, f64, f64, )
WAH_RUN(I32_LOAD8_S) LOAD_OP(0, WAH_SP_ADDR_I32, 8, u8, i32, (int32_t)(int8_t))
WAH_RUN(I32_LOAD8_U) LOAD_OP(0, WAH_SP_ADDR_I32, 8, u8, i32, (int32_t))
WAH_RUN(I32_LOAD16_S) LOAD_OP(0, WAH_SP_ADDR_I32, 16, u16, i32, (int32_t)(int16_t))
WAH_RUN(I32_LOAD16_U) LOAD_OP(0, WAH_SP_ADDR_I32, 16, u16, i32, (int32_t))
WAH_RUN(I64_LOAD8_S) LOAD_OP(0, WAH_SP_ADDR_I32, 8, u8, i64, (int64_t)(int8_t))
WAH_RUN(I64_LOAD8_U) LOAD_OP(0, WAH_SP_ADDR_I32, 8, u8, i64, (int64_t))
WAH_RUN(I64_LOAD16_S) LOAD_OP(0, WAH_SP_ADDR_I32, 16, u16, i64, (int64_t)(int16_t))
WAH_RUN(I64_LOAD16_U) LOAD_OP(0, WAH_SP_ADDR_I32, 16, u16, i64, (int64_t))
WAH_RUN(I64_LOAD32_S) LOAD_OP(0, WAH_SP_ADDR_I32, 32, u32, i64, (int64_t)(int32_t))
WAH_RUN(I64_LOAD32_U) LOAD_OP(0, WAH_SP_ADDR_I32, 32, u32, i64, (int64_t))

WAH_RUN(I32_LOAD_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 32, u32, i32, (int32_t))
WAH_RUN(I64_LOAD_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 64, u64, i64, (int64_t))
WAH_RUN(F32_LOAD_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 32, f32, f32, )
WAH_RUN(F64_LOAD_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 64, f64, f64, )
WAH_RUN(I32_LOAD8_S_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 8, u8, i32, (int32_t)(int8_t))
WAH_RUN(I32_LOAD8_U_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 8, u8, i32, (int32_t))
WAH_RUN(I32_LOAD16_S_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 16, u16, i32, (int32_t)(int16_t))
WAH_RUN(I32_LOAD16_U_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 16, u16, i32, (int32_t))
WAH_RUN(I64_LOAD8_S_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 8, u8, i64, (int64_t)(int8_t))
WAH_RUN(I64_LOAD8_U_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 8, u8, i64, (int64_t))
WAH_RUN(I64_LOAD16_S_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 16, u16, i64, (int64_t)(int16_t))
WAH_RUN(I64_LOAD16_U_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 16, u16, i64, (int64_t))
WAH_RUN(I64_LOAD32_S_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 32, u32, i64, (int64_t)(int32_t))
WAH_RUN(I64_LOAD32_U_i32_mem0) LOAD_OP(1, WAH_SP_ADDR_I32, 32, u32, i64, (int64_t))

WAH_RUN(I32_STORE) STORE_OP(0, WAH_SP_ADDR_I32, 32, u32, i32, int32_t, (uint32_t))
WAH_RUN(I64_STORE) STORE_OP(0, WAH_SP_ADDR_I32, 64, u64, i64, int64_t, (uint64_t))
WAH_RUN(F32_STORE) STORE_OP(0, WAH_SP_ADDR_I32, 32, f32, f32, float, )
WAH_RUN(F64_STORE) STORE_OP(0, WAH_SP_ADDR_I32, 64, f64, f64, double, )
WAH_RUN(I32_STORE8) STORE_OP(0, WAH_SP_ADDR_I32, 8, u8, i32, int32_t, (uint8_t))
WAH_RUN(I32_STORE16) STORE_OP(0, WAH_SP_ADDR_I32, 16, u16, i32, int32_t, (uint16_t))
WAH_RUN(I64_STORE8) STORE_OP(0, WAH_SP_ADDR_I32, 8, u8, i64, int64_t, (uint8_t))
WAH_RUN(I64_STORE16) STORE_OP(0, WAH_SP_ADDR_I32, 16, u16, i64, int64_t, (uint16_t))
WAH_RUN(I64_STORE32) STORE_OP(0, WAH_SP_ADDR_I32, 32, u32, i64, int64_t, (uint32_t))

WAH_RUN(I32_STORE_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 32, u32, i32, int32_t, (uint32_t))
WAH_RUN(I64_STORE_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 64, u64, i64, int64_t, (uint64_t))
WAH_RUN(F32_STORE_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 32, f32, f32, float, )
WAH_RUN(F64_STORE_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 64, f64, f64, double, )
WAH_RUN(I32_STORE8_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 8, u8, i32, int32_t, (uint8_t))
WAH_RUN(I32_STORE16_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 16, u16, i32, int32_t, (uint16_t))
WAH_RUN(I64_STORE8_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 8, u8, i64, int64_t, (uint8_t))
WAH_RUN(I64_STORE16_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 16, u16, i64, int64_t, (uint16_t))
WAH_RUN(I64_STORE32_i32_mem0) STORE_OP(1, WAH_SP_ADDR_I32, 32, u32, i64, int64_t, (uint32_t))

// --- i64-addressed memory loads/stores (non-mem0) ---
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
WAH_RUN(I32_LOAD_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 32, u32, i32, (int32_t))
WAH_RUN(I64_LOAD_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 64, u64, i64, (int64_t))
WAH_RUN(F32_LOAD_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 32, f32, f32, )
WAH_RUN(F64_LOAD_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 64, f64, f64, )
WAH_RUN(I32_LOAD8_S_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 8, u8, i32, (int32_t)(int8_t))
WAH_RUN(I32_LOAD8_U_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 8, u8, i32, (int32_t))
WAH_RUN(I32_LOAD16_S_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 16, u16, i32, (int32_t)(int16_t))
WAH_RUN(I32_LOAD16_U_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 16, u16, i32, (int32_t))
WAH_RUN(I64_LOAD8_S_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 8, u8, i64, (int64_t)(int8_t))
WAH_RUN(I64_LOAD8_U_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 8, u8, i64, (int64_t))
WAH_RUN(I64_LOAD16_S_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 16, u16, i64, (int64_t)(int16_t))
WAH_RUN(I64_LOAD16_U_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 16, u16, i64, (int64_t))
WAH_RUN(I64_LOAD32_S_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 32, u32, i64, (int64_t)(int32_t))
WAH_RUN(I64_LOAD32_U_i64) LOAD_OP(0, WAH_SP_ADDR_I64, 32, u32, i64, (int64_t))

WAH_RUN(I32_STORE_i64) STORE_OP(0, WAH_SP_ADDR_I64, 32, u32, i32, int32_t, (uint32_t))
WAH_RUN(I64_STORE_i64) STORE_OP(0, WAH_SP_ADDR_I64, 64, u64, i64, int64_t, (uint64_t))
WAH_RUN(F32_STORE_i64) STORE_OP(0, WAH_SP_ADDR_I64, 32, f32, f32, float, )
WAH_RUN(F64_STORE_i64) STORE_OP(0, WAH_SP_ADDR_I64, 64, f64, f64, double, )
WAH_RUN(I32_STORE8_i64) STORE_OP(0, WAH_SP_ADDR_I64, 8, u8, i32, int32_t, (uint8_t))
WAH_RUN(I32_STORE16_i64) STORE_OP(0, WAH_SP_ADDR_I64, 16, u16, i32, int32_t, (uint16_t))
WAH_RUN(I64_STORE8_i64) STORE_OP(0, WAH_SP_ADDR_I64, 8, u8, i64, int64_t, (uint8_t))
WAH_RUN(I64_STORE16_i64) STORE_OP(0, WAH_SP_ADDR_I64, 16, u16, i64, int64_t, (uint16_t))
WAH_RUN(I64_STORE32_i64) STORE_OP(0, WAH_SP_ADDR_I64, 32, u32, i64, int64_t, (uint32_t))

// --- i64-addressed memory loads/stores (mem0 fast-path) ---
WAH_RUN(I32_LOAD_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 32, u32, i32, (int32_t))
WAH_RUN(I64_LOAD_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 64, u64, i64, (int64_t))
WAH_RUN(F32_LOAD_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 32, f32, f32, )
WAH_RUN(F64_LOAD_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 64, f64, f64, )
WAH_RUN(I32_LOAD8_S_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 8, u8, i32, (int32_t)(int8_t))
WAH_RUN(I32_LOAD8_U_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 8, u8, i32, (int32_t))
WAH_RUN(I32_LOAD16_S_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 16, u16, i32, (int32_t)(int16_t))
WAH_RUN(I32_LOAD16_U_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 16, u16, i32, (int32_t))
WAH_RUN(I64_LOAD8_S_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 8, u8, i64, (int64_t)(int8_t))
WAH_RUN(I64_LOAD8_U_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 8, u8, i64, (int64_t))
WAH_RUN(I64_LOAD16_S_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 16, u16, i64, (int64_t)(int16_t))
WAH_RUN(I64_LOAD16_U_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 16, u16, i64, (int64_t))
WAH_RUN(I64_LOAD32_S_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 32, u32, i64, (int64_t)(int32_t))
WAH_RUN(I64_LOAD32_U_i64_mem0) LOAD_OP(1, WAH_SP_ADDR_I64, 32, u32, i64, (int64_t))

WAH_RUN(I32_STORE_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 32, u32, i32, int32_t, (uint32_t))
WAH_RUN(I64_STORE_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 64, u64, i64, int64_t, (uint64_t))
WAH_RUN(F32_STORE_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 32, f32, f32, float, )
WAH_RUN(F64_STORE_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 64, f64, f64, double, )
WAH_RUN(I32_STORE8_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 8, u8, i32, int32_t, (uint8_t))
WAH_RUN(I32_STORE16_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 16, u16, i32, int32_t, (uint16_t))
WAH_RUN(I64_STORE8_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 8, u8, i64, int64_t, (uint8_t))
WAH_RUN(I64_STORE16_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 16, u16, i64, int64_t, (uint16_t))
WAH_RUN(I64_STORE32_i64_mem0) STORE_OP(1, WAH_SP_ADDR_I64, 32, u32, i64, int64_t, (uint32_t))
#endif // WAH_FEATURE_MEMORY64 (i64 memory loads/stores)

WAH_RUN(I32_WRAP_I64) CONVERT(i64, (int32_t), i32)
WAH_RUN(I32_TRUNC_F32_S) CONVERT_CHECK(f32, wah_trunc_f32_to_i32, int32_t, , i32)
WAH_RUN(I32_TRUNC_F32_U) CONVERT_CHECK(f32, wah_trunc_f32_to_u32, uint32_t, (int32_t), i32)
WAH_RUN(I32_TRUNC_F64_S) CONVERT_CHECK(f64, wah_trunc_f64_to_i32, int32_t, , i32)
WAH_RUN(I32_TRUNC_F64_U) CONVERT_CHECK(f64, wah_trunc_f64_to_u32, uint32_t, (int32_t), i32)

WAH_RUN(I64_EXTEND_I32_S) CONVERT(i32, (int64_t), i64)
WAH_RUN(I64_EXTEND_I32_U) CONVERT(i32, (int64_t)(uint32_t), i64)
WAH_RUN(I64_TRUNC_F32_S) CONVERT_CHECK(f32, wah_trunc_f32_to_i64, int64_t, , i64)
WAH_RUN(I64_TRUNC_F32_U) CONVERT_CHECK(f32, wah_trunc_f32_to_u64, uint64_t, (int64_t), i64)
WAH_RUN(I64_TRUNC_F64_S) CONVERT_CHECK(f64, wah_trunc_f64_to_i64, int64_t, , i64)
WAH_RUN(I64_TRUNC_F64_U) CONVERT_CHECK(f64, wah_trunc_f64_to_u64, uint64_t, (int64_t), i64)

WAH_RUN(F32_CONVERT_I32_S) CONVERT(i32, (float), f32)
WAH_RUN(F32_CONVERT_I32_U) CONVERT(i32, (float)(uint32_t), f32)
WAH_RUN(F32_CONVERT_I64_S) CONVERT(i64, (float), f32)
WAH_RUN(F32_CONVERT_I64_U) CONVERT(i64, (float)(uint64_t), f32)
WAH_RUN(F32_DEMOTE_F64) { sp[-1].f32 = wah_canonicalize_f32((float)sp[-1].f64); WAH_NEXT(); }

WAH_RUN(F64_CONVERT_I32_S) CONVERT(i32, (double), f64)
WAH_RUN(F64_CONVERT_I32_U) CONVERT(i32, (double)(uint32_t), f64)
WAH_RUN(F64_CONVERT_I64_S) CONVERT(i64, (double), f64)
WAH_RUN(F64_CONVERT_I64_U) CONVERT(i64, (double)(uint64_t), f64)
WAH_RUN(F64_PROMOTE_F32) { sp[-1].f64 = wah_canonicalize_f64((double)sp[-1].f32); WAH_NEXT(); }

WAH_RUN(I32_REINTERPRET_F32) REINTERPRET(f32, float, i32, int32_t)
WAH_RUN(I64_REINTERPRET_F64) REINTERPRET(f64, double, i64, int64_t)
WAH_RUN(F32_REINTERPRET_I32) REINTERPRET(i32, int32_t, f32, float)
WAH_RUN(F64_REINTERPRET_I64) REINTERPRET(i64, int64_t, f64, double)

WAH_RUN(I32_EXTEND8_S)  { sp[-1].i32 = (int32_t) (int8_t)sp[-1].i32; WAH_NEXT(); }
WAH_RUN(I32_EXTEND16_S) { sp[-1].i32 = (int32_t)(int16_t)sp[-1].i32; WAH_NEXT(); }
WAH_RUN(I64_EXTEND8_S)  { sp[-1].i64 = (int64_t) (int8_t)sp[-1].i64; WAH_NEXT(); }
WAH_RUN(I64_EXTEND16_S) { sp[-1].i64 = (int64_t)(int16_t)sp[-1].i64; WAH_NEXT(); }
WAH_RUN(I64_EXTEND32_S) { sp[-1].i64 = (int64_t)(int32_t)sp[-1].i64; WAH_NEXT(); }

WAH_RUN(I32_TRUNC_SAT_F32_S) { sp[-1].i32 =          wah_trunc_sat_f32_to_i32(sp[-1].f32); WAH_NEXT(); }
WAH_RUN(I32_TRUNC_SAT_F32_U) { sp[-1].i32 = (int32_t)wah_trunc_sat_f32_to_u32(sp[-1].f32); WAH_NEXT(); }
WAH_RUN(I32_TRUNC_SAT_F64_S) { sp[-1].i32 =          wah_trunc_sat_f64_to_i32(sp[-1].f64); WAH_NEXT(); }
WAH_RUN(I32_TRUNC_SAT_F64_U) { sp[-1].i32 = (int32_t)wah_trunc_sat_f64_to_u32(sp[-1].f64); WAH_NEXT(); }
WAH_RUN(I64_TRUNC_SAT_F32_S) { sp[-1].i64 =          wah_trunc_sat_f32_to_i64(sp[-1].f32); WAH_NEXT(); }
WAH_RUN(I64_TRUNC_SAT_F32_U) { sp[-1].i64 = (int64_t)wah_trunc_sat_f32_to_u64(sp[-1].f32); WAH_NEXT(); }
WAH_RUN(I64_TRUNC_SAT_F64_S) { sp[-1].i64 =          wah_trunc_sat_f64_to_i64(sp[-1].f64); WAH_NEXT(); }
WAH_RUN(I64_TRUNC_SAT_F64_U) { sp[-1].i64 = (int64_t)wah_trunc_sat_f64_to_u64(sp[-1].f64); WAH_NEXT(); }

#undef BINOP_I
#undef CMP_I_S
#undef CMP_I_U
#undef BINOP_F
#undef CMP_F
#undef UNOP_I_FN
#undef BINOP_I_FN
#undef UNOP_F_FN
#undef BINOP_F_FN
#undef NUM_OPS
#undef LOAD_OP
#undef STORE_OP
#undef WAH_SP_ADDR_I32
#undef WAH_SP_ADDR_I64
#undef CONVERT
#undef CONVERT_CHECK
#undef REINTERPRET

#define WAH_MEMORY_SIZE_IMPL(N) { \
    uint32_t mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    WAH_ASSERT(mem_idx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); \
    (*sp++).i##N = (int##N##_t)(fctx->memories[mem_idx].size / WAH_WASM_PAGE_SIZE); \
    WAH_NEXT(); \
}

#define WAH_MEMORY_GROW_IMPL(N) { \
    uint32_t mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    WAH_ASSERT(mem_idx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); \
    \
    int##N##_t pages_to_grow = (*--sp).i##N; \
    if (pages_to_grow < 0) { \
        (*sp++).i##N = -1; /* Cannot grow by negative pages */ \
        WAH_NEXT(); \
    } \
    \
    uint64_t old_pages; \
    if (!wah_memory_grow_internal(ctx, fctx, mem_idx, (uint64_t)pages_to_grow, &old_pages)) { \
        (*sp++).i##N = -1; \
        WAH_NEXT(); \
    } \
    (*sp++).i##N = (int##N##_t)old_pages; \
    WAH_NEXT(); \
}

#define WAH_MEMORY_FILL_IMPL(N) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    WAH_ASSERT(mem_idx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); \
    \
    uint64_t size = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    uint8_t val = (uint8_t)(*--sp).i32; \
    uint64_t dst = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(dst, size, fctx->memories[mem_idx].size), WAH_ERROR_MEMORY_OUT_OF_BOUNDS, cleanup); \
    uint64_t done = wah_memory_fill_internal(ctx, fctx->memories[mem_idx].data, dst, val, size); \
    if (done < size) { \
        (*sp++).i##N = (int##N##_t)(dst + done); \
        (*sp++).i32 = (int32_t)val; \
        (*sp++).i##N = (int##N##_t)(size - done); \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_MEMORY_INIT_IMPL(N) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t data_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint32_t mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    \
    WAH_ASSERT(mem_idx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); \
    WAH_ASSERT(data_idx < fctx->module->data_segment_count && "validation didn't catch out-of-bound data segment index"); \
    \
    uint32_t size = (uint32_t)(*--sp).i32; \
    uint32_t src_offset = (uint32_t)(*--sp).i32; \
    uint64_t dest_offset = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    \
    const wah_data_segment_t *segment = &fctx->module->data_segments[data_idx]; \
    uint32_t _seg_len = wah_data_seg_data_len(fctx, data_idx); \
    \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(dest_offset, size, fctx->memories[mem_idx].size), WAH_ERROR_MEMORY_OUT_OF_BOUNDS, cleanup); \
    WAH_ENSURE_GOTO((uint64_t)src_offset + size <= _seg_len, WAH_ERROR_TRAP, cleanup); \
    WAH_ENSURE_GOTO(size <= _seg_len, WAH_ERROR_TRAP, cleanup); \
    \
    uint32_t done = (uint32_t)wah_memory_init_internal(ctx, fctx->memories[mem_idx].data, dest_offset, \
                                                       segment->data, src_offset, size); \
    if (done < size) { \
        (*sp++).i##N = (int##N##_t)(dest_offset + done); \
        (*sp++).i32 = (int32_t)(src_offset + done); \
        (*sp++).i32 = (int32_t)(size - done); \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define WAH_MEMORY_COPY_IMPL(N,M,Z) { \
    const uint8_t *instr_start = bytecode_ip - sizeof(uint16_t); \
    uint32_t dest_mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    uint32_t src_mem_idx = wah_read_u32_le(bytecode_ip); \
    bytecode_ip += sizeof(uint32_t); \
    \
    WAH_ASSERT(dest_mem_idx < fctx->memory_count); \
    WAH_ASSERT(src_mem_idx < fctx->memory_count); \
    \
    uint64_t size = (uint64_t)(uint##Z##_t)(*--sp).i##Z; \
    uint64_t src = (uint64_t)(uint##N##_t)(*--sp).i##N; \
    uint64_t dest = (uint64_t)(uint##M##_t)(*--sp).i##M; \
    \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(dest, size, fctx->memories[dest_mem_idx].size), WAH_ERROR_MEMORY_OUT_OF_BOUNDS, cleanup); \
    WAH_ENSURE_GOTO(wah_u64_range_in_bounds(src, size, fctx->memories[src_mem_idx].size), WAH_ERROR_MEMORY_OUT_OF_BOUNDS, cleanup); \
    \
    bool backward = (dest_mem_idx == src_mem_idx && dest > src && dest < src + size); \
    uint64_t done = wah_memory_copy_internal(ctx, fctx->memories[dest_mem_idx].data, dest, \
                                             fctx->memories[src_mem_idx].data, src, size, backward); \
    if (done < size) { \
        if (backward) { \
            (*sp++).i##M = (int##M##_t)dest; \
            (*sp++).i##N = (int##N##_t)src; \
        } else { \
            (*sp++).i##M = (int##M##_t)(dest + done); \
            (*sp++).i##N = (int##N##_t)(src + done); \
        } \
        (*sp++).i##Z = (int##Z##_t)(size - done); \
        WAH_BULK_YIELD(instr_start); \
    } \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_RUN(MEMORY_SIZE) WAH_MEMORY_SIZE_IMPL(32)
WAH_RUN(MEMORY_GROW) WAH_MEMORY_GROW_IMPL(32)
WAH_RUN(MEMORY_FILL) WAH_MEMORY_FILL_IMPL(32)
WAH_RUN(MEMORY_INIT) WAH_MEMORY_INIT_IMPL(32)
WAH_RUN(MEMORY_COPY) WAH_MEMORY_COPY_IMPL(32,32,32)

WAH_RUN(DATA_DROP) {
    uint32_t data_idx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    WAH_ASSERT(data_idx < fctx->module->data_segment_count && "validation didn't catch out-of-bound data segment index");

    wah_data_seg_mark_dropped(fctx, data_idx);
    WAH_NEXT();
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64)
WAH_RUN(MEMORY_SIZE_i64) WAH_MEMORY_SIZE_IMPL(64)
WAH_RUN(MEMORY_GROW_i64) WAH_MEMORY_GROW_IMPL(64)
WAH_RUN(MEMORY_FILL_i64) WAH_MEMORY_FILL_IMPL(64)
WAH_RUN(MEMORY_INIT_i64) WAH_MEMORY_INIT_IMPL(64)
WAH_RUN(MEMORY_COPY_i32_to_i64) WAH_MEMORY_COPY_IMPL(32,64,32)
WAH_RUN(MEMORY_COPY_i64_to_i32) WAH_MEMORY_COPY_IMPL(64,32,32)
WAH_RUN(MEMORY_COPY_i64_to_i64) WAH_MEMORY_COPY_IMPL(64,64,64)
#endif

WAH_RUN(DROP) { sp--; WAH_NEXT(); }

WAH_RUN(SELECT) {
    wah_value_t c = *--sp;
    wah_value_t b = *--sp;
    wah_value_t a = *--sp;
    *sp++ = c.i32 ? a : b;
    WAH_NEXT();
}

WAH_RUN(UNREACHABLE) {
    (void)bytecode_base;
    (void)fctx;
    err = WAH_ERROR_TRAP;
    WAH_CLEANUP();
}

// --- Vector instructions ---
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)

#define M128I_UNARY_OP(fn, ...) WAH_IF_X86_64({ sp[-1]._m128i = fn(sp[-1]._m128i); WAH_NEXT(); }, __VA_ARGS__)
#define M128I_UNARY_I32_OP(fn, ...) WAH_IF_X86_64({ sp[-1].i32 = fn(sp[-1]._m128i); WAH_NEXT(); }, __VA_ARGS__)
#define M128I_BINARY_OP(fn, ...) WAH_IF_X86_64({ sp[-2]._m128i = fn(sp[-2]._m128i, sp[-1]._m128i); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128I_NOT_BINARY_OP(fn, ...) \
    WAH_IF_X86_64({ sp[-2]._m128i = _mm_xor_si128(fn(sp[-2]._m128i, sp[-1]._m128i), _mm_set1_epi8(-1)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128I_SHIFT_OP(fn, mask, ...) \
    WAH_IF_X86_64({ int32_t count = sp[-1].i32; sp[-2]._m128i = fn(sp[-2]._m128i, count & mask); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128I_TERNARY_OP(expr, ...) \
    WAH_IF_X86_64({ __m128i c = sp[-1]._m128i; __m128i b = sp[-2]._m128i; __m128i a = sp[-3]._m128i; \
                    sp[-3]._m128i = (expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define M128_UNARY_OP(fn, ...) WAH_IF_X86_64({ sp[-1]._m128 = fn(sp[-1]._m128); WAH_NEXT(); }, __VA_ARGS__)
#define M128_BINARY_OP(fn, ...) WAH_IF_X86_64({ sp[-2]._m128 = fn(sp[-2]._m128, sp[-1]._m128); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128_NOT_BINARY_OP(fn, ...) \
    WAH_IF_X86_64({ sp[-2]._m128 = _mm_xor_ps(fn(sp[-2]._m128, sp[-1]._m128), _mm_castsi128_ps(_mm_set1_epi8(-1))); \
                    sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128_TERNARY_OP(expr, ...) \
    WAH_IF_X86_64({ __m128 c = sp[-1]._m128; __m128 b = sp[-2]._m128; __m128 a = sp[-3]._m128; \
                    sp[-3]._m128 = (expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define M128_FP_UNARY_OP(fn, ...) WAH_IF_X86_64({ sp[-1]._m128 = wah_canonicalize_f32x4_sse2(fn(sp[-1]._m128)); WAH_NEXT(); }, __VA_ARGS__)
#define M128_FP_BINARY_OP(fn, ...) WAH_IF_X86_64({ sp[-2]._m128 = wah_canonicalize_f32x4_sse2(fn(sp[-2]._m128, sp[-1]._m128)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128_FP_TERNARY_OP(expr, ...) \
    WAH_IF_X86_64({ __m128 c = sp[-1]._m128; __m128 b = sp[-2]._m128; __m128 a = sp[-3]._m128; \
                    sp[-3]._m128 = wah_canonicalize_f32x4_sse2(expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define M128D_UNARY_OP(fn, ...) WAH_IF_X86_64({ sp[-1]._m128d = fn(sp[-1]._m128d); WAH_NEXT(); }, __VA_ARGS__)
#define M128D_BINARY_OP(fn, ...) WAH_IF_X86_64({ sp[-2]._m128d = fn(sp[-2]._m128d, sp[-1]._m128d); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128D_NOT_BINARY_OP(fn, ...) \
    WAH_IF_X86_64({ sp[-2]._m128d = _mm_xor_pd(fn(sp[-2]._m128d, sp[-1]._m128d), _mm_castsi128_pd(_mm_set1_epi8(-1))); \
                    sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128D_TERNARY_OP(expr, ...) \
    WAH_IF_X86_64({ __m128d c = sp[-1]._m128d; __m128d b = sp[-2]._m128d; __m128d a = sp[-3]._m128d; \
                    sp[-3]._m128d = (expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define M128D_FP_UNARY_OP(fn, ...) WAH_IF_X86_64({ sp[-1]._m128d = wah_canonicalize_f64x2_sse2(fn(sp[-1]._m128d)); WAH_NEXT(); }, __VA_ARGS__)
#define M128D_FP_BINARY_OP(fn, ...) WAH_IF_X86_64({ sp[-2]._m128d = wah_canonicalize_f64x2_sse2(fn(sp[-2]._m128d, sp[-1]._m128d)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define M128D_FP_TERNARY_OP(expr, ...) \
    WAH_IF_X86_64({ __m128d c = sp[-1]._m128d; __m128d b = sp[-2]._m128d; __m128d a = sp[-3]._m128d; \
                    sp[-3]._m128d = wah_canonicalize_f64x2_sse2(expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define N128_UNARY_OP(fn, ...) WAH_IF_AARCH64({ sp[-1]._u8x16 = fn(sp[-1]._u8x16); WAH_NEXT(); }, __VA_ARGS__)
#define N128_UNARY_I32_OP(fn, ...) WAH_IF_AARCH64({ sp[-1].i32 = fn(sp[-1]._u8x16); WAH_NEXT(); }, __VA_ARGS__)
#define N128_BINARY_OP(fn, ...) WAH_IF_AARCH64({ sp[-2]._u8x16 = fn(sp[-2]._u8x16, sp[-1]._u8x16); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define N128_NOT_BINARY_OP(fn, ...) \
    WAH_IF_AARCH64({ sp[-2]._u8x16 = veorq_u8(fn(sp[-2]._u8x16, sp[-1]._u8x16), vdupq_n_u8(0xFF)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define N128_SHIFT_OP(fn, ...) \
    WAH_IF_AARCH64({ int32_t count = sp[-1].i32; sp[-2]._u8x16 = fn(sp[-2]._u8x16, count); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define N128_TERNARY_OP(expr, ...) \
    WAH_IF_AARCH64({ uint8x16_t c = sp[-1]._u8x16; uint8x16_t b = sp[-2]._u8x16; uint8x16_t a = sp[-3]._u8x16; \
                     sp[-3]._u8x16 = (expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

#define N128_FP_UNARY_OP_F32(fn, ...) WAH_IF_AARCH64({ sp[-1]._u8x16 = wah_canonicalize_f32x4_neon(fn(sp[-1]._u8x16)); WAH_NEXT(); }, __VA_ARGS__)
#define N128_FP_UNARY_OP_F64(fn, ...) WAH_IF_AARCH64({ sp[-1]._u8x16 = wah_canonicalize_f64x2_neon(fn(sp[-1]._u8x16)); WAH_NEXT(); }, __VA_ARGS__)
#define N128_FP_BINARY_OP_F32(fn, ...) WAH_IF_AARCH64({ sp[-2]._u8x16 = wah_canonicalize_f32x4_neon(fn(sp[-2]._u8x16, sp[-1]._u8x16)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define N128_FP_BINARY_OP_F64(fn, ...) WAH_IF_AARCH64({ sp[-2]._u8x16 = wah_canonicalize_f64x2_neon(fn(sp[-2]._u8x16, sp[-1]._u8x16)); sp--; WAH_NEXT(); }, __VA_ARGS__)
#define N128_FP_TERNARY_OP_F32(expr, ...) \
    WAH_IF_AARCH64({ uint8x16_t c = sp[-1]._u8x16; uint8x16_t b = sp[-2]._u8x16; uint8x16_t a = sp[-3]._u8x16; \
                     sp[-3]._u8x16 = wah_canonicalize_f32x4_neon(expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)
#define N128_FP_TERNARY_OP_F64(expr, ...) \
    WAH_IF_AARCH64({ uint8x16_t c = sp[-1]._u8x16; uint8x16_t b = sp[-2]._u8x16; uint8x16_t a = sp[-3]._u8x16; \
                     sp[-3]._u8x16 = wah_canonicalize_f64x2_neon(expr); sp -= 2; WAH_NEXT(); }, __VA_ARGS__)

// addr_expr pops the address from the stack with appropriate type:
//   i32 addressing: WAH_SP_ADDR_I32  (zero-extends via uint32_t)
//   i64 addressing: WAH_SP_ADDR_I64
#define WAH_SP_ADDR_I32 ((uint64_t)(uint32_t)(*--sp).i32)
#define WAH_SP_ADDR_I64 ((uint64_t)(*--sp).i64)

#define V128_LOAD_COMMON(mem0, read_size, addr_expr) \
    uint32_t memidx = 0; \
    if (!(mem0)) { memidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); } \
    uint64_t offset = wah_read_u64_le(bytecode_ip); \
    bytecode_ip += sizeof(uint64_t); \
    uint64_t addr = (addr_expr); \
    uint64_t effective_addr; \
    if (!(mem0)) { WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); } \
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, (read_size), \
        (mem0) ? fctx->memory_size : fctx->memories[memidx].size, &effective_addr), cleanup); \
    uint8_t *_mem_base = (mem0) ? fctx->memory_base : fctx->memories[memidx].data

#define V128_LOAD_HALF_OP(mem0, addr_expr, N, elem_ty, cast) { \
    V128_LOAD_COMMON(mem0, 8, addr_expr); \
    wah_v128_t *v = &(*sp++).v128; \
    for (int i = 0; i < 64/N; ++i) \
        v->elem_ty[i] = cast(wah_read_u##N##_le(_mem_base + effective_addr + i * (N/8))); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define V128_LOAD_SPLAT_OP(mem0, addr_expr, N) { \
    V128_LOAD_COMMON(mem0, N/8, addr_expr); \
    wah_v128_t *v = &(*sp++).v128; \
    uint##N##_t val = wah_read_u##N##_le(_mem_base + effective_addr); \
    for (int i = 0; i < 128/N; ++i) v->u##N[i] = val; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

#define V128_LOAD_LANE_OP(mem0, addr_expr, N) { \
    uint32_t memidx = 0; \
    if (!(mem0)) { memidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); } \
    uint64_t offset = wah_read_u64_le(bytecode_ip); \
    bytecode_ip += sizeof(uint64_t); \
    uint32_t lane_idx = *bytecode_ip++; \
    wah_v128_t val = (*--sp).v128; \
    uint64_t addr = (addr_expr); \
    uint64_t effective_addr; \
    if (!(mem0)) { WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index"); } \
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, N/8, \
        (mem0) ? fctx->memory_size : fctx->memories[memidx].size, &effective_addr), cleanup); \
    WAH_ASSERT(lane_idx < 128/N && "validation didn't catch out-of-bound lane index"); \
    val.u##N[lane_idx] = wah_read_u##N##_le( \
        ((mem0) ? fctx->memory_base : fctx->memories[memidx].data) + effective_addr); \
    (*sp++).v128 = val; \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_RUN(V128_LOAD) {
    V128_LOAD_COMMON(0, sizeof(wah_v128_t), WAH_SP_ADDR_I32);
    memcpy(&(*sp++).v128, _mem_base + effective_addr, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD_i32_mem0) {
    V128_LOAD_COMMON(1, sizeof(wah_v128_t), WAH_SP_ADDR_I32);
    memcpy(&(*sp++).v128, _mem_base + effective_addr, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD8X8_S) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 8, i16, (int16_t)(int8_t))
WAH_RUN(V128_LOAD8X8_U) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 8, u16, (uint16_t))
WAH_RUN(V128_LOAD16X4_S) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 16, i32, (int32_t)(int16_t))
WAH_RUN(V128_LOAD16X4_U) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 16, u32, (uint32_t))
WAH_RUN(V128_LOAD32X2_S) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 32, i64, (int64_t)(int32_t))
WAH_RUN(V128_LOAD32X2_U) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I32, 32, u64, (uint64_t))

WAH_RUN(V128_LOAD8X8_S_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 8, i16, (int16_t)(int8_t))
WAH_RUN(V128_LOAD8X8_U_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 8, u16, (uint16_t))
WAH_RUN(V128_LOAD16X4_S_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 16, i32, (int32_t)(int16_t))
WAH_RUN(V128_LOAD16X4_U_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 16, u32, (uint32_t))
WAH_RUN(V128_LOAD32X2_S_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 32, i64, (int64_t)(int32_t))
WAH_RUN(V128_LOAD32X2_U_i32_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I32, 32, u64, (uint64_t))

WAH_RUN(V128_LOAD8_SPLAT) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_LOAD16_SPLAT) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_LOAD32_SPLAT) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_LOAD64_SPLAT) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I32, 64)

WAH_RUN(V128_LOAD8_SPLAT_i32_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_LOAD16_SPLAT_i32_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_LOAD32_SPLAT_i32_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_LOAD64_SPLAT_i32_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I32, 64)

WAH_RUN(V128_LOAD32_ZERO) {
    V128_LOAD_COMMON(0, 4, WAH_SP_ADDR_I32);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u32_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}
WAH_RUN(V128_LOAD64_ZERO) {
    V128_LOAD_COMMON(0, 8, WAH_SP_ADDR_I32);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u64_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD32_ZERO_i32_mem0) {
    V128_LOAD_COMMON(1, 4, WAH_SP_ADDR_I32);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u32_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}
WAH_RUN(V128_LOAD64_ZERO_i32_mem0) {
    V128_LOAD_COMMON(1, 8, WAH_SP_ADDR_I32);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u64_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD8_LANE) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_LOAD16_LANE) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_LOAD32_LANE) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_LOAD64_LANE) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I32, 64)

WAH_RUN(V128_LOAD8_LANE_i32_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_LOAD16_LANE_i32_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_LOAD32_LANE_i32_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_LOAD64_LANE_i32_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I32, 64)

#define V128_STORE_LANE_OP(mem0, addr_expr, N) { \
    uint32_t memidx = 0; \
    if (!(mem0)) { memidx = wah_read_u32_le(bytecode_ip); bytecode_ip += sizeof(uint32_t); } \
    uint64_t offset = wah_read_u64_le(bytecode_ip); \
    bytecode_ip += sizeof(uint64_t); \
    uint32_t lane_idx = *bytecode_ip++; \
    wah_v128_t val = (*--sp).v128; \
    uint64_t addr = (addr_expr); \
    uint64_t effective_addr; \
    if (!(mem0)) { WAH_ASSERT(memidx < fctx->memory_count); } \
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, N/8, \
        (mem0) ? fctx->memory_size : fctx->memories[memidx].size, &effective_addr), cleanup); \
    WAH_ASSERT(lane_idx < 128/N); \
    wah_write_u##N##_le( \
        ((mem0) ? fctx->memory_base : fctx->memories[memidx].data) + effective_addr, val.u##N[lane_idx]); \
    WAH_NEXT(); \
    WAH_CLEANUP(); \
}

WAH_RUN(V128_STORE8_LANE) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_STORE16_LANE) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_STORE32_LANE) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_STORE64_LANE) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I32, 64)

WAH_RUN(V128_STORE8_LANE_i32_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I32, 8)
WAH_RUN(V128_STORE16_LANE_i32_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I32, 16)
WAH_RUN(V128_STORE32_LANE_i32_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I32, 32)
WAH_RUN(V128_STORE64_LANE_i32_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I32, 64)

WAH_RUN(V128_STORE) {
    uint32_t memidx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint64_t offset = wah_read_u64_le(bytecode_ip);
    bytecode_ip += sizeof(uint64_t);
    wah_v128_t val = (*--sp).v128;
    uint32_t addr = (uint32_t)(*--sp).i32;
    uint64_t effective_addr;
    WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index");
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, sizeof(wah_v128_t), fctx->memories[memidx].size, &effective_addr), cleanup);
    memcpy(fctx->memories[memidx].data + effective_addr, &val, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_STORE_i32_mem0) {
    uint64_t offset = wah_read_u64_le(bytecode_ip);
    bytecode_ip += sizeof(uint64_t);
    wah_v128_t val = (*--sp).v128;
    uint32_t addr = (uint32_t)(*--sp).i32;
    uint64_t effective_addr;
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, sizeof(wah_v128_t), fctx->memory_size, &effective_addr), cleanup);
    memcpy(fctx->memory_base + effective_addr, &val, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

// --- i64-addressed V128 memory operations ---
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_MEMORY64) && ((WAH_COMPILED_FEATURES) & WAH_FEATURE_SIMD)

WAH_RUN(V128_LOAD_i64) {
    V128_LOAD_COMMON(0, sizeof(wah_v128_t), WAH_SP_ADDR_I64);
    memcpy(&(*sp++).v128, _mem_base + effective_addr, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD_i64_mem0) {
    V128_LOAD_COMMON(1, sizeof(wah_v128_t), WAH_SP_ADDR_I64);
    memcpy(&(*sp++).v128, _mem_base + effective_addr, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD8X8_S_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 8, i16, (int16_t)(int8_t))
WAH_RUN(V128_LOAD8X8_U_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 8, u16, (uint16_t))
WAH_RUN(V128_LOAD16X4_S_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 16, i32, (int32_t)(int16_t))
WAH_RUN(V128_LOAD16X4_U_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 16, u32, (uint32_t))
WAH_RUN(V128_LOAD32X2_S_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 32, i64, (int64_t)(int32_t))
WAH_RUN(V128_LOAD32X2_U_i64) V128_LOAD_HALF_OP(0, WAH_SP_ADDR_I64, 32, u64, (uint64_t))

WAH_RUN(V128_LOAD8X8_S_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 8, i16, (int16_t)(int8_t))
WAH_RUN(V128_LOAD8X8_U_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 8, u16, (uint16_t))
WAH_RUN(V128_LOAD16X4_S_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 16, i32, (int32_t)(int16_t))
WAH_RUN(V128_LOAD16X4_U_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 16, u32, (uint32_t))
WAH_RUN(V128_LOAD32X2_S_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 32, i64, (int64_t)(int32_t))
WAH_RUN(V128_LOAD32X2_U_i64_mem0) V128_LOAD_HALF_OP(1, WAH_SP_ADDR_I64, 32, u64, (uint64_t))

WAH_RUN(V128_LOAD8_SPLAT_i64) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_LOAD16_SPLAT_i64) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_LOAD32_SPLAT_i64) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_LOAD64_SPLAT_i64) V128_LOAD_SPLAT_OP(0, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_LOAD8_SPLAT_i64_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_LOAD16_SPLAT_i64_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_LOAD32_SPLAT_i64_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_LOAD64_SPLAT_i64_mem0) V128_LOAD_SPLAT_OP(1, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_LOAD32_ZERO_i64) {
    V128_LOAD_COMMON(0, 4, WAH_SP_ADDR_I64);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u32_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}
WAH_RUN(V128_LOAD64_ZERO_i64) {
    V128_LOAD_COMMON(0, 8, WAH_SP_ADDR_I64);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u64_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD32_ZERO_i64_mem0) {
    V128_LOAD_COMMON(1, 4, WAH_SP_ADDR_I64);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u32_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}
WAH_RUN(V128_LOAD64_ZERO_i64_mem0) {
    V128_LOAD_COMMON(1, 8, WAH_SP_ADDR_I64);
    wah_v128_t *v = &(*sp++).v128;
    *v = (wah_v128_t){ .u64 = {wah_read_u64_le(_mem_base + effective_addr), 0} };
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_LOAD8_LANE_i64) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_LOAD16_LANE_i64) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_LOAD32_LANE_i64) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_LOAD64_LANE_i64) V128_LOAD_LANE_OP(0, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_LOAD8_LANE_i64_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_LOAD16_LANE_i64_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_LOAD32_LANE_i64_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_LOAD64_LANE_i64_mem0) V128_LOAD_LANE_OP(1, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_STORE8_LANE_i64) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_STORE16_LANE_i64) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_STORE32_LANE_i64) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_STORE64_LANE_i64) V128_STORE_LANE_OP(0, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_STORE8_LANE_i64_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I64, 8)
WAH_RUN(V128_STORE16_LANE_i64_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I64, 16)
WAH_RUN(V128_STORE32_LANE_i64_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I64, 32)
WAH_RUN(V128_STORE64_LANE_i64_mem0) V128_STORE_LANE_OP(1, WAH_SP_ADDR_I64, 64)

WAH_RUN(V128_STORE_i64) {
    uint32_t memidx = wah_read_u32_le(bytecode_ip);
    bytecode_ip += sizeof(uint32_t);
    uint64_t offset = wah_read_u64_le(bytecode_ip);
    bytecode_ip += sizeof(uint64_t);
    wah_v128_t val = (*--sp).v128;
    uint64_t addr = (uint64_t)(*--sp).i64;
    uint64_t effective_addr;
    WAH_ASSERT(memidx < fctx->memory_count && "validation didn't catch out-of-bound memory index");
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, sizeof(wah_v128_t), fctx->memories[memidx].size, &effective_addr), cleanup);
    memcpy(fctx->memories[memidx].data + effective_addr, &val, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

WAH_RUN(V128_STORE_i64_mem0) {
    uint64_t offset = wah_read_u64_le(bytecode_ip);
    bytecode_ip += sizeof(uint64_t);
    wah_v128_t val = (*--sp).v128;
    uint64_t addr = (uint64_t)(*--sp).i64;
    uint64_t effective_addr;
    WAH_CHECK_GOTO(wah_check_effective_addr(addr, offset, sizeof(wah_v128_t), fctx->memory_size, &effective_addr), cleanup);
    memcpy(fctx->memory_base + effective_addr, &val, sizeof(wah_v128_t));
    WAH_NEXT();
    WAH_CLEANUP();
}

#endif // WAH_FEATURE_MEMORY64 && WAH_FEATURE_SIMD (i64 V128 memory)

#undef WAH_SP_ADDR_I32
#undef WAH_SP_ADDR_I64
#undef V128_LOAD_COMMON
#undef V128_LOAD_HALF_OP
#undef V128_LOAD_LANE_OP
#undef V128_LOAD_SPLAT_OP
#undef V128_STORE_LANE_OP

#define EXTRACT_LANE_OP(VEC_TYPE, SCALAR_TYPE, LANE_COUNT) { \
    wah_v128_t vec = (*--sp).v128; \
    uint8_t laneidx = *bytecode_ip++; \
    WAH_ASSERT(laneidx < LANE_COUNT && "validation didn't catch out-of-bound lane index"); \
    wah_value_t result; \
    result.SCALAR_TYPE = vec.VEC_TYPE[laneidx]; \
    *sp++ = result; \
    WAH_NEXT(); \
}

#define REPLACE_LANE_OP(VEC_TYPE, C_VEC_TYPE, SCALAR_TYPE, LANE_COUNT) { \
    wah_value_t scalar_val = *--sp; \
    wah_v128_t vec = (*--sp).v128; \
    uint8_t laneidx = *bytecode_ip++; \
    WAH_ASSERT(laneidx < LANE_COUNT && "validation didn't catch out-of-bound lane index"); \
    vec.VEC_TYPE[laneidx] = (C_VEC_TYPE)scalar_val.SCALAR_TYPE; \
    (*sp++).v128 = vec; \
    WAH_NEXT(); \
}

#define SPLAT_OP(VEC_TYPE, C_VEC_TYPE, SCALAR_TYPE) { \
    wah_value_t scalar_val = *--sp; \
    wah_v128_t result = {0}; \
    for (uint32_t i = 0; i < sizeof(wah_v128_t) / sizeof(result.VEC_TYPE[0]); ++i) { \
        result.VEC_TYPE[i] = (C_VEC_TYPE)scalar_val.SCALAR_TYPE; \
    } \
    (*sp++).v128 = result; \
    WAH_NEXT(); \
}

WAH_RUN(I8X16_SHUFFLE) {
    wah_v128_t vec2 = (*--sp).v128;
    wah_v128_t vec1 = (*--sp).v128;
    wah_v128_t result = {0};
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t lane_idx = bytecode_ip[i];
        result.u8[i] = lane_idx < 16 ? vec1.u8[lane_idx] : vec2.u8[lane_idx - 16];
    }
    bytecode_ip += 16; // Advance past the shuffle mask
    (*sp++).v128 = result;
    WAH_NEXT();
}

WAH_RUN(I8X16_SWIZZLE) {
    wah_v128_t mask = (*--sp).v128;
    wah_v128_t data = (*--sp).v128;
    wah_v128_t result;
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t lane_idx = mask.u8[i];
        result.u8[i] = lane_idx < 16 ? data.u8[lane_idx] : 0;
    }
    (*sp++).v128 = result;
    WAH_NEXT();
}

WAH_RUN(I8X16_EXTRACT_LANE_S) EXTRACT_LANE_OP(i8, i32, 16)
WAH_RUN(I8X16_EXTRACT_LANE_U) EXTRACT_LANE_OP(u8, i32, 16)
WAH_RUN(I8X16_REPLACE_LANE) REPLACE_LANE_OP(i8, int8_t, i32, 16)
WAH_RUN(I16X8_EXTRACT_LANE_S) EXTRACT_LANE_OP(i16, i32, 8)
WAH_RUN(I16X8_EXTRACT_LANE_U) EXTRACT_LANE_OP(u16, i32, 8)
WAH_RUN(I16X8_REPLACE_LANE) REPLACE_LANE_OP(i16, int16_t, i32, 8)
WAH_RUN(I32X4_EXTRACT_LANE) EXTRACT_LANE_OP(i32, i32, 4)
WAH_RUN(I32X4_REPLACE_LANE) REPLACE_LANE_OP(i32, int32_t, i32, 4)
WAH_RUN(I64X2_EXTRACT_LANE) EXTRACT_LANE_OP(i64, i64, 2)
WAH_RUN(I64X2_REPLACE_LANE) REPLACE_LANE_OP(i64, int64_t, i64, 2)
WAH_RUN(F32X4_EXTRACT_LANE) EXTRACT_LANE_OP(f32, f32, 4)
WAH_RUN(F32X4_REPLACE_LANE) REPLACE_LANE_OP(f32, float, f32, 4)
WAH_RUN(F64X2_EXTRACT_LANE) EXTRACT_LANE_OP(f64, f64, 2)
WAH_RUN(F64X2_REPLACE_LANE) REPLACE_LANE_OP(f64, double, f64, 2)

WAH_RUN(I8X16_SPLAT) SPLAT_OP(i8, int8_t, i32)
WAH_RUN(I16X8_SPLAT) SPLAT_OP(i16, int16_t, i32)
WAH_RUN(I32X4_SPLAT) SPLAT_OP(i32, int32_t, i32)
WAH_RUN(I64X2_SPLAT) SPLAT_OP(i64, int64_t, i64)
WAH_RUN(F32X4_SPLAT) SPLAT_OP(f32, float, f32)
WAH_RUN(F64X2_SPLAT) SPLAT_OP(f64, double, f64)

#undef EXTRACT_LANE_OP
#undef REPLACE_LANE_OP
#undef SPLAT_OP

#define V128_UNARY_OP(op) { sp[-1].v128 = op(sp[-1].v128); WAH_NEXT(); }
#define V128_BINARY_OP(op) { sp[-2].v128 = op(sp[-2].v128, sp[-1].v128); sp--; WAH_NEXT(); }
#define V128_BINARY_OP_LANE(N, op, field) { \
    for (int i = 0; i < 128/N; ++i) { \
        sp[-2].v128.field[i] = sp[-2].v128.field[i] op sp[-1].v128.field[i]; \
    } \
    sp--; \
    WAH_NEXT(); \
}
#define V128_BINARY_OP_LANE_WRAP(N, op, field) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (uint##N##_t)(a.field[i] op b.field[i]); \
    } \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}
#define V128_BINARY_OP_LANE_SAT_S(N, op, field, min_val, max_val) { \
    for (int i = 0; i < 128/N; ++i) { \
        int64_t res = (int64_t)sp[-2].v128.field[i] op (int64_t)sp[-1].v128.field[i]; \
        if (res < min_val) res = min_val; \
        if (res > max_val) res = max_val; \
        sp[-2].v128.field[i] = (int##N##_t)res; \
    } \
    sp--; \
    WAH_NEXT(); \
}
#define V128_BINARY_OP_LANE_SAT_U(N, op, field, max_val) { \
    for (int i = 0; i < 128/N; ++i) { \
        int64_t res = (uint64_t)sp[-2].v128.field[i] op (uint64_t)sp[-1].v128.field[i]; \
        if (res < 0) res = 0; \
        if (res > max_val) res = max_val; \
        sp[-2].v128.field[i] = (uint##N##_t)res; \
    } \
    sp--; \
    WAH_NEXT(); \
}
#define V128_BINARY_OP_LANE_F(N, op, field) { \
    for (int i = 0; i < 128/N; ++i) { \
        sp[-2].v128.field[i] = wah_canonicalize_##field(sp[-2].v128.field[i] op sp[-1].v128.field[i]); \
    } \
    sp--; \
    WAH_NEXT(); \
}

#define V128_CMP_I_LANE(N, op, field) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (a.field[i] op b.field[i]) ? (~0ULL >> (64 - N)) : 0; \
    } \
    sp[-2].v128 = a; sp--; WAH_NEXT(); \
}
#define V128_CMP_I_LANE_S(N, op, field) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (((int##N##_t)a.field[i]) op ((int##N##_t)b.field[i])) ? (~0ULL >> (64 - N)) : 0; \
    } \
    sp[-2].v128 = a; sp--; WAH_NEXT(); \
}
#define V128_CMP_I_LANE_U(N, op, field) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (((uint##N##_t)a.field[i]) op ((uint##N##_t)b.field[i])) ? (~0ULL >> (64 - N)) : 0; \
    } \
    sp[-2].v128 = a; sp--; WAH_NEXT(); \
}
#define V128_CMP_F_LANE(N, op, field) { \
    wah_v128_t *a_ptr = &sp[-2].v128; \
    wah_v128_t b = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) a_ptr->i##N[i] = (a_ptr->field[i] op b.field[i]) ? -1 : 0; \
    sp--; WAH_NEXT(); \
}

#define V128_TRUNC_SAT_PAIR_OP(fn, dest_field, src_field, K) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128, result; \
    for (int i = 0; i < K; ++i) { \
        result.dest_field[i] = fn(a.src_field[i]); \
        result.dest_field[i+K] = fn(b.src_field[i]); \
    } \
    sp[-2].v128 = result; sp--; WAH_NEXT(); \
}

#define V128_TRUNC_SAT_F2I_OP(N, F_FIELD, I_FIELD, TRUNC_FUNC) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.I_FIELD[i] = TRUNC_FUNC(val.F_FIELD[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_TRUNC_SAT_F2I_ZERO_OP(N, F_FIELD, I_FIELD, TRUNC_FUNC) { \
    wah_v128_t val = sp[-1].v128; \
    wah_v128_t result = { .I_FIELD = {TRUNC_FUNC(val.F_FIELD[0]), TRUNC_FUNC(val.F_FIELD[1])} }; \
    sp[-1].v128 = result; \
    WAH_NEXT(); \
}

#define V128_TERNARY_OP_F(N, op1, op2, field) { \
    WAH_PRAGMA_FP_CONTRACT_OFF() /* No FMA allowed here */ \
    wah_v128_t c = (*--sp).v128; \
    wah_v128_t b = (*--sp).v128; \
    wah_v128_t a = (*--sp).v128; \
    wah_v128_t result; \
    for (int i = 0; i < 128/N; ++i) { \
        result.field[i] = wah_canonicalize_##field(a.field[i] op1 b.field[i] op2 c.field[i]); \
    } \
    (*sp++).v128 = result; \
    WAH_NEXT(); \
}

#define V128_LANESELECT_OP(N, U_FIELD) { \
    wah_v128_t v3 = (*--sp).v128; \
    wah_v128_t v2 = (*--sp).v128; \
    wah_v128_t v1 = (*--sp).v128; \
    wah_v128_t result; \
    for (int i = 0; i < 128/N; ++i) { \
        result.U_FIELD[i] = (v3.U_FIELD[i] & v1.U_FIELD[i]) | (~v3.U_FIELD[i] & v2.U_FIELD[i]); \
    } \
    (*sp++).v128 = result; \
    WAH_NEXT(); \
}

#define V128_BITWISE_UNARY_FALLBACK(expr) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 16; ++i) val.u8[i] = (expr); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}
#define V128_BITWISE_BINARY_FALLBACK(expr) { \
    wah_v128_t b = sp[-1].v128; \
    wah_v128_t a = sp[-2].v128; \
    for (int i = 0; i < 16; ++i) a.u8[i] = (expr); \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}

WAH_RUN(V128_NOT) M128I_UNARY_OP(wah_v128_not_sse2, N128_UNARY_OP(vmvnq_u8,
    V128_BITWISE_UNARY_FALLBACK((uint8_t)~val.u8[i])))
WAH_RUN(V128_AND) M128I_BINARY_OP(_mm_and_si128, N128_BINARY_OP(vandq_u8,
    V128_BITWISE_BINARY_FALLBACK(a.u8[i] & b.u8[i])))
WAH_RUN(V128_ANDNOT) M128I_BINARY_OP(wah_v128_andnot_rev_sse2, N128_BINARY_OP(wah_v128_andnot_neon,
    V128_BITWISE_BINARY_FALLBACK(a.u8[i] & (uint8_t)~b.u8[i])))
WAH_RUN(V128_OR) M128I_BINARY_OP(_mm_or_si128, N128_BINARY_OP(vorrq_u8,
    V128_BITWISE_BINARY_FALLBACK(a.u8[i] | b.u8[i])))
WAH_RUN(V128_XOR) M128I_BINARY_OP(_mm_xor_si128, N128_BINARY_OP(veorq_u8,
    V128_BITWISE_BINARY_FALLBACK(a.u8[i] ^ b.u8[i])))

#undef V128_BITWISE_UNARY_FALLBACK
#undef V128_BITWISE_BINARY_FALLBACK

WAH_RUN(I8X16_ADD) M128I_BINARY_OP(_mm_add_epi8, N128_BINARY_OP(wah_vaddq_s8, V128_BINARY_OP_LANE_WRAP(8, +, u8)))
WAH_RUN(I8X16_ADD_SAT_S) M128I_BINARY_OP(_mm_adds_epi8, N128_BINARY_OP(wah_vqaddq_s8, V128_BINARY_OP_LANE_SAT_S(8, +, i8, -128, 127)))
WAH_RUN(I8X16_ADD_SAT_U) M128I_BINARY_OP(_mm_adds_epu8, N128_BINARY_OP(wah_vqaddq_u8, V128_BINARY_OP_LANE_SAT_U(8, +, u8, 255)))
WAH_RUN(I8X16_SUB) M128I_BINARY_OP(_mm_sub_epi8, N128_BINARY_OP(wah_vsubq_s8, V128_BINARY_OP_LANE_WRAP(8, -, u8)))
WAH_RUN(I8X16_SUB_SAT_S) M128I_BINARY_OP(_mm_subs_epi8, N128_BINARY_OP(wah_vqsubq_s8, V128_BINARY_OP_LANE_SAT_S(8, -, i8, -128, 127)))
WAH_RUN(I8X16_SUB_SAT_U) M128I_BINARY_OP(_mm_subs_epu8, N128_BINARY_OP(wah_vqsubq_u8, V128_BINARY_OP_LANE_SAT_U(8, -, u8, 255)))

WAH_RUN(I16X8_ADD) M128I_BINARY_OP(_mm_add_epi16, N128_BINARY_OP(wah_vaddq_s16, V128_BINARY_OP_LANE_WRAP(16, +, u16)))
WAH_RUN(I16X8_ADD_SAT_S) M128I_BINARY_OP(_mm_adds_epi16, N128_BINARY_OP(wah_vqaddq_s16, V128_BINARY_OP_LANE_SAT_S(16, +, i16, -32768, 32767)))
WAH_RUN(I16X8_ADD_SAT_U) M128I_BINARY_OP(_mm_adds_epu16, N128_BINARY_OP(wah_vqaddq_u16, V128_BINARY_OP_LANE_SAT_U(16, +, u16, 65535)))
WAH_RUN(I16X8_SUB) M128I_BINARY_OP(_mm_sub_epi16, N128_BINARY_OP(wah_vsubq_s16, V128_BINARY_OP_LANE_WRAP(16, -, u16)))
WAH_RUN(I16X8_SUB_SAT_S) M128I_BINARY_OP(_mm_subs_epi16, N128_BINARY_OP(wah_vqsubq_s16, V128_BINARY_OP_LANE_SAT_S(16, -, i16, -32768, 32767)))
WAH_RUN(I16X8_SUB_SAT_U) M128I_BINARY_OP(_mm_subs_epu16, N128_BINARY_OP(wah_vqsubq_u16, V128_BINARY_OP_LANE_SAT_U(16, -, u16, 65535)))
WAH_RUN(I16X8_MUL) M128I_BINARY_OP(_mm_mullo_epi16, N128_BINARY_OP(wah_vmulq_s16, V128_BINARY_OP_LANE_WRAP(16, *, u16)))

WAH_RUN(I32X4_ADD) M128I_BINARY_OP(_mm_add_epi32, N128_BINARY_OP(wah_vaddq_s32, V128_BINARY_OP_LANE_WRAP(32, +, u32)))
WAH_RUN(I32X4_SUB) M128I_BINARY_OP(_mm_sub_epi32, N128_BINARY_OP(wah_vsubq_s32, V128_BINARY_OP_LANE_WRAP(32, -, u32)))
WAH_RUN(I32X4_MUL) M128I_BINARY_OP(wah_i32x4_mul_sse2, N128_BINARY_OP(wah_vmulq_s32, V128_BINARY_OP_LANE_WRAP(32, *, u32)))

WAH_RUN(I64X2_ADD) M128I_BINARY_OP(_mm_add_epi64, N128_BINARY_OP(wah_vaddq_s64, V128_BINARY_OP_LANE_WRAP(64, +, u64)))
WAH_RUN(I64X2_SUB) M128I_BINARY_OP(_mm_sub_epi64, N128_BINARY_OP(wah_vsubq_s64, V128_BINARY_OP_LANE_WRAP(64, -, u64)))
WAH_RUN(I64X2_MUL) M128I_BINARY_OP(wah_i64x2_mul_sse2, V128_BINARY_OP_LANE_WRAP(64, *, u64))

WAH_RUN(F32X4_ADD) M128_FP_BINARY_OP(_mm_add_ps, N128_FP_BINARY_OP_F32(wah_vaddq_f32, V128_BINARY_OP_LANE_F(32, +, f32)))
WAH_RUN(F32X4_SUB) M128_FP_BINARY_OP(_mm_sub_ps, N128_FP_BINARY_OP_F32(wah_vsubq_f32, V128_BINARY_OP_LANE_F(32, -, f32)))
WAH_RUN(F32X4_MUL) M128_FP_BINARY_OP(_mm_mul_ps, N128_FP_BINARY_OP_F32(wah_vmulq_f32, V128_BINARY_OP_LANE_F(32, *, f32)))
WAH_RUN(F32X4_DIV) M128_FP_BINARY_OP(_mm_div_ps, N128_FP_BINARY_OP_F32(wah_vdivq_f32, V128_BINARY_OP_LANE_F(32, /, f32)))

WAH_RUN(F64X2_ADD) M128D_FP_BINARY_OP(_mm_add_pd, N128_FP_BINARY_OP_F64(wah_vaddq_f64, V128_BINARY_OP_LANE_F(64, +, f64)))
WAH_RUN(F64X2_SUB) M128D_FP_BINARY_OP(_mm_sub_pd, N128_FP_BINARY_OP_F64(wah_vsubq_f64, V128_BINARY_OP_LANE_F(64, -, f64)))
WAH_RUN(F64X2_MUL) M128D_FP_BINARY_OP(_mm_mul_pd, N128_FP_BINARY_OP_F64(wah_vmulq_f64, V128_BINARY_OP_LANE_F(64, *, f64)))
WAH_RUN(F64X2_DIV) M128D_FP_BINARY_OP(_mm_div_pd, N128_FP_BINARY_OP_F64(wah_vdivq_f64, V128_BINARY_OP_LANE_F(64, /, f64)))

#define V128_UNARY_OP_LANE(N, op, field) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.field[i] = op(val.field[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_NEG_OP_LANE_WRAP(N, field) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.field[i] = (uint##N##_t)(0 - val.field[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_UNARY_OP_LANE_FN(N, fn, field) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.field[i] = fn(val.field[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_UNARY_OP_LANE_CAST_FN(N, fn, field, cast_type) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.field[i] = (field)fn((cast_type)val.field[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_BINARY_OP_LANE_FN(N, fn, field) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) a.field[i] = fn(a.field[i], b.field[i]); \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}

#define V128_SHIFT_OP_LANE(N, op, field) { \
    int32_t b = sp[-1].i32; \
    wah_v128_t a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) a.field[i] = a.field[i] op (b & (N - 1)); \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}

#define V128_SHIFT_OP_LANE_U(N, op, field) { \
    int32_t b = sp[-1].i32; \
    wah_v128_t a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (uint##N##_t)((uint##N##_t)a.field[i] op (b & (N - 1))); \
    } \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}

#define V128_SHL_OP_LANE_WRAP(N, field) { \
    int32_t b = sp[-1].i32; \
    wah_v128_t a = sp[-2].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        a.field[i] = (uint##N##_t)(a.field[i] << (b & (N - 1))); \
    } \
    sp[-2].v128 = a; \
    sp--; \
    WAH_NEXT(); \
}

#define V128_ABS_OP(N, field, abs_func) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) val.field[i] = (int##N##_t)abs_func(val.field[i]); \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_ABS_OP_WRAP(N, field) { \
    wah_v128_t val = sp[-1].v128; \
    for (int i = 0; i < 128/N; ++i) { \
        uint##N##_t sign = val.field[i] >> (N - 1); \
        uint##N##_t mask = (uint##N##_t)(0 - sign); \
        val.field[i] = (uint##N##_t)((val.field[i] ^ mask) - mask); \
    } \
    sp[-1].v128 = val; \
    WAH_NEXT(); \
}

#define V128_ALL_TRUE_OP(N, field) { \
    wah_v128_t val = sp[-1].v128; \
    int32_t result = 1; \
    for (int i = 0; i < 128/N; ++i) { \
        if (val.field[i] == 0) { \
            result = 0; \
            break; \
        } \
    } \
    sp[-1].i32 = result; \
    WAH_NEXT(); \
}

#define V128_BITMASK_OP(N, field) { \
    wah_v128_t val = sp[-1].v128; \
    int32_t result = 0; \
    for (int i = 0; i < 128/N; ++i) { \
        if (val.field[i] < 0) { \
            result |= (1 << i); \
        } \
    } \
    sp[-1].i32 = result; \
    WAH_NEXT(); \
}

#define V128_EXTEND_LOW_OP(DST_N, DST_FIELD, SRC_N, SRC_FIELD, SIGN_TYPE) { \
    wah_v128_t val = sp[-1].v128; \
    wah_v128_t result; \
    for (int i = 0; i < 128/DST_N; ++i) result.DST_FIELD[i] = (SIGN_TYPE##DST_N##_t)val.SRC_FIELD[i]; \
    sp[-1].v128 = result; \
    WAH_NEXT(); \
}

#define V128_EXTEND_HIGH_OP(DST_N, DST_FIELD, SRC_N, SRC_FIELD, SIGN_TYPE) { \
    wah_v128_t val = sp[-1].v128; \
    wah_v128_t result; \
    for (int i = 0; i < 128/DST_N; ++i) result.DST_FIELD[i] = (SIGN_TYPE##DST_N##_t)val.SRC_FIELD[i + (128/SRC_N)/2]; \
    sp[-1].v128 = result; \
    WAH_NEXT(); \
}

#define V128_EXTMUL_LOW_OP(DST_N, DST_FIELD, INTERM_T, SRC_N, SRC_FIELD, SIGN_TYPE) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    wah_v128_t result; \
    for (int i = 0; i < 128/DST_N; ++i) { \
        result.DST_FIELD[i] = (SIGN_TYPE##DST_N##_t)((INTERM_T)a.SRC_FIELD[i] * (INTERM_T)b.SRC_FIELD[i]); \
    } \
    sp[-2].v128 = result; \
    sp--; \
    WAH_NEXT(); \
}

#define V128_EXTMUL_HIGH_OP(DST_N, DST_FIELD, INTERM_T, SRC_N, SRC_FIELD, SIGN_TYPE) { \
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128; \
    wah_v128_t result; \
    int offset = (128/SRC_N)/2; \
    for (int i = 0; i < 128/DST_N; ++i) { \
        result.DST_FIELD[i] = (SIGN_TYPE##DST_N##_t)((INTERM_T)a.SRC_FIELD[i + offset] * (INTERM_T)b.SRC_FIELD[i + offset]); \
    } \
    sp[-2].v128 = result; \
    sp--; \
    WAH_NEXT(); \
}

WAH_RUN(I8X16_ABS) M128I_UNARY_OP(wah_i8x16_abs_sse2, N128_UNARY_OP(wah_vabsq_s8, V128_ABS_OP_WRAP(8, u8)))
WAH_RUN(I8X16_NEG)
    WAH_IF_X86_64({ sp[-1]._m128i = _mm_sub_epi8(_mm_setzero_si128(), sp[-1]._m128i); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_s8, V128_NEG_OP_LANE_WRAP(8, u8)))
WAH_RUN(I8X16_POPCNT) M128I_UNARY_OP(wah_i8x16_popcnt_sse2, N128_UNARY_OP(vcntq_u8, V128_UNARY_OP_LANE_FN(8, wah_popcount_u8, u8)))
WAH_RUN(I8X16_ALL_TRUE)
    WAH_IF_X86_64({ sp[-1].i32 = (_mm_movemask_epi8(_mm_cmpeq_epi8(sp[-1]._m128i, _mm_setzero_si128())) == 0); WAH_NEXT(); },
        N128_UNARY_I32_OP(wah_all_true_i8x16_neon, V128_ALL_TRUE_OP(8, u8)))
WAH_RUN(I8X16_BITMASK) M128I_UNARY_I32_OP(_mm_movemask_epi8, N128_UNARY_I32_OP(wah_bitmask_i8x16_neon, V128_BITMASK_OP(8, i8)))
WAH_RUN(I8X16_NARROW_I16X8_S)
    M128I_BINARY_OP(_mm_packs_epi16, N128_BINARY_OP(wah_i8x16_narrow_i16x8_s_neon,
        V128_TRUNC_SAT_PAIR_OP(wah_trunc_sat_i16_to_i8, i8, i16, 8)))
WAH_RUN(I8X16_NARROW_I16X8_U)
    M128I_BINARY_OP(_mm_packus_epi16, N128_BINARY_OP(wah_i8x16_narrow_i16x8_u_neon,
        V128_TRUNC_SAT_PAIR_OP(wah_trunc_sat_i16_to_u8, u8, u16, 8)))
WAH_RUN(I8X16_SHL) M128I_SHIFT_OP(wah_i8x16_shl_sse2, -1, N128_SHIFT_OP(wah_i8x16_shl_neon, V128_SHL_OP_LANE_WRAP(8, u8)))
WAH_RUN(I8X16_SHR_S) M128I_SHIFT_OP(wah_i8x16_shr_s_sse2, -1, N128_SHIFT_OP(wah_i8x16_shr_s_neon, V128_SHIFT_OP_LANE(8, >>, i8)))
WAH_RUN(I8X16_SHR_U) M128I_SHIFT_OP(wah_i8x16_shr_u_sse2, -1, N128_SHIFT_OP(wah_i8x16_shr_u_neon, V128_SHIFT_OP_LANE_U(8, >>, u8)))
WAH_RUN(I8X16_MIN_S) M128I_BINARY_OP(wah_i8x16_min_s_sse2, N128_BINARY_OP(wah_vminq_s8, V128_BINARY_OP_LANE_FN(8, WAH_MIN_S_8, i8)))
WAH_RUN(I8X16_MIN_U) M128I_BINARY_OP(_mm_min_epu8, N128_BINARY_OP(wah_vminq_u8, V128_BINARY_OP_LANE_FN(8, WAH_MIN_U_8, u8)))
WAH_RUN(I8X16_MAX_S) M128I_BINARY_OP(wah_i8x16_max_s_sse2, N128_BINARY_OP(wah_vmaxq_s8, V128_BINARY_OP_LANE_FN(8, WAH_MAX_S_8, i8)))
WAH_RUN(I8X16_MAX_U) M128I_BINARY_OP(_mm_max_epu8, N128_BINARY_OP(wah_vmaxq_u8, V128_BINARY_OP_LANE_FN(8, WAH_MAX_U_8, u8)))
WAH_RUN(I8X16_AVGR_U) M128I_BINARY_OP(_mm_avg_epu8, N128_BINARY_OP(vrhaddq_u8, {
    wah_v128_t b = sp[-1].v128;
    wah_v128_t a = sp[-2].v128;
    for (int i = 0; i < 16; ++i) {
        a.u8[i] = (uint8_t)(((uint16_t)a.u8[i] + (uint16_t)b.u8[i] + 1) >> 1);
    }
    sp[-2].v128 = a;
    sp--;
    WAH_NEXT();
}))

WAH_RUN(I16X8_EXTADD_PAIRWISE_I8X16_S) N128_UNARY_OP(wah_extadd_pairwise_s8_neon, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result;
    for (int i = 0; i < 8; ++i) {
        result.i16[i] = (int16_t)val.i8[i*2] + (int16_t)val.i8[i*2+1];
    }
    sp[-1].v128 = result;
    WAH_NEXT();
})
WAH_RUN(I16X8_EXTADD_PAIRWISE_I8X16_U) N128_UNARY_OP(wah_extadd_pairwise_u8_neon, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result;
    for (int i = 0; i < 8; ++i) {
        result.u16[i] = (uint16_t)val.u8[i*2] + (uint16_t)val.u8[i*2+1];
    }
    sp[-1].v128 = result;
    WAH_NEXT();
})
WAH_RUN(I16X8_ABS) M128I_UNARY_OP(wah_i16x8_abs_sse2, N128_UNARY_OP(wah_vabsq_s16, V128_ABS_OP_WRAP(16, u16)))
WAH_RUN(I16X8_NEG)
    WAH_IF_X86_64({ sp[-1]._m128i = _mm_sub_epi16(_mm_setzero_si128(), sp[-1]._m128i); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_s16, V128_NEG_OP_LANE_WRAP(16, u16)))

WAH_RUN(I16X8_Q15MULR_SAT_S) {
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128;
    sp[-2].v128 = wah_q15mulr_sat_s(a, b);
    sp--;
    WAH_NEXT();
}
WAH_RUN(I16X8_ALL_TRUE)
    WAH_IF_X86_64({ sp[-1].i32 = (_mm_movemask_epi8(_mm_cmpeq_epi16(sp[-1]._m128i, _mm_setzero_si128())) == 0); WAH_NEXT(); },
        V128_ALL_TRUE_OP(16, u16))
WAH_RUN(I16X8_BITMASK) M128I_UNARY_I32_OP(wah_i16x8_bitmask_sse2, N128_UNARY_I32_OP(wah_bitmask_i16x8_neon, V128_BITMASK_OP(16, i16)))
WAH_RUN(I16X8_NARROW_I32X4_S)
    M128I_BINARY_OP(_mm_packs_epi32, N128_BINARY_OP(wah_i16x8_narrow_i32x4_s_neon,
        V128_TRUNC_SAT_PAIR_OP(wah_trunc_sat_i32_to_i16, i16, i32, 4)))
WAH_RUN(I16X8_NARROW_I32X4_U)
    M128I_BINARY_OP(wah_i16x8_narrow_i32x4_u_sse2, N128_BINARY_OP(wah_i16x8_narrow_i32x4_u_neon,
        V128_TRUNC_SAT_PAIR_OP(wah_trunc_sat_i32_to_u16, u16, i32, 4)))
WAH_RUN(I16X8_EXTEND_LOW_I8X16_S) N128_UNARY_OP(wah_i16x8_extend_low_i8x16_s_neon, V128_EXTEND_LOW_OP(16, i16, 8, i8, int))
WAH_RUN(I16X8_EXTEND_HIGH_I8X16_S) N128_UNARY_OP(wah_i16x8_extend_high_i8x16_s_neon, V128_EXTEND_HIGH_OP(16, i16, 8, i8, int))
WAH_RUN(I16X8_EXTEND_LOW_I8X16_U) N128_UNARY_OP(wah_i16x8_extend_low_i8x16_u_neon, V128_EXTEND_LOW_OP(16, u16, 8, u8, uint))
WAH_RUN(I16X8_EXTEND_HIGH_I8X16_U) N128_UNARY_OP(wah_i16x8_extend_high_i8x16_u_neon, V128_EXTEND_HIGH_OP(16, u16, 8, u8, uint))
WAH_RUN(I16X8_SHL) M128I_SHIFT_OP(_mm_slli_epi16, 15, N128_SHIFT_OP(wah_i16x8_shl_neon, V128_SHL_OP_LANE_WRAP(16, u16)))
WAH_RUN(I16X8_SHR_S) M128I_SHIFT_OP(_mm_srai_epi16, 15, N128_SHIFT_OP(wah_i16x8_shr_s_neon, V128_SHIFT_OP_LANE(16, >>, i16)))
WAH_RUN(I16X8_SHR_U) M128I_SHIFT_OP(_mm_srli_epi16, 15, N128_SHIFT_OP(wah_i16x8_shr_u_neon, V128_SHIFT_OP_LANE_U(16, >>, u16)))
WAH_RUN(I16X8_MIN_S) M128I_BINARY_OP(_mm_min_epi16, N128_BINARY_OP(wah_vminq_s16, V128_BINARY_OP_LANE_FN(16, WAH_MIN_S_16, i16)))
WAH_RUN(I16X8_MIN_U) M128I_BINARY_OP(wah_i16x8_min_u_sse2, N128_BINARY_OP(wah_vminq_u16, V128_BINARY_OP_LANE_FN(16, WAH_MIN_U_16, u16)))
WAH_RUN(I16X8_MAX_S) M128I_BINARY_OP(_mm_max_epi16, N128_BINARY_OP(wah_vmaxq_s16, V128_BINARY_OP_LANE_FN(16, WAH_MAX_S_16, i16)))
WAH_RUN(I16X8_MAX_U) M128I_BINARY_OP(wah_i16x8_max_u_sse2, N128_BINARY_OP(wah_vmaxq_u16, V128_BINARY_OP_LANE_FN(16, WAH_MAX_U_16, u16)))
WAH_RUN(I16X8_AVGR_U) M128I_BINARY_OP(_mm_avg_epu16, N128_BINARY_OP(wah_avgr_u16_neon, {
    wah_v128_t b = sp[-1].v128;
    wah_v128_t a = sp[-2].v128;
    for (int i = 0; i < 8; ++i) {
        a.u16[i] = (uint16_t)(((uint32_t)a.u16[i] + (uint32_t)b.u16[i] + 1) >> 1);
    }
    sp[-2].v128 = a;
    sp--;
    WAH_NEXT();
}))

WAH_RUN(I16X8_EXTMUL_LOW_I8X16_S) N128_BINARY_OP(wah_extmul_low_s8_neon, V128_EXTMUL_LOW_OP(16, i16, int32_t, 8, i8, int))
WAH_RUN(I16X8_EXTMUL_HIGH_I8X16_S) N128_BINARY_OP(wah_extmul_high_s8_neon, V128_EXTMUL_HIGH_OP(16, i16, int32_t, 8, i8, int))
WAH_RUN(I16X8_EXTMUL_LOW_I8X16_U) N128_BINARY_OP(wah_extmul_low_u8_neon, V128_EXTMUL_LOW_OP(16, u16, uint32_t, 8, u8, uint))
WAH_RUN(I16X8_EXTMUL_HIGH_I8X16_U) N128_BINARY_OP(wah_extmul_high_u8_neon, V128_EXTMUL_HIGH_OP(16, u16, uint32_t, 8, u8, uint))

WAH_RUN(I32X4_EXTADD_PAIRWISE_I16X8_S) N128_UNARY_OP(wah_extadd_pairwise_s16_neon, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result;
    for (int i = 0; i < 4; ++i) {
        result.i32[i] = (int32_t)val.i16[i*2] + (int32_t)val.i16[i*2+1];
    }
    sp[-1].v128 = result;
    WAH_NEXT();
})
WAH_RUN(I32X4_EXTADD_PAIRWISE_I16X8_U) N128_UNARY_OP(wah_extadd_pairwise_u16_neon, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result;
    for (int i = 0; i < 4; ++i) {
        result.u32[i] = (uint32_t)val.u16[i*2] + (uint32_t)val.u16[i*2+1];
    }
    sp[-1].v128 = result;
    WAH_NEXT();
})

WAH_RUN(I32X4_ABS) M128I_UNARY_OP(wah_i32x4_abs_sse2, N128_UNARY_OP(wah_vabsq_s32, V128_ABS_OP_WRAP(32, u32)))
WAH_RUN(I32X4_NEG)
    WAH_IF_X86_64({ sp[-1]._m128i = _mm_sub_epi32(_mm_setzero_si128(), sp[-1]._m128i); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_s32, V128_NEG_OP_LANE_WRAP(32, u32)))
WAH_RUN(I32X4_ALL_TRUE)
    WAH_IF_X86_64({ sp[-1].i32 = (_mm_movemask_epi8(_mm_cmpeq_epi32(sp[-1]._m128i, _mm_setzero_si128())) == 0); WAH_NEXT(); },
        N128_UNARY_I32_OP(wah_all_true_i32x4_neon, V128_ALL_TRUE_OP(32, u32)))
WAH_RUN(I32X4_BITMASK) M128I_UNARY_I32_OP(wah_i32x4_bitmask_sse2, N128_UNARY_I32_OP(wah_bitmask_i32x4_neon, V128_BITMASK_OP(32, i32)))
WAH_RUN(I32X4_EXTEND_LOW_I16X8_S) N128_UNARY_OP(wah_i32x4_extend_low_i16x8_s_neon, V128_EXTEND_LOW_OP(32, i32, 16, i16, int))
WAH_RUN(I32X4_EXTEND_HIGH_I16X8_S) N128_UNARY_OP(wah_i32x4_extend_high_i16x8_s_neon, V128_EXTEND_HIGH_OP(32, i32, 16, i16, int))
WAH_RUN(I32X4_EXTEND_LOW_I16X8_U) N128_UNARY_OP(wah_i32x4_extend_low_i16x8_u_neon, V128_EXTEND_LOW_OP(32, u32, 16, u16, uint))
WAH_RUN(I32X4_EXTEND_HIGH_I16X8_U) N128_UNARY_OP(wah_i32x4_extend_high_i16x8_u_neon, V128_EXTEND_HIGH_OP(32, u32, 16, u16, uint))
WAH_RUN(I32X4_SHL) M128I_SHIFT_OP(_mm_slli_epi32, 31, N128_SHIFT_OP(wah_i32x4_shl_neon, V128_SHL_OP_LANE_WRAP(32, u32)))
WAH_RUN(I32X4_SHR_S) M128I_SHIFT_OP(_mm_srai_epi32, 31, N128_SHIFT_OP(wah_i32x4_shr_s_neon, V128_SHIFT_OP_LANE(32, >>, i32)))
WAH_RUN(I32X4_SHR_U) M128I_SHIFT_OP(_mm_srli_epi32, 31, N128_SHIFT_OP(wah_i32x4_shr_u_neon, V128_SHIFT_OP_LANE_U(32, >>, u32)))
WAH_RUN(I32X4_MIN_S) M128I_BINARY_OP(wah_i32x4_min_s_sse2, N128_BINARY_OP(wah_vminq_s32, V128_BINARY_OP_LANE_FN(32, WAH_MIN_S_32, i32)))
WAH_RUN(I32X4_MIN_U) M128I_BINARY_OP(wah_i32x4_min_u_sse2, N128_BINARY_OP(wah_vminq_u32, V128_BINARY_OP_LANE_FN(32, WAH_MIN_U_32, u32)))
WAH_RUN(I32X4_MAX_S) M128I_BINARY_OP(wah_i32x4_max_s_sse2, N128_BINARY_OP(wah_vmaxq_s32, V128_BINARY_OP_LANE_FN(32, WAH_MAX_S_32, i32)))
WAH_RUN(I32X4_MAX_U) M128I_BINARY_OP(wah_i32x4_max_u_sse2, N128_BINARY_OP(wah_vmaxq_u32, V128_BINARY_OP_LANE_FN(32, WAH_MAX_U_32, u32)))

WAH_RUN(I32X4_DOT_I16X8_S) M128I_BINARY_OP(_mm_madd_epi16, {
    wah_v128_t b = sp[-1].v128;
    wah_v128_t a = sp[-2].v128;
    wah_v128_t result;
    for (int i = 0; i < 4; ++i) {
        int32_t p0 = (int32_t)a.i16[i*2] * (int32_t)b.i16[i*2];
        int32_t p1 = (int32_t)a.i16[i*2+1] * (int32_t)b.i16[i*2+1];
        result.u32[i] = (uint32_t)p0 + (uint32_t)p1;
    }
    sp[-2].v128 = result;
    sp--;
    WAH_NEXT();
})
WAH_RUN(I32X4_EXTMUL_LOW_I16X8_S) N128_BINARY_OP(wah_extmul_low_s16_neon, V128_EXTMUL_LOW_OP(32, i32, int64_t, 16, i16, int))
WAH_RUN(I32X4_EXTMUL_HIGH_I16X8_S) N128_BINARY_OP(wah_extmul_high_s16_neon, V128_EXTMUL_HIGH_OP(32, i32, int64_t, 16, i16, int))
WAH_RUN(I32X4_EXTMUL_LOW_I16X8_U) N128_BINARY_OP(wah_extmul_low_u16_neon, V128_EXTMUL_LOW_OP(32, u32, uint64_t, 16, u16, uint))
WAH_RUN(I32X4_EXTMUL_HIGH_I16X8_U) N128_BINARY_OP(wah_extmul_high_u16_neon, V128_EXTMUL_HIGH_OP(32, u32, uint64_t, 16, u16, uint))

WAH_RUN(I64X2_ABS) M128I_UNARY_OP(wah_i64x2_abs_sse2, N128_UNARY_OP(wah_vabsq_s64, V128_ABS_OP_WRAP(64, u64)))
WAH_RUN(I64X2_NEG)
    WAH_IF_X86_64({ sp[-1]._m128i = _mm_sub_epi64(_mm_setzero_si128(), sp[-1]._m128i); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_s64, V128_NEG_OP_LANE_WRAP(64, u64)))
WAH_RUN(I64X2_ALL_TRUE) M128I_UNARY_I32_OP(wah_i64x2_all_true_sse2, N128_UNARY_I32_OP(wah_all_true_i64x2_neon, V128_ALL_TRUE_OP(64, u64)))
WAH_RUN(I64X2_BITMASK) M128I_UNARY_I32_OP(wah_i64x2_bitmask_sse2, N128_UNARY_I32_OP(wah_bitmask_i64x2_neon, V128_BITMASK_OP(64, i64)))
WAH_RUN(I64X2_EXTEND_LOW_I32X4_S) N128_UNARY_OP(wah_i64x2_extend_low_i32x4_s_neon, V128_EXTEND_LOW_OP(64, i64, 32, i32, int))
WAH_RUN(I64X2_EXTEND_HIGH_I32X4_S) N128_UNARY_OP(wah_i64x2_extend_high_i32x4_s_neon, V128_EXTEND_HIGH_OP(64, i64, 32, i32, int))
WAH_RUN(I64X2_EXTEND_LOW_I32X4_U) N128_UNARY_OP(wah_i64x2_extend_low_i32x4_u_neon, V128_EXTEND_LOW_OP(64, u64, 32, u32, uint))
WAH_RUN(I64X2_EXTEND_HIGH_I32X4_U) N128_UNARY_OP(wah_i64x2_extend_high_i32x4_u_neon, V128_EXTEND_HIGH_OP(64, u64, 32, u32, uint))
WAH_RUN(I64X2_SHL) M128I_SHIFT_OP(_mm_slli_epi64, 63, N128_SHIFT_OP(wah_i64x2_shl_neon, V128_SHL_OP_LANE_WRAP(64, u64)))
WAH_RUN(I64X2_SHR_S) V128_SHIFT_OP_LANE(64, >>, i64)
WAH_RUN(I64X2_SHR_U) M128I_SHIFT_OP(_mm_srli_epi64, 63, N128_SHIFT_OP(wah_i64x2_shr_u_neon, V128_SHIFT_OP_LANE_U(64, >>, u64)))
WAH_RUN(I64X2_EXTMUL_LOW_I32X4_S) V128_EXTMUL_LOW_OP(64, i64, int64_t, 32, i32, int)
WAH_RUN(I64X2_EXTMUL_HIGH_I32X4_S) V128_EXTMUL_HIGH_OP(64, i64, int64_t, 32, i32, int)
WAH_RUN(I64X2_EXTMUL_LOW_I32X4_U) V128_EXTMUL_LOW_OP(64, u64, uint64_t, 32, u32, uint)
WAH_RUN(I64X2_EXTMUL_HIGH_I32X4_U) V128_EXTMUL_HIGH_OP(64, u64, uint64_t, 32, u32, uint)

WAH_RUN(F32X4_CEIL) N128_FP_UNARY_OP_F32(wah_vrndpq_f32, V128_UNARY_OP_LANE_FN(32, wah_ceilf, f32))
WAH_RUN(F32X4_FLOOR) N128_FP_UNARY_OP_F32(wah_vrndmq_f32, V128_UNARY_OP_LANE_FN(32, wah_floorf, f32))
WAH_RUN(F32X4_TRUNC) N128_FP_UNARY_OP_F32(wah_vrndq_f32, V128_UNARY_OP_LANE_FN(32, wah_truncf, f32))
WAH_RUN(F32X4_NEAREST) N128_FP_UNARY_OP_F32(wah_vrndnq_f32, V128_UNARY_OP_LANE_FN(32, wah_nearest_f32, f32))
WAH_RUN(F32X4_ABS)
    WAH_IF_X86_64({ sp[-1]._m128 = _mm_andnot_ps(_mm_set1_ps(-0.0f), sp[-1]._m128); WAH_NEXT(); },
        N128_UNARY_OP(wah_vabsq_f32, V128_UNARY_OP_LANE_FN(32, wah_fabsf, f32)))
WAH_RUN(F32X4_NEG)
    WAH_IF_X86_64({ sp[-1]._m128 = _mm_xor_ps(_mm_set1_ps(-0.0f), sp[-1]._m128); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_f32, V128_UNARY_OP_LANE(32, -, f32)))
WAH_RUN(F32X4_SQRT) M128_FP_UNARY_OP(_mm_sqrt_ps, N128_FP_UNARY_OP_F32(wah_vsqrtq_f32, V128_UNARY_OP_LANE_FN(32, wah_sqrtf, f32)))
WAH_RUN(F32X4_MIN) M128_BINARY_OP(wah_f32x4_min_sse2, N128_BINARY_OP(wah_f32x4_min_neon, V128_BINARY_OP_LANE_FN(32, wah_minf, f32)))
WAH_RUN(F32X4_MAX) M128_BINARY_OP(wah_f32x4_max_sse2, N128_BINARY_OP(wah_f32x4_max_neon, V128_BINARY_OP_LANE_FN(32, wah_maxf, f32)))
WAH_RUN(F32X4_PMIN) M128_BINARY_OP(wah_f32x4_pmin_sse2, N128_BINARY_OP(wah_f32x4_pmin_neon, V128_BINARY_OP_LANE_FN(32, wah_pminf, f32)))
WAH_RUN(F32X4_PMAX) M128_BINARY_OP(wah_f32x4_pmax_sse2, N128_BINARY_OP(wah_f32x4_pmax_neon, V128_BINARY_OP_LANE_FN(32, wah_pmaxf, f32)))

WAH_RUN(F64X2_CEIL) N128_FP_UNARY_OP_F64(wah_vrndpq_f64, V128_UNARY_OP_LANE_FN(64, wah_ceil, f64))
WAH_RUN(F64X2_FLOOR) N128_FP_UNARY_OP_F64(wah_vrndmq_f64, V128_UNARY_OP_LANE_FN(64, wah_floor, f64))
WAH_RUN(F64X2_TRUNC) N128_FP_UNARY_OP_F64(wah_vrndq_f64, V128_UNARY_OP_LANE_FN(64, wah_trunc, f64))
WAH_RUN(F64X2_NEAREST) N128_FP_UNARY_OP_F64(wah_vrndnq_f64, V128_UNARY_OP_LANE_FN(64, wah_nearest_f64, f64))
WAH_RUN(F64X2_ABS)
    WAH_IF_X86_64({ sp[-1]._m128d = _mm_andnot_pd(_mm_set1_pd(-0.0), sp[-1]._m128d); WAH_NEXT(); },
        N128_UNARY_OP(wah_vabsq_f64, V128_UNARY_OP_LANE_FN(64, wah_fabs, f64)))
WAH_RUN(F64X2_NEG)
    WAH_IF_X86_64({ sp[-1]._m128d = _mm_xor_pd(_mm_set1_pd(-0.0), sp[-1]._m128d); WAH_NEXT(); },
        N128_UNARY_OP(wah_vnegq_f64, V128_UNARY_OP_LANE(64, -, f64)))
WAH_RUN(F64X2_SQRT) M128D_FP_UNARY_OP(_mm_sqrt_pd, N128_FP_UNARY_OP_F64(wah_vsqrtq_f64, V128_UNARY_OP_LANE_FN(64, wah_sqrt, f64)))
WAH_RUN(F64X2_MIN) M128D_BINARY_OP(wah_f64x2_min_sse2, N128_BINARY_OP(wah_f64x2_min_neon, V128_BINARY_OP_LANE_FN(64, wah_min, f64)))
WAH_RUN(F64X2_MAX) M128D_BINARY_OP(wah_f64x2_max_sse2, N128_BINARY_OP(wah_f64x2_max_neon, V128_BINARY_OP_LANE_FN(64, wah_max, f64)))
WAH_RUN(F64X2_PMIN) M128D_BINARY_OP(wah_f64x2_pmin_sse2, N128_BINARY_OP(wah_f64x2_pmin_neon, V128_BINARY_OP_LANE_FN(64, wah_pmin, f64)))
WAH_RUN(F64X2_PMAX) M128D_BINARY_OP(wah_f64x2_pmax_sse2, N128_BINARY_OP(wah_f64x2_pmax_neon, V128_BINARY_OP_LANE_FN(64, wah_pmax, f64)))

WAH_RUN(V128_BITSELECT) M128I_TERNARY_OP(_mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(c, b)), N128_TERNARY_OP(vbslq_u8(c, a, b), {
    wah_v128_t v3 = sp[-1].v128;
    wah_v128_t v2 = sp[-2].v128;
    wah_v128_t v1 = sp[-3].v128;
    wah_v128_t result;
    for (int i = 0; i < 16; ++i) {
        result.u8[i] = (v1.u8[i] & v3.u8[i]) | (v2.u8[i] & ~v3.u8[i]);
    }
    sp[-3].v128 = result;
    sp -= 2;
    WAH_NEXT();
}))

WAH_RUN(V128_ANY_TRUE) WAH_IF_X86_64({
    sp[-1].i32 = (_mm_movemask_epi8(_mm_cmpeq_epi8(sp[-1]._m128i, _mm_setzero_si128())) != 0xffff);
    WAH_NEXT();
}, N128_UNARY_I32_OP(wah_any_true_neon, {
    wah_v128_t val = sp[-1].v128;
    int32_t result = 0;
    for (int i = 0; i < 16; ++i) {
        if (val.u8[i] != 0) {
            result = 1;
            break;
        }
    }
    sp[-1].i32 = result;
    WAH_NEXT();
}))

WAH_RUN(I8X16_EQ) M128I_BINARY_OP(_mm_cmpeq_epi8, N128_BINARY_OP(wah_vceqq_s8, V128_CMP_I_LANE(8, ==, u8)))
WAH_RUN(I8X16_NE) M128I_NOT_BINARY_OP(_mm_cmpeq_epi8, N128_NOT_BINARY_OP(wah_vceqq_s8, V128_CMP_I_LANE(8, !=, u8)))
WAH_RUN(I8X16_LT_S) M128I_BINARY_OP(_mm_cmplt_epi8, N128_BINARY_OP(wah_vcltq_s8, V128_CMP_I_LANE_S(8, <, i8)))
WAH_RUN(I8X16_LT_U) M128I_BINARY_OP(wah_i8x16_lt_u_sse2, N128_BINARY_OP(wah_vcltq_u8, V128_CMP_I_LANE_U(8, <, u8)))
WAH_RUN(I8X16_GT_S) M128I_BINARY_OP(_mm_cmpgt_epi8, N128_BINARY_OP(wah_vcgtq_s8, V128_CMP_I_LANE_S(8, >, i8)))
WAH_RUN(I8X16_GT_U) M128I_BINARY_OP(wah_i8x16_gt_u_sse2, N128_BINARY_OP(wah_vcgtq_u8, V128_CMP_I_LANE_U(8, >, u8)))
WAH_RUN(I8X16_LE_S) M128I_NOT_BINARY_OP(_mm_cmpgt_epi8, N128_NOT_BINARY_OP(wah_vcgtq_s8, V128_CMP_I_LANE_S(8, <=, i8)))
WAH_RUN(I8X16_LE_U) M128I_NOT_BINARY_OP(wah_i8x16_gt_u_sse2, N128_NOT_BINARY_OP(wah_vcgtq_u8, V128_CMP_I_LANE_U(8, <=, u8)))
WAH_RUN(I8X16_GE_S) M128I_NOT_BINARY_OP(_mm_cmplt_epi8, N128_NOT_BINARY_OP(wah_vcltq_s8, V128_CMP_I_LANE_S(8, >=, i8)))
WAH_RUN(I8X16_GE_U) M128I_NOT_BINARY_OP(wah_i8x16_lt_u_sse2, N128_NOT_BINARY_OP(wah_vcltq_u8, V128_CMP_I_LANE_U(8, >=, u8)))

WAH_RUN(I16X8_EQ) M128I_BINARY_OP(_mm_cmpeq_epi16, N128_BINARY_OP(wah_vceqq_s16, V128_CMP_I_LANE(16, ==, u16)))
WAH_RUN(I16X8_NE) M128I_NOT_BINARY_OP(_mm_cmpeq_epi16, N128_NOT_BINARY_OP(wah_vceqq_s16, V128_CMP_I_LANE(16, !=, u16)))
WAH_RUN(I16X8_LT_S) M128I_BINARY_OP(_mm_cmplt_epi16, N128_BINARY_OP(wah_vcltq_s16, V128_CMP_I_LANE_S(16, <, i16)))
WAH_RUN(I16X8_LT_U) M128I_BINARY_OP(wah_i16x8_lt_u_sse2, N128_BINARY_OP(wah_vcltq_u16, V128_CMP_I_LANE_U(16, <, u16)))
WAH_RUN(I16X8_GT_S) M128I_BINARY_OP(_mm_cmpgt_epi16, N128_BINARY_OP(wah_vcgtq_s16, V128_CMP_I_LANE_S(16, >, i16)))
WAH_RUN(I16X8_GT_U) M128I_BINARY_OP(wah_i16x8_gt_u_sse2, N128_BINARY_OP(wah_vcgtq_u16, V128_CMP_I_LANE_U(16, >, u16)))
WAH_RUN(I16X8_LE_S) M128I_NOT_BINARY_OP(_mm_cmpgt_epi16, N128_NOT_BINARY_OP(wah_vcgtq_s16, V128_CMP_I_LANE_S(16, <=, i16)))
WAH_RUN(I16X8_LE_U) M128I_NOT_BINARY_OP(wah_i16x8_gt_u_sse2, N128_NOT_BINARY_OP(wah_vcgtq_u16, V128_CMP_I_LANE_U(16, <=, u16)))
WAH_RUN(I16X8_GE_S) M128I_NOT_BINARY_OP(_mm_cmplt_epi16, N128_NOT_BINARY_OP(wah_vcltq_s16, V128_CMP_I_LANE_S(16, >=, i16)))
WAH_RUN(I16X8_GE_U) M128I_NOT_BINARY_OP(wah_i16x8_lt_u_sse2, N128_NOT_BINARY_OP(wah_vcltq_u16, V128_CMP_I_LANE_U(16, >=, u16)))

WAH_RUN(I32X4_EQ) M128I_BINARY_OP(_mm_cmpeq_epi32, N128_BINARY_OP(wah_vceqq_s32, V128_CMP_I_LANE(32, ==, u32)))
WAH_RUN(I32X4_NE) M128I_NOT_BINARY_OP(_mm_cmpeq_epi32, N128_NOT_BINARY_OP(wah_vceqq_s32, V128_CMP_I_LANE(32, !=, u32)))
WAH_RUN(I32X4_LT_S) M128I_BINARY_OP(_mm_cmplt_epi32, N128_BINARY_OP(wah_vcltq_s32, V128_CMP_I_LANE_S(32, <, i32)))
WAH_RUN(I32X4_LT_U) M128I_BINARY_OP(wah_i32x4_lt_u_sse2, N128_BINARY_OP(wah_vcltq_u32, V128_CMP_I_LANE_U(32, <, u32)))
WAH_RUN(I32X4_GT_S) M128I_BINARY_OP(_mm_cmpgt_epi32, N128_BINARY_OP(wah_vcgtq_s32, V128_CMP_I_LANE_S(32, >, i32)))
WAH_RUN(I32X4_GT_U) M128I_BINARY_OP(wah_i32x4_gt_u_sse2, N128_BINARY_OP(wah_vcgtq_u32, V128_CMP_I_LANE_U(32, >, u32)))
WAH_RUN(I32X4_LE_S) M128I_NOT_BINARY_OP(_mm_cmpgt_epi32, N128_NOT_BINARY_OP(wah_vcgtq_s32, V128_CMP_I_LANE_S(32, <=, i32)))
WAH_RUN(I32X4_LE_U) M128I_NOT_BINARY_OP(wah_i32x4_gt_u_sse2, N128_NOT_BINARY_OP(wah_vcgtq_u32, V128_CMP_I_LANE_U(32, <=, u32)))
WAH_RUN(I32X4_GE_S) M128I_NOT_BINARY_OP(_mm_cmplt_epi32, N128_NOT_BINARY_OP(wah_vcltq_s32, V128_CMP_I_LANE_S(32, >=, i32)))
WAH_RUN(I32X4_GE_U) M128I_NOT_BINARY_OP(wah_i32x4_lt_u_sse2, N128_NOT_BINARY_OP(wah_vcltq_u32, V128_CMP_I_LANE_U(32, >=, u32)))

WAH_RUN(I64X2_EQ) N128_BINARY_OP(wah_vceqq_s64, V128_CMP_I_LANE(64, ==, u64))
WAH_RUN(I64X2_NE) N128_NOT_BINARY_OP(wah_vceqq_s64, V128_CMP_I_LANE(64, !=, u64))
WAH_RUN(I64X2_LT_S) N128_BINARY_OP(wah_vcltq_s64, V128_CMP_I_LANE_S(64, <, i64))
WAH_RUN(I64X2_GT_S) N128_BINARY_OP(wah_vcgtq_s64, V128_CMP_I_LANE_S(64, >, i64))
WAH_RUN(I64X2_LE_S) N128_NOT_BINARY_OP(wah_vcgtq_s64, V128_CMP_I_LANE_S(64, <=, i64))
WAH_RUN(I64X2_GE_S) N128_NOT_BINARY_OP(wah_vcltq_s64, V128_CMP_I_LANE_S(64, >=, i64))

WAH_RUN(F32X4_EQ) M128_BINARY_OP(_mm_cmpeq_ps, N128_BINARY_OP(wah_vceqq_f32, V128_CMP_F_LANE(32, ==, f32)))
WAH_RUN(F32X4_NE) M128_NOT_BINARY_OP(_mm_cmpeq_ps, N128_NOT_BINARY_OP(wah_vceqq_f32, V128_CMP_F_LANE(32, !=, f32)))
WAH_RUN(F32X4_LT) M128_BINARY_OP(_mm_cmplt_ps, N128_BINARY_OP(wah_vcltq_f32, V128_CMP_F_LANE(32, <, f32)))
WAH_RUN(F32X4_GT) M128_BINARY_OP(_mm_cmpgt_ps, N128_BINARY_OP(wah_vcgtq_f32, V128_CMP_F_LANE(32, >, f32)))
WAH_RUN(F32X4_LE) M128_BINARY_OP(_mm_cmple_ps, N128_BINARY_OP(wah_vcleq_f32, V128_CMP_F_LANE(32, <=, f32)))
WAH_RUN(F32X4_GE) M128_BINARY_OP(_mm_cmpge_ps, N128_BINARY_OP(wah_vcgeq_f32, V128_CMP_F_LANE(32, >=, f32)))

WAH_RUN(F64X2_EQ) M128D_BINARY_OP(_mm_cmpeq_pd, N128_BINARY_OP(wah_vceqq_f64, V128_CMP_F_LANE(64, ==, f64)))
WAH_RUN(F64X2_NE) M128D_NOT_BINARY_OP(_mm_cmpeq_pd, N128_NOT_BINARY_OP(wah_vceqq_f64, V128_CMP_F_LANE(64, !=, f64)))
WAH_RUN(F64X2_LT) M128D_BINARY_OP(_mm_cmplt_pd, N128_BINARY_OP(wah_vcltq_f64, V128_CMP_F_LANE(64, <, f64)))
WAH_RUN(F64X2_GT) M128D_BINARY_OP(_mm_cmpgt_pd, N128_BINARY_OP(wah_vcgtq_f64, V128_CMP_F_LANE(64, >, f64)))
WAH_RUN(F64X2_LE) M128D_BINARY_OP(_mm_cmple_pd, N128_BINARY_OP(wah_vcleq_f64, V128_CMP_F_LANE(64, <=, f64)))
WAH_RUN(F64X2_GE) M128D_BINARY_OP(_mm_cmpge_pd, N128_BINARY_OP(wah_vcgeq_f64, V128_CMP_F_LANE(64, >=, f64)))

WAH_RUN(I32X4_TRUNC_SAT_F32X4_S) N128_UNARY_OP(wah_i32x4_trunc_sat_f32x4_s_neon, {
    wah_v128_t val = sp[-1].v128;
    for (int i = 0; i < 4; ++i) val.i32[i] = wah_trunc_sat_f32_to_i32(val.f32[i]);
    sp[-1].v128 = val;
    WAH_NEXT();
})
WAH_RUN(I32X4_TRUNC_SAT_F32X4_U) N128_UNARY_OP(wah_i32x4_trunc_sat_f32x4_u_neon, {
    wah_v128_t val = sp[-1].v128;
    for (int i = 0; i < 4; ++i) val.u32[i] = wah_trunc_sat_f32_to_u32(val.f32[i]);
    sp[-1].v128 = val;
    WAH_NEXT();
})
WAH_RUN(F32X4_CONVERT_I32X4_S) WAH_IF_X86_64({
    sp[-1]._m128 = _mm_cvtepi32_ps(sp[-1]._m128i);
    WAH_NEXT();
}, N128_UNARY_OP(wah_f32x4_convert_i32x4_s_neon, {
    wah_v128_t val = sp[-1].v128;
    for (int i = 0; i < 4; ++i) val.f32[i] = (float)val.i32[i];
    sp[-1].v128 = val;
    WAH_NEXT();
}))
WAH_RUN(F32X4_CONVERT_I32X4_U) WAH_IF_X86_64({
    sp[-1]._m128 = wah_f32x4_convert_i32x4_u_sse2(sp[-1]._m128i);
    WAH_NEXT();
}, N128_UNARY_OP(wah_f32x4_convert_i32x4_u_neon, {
    wah_v128_t val = sp[-1].v128;
    for (int i = 0; i < 4; ++i) val.f32[i] = (float)val.u32[i];
    sp[-1].v128 = val;
    WAH_NEXT();
}))
WAH_RUN(I32X4_TRUNC_SAT_F64X2_S_ZERO) {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = { .i32 = {wah_trunc_sat_f64_to_i32(val.f64[0]), wah_trunc_sat_f64_to_i32(val.f64[1])} };
    sp[-1].v128 = result;
    WAH_NEXT();
}
WAH_RUN(I32X4_TRUNC_SAT_F64X2_U_ZERO) {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = { .u32 = {wah_trunc_sat_f64_to_u32(val.f64[0]), wah_trunc_sat_f64_to_u32(val.f64[1])} };
    sp[-1].v128 = result;
    WAH_NEXT();
}
WAH_RUN(F64X2_CONVERT_LOW_I32X4_S) WAH_IF_X86_64({
    sp[-1]._m128d = _mm_cvtepi32_pd(sp[-1]._m128i);
    WAH_NEXT();
}, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = ((wah_v128_t) { .f64 = {(double)val.i32[0], (double)val.i32[1]} });
    sp[-1].v128 = result;
    WAH_NEXT();
})
WAH_RUN(F64X2_CONVERT_LOW_I32X4_U) {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = ((wah_v128_t) { .f64 = {(double)val.u32[0], (double)val.u32[1]} });
    sp[-1].v128 = result;
    WAH_NEXT();
}
WAH_RUN(F32X4_DEMOTE_F64X2_ZERO) WAH_IF_X86_64({
    sp[-1]._m128 = wah_canonicalize_f32x4_sse2(_mm_cvtpd_ps(sp[-1]._m128d));
    WAH_NEXT();
}, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = ((wah_v128_t) { .f32 = {wah_canonicalize_f32((float)val.f64[0]), wah_canonicalize_f32((float)val.f64[1])} });
    sp[-1].v128 = result;
    WAH_NEXT();
})
WAH_RUN(F64X2_PROMOTE_LOW_F32X4) WAH_IF_X86_64({
    sp[-1]._m128d = wah_canonicalize_f64x2_sse2(_mm_cvtps_pd(sp[-1]._m128));
    WAH_NEXT();
}, {
    wah_v128_t val = sp[-1].v128;
    wah_v128_t result = ((wah_v128_t) { .f64 = {wah_canonicalize_f64((double)val.f32[0]), wah_canonicalize_f64((double)val.f32[1])} });
    sp[-1].v128 = result;
    WAH_NEXT();
})

WAH_RUN(I8X16_RELAXED_SWIZZLE) N128_BINARY_OP(vqtbl1q_u8, {
    wah_v128_t mask = sp[-1].v128;
    wah_v128_t data = sp[-2].v128;
    wah_v128_t result;
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t lane_idx = mask.u8[i];
        result.u8[i] = lane_idx < 16 ? data.u8[lane_idx] : 0;
    }
    sp[-2].v128 = result;
    sp--;
    WAH_NEXT();
})

WAH_RUN(I32X4_RELAXED_TRUNC_F32X4_S) V128_TRUNC_SAT_F2I_OP(32, f32, i32, wah_trunc_sat_f32_to_i32)
WAH_RUN(I32X4_RELAXED_TRUNC_F32X4_U) V128_TRUNC_SAT_F2I_OP(32, f32, u32, wah_trunc_sat_f32_to_u32)
WAH_RUN(I32X4_RELAXED_TRUNC_F64X2_S) V128_TRUNC_SAT_F2I_ZERO_OP(64, f64, i32, wah_trunc_sat_f64_to_i32)
WAH_RUN(I32X4_RELAXED_TRUNC_F64X2_U) V128_TRUNC_SAT_F2I_ZERO_OP(64, f64, u32, wah_trunc_sat_f64_to_u32)

WAH_RUN(F32X4_RELAXED_MADD)
    M128_FP_TERNARY_OP(_mm_add_ps(_mm_mul_ps(a, b), c), N128_FP_TERNARY_OP_F32(wah_vaddq_f32(wah_vmulq_f32(a, b), c), V128_TERNARY_OP_F(32, *, +, f32)))
WAH_RUN(F32X4_RELAXED_NMADD)
    WAH_IF_X86_64({ __m128 c = sp[-1]._m128; __m128 b = sp[-2]._m128; __m128 a = sp[-3]._m128;
                    sp[-3]._m128 = wah_canonicalize_f32x4_sse2(_mm_sub_ps(c, _mm_mul_ps(a, b))); sp -= 2; WAH_NEXT(); },
        N128_TERNARY_OP(wah_canonicalize_f32x4_neon(wah_vsubq_f32(c, wah_vmulq_f32(a, b))), V128_TERNARY_OP_F(32, *-, +, f32)))
WAH_RUN(F64X2_RELAXED_MADD)
    M128D_FP_TERNARY_OP(_mm_add_pd(_mm_mul_pd(a, b), c), N128_FP_TERNARY_OP_F64(wah_vaddq_f64(wah_vmulq_f64(a, b), c), V128_TERNARY_OP_F(64, *, +, f64)))
WAH_RUN(F64X2_RELAXED_NMADD)
    WAH_IF_X86_64({ __m128d c = sp[-1]._m128d; __m128d b = sp[-2]._m128d; __m128d a = sp[-3]._m128d;
                    sp[-3]._m128d = wah_canonicalize_f64x2_sse2(_mm_sub_pd(c, _mm_mul_pd(a, b))); sp -= 2; WAH_NEXT(); },
        N128_TERNARY_OP(wah_canonicalize_f64x2_neon(wah_vsubq_f64(c, wah_vmulq_f64(a, b))), V128_TERNARY_OP_F(64, *-, +, f64)))

WAH_RUN(I8X16_RELAXED_LANESELECT)
    M128I_TERNARY_OP(_mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(c, b)),
        N128_TERNARY_OP(vbslq_u8(c, a, b), V128_LANESELECT_OP(8, u8)))
WAH_RUN(I16X8_RELAXED_LANESELECT)
    M128I_TERNARY_OP(_mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(c, b)),
        N128_TERNARY_OP(vbslq_u8(c, a, b), V128_LANESELECT_OP(16, u16)))
WAH_RUN(I32X4_RELAXED_LANESELECT)
    M128I_TERNARY_OP(_mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(c, b)),
        N128_TERNARY_OP(vbslq_u8(c, a, b), V128_LANESELECT_OP(32, u32)))
WAH_RUN(I64X2_RELAXED_LANESELECT)
    M128I_TERNARY_OP(_mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(c, b)),
        N128_TERNARY_OP(vbslq_u8(c, a, b), V128_LANESELECT_OP(64, u64)))

WAH_RUN(F32X4_RELAXED_MIN) M128_BINARY_OP(wah_f32x4_min_sse2, N128_FP_BINARY_OP_F32(wah_vminq_f32, V128_BINARY_OP_LANE_FN(32, wah_minf, f32)))
WAH_RUN(F32X4_RELAXED_MAX) M128_BINARY_OP(wah_f32x4_max_sse2, N128_FP_BINARY_OP_F32(wah_vmaxq_f32, V128_BINARY_OP_LANE_FN(32, wah_maxf, f32)))
WAH_RUN(F64X2_RELAXED_MIN) M128D_BINARY_OP(wah_f64x2_min_sse2, N128_FP_BINARY_OP_F64(wah_vminq_f64, V128_BINARY_OP_LANE_FN(64, wah_min, f64)))
WAH_RUN(F64X2_RELAXED_MAX) M128D_BINARY_OP(wah_f64x2_max_sse2, N128_FP_BINARY_OP_F64(wah_vmaxq_f64, V128_BINARY_OP_LANE_FN(64, wah_max, f64)))

WAH_RUN(I16X8_RELAXED_Q15MULR_S) {
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128;
    sp[-2].v128 = wah_q15mulr_sat_s(a, b);
    sp--;
    WAH_NEXT();
}

WAH_RUN(I16X8_RELAXED_DOT_I8X16_I7X16_S) M128I_BINARY_OP(wah_i16x8_relaxed_dot_i8x16_i7x16_s_sse2, {
    wah_v128_t b = sp[-1].v128, a = sp[-2].v128;
    wah_v128_t result;
    for (int i = 0; i < 8; ++i) {
        int8_t a0 = a.i8[i * 2], a1 = a.i8[i * 2 + 1];
        int8_t b0 = b.i8[i * 2], b1 = b.i8[i * 2 + 1];
        result.i16[i] = (int16_t)a0 * b0 + (int16_t)a1 * b1;
    }
    sp[-2].v128 = result;
    sp--;
    WAH_NEXT();
})
WAH_RUN(I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S) M128I_TERNARY_OP(wah_i32x4_relaxed_dot_i8x16_i7x16_add_s_sse2(a, b, c), {
    wah_v128_t a = sp[-3].v128;
    wah_v128_t b = sp[-2].v128;
    wah_v128_t c = sp[-1].v128;
    wah_v128_t result;
    for (int i = 0; i < 4; ++i) {
        int8_t a0 = a.i8[i*4], a1 = a.i8[i*4+1], a2 = a.i8[i*4+2], a3 = a.i8[i*4+3];
        int8_t b0 = b.i8[i*4], b1 = b.i8[i*4+1], b2 = b.i8[i*4+2], b3 = b.i8[i*4+3];
        result.i32[i] = c.i32[i] + (int32_t)a0*b0 + (int32_t)a1*b1 + (int32_t)a2*b2 + (int32_t)a3*b3;
    }
    sp[-3].v128 = result;
    sp -= 2;
    WAH_NEXT();
})

// x86-64 specific optimized opcodes
WAH_IF_X86_64(
    WAH_IF_SSSE3(
        WAH_RUN(I8X16_ABS_ssse3) M128I_UNARY_OP(wah_mm_abs_epi8)
        WAH_RUN(I16X8_ABS_ssse3) M128I_UNARY_OP(wah_mm_abs_epi16)
        WAH_RUN(I32X4_ABS_ssse3) M128I_UNARY_OP(wah_mm_abs_epi32)
        WAH_RUN(I8X16_RELAXED_SWIZZLE_ssse3) {
            // pshufb only zeros lanes when mask MSB is set. For WASM semantics,
            // we need to zero lanes where mask >= 16. Set MSB for those lanes.
            __m128i mask = sp[-1]._m128i;
            __m128i oob_mask = _mm_cmpgt_epi8(mask, _mm_set1_epi8(15));
            mask = _mm_or_si128(mask, oob_mask);
            sp[-2]._m128i = wah_mm_shuffle_epi8(sp[-2]._m128i, mask);
            sp--;
            WAH_NEXT();
        }
        WAH_RUN(I16X8_Q15MULR_SAT_S_ssse3) {
            __m128i a = sp[-2]._m128i, b = sp[-1]._m128i;
            __m128i result = wah_mm_mulhrs_epi16(a, b);
            __m128i min16 = _mm_set1_epi16(INT16_MIN);
            __m128i both_min = _mm_and_si128(_mm_cmpeq_epi16(a, min16), _mm_cmpeq_epi16(b, min16));
            sp[-2]._m128i = _mm_xor_si128(result, both_min);
            sp--;
            WAH_NEXT();
        }
        WAH_RUN(I16X8_RELAXED_Q15MULR_S_ssse3) M128I_BINARY_OP(wah_mm_mulhrs_epi16)
    )

    WAH_IF_SSE41(
        WAH_RUN(I32X4_MUL_sse41) M128I_BINARY_OP(wah_mm_mullo_epi32)
        WAH_RUN(I8X16_MIN_S_sse41) M128I_BINARY_OP(wah_mm_min_epi8)
        WAH_RUN(I8X16_MAX_S_sse41) M128I_BINARY_OP(wah_mm_max_epi8)
        WAH_RUN(I16X8_EXTEND_LOW_I8X16_S_sse41) M128I_UNARY_OP(wah_mm_cvtepi8_epi16)
        WAH_RUN(I16X8_EXTEND_HIGH_I8X16_S_sse41) M128I_UNARY_OP(wah_i16x8_extend_high_i8x16_s_sse41)
        WAH_RUN(I16X8_EXTEND_LOW_I8X16_U_sse41) M128I_UNARY_OP(wah_mm_cvtepu8_epi16)
        WAH_RUN(I16X8_EXTEND_HIGH_I8X16_U_sse41) M128I_UNARY_OP(wah_i16x8_extend_high_i8x16_u_sse41)
        WAH_RUN(I16X8_MIN_U_sse41) M128I_BINARY_OP(wah_mm_min_epu16)
        WAH_RUN(I16X8_MAX_U_sse41) M128I_BINARY_OP(wah_mm_max_epu16)
        WAH_RUN(I16X8_EXTMUL_LOW_I8X16_S_sse41) M128I_BINARY_OP(wah_i16x8_extmul_low_i8x16_s_sse41)
        WAH_RUN(I16X8_EXTMUL_HIGH_I8X16_S_sse41) M128I_BINARY_OP(wah_i16x8_extmul_high_i8x16_s_sse41)
        WAH_RUN(I16X8_EXTMUL_LOW_I8X16_U_sse41) M128I_BINARY_OP(wah_i16x8_extmul_low_i8x16_u_sse41)
        WAH_RUN(I16X8_EXTMUL_HIGH_I8X16_U_sse41) M128I_BINARY_OP(wah_i16x8_extmul_high_i8x16_u_sse41)
        WAH_RUN(I32X4_EXTEND_LOW_I16X8_S_sse41) M128I_UNARY_OP(wah_mm_cvtepi16_epi32)
        WAH_RUN(I32X4_EXTEND_HIGH_I16X8_S_sse41) M128I_UNARY_OP(wah_i32x4_extend_high_i16x8_s_sse41)
        WAH_RUN(I32X4_EXTEND_LOW_I16X8_U_sse41) M128I_UNARY_OP(wah_mm_cvtepu16_epi32)
        WAH_RUN(I32X4_EXTEND_HIGH_I16X8_U_sse41) M128I_UNARY_OP(wah_i32x4_extend_high_i16x8_u_sse41)
        WAH_RUN(I32X4_MIN_S_sse41) M128I_BINARY_OP(wah_mm_min_epi32)
        WAH_RUN(I32X4_MIN_U_sse41) M128I_BINARY_OP(wah_mm_min_epu32)
        WAH_RUN(I32X4_MAX_S_sse41) M128I_BINARY_OP(wah_mm_max_epi32)
        WAH_RUN(I32X4_MAX_U_sse41) M128I_BINARY_OP(wah_mm_max_epu32)
        WAH_RUN(I32X4_EXTMUL_LOW_I16X8_S_sse41) M128I_BINARY_OP(wah_i32x4_extmul_low_i16x8_s_sse41)
        WAH_RUN(I32X4_EXTMUL_HIGH_I16X8_S_sse41) M128I_BINARY_OP(wah_i32x4_extmul_high_i16x8_s_sse41)
        WAH_RUN(I32X4_EXTMUL_LOW_I16X8_U_sse41) M128I_BINARY_OP(wah_i32x4_extmul_low_i16x8_u_sse41)
        WAH_RUN(I32X4_EXTMUL_HIGH_I16X8_U_sse41) M128I_BINARY_OP(wah_i32x4_extmul_high_i16x8_u_sse41)
        WAH_RUN(I64X2_EXTEND_LOW_I32X4_S_sse41) M128I_UNARY_OP(wah_mm_cvtepi32_epi64)
        WAH_RUN(I64X2_EXTEND_HIGH_I32X4_S_sse41) M128I_UNARY_OP(wah_i64x2_extend_high_i32x4_s_sse41)
        WAH_RUN(I64X2_EXTEND_LOW_I32X4_U_sse41) M128I_UNARY_OP(wah_mm_cvtepu32_epi64)
        WAH_RUN(I64X2_EXTEND_HIGH_I32X4_U_sse41) M128I_UNARY_OP(wah_i64x2_extend_high_i32x4_u_sse41)
        WAH_RUN(I64X2_EXTMUL_LOW_I32X4_S_sse41) M128I_BINARY_OP(wah_i64x2_extmul_low_i32x4_s_sse41)
        WAH_RUN(I64X2_EXTMUL_HIGH_I32X4_S_sse41) M128I_BINARY_OP(wah_i64x2_extmul_high_i32x4_s_sse41)
        WAH_RUN(I64X2_EXTMUL_LOW_I32X4_U_sse41) M128I_BINARY_OP(wah_i64x2_extmul_low_i32x4_u_sse41)
        WAH_RUN(I64X2_EXTMUL_HIGH_I32X4_U_sse41) M128I_BINARY_OP(wah_i64x2_extmul_high_i32x4_u_sse41)
        WAH_RUN(F32X4_CEIL_sse41) M128_FP_UNARY_OP(wah_mm_ceil_ps)
        WAH_RUN(F32X4_FLOOR_sse41) M128_FP_UNARY_OP(wah_mm_floor_ps)
        WAH_RUN(F32X4_TRUNC_sse41) { sp[-1]._m128 = wah_canonicalize_f32x4_sse2(wah_mm_round_ps_trunc(sp[-1]._m128)); WAH_NEXT(); }
        WAH_RUN(F32X4_NEAREST_sse41) { sp[-1]._m128 = wah_canonicalize_f32x4_sse2(wah_mm_round_ps_nearest(sp[-1]._m128)); WAH_NEXT(); }
        WAH_RUN(F64X2_CEIL_sse41) M128D_FP_UNARY_OP(wah_mm_ceil_pd)
        WAH_RUN(F64X2_FLOOR_sse41) M128D_FP_UNARY_OP(wah_mm_floor_pd)
        WAH_RUN(F64X2_TRUNC_sse41) { sp[-1]._m128d = wah_canonicalize_f64x2_sse2(wah_mm_round_pd_trunc(sp[-1]._m128d)); WAH_NEXT(); }
        WAH_RUN(F64X2_NEAREST_sse41) { sp[-1]._m128d = wah_canonicalize_f64x2_sse2(wah_mm_round_pd_nearest(sp[-1]._m128d)); WAH_NEXT(); }
        WAH_RUN(I64X2_EQ_sse41) M128I_BINARY_OP(wah_mm_cmpeq_epi64)
        WAH_RUN(I64X2_NE_sse41) M128I_NOT_BINARY_OP(wah_mm_cmpeq_epi64)
        WAH_RUN(I32X4_TRUNC_SAT_F32X4_S_sse41) { sp[-1]._m128i = wah_i32x4_trunc_sat_f32x4_s_sse41(sp[-1]._m128); WAH_NEXT(); }
        WAH_RUN(I32X4_TRUNC_SAT_F32X4_U_sse41) { sp[-1]._m128i = wah_i32x4_trunc_sat_f32x4_u_sse41(sp[-1]._m128); WAH_NEXT(); }
    )

    WAH_IF_SSE42(
        WAH_RUN(I64X2_LT_S_sse42) M128I_BINARY_OP(wah_i64x2_lt_s_sse42)
        WAH_RUN(I64X2_GT_S_sse42) M128I_BINARY_OP(wah_i64x2_gt_s_sse42)
        WAH_RUN(I64X2_LE_S_sse42) M128I_BINARY_OP(wah_i64x2_le_s_sse42)
        WAH_RUN(I64X2_GE_S_sse42) M128I_BINARY_OP(wah_i64x2_ge_s_sse42)
    )

    WAH_IF_AVX2(
        WAH_RUN(I8X16_POPCNT_avx2) M128I_UNARY_OP(wah_i8x16_popcnt_avx2)
    )

    WAH_IF_AVX512(
        WAH_RUN(I32X4_TRUNC_SAT_F32X4_U_avx512f_vl) {
            __m128 v = sp[-1]._m128;
            __m128 clamped = _mm_max_ps(v, _mm_setzero_ps());
            sp[-1]._m128i = _mm_and_si128(wah_mm_cvttps_epu32(clamped), _mm_castps_si128(_mm_cmpord_ps(v, v)));
            WAH_NEXT();
        }
        WAH_RUN(I8X16_POPCNT_avx512bitalg_vl) M128I_UNARY_OP(wah_mm_popcnt_epi8)
        WAH_RUN(I8X16_LT_U_avx512bw_vl) M128I_BINARY_OP(wah_mm_cmplt_epu8_avx512)
        WAH_RUN(I64X2_MUL_avx512dq_vl) M128I_BINARY_OP(wah_mm_mullo_epi64)
        WAH_RUN(F32X4_CONVERT_I32X4_U_avx512f_vl) { sp[-1]._m128 = wah_mm_cvtepu32_ps(sp[-1]._m128i); WAH_NEXT(); }
        WAH_RUN(F64X2_CONVERT_LOW_I32X4_U_avx512f_vl) { sp[-1]._m128d = wah_mm_cvtepu32_pd(sp[-1]._m128i); WAH_NEXT(); }
        WAH_RUN(V128_BITSELECT_avx512f_vl) M128I_TERNARY_OP(wah_mm_ternarylogic_epi32_bitselect(a, b, c))
    )
)

#undef V128_UNARY_OP
#undef V128_BINARY_OP
#undef V128_BINARY_OP_LANE
#undef V128_BINARY_OP_LANE_WRAP
#undef V128_BINARY_OP_LANE_SAT_S
#undef V128_BINARY_OP_LANE_SAT_U
#undef V128_BINARY_OP_LANE_F
#undef V128_CMP_I_LANE
#undef V128_CMP_I_LANE_S
#undef V128_CMP_I_LANE_U
#undef V128_CMP_F_LANE
#undef V128_NEG_OP_LANE_WRAP
#undef V128_SHL_OP_LANE_WRAP
#undef V128_ABS_OP_WRAP

#undef M128I_UNARY_OP
#undef M128I_UNARY_I32_OP
#undef M128I_BINARY_OP
#undef M128I_NOT_BINARY_OP
#undef M128I_TERNARY_OP
#undef M128_UNARY_OP
#undef M128_BINARY_OP
#undef M128_NOT_BINARY_OP
#undef M128_TERNARY_OP
#undef M128_FP_UNARY_OP
#undef M128_FP_BINARY_OP
#undef M128_FP_TERNARY_OP
#undef M128D_UNARY_OP
#undef M128D_BINARY_OP
#undef M128D_NOT_BINARY_OP
#undef M128D_TERNARY_OP
#undef M128D_FP_UNARY_OP
#undef M128D_FP_BINARY_OP
#undef M128D_FP_TERNARY_OP

#undef N128_UNARY_OP
#undef N128_UNARY_I32_OP
#undef N128_BINARY_OP
#undef N128_NOT_BINARY_OP
#undef N128_SHIFT_OP
#undef N128_TERNARY_OP
#undef N128_FP_UNARY_OP_F32
#undef N128_FP_UNARY_OP_F64
#undef N128_FP_BINARY_OP_F32
#undef N128_FP_BINARY_OP_F64
#undef N128_FP_TERNARY_OP_F32
#undef N128_FP_TERNARY_OP_F64

#else // !WAH_FEATURE_SIMD

#define WAH_SIMD_NEVER_RUN(name, ...) WAH_NEVER_RUN(name)
WAH_SIMD_OPCODES(WAH_SIMD_NEVER_RUN)
#undef WAH_SIMD_NEVER_RUN

#endif // WAH_FEATURE_SIMD

//------------------------------------------------------------------------------

#ifdef WAH_USE_MUSTTAIL

static wah_error_t wah_run_single(wah_exec_context_t *ctx, wah_call_frame_t *frame,
                                  const uint8_t *bytecode_ip, const uint8_t *bytecode_base,
                                  wah_value_t *sp, wah_exec_context_t *fctx, wah_error_t err) {
    uint16_t opcode = wah_read_u16_le(bytecode_ip);
    bytecode_ip += sizeof(uint16_t);

    switch (opcode) {
        #define WAH_OPCODE_CASES(opcode, ...) \
            case WAH_OP_##opcode: __attribute__((musttail)) return wah_run_##opcode(ctx, frame, bytecode_ip, bytecode_base, sp, fctx, err);
        #define WAH_EXTRA_OPCODE_CASES(opcode, suffix) WAH_OPCODE_CASES(opcode##_##suffix)
        WAH_OPCODES(WAH_OPCODE_CASES)
        WAH_INTERNAL_OPCODES(WAH_OPCODE_CASES)
        WAH_EXTRA_OPCODES(WAH_EXTRA_OPCODE_CASES)
        #undef WAH_OPCODE_CASES
        #undef WAH_EXTRA_OPCODE_CASES
        #undef WAH_INTERNAL_OPCODE_CASES
    default:
        WAH_UNREACHABLE();
    }
}

static wah_error_t wah_run_interpreter(wah_exec_context_t *ctx) {
    // These are pointers to the current frame's state for faster access.
    wah_call_frame_t *frame = ctx->frame_ptr;
    const uint8_t *bytecode_ip = frame->bytecode_ip;
    const uint8_t *bytecode_base = frame->code->parsed_code.bytecode;
    wah_value_t *sp = ctx->sp;
    wah_exec_context_t *fctx = frame->frame_ctx;   // Module-local context for table/memory access

    return wah_run_single(ctx, frame, bytecode_ip, bytecode_base, sp, fctx, WAH_OK);
}

#else

#ifndef WAH_USE_COMPUTED_GOTO
        default:
            WAH_UNREACHABLE();
        }
    }
#endif

cleanup:
    ctx->sp = sp;
    if (ctx->call_depth > 0) {
        frame->bytecode_ip = bytecode_ip;
    }
    return err;
} // End of wah_run_interpreter

#endif

#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
    #pragma GCC pop_options
#elif defined(_MSC_VER)
    #pragma float_control(pop)
#endif

////////////////////////////////////////////////////////////////////////////////
// Type specification DSL parser ///////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

typedef struct {
    const char *cur;
    wah_module_t *module;
    va_list *args;
    bool allow_placeholders;
} wah_type_spec_parser_t;

typedef struct {
    const char *kw;
    wah_type_t type;
    bool packed;
} wah_type_spec_kw_t;

static const wah_type_spec_kw_t wah_type_spec_type_kws[] = {
    {"i32", WAH_TYPE_I32, false}, {"i64", WAH_TYPE_I64, false},
    {"f32", WAH_TYPE_F32, false}, {"f64", WAH_TYPE_F64, false},
    {"v128", WAH_TYPE_V128, false}, {"i8", WAH_TYPE_PACKED_I8, true},
    {"i16", WAH_TYPE_PACKED_I16, true}, {"funcref", WAH_TYPE_FUNCREF, false},
    {"externref", WAH_TYPE_EXTERNREF, false}, {"exnref", WAH_TYPE_EXNREF, false},
    {"anyref", WAH_TYPE_ANYREF, false}, {"eqref", WAH_TYPE_EQREF, false},
    {"i31ref", WAH_TYPE_I31REF, false}, {"structref", WAH_TYPE_STRUCTREF, false},
    {"arrayref", WAH_TYPE_ARRAYREF, false}, {"nullref", WAH_TYPE_NULLREF, false},
    {"nullfuncref", WAH_TYPE_NULLFUNCREF, false},
    {"nullexternref", WAH_TYPE_NULLEXTERNREF, false},
    {"nullexnref", WAH_TYPE_NULLEXNREF, false}, {NULL, 0, false}
};

static const wah_type_spec_kw_t wah_type_spec_heap_kws[] = {
    {"func", WAH_TYPE_FUNC, false}, {"extern", WAH_TYPE_EXTERN, false},
    {"exn", WAH_TYPE_EXN, false}, {"any", WAH_TYPE_ANY, false},
    {"eq", WAH_TYPE_EQ, false}, {"i31", WAH_TYPE_I31, false},
    {"struct", WAH_TYPE_STRUCT, false}, {"array", WAH_TYPE_ARRAY, false},
    {"nofunc", WAH_TYPE_NOFUNC, false}, {"noextern", WAH_TYPE_NOEXTERN, false},
    {"noexn", WAH_TYPE_NOEXN, false}, {NULL, 0, false}
};

static void wah_type_spec_skip_ws(wah_type_spec_parser_t *p) {
    while (*p->cur == ' ' || *p->cur == '\n' || *p->cur == '\r' || *p->cur == '\t') p->cur++;
}

static bool wah_type_spec_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool wah_type_spec_take_lit(wah_type_spec_parser_t *p, const char *lit) {
    size_t n = strlen(lit);
    wah_type_spec_skip_ws(p);
    if (strncmp(p->cur, lit, n) != 0) return false;
    p->cur += n;
    return true;
}

static bool wah_type_spec_take_kw(wah_type_spec_parser_t *p, const char *kw) {
    size_t n = strlen(kw);
    wah_type_spec_skip_ws(p);
    if (strncmp(p->cur, kw, n) != 0 || wah_type_spec_is_ident_char(p->cur[n])) return false;
    p->cur += n;
    return true;
}

static bool wah_type_spec_fail(wah_type_spec_parser_t *p, const char *msg) {
    WAH_LOG("Type spec parse error: %s", msg);
    (void)p;
    (void)msg;
    return false;
}

static bool wah_type_spec_take_kw_type(wah_type_spec_parser_t *p, const wah_type_spec_kw_t *kws,
                                       bool allow_packed, wah_type_t *out) {
    for (; kws->kw; kws++) {
        if (kws->packed && !allow_packed) continue;
        if (wah_type_spec_take_kw(p, kws->kw)) {
            *out = kws->type;
            return true;
        }
    }
    return false;
}

static bool wah_type_spec_is_known_builtin(wah_type_t t) {
    const wah_type_spec_kw_t *kw;
    for (kw = wah_type_spec_type_kws; kw->kw; kw++) if (kw->type == t) return true;
    for (kw = wah_type_spec_heap_kws; kw->kw; kw++) if (kw->type == t) return true;
    return false;
}

static bool wah_type_spec_check_typeidx(wah_type_spec_parser_t *p, wah_type_t t) {
    if (t >= 0 && WAH_TYIDX(t) >= p->module->type_count) {
        return wah_type_spec_fail(p, "%T references an unknown module type");
    }
    return true;
}

static bool wah_type_spec_parse_placeholder(wah_type_spec_parser_t *p, bool allow_packed, wah_type_t *out) {
    if (!wah_type_spec_take_lit(p, "%T")) return false;
    if (!p->allow_placeholders) return wah_type_spec_fail(p, "%T is not valid in this type spec");
    wah_type_t t = va_arg(*p->args, wah_type_t);
    if (!allow_packed && (t == WAH_TYPE_PACKED_I8 || t == WAH_TYPE_PACKED_I16)) {
        return wah_type_spec_fail(p, "%T packed type is only valid in struct fields and array elements");
    }
    if (t < 0 && !wah_type_spec_is_known_builtin(t)) {
        return wah_type_spec_fail(p, "%T is not a known builtin type");
    }
    if (t <= WAH_TYPE_FUNC && t >= WAH_TYPE_NOEXN && !WAH_TYPE_IS_NULLABLE(t)) {
        return wah_type_spec_fail(p, "%T non-null heap type must be written as ref %T or ref null %T");
    }
    if (!wah_type_spec_check_typeidx(p, t)) return false;
    *out = t;
    return true;
}

static bool wah_type_spec_parse_named_scalar(wah_type_spec_parser_t *p, bool allow_packed, wah_type_t *out) {
    return wah_type_spec_take_kw_type(p, wah_type_spec_type_kws, allow_packed, out);
}

static bool wah_type_spec_parse_ref_type(wah_type_spec_parser_t *p, wah_type_t *out) {
    const char *save = p->cur;
    wah_type_t ht = 0;
    bool nullable;
    if (!wah_type_spec_take_kw(p, "ref")) return false;
    nullable = wah_type_spec_take_kw(p, "null");
    if (wah_type_spec_take_lit(p, "%T")) {
        if (!p->allow_placeholders) return wah_type_spec_fail(p, "%T is not valid in this type spec");
        wah_type_t t = va_arg(*p->args, wah_type_t);
        if (t < 0) {
            if (!wah_type_spec_is_known_builtin(t)) return wah_type_spec_fail(p, "ref %T is not a known builtin type");
            if (!(t <= WAH_TYPE_FUNC && t >= WAH_TYPE_NOEXN) || WAH_TYPE_IS_NULLABLE(t)) {
                return wah_type_spec_fail(p, "ref %T requires a non-null heap type or non-negative type use");
            }
            *out = nullable ? WAH_TYPE_AS_NULLABLE(t) : t;
            return true;
        }
        if (!wah_type_spec_check_typeidx(p, t)) return false;
        *out = WAH_TYPE_FROM_IDX(WAH_TYIDX(t), nullable);
        return true;
    }
    if (wah_type_spec_take_kw_type(p, wah_type_spec_heap_kws, false, &ht)) {
        *out = nullable ? WAH_TYPE_AS_NULLABLE(ht) : ht;
        return true;
    }
    p->cur = save;
    return false;
}

static bool wah_type_spec_parse_type(wah_type_spec_parser_t *p, bool allow_packed, wah_type_t *out) {
    const char *save = p->cur;
    if (wah_type_spec_parse_placeholder(p, allow_packed, out)) return true; else p->cur = save;
    if (wah_type_spec_parse_named_scalar(p, allow_packed, out)) return true; else p->cur = save;
    if (wah_type_spec_parse_ref_type(p, out)) return true; else p->cur = save;
    return false;
}

static wah_error_t wah_type_spec_push_type(wah_type_t **arr, uint32_t *count, wah_type_t type, const wah_alloc_t *alloc) {
    wah_error_t err;
    uint32_t new_count = *count + 1;
    WAH_REALLOC_ARRAY_GOTO(*arr, new_count, cleanup);
    (*arr)[*count] = type;
    *count = new_count;
    return WAH_OK;
cleanup:
    return err;
}

static wah_error_t wah_type_spec_parse_types_until(wah_type_spec_parser_t *p, char end,
                                                   bool allow_packed, wah_type_t **out_types,
                                                   uint32_t *out_count, const wah_alloc_t *alloc) {
    wah_error_t err;
    *out_types = NULL;
    *out_count = 0;
    wah_type_spec_skip_ws(p);
    if (*p->cur == end) return WAH_OK;
    for (;;) {
        wah_type_t t;
        if (!wah_type_spec_parse_type(p, allow_packed, &t)) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
        WAH_CHECK_GOTO(wah_type_spec_push_type(out_types, out_count, t, alloc), cleanup);
        wah_type_spec_skip_ws(p);
        if (*p->cur != ',') return WAH_OK;
        p->cur++;
        wah_type_spec_skip_ws(p);
        if (*p->cur == end) return WAH_OK;
    }
cleanup:
    wah_free(alloc, *out_types);
    *out_types = NULL;
    *out_count = 0;
    return err;
}

static wah_error_t wah_type_spec_parse_func(wah_type_spec_parser_t *p, wah_func_type_t *ft, bool implicit_fn, const wah_alloc_t *alloc) {
    wah_error_t err;
    if (!implicit_fn && !wah_type_spec_take_kw(p, "fn")) return WAH_ERROR_BAD_SPEC;
    if (!wah_type_spec_take_lit(p, "(")) return WAH_ERROR_BAD_SPEC;
    WAH_CHECK_GOTO(wah_type_spec_parse_types_until(p, ')', false, &ft->param_types, &ft->param_count, alloc), cleanup);
    if (!wah_type_spec_take_lit(p, ")")) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
    if (wah_type_spec_take_lit(p, "->")) {
        if (wah_type_spec_take_lit(p, "(")) {
            WAH_CHECK_GOTO(wah_type_spec_parse_types_until(p, ')', false, &ft->result_types, &ft->result_count, alloc), cleanup);
            if (!wah_type_spec_take_lit(p, ")")) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
        } else {
            wah_type_t result_type;
            if (!wah_type_spec_parse_type(p, false, &result_type)) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
            WAH_CHECK_GOTO(wah_type_spec_push_type(&ft->result_types, &ft->result_count, result_type, alloc), cleanup);
        }
    }
    return WAH_OK;
cleanup:
    wah_free(alloc, ft->param_types);
    wah_free(alloc, ft->result_types);
    *ft = (wah_func_type_t){0};
    return err;
}

#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
static wah_error_t wah_type_spec_push_field(wah_type_def_t *td, wah_type_t type, bool is_mutable, const wah_alloc_t *alloc) {
    wah_error_t err;
    uint32_t new_count = td->field_count + 1;
    WAH_REALLOC_ARRAY_GOTO(td->field_types, new_count, cleanup);
    WAH_REALLOC_ARRAY_GOTO(td->field_mutables, new_count, cleanup);
    td->field_types[td->field_count] = type;
    td->field_mutables[td->field_count] = is_mutable;
    td->field_count = new_count;
    return WAH_OK;
cleanup:
    return err;
}

static wah_error_t wah_type_spec_parse_struct(wah_type_spec_parser_t *p, wah_type_def_t *td, const wah_alloc_t *alloc) {
    wah_error_t err;
    if (!wah_type_spec_take_kw(p, "struct")) return WAH_ERROR_BAD_SPEC;
    if (!wah_type_spec_take_lit(p, "{")) return WAH_ERROR_BAD_SPEC;
    wah_type_spec_skip_ws(p);
    while (*p->cur != '}') {
        bool is_mutable = wah_type_spec_take_kw(p, "mut");
        wah_type_t field_type;
        if (!wah_type_spec_parse_type(p, true, &field_type)) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
        WAH_CHECK_GOTO(wah_type_spec_push_field(td, field_type, is_mutable, alloc), cleanup);
        wah_type_spec_skip_ws(p);
        if (*p->cur != ',') break;
        p->cur++;
        wah_type_spec_skip_ws(p);
    }
    if (!wah_type_spec_take_lit(p, "}")) { err = WAH_ERROR_BAD_SPEC; goto cleanup; }
    return WAH_OK;
cleanup:
    wah_free(alloc, td->field_types);
    wah_free(alloc, td->field_mutables);
    td->field_types = NULL;
    td->field_mutables = NULL;
    td->field_count = 0;
    return err;
}

static wah_error_t wah_type_spec_parse_array(wah_type_spec_parser_t *p, wah_type_def_t *td, const wah_alloc_t *alloc) {
    wah_error_t err;
    bool is_mutable;
    wah_type_t elem_type;
    if (!wah_type_spec_take_kw(p, "array")) return WAH_ERROR_BAD_SPEC;
    is_mutable = wah_type_spec_take_kw(p, "mut");
    if (!wah_type_spec_parse_type(p, true, &elem_type)) return WAH_ERROR_BAD_SPEC;
    WAH_CHECK_GOTO(wah_type_spec_push_field(td, elem_type, is_mutable, alloc), cleanup);
    return WAH_OK;
cleanup:
    return err;
}
#endif // WAH_FEATURE_GC

////////////////////////////////////////////////////////////////////////////////
// Public API implementation ///////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

static void wah_cancel_internal(wah_exec_context_t *ctx) {
    wah_timer_set_armed(ctx, false);
    WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 0);
    wah_recompute_poll_flag(ctx);
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_EXCEPTION)
    ctx->pending_exception = NULL;
#endif
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
    if (ctx->gc) {
        wah_gc_state_t *gc = ctx->gc;
        wah_gc_object_t *prev = NULL;
        wah_gc_object_t *obj = gc->all_objects;
        while (obj) {
            wah_gc_object_t *next = wah_gc_next(obj);
            if (obj->repr_id == WAH_TYPE_EXN) {
                if (prev)
                    wah_gc_set_next(prev, next);
                else
                    gc->all_objects = next;
                gc->allocated_bytes -= obj->size_bytes;
                gc->object_count--;
                wah_budget_release(ctx, obj->size_bytes);
                wah_free(&ctx->alloc, obj);
            } else {
                prev = obj;
            }
            obj = next;
        }
    }
#endif
    if (ctx->lifecycle.state == WAH_EXEC_READY) return;
    ctx->sp = ctx->lifecycle.base_sp;
    ctx->call_depth = ctx->lifecycle.base_call_depth;
    ctx->frame_ptr = ctx->lifecycle.base_frame_ptr;
    ctx->exception_handler_depth = ctx->lifecycle.base_handler_depth;
    ctx->lifecycle = (struct wah_exec_lifecycle_s){0};
}

static wah_error_t wah_start_internal(
    wah_exec_context_t *ctx, uint32_t func_idx,
    const wah_value_t *params, uint32_t param_count
) {
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_READY, WAH_ERROR_MISUSE);
    WAH_ENSURE(func_idx < ctx->function_table_count, WAH_ERROR_NOT_FOUND);
    const wah_function_t *fn = &ctx->function_table[func_idx];

    ctx->lifecycle.base_sp = ctx->sp;
    ctx->lifecycle.base_call_depth = ctx->call_depth;
    ctx->lifecycle.base_frame_ptr = ctx->frame_ptr;
    ctx->lifecycle.base_handler_depth = ctx->exception_handler_depth;

    if (fn->is_host) {
        WAH_ENSURE(param_count == fn->nparams, WAH_ERROR_VALIDATION_FAILED);
        uint32_t result_count = (uint32_t)fn->nresults;
        wah_value_t *preflight_top = ctx->sp + param_count + result_count;
        WAH_ENSURE((uint8_t *)preflight_top <= (uint8_t *)ctx->frame_ptr, WAH_ERROR_STACK_OVERFLOW);
        for (uint32_t i = 0; i < param_count; ++i) {
            *ctx->sp++ = params[i];
        }
        if (result_count > 0) {
            memset(ctx->sp, 0, sizeof(wah_value_t) * result_count);
        }
        ctx->sp += result_count;
        ctx->lifecycle.entry_param_count = param_count;
        ctx->lifecycle.entry_result_count = result_count;
        ctx->lifecycle.entry_host_fn = fn;
        ctx->lifecycle.state = WAH_EXEC_SUSPENDED;
        ctx->lifecycle.stop_reason = WAH_OK;
        return WAH_OK;
    }

    uint32_t local_idx = (uint32_t)fn->local_idx;
    const wah_module_t *fn_module = fn->fn_module ? fn->fn_module : ctx->module;
    const wah_func_type_t *func_type = &fn_module->types[fn_module->function_type_indices[local_idx]];
    WAH_ENSURE(param_count == func_type->param_count, WAH_ERROR_VALIDATION_FAILED);
    ctx->lifecycle.entry_result_count = func_type->result_count;

    const wah_code_body_t *start_code = &fn_module->code_bodies[local_idx];
    wah_value_t *preflight_top = ctx->sp + param_count + start_code->max_frame_slots;
    // Preflight: one frame + params + max_frame_slots must fit
    WAH_ENSURE((uint8_t *)preflight_top <= (uint8_t *)(ctx->frame_ptr - 1), WAH_ERROR_STACK_OVERFLOW);

    for (uint32_t i = 0; i < param_count; ++i) {
        *ctx->sp++ = params[i];
    }

    wah_error_t err = wah_push_frame(ctx, fn_module, local_idx,
        ctx->sp - func_type->param_count, func_type->result_count, fn->fn_ctx, preflight_top);
    if (err != WAH_OK) {
        ctx->sp = ctx->lifecycle.base_sp;
        ctx->lifecycle = (struct wah_exec_lifecycle_s){0};
        return err;
    }

    uint32_t num_locals = start_code->local_count;
    if (num_locals > 0) {
        memset(ctx->sp, 0, sizeof(wah_value_t) * num_locals);
        ctx->sp += num_locals;
    }

    ctx->lifecycle.state = WAH_EXEC_SUSPENDED;
    ctx->lifecycle.stop_reason = WAH_OK;
    return WAH_OK;
}

static wah_error_t wah_resume_internal(wah_exec_context_t *ctx) {
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_SUSPENDED, WAH_ERROR_MISUSE);
    ctx->lifecycle.state = WAH_EXEC_RUNNING;
    wah_timer_set_armed(ctx, true);
    wah_error_t err;
    if (ctx->lifecycle.entry_host_fn) {
        const wah_function_t *fn = ctx->lifecycle.entry_host_fn;
        wah_value_t *params = ctx->lifecycle.base_sp;
        wah_value_t *results = ctx->lifecycle.base_sp + ctx->lifecycle.entry_param_count;
        err = wah_call_host_function_internal(ctx, fn, params, ctx->lifecycle.entry_param_count, results);
        if (err == WAH_OK) {
            ctx->sp = results + ctx->lifecycle.entry_result_count;
        }
    } else {
        err = wah_run_interpreter(ctx);
    }
    wah_timer_set_armed(ctx, false);
    if (err == WAH_OK) {
        ctx->lifecycle.state = WAH_EXEC_FINISHED;
    } else if (err > 0) {
        ctx->lifecycle.state = WAH_EXEC_SUSPENDED;
    } else {
        ctx->lifecycle.state = WAH_EXEC_TRAPPED;
    }
    ctx->lifecycle.stop_reason = err;
    return err;
}

static wah_error_t wah_finish_internal(
    wah_exec_context_t *ctx, wah_value_t *results,
    uint32_t max_result_count, uint32_t *actual_result_count
) {
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_FINISHED, WAH_ERROR_MISUSE);
    uint32_t result_count = ctx->lifecycle.entry_result_count;
    uint32_t copy_count = result_count < max_result_count ? result_count : max_result_count;

    if (results) {
        if (result_count == 0 && max_result_count > 0) {
            memset(results, 0, sizeof(wah_value_t));
        } else if (copy_count > 0 && ctx->sp >= ctx->lifecycle.base_sp + result_count) {
            for (uint32_t i = 0; i < copy_count; ++i) {
                results[i] = *(ctx->sp - result_count + i);
            }
        }
    }
    if (actual_result_count) *actual_result_count = result_count;

    ctx->sp = ctx->lifecycle.base_sp;
    ctx->call_depth = ctx->lifecycle.base_call_depth;
    ctx->frame_ptr = ctx->lifecycle.base_frame_ptr;
    ctx->exception_handler_depth = ctx->lifecycle.base_handler_depth;
    ctx->lifecycle = (struct wah_exec_lifecycle_s){0};
    return WAH_OK;
}

wah_error_t wah_start(wah_exec_context_t *ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->module, WAH_ERROR_MISUSE);
    if (!ctx->is_instantiated) {
        WAH_CHECK(wah_instantiate(ctx));
    }
    WAH_ENSURE(func_idx <= UINT32_MAX, WAH_ERROR_NOT_FOUND);
    return wah_start_internal(ctx, (uint32_t)func_idx, params, param_count);
}

wah_error_t wah_resume(wah_exec_context_t *ctx) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    return wah_resume_internal(ctx);
}

wah_error_t wah_finish(wah_exec_context_t *ctx, wah_value_t *results, uint32_t max_results, uint32_t *actual_results) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    return wah_finish_internal(ctx, results, max_results, actual_results);
}

void wah_cancel(wah_exec_context_t *ctx) {
    if (!ctx) return;
    wah_cancel_internal(ctx);
}

bool wah_is_suspended(const wah_exec_context_t *ctx) {
    return ctx && ctx->lifecycle.state == WAH_EXEC_SUSPENDED;
}

wah_exec_state_t wah_exec_state(const wah_exec_context_t *ctx) {
    if (!ctx) return WAH_EXEC_TRAPPED;
    return ctx->lifecycle.state;
}

static wah_error_t wah_call_module_multi(
    wah_exec_context_t *exec_ctx, uint32_t func_idx, const wah_value_t *params, uint32_t param_count,
    wah_value_t *results, uint32_t max_result_count, uint32_t *actual_result_count
) {
    wah_error_t err = wah_start_internal(exec_ctx, func_idx, params, param_count);
    if (err != WAH_OK) return err;

    err = wah_resume_internal(exec_ctx);
    if (err == WAH_OK) {
        return wah_finish_internal(exec_ctx, results, max_result_count, actual_result_count);
    }

    wah_cancel_internal(exec_ctx);
    return err;
}

static wah_error_t wah_call_module(wah_exec_context_t *exec_ctx, uint32_t func_idx,
                                   const wah_value_t *params, uint32_t param_count, wah_value_t *result) {
    WAH_ENSURE(func_idx < exec_ctx->function_table_count, WAH_ERROR_NOT_FOUND);
    const wah_function_t *fn = &exec_ctx->function_table[func_idx];
    uint32_t nresults;
    if (fn->is_host) {
        nresults = (uint32_t)fn->nresults;
    } else {
        const wah_module_t *fmod = fn->fn_module ? fn->fn_module : exec_ctx->module;
        nresults = fmod->types[fmod->function_type_indices[fn->local_idx]].result_count;
    }
    WAH_ENSURE(nresults <= 1, WAH_ERROR_MULTI_RETURN);

    uint32_t actual_results;
    return wah_call_module_multi(exec_ctx, func_idx, params, param_count, result, result ? 1 : 0, &actual_results);
}

wah_error_t wah_call(wah_exec_context_t *exec_ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count, wah_value_t *result) {
    WAH_ENSURE(exec_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(exec_ctx->module, WAH_ERROR_MISUSE);

    if (!exec_ctx->is_instantiated) {
        WAH_CHECK(wah_instantiate(exec_ctx));
    }

    WAH_ENSURE(func_idx <= UINT32_MAX, WAH_ERROR_NOT_FOUND);
    return wah_call_module(exec_ctx, (uint32_t)func_idx, params, param_count, result);
}

wah_error_t wah_call_multi(
    wah_exec_context_t *exec_ctx, uint64_t func_idx, const wah_value_t *params, uint32_t param_count,
    wah_value_t *results, uint32_t max_result_count, uint32_t *actual_result_count
) {
    WAH_ENSURE(exec_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(exec_ctx->module, WAH_ERROR_MISUSE);

    if (!exec_ctx->is_instantiated) {
        WAH_CHECK(wah_instantiate(exec_ctx));
    }

    WAH_ENSURE(func_idx <= UINT32_MAX, WAH_ERROR_NOT_FOUND);
    return wah_call_module_multi(exec_ctx, (uint32_t)func_idx, params, param_count, results, max_result_count, actual_result_count);
}

wah_error_t wah_call_by_name(wah_exec_context_t *exec_ctx, const char *name, const wah_value_t *params, uint32_t param_count, wah_value_t *result) {
    WAH_ENSURE(exec_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(exec_ctx->module, WAH_ERROR_MISUSE);
    wah_export_desc_t desc;
    WAH_CHECK(wah_export_by_name(exec_ctx->module, name, &desc));
    WAH_ENSURE(desc.kind == WAH_KIND_FUNCTION, WAH_ERROR_NOT_FOUND);
    return wah_call(exec_ctx, desc.index, params, param_count, result);
}

// --- Module Cleanup Implementation ---
void wah_free_module(wah_module_t *module) {
    if (!module) {
        return;
    }
    wah_alloc_t alloc_storage = wah_resolve_alloc(&module->alloc);
    const wah_alloc_t *alloc = &alloc_storage;

    if (module->types) {
        for (uint32_t i = 0; i < module->type_count; ++i) {
            wah_free(alloc, module->types[i].param_types);
            wah_free(alloc, module->types[i].result_types);
        }
        wah_free(alloc, module->types);
    }
    if (module->type_defs) {
        for (uint32_t i = 0; i < module->type_count; ++i) {
            wah_free(alloc, module->type_defs[i].field_types);
            wah_free(alloc, module->type_defs[i].field_mutables);
        }
        wah_free(alloc, module->type_defs);
    }
    wah_free(alloc, module->function_type_indices);

    #define WAH_FREE_IMPORTS(arr, count) do { \
        if (arr) { \
            for (uint32_t i_ = 0; i_ < (count); i_++) { \
                wah_free(alloc, (arr)[i_].name.module); \
                wah_free(alloc, (arr)[i_].name.field); \
            } \
            wah_free(alloc, arr); \
        } \
    } while(0)
    WAH_FREE_IMPORTS(module->func_imports, module->import_function_count);
    WAH_FREE_IMPORTS(module->table_imports, module->import_table_count);
    WAH_FREE_IMPORTS(module->memory_imports, module->import_memory_count);
    WAH_FREE_IMPORTS(module->global_imports, module->import_global_count);
    WAH_FREE_IMPORTS(module->tag_imports, module->import_tag_count);
    #undef WAH_FREE_IMPORTS
    wah_free(alloc, module->imports);

    wah_free(alloc, module->tags);

    wah_free_code_bodies(module);

    if (module->globals) {
        for (uint32_t i = 0; i < module->global_count; ++i) {
            wah_free_parsed_code(&module->globals[i].init_expr, alloc);
        }
        wah_free(alloc, module->globals);
    }
    wah_free(alloc, module->memories);
    if (module->tables) {
        for (uint32_t i = 0; i < module->table_count; ++i) {
            wah_free_parsed_code(&module->tables[i].init_expr, alloc);
        }
        wah_free(alloc, module->tables);
    }

    if (module->element_segments) {
        for (uint32_t i = 0; i < module->element_segment_count; ++i) {
            wah_free_element_segment_data(&module->element_segments[i], alloc);
        }
        wah_free(alloc, module->element_segments);
    }

    if (module->data_segments) {
        for (uint32_t i = 0; i < module->data_segment_count; ++i) {
            wah_free(alloc, (void *)module->data_segments[i].data);
            wah_free_parsed_code(&module->data_segments[i].offset_expr, alloc);
        }
        wah_free(alloc, module->data_segments);
    }

    // Free exports. For host function exports the name is owned by functions[], freed below.
    if (module->exports) {
        for (uint32_t i = 0; i < module->export_count; ++i) {
            uint32_t idx = module->exports[i].index;
            bool is_host_export = (module->exports[i].kind == WAH_KIND_FUNCTION &&
                                   module->functions &&
                                   idx < module->local_function_count &&
                                   module->functions[idx].is_host);
            // For memory (kind 2) and global (kind 3) exports from programmatically created modules,
            // the name is owned by the export entry itself, not by another structure.
            // For WASM function exports (kind 0, not host), the name is also owned by the export entry.
            if (!is_host_export) {
                wah_free(alloc, (void*)module->exports[i].name);
            }
        }
        wah_free(alloc, module->exports);
    }

    // Free host function resources stored in the unified functions[] array.
    if (module->functions) {
        for (uint32_t i = module->wasm_function_count; i < module->local_function_count; ++i) {
            wah_function_t *fn = &module->functions[i];
            if (fn->is_host) {
                if (fn->finalize) fn->finalize(fn->userdata);
                wah_free(alloc, fn->name);
                wah_free(alloc, fn->param_types);
                wah_free(alloc, fn->result_types);
            }
        }
        wah_free(alloc, module->functions);
    }

    if (module->repr_infos) {
        for (uint32_t i = 0; i < module->repr_count; ++i) {
            wah_free(alloc, module->repr_infos[i]);
        }
        wah_free(alloc, module->repr_infos);
    }
    wah_free(alloc, module->typeidx_to_repr);
    wah_free(alloc, module->canonical_map);
    if (module->type_cast_sets) {
        for (uint32_t i = 0; i < module->type_count; ++i) {
            wah_free(alloc, module->type_cast_sets[i].bits);
        }
        wah_free(alloc, module->type_cast_sets);
    }
    wah_free(alloc, module->declared_funcs);

    *module = (wah_module_t){0};
}

// --- Programmatically created module API ---

wah_error_t wah_new_module(wah_module_t *mod, const wah_alloc_t *alloc_arg) {
    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(wah_alloc_valid(alloc_arg), WAH_ERROR_MISUSE);

    *mod = (wah_module_t){ .functions_cap = 16, .local_function_count = 0, .exports_cap = 16,
                           .alloc = wah_resolve_alloc(alloc_arg) };
    const wah_alloc_t *alloc = &mod->alloc;
    wah_error_t err = WAH_OK;

    // Allocate initial unified functions[] array (all host functions for a new module)
    WAH_MALLOC_ARRAY_GOTO(mod->functions, mod->functions_cap, cleanup);

    // Allocate initial export array
    WAH_MALLOC_ARRAY_GOTO(mod->exports, mod->exports_cap, cleanup);

    return WAH_OK;

cleanup:
    wah_free_module(mod);
    return err;
}

static bool wah_define_type_matches(const wah_module_t *mod, uint32_t i, const wah_func_type_t *ft, const wah_type_def_t *td) {
    const wah_type_def_t *ei = &mod->type_defs[i];
    if (ei->kind != td->kind) return false;
    if (ei->is_final != td->is_final) return false;
    if (ei->supertype != td->supertype) return false;
    if (td->kind == WAH_COMP_FUNC) {
        const wah_func_type_t *ef = &mod->types[i];
        if (ef->param_count != ft->param_count) return false;
        if (ef->result_count != ft->result_count) return false;
        for (uint32_t j = 0; j < ft->param_count; ++j)
            if (ef->param_types[j] != ft->param_types[j]) return false;
        for (uint32_t j = 0; j < ft->result_count; ++j)
            if (ef->result_types[j] != ft->result_types[j]) return false;
    } else {
        if (ei->field_count != td->field_count) return false;
        for (uint32_t j = 0; j < td->field_count; ++j) {
            if (ei->field_types[j] != td->field_types[j]) return false;
            if (ei->field_mutables[j] != td->field_mutables[j]) return false;
        }
    }
    return true;
}

wah_error_t wah_define_typev(wah_module_t *mod, wah_type_t *out_type, const char *spec, va_list *args) {
    wah_error_t err;
    wah_type_spec_parser_t p;
    wah_func_type_t ft = {0};
    wah_type_def_t td = { .kind = WAH_COMP_FUNC, .is_final = true, .supertype = WAH_NO_SUPERTYPE };
    uint32_t idx;
    bool fresh;
    const wah_alloc_t *alloc;

    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(out_type, WAH_ERROR_MISUSE);
    WAH_ENSURE(spec, WAH_ERROR_MISUSE);
    alloc = &mod->alloc;

    p = (wah_type_spec_parser_t){ .cur = spec, .module = mod, .args = args, .allow_placeholders = true };
    fresh = wah_type_spec_take_kw(&p, "fresh");
    const char *after_fresh = p.cur;
    if (wah_type_spec_take_kw(&p, "fn")) {
        p.cur = after_fresh;
        td.kind = WAH_COMP_FUNC;
        WAH_CHECK_GOTO(wah_type_spec_parse_func(&p, &ft, false, alloc), cleanup);
    } else if (wah_type_spec_take_kw(&p, "struct")) {
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
        p.cur = after_fresh;
        td.kind = WAH_COMP_STRUCT;
        WAH_CHECK_GOTO(wah_type_spec_parse_struct(&p, &td, alloc), cleanup);
#else
        err = WAH_ERROR_DISABLED_FEATURE; goto cleanup;
#endif
    } else if (wah_type_spec_take_kw(&p, "array")) {
#if ((WAH_COMPILED_FEATURES) & WAH_FEATURE_GC)
        p.cur = after_fresh;
        td.kind = WAH_COMP_ARRAY;
        WAH_CHECK_GOTO(wah_type_spec_parse_array(&p, &td, alloc), cleanup);
#else
        err = WAH_ERROR_DISABLED_FEATURE; goto cleanup;
#endif
    } else {
        err = WAH_ERROR_BAD_SPEC;
        goto cleanup;
    }

    wah_type_spec_skip_ws(&p);
    WAH_ENSURE_GOTO(*p.cur == '\0', WAH_ERROR_BAD_SPEC, cleanup);

    if (!fresh) {
        for (uint32_t i = 0; i < mod->type_count; ++i) {
            if (wah_define_type_matches(mod, i, &ft, &td)) {
                wah_free(alloc, ft.param_types);
                wah_free(alloc, ft.result_types);
                wah_free(alloc, td.field_types);
                wah_free(alloc, td.field_mutables);
                *out_type = WAH_TYPE_FROM_IDX(i, false);
                return WAH_OK;
            }
        }
    }

    idx = mod->type_count;
    WAH_CHECK_GOTO(wah_type_section_ensure_capacity(mod, idx + 1), cleanup);
    mod->types[idx] = ft;
    mod->type_defs[idx] = td;
    mod->type_defs[idx].rec_group_start = idx;
    mod->type_defs[idx].rec_group_size = 1;
    mod->type_count++;
    ft = (wah_func_type_t){0};
    td = (wah_type_def_t){0};

    err = wah_module_add_latest_type_metadata(mod);
    if (err != WAH_OK) {
        mod->type_count--;
        wah_free(alloc, mod->types[idx].param_types);
        wah_free(alloc, mod->types[idx].result_types);
        wah_free(alloc, mod->type_defs[idx].field_types);
        wah_free(alloc, mod->type_defs[idx].field_mutables);
        mod->types[idx] = (wah_func_type_t){0};
        mod->type_defs[idx] = (wah_type_def_t){0};
        return err;
    }

    *out_type = WAH_TYPE_FROM_IDX(idx, false);
    return WAH_OK;

cleanup:
    wah_free(alloc, ft.param_types);
    wah_free(alloc, ft.result_types);
    wah_free(alloc, td.field_types);
    wah_free(alloc, td.field_mutables);
    return err == WAH_OK ? WAH_ERROR_BAD_SPEC : err;
}

wah_error_t wah_define_type(wah_module_t *mod, wah_type_t *out_type, const char *spec, ...) {
    wah_error_t err;
    va_list args;
    va_start(args, spec);
    err = wah_define_typev(mod, out_type, spec, &args);
    va_end(args);
    return err;
}

static wah_error_t wah_module_ensure_export(wah_module_t *mod, const char *name) {
    const wah_alloc_t *alloc = &mod->alloc;
    for (uint32_t i = 0; i < mod->export_count; ++i) {
        if (mod->exports[i].name && strcmp(mod->exports[i].name, name) == 0) {
            return WAH_ERROR_VALIDATION_FAILED;
        }
    }
    WAH_ENSURE_CAP(mod->exports, mod->export_count + 1);
    return WAH_OK;
}

static wah_error_t wah_module_register_host_func(
    wah_module_t *mod, const char *name, const wah_func_type_t *ft,
    wah_func_t func, void *userdata, wah_finalize_t finalize
) {
    wah_error_t err;
    const wah_alloc_t *alloc = &mod->alloc;
    char *name_copy = NULL;
    wah_type_t *param_types_copy = NULL;
    wah_type_t *result_types_copy = NULL;

    WAH_CHECK_GOTO(wah_module_ensure_export(mod, name), cleanup);
    WAH_ENSURE_CAP_GOTO(mod->functions, mod->local_function_count + 1, cleanup);

    name_copy = wah_strdup(name, alloc);
    WAH_ENSURE_GOTO(name_copy, WAH_ERROR_OUT_OF_MEMORY, cleanup);

    if (ft->param_count > 0) {
        WAH_MALLOC_ARRAY_GOTO(param_types_copy, ft->param_count, cleanup);
        memcpy(param_types_copy, ft->param_types, ft->param_count * sizeof(wah_type_t));
    }

    if (ft->result_count > 0) {
        WAH_MALLOC_ARRAY_GOTO(result_types_copy, ft->result_count, cleanup);
        memcpy(result_types_copy, ft->result_types, ft->result_count * sizeof(wah_type_t));
    }

    uint32_t new_func_idx = mod->local_function_count;
    mod->functions[new_func_idx] = (wah_function_t){
        .fake_header = (wah_gc_object_t)WAH_FUNCREF_FAKE_HEADER, .is_host = true,
        .global_idx = mod->import_function_count + new_func_idx,
        .name = name_copy, .func = func, .userdata = userdata, .finalize = finalize,
        .nparams = ft->param_count, .param_types = param_types_copy,
        .nresults = ft->result_count, .result_types = result_types_copy,
    };
    mod->local_function_count++;
    mod->exports[mod->export_count++] = (wah_export_t){ .name = name_copy, .name_len = strlen(name_copy),
                                                        .kind = WAH_KIND_FUNCTION, .index = new_func_idx };
    name_copy = NULL;
    param_types_copy = NULL;
    result_types_copy = NULL;
    err = WAH_OK;
cleanup:
    wah_free(alloc, param_types_copy);
    wah_free(alloc, result_types_copy);
    wah_free(alloc, name_copy);
    return err;
}

wah_error_t wah_export_func(wah_module_t *mod, const char *name, const char *types,
                            wah_func_t func, void *userdata, wah_finalize_t finalize) {
    wah_error_t err;
    wah_type_spec_parser_t p;
    wah_func_type_t ft = {0};
    const wah_alloc_t *alloc;

    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(types, WAH_ERROR_MISUSE);
    WAH_ENSURE(func, WAH_ERROR_MISUSE);
    alloc = &mod->alloc;

    p = (wah_type_spec_parser_t){ .cur = types, .module = mod, .allow_placeholders = false };
    bool has_fn = wah_type_spec_take_kw(&p, "fn");
    p.cur = types;
    WAH_CHECK_GOTO(wah_type_spec_parse_func(&p, &ft, !has_fn, alloc), cleanup);
    wah_type_spec_skip_ws(&p);
    WAH_ENSURE_GOTO(*p.cur == '\0', WAH_ERROR_BAD_SPEC, cleanup);

    err = wah_module_register_host_func(mod, name, &ft, func, userdata, finalize);
cleanup:
    wah_free(alloc, ft.param_types);
    wah_free(alloc, ft.result_types);

    return err;
}

wah_error_t wah_export_typed_func(wah_module_t *mod, const char *name, wah_type_t type,
                                  wah_func_t func, void *userdata, wah_finalize_t finalize) {
    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(func, WAH_ERROR_MISUSE);
    WAH_ENSURE(type >= 0 && WAH_TYIDX(type) < mod->type_count, WAH_ERROR_MISUSE);
    WAH_ENSURE(mod->type_defs[WAH_TYIDX(type)].kind == WAH_COMP_FUNC, WAH_ERROR_MISUSE);

    const wah_func_type_t *ft = &mod->types[WAH_TYIDX(type)];
    return wah_module_register_host_func(mod, name, ft, func, userdata, finalize);
}

// Helper to create a preparsed const expression from a simple constant value
static wah_error_t wah_new_const_expr(wah_type_t type, const wah_value_t *value, wah_parsed_code_t *parsed_code, const wah_alloc_t *alloc) {
    uint16_t opcode;
    const void *payload;
    uint32_t payload_size;

    switch (type) {
        case WAH_TYPE_I32:  opcode = WAH_OP_I32_CONST;  payload = &value->i32; payload_size = sizeof(int32_t); break;
        case WAH_TYPE_I64:  opcode = WAH_OP_I64_CONST;  payload = &value->i64; payload_size = sizeof(int64_t); break;
        case WAH_TYPE_F32:  opcode = WAH_OP_F32_CONST;  payload = &value->f32; payload_size = sizeof(float); break;
        case WAH_TYPE_F64:  opcode = WAH_OP_F64_CONST;  payload = &value->f64; payload_size = sizeof(double); break;
        case WAH_TYPE_V128: opcode = WAH_OP_V128_CONST; payload = &value->v128; payload_size = sizeof(wah_v128_t); break;
        default: return WAH_ERROR_VALIDATION_FAILED;
    }

    uint32_t size = sizeof(uint16_t) + payload_size + sizeof(uint16_t);
    WAH_MALLOC_ARRAY(parsed_code->bytecode, size);
    parsed_code->bytecode_size = size;

    uint8_t *out = parsed_code->bytecode;
    wah_write_u16_le(out, opcode);
    out += sizeof(uint16_t);
    memcpy(out, payload, payload_size);
    out += payload_size;
    wah_write_u16_le(out, WAH_OP_END);

    return WAH_OK;
}

// Internal helper for exporting globals
static wah_error_t wah_export_global_internal(wah_module_t *mod, const char *name, wah_type_t type, bool is_mutable, const wah_value_t *init_value) {
    wah_error_t err;
    char *name_copy = NULL;

    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    const wah_alloc_t *alloc = &mod->alloc;

    WAH_CHECK_GOTO(wah_module_ensure_export(mod, name), cleanup);

    WAH_REALLOC_ARRAY_GOTO(mod->globals, mod->global_count + 1, cleanup);

    name_copy = wah_strdup(name, alloc);
    WAH_ENSURE_GOTO(name_copy, WAH_ERROR_OUT_OF_MEMORY, cleanup);

    mod->globals[mod->global_count] = (wah_global_t){ .type = type, .is_mutable = is_mutable };
    WAH_CHECK_GOTO(wah_new_const_expr(type, init_value, &mod->globals[mod->global_count].init_expr, alloc), cleanup);
    mod->exports[mod->export_count++] = (wah_export_t){ .name = name_copy, .name_len = strlen(name_copy),
                                                        .kind = WAH_KIND_GLOBAL, .index = mod->global_count };
    mod->global_count++;
    return WAH_OK;

cleanup:
    wah_free(alloc, name_copy);
    return err;
}

wah_error_t wah_export_memory(wah_module_t *mod, const char *name, uint64_t min_pages, uint64_t max_pages) {
    wah_error_t err;
    const wah_alloc_t *alloc = &mod->alloc;
    char *name_copy = NULL;

    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(min_pages > 0, WAH_ERROR_MISUSE);
    WAH_ENSURE(max_pages >= min_pages, WAH_ERROR_MISUSE);

    WAH_CHECK_GOTO(wah_module_ensure_export(mod, name), cleanup);

    WAH_REALLOC_ARRAY_GOTO(mod->memories, mod->memory_count + 1, cleanup);

    name_copy = wah_strdup(name, alloc);
    WAH_ENSURE_GOTO(name_copy, WAH_ERROR_OUT_OF_MEMORY, cleanup);

    mod->memories[mod->memory_count] = (wah_memory_type_t){ .addr_type = WAH_TYPE_I32, .min_pages = min_pages, .max_pages = max_pages };
    mod->exports[mod->export_count++] = (wah_export_t){ .name = name_copy, .name_len = strlen(name_copy),
                                                        .kind = WAH_KIND_MEMORY, .index = mod->memory_count };
    mod->memory_count++;
    return WAH_OK;

cleanup:
    wah_free(alloc, name_copy);
    return err;
}

wah_error_t wah_export_global_i32(wah_module_t *mod, const char *name, bool is_mutable, int32_t init_value) {
    wah_value_t val = { .i32 = init_value };
    return wah_export_global_internal(mod, name, WAH_TYPE_I32, is_mutable, &val);
}

wah_error_t wah_export_global_i64(wah_module_t *mod, const char *name, bool is_mutable, int64_t init_value) {
    wah_value_t val = { .i64 = init_value };
    return wah_export_global_internal(mod, name, WAH_TYPE_I64, is_mutable, &val);
}

wah_error_t wah_export_global_f32(wah_module_t *mod, const char *name, bool is_mutable, float init_value) {
    wah_value_t val = { .f32 = init_value };
    return wah_export_global_internal(mod, name, WAH_TYPE_F32, is_mutable, &val);
}

wah_error_t wah_export_global_f64(wah_module_t *mod, const char *name, bool is_mutable, double init_value) {
    wah_value_t val = { .f64 = init_value };
    return wah_export_global_internal(mod, name, WAH_TYPE_F64, is_mutable, &val);
}

wah_error_t wah_export_global_v128(wah_module_t *mod, const char *name, bool is_mutable, const wah_v128_t *init_value) {
    wah_value_t val = { .v128 = *init_value };
    return wah_export_global_internal(mod, name, WAH_TYPE_V128, is_mutable, &val);
}

// --- Call Context Implementation ---

#define WAH_PARAM(ty, field) \
    WAH_ASSERT(ctx && "Call context is NULL"); \
    WAH_ASSERT(index < ctx->nparams && "Parameter index out of bounds"); \
    WAH_ASSERT(ctx->param_types[index] == WAH_TYPE_##ty && "Parameter type mismatch"); \
    return ctx->params[index].field
#define WAH_RESULT(ty, field, pre) \
    WAH_ASSERT(ctx && "Call context is NULL"); \
    WAH_ASSERT(index < ctx->nresults && "Result index out of bounds"); \
    WAH_ASSERT(ctx->result_types[index] == WAH_TYPE_##ty && "Result type mismatch"); \
    ctx->results[index].field = pre value

size_t wah_param_count(const wah_call_context_t *ctx) {
    WAH_ASSERT(ctx && "Call context is NULL");
    return ctx->nparams;
}
wah_type_t wah_param_type(const wah_call_context_t *ctx, size_t index) {
    WAH_ASSERT(ctx && "Call context is NULL");
    WAH_ASSERT(index < ctx->nparams && "Parameter index out of bounds");
    return ctx->param_types[index];
}
int32_t wah_param_i32(const wah_call_context_t *ctx, size_t index) { WAH_PARAM(I32, i32); }
int64_t wah_param_i64(const wah_call_context_t *ctx, size_t index) { WAH_PARAM(I64, i64); }
float wah_param_f32(const wah_call_context_t *ctx, size_t index) { WAH_PARAM(F32, f32); }
double wah_param_f64(const wah_call_context_t *ctx, size_t index) { WAH_PARAM(F64, f64); }
wah_v128_t wah_param_v128(const wah_call_context_t *ctx, size_t index) { WAH_PARAM(V128, v128); }
void *wah_param_ref(const wah_call_context_t *ctx, size_t index) {
    WAH_ASSERT(ctx && "Call context is NULL");
    WAH_ASSERT(index < ctx->nparams && "Parameter index out of bounds");
    WAH_ASSERT(WAH_TYPE_IS_REF(ctx->param_types[index]) && "Parameter type mismatch");
    return ctx->params[index].ref;
}

size_t wah_result_count(const wah_call_context_t *ctx) {
    WAH_ASSERT(ctx && "Call context is NULL");
    return ctx->nresults;
}
wah_type_t wah_result_type(const wah_call_context_t *ctx, size_t index) {
    WAH_ASSERT(ctx && "Call context is NULL");
    WAH_ASSERT(index < ctx->nresults && "Result index out of bounds");
    return ctx->result_types[index];
}
void wah_result_i32(wah_call_context_t *ctx, size_t index, int32_t value) { WAH_RESULT(I32, i32, ); }
void wah_result_i64(wah_call_context_t *ctx, size_t index, int64_t value) { WAH_RESULT(I64, i64, ); }
void wah_result_f32(wah_call_context_t *ctx, size_t index, float value) { WAH_RESULT(F32, f32, ); }
void wah_result_f64(wah_call_context_t *ctx, size_t index, double value) { WAH_RESULT(F64, f64, ); }
void wah_result_v128(wah_call_context_t *ctx, size_t index, const wah_v128_t *value) {
    WAH_ASSERT(value && "Value is NULL");
    WAH_RESULT(V128, v128, *);
}
void wah_result_ref(wah_call_context_t *ctx, size_t index, void *value) {
    WAH_ASSERT(ctx && "Call context is NULL");
    WAH_ASSERT(index < ctx->nresults && "Result index out of bounds");
    WAH_ASSERT(WAH_TYPE_IS_REF(ctx->result_types[index]) && "Result type mismatch");
    WAH_ASSERT((value != NULL || WAH_TYPE_IS_NULLABLE(ctx->result_types[index])) &&
               "Non-nullable reference result cannot be set to NULL");
    ctx->results[index].ref = value;
}

#undef WAH_PARAM
#undef WAH_RESULT

void wah_trap(wah_call_context_t *ctx, wah_error_t reason) {
    WAH_ASSERT(ctx && "Call context is NULL");
    WAH_ASSERT(reason < 0 && "Cannot trap with a non-error code such as WAH_OK");
    WAH_ASSERT(ctx->trap_reason == WAH_OK && "Call context already has a trap reason set");
    if (ctx->trap_reason == WAH_OK) ctx->trap_reason = reason;
}

// --- Linkage Implementation ---

wah_error_t wah_link_module(wah_exec_context_t *ctx, const char *name, const wah_module_t *mod) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    const wah_alloc_t *alloc = &ctx->alloc;
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(mod, WAH_ERROR_MISUSE);
    WAH_ENSURE(!ctx->is_instantiated, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_READY, WAH_ERROR_MISUSE);

    // Check for duplicate module name
    for (uint32_t i = 0; i < ctx->linked_module_count; ++i) {
        if (strcmp(ctx->linked_modules[i].name, name) == 0) {
            return WAH_ERROR_VALIDATION_FAILED;  // Duplicate module name
        }
    }

    WAH_ENSURE((mod->required_features & ~ctx->enabled_features) == 0, WAH_ERROR_DISABLED_FEATURE);

    WAH_ENSURE_CAP(ctx->linked_modules, ctx->linked_module_count + 1);

    char *name_copy = wah_strdup(name, alloc);
    WAH_ENSURE(name_copy, WAH_ERROR_OUT_OF_MEMORY);

    ctx->linked_modules[ctx->linked_module_count++] =
        (wah_linked_module_t){ .name = name_copy, .module = mod, .ctx = NULL, .owns_ctx = false };
    return WAH_OK;
}

wah_error_t wah_link_context(wah_exec_context_t *ctx, const char *name, wah_exec_context_t *linked_ctx) {
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    const wah_alloc_t *alloc = &ctx->alloc;
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(linked_ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(!ctx->is_instantiated, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_READY, WAH_ERROR_MISUSE);
    WAH_ENSURE(linked_ctx->is_instantiated, WAH_ERROR_MISUSE);

    for (uint32_t i = 0; i < ctx->linked_module_count; ++i) {
        if (strcmp(ctx->linked_modules[i].name, name) == 0) {
            return WAH_ERROR_VALIDATION_FAILED;
        }
    }

    WAH_ENSURE((linked_ctx->module->required_features & ~ctx->enabled_features) == 0, WAH_ERROR_DISABLED_FEATURE);

    WAH_ENSURE_CAP(ctx->linked_modules, ctx->linked_module_count + 1);

    char *name_copy = wah_strdup(name, alloc);
    WAH_ENSURE(name_copy, WAH_ERROR_OUT_OF_MEMORY);

    ctx->linked_modules[ctx->linked_module_count++] =
        (wah_linked_module_t){ .name = name_copy, .module = linked_ctx->module, .ctx = linked_ctx, .owns_ctx = false };
    return WAH_OK;
}

// --- Const Expression Evaluator ---
// Evaluates a preparsed const expression via the main interpreter.
static wah_error_t wah_eval_const_expr(wah_exec_context_t *ctx, const uint8_t *bytecode, uint32_t bytecode_size, wah_value_t *result) {
    wah_code_body_t dummy_code = { .parsed_code = { .bytecode = (uint8_t *)bytecode, .bytecode_size = bytecode_size } };
    wah_value_t local_stack[WAH_MAX_TYPE_STACK_SIZE];
    wah_call_frame_t local_frame = { .code = &dummy_code, .bytecode_ip = bytecode, .locals = local_stack,
                                     .result_count = 1, .frame_globals = ctx->globals, .module = ctx->module };
    wah_exec_context_t cctx = { .module = ctx->module, .globals = ctx->globals, .global_count = ctx->global_count,
                                .value_stack = local_stack, .sp = local_stack, .frame_ptr = &local_frame,
                                .call_depth = 1, .gc = ctx->gc, .max_memory_bytes = ctx->max_memory_bytes,
                                .memory_bytes_committed = ctx->memory_bytes_committed, .alloc = ctx->alloc };
    local_frame.frame_ctx = &cctx;

    wah_error_t err = wah_run_interpreter(&cctx);
    ctx->memory_bytes_committed = cctx.memory_bytes_committed;
    if (err != WAH_OK) return err;
    WAH_ASSERT(cctx.sp == local_stack + 1 && "Const expression should leave exactly one value on the stack");
    *result = local_stack[0];
    return WAH_OK;
}

wah_error_t wah_instantiate(wah_exec_context_t *ctx) {
    wah_error_t err = WAH_OK;
    WAH_ENSURE(ctx, WAH_ERROR_MISUSE);
    WAH_ENSURE(!ctx->is_instantiated, WAH_ERROR_MISUSE);
    WAH_ENSURE(ctx->lifecycle.state == WAH_EXEC_READY, WAH_ERROR_MISUSE);
    const wah_alloc_t *alloc = &ctx->alloc;
    WAH_POLL_FLAG_STORE(ctx->interrupt_flag, 0);
    wah_recompute_poll_flag(ctx);

    WAH_ENSURE((ctx->module->required_features & ~ctx->enabled_features) == 0, WAH_ERROR_DISABLED_FEATURE);

    const wah_module_t *module = ctx->module;
    uint32_t import_count = module->import_function_count;
    uint32_t ig_count = module->import_global_count;

    // Allocate and initialize globals for linked modules (must happen before local globals
    // and global import resolution, since imports may reference linked module globals)
    uint32_t total_globals = wah_global_index_limit(module);
    for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
        uint32_t gc = ctx->linked_modules[j].module->global_count;
        WAH_ENSURE_GOTO(total_globals <= UINT32_MAX - gc, WAH_ERROR_TOO_LARGE, cleanup);
        total_globals += gc;
    }

    if (total_globals > wah_global_index_limit(module)) {
        wah_value_t *new_globals = NULL;
        WAH_MALLOC_ARRAY_GOTO(new_globals, total_globals, cleanup);

        uint32_t local_global_limit = wah_global_index_limit(module);
        if (local_global_limit > 0) {
            memcpy(new_globals, ctx->globals, local_global_limit * sizeof(wah_value_t));
        }

        wah_value_t *saved_globals = ctx->globals;
        uint32_t saved_global_count = ctx->global_count;
        uint32_t offset = local_global_limit;
        for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
            const wah_module_t *linked = ctx->linked_modules[j].module;
            wah_exec_context_t *lctx = ctx->linked_modules[j].ctx;
            if (lctx && linked->global_count > 0) {
                uint32_t lg_offset = wah_global_index_limit(linked) - linked->global_count;
                memcpy(new_globals + offset, lctx->globals + lg_offset,
                        linked->global_count * sizeof(wah_value_t));
            } else {
                ctx->globals = new_globals + offset;
                ctx->global_count = linked->global_count;
                const wah_module_t *saved_module = ctx->module;
                ctx->module = linked;
                for (uint32_t k = 0; k < linked->global_count; k++) {
                    err = wah_eval_const_expr(ctx,
                                              linked->globals[k].init_expr.bytecode,
                                              linked->globals[k].init_expr.bytecode_size,
                                              &new_globals[offset + k]);
                    if (err != WAH_OK) {
                        ctx->module = saved_module;
                        ctx->globals = saved_globals;
                        ctx->global_count = saved_global_count;
                        wah_free(alloc, new_globals);
                        goto cleanup;
                    }
                }
                ctx->module = saved_module;
            }
            offset += linked->global_count;
        }
        ctx->globals = saved_globals;
        ctx->global_count = saved_global_count;

        wah_free(alloc, ctx->globals);
        ctx->globals = new_globals;
    }

    // Resolve each function import from linked modules
    for (uint32_t i = 0; i < import_count; i++) {
        wah_func_import_t *fi = &module->func_imports[i];

        const wah_module_t *linked = NULL;
        wah_exec_context_t *fi_linked_ctx = NULL;
        WAH_ENSURE_GOTO(wah_find_linked_module(ctx, &fi->name, &linked, &fi_linked_ctx, NULL), WAH_ERROR_LINK_FAILED, cleanup);

        const wah_export_t *exp = wah_find_export(linked, 0, &fi->name);
        WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);

        // exp->index is the global function index in the linked module.
        // For host-only modules (import_function_count=0), global == local (functions[] index).
        uint32_t linked_import_count = linked->import_function_count;
        WAH_ENSURE_GOTO(exp->index >= linked_import_count, WAH_ERROR_LINK_FAILED, cleanup);
        uint32_t linked_local_idx = exp->index - linked_import_count;
        WAH_ENSURE_GOTO(linked_local_idx < linked->local_function_count, WAH_ERROR_LINK_FAILED, cleanup);

        const wah_function_t *src = &linked->functions[linked_local_idx];

        // Verify function type compatibility
        WAH_ENSURE_GOTO(fi->type_index < module->type_count, WAH_ERROR_LINK_FAILED, cleanup);
        const wah_func_type_t *import_type = &module->types[fi->type_index];
        if (src->is_host) {
            WAH_ENSURE_GOTO(import_type->param_count == src->nparams, WAH_ERROR_LINK_FAILED, cleanup);
            WAH_ENSURE_GOTO(import_type->result_count == src->nresults, WAH_ERROR_LINK_FAILED, cleanup);
            for (uint32_t p = 0; p < import_type->param_count; p++) {
                WAH_ENSURE_GOTO(wah_type_is_subtype(import_type->param_types[p], src->param_types[p], module),
                                WAH_ERROR_LINK_FAILED, cleanup);
            }
            for (uint32_t r = 0; r < import_type->result_count; r++) {
                WAH_ENSURE_GOTO(wah_type_is_subtype(src->result_types[r], import_type->result_types[r], module),
                                WAH_ERROR_LINK_FAILED, cleanup);
            }
        } else {
            uint32_t src_local_fn_idx = linked_local_idx;
            if (src_local_fn_idx < linked->wasm_function_count) {
                uint32_t linked_type_idx = linked->function_type_indices[src_local_fn_idx];
                WAH_ENSURE_GOTO(linked_type_idx < linked->type_count, WAH_ERROR_LINK_FAILED, cleanup);
                WAH_ENSURE_GOTO(
                    wah_cross_module_subtype(linked, WAH_TYPE_FROM_IDX(linked_type_idx, 0),
                                             module, WAH_TYPE_FROM_IDX(fi->type_index, 0)),
                    WAH_ERROR_LINK_FAILED, cleanup);
            }
        }

        ctx->function_table[i] = *src;  // shallow copy (pointers shared with linked module)
        ctx->function_table[i].global_idx = exp->index;  // Set global index from export
        if (!src->is_host) {
            ctx->function_table[i].fn_module = linked;
            ctx->function_table[i].local_idx = linked_local_idx;
            // Cache the providing context so funcrefs into this slot can be dispatched
            // from a third-party context that did not directly link to `linked`.
            ctx->function_table[i].fn_ctx = fi_linked_ctx;
        }
    }

    // Resolve global imports from linked modules
    for (uint32_t i = 0; i < module->import_global_count; i++) {
        wah_global_import_t *gi = &module->global_imports[i];

        const wah_module_t *linked = NULL;
        wah_exec_context_t *gi_linked_ctx = NULL;
        uint32_t gi_linked_idx = 0;
        WAH_ENSURE_GOTO(wah_find_linked_module(ctx, &gi->name, &linked, &gi_linked_ctx, &gi_linked_idx), WAH_ERROR_LINK_FAILED, cleanup);

        const wah_export_t *exp = wah_find_export(linked, 3, &gi->name);
        WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);

        // Find the linked module's globals offset in ctx->globals
        uint32_t linked_globals_offset = wah_global_index_limit(module);
        for (uint32_t j = 0; j < gi_linked_idx; j++) {
            linked_globals_offset += ctx->linked_modules[j].module->global_count;
        }

        uint32_t linked_global_idx = exp->index;
        WAH_ENSURE_GOTO(linked_global_idx < linked->import_global_count + linked->global_count, WAH_ERROR_LINK_FAILED, cleanup);
        WAH_ENSURE_GOTO(linked_global_idx >= linked->import_global_count, WAH_ERROR_LINK_FAILED, cleanup);
        uint32_t linked_local_global_idx = linked_global_idx - linked->import_global_count;

        // Verify global type compatibility: (mut1? vt1) <: (mut2? vt2) iff
        //   vt1 <: vt2 && (both immut || (both mut && vt2 <: vt1))
        const wah_global_t *exported_global = &linked->globals[linked_local_global_idx];
        wah_type_t vt1 = exported_global->type, vt2 = gi->type;
        WAH_ENSURE_GOTO(wah_cross_module_subtype(linked, vt1, module, vt2) && exported_global->is_mutable == gi->is_mutable
                     && (!gi->is_mutable || wah_cross_module_subtype(module, vt2, linked, vt1)), WAH_ERROR_LINK_FAILED, cleanup);

        if (gi->is_mutable) {
            if (gi_linked_ctx) {
                ctx->globals[i].ref = &gi_linked_ctx->globals[linked_global_idx];
            } else {
                ctx->globals[i].ref = &ctx->globals[linked_globals_offset + linked_local_global_idx];
            }
        } else {
            ctx->globals[i] = ctx->globals[linked_globals_offset + linked_local_global_idx];
        }
    }

    // Create lightweight internal contexts for wah_link_module-linked modules
    // that have tags, so they get their own tag instance space.
    // These contexts share memories/tables/globals with the primary context but
    // have independent tag_instances for correct cross-module tag identity.
    uint32_t g_offset = wah_global_index_limit(module);
    for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
        const wah_module_t *lmod = ctx->linked_modules[j].module;
        if (ctx->linked_modules[j].ctx == NULL) {
            uint32_t ltotal_tags = lmod->import_tag_count + lmod->tag_count;
            if (ltotal_tags > 0) {
                wah_exec_context_t *ictx = NULL;
                WAH_CHECK_GOTO(wah_malloc(alloc, 1, sizeof(wah_exec_context_t), (void **)&ictx), cleanup);
                *ictx = (wah_exec_context_t){
                    .alloc = ctx->alloc, .module = lmod, .memories = ctx->memories, .memory_count = ctx->memory_count,
                    .tables = ctx->tables, .table_count = ctx->table_count,
                    .globals = g_offset ? ctx->globals + g_offset : ctx->globals, .global_count = lmod->global_count,
                    .function_table = ctx->function_table, .function_table_count = ctx->function_table_count,
                    .gc = ctx->gc, .tag_instance_count = 0,
                };
                ctx->linked_modules[j].ctx = ictx;
                ctx->linked_modules[j].owns_ctx = true;
                WAH_MALLOC_ARRAY_GOTO(ictx->tag_instances, ltotal_tags, cleanup);
                for (uint32_t t = 0; t < lmod->import_tag_count; t++) {
                    ictx->tag_instances[ictx->tag_instance_count++] =
                        (wah_tag_instance_t){ .type_index = lmod->tag_imports[t].type_index };
                }
                for (uint32_t t = 0; t < lmod->tag_count; t++) {
                    uint32_t slot = ictx->tag_instance_count++;
                    ictx->tag_instances[slot] =
                        (wah_tag_instance_t){ .type_index = lmod->tags[t].type_index, .identity = &ictx->tag_instances[slot] };
                }
                ictx->is_instantiated = true;
            }
        }
        g_offset += lmod->global_count;
    }

    // Resolve tag imports from linked modules
    for (uint32_t i = 0; i < module->import_tag_count; i++) {
        wah_tag_import_t *tgi = &module->tag_imports[i];

        const wah_module_t *linked = NULL;
        wah_exec_context_t *linked_ctx = NULL;
        WAH_ENSURE_GOTO(wah_find_linked_module(ctx, &tgi->name, &linked, &linked_ctx, NULL), WAH_ERROR_LINK_FAILED, cleanup);

        const wah_export_t *exp = wah_find_export(linked, 4, &tgi->name);
        WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);

        uint32_t linked_tag_idx = exp->index;
        WAH_ENSURE_GOTO(linked_tag_idx < linked->import_tag_count + linked->tag_count, WAH_ERROR_LINK_FAILED, cleanup);

        // Verify type compatibility (tags require exact type match)
        uint32_t linked_type_idx = linked_tag_idx < linked->import_tag_count
            ? linked->tag_imports[linked_tag_idx].type_index
            : linked->tags[linked_tag_idx - linked->import_tag_count].type_index;
        WAH_ENSURE_GOTO(
            wah_cross_module_type_ref_eq(linked, WAH_TYPE_FROM_IDX(linked_type_idx, 0),
                                         module, WAH_TYPE_FROM_IDX(tgi->type_index, 0)),
            WAH_ERROR_LINK_FAILED, cleanup);

        WAH_ENSURE_GOTO(linked_ctx != NULL, WAH_ERROR_LINK_FAILED, cleanup);
        WAH_ENSURE_GOTO(linked_tag_idx < linked_ctx->tag_instance_count, WAH_ERROR_LINK_FAILED, cleanup);
        ctx->tag_instances[i] = linked_ctx->tag_instances[linked_tag_idx];
        ctx->tag_instances[i].type_index = tgi->type_index;
    }

    if (!ctx->gc) {
        WAH_CHECK_GOTO(wah_gc_start(ctx), cleanup);
    }

    // Fix up wah_exec_context_t::gc for linked modules.
    for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
        if (ctx->linked_modules[j].owns_ctx && ctx->linked_modules[j].ctx) {
            ctx->linked_modules[j].ctx->gc = ctx->gc;
        }
    }

    // Initialize primary module's local globals (at offset import_global_count)
    // Must happen after global import resolution so global.get in init exprs can see imported values
    for (uint32_t i = 0; i < module->global_count; ++i) {
        uint32_t slot = ig_count + i;
        WAH_CHECK_GOTO(wah_eval_const_expr(ctx,
                                           module->globals[i].init_expr.bytecode,
                                           module->globals[i].init_expr.bytecode_size,
                                           &ctx->globals[slot]), cleanup);
        if (ctx->globals[slot].ref == wah_func_to_ref(wah_funcref_sentinel)) {
            uint32_t fidx = ctx->globals[slot]._prefuncref.func_idx;
            WAH_ENSURE_GOTO(fidx < ctx->function_table_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            wah_function_t *fn = &ctx->function_table[fidx];
            if (!fn->is_host && fn->fn_ctx == NULL && fn->fn_module == ctx->module) fn->fn_ctx = ctx;
            ctx->globals[slot].ref = wah_func_to_ref(fn);
        }
    }

    // Resolve table imports from linked modules
    for (uint32_t i = 0; i < module->import_table_count; i++) {
        wah_table_import_t *ti = &module->table_imports[i];

        const wah_module_t *linked = NULL;
        wah_exec_context_t *linked_ctx = NULL;
        WAH_ENSURE_GOTO(wah_find_linked_module(ctx, &ti->name, &linked, &linked_ctx, NULL), WAH_ERROR_LINK_FAILED, cleanup);

        const wah_export_t *exp = wah_find_export(linked, 1, &ti->name);
        WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);

        uint32_t linked_table_idx = exp->index;
        WAH_ENSURE_GOTO(linked_table_idx < wah_table_index_limit(linked), WAH_ERROR_LINK_FAILED, cleanup);
        uint32_t local_table_idx;
        wah_table_type_t *exp_tt = NULL;

        const wah_table_type_t *linked_tt = wah_table_type(linked, linked_table_idx);
        WAH_ENSURE_GOTO(wah_cross_module_type_ref_eq(linked, linked_tt->elem_type,
                                                        module, ti->type.elem_type), WAH_ERROR_LINK_FAILED, cleanup);
        WAH_ENSURE_GOTO(linked_tt->addr_type == ti->type.addr_type, WAH_ERROR_LINK_FAILED, cleanup);
        if (ti->type.max_elements != UINT64_MAX) {
            WAH_ENSURE_GOTO(linked_tt->max_elements != UINT64_MAX, WAH_ERROR_LINK_FAILED, cleanup);
            WAH_ENSURE_GOTO(linked_tt->max_elements <= ti->type.max_elements, WAH_ERROR_LINK_FAILED, cleanup);
        }
        if (linked_table_idx >= linked->import_table_count) {
            local_table_idx = linked_table_idx - linked->import_table_count;
            exp_tt = &linked->tables[local_table_idx];
        }

        if (linked_ctx && linked_table_idx < linked_ctx->table_count) {
            if (linked_table_idx >= linked->import_table_count) {
                WAH_ENSURE_GOTO(linked_ctx->tables[linked_table_idx].size >= ti->type.min_elements, WAH_ERROR_LINK_FAILED, cleanup);
            }
            uint64_t imp_bytes = 0;
            WAH_CHECK_GOTO(wah_table_byte_size(linked_ctx->tables[linked_table_idx].size, &imp_bytes), cleanup);
            WAH_ENSURE_GOTO(wah_budget_check(ctx, imp_bytes), WAH_ERROR_TOO_LARGE, cleanup);
            ctx->tables[i].entries = linked_ctx->tables[linked_table_idx].entries;
            ctx->tables[i].size = linked_ctx->tables[linked_table_idx].size;
            ctx->tables[i].max_size = linked_ctx->tables[linked_table_idx].max_size;
            ctx->tables[i].is_imported = true;
            ctx->tables[i].import_ctx = linked_ctx;
            ctx->tables[i].import_idx = linked_table_idx;
            wah_budget_charge(ctx, imp_bytes);
        } else if (linked_table_idx >= linked->import_table_count) {
            WAH_ENSURE_GOTO(exp_tt->min_elements >= ti->type.min_elements, WAH_ERROR_LINK_FAILED, cleanup);
            uint64_t min_elements = exp_tt->min_elements;
            uint64_t table_bytes = 0;
            WAH_CHECK_GOTO(wah_table_byte_size(min_elements, &table_bytes), cleanup);
            WAH_ENSURE_GOTO(wah_budget_check(ctx, table_bytes), WAH_ERROR_TOO_LARGE, cleanup);
            ctx->tables[i].is_imported = false;
            ctx->tables[i].size = min_elements;
            ctx->tables[i].max_size = exp_tt->max_elements;
            WAH_MALLOC_ARRAY_GOTO(ctx->tables[i].entries, min_elements > 0 ? min_elements : 1, cleanup);
            memset(ctx->tables[i].entries, 0, (size_t)table_bytes);
            wah_budget_charge(ctx, table_bytes);
        }
    }

    // Resolve memory imports from linked modules
    for (uint32_t i = 0; i < module->import_memory_count; i++) {
        wah_memory_import_t *mi = &module->memory_imports[i];

        const wah_module_t *linked = NULL;
        wah_exec_context_t *linked_ctx = NULL;
        WAH_ENSURE_GOTO(wah_find_linked_module(ctx, &mi->name, &linked, &linked_ctx, NULL), WAH_ERROR_LINK_FAILED, cleanup);

        const wah_export_t *exp = wah_find_export(linked, 2, &mi->name);
        WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);

        uint32_t linked_mem_idx = exp->index;
        WAH_ENSURE_GOTO(linked_mem_idx < wah_memory_index_limit(linked), WAH_ERROR_LINK_FAILED, cleanup);
        uint32_t local_mem_idx = 0;
        wah_memory_type_t *exp_mt = NULL;

        const wah_memory_type_t *linked_mt = wah_memory_type(linked, linked_mem_idx);
        WAH_ENSURE_GOTO(linked_mt->addr_type == mi->type.addr_type, WAH_ERROR_LINK_FAILED, cleanup);
        if (mi->type.max_pages != UINT64_MAX) {
            WAH_ENSURE_GOTO(linked_mt->max_pages != UINT64_MAX, WAH_ERROR_LINK_FAILED, cleanup);
            WAH_ENSURE_GOTO(linked_mt->max_pages <= mi->type.max_pages, WAH_ERROR_LINK_FAILED, cleanup);
        }
        if (linked_mem_idx >= linked->import_memory_count) {
            local_mem_idx = linked_mem_idx - linked->import_memory_count;
            exp_mt = &linked->memories[local_mem_idx];
        }

        if (linked_ctx && linked_mem_idx < linked_ctx->memory_count) {
            if (linked_mem_idx >= linked->import_memory_count) {
                uint64_t cur_pages = linked_ctx->memories[linked_mem_idx].size / WAH_WASM_PAGE_SIZE;
                WAH_ENSURE_GOTO(cur_pages >= mi->type.min_pages, WAH_ERROR_LINK_FAILED, cleanup);
            }
            uint64_t imp_bytes = linked_ctx->memories[linked_mem_idx].size;
            WAH_ENSURE_GOTO(wah_budget_check(ctx, imp_bytes), WAH_ERROR_TOO_LARGE, cleanup);
            ctx->memories[i].data = linked_ctx->memories[linked_mem_idx].data;
            ctx->memories[i].size = linked_ctx->memories[linked_mem_idx].size;
            ctx->memories[i].max_pages = linked_ctx->memories[linked_mem_idx].max_pages;
            ctx->memories[i].is_imported = true;
            ctx->memories[i].import_ctx = linked_ctx;
            ctx->memories[i].import_idx = linked_mem_idx;
            wah_budget_charge(ctx, imp_bytes);
        } else if (linked_mem_idx >= linked->import_memory_count) {
            WAH_ENSURE_GOTO(exp_mt->min_pages >= mi->type.min_pages, WAH_ERROR_LINK_FAILED, cleanup);
            ctx->memories[i].max_pages = exp_mt->max_pages;
            uint64_t min_pages = exp_mt->min_pages;
            WAH_ENSURE_GOTO(min_pages <= SIZE_MAX / WAH_WASM_PAGE_SIZE, WAH_ERROR_TOO_LARGE, cleanup);
            uint64_t byte_size = min_pages * (uint64_t)WAH_WASM_PAGE_SIZE;
            WAH_ENSURE_GOTO(wah_budget_check(ctx, byte_size), WAH_ERROR_TOO_LARGE, cleanup);
            ctx->memories[i].is_imported = false;
            ctx->memories[i].size = byte_size;
            if (byte_size > 0) {
                WAH_MALLOC_ARRAY_GOTO(ctx->memories[i].data, byte_size, cleanup);
                memset(ctx->memories[i].data, 0, byte_size);
            }
            wah_budget_charge(ctx, byte_size);
        }

        // Update fast-path aliases if memory 0 was just resolved
        if (i == 0) {
            ctx->memory_base = ctx->memories[0].data;
            ctx->memory_size = ctx->memories[0].size;
        }
    }

    // module->functions[] is fully initialized at parse time, including local_idx,
    // global_idx, and fn_module for both the primary and any linked modules. No
    // per-instance fixup of those fields is needed here. (fn_ctx is per-instance and
    // resolved lazily; see wah_bind_frame_module() and the funcref-creation sites.)

    // Convert funcref globals for linked modules from prefuncref to real pointers.
    // Primary module globals were converted above (after their own eval loop).
    uint32_t lg_offset = wah_global_index_limit(module);
    for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
        const wah_module_t *linked = ctx->linked_modules[j].module;
        for (uint32_t k = 0; k < linked->global_count; k++) {
            if (ctx->globals[lg_offset + k].ref == wah_func_to_ref(wah_funcref_sentinel)) {
                uint32_t fidx = ctx->globals[lg_offset + k]._prefuncref.func_idx;
                uint32_t linked_import_count = linked->import_function_count;
                if (fidx >= linked_import_count) {
                    uint32_t local_k = fidx - linked_import_count;
                    WAH_ENSURE_GOTO(local_k < linked->wasm_function_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
                    ctx->globals[lg_offset + k].ref = wah_func_to_ref(&linked->functions[local_k]);
                } else {
                    // ref.func to linked module's own import: resolve from another linked module
                    wah_func_import_t *fi = &linked->func_imports[fidx];
                    const wah_module_t *provider = NULL;
                    for (uint32_t m = 0; m < ctx->linked_module_count; m++) {
                        const char *lname = ctx->linked_modules[m].name;
                        if (strncmp(lname, fi->name.module, fi->name.module_len) == 0 &&
                            lname[fi->name.module_len] == '\0') {
                            provider = ctx->linked_modules[m].module;
                            break;
                        }
                    }
                    WAH_ENSURE_GOTO(provider != NULL, WAH_ERROR_LINK_FAILED, cleanup);
                    const wah_export_t *exp = NULL;
                    for (uint32_t m = 0; m < provider->export_count; m++) {
                        if (provider->exports[m].kind == WAH_KIND_FUNCTION &&
                            wah_name_matches(provider->exports[m].name, provider->exports[m].name_len,
                                             fi->name.field, fi->name.field_len)) {
                            exp = &provider->exports[m];
                            break;
                        }
                    }
                    WAH_ENSURE_GOTO(exp != NULL, WAH_ERROR_LINK_FAILED, cleanup);
                    uint32_t provider_import_count = provider->import_function_count;
                    WAH_ENSURE_GOTO(exp->index >= provider_import_count, WAH_ERROR_LINK_FAILED, cleanup);
                    uint32_t provider_local_idx = exp->index - provider_import_count;
                    WAH_ENSURE_GOTO(provider_local_idx < provider->local_function_count, WAH_ERROR_LINK_FAILED, cleanup);
                    ctx->globals[lg_offset + k].ref = wah_func_to_ref(&provider->functions[provider_local_idx]);
                }
            }
        }
        lg_offset += linked->global_count;
    }

    // Evaluate table init expressions (for tables declared with 0x40 encoding)
    for (uint32_t i = 0; i < module->table_count; ++i) {
        if (module->tables[i].init_expr.bytecode) {
            uint32_t slot = module->import_table_count + i;
            wah_value_t init_val;
            WAH_CHECK_GOTO(wah_eval_const_expr(ctx,
                module->tables[i].init_expr.bytecode,
                module->tables[i].init_expr.bytecode_size,
                &init_val), cleanup);
            if (init_val.ref == wah_func_to_ref(wah_funcref_sentinel)) {
                uint32_t func_idx = init_val._prefuncref.func_idx;
                WAH_ENSURE_GOTO(func_idx < ctx->function_table_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
                init_val.ref = wah_func_to_ref(&ctx->function_table[func_idx]);
            }
            for (uint64_t j = 0; j < ctx->tables[slot].size; ++j) {
                ctx->tables[slot].entries[j] = init_val;
            }
        }
    }

    // Initialize active element segments (before data segments per spec)
    for (uint32_t i = 0; i < module->element_segment_count; ++i) {
        const wah_element_segment_t *segment = &module->element_segments[i];

        // Skip passive and declarative segments. (We don't need to check the
        // per-instance dropped bitset: it's only set by the loop below or by
        // elem.drop, and we run before any user code.)
        if (!segment->is_active || segment->is_declarative) {
            continue;
        }

        WAH_ASSERT(segment->table_idx < ctx->table_count);

        // Evaluate offset expression to get the table offset
        wah_value_t offset_val;
        WAH_CHECK_GOTO(wah_eval_const_expr(ctx, segment->offset_expr.bytecode, segment->offset_expr.bytecode_size, &offset_val), cleanup);
        uint64_t offset;
        if (segment->table_idx < wah_table_index_limit(ctx->module) &&
            wah_table_type(ctx->module, segment->table_idx)->addr_type == WAH_TYPE_I64) {
            offset = (uint64_t)offset_val.i64;
        } else {
            offset = (uint32_t)offset_val.i32;
        }

        WAH_ENSURE_GOTO(wah_u64_range_in_bounds(offset, segment->num_elems,
                                                ctx->tables[segment->table_idx].size), WAH_ERROR_TRAP, cleanup);

        for (uint32_t j = 0; j < segment->num_elems; ++j) {
            if (!segment->is_expr_elem) {
                uint32_t global_func_idx = segment->u.func_indices[j];
                WAH_ENSURE_GOTO(global_func_idx < ctx->function_table_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
                wah_function_t *fn = &ctx->function_table[global_func_idx];
                if (!fn->is_host && fn->fn_ctx == NULL && fn->fn_module == ctx->module) fn->fn_ctx = ctx;
                ctx->tables[segment->table_idx].entries[offset + j].ref = wah_func_to_ref(fn);
            } else {
                wah_value_t elem_val;
                WAH_CHECK_GOTO(wah_eval_const_expr(ctx,
                                                   segment->u.expr.bytecodes[j],
                                                   segment->u.expr.bytecode_sizes[j],
                                                   &elem_val), cleanup);
                if (elem_val.ref == wah_func_to_ref(wah_funcref_sentinel)) {
                    uint32_t global_func_idx = elem_val._prefuncref.func_idx;
                    WAH_ENSURE_GOTO(global_func_idx < ctx->function_table_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
                    wah_function_t *fn = &ctx->function_table[global_func_idx];
                    if (!fn->is_host && fn->fn_ctx == NULL && fn->fn_module == ctx->module) fn->fn_ctx = ctx;
                    ctx->tables[segment->table_idx].entries[offset + j].ref = wah_func_to_ref(fn);
                } else {
                    ctx->tables[segment->table_idx].entries[offset + j] = elem_val;
                }
            }
        }

        // Active element segments are dropped (per-instance) after init per spec.
        wah_elem_seg_mark_dropped(ctx, i);
    }

    // Initialize active data segments (after element segments per spec)
    for (uint32_t i = 0; i < module->data_segment_count; ++i) {
        const wah_data_segment_t *segment = &module->data_segments[i];
        if (segment->flags == 0x00 || segment->flags == 0x02) { // Active segments
            WAH_ENSURE_GOTO(segment->memory_idx < ctx->memory_count, WAH_ERROR_VALIDATION_FAILED, cleanup);
            wah_value_t offset_val;
            WAH_CHECK_GOTO(wah_eval_const_expr(ctx, segment->offset_expr.bytecode, segment->offset_expr.bytecode_size, &offset_val), cleanup);
            uint64_t offset;
            if (wah_memory_type(module, segment->memory_idx)->addr_type == WAH_TYPE_I64) {
                offset = (uint64_t)offset_val.i64;
            } else {
                offset = (uint32_t)offset_val.i32;
            }
            WAH_ENSURE_GOTO(wah_u64_range_in_bounds(offset, segment->data_len,
                                                    ctx->memories[segment->memory_idx].size), WAH_ERROR_MEMORY_OUT_OF_BOUNDS, cleanup);
            if (segment->data_len > 0) {
                memcpy(ctx->memories[segment->memory_idx].data + offset, segment->data, segment->data_len);
            }
        }
    }

    // Ensure all wah_link_module-linked modules have an internal ctx so that
    // cross-module calls use the correct module pointer for type lookups.
    // This runs after all import resolution so memories/tables/globals are final.
    uint32_t go = wah_global_index_limit(module);
    for (uint32_t j = 0; j < ctx->linked_module_count; j++) {
        const wah_module_t *lmod = ctx->linked_modules[j].module;
        if (ctx->linked_modules[j].ctx == NULL) {
            wah_exec_context_t *ictx = NULL;
            WAH_CHECK_GOTO(wah_malloc(alloc, 1, sizeof(wah_exec_context_t), (void **)&ictx), cleanup);
            *ictx = (wah_exec_context_t){
                .alloc = ctx->alloc, .module = lmod, .memories = ctx->memories, .memory_count = ctx->memory_count,
                .tables = ctx->tables, .table_count = ctx->table_count,
                .globals = go ? ctx->globals + go : ctx->globals, .global_count = lmod->global_count,
                .function_table = ctx->function_table, .function_table_count = ctx->function_table_count,
                .memory_base = ctx->memory_base, .memory_size = ctx->memory_size, .gc = ctx->gc,
            };
            ctx->linked_modules[j].ctx = ictx;
            ctx->linked_modules[j].owns_ctx = true;
            ictx->is_instantiated = true;
        }
        wah_exec_context_t *ictx = ctx->linked_modules[j].ctx;
        if (ictx && !ictx->dropped_elem_segments && lmod->element_segment_count > 0) {
            size_t bytes = (lmod->element_segment_count + 7) / 8;
            WAH_MALLOC_ARRAY_GOTO(ictx->dropped_elem_segments, bytes, cleanup);
            memset(ictx->dropped_elem_segments, 0, bytes);
            for (uint32_t i = 0; i < lmod->element_segment_count; ++i) {
                if (lmod->element_segments[i].is_declarative) {
                    wah_elem_seg_mark_dropped(ictx, i);
                }
            }
        }
        if (ictx && !ictx->dropped_data_segments && lmod->data_segment_count > 0) {
            size_t bytes = (lmod->data_segment_count + 7) / 8;
            WAH_MALLOC_ARRAY_GOTO(ictx->dropped_data_segments, bytes, cleanup);
            memset(ictx->dropped_data_segments, 0, bytes);
        }
        go += lmod->global_count;
    }

    // If a start function is defined, call it after all imports/globals/elements are ready.
    if (module->has_start_function) {
        WAH_CHECK_GOTO(wah_call_module(ctx, module->start_function_idx, NULL, 0, NULL), cleanup);
    }

    ctx->is_instantiated = true;
    return WAH_OK;

cleanup:
    return err;
}

// --- Export API Implementation ---
uint32_t wah_module_type_count(const wah_module_t *module) {
    return module ? module->type_count : 0;
}
uint32_t wah_module_function_count(const wah_module_t *module) {
    return module ? module->import_function_count + module->local_function_count : 0;
}
uint32_t wah_module_global_count(const wah_module_t *module) {
    return module ? wah_global_index_limit(module) : 0;
}
uint32_t wah_module_memory_count(const wah_module_t *module) {
    return module ? wah_memory_index_limit(module) : 0;
}
uint32_t wah_module_table_count(const wah_module_t *module) {
    return module ? wah_table_index_limit(module) : 0;
}
uint32_t wah_module_tag_count(const wah_module_t *module) {
    return module ? module->import_tag_count + module->tag_count : 0;
}
uint32_t wah_module_import_count(const wah_module_t *module) {
    return module ? module->import_count : 0;
}
uint32_t wah_module_export_count(const wah_module_t *module) {
    return module ? module->export_count : 0;
}

wah_error_t wah_module_type(const wah_module_t *module, uint32_t typeidx, wah_type_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(typeidx < module->type_count, WAH_ERROR_NOT_FOUND);

    const wah_func_type_t *ft = &module->types[typeidx];
    const wah_type_def_t *td = &module->type_defs[typeidx];
    *out = (wah_type_desc_t){
        .kind = td->kind, .is_final = td->is_final, .supertype = td->supertype,
        .param_count = ft->param_count, .result_count = ft->result_count,
        .param_types = ft->param_types, .result_types = ft->result_types,
        .field_count = td->field_count, .field_types = td->field_types, .field_mutables = td->field_mutables,
    };
    return WAH_OK;
}

wah_error_t wah_module_function(const wah_module_t *module, uint32_t funcidx, wah_func_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(funcidx < wah_module_function_count(module), WAH_ERROR_NOT_FOUND);

    *out = (wah_func_desc_t){0};
    if (funcidx < module->import_function_count) {
        const wah_func_import_t *fi = &module->func_imports[funcidx];
        WAH_ENSURE(fi->type_index < module->type_count, WAH_ERROR_NOT_FOUND);
        const wah_func_type_t *ft = &module->types[fi->type_index];
        *out = (wah_func_desc_t){
            .is_import = true, .type_index = fi->type_index,
            .param_count = ft->param_count, .param_types = ft->param_types,
            .result_count = ft->result_count, .result_types = ft->result_types,
        };
        return WAH_OK;
    }

    uint32_t local_fn_idx = funcidx - module->import_function_count;
    WAH_ENSURE(module->functions && local_fn_idx < module->local_function_count, WAH_ERROR_NOT_FOUND);
    if (module->functions[local_fn_idx].is_host) {
        const wah_function_t *fn = &module->functions[local_fn_idx];
        *out = (wah_func_desc_t){
            .is_host = true, .type_index = UINT32_MAX,
            .param_count = (uint32_t)fn->nparams, .param_types = fn->param_types,
            .result_count = (uint32_t)fn->nresults, .result_types = fn->result_types,
        };
        return WAH_OK;
    }

    WAH_ENSURE(local_fn_idx < module->wasm_function_count, WAH_ERROR_NOT_FOUND);
    uint32_t type_idx = module->function_type_indices[local_fn_idx];
    WAH_ENSURE(type_idx < module->type_count, WAH_ERROR_NOT_FOUND);
    const wah_func_type_t *ft = &module->types[type_idx];
    *out = (wah_func_desc_t){
        .type_index = type_idx,
        .param_count = ft->param_count, .param_types = ft->param_types,
        .result_count = ft->result_count, .result_types = ft->result_types,
    };
    return WAH_OK;
}

wah_error_t wah_module_global(const wah_module_t *module, uint32_t globalidx, wah_global_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(globalidx < wah_global_index_limit(module), WAH_ERROR_NOT_FOUND);
    *out = (wah_global_desc_t){
        .type = wah_global_type(module, globalidx),
        .is_mutable = wah_global_is_mutable(module, globalidx),
    };
    return WAH_OK;
}

wah_error_t wah_module_memory(const wah_module_t *module, uint32_t memidx, wah_memory_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(memidx < wah_memory_index_limit(module), WAH_ERROR_NOT_FOUND);
    const wah_memory_type_t *mt = wah_memory_type(module, memidx);
    *out = (wah_memory_desc_t){ .addr_type = mt->addr_type, .min_pages = mt->min_pages, .max_pages = mt->max_pages };
    return WAH_OK;
}

wah_error_t wah_module_table(const wah_module_t *module, uint32_t tableidx, wah_table_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(tableidx < wah_table_index_limit(module), WAH_ERROR_NOT_FOUND);
    const wah_table_type_t *tt = wah_table_type(module, tableidx);
    *out = (wah_table_desc_t){
        .elem_type = tt->elem_type, .addr_type = tt->addr_type,
        .min_elements = tt->min_elements, .max_elements = tt->max_elements,
    };
    return WAH_OK;
}

wah_error_t wah_module_tag(const wah_module_t *module, uint32_t tagidx, wah_tag_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(tagidx < wah_module_tag_count(module), WAH_ERROR_NOT_FOUND);
    uint32_t type_index = wah_tag_type_index(module, tagidx);
    WAH_ENSURE(type_index < module->type_count, WAH_ERROR_NOT_FOUND);
    const wah_func_type_t *ft = &module->types[type_index];
    *out = (wah_tag_desc_t){ .type_index = type_index, .param_count = ft->param_count, .param_types = ft->param_types };
    return WAH_OK;
}

static wah_error_t wah_module_extern_desc(int8_t kind, const wah_module_t *module, uint32_t idx, wah_extern_desc_t *out) {
    switch (kind) {
        case WAH_KIND_FUNCTION: return wah_module_function(module, idx, &out->func);
        case WAH_KIND_TABLE: return wah_module_table(module, idx, &out->table);
        case WAH_KIND_MEMORY: return wah_module_memory(module, idx, &out->memory);
        case WAH_KIND_GLOBAL: return wah_module_global(module, idx, &out->global);
        case WAH_KIND_TAG: return wah_module_tag(module, idx, &out->tag);
        default:
            WAH_ASSERT(false && "verification should have prevented invalid import/export kinds");
            return WAH_ERROR_VALIDATION_FAILED;
    }
}

wah_error_t wah_module_import(const wah_module_t *module, uint32_t importidx, wah_import_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(importidx < module->import_count, WAH_ERROR_NOT_FOUND);
    *out = module->imports[importidx];
    return wah_module_extern_desc(out->kind, module, out->index, &out->u);
}

wah_error_t wah_module_export(const wah_module_t *module, size_t idx, wah_export_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);
    WAH_ENSURE(idx < module->export_count, WAH_ERROR_NOT_FOUND);

    const wah_export_t *export_entry = &module->exports[idx];
    *out = (wah_export_desc_t){
        .kind = export_entry->kind, .index = export_entry->index,
        .name = export_entry->name, .name_len = export_entry->name_len,
    };

    return wah_module_extern_desc(export_entry->kind, module, export_entry->index, &out->u);
}

wah_error_t wah_export_by_name(const wah_module_t *module, const char *name, wah_export_desc_t *out) {
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    return wah_export_by_name_len(module, name, strlen(name), out);
}

wah_error_t wah_export_by_name_len(const wah_module_t *module, const char *name, size_t name_len, wah_export_desc_t *out) {
    WAH_ENSURE(module, WAH_ERROR_MISUSE);
    WAH_ENSURE(name, WAH_ERROR_MISUSE);
    WAH_ENSURE(out, WAH_ERROR_MISUSE);

    for (uint32_t i = 0; i < module->export_count; ++i) {
        const wah_export_t *export_entry = &module->exports[i];
        if (wah_name_matches(export_entry->name, export_entry->name_len, name, name_len)) {
            return wah_module_export(module, i, out);
        }
    }
    return WAH_ERROR_NOT_FOUND;
}

////////////////////////////////////////////////////////////////////////////////

#endif // !__cplusplus
#endif // WAH_IMPLEMENTATION
#endif // WAH_H
