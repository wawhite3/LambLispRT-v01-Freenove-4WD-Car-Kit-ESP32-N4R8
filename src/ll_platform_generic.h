// Copyright 2026 by Frobenius Norm LLC 2026-05-16
// Free for non-commercial use. Commercial use requires a license.
#ifndef LL_PLATFORM_GENERIC_H
#define LL_PLATFORM_GENERIC_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <type_traits>
#include <new>
#include "assert.h"
#include "unistd.h"

// Default all LL_ feature flags to 0 so they can be used as C++ values, not just #if guards.
//! TWO MUTUALLY EXCLUSIVE BACKENDS FOR THE SAME API.  Read this before editing either.
#ifndef LL_ESP_ARDUINO
#define LL_ESP_ARDUINO       0
#endif

#ifndef LL_ARDUINO
#define LL_ARDUINO  0
#endif

#ifndef LL_POSIX
#define LL_POSIX         0
#endif

//! WebAssembly.  LL_WASM is the umbrella ("this is a wasm32 target"); LL_WASI and
//! LL_EMSCRIPTEN name the two HOSTS, which differ only in their I/O and filesystem shims.
//! LL_WASM IMPLIES LL_POSIX HERE
#ifndef LL_WASM
#define LL_WASM          0
#endif
#ifndef LL_WASI
#define LL_WASI          0
#endif
#ifndef LL_EMSCRIPTEN
#define LL_EMSCRIPTEN    0
#endif

//! Native Windows, cross-built with MinGW-w64.
//! LL_WINDOWS IMPLIES LL_POSIX AND SUBTRACTS, exactly as LL_WASM does.
#ifndef LL_WINDOWS
#define LL_WINDOWS       0
#endif
#ifndef LL_WIFI
#define LL_WIFI          0
#endif
#ifndef LL_LITTLEFS
#define LL_LITTLEFS      0
#endif

#ifndef LL_GCCHK_VISITED_GROWBY
  #if LL_ESP32
    #define LL_GCCHK_VISITED_GROWBY  4096
    #define LL_GCCHK_STACK_INIT      256
    #define LL_GCCHK_STACK_GROWBY    8
    #define LL_GCCHK_STACK_CAP       8192
    #define LL_GCCHK_VECSTACK_INIT   64
    #define LL_GCCHK_VECSTACK_GROWBY 8
    #define LL_GCCHK_VECSTACK_CAP    1024
  #else
    #define LL_GCCHK_VISITED_GROWBY  (-1)
    #define LL_GCCHK_STACK_INIT      256
    #define LL_GCCHK_STACK_GROWBY    256
    #define LL_GCCHK_STACK_CAP       (1<<20)
    #define LL_GCCHK_VECSTACK_INIT   64
    #define LL_GCCHK_VECSTACK_GROWBY 64
    #define LL_GCCHK_VECSTACK_CAP    (1<<16)
  #endif
#endif

#ifndef LL_EVAL_STACK_BUDGET
  #if LL_WASM
  //! WASM HAS NO 8 MB STACK.  The linear-memory shadow stack is exactly what the LINKER was told
  //! to reserve (`-Wl,-z,stack-size=`, default 64 KB), and overrunning it does not fault -- it
  //! silently walks into static data below __stack_pointer, so the first symptom is corrupted
  //! globals, not a crash at the overrun.  This budget MUST stay below the linker's stack-size in
  //! w3_pio/platforms/wasm32/builder/main.py; change one and you must change the other.
  #define LL_EVAL_STACK_BUDGET  (3UL * 1024 * 1024)   /*!< 3 MB of the 4 MB wasm stack (1 MB headroom) */
  #elif LL_POSIX
  #define LL_EVAL_STACK_BUDGET  (6UL * 1024 * 1024)   /*!< ~6 MB of the 8 MB POSIX stack (2 MB headroom) */
  #else
  #define LL_EVAL_STACK_BUDGET  (40UL * 1024)         /*!< ~40 KB of the 48 KB ESP32 loopTask stack (8 KB headroom); see getArduinoLoopTaskStackSize() in main.cpp -- keep budget < stack so the recursion guard raises a catchable "recursion too deep" before the real stack overflows */
  #endif
#endif

/*!
  FLOOR for a RUNTIME-lowered budget -- see `(recursion-limit N)` (mop3_recursion_limit, ll_vm_mop3_extra.cpp).
  The budget is settable from Scheme so a test can make the recursion guard trip at shallow depth instead of after ~39 k frames
  It's a budget small enough to be exceeded by the REPL's own frames trips on the very `(recursion-limit ...)`
  call that would restore it, so the session can only be ended by killing the process -- there is no depth
  shallow enough to recover from. 16 KB is ~100 eval levels on POSIX (~160 B/level) and
  ~240 on the ESP32 (~68 B/level): deep enough that a top-level restore always fits, shallow enough that provoking the guard is instant.
*/
#ifndef LL_EVAL_STACK_BUDGET_MIN
#define LL_EVAL_STACK_BUDGET_MIN  (16UL * 1024)
#endif

/*! LL_NCG_DIRAM_POOL below tests LL_RISCV32, and a #define that lands AFTER its own use reads as 0 -- the guard would
  silently take the wrong arm on any target that does not pass -DLL_RISCV32 on the command line.
*/
#ifndef LL_XTENSA
#define LL_XTENSA                         0
#endif
#ifndef LL_RISCV32
#define LL_RISCV32                        0
#endif

//! Size of the NCG executable (D/IRAM) pool.
#ifndef LL_NCG_DIRAM_POOL
  #if LL_ESP32 && LL_XTENSA
    #define LL_NCG_DIRAM_POOL 1
  #else
    #define LL_NCG_DIRAM_POOL 0
  #endif
#endif

#ifndef LL_NCG_RISCV_POOL
  #if LL_RISCV32 && ((defined(LL_C5_PMP_RWX) && LL_C5_PMP_RWX) || \
                     (defined(LL_C5_NCG_ARENA) && LL_C5_NCG_ARENA))
    #define LL_NCG_RISCV_POOL 1
  #else
    #define LL_NCG_RISCV_POOL 0
  #endif
#endif

//! Minimum internal RAM to reserve for system functions (WiFi, FS buffers etc).
#ifndef LL_NCG_EXEC_FLOOR_DEFAULT
  #define LL_NCG_EXEC_FLOOR_DEFAULT 24576
#endif

#ifndef LL_NCG_PSRAM_EXEC
  #if defined(BOARD_HAS_PSRAM) && (LL_ESP32S3 || LL_RISCV32)
    #define LL_NCG_PSRAM_EXEC 1
  #else
    #define LL_NCG_PSRAM_EXEC 0
  #endif
#endif

//! LL_EXTRAM_SEPARATE_IBUS -- IS EXTERNAL RAM REACHED THROUGH A SEPARATE INSTRUCTION-BUS WINDOW?
#ifndef LL_EXTRAM_SEPARATE_IBUS
  #if LL_ESP32 && LL_XTENSA
    #define LL_EXTRAM_SEPARATE_IBUS 1
  #else
    #define LL_EXTRAM_SEPARATE_IBUS 0
  #endif
#endif

//! LL_NCG_PSRAM_ALIAS -- does this target need the DBUS->IBUS address transform, or is the data
//! pointer directly executable?
//!
//!   Xtensa S3: TRANSFORM.  Data and instructions reach PSRAM through different windows
//!              (0x3C…… and 0x42……) onto the same MMU entry, so the exec pointer is
//!              `w - SOC_DRAM0_CACHE_ADDRESS_LOW + SOC_IRAM0_CACHE_ADDRESS_LOW`.  This is the same
//!              shape as LL_NCG_DIRAM_POOL's D/IRAM alias for internal memory, one window further
//!              out.  Verified on esp32-s3-devkitc-1 (2 MB quad) and esp32s3-n32r16-00 (16 MB
//!              octal), at linear offsets 0x2686b0 and 0x392524 -- different memory types, different
//!              regions of the window, both reporting the alias probe's result=42.
//!   RISC-V C5: NONE.  SOC_MMU_DI_VADDR_SHARED -- one address space, so the write pointer IS the
//!              exec pointer and a transform would be wrong rather than redundant.
#ifndef LL_NCG_PSRAM_ALIAS
  #if LL_NCG_PSRAM_EXEC && LL_ESP32S3
    #define LL_NCG_PSRAM_ALIAS 1
  #else
    #define LL_NCG_PSRAM_ALIAS 0
  #endif
#endif

//! LL_WIFI_RESERVE_BYTES -- what the radio stack costs, held from installer time until WiFi.begin().
//! The reservation must be made BEFORE the radio runs.  This number is conservative, not exact.
#ifndef LL_WIFI_RESERVE_BYTES
  #define LL_WIFI_RESERVE_BYTES  (48UL * 1024)
#endif

//! [B973] RAISE THE GC BUDGET WHEN THE CELL POOL IS MAXED AND yuasa_N STILL EXCEEDS IT.
//! OFF BY DEFAULT, so shipped pause behaviour is byte-identical and the experiment cannot leak into
//! a release.  ON, it trades LATENCY for PACE: the budget doubles (at most 4x, so 400us worst case)
//! where the alternative is the collector falling behind, which is the state measured on
//! esp32-c5-wroom-1-n8r8-00 at 77% occupancy -- yuasa_N 66,857 against a pinned 65,536-cell pool,
//! `gc-fell-behind` on one run and a bare pass on an identical one.  See the block at the per-cycle
//! repacing call in ll_vm_mem.cpp for why the budget is the only lever once growth is unavailable.
//! THIS CHANGES THE REAL-TIME CLAIM WHERE IT FIRES, which is why it is measured before it is a
//! default: a longer bounded pause is still a bounded pause, but it is not the SAME bound.
#ifndef LL_GC_BUDGET_WHEN_MAXED
#define LL_GC_BUDGET_WHEN_MAXED 0
#endif

#ifndef LL_NCG_EXEC_POOL_BYTES
  #if LL_NCG_RISCV_POOL
    #define LL_NCG_EXEC_POOL_BYTES  (32UL * 1024)
  #elif LL_ESP32S3
    #define LL_NCG_EXEC_POOL_BYTES  (32UL * 1024)
  #elif LL_ESP32
    //! THE CLASSIC ESP32 (LX6) GETS 32 KB internal RAM, and it is the only fast exec memory it will ever have.
    #define LL_NCG_EXEC_POOL_BYTES  (32UL * 1024)
  #else
    #define LL_NCG_EXEC_POOL_BYTES  (32UL * 1024)
  #endif
#endif
    
//! Hard cap on the GC rootstack (entries).  Each non-tail recursion level pushes ~5 roots.
#ifndef LL_ROOTSTACK_MAX
  #if LL_WASM
  //! Sized from LL_EVAL_STACK_BUDGET above (3 MB, not the POSIX 6 MB), same ~5 roots per level.
  #define LL_ROOTSTACK_MAX  131072
  #elif LL_POSIX
  #define LL_ROOTSTACK_MAX  262144
  #else
  #define LL_ROOTSTACK_MAX  65536
  #endif
#endif
    
//! Physical slack allocated above LL_ROOTSTACK_MAX so the overflow error path (which itself
//! allocates a T_ERROR and pushes a few roots) has real buffer instead of writing out of bounds.
#ifndef LL_ROOTSTACK_SLACK
#define LL_ROOTSTACK_SLACK  256

/*! INITIAL sizes for the GC stacks, which are NOT their caps (owner, 2026-10-21).
  These are autobufs: they allocate small, grow on demand via upsize(), and LL_ROOTSTACK_MAX above
  is only the ceiling they may never pass.  The distinction matters because it is easy to reason
  about the cap and conclude a buffer is too big to live in internal DRAM -- at 65,536 entries the
  rootstack would be 256 KB, but it STARTS at 256 entries (1 KB) and CLAUDE.md's own measurement
  puts an MCU at ~165 eval levels, i.e. ~800 roots, before the C stack runs out.  So the live
  buffer is a few KB and internal residency is the ordinary case, which is what qualifies it for
  P254's use_dram().
  NAMED RATHER THAN LITERAL so the three places that size a GC stack agree and can be tuned in one
  edit; they were three separate `256`s, and the markstack's pair still are.  Growth stays with the
  autobuf (upsize doubles), and migration to PSRAM on outgrowing internal needs no code -- see the
  use_dram() note at the rootstack's init. */
#ifndef LL_ROOTSTACK_INIT
  #define LL_ROOTSTACK_INIT  256
#endif
#ifndef LL_MARKSTACK_INIT
  #define LL_MARKSTACK_INIT  256
#endif
#endif
  
//! Recursion guard's rootstack trip point -- a little below the cap, leaving room for the error path.
#ifndef LL_EVAL_ROOT_DEPTH_MAX
#define LL_EVAL_ROOT_DEPTH_MAX  (LL_ROOTSTACK_MAX - 536)
#endif

/*! ROOT-AT-MATERIALIZE IS UNCONDITIONAL -- THE FLAG IS GONE (owner, 2026-10-23: *"remove the
  materialize flag, and leave the feature ON, materialize as required and root-at-materialize"*).

  `LL_ROOT_ON_MATERIALIZE` (earlier `LL_ROOT_AT_ALLOC`) gated an experiment: does rooting an
  accessor's box AT THE MOMENT IT IS CREATED replace the per-site protection rule?  It was
  measured, it answered yes, it defaulted to 1 on every target from 2026-10-21, and it is now
  REMOVED rather than left as a switch nobody should throw.  There is no A/B to restore and no
  off path to reason about.

  THE REASONING MOVED TO THE CODE, not to a flag block: see `Lamb::materialized()` in LambLisp.h,
  which is the single place a box is rooted, for what this covers, what it does NOT cover
  (allocator results stay root-on-need) and what made it viable (the folds, 100,024 roots -> 28).
  [B846] [B856] [P282] */

/*! LL_ROOT_HIGH_WATER -- track the deepest the GC rootstack has ever been.

  ON BY DEFAULT ON A HOST, OFF ON A BOARD -- the same split as LL_ROOT_BALANCE below, and for the
  same reason: this is an INSTRUMENT, the host is where the experiments run, and an MCU should not
  pay for a diagnostic it is not reading.  Gated on the owner's instruction, 2026-10-21.

  WHAT IT COSTS WHEN ON: one compare and a rarely-taken store in `rootpush()`, which is ~2
  instructions against a push of ~10-12.  Small, but it was UNCONDITIONAL and therefore in every
  customer build's hot path, which is the objection -- not the size of it.

  WHY ITS OWN FLAG.  It was kept out of the root-at-materialize flag (`LL_ROOT_AT_ALLOC`, later
  `LL_ROOT_ON_MATERIALIZE`) because it was the INSTRUMENT deciding whether that flag was viable, so
  it had to read the same way in both arms.  That flag is gone -- the feature is unconditional as of
  2026-10-23 -- and this one stays, both because a board should not pay for a diagnostic it is not
  reading and because the peak outlives the experiment that needed it.

  WHEN COMPILED OUT, `root_depth_max()` RETURNS -1, NOT 0.  Zero is a number that could be true and
  would read as "the rootstack never grew"; -1 cannot be mistaken for a depth, so a caller and a
  test can both tell "not compiled in" from "measured".  `(Platform.root-depth-max)` therefore
  answers -1 on a stock board build, and `(Platform.root-depth-max!)` is a no-op there.

  IT OUTLIVES THE EXPERIMENT, which is why it is kept rather than deleted with the flag it served:
  `rootpush_overflow()` throws [B71] with no record of how fast the depth got there, and a peak is
  the one number that separates "a loop with no chop" from "a deep call". */
#ifndef LL_ROOT_HIGH_WATER
  #if LL_POSIX
  #define LL_ROOT_HIGH_WATER  1
  #else
  #define LL_ROOT_HIGH_WATER  0
  #endif
#endif

/*! LL_ROOT_BALANCE -- check, at run time, that a callee leaves the rootstack exactly as it found it.

  THE INVARIANT (owner, 2026-10-20): with minimal selective pushes terminated by ONE `setdepth`,
  *no function should exit with a larger rootstack -- nor with a smaller one.*  Larger means a push
  was left behind for the caller to pay for; smaller means the chop went below its own entry depth
  and took the CALLER's roots with it, which is the `gc_root_clear()` hazard the GC rules warn about.

  WHY A SECOND INSTRUMENT WHEN THREE ALREADY REPORT "root leak".  `Lamb::eval`, `apply_proc_partial`
  and `ncg_eval_jit_call` each compare an entry depth with an exit depth -- and all three take the
  comparison AFTER their own frame closes, so the frame's `setdepth` has already restored the depth
  being compared and the equality is true by construction.  PROVEN with a positive control rather
  than argued: an unbalanced `gc_root_push` added to a mop3 body produces ZERO "root leak" lines on
  a host build, and moving one of those comparisons inside its frame made it fire 367 times on the
  same four-expression session.  So a root leaked anywhere inside a mop3 is invisible to them, and
  a clean 1407-test conformance run saying nothing is not evidence of balance.

  WHAT THIS MEASURES INSTEAD: the depth immediately before a CALL against the depth immediately
  after it returns, at the four dispatch sites every evaluator path goes through -- so one bracket
  covers every mop3, every bytecode apply and every native apply, with no per-function edits.
  See `LL_ROOT_BRACKET` in LambLisp.h and `(root-leak-inject N)`, the injector that exists so this
  check can be SEEN reporting a violation instead of only ever agreeing.

  ON BY DEFAULT ON A HOST, OFF ON A BOARD.  The cost is two counter reads and a compare per
  dispatch, which is cheap but not free on an MCU's hot apply path; the host is where the
  conformance suite runs, so that is where it earns its keep.  `-DLL_ROOT_BALANCE=1` turns it on
  anywhere, `=0` off. */
#ifndef LL_ROOT_BALANCE
  #if LL_POSIX
  #define LL_ROOT_BALANCE  1
  #else
  #define LL_ROOT_BALANCE  0
  #endif
#endif
//! LL_HAVE_BACKTRACE -- does this platform provide glibc's <execinfo.h> `backtrace()`?  [B824]
//! THE SINGLE HOME FOR THIS PREDICATE, and it is a POSITIVE test naming what the facility requires
//! rather than a denylist of the platforms that lack it.  `src/` is compiled by EVERY env, so the
//! question is never "does this break the board" but "which envs have this header".  Written as
//! `#if !LL_ESP32` it reads as correct and breaks `windows_x86_64` (mingw) and both wasm envs --
//! which it did, killing a publish_run at PHASE 1 with nothing released.  A positive predicate also
//! survives someone adding a thirteenth env; a `!PLATFORM` guard silently does not.
//! IT LIVES HERE BECAUSE IT HAD ALREADY BEEN DUPLICATED, across an include and its own call site,
//! and correcting one copy breaks the other: the include without the call is merely dead code, while
//! the call without the include is an implicit-declaration failure in whichever env you were not
//! building.  Two copies would next have disagreed only on a platform nobody here builds.
//! Safe to test `__GLIBC__` at this point: <stdio.h> is included at the top of this header, so
//! glibc's <features.h> has already defined it.  Tested before this header, it would read 0 on a
//! glibc host and silently compile the diagnostic OUT -- verify with `nm`, never with build status.
#ifndef LL_HAVE_BACKTRACE
  #if defined(__GLIBC__) && !LL_ESP32
  #define LL_HAVE_BACKTRACE 1
  #else
  #define LL_HAVE_BACKTRACE 0
  #endif
#endif

#ifndef LL_I2C
#define LL_I2C           0
#endif
#ifndef LL_COMMONIO
#define LL_COMMONIO      0
#endif
#ifndef LL_ESP32
#define LL_ESP32         0
#endif
#ifndef LL_COMPLEX
#define LL_COMPLEX       0
#endif
#ifndef LL_RATIONAL
#define LL_RATIONAL      0
#endif
#ifndef LL_BIGNUM
#define LL_BIGNUM        0
#endif
#ifndef LL_BIGNUM_LIMB_BITS
#define LL_BIGNUM_LIMB_BITS 32
#endif
//! STRICT REAL-TIME BIGNUM: no heap allocation at runtime, so an oversized temporary is a catchable failure instead of a malloc.
#ifndef LL_BIGNUM_STRICT_RT
#define LL_BIGNUM_STRICT_RT 0
#endif
  
#ifndef LL_MODBUS
#define LL_MODBUS        0
#endif
#ifndef LL_PROFIBUS
#define LL_PROFIBUS      0
#endif
#ifndef LL_PROFINET
#define LL_PROFINET      0
#endif
#ifndef LL_CUDA
#define LL_CUDA          0
#endif
#ifndef LL_HIP
#define LL_HIP           0
#endif

// Board-specific flags (set by platformio.ini build_flags per env)
#ifndef LL_ESP32_S3_DEVKIT_C
#define LL_ESP32_S3_DEVKIT_C              0
#endif
#ifndef LL_ESP32S3_N8R2
#define LL_ESP32S3_N8R2                   0
#endif
#ifndef LL_Freenove_4WD_Car_Kit_ESP32
#define LL_Freenove_4WD_Car_Kit_ESP32     0
#endif
#ifndef LL_Freenove_ESP32_WROVER
#define LL_Freenove_ESP32_WROVER          0
#endif
#ifndef LL_ESP32_N4R4
#define LL_ESP32_N4R4                     0
#endif
#ifndef LL_AMD64
#define LL_AMD64                          0
#endif
#ifndef LL_X86_64
#define LL_X86_64                         0
#endif
#ifndef LL_AARCH64
#define LL_AARCH64                        0
#endif
#ifndef LL_ARM64
#define LL_ARM64                          0
#endif
#ifndef LL_WASM32
#define LL_WASM32                         0
#endif

//! THE "DEMO PROFILE" SWITCHES -- factored here ONCE because the Windows (MinGW) and WebAssembly
//! (WebAssembly) need byte-for-byte the same exclusion set, and two proposals each carrying
//! their own copy of the list is how the two drift apart.
//!
//!   LL_NETWORK  0 -> no BSD/WiFi sockets at all (TCP client/server ports, LLIP over TCP).
//!   LL_TERMIOS  0 -> no raw terminal mode; line-at-a-time stdio only.
//!   LL_NCG      0 -> no native code generator; the AST and bytecode tiers are the runtime.
//!
//! Each DEFAULTS to what the platform historically had, so no existing target changes; a port
//! that cannot provide one sets it to 0 in its build_flags and every guard follows.
//!
//! WHY THESE ARE SEPARATE FROM LL_POSIX: LL_POSIX has quietly meant three unrelated things --
//! "has open/read/write", "has sockets", and "has termios".  wasm32-wasi is the first target to
//! have the first without the other two, so the single flag stopped being able to express the
//! platform.  A guard that means "has a socket" must now SAY so.
#ifndef LL_NETWORK
  #if LL_WASM
  #define LL_NETWORK  0	//!< browsers have no raw TCP (WebSocket only); WASI sockets are incomplete
  #else
  #define LL_NETWORK  ((LL_WIFI) || (LL_POSIX))
  #endif
#endif
//! Sockets that specifically come from the BSD/POSIX implementation (the
//! posix_fd paths), as opposed to the Arduino WiFi ones.
#define LL_POSIX_NET  ((LL_POSIX) && (LL_NETWORK))

#ifndef LL_TERMIOS
  #if LL_WASM
  #define LL_TERMIOS  0	//!< wasi-libc has no termios.h at all -- not a stub, the header is absent
  #else
  #define LL_TERMIOS  (LL_POSIX)
  #endif
#endif

//! LL_NCG -- is a native code generator COMPILED IN at all.
//! WASM IS A BUILD TARGET, NOT AN NCG BACKEND, and that is a design decision, not a gap to fill
//! later: wasm is a stack machine with structured control flow and no
//! self-modifying code, the host engine already JITs it, and emitting wasm from our
//! register-machine backends would be a JIT feeding a JIT.  If you are here because you want to
//! "add a wasm NCG backend", read that section first.
#ifndef LL_NCG
  #if LL_WASM
  #define LL_NCG  0
  #else
  #define LL_NCG  1
  #endif
#endif

//! LL_AUTOCOMPILE -- what the RUNTIME compiles for itself at startup, in main.cpp:
//!   0  leave every procedure as an AST (T_PROC), compile nothing
//!   1  run (compile-environment!)       -- AST -> bytecode
//!   2  also run (ncg-compile-environment!) -- bytecode -> native
//!
//! DEFAULT 2 WHERE THERE IS A BACKEND.  Until now this was set ONLY by the linux_x86_64_bc and
//! linux_x86_64_ncg diagnostic envs, so every shipping build booted fully interpreted and the
//! bytecode and native paths -- the whole point of the runtime -- ran only when a test asked for
//! them.  The startup census added alongside this reports the result, so the mode a board is
//! actually in is visible at boot rather than inferred.
//!
//! wasm gets 1, not 2: LL_NCG is 0 there (the host engine already JITs; see the note above), so
//! asking for a native pass would be asking a backend that does not exist.
//!
//! READ THIS BEFORE SHIPPING IT ON AN MCU.  On ESP32 the native code pool is 32 KB and
//! ncg-compile-environment! is DESIGNED to consume it down to a floor -- ncg_exec_budget_low()
//! stops at pool_free < 4096, and the last compile overshoots (measured: 1608 free of 32768,
//! ).  Doing that at BOOT means every procedure the user defines afterwards, and every test
//! that compiles one, starts against a pool that is already spent.  On the host this is free --
//! the pool is megabytes -- so a green host build says nothing about it.  That is the open
//! measurement of what that costs; do not read a passing linux run as clearance for a board.
//! DEFAULT 1 -- COMPILE EVERYTHING TO BYTECODE, NOTHING TO NATIVE.  Not a compromise; it follows
//! from where the two things live.  BYTECODE IS ORDINARY HEAP.  NATIVE CODE IS THE 32 KB EXEC
//! POOL.  Compiling every procedure to bytecode costs nothing scarce.  Compiling them to native
//! spends a small fixed shared resource that is never reclaimed -- so native must be rationed, and
//! rationing by "whatever the environment hash-walk reaches first" is not rationing.
//!
//! MEASURED on an S3 with 2 MB PSRAM, with the blanket LL_AUTOCOMPILE=2 this replaces:
//!
//!     auto-compile: 230 procedures compiled
//!     ncg_compile_env() exec budget spent -- stopping (pool free 2896 of 32768)
//!     ncg-compile: 18 procedures compiled
//!     execution modes: AST 2  BC 212  NCG 18  (232 procedures)
//!
//! 18 of 230 reached native -- 8% -- for 91% of the pool.  The 18 were whichever the walk reached
//! first: WS2812.setAll, WS2812.setLedColorData, Buzzer.click, Sonar.start, diffie-hellman-keygen,
//! define-library, csv-opt, cddadr ... device drivers and rare library calls.  The pool bought
//! nothing and was then unavailable to code that mattered -- including the benchmark harness, which
//! must get ~14 procedures native to measure the NCG path at all (see what a perf row looks
//! like when it silently does not).
//!
//! IT IS NOT "FAST RAM".  IT IS LOW-LATENCY RAM, AND THE DIFFERENCE DECIDES THE WHOLE POLICY
//! (an owner decision).  On-die SRAM runs at roughly CACHE speed -- not faster than cached PSRAM,
//! just never slower.  A hot function is cache-friendly by definition: it is fetched once and
//! hits thereafter, so moving it on-die buys almost nothing on average.  That is not a guess, it
//! is the measurement two blocks up: pool residency is worth 0.022%-0.157% of THROUGHPUT.
//! What the pool actually buys is that a fetch stall CANNOT HAPPEN -- a worst-case guarantee, of
//! value to a control loop with a deadline and of almost none to anything else.
//!
//! SO THE PER-PART PICTURE IS NOT "FASTER MEMORY, RATION IT":
//!   S3, C5     PSRAM is EXECUTABLE and there are megabytes of it.  Native code belongs there by
//!              default.  The on-die pool is a latency EXCEPTION, requested per procedure.
//!   WROVER     PSRAM is NOT executable, so internal SRAM is the only place native code can live
//!              at all.  There the pool is a capability, not an optimisation -- and note it is
//!              reached through heap_caps_malloc(MALLOC_CAP_EXEC), which draws on the whole
//!              128 KB D/IRAM region rather than a pre-reserved 32 KB slice of it.
//!
//! AND THEREFORE PINNING, NOT A HEURISTIC.  Which procedures need a latency guarantee is an
//! APPLICATION fact -- it depends on which loop has a deadline, and the runtime cannot know that.
//! Any heuristic the VM could apply (size, call count, compile order) is a proxy for something it
//! cannot see, and the measured cost of guessing is on record: a hash-walk order gave 91% of the
//! pool to WS2812.setAll, Buzzer.click, cddadr and define-library.  `compile-low-latency!` lets
//! the application say which, and that is the whole mechanism -- there is deliberately no
//! automatic promotion into this tier, on any part.  A later fix made the RISC-V arm honour it; it had
//! been taking the pool first-come because it could not see the request flag.
//!
//! SO: 1 here, with SELECTIVE native compilation layered on top -- a prioritised, per-application
//! list.  That needs no new C++: `procedure->bytecode` puts a procedure in bytecode,
//! `ncg-compile-transitive!` takes it native, and bench-runner has driven both from an explicit
//! list for months.  What is missing is only where the list lives and who supplies it per target.
//!
//! wasm gets 1 and never 2: LL_NCG is 0 there (the host engine already JITs).  The linux_x86_64_bc
//! (=1) and linux_x86_64_ncg (=2) diagnostic envs set it explicitly and are unaffected; on a host
//! the pool is megabytes, so a blanket native pass is genuinely free there.
#ifndef LL_AUTOCOMPILE
  #define LL_AUTOCOMPILE  1
#endif


//! LL_XTENSA / LL_RISCV32 -- NAME THE ISA, NOT THE CHIP.
//!
//! An NCG backend is selected by INSTRUCTION SET, and nothing in this tree previously said
//! so.  `LL_XTENSA` was referenced by the backend guard and DEFINED NOWHERE -- grep the
//! whole tree and it appears only inside `#if LL_XTENSA || LL_ESP32` -- so the Xtensa backend was
//! in practice selected by **LL_ESP32 alone**.
//!
//! THE CONSEQUENCE, and it is the reason this block exists rather than a comment: every Espressif
//! part newer than the ESP32-S3 is RISC-V (C2, C3, C5, C6, C61, H2, P4).  Such a target MUST set
//! -DLL_ESP32=1 -- it needs heap_caps, the Arduino core, and the ESP32 branch of the platform
//! layer -- and the moment it does, it compiles the XTENSA CODE GENERATOR into a RISC-V binary.
//! That is not a build error; it emits Xtensa opcodes into a buffer and jumps to them.
//!
//! This is the identical defect the LL_NCG_DIRAM_POOL note above documents -- keying a capability
//! off a chip model means the only way to enable it elsewhere is to CLAIM TO BE that chip -- and
//! the LX6 boards already lost NCG entirely to that mistake once.
//!
//! Both default to 0, and the existing Xtensa envs now pass -DLL_XTENSA=1 EXPLICITLY, so current
//! behaviour is preserved by statement rather than by omission.  A target that sets neither gets
//! `ncg_make_backend()` returning nullptr (the complement guard) and therefore runs the bytecode
//! interpreter -- a SUPPORTED state, but ONLY WHEN IT IS DECLARED: that complement
//! guard now `#error`s if LL_NCG is on and no ISA above matched, because LL_NCG DEFAULTS TO 1
//! (see its definition just above) and "no backend" was therefore reachable by OMISSION -- a new
//! ISA with no flag built clean and ran bytecode while its benchmarks still said NCG.
//! Declare the absence with -DLL_NCG=0 in the env's build_flags, as wasm32_wasi, wasm32_browser
//! and windows_x86_64 do.  If you add an ISA here, add it to that complement guard too, or you
//! get a duplicate symbol instead of a missing one.
#ifndef LL_XTENSA
#define LL_XTENSA        0                //!<Tensilica Xtensa LX6/LX7 (ESP32, ESP32-S2/S3).
#endif
#ifndef LL_RISCV32
#define LL_RISCV32       0                //!<32-bit RISC-V (RV32IMC): ESP32-C/H/P series, and others.
#endif


//! Backend selection.  See the note at the flag definitions above: LL_ESP_ARDUINO is Espressif's core,
//! LL_ARDUINO is ours -- one underscore apart, opposite meanings.
#if LL_ESP_ARDUINO
#include "Arduino.h"
#endif

//! LLArduino: LambLisp's own implementation of the Arduino API.  Not a stub and not a
//! fallback -- it is the ONLY backend on POSIX, WASM and Windows, and it is what the entire host
//! test suite exercises.  Anything added here is API SURFACE WE OWE ON EVERY TARGET.
#if LL_ARDUINO
#include <string.h>

unsigned long millis();
unsigned long micros();
/*! `delay_ms` IS THE NON-PORTABLE SPELLING, DESPITE LOOKING LIKE THE PORTABLE ONE.
    It is declared HERE and only here -- inside `#if LL_ARDUINO`, LambLisp's own backend, which is
    what POSIX, WASM and Windows use.  A real ESP32 builds LL_ESP_ARDUINO (Espressif's core) instead and
    has no such name.  So a call to delay_ms() from code SHARED between host and MCU compiles
    cleanly on the host and breaks every firmware target -- which is exactly what 72eba89 did to the
    whole fleet for an hour, from inside `#if LL_WIFI || LL_POSIX_NET`, a guard true on both.

    USE `delay()` IN SHARED CODE.  It exists on both backends: Arduino.h provides it under
    LL_ESP_ARDUINO, and the forwarder just below provides it under LL_ARDUINO.  The portable-looking
    name is the trap and the plain one is the safe choice -- the reverse of the usual instinct.

    A host-only build CANNOT catch this.  `w3 make one esp32s3-n8r2` catches it in ~22 seconds, and
    nothing in the pipeline compiles a firmware target on commit. */
void delay_ms(unsigned long ms);

static void delay(unsigned long ms) { delay_ms(ms); }

//! LLArduino GPIO surface, implemented by a BACKEND -- the null backend simulates it in
//! memory; a libgpiod backend drives real pins on a Pi or Jetson.  These are the eight functions
//! ll_xmop3_CommonIO.cpp actually calls; without them `commonio` cannot compile off-Arduino, which
//! is why 26 Scheme procedures were ESP32-only and untestable without a board.
//! Arduino spells the constants as macros, so match that rather than inventing an enum.
#ifndef HIGH
#define HIGH          1
#define LOW           0
#endif
#ifndef INPUT
#define INPUT         0x0
#define OUTPUT        0x1
#define INPUT_PULLUP  0x2
#endif

void pinMode(int pin, int mode);
void digitalWrite(int pin, int val);
int  digitalRead(int pin);
void analogWrite(int pin, int val);
int  analogRead(int pin);
void tone(int pin, unsigned int freq);
void tone(int pin, unsigned int freq, unsigned long dur);
void noTone(int pin);
void delayMicroseconds(unsigned long us);

//! Backend inspection -- lets a test assert on state the Arduino API cannot report (mode, duty,
//! tone) and name which backend answered.  See ll_llarduino_null.cpp.
int         ll_llarduino_pin_mode(int pin);
int         ll_llarduino_pin_duty(int pin);
int         ll_llarduino_pin_tone(int pin);
int         ll_llarduino_npins();
const char *ll_llarduino_backend();

typedef uint8_t byte;

/*! A small owning string.  [B834] AMORTISED GROWTH -- it used to reallocate and full-copy on EVERY
    append, which made the REPL's per-character input accumulation quadratic.

    WHAT IT COST, measured before the change.  `ll_vm_term.cpp`'s `commandLine += c` runs once per
    RECEIVED CHARACTER, and `append` did `strlen` + `new char[n0+n1+1]` + copy-all + `delete[]` --
    so every byte of an input line cost a heap allocation and a full copy, O(n^2) per line.  A
    150-byte LLIP frame meant ~150 allocations and ~11,000 byte copies on a 240 MHz part.  That is
    the ~100 B/s ceiling `_w3_llip_stage_tests` derives its timeouts from (`r7rs-tests.scm`: 904.7 s
    for 91,848 B) on a line whose wire rate is ~11.5 KB/s -- under 1% of the link.  `push_file`'s
    comment named the symptom ("O(n) String rebuild per char") without naming this as the cause.

    NOW: a stored length and capacity, geometric growth, in-place append.  `+=` is amortised O(1)
    and `length()` is O(1) instead of a `strlen`.

    MEASURED, same benchmark against both classes (10 correctness arms + a growth curve):
        20,000 single-char appends    old 201 ms    new 0.13 ms     ~1,500x
        10x the work                  old 41x time  new 13x time    (quadratic -> near-linear)
    The old timing was first measured as 0.17 ms, which is 8.5 ns per append and impossible for a
    heap allocation plus a full copy -- the harness had compiled the NEW class twice, because a
    quoted `#include` searches the including file's own directory first.  **An impossible number is
    the only reason that was caught**, so the arm now verifies which class it built before trusting
    the figure.

    THREE THINGS THAT MADE THIS NOT A ONE-LINE CHANGE, each checked rather than assumed:

    1. ALIASING -- AND THE OLD CODE WAS NOT SAFE HERE, WHICH I ASSUMED BEFORE TESTING IT.  `append`
       WAS accidentally safe (it allocated a fresh buffer, then read the old one, then freed).  But
       `set` did `delete[] ptr` FIRST and copied afterwards, so a source inside our own buffer was
       read AFTER being freed.  Two arms fail against the old class, both use-after-free:
           a.set(a.c_str() + 6, 5)   -> ''        (self-set from own buffer)
           a = a                     -> '\xefq'   (operator= forwards to set(other.ptr))
       Both now pass: `set` detects a source within `[ptr, ptr+len]` and memmoves within the buffer
       without reallocating (no growth can be needed, since n <= len), `append` re-derives the
       source after growth, and `operator=` short-circuits on `this == &other`.
       So this change fixes two latent use-after-frees as well as the quadratic cost.
    2. `operator[]` RETURNS `char&`, so a caller writing `'\0'` into the middle used to truncate the
       string -- `length()` was a `strlen` and would simply see less.  With a stored length it would
       not, and the two would silently disagree.  Checked before relying on it: NO `LL_String`
       subscript assignment exists anywhere in `src/` (every hit is a raw `char` array -- `buf`,
       `_b`, `ll_lla_fmtbuf`, `line`).  If one is ever added, it must call `set()` instead.
    3. `concat` LEAKED, and still would have.  It did `char *s3 = new char[...]; ... return s3;` --
       which converts through `LL_String(const char*)`, COPIES s3 into a fresh buffer, and never
       frees s3.  Every `operator+` leaked its own result length.  Now it builds the result in place.

    A NOTE ON WHAT GOT SAFER RATHER THAN RISKIER.  `c_str()` hands out `ptr`, and a caller caching it
    across a mutation was ALWAYS reading freed memory before (the buffer was replaced every time);
    now it survives every append that does not grow.  That is strictly fewer dangling reads, not
    more -- but it also means such a misuse would now be intermittent rather than constant, so do not
    cache `c_str()` across a mutation.
*/
class LL_String {
public:

  LL_String()				{ _init();  set("");  }
  LL_String(const char *other)		{ _init();  set(other);  }
  LL_String(const char *other, int n)	{ _init();  set(other, n);  }
  LL_String(const LL_String &other)	{ _init();  set(other.ptr, other.len);  }

  ~LL_String()	{ if (ptr) { delete[] ptr;  ptr = 0; }  len = cap = 0; }

  void set(const char *s) { set(s, s ? (int) strlen(s) : 0); }

  void set(const char *s, int n) {
    if (!s || n <= 0) { _ensure(0);  ptr[0] = '\0';  len = 0;  return; }
    if (ptr && s >= ptr && s <= ptr + len) {        //!< aliases us: no growth can be needed
      const int off = (int) (s - ptr);
      memmove(ptr, ptr + off, (size_t) n);
    }
    else {
      _ensure(n);
      memmove(ptr, s, (size_t) n);
    }
    ptr[n] = '\0';
    len = n;
  }

  LL_String concat(const char *s1, const char *s2) {
    LL_String r;
    const int n1 = s1 ? (int) strlen(s1) : 0;
    const int n2 = s2 ? (int) strlen(s2) : 0;
    r._ensure(n1 + n2);
    if (n1) memmove(r.ptr,      s1, (size_t) n1);
    if (n2) memmove(r.ptr + n1, s2, (size_t) n2);
    r.ptr[n1 + n2] = '\0';
    r.len = n1 + n2;
    return r;
  }

  LL_String &append(const char *s1) {
    if (s1 && *s1) {
      const int n1 = (int) strlen(s1);
      const bool self = (ptr && s1 >= ptr && s1 <= ptr + len);
      const int off = self ? (int) (s1 - ptr) : 0;
      _ensure(len + n1);                           //!< may move ptr; preserves [0,len]
      memmove(ptr + len, self ? ptr + off : s1, (size_t) n1);
      len += n1;
      ptr[len] = '\0';
    }
    return *this;
  }

  //! Appending ONE character is the hot path (the terminal's per-char input accumulation), so it
  //! does not route through the 2-byte-cstr + strlen detour the operator used to take.
  LL_String &append(char c) {
    _ensure(len + 1);
    ptr[len++] = c;
    ptr[len] = '\0';
    return *this;
  }

  LL_String substring(int start, int len_)	{ return LL_String(&(ptr[start]), len_); }

  LL_String &operator=(const char *other)	{ set(other); return *this; }
  LL_String &operator=(const LL_String &other)	{ if (this != &other) set(other.ptr, other.len); return *this; }

  bool operator==(const char *other)		{ return strcmp(ptr, other) == 0; }
  bool operator!=(const char *other)		{ return strcmp(ptr, other) != 0; }
  bool operator==(const LL_String &other)	{ return strcmp(ptr, other.ptr) == 0; }
  bool operator!=(const LL_String &other)	{ return strcmp(ptr, other.ptr) != 0; }
  char &operator[](int ix)			{ return ptr[ix]; }

  LL_String operator+(const char *other)	{ return concat(ptr, other); }
  LL_String operator+(const LL_String &other)	{ return concat(ptr, other.ptr); }
  LL_String operator+(char c)			{ char cstr[2];  cstr[0] = c;  cstr[1] = '\0';  return concat(ptr, cstr); }

  LL_String &operator+=(const char *other)	{ return append(other); }
  LL_String &operator+=(const LL_String &other)	{ return append(other.ptr); }
  LL_String &operator+=(char c)			{ return append(c); }

  char *c_str() const	{ return ptr; }
  int length() const	{ return len; }          //!< O(1) since [B834]; was a strlen on every call

private:
  void _init() { ptr = 0;  len = 0;  cap = 0; }

  //! Make room for `need` characters plus the NUL, preserving the first `len` and the terminator.
  //! Geometric, so N appends cost O(N) allocations amortised rather than N.
  void _ensure(int need) {
    if (cap >= need + 1) return;
    int ncap = cap ? cap : 16;
    while (ncap < need + 1) ncap += ncap;        //!< double; no multiply, no overflow check needed
                                                 //!< at these sizes (lines, not megabytes)
    char *np = new char[ncap];
    if (ptr) { memmove(np, ptr, (size_t) len + 1);  delete[] ptr; }
    else     { np[0] = '\0'; }
    ptr = np;
    cap = ncap;
  }

  char *ptr;
  int   len;        //!< characters before the NUL
  int   cap;        //!< bytes allocated, including room for the NUL
};

typedef LL_String String;
#endif

//! @name Handy directives, macros and utility functions.
//!@{
#define NOTUSED __attribute__((__unused__))			/*!<GCC extension to suppress individual "not used" warnings. */
#define INLINE __attribute__((__inline__))			/*!<GCC extension to encourage inlining a function. */
#define NOINLINE __attribute__((__noinline__))			/*!<GCC extension to avoid inlining a function. */
#define CHECKPRINTF __attribute__((format(printf, 1, 2)))	/*!<GCC extension, turn on checking of printf() const format strings, if they are known at compile time. */
#define CHECKPRINTF_pos2 __attribute__((format(printf, 2, 3)))	/*!<GCC extension, turn on checking of printf() const format strings, if they are known at compile time. */

//! MOVED HERE, and it must stay below this point: Print uses String (typedef at
//! `typedef LL_String String` above) and CHECKPRINTF_pos2 (defined just above).  Declared any
//! earlier in this header and the compiler reports "String does not name a type" -- which is a
//! header-ordering error, not a missing class.

//! GUARDED, AND IT MUST BE.  On ESP32 the VENDOR Arduino core defines its own Print and Stream
//! (cores/esp32/Print.h, Stream.h) via Arduino.h, so declaring these unconditionally is a
//! "redefinition of class Print" build failure on every ESP32 target.  It compiled clean on the
//! hosts because there is no Arduino core there to collide with -- which is exactly why the
//! mistake was invisible until an ESP32 build ran.  When this block was moved down here to sit
//! below the String typedef it left the LL_ARDUINO guard that ends further up; it now carries its
//! own.  LLArduino PROVIDES these where the vendor core is absent, and defers to it where present.
#if LL_ARDUINO
//! LL_ARDUINO Print / Stream / Serial.  NOT STUBS, and they need not be: Print and Stream are
//! pure character I/O, and LambStdio ALREADY implements exactly that on POSIX (stdin/stdout in
//! non-canonical mode).  So this is an Arduino-SHAPED FACADE over working I/O, in the same class as
//! millis() -- genuinely implemented, not simulated.  It is what lets sketch-style code
//! (`Serial.println("hi")`) run on a host with no board, and on WebAssembly and Windows,
//! where Espressif's core cannot go at all.
//!
//! FIDELITY IS THE RISK HERE, not capability.  Arduino's Print has behaviours worth matching
//! DELIBERATELY rather than by accident, and each is a place a host test could pass while the
//! device disagrees: println() terminates CRLF ("\r\n") not "\n"; write() returns a BYTE COUNT;
//! print(double, digits) defaults to 2 decimal places; print(int, base) supports BIN/OCT/DEC/HEX.
//! Those are exactly what success criterion 3 exists to check -- the same suite run against
//! the vendor core and against LLArduino must agree.
class Print {
public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t *buf, size_t n);
  size_t write(const char *s);
  size_t print(char c);
  size_t print(const char *s);
  size_t print(const String &s);
  size_t print(int n, int base = 10);
  size_t print(long n, int base = 10);
  size_t print(unsigned long n, int base = 10);
  size_t print(double v, int digits = 2);
  size_t println();                                  //!< CRLF, as Arduino does -- not "\n"
  size_t println(char c);
  size_t println(const char *s);
  size_t println(const String &s);
  size_t println(int n, int base = 10);
  size_t println(long n, int base = 10);
  size_t println(unsigned long n, int base = 10);
  size_t println(double v, int digits = 2);
  size_t printf(const char *fmt, ...) CHECKPRINTF_pos2;   //!< HardwareSerial::printf, used by LL itself
};

class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read()      = 0;
  virtual int peek()      = 0;
  virtual void flush()    {}
};

//! Serial over LambStdio.  peek() is implemented HERE because LambStdioClass has no peek: one byte
//! of pushback, which is all Stream promises.
class LLArduinoSerial : public Stream {
  int  _peeked;                                      //!< -1 = empty
public:
  LLArduinoSerial() : _peeked(-1) {}
  void begin(unsigned long baud = 115200);
  void end();
  int  available() override;
  int  availableForWrite();
  int  read() override;
  int  peek() override;
  void flush() override;
  size_t write(uint8_t c) override;
  using Print::write;
};

extern LLArduinoSerial Serial;


#endif // LL_ARDUINO -- Print/Stream/Serial

//! Render THROUGH whichever Print is compiled in -- LLArduino's or the vendor core's -- so Scheme
//! can assert what a device would actually emit and the SAME suite can be pointed at both.
//! Declared outside the guard above deliberately: see the note in ll_llarduino_print.cpp.
#if LL_ARDUINO || LL_ESP_ARDUINO
const char *ll_llarduino_fmt_int(long v, int base);
const char *ll_llarduino_fmt_real(double v, int digits);
const char *ll_llarduino_eol();
#endif

#define CHECKPRINTF_pos3 __attribute__((format(printf, 3, 4)))	/*!<GCC extension, turn on checking of printf() const format strings, if they are known at compile time. */
#define CHECKPRINTF_pos4 __attribute__((format(printf, 4, 5)))	/*!<GCC extension, turn on checking of printf() const format strings, if they are known at compile time. */

// On Arduino/ESP32, IRAM_ATTR and DRAM_ATTR are already defined by Arduino.h (esp_attr.h).
// On other platforms (Linux, desktop simulation) define them as empty so the same source compiles.
#if !defined(IRAM_ATTR)
#define IRAM_ATTR					/*!<Place function/data in fast on-chip IRAM (ESP32: ~1-cycle fetch). No-op on other platforms. */
#endif
#if !defined(DRAM_ATTR)
#define DRAM_ATTR					/*!<Place data in on-chip DRAM (ESP32: avoids PSRAM latency). No-op on other platforms. */
#endif

//! @name LambLisp imposes a limit on the length of strings, to reduce opportunities for runaway in an embedded system.
//!@{
// [B410, B389, B396, P208] the DRAM budget behind this number
//! CONSOLE RX RING SIZE.  The ESP32 Arduino default is 256 bytes, and a REPL line longer than
//! the driver can buffer WHILE THE LOOP IS BUSY is lost mid-line -- the board echoes what arrived,
//! the expression is left unbalanced, and the reader waits forever for a closing paren that was
//! dropped by the UART.  Nothing reports it, so a healthy board looks dead.  Measured on
//! esp32s3-n8r2-00: 453 bytes evaluated, 513 did not, and the cliff moved with how busy the control
//! loop was -- which is the signature of a ring overrun rather than a fixed parser limit.
//!
//! COSTS INTERNAL DRAM, which is the scarcest thing on these parts (one bug died with 324 bytes left,
//! another starves WiFi).  2048 is chosen as the smallest size that clears every probe this project
//! actually sends by a wide margin, for +1792 bytes over the default -- NOT the largest that fits.
//! Raising it is spending from an overdrawn account; measure before you do.
//! CONSOLE BUFFERS -- THESE CANNOT GO IN PSRAM, AND THE SIZE IS A DIRECT INTERNAL-DRAM COST.
//!
//! Do not "move them to PSRAM" by raising the size past the default-malloc threshold (set at the
//! `heap_caps_malloc_extmem_enable` call in `ll_platform_ESP32.cpp` -- not the sdkconfig value,
//! which that call overrides).  That threshold governs capability-less malloc() only; the UART
//! driver does not use one.
//! `esp_driver_uart/src/uart.c` allocates BOTH rings with an explicit capability:
//!
//!     #define UART_MALLOC_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
//!     uart_obj->rx_ring_buf = xRingbufferCreateWithCaps(rx_buffer_size, ..., UART_MALLOC_CAPS);
//!     uart_obj->tx_ring_buf = xRingbufferCreateWithCaps(tx_buffer_size, ..., UART_MALLOC_CAPS);
//!
//! So every byte of these buffers is internal DRAM at ANY size, and raising the number spends from
//! the account WiFi.begin() draws on.  Measured on the WROVER, same binary, one variable: a 2048
//! ring boot-looped the board with the bench overlay staged (7 aborts, 7 reboots) while 256 booted
//! clean, and the internal free difference was ~1992 bytes -- which is itself the proof they are
//! internal, since a PSRAM-backed ring would have shown no difference at all.
//!
//! A long REPL line silently dropped is therefore NOT fixable by enlarging this.  The fix is
//! to drain promptly into a buffer WE allocate in PSRAM; the driver ring then only has
//! to cover drain latency rather than a whole ~1300-byte LLIP frame.  Until that exists, the ring
//! stays at the framework default and the OVERFLOW IS REPORTED instead of being silent, which costs
//! nothing and is the half of that fix that was actually worth having.
#ifndef LL_STDIO_RX_BUFSIZE
#define LL_STDIO_RX_BUFSIZE 0            //!< 0 = leave the framework default; see above
#endif

const unsigned long toString_MAX_LENGTH = 8192;		//!<Global limit on Lamb-generated strings.
//! How much of an offending form an error message may quote back.  Well under
//! toString_MAX_LENGTH so the surrounding message text cannot push the total over it.
const unsigned long LL_ERRFORM_MAX = 400;
//! How many bindings of an ALIST environment frame a printed environment may expand before
//! it collapses to `#frame(N+)`.  An svec frame has always printed compactly; an alist frame did
//! not, and each of its values may be a closure that re-captures the same environment -- so
//! expanding one expands the program.  Measured on a chibi r7rs-tests run: single calls asking for
//! 13.7 MB, 14.8 MB and 31.8 MB of environment text, inside DIAGNOSTICS.  Chosen generously so
//! ordinary frames print in full and only unreadable ones are bounded.
const unsigned long LL_ENVFRAME_MAX = 32;
//! How many ELEMENTS of a list the printer may emit before it elides with " ...".
//!
//! THIS IS A LENGTH CAP AND MUST NOT BE THE DEPTH CAP.  The cdr spine is walked as a LOOP, so a
//! proper list costs no recursion and no print depth -- but the loop used `max_depth` as its
//! element budget, and `max_depth` defaults to 10 (LambLisp.h).  So an ELEVEN-element flat list
//! printed as `(12 11 10 9 8 7 6 5 4 3 ...)` and the runtime reported it as "datum deeper than the
//! print limit", which it was not: it was eleven items long.  Reported from the demo-09-16 as
//! loop-stats being truncated, and it silently shortened every `Output:` line and every `~a` in a
//! log message.  Depth measures NESTING; this measures LENGTH; conflating them makes a list of
//! atoms indistinguishable from a structure nested past the stack backstop.
//!
//! IT IS ALSO THE CYCLE BACKSTOP, but only for callers that pass no DatumLabels table: with labels,
//! a cyclic or shared tail is detected exactly and printed as `#N#`.  Environments do NOT rely on
//! this -- they contain themselves by construction and have their own cap above.  Chosen large
//! enough that real data prints in full; a message that long is then bounded by
//! toString_MAX_LENGTH anyway.
const unsigned long LL_PRINT_LIST_MAX = 4096;
String toString(const char *fmt, ...) CHECKPRINTF;	//!<Produce a new string from the format and arguments, respecting the global limit on string length.
//!@}


#define ME(_me_) NOTUSED const char me[] = _me_			/*!<Declare an identifier "const char me[]" without causing "unused variable" warnings and/or code clutter to suppress them. */
#define ll_isdef(sym) (#sym[0])					//!<Determine (cheaply) at runtime if a preprocessor symbol is defined.

void global_printf(const char *fmt, ...);		//!<This function enforces the limit on generated strings.

/*! Start the console spill reservoir.  Call once, LATE in boot.

  A FREE FUNCTION FOR THE SAME REASON global_printf IS ONE: the terminal is a VM internal, and
  `ll_vm_*` is never shipped to a customer.  `src/main.cpp` IS shipped -- it is the entry point a
  customer edits -- and it called `ll_term.spill_begin()` directly, which dragged `ll_vm_term.h`
  into a customer build that does not have it.  The package therefore could not compile at all
  the first `pio run` in QUICKSTART died on the missing header.

  WHY main.cpp CALLS THIS AT ALL, rather than the VM doing it for itself: the ORDERING is the
  point, and it belongs to whoever owns boot.  The reservoir must be claimed AFTER setup.scm has
  loaded and after WiFi's radio buffers and LittleFS's DMA have taken their internal DRAM -- doing
  it earlier boot-looped the WROVER on a 2048-byte console ring.  The reservoir
  is PSRAM, so it costs no internal DRAM; a board without PSRAM degrades to the driver ring.
*/
void global_spill_begin();

/*! Silence the line editor's echo and/or the VM's log writes.  Both default to off.

  SAME REASON global_spill_begin EXISTS -- see above.  `ll_wasm_browser.cpp` is the browser entry
  point, it is NOT an `ll_vm_*` file, and so it SHIPS; it set `ll_term.quiet_echo` and
  `ll_term.quiet_logs` directly, which dragged `ll_vm_term.h` into a package that does not have it
  Its include sat inside `#if LL_EMSCRIPTEN`, so it was inert in every package cut so far
  and would have broken the first `wasm32_browser` one -- a guard hides the dependency on the
  targets that skip the file, it does not remove it.

  WHY A SETTER AND NOT TWO EXPOSED FLAGS: a flag in a shipped header is a promise about the
  terminal's layout, and the class is a VM internal that moves.  A hand-declared copy of it would
  link and then disagree about offsets, which is the one failure mode worse than a missing header
  -- it does not fail, it corrupts. */
void global_term_quiet(bool echo, bool logs);

/*! Hand the line editor a complete input line, as if it had been typed.

  The browser cannot block in a read, so the input path is INVERTED: JavaScript pushes a line and
  then runs one `loop()`.  See `LL_Term::push_line()` for why that is preferred to making the
  reader asynchronous. */
//! GUARDED, AND THE DECLARATION TOO, NOT JUST THE DEFINITION.  `LL_Term::push_line` itself exists
//! only under `#if LL_EMSCRIPTEN`, so there is nothing to forward to elsewhere.  Declaring it
//! unconditionally would compile a non-wasm caller cleanly and fail at LINK -- the failure mode
//! this tree keeps paying for; guarding the declaration makes a wrong caller fail where it is
//! written instead.
#if LL_EMSCRIPTEN
void global_term_push_line(const char *s);
#endif

//!@}

/*! @name These primitive types are shared by LambLisp and the underlying VM.

  Sizing requirements for LambLisp Cells:

  | Each cell contains 3 fields: tag, car, cdr.                                     |
  | The fields are equal in size and sequential in memory.                          |
  | The bytes within each LambLisp Cell are individually addressable.               |
  | Each field can hold a generic computer "word", a signed integer, or an address. |
  | An integer fills the car field, and may also fill the cdr field.                |
  | A real number also fills the car field, and may also fill the cdr field.        |

  Since the beginning of time (Jan 1 1970) the specific organization of these have been platform-dependent.
  There is a (mostly) obvious correspondence between the shared type name and the underlying C++ type.

  Note the difference between *Charst_t* and *CharVec_t*; one is mutable, the other not.  The immutable version may go away.
  The same applies to *Bytest_t* and *ByteVec_t*.
*/
//!@{

typedef unsigned char Byte_t;	//!<Universally known byte type.
typedef bool Bool_t;		//!<Boolean type.
//! [B855] `char` IS UNSIGNED ON EVERY TARGET WE SHIP EXCEPT linux_x86_64 -- MEASURED, NOT ASSUMED:
//! xtensa-esp32/s2/s3, riscv32-esp (the C5) and aarch64-linux-gnu all define `__CHAR_UNSIGNED__`;
//! host g++ does not, and nothing in our builds passes `-fsigned-char`.  So NEVER store a reader's
//! return value in a Char_t: `LL_Port::read_char()` returns LL_int32 and signals end of input with
//! CHAR_EOF (-1), and `Char_t ch = src.read_char()` truncates that to 0xFF, which promotes back to
//! 255 where char is unsigned and to -1 where it is signed.  The EOF test then silently fails on
//! every board and passes on the development host -- which is how this survived: the arm that would
//! have shown it is the only platform that cannot.  Hold the value in LL_int32, test for CHAR_EOF,
//! and narrow to Char_t only after.  See the matching note at CHAR_EOF in ll_vm_port.h.
typedef char Char_t;		//!<Character type.
typedef int32_t ll_codepoint_t;  //!< Unicode scalar value (U+0000 – U+10FFFF)

typedef Char_t const *Charst_t;		//!<Pointer to immutable character array.
typedef Byte_t const *Bytest_t;		//!<Pointer to immutable byte array
typedef Char_t *CharVec_t;		//!<Pointer to mutable char array
typedef Byte_t *ByteVec_t;		//!<Pointer to mutable byte array

/*! @name Payload allocator -- bulk VM payloads belong in PSRAM, not internal DRAM.
  String/symbol characters, vector and bytevector bodies, bignum digits and compiled bytecode are
  PURE DATA: the VM never DMAs out of them.  They must NOT come from plain new/malloc on an ESP32,
  because the IDF only diverts allocations LARGER than the default-malloc threshold to PSRAM (set
  at the `heap_caps_malloc_extmem_enable` call in `ll_platform_ESP32.cpp`, which overrides the
  sdkconfig's SPIRAM_MALLOC_ALWAYSINTERNAL -- read that block, do not restate the number) -- so
  the many SMALL payloads a load chain creates (every symbol name, every string literal) all land
  in INTERNAL DRAM.  Internal DRAM is also the only memory a flash read can bounce through, so
  draining it makes every LittleFS read fail with ESP_ERR_NO_MEM (err 257) -- the
  setup.scm load chain consumed ~115 KB of internal DRAM and left 1176 bytes, while 1.7 MB of
  PSRAM sat unused.  Allocating these payloads from PSRAM keeps internal DRAM for DMA.

  Falls back to malloc when there is no PSRAM (host builds, non-PSRAM parts) or PSRAM is full, so
  behaviour is unchanged everywhere else.  Pairs with ll_payload_free -- these blocks are freed by
  the GC sweep, so they must NEVER be released with delete[].
*/
//!@{
void *ll_payload_alloc_bytes(size_t nbytes);
void  ll_payload_free(void *p);

//! Typed convenience wrapper.  POD payloads only -- no constructors are run.
template <typename T> static inline T *ll_payload_new(size_t n)
{
  return (T *) ll_payload_alloc_bytes(n * sizeof(T));
}
//!@}

/*! @name DRAM allocator -- the exact OPPOSITE of the payload allocator above, and for the same reason.
  A few VM structures are HOT, PERMANENTLY RESIDENT and SMALL, and those
  three together are what earns internal DRAM.  Today there are exactly two:

    sint_cache_cells   6145 x sizeof(Cell) = 73,740 B, built once, gcst_marked forever, and read by
                       EVERY arithmetic result in [-2048..4096] -- i.e. most integers in most programs
    rootstack          starts at 256 entries (1 KB), pushed/popped on every gc_root_push and walked
                       WHOLE on every mark

  Both are over the `heap_caps_malloc_extmem_enable` threshold set in `ll_platform_ESP32.cpp`, so a
  plain new/malloc puts them in PSRAM.  SIZING DOES NOT STEER PLACEMENT -- only an explicit
  capability does, which is the same lesson the payload allocator above records from the other side.

  WHY THIS IS NOT THE MICROPYTHON MECHANISM, so nobody re-derives a refuted one: uPy's heap is in
  PSRAM too (`w3_ai_exch/micropython_dram_strategy.md` sec.1.3 -- MP_PLAT_ALLOC_HEAP is a plain
  malloc and its smallest area is 36 KB, four times its own threshold).  uPy is fast on small
  integers because they are TAGGED IMMEDIATES that never touch memory at all, not because they are
  in DRAM.  LambLisp boxes every integer as a Cell, so this moves the boxes to the fast side; it
  does not close that gap.

  THE PSRAM FALLBACK IS LOAD-BEARING AND MUST STAY A PREFERENCE, NOT A PIN.  If internal DRAM cannot
  satisfy the request the allocation SUCCEEDS IN PSRAM rather than failing -- a hard
  MALLOC_CAP_INTERNAL would turn a performance preference into a boot failure on a part with less
  internal SRAM, and the C5 has ~25% less than the S3.  For an autobuf the fallback needs no separate
  migration path: a grown buffer is a FRESH allocation, so a rootstack that outgrows internal DRAM
  migrates to PSRAM at its next upsize() and copies itself across.

  Every placement is printed at boot by diag_internal_report() via mem_placement(), so a silent
  fallback to PSRAM is VISIBLE rather than inferred.  That report is the acceptance evidence for
  it must be capable of printing both words -- see the proposal.
*/
//!@{
void *ll_dram_alloc_bytes(size_t nbytes);
void  ll_dram_free(void *p);
//!@}

//! ONE definition, not three per-platform ones, and spelled in the <stdint.h> idiom
//! so it states its ACTUAL requirement instead of satisfying it by accident.
//!
//! Word_t HOLDS POINTERS -- `rplacd((Word_t) val)`, the whole Cell payload -- so the requirement is
//! "wide enough for a pointer", which is exactly what uintptr_t means.  It was previously declared
//! per-platform as `unsigned long` (LL_AMD64, LL_ARM64) and `unsigned` (LL_ESP32).  Both were
//! CORRECT BY ACCIDENT rather than by construction: LP64 Linux and ILP32 Xtensa happen to make
//! those the same size as a pointer.  Windows x64 is LLP64 -- `long` is 32 bits while pointers are
//! 64 -- so the LL_AMD64 spelling TRUNCATES EVERY POINTER there, silently, at compile time, with
//! the first symptom being a GC walking garbage.  uintptr_t is 4 bytes on the ESP32 and 8 on every
//! 64-bit host, so one line is right on all of them and a fourth platform cannot reintroduce the
//! bug by copying the wrong branch.
//!
//! THE STATIC ASSERT IS THE POINT, not decoration.  Nothing else in the tree checks this property;
//! it is the only mechanism that catches a width regression at build time rather than in a
//! debugger.  Do not remove it, and do not replace uintptr_t with a fixed width (uint64_t would
//! waste half of every cell on the ESP32; uint32_t would truncate on a host).
//!
//! Related, and the reason the idiom matters beyond Windows: LL_int32 is int32_t, and the ESP-IDF 5
//! xtensa toolchain changed int32_t from `int` to `long` -- which silently made every
//! ascii.dec(someInt) call ambiguous (see the dec(int) forwarder below).  A type that NAMES its
//! width does not move under a toolchain upgrade; a type spelled `long` does.
typedef uintptr_t Word_t;	//!<A generic computer word, used only for setting and retrieval -- WIDE ENOUGH FOR A POINTER.  Not an *unsigned int* used for arithmetic.
typedef void *Ptr_t;		//!<Generic pointer in C++
static_assert(sizeof(Word_t) >= sizeof(void *),
              "Word_t must hold a pointer -- see the P174 note above; LLP64 (Windows x64) breaks "
              "the old `unsigned long` spelling because long is 32 bits there and pointers are 64.");

typedef int32_t  LL_int32;	//!<LambLisp 32-bit exact integer.

//! @name UTF-8 codepoint helpers
//! R7RS indexes strings by CHARACTER, not by byte.  LambLisp stores strings as NUL-terminated
//! UTF-8, so every index operation has to walk the encoding.  These are the one shared
//! implementation -- encoders were previously duplicated across several modules
//! which is how the write path came to encode real UTF-8 while the measure
//! path still counted bytes (`(string-length (list->string (list (integer->char 945))))`
//! returned 2 for a one-character string).
//!@{

//! Bytes in the UTF-8 sequence that starts with lead byte b0.  A continuation or invalid lead
//! counts as 1 so a malformed string still advances and cannot hang a scan.
inline int ll_u8_seqlen(unsigned char b0)
{
  if (b0 < 0x80) return 1;
  if ((b0 & 0xE0) == 0xC0) return 2;
  if ((b0 & 0xF0) == 0xE0) return 3;
  if ((b0 & 0xF8) == 0xF0) return 4;
  return 1;
}

//! Decode the sequence at p; *nb receives its byte length.  Invalid input yields the lead byte
//! itself, matching the reader's pass-through behaviour rather than throwing mid-scan.
inline ll_codepoint_t ll_u8_decode(const char *p, int *nb)
{
  unsigned char b0 = (unsigned char) p[0];
  int n = ll_u8_seqlen(b0);
  if (nb) *nb = n;
  if (n == 1) return (ll_codepoint_t) b0;
  ll_codepoint_t cp = b0 & (0xFF >> (n + 1));
  for (int i = 1; i < n; i++) {
    unsigned char c = (unsigned char) p[i];
    if ((c & 0xC0) != 0x80) { if (nb) *nb = 1; return (ll_codepoint_t) b0; }
    cp = (cp << 6) | (c & 0x3F);
  }
  return cp;
}

//! Encode cp into out (at least 4 bytes); returns the byte count.  No NUL is written.
inline int ll_u8_encode(ll_codepoint_t cp, char *out)
{
  if (cp < 0x80)    { out[0] = (char) cp; return 1; }
  if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));  out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                      out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  out[0] = (char)(0xF0 | (cp >> 18));         out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));  return 4;
}

//! CHARACTER count of a NUL-terminated UTF-8 string (R7RS string-length).
inline LL_int32 ll_u8_strlen(const char *s)
{
  LL_int32 n = 0;
  for (const char *p = s; *p; n++) p += ll_u8_seqlen((unsigned char) *p);
  return n;
}

//! CHARACTER count of a UTF-8 buffer of KNOWN BYTE LENGTH.  Once a string may contain a NUL,
//! ll_u8_strlen() above is the wrong tool -- it stops at the first 0 byte and would report a short
//! count for a perfectly good string.  Counting to an explicit byte length is the only form that
//! stays correct, so this is what the stored character length is computed with.
//! A truncated final sequence (a lead byte whose continuations run past nbytes) still counts as one
//! character and the loop still terminates: seqlen never returns 0.
inline LL_int32 ll_u8_strlen_n(const char *s, LL_int32 nbytes)
{
  LL_int32 n = 0;
  for (LL_int32 i = 0; i < nbytes; n++) i += ll_u8_seqlen((unsigned char) s[i]);
  return n;
}

//! Byte pointer to character index k within a buffer of KNOWN BYTE LENGTH; 0 if k is past the end.
//! The NUL-terminated ll_u8_index() below cannot be used on a string that may contain a NUL.
inline const char *ll_u8_index_n(const char *s, LL_int32 nbytes, LL_int32 k)
{
  LL_int32 i = 0;
  for (LL_int32 c = 0; c < k; c++) {
    if (i >= nbytes) return 0;
    i += ll_u8_seqlen((unsigned char) s[i]);
  }
  return (i <= nbytes) ? (s + i) : 0;
}

//! Byte pointer to character index k, or NULL if k is past the end.  k == length returns the
//! terminator, so a caller can use it as an exclusive range end.
inline const char *ll_u8_index(const char *s, LL_int32 k)
{
  const char *p = s;
  for (LL_int32 i = 0; i < k; i++) {
    if (!*p) return 0;
    p += ll_u8_seqlen((unsigned char) *p);
  }
  return p;
}
//!@}

typedef int64_t  LL_int64;	//!<LambLisp 64-bit exact integer.
typedef float   LL_float32;	//!<LambLisp IEEE 754 single-precision (32-bit).
typedef double  LL_float64;	//!<LambLisp IEEE 754 double-precision (64-bit).

static_assert(sizeof(Word_t)    == sizeof(Ptr_t),  "Word_t size must be == Ptr_t size\n");
static_assert(sizeof(Word_t)    >= sizeof(LL_int32), "Word_t size must be >= LL_int32 size\n");
static_assert(sizeof(LL_float32) == 4,              "LL_float32 must be 4 bytes\n");
static_assert(sizeof(LL_float64) == 8,              "LL_float64 must be 8 bytes\n");
//!@}

//The AsciiConverter class mitigates printf formatting problems caused by different int sizes on different processors, when using printf-style format string (%u, %d %lu %ld etc).
class AsciiConverter {
public:

  char *dec(Word_t n) {                       //!< PRIMITIVE: takes a slot
    char *ptr = slot_end();

    *ptr = '\0';
    do {
      Word_t d = n % 10;
      n /= 10;
      char c = d + '0';
      *--ptr = c;
    } while (n);
    return ptr;
  }

  char *dec(LL_int32 n) {
    if (n >= 0) return dec((Word_t) n);
    if (n == INT32_MIN) return dec((LL_int64) n);   //!< -INT32_MIN overflows int32; int64 path prints it in full
    char *ptr = dec(-n);
    *--ptr = '-';
    return ptr;
  }

  //! dec(int) -- EXISTS ONLY WHERE int32_t IS NOT int, AND THAT VARIES BY TOOLCHAIN.
  //! The xtensa toolchain shipped with ESP-IDF 5 typedefs int32_t as `long`; the one with IDF 4.4
  //! used `int`.  With int32_t == long the overload set below is {unsigned, long, long long,
  //! float} and a plain `int` argument converts to the first three at IDENTICAL rank, so every
  //! ascii.dec(someInt) in the tree stops compiling at once with
  //!     error: call of overloaded 'dec(int&)' is ambiguous
  //! -- which points at the CALL rather than at the typedef that moved, and looks like the call
  //! site is wrong when nothing about it changed.  Under IDF 4.4 int32_t == int made dec(LL_int32)
  //! an exact match, so the ambiguity simply could not arise and the calls were always fine.
  //! Adding this forwarder fixes every call site at once instead of casting at each one.
  //! THE enable_if IS LOAD-BEARING, NOT DECORATION: on hosts where int32_t IS int (x86-64 Linux,
  //! the IDF 4.4 toolchain) this would be a redeclaration of dec(LL_int32) and would not compile,
  //! so it must vanish there rather than be #ifdef'd on a chip or an IDF version -- the condition
  //! is a property of the TYPEDEF, and only the compiler knows it.
  template <typename T>
  typename std::enable_if<std::is_same<T, int>::value && !std::is_same<LL_int32, int>::value,
                          char *>::type
  dec(T n) { return dec((LL_int32) n); }

  char *dec(LL_int64 n) {
    // Full 64-bit signed decimal; avoids conflict with dec(Word_t) on 64-bit platforms.
    char *ptr = slot_end();                   //!< PRIMITIVE: takes a slot
    bool neg = (n < 0);
    uint64_t uv = neg ? (n == INT64_MIN ? (uint64_t) 9223372036854775808ULL : (uint64_t) -n)
                      : (uint64_t) n;
    do { *--ptr = (char)('0' + (uv % 10)); uv /= 10; } while (uv);
    if (neg) *--ptr = '-';
    return ptr;
  }

  char *hex(Word_t n) {
    char *ptr  = slot_end();                  //!< PRIMITIVE: takes a slot
    LL_int32 nibs = sizeof(Word_t) * 2;

    *ptr = '\0';
    while (nibs--) {
      Word_t d = n & 0x0f;
      n >>= 4;
      char c = (d < 10) ? (d + '0') : (d - 10 + 'a');
      *--ptr = c;
    }
    return ptr;

  }

  char *dec(LL_float32 n) {
    char *b = slot_base();                    //!< PRIMITIVE: takes a slot
    snprintf(b, SLOT, "%f", (LL_float64) n);
    return b;
  }

private:
  // [P238 Ph1]
  /*! A RING OF SLOTS, SO ONE `ascii` CAN FORMAT SEVERAL INTEGERS IN ONE FORMAT STRING.
    There used to be a single `char buffer[128]`, so two `ascii.dec()` calls in one `printf` both
    returned pointers into it and the second overwrote the first -- silently, and with unspecified
    argument evaluation order, so the output was a plausible WRONG NUMBER rather than a crash.  The
    workaround was 76 local `AsciiConverter` declarations across `src/`, each costing 128 bytes of
    C STACK on `mk_error`/`throw` paths that are reached at depth.

    **ADVANCE THE RING IN THE PRIMITIVE WRITERS ONLY, NEVER IN THE WRAPPERS.**  This is the rule an
    implementer gets wrong, because "take a slot on entry" is the obvious reading and it compiles,
    runs, and passes a casual test.  `dec(LL_int32)` on a negative value calls `dec(-n)` and then
    writes the '-' INTO THE SLOT THE CALLEE TOOK, through the returned pointer; the `enable_if`
    forwarder for `int` and the INT32_MIN path delegate the same way.  So one PUBLIC call consumes
    exactly ONE slot however many internal hops it makes.  If every entry point advanced instead, a
    negative int would burn two slots and write its sign into a slot the ring had moved past --
    which still WORKS until the ring wraps, i.e. a defect that appears at the 9th call, not the
    2nd, and makes capacity data-dependent.

    SIZED BY static_assert AGAINST sizeof(Word_t), not against a quoted number: `hex()` is 16
    nibbles on a 64-bit host and 8 on the ESP32, so either literal would be wrong somewhere.  It
    matters because the writers fill BACKWARD -- an oversized value runs off the slot's START into
    the PREVIOUS slot, corrupting a value another argument still holds.

    NOT THREAD-SAFE, AND NOT NEWLY SO: `next` is shared mutable state, exactly as the single buffer
    was.  `ascii` is for the loop task; code reachable from an ISR must keep its own local.
  */
  //! 16, raised from 8 (an owner decision).  The ring is what lets ONE `ascii` format several
  //! integers in one format string; at 8, a line with more than 8 of them silently reused slots
  //! and printed DUPLICATE values, so such lines had to carry a local `AsciiConverter a[N]`
  //! instead.  The GC report is 13 integers and was one of them.  Cost is
  //! SLOTS * SLOT = 1024 bytes of static, up from 512 -- paid once, globally, and cheap against a
  //! class of silently-wrong diagnostics.
  //! `verify_truth.sh` section 28 READS THIS NUMBER out of this header rather than keeping its own
  //! copy, so raising it here moves the checker with it; do not add a second copy anywhere.
  static const LL_int32 SLOTS = 16;
  static const LL_int32 SLOT  = 64;
  static_assert(SLOT >= 3 + 20,                  "dec(LL_int64): sign + 20 digits + NUL");
  static_assert(SLOT >= 2 + sizeof(Word_t) * 2,  "hex(Word_t): nibbles + NUL");
  static_assert(SLOT >= 48,                      "dec(LL_float32): %f of 3.4e38 needs 47 chars");
  char     buf[SLOTS][SLOT];
  LL_int32 next = 0;
  char *slot_base() { next = (next + 1) % SLOTS;  return buf[next]; }
  char *slot_end()  { char *b = slot_base();  b[SLOT - 1] = '\0';  return &b[SLOT - 1]; }
};

extern AsciiConverter ascii;
  
class LambPlatform {
public:

  //! @name Interaction with the underlying runtime platform.
  //!@{
  LambPlatform() {}
  ~LambPlatform() { end(); }

  void begin(void);
  void loop(void);			//!<Perform any platform-specific activity needed at loop() time.  Call only once per loop at beginning main loop().
  void end(void);
  
  void reboot(void);
  
  const char *name();			//!<Return a pointer to a chacter array containing a description of the runtime platform.
  void identification(void);		//!<Emit a string with the complete detailed description of the platform.

  LL_int32 free_heap();
  /*! @brief Free space in the pool cell BLOCKS are actually allocated from.
      expand() used to test free_heap(), which on ESP32 is esp_get_free_heap_size() -- the AGGREGATE
      of internal DRAM and PSRAM -- and then allocated specifically from MALLOC_CAP_SPIRAM.  It could
      therefore believe there was room when the space was in the wrong pool, let the cell heap grow
      until PSRAM was full, and starve the PAYLOADS (strings, bignum digits, vectors, bytecode) that
      share that pool.  A bignum payload then got NULL and the board died with StoreProhibited.
      On non-ESP32 targets this is just free_heap(). */
  // [B508, B609, B676] why these four exist separately from free_heap()
    LL_int32 free_internal();			//!<Free INTERNAL DRAM only -- where lwIP sockets live.  free_heap() is the aggregate and cannot see this go.
  LL_int32 free_dma();				//!<Free DMA-capable memory only -- camera and peripheral buffers.
  LL_int32 largest_free_internal();		//!<Largest CONTIGUOUS internal block -- falls with FRAGMENTATION, not only with occupancy; read beside free_internal(), never instead of it.
  LL_int32 largest_free_dma();			//!<Largest CONTIGUOUS DMA-CAPABLE block -- the number that decides whether a driver's buffer can be had at all.  free_dma() is the wrong meter for that: a fragmented heap reports megabytes free and refuses a 4 KB contiguous request.
  LL_int32 cell_pool_free();			//!<Return the unused space available for LambLisp expansion.  Whether this is *total* space or *largest* space is platform-dependent.  Accuracy is specifically not guaranteed.

  // [B593] why the pool query was abstracted here
  /*! ALLOCATE AND RELEASE A CELL BLOCK.  The pool's QUERY was abstracted here while the
      pool's ALLOCATION stayed inlined behind `#if LL_ESP32` in the collector -- one concern, two
      conventions, and the inlined half maintained by hand across THREE sites (`ll_vm_mem.cpp`
      expand(), the partial-release loop, and the destructor).  The file asked the next reader
      to keep them in step in prose -- *"Mirror the destructor's free EXACTLY"*, *"THE TWO ENDS MUST
      MOVE TOGETHER"* -- which is the tell that the pairing was unenforceable.  A
      `heap_caps_malloc` released with `delete[]`, or the reverse, is undefined behaviour that
      COMPILES CLEAN ON BOTH TARGETS and corrupts the heap on one.  Routing all three through one
      pair makes the pairing unforgeable instead of documented.

      BYTES, NOT CELLS, AND void* NOT Sexpr_t, DELIBERATELY: the platform layer owns MEMORY and must
      not need to know what a Cell is -- this header is included long before the cell types exist.

      RETURNS nullptr ON FAILURE ON EVERY TARGET.  The two arms did not previously agree about this:
      `heap_caps_malloc` returns null and `new Cell[]` THROWS, so the collector's OOM path was
      reachable on the MCU and not on the hosts.  One caller cannot handle two contracts, so the
      POSIX/WASM side uses nothrow and the single caller checks the pointer. */
  void *cell_block_alloc(unsigned long nbytes);  //!<PSRAM-first where that exists; nullptr on OOM, never throws.
  void  cell_block_free(void *blk);              //!<Release a block from cell_block_alloc().  nullptr is a no-op.

  // [B593, B117]
  /*! WHICH POOL DOES THIS POINTER LIVE IN?  Returns a fixed-width label for a diagnostic
      table.  The ONLY platform-specific thing in the memory report was `esp_ptr_external_ram()`,
      and a whole function -- plus its caller -- sat behind `#if LL_ESP32` because of it.  With the
      one query behind a platform method the report itself is platform-independent and compiles
      everywhere, which is the point: a diagnostic that exists on one target is a diagnostic nobody
      reaches for on the others.  A host has one heap and honestly says so rather than claiming a
      distinction it does not have. */
  const char *mem_placement(void *p);            //!<8-char label: "PSRAM   ", "INTERNAL", or "HEAP    ".

  // [B627] the absence this records, and the write-cost measurement
  /*! FILESYSTEM SPACE, IN BYTES.  A running program could not ask how full its flash was --
      there was no accessor at all -- so it could not see a cliff coming, could not report it, and
      could not refuse.  That absence is the defect; the COST of a write is not one.
      MEASURED on an S3 devkit: a chunk round trip is 76.7 ms on a clean
      filesystem and 1,211 ms with ~82 KB of scratch present -- a ~1,134 ms DELTA that appears when
      the files are added and vanishes when they are deleted, three times.  The write still
      SUCCEEDS and its checksum matches, so nothing fails and nothing warns: a robot simply starts
      missing deadlines, and the evidence points at the collector or the scheduler.  That is the
      same effect seen from the other end -- a 27 KB push taking 276 s at an 89.6x echo ratio.
      Returns 0 where no filesystem is mounted, so a caller can tell "no FS" from "full FS". */
  LL_int64 fs_total();                           //!<Total bytes of the mounted filesystem, 0 if none.
  LL_int64 fs_used();                            //!<Bytes in use, 0 if none.  free = total - used.
  LL_int32 free_stack();			//!<Return the unused execution stack space available.  Accuracy is specifically not guaranteed.
  Bool_t heap_integrity_check(Bool_t complain=false);	//!<Run intensive heap check; print errors if found; return true if errors found.

  //!Return a real number between -1.0 and +1.0.  May include -1.0 but not +1.0.
  LL_float32 rand11() {
    const int max_int = (~((unsigned int) 0)) >> 1;
    const int min_int = -max_int - 1;
    const LL_float32 min  = (LL_float32) min_int;
    
    LL_int32 n;
    rand((byte *) &n, sizeof(n));
    return (n / min);
  }
  
  LL_float32 rand01()			{ return (rand11() + 1.0) / 2.0; }

  void rand(byte *buf, LL_int32 len);	//!<Fill a buffer with the highest-quality random numbers available on this platform.
  void rand11(LL_float32 *buf, LL_int32 n)	{ while (n--) *buf++ = rand11(); }
  void rand01(LL_float32 *buf, LL_int32 n)	{ while (n--) *buf++ = rand01(); }

  LL_int32 loop_elapsed_ms()	{ return millis() - loop_start_ms; }	//!<Return the time elapsed since the beginning of the current loop().
  LL_int32 loop_elapsed_us()	{ return micros() - loop_start_us; }	//!<Return the time elapsed since the beginning of the current loop().

  //!@}

private:
  LL_int32 loop_start_ms;
  LL_int32 loop_start_us;
};

extern LambPlatform lambPlatform;

//! @name Elide the differences between platform "files" with this typedef.
//!@{
#if LL_LITTLEFS
#include "LittleFS.h"
typedef File File_Native;
#endif

#if LL_POSIX
#include <stdio.h>
typedef FILE* File_Native;
#endif
//!@}

/*! \class LL_File
  
  The *file* type is ultimately provided by the underlying operating system, not by LambLisp.
  This class elides the differences between file types on different platforms, providing a POSIX-like interface.

  Note that there is no *open* operation on files.
  A file is opened by the *file system* and then a *file* is returned.
  After a *file* is closed, the same file object cannot be opened again; instead a new file must be requested from the *file system*.
*/
//!@{
class LL_File {
public:

  LL_File();
  ~LL_File();

  bool isOpen() { return _path != ""; }
  int read(void);
  int write(byte b);
  int seek(unsigned long target, int whence=SEEK_SET);
  int tell();
  int size();
  int close();

  int read(byte *b, int n) {
    LL_int32 nread = 0;
    while (n--) {
      int ch = read();
      if (ch == EOF) return nread;
      b[nread++] = ch;
    }
    return nread;
  }
  
  int read(char *s, int n) { return read((byte *) s, n); }  
  int write(const byte *b, int n) { while (n--) write(*b++); return n; }
  int write(const char *s, int n) { while (n--) write((byte) *s++); return n; }
  int peek();
  
  File_Native _theFile;
  String _path;
  String _mode;

private:

};
//!@}

// [P211 Ph1] and [B396], [B88], [B60], [B428], [B411], [B662] for the claims that collided
/*! @name Fast-RAM reservations -- claim internal DRAM at install time, hand it to the consumer.

  WHY THIS EXISTS.  Internal DRAM is claimed first-come-first-get, and the largest single claimant
  on an ESP32 board makes its claim from INSIDE the `.scm` load phase: one form
  `(when (positive? (setting 'wifi 0)) (WiFi.begin ...))` in `WiFi.scm` costs ~48,752 bytes and
  collapses the largest contiguous block from 69,620 to 31,732.  Every claimant that ran before it
  budgeted against a number that was about to be wrong by 48 KB.  Nothing orders the claims; this
  does.

  THE PATTERN:

    1. RESERVE at install time, when the pool is still whole and contiguous.
    2. RELEASE immediately before the real consumer allocates, so the consumer lands in the hole.
    3. RELEASE UNCLAIMED at the end of boot, so a board that never uses the subsystem pays nothing.

  Step 3 is what makes this "if it is to be used, allocation first; otherwise not" rather than a
  tax: without it every board would pay for WiFi whether or not it ever starts a radio, and two
  boards in this fleet boot ONLY because they do not.

  BEST-EFFORT BY DESIGN.  A reservation that cannot be met is NOT an error -- the consumer simply
  allocates as it would have.  A WROVER finishes boot with ~3,364 bytes free, so a hard failure
  here would brick the very boards this helps.  **Every outcome is REPORTED**, success included: an
  unreported success is as useless as an unreported failure when you are reading a ledger
  afterwards.
*/
//!@{
//! Reserve `bytes` of internal DRAM under `tag`.  Returns true if held.  Reports either way.
bool ll_fastram_reserve(const char *tag, size_t bytes);

/*! Take the radio's boot-time fast-RAM reservation.  Called from `Lamb::setup()` BEFORE the VM
    claims its internal-DRAM residents, so the reserve is made while a contiguous block that size
    still exists.  No-op where there is no radio or the part is not an ESP32. */
void ll_wifi_boot_reserve();

/*! Reserve for a REQUIRED consumer -- identical to `ll_fastram_reserve` except that
    `ll_fastram_release_unclaimed()` will NOT sweep it.

    WHY A SECOND FLAVOUR, AND WHY IT IS NOT A LOOPHOLE.  Step 3 exists so a board that never
    starts a subsystem pays nothing for it -- the reservation is SPECULATIVE, and the sweep is what
    makes speculation free.  Some consumers are not speculative: on a board compiled WITH a panel,
    the panel's memory is as required as the radio's, and whether it is "started" is not in doubt.

    AND THE SWEEP ACTIVELY BREAKS THOSE.  A panel that starts from the app-loop's FIRST TICK starts
    AFTER `ll_fastram_release_unclaimed()` has run at the end of boot -- so its reservations are
    swept as "subsystem never started" moments before the subsystem starts, and the display then
    allocates from whatever is left.  The reservation was correct, released at the correct moment,
    and useless, because the SWEEP ran between the two.

    So the test is not "did it start yet" but "can this board ever not need it", and that is a
    COMPILE-TIME question.  A pinned reservation is only ever taken under the board's own `#if`. */
bool ll_fastram_reserve_pinned(const char *tag, size_t bytes);

//! Hand the reservation back so the real consumer can take the hole.  Safe if never reserved.
void ll_fastram_release(const char *tag);

//! Release every reservation still held -- call once boot is past the point anyone would claim.
void ll_fastram_release_unclaimed();
//!@}

/*! @name Windows host services
    Three questions whose POSIX spelling does not exist on a Windows host, asked through names of
    ours so that no general LambLisp file names a Win32 API.  The implementations live in
    `ll_platform_Windows.cpp`, which is the only file that includes `<windows.h>` -- exactly one
    file has to get that include order right, instead of every file that ever needs a Windows API.
    @{ */

// Declarations lived in ll_platform_Windows.h until 2026-10-06; the implementation file is
// unchanged and is still the only place Win32 is named.  [P264] [P174]
#if LL_WINDOWS

/*! Write the running executable's own path into `buf` (NUL-terminated); return its length, or 0.

    Replaces `readlink("/proc/self/exe", ...)`, which is Linux-only -- it is not even POSIX.  Both
    spellings answer the same question and neither is portable, which is why the caller asks through
    a name of ours rather than through either platform's.

    Returning 0 is not fatal to the caller: `posix_find_data_dir()` then falls back to the relative
    string "data".  Know what that looks like, because it does not look like a path bug -- the VM
    starts normally and then reports a pile of missing `.scm` files, which reads as a broken
    filesystem image. */
size_t ll_win_self_exe_path(char *buf, size_t cap);

/*! mkdir() with the POSIX two-argument shape, ignoring `mode`.

    MinGW's `::mkdir` is the MSVC one-argument form -- there is no mode argument, because Windows
    ACLs are not POSIX permission bits.  Passing 0777 does not compile ("too many arguments to
    function 'int mkdir(const char*)'"), which is the good outcome; the bad one would have been a
    silent permission difference.  `mode` is accepted and dropped so the call site reads the same on
    every target. */
int ll_win_mkdir(const char *path, int mode);

/*! Set the system clock from a UTC epoch-seconds value; 0 on success, -1 on failure.

    MinGW has `gettimeofday` and NOT `settimeofday`, so an unguarded call there is a COMPILE error
    rather than a runtime one -- and the same hole exists one platform over, where Emscripten lacks
    the setter too and only the WASM arm was guarded.

    Windows CAN set its clock, given `SE_SYSTEMTIME_NAME` privilege, so this is a real
    implementation and not a stub: reporting "cannot" on a platform that can would be the same lie
    in the other direction.  Without the privilege `SetSystemTime` fails and this returns -1, which
    reaches Scheme as `#f` -- the documented answer on any host that refuses, exactly as on Linux
    without `CAP_SYS_TIME`.  **Never report success here without checking:** certificate validation
    is done against this clock, so a caller told the time was set when it was not is worse off than
    one told plainly. */
int ll_win_set_system_time(long long utc_seconds);

#endif // LL_WINDOWS

/*! @} */

/*! @name Panel (display) interface
    A panel is a CONFIGURATION PLUS FIVE OPERATIONS, supplied by the board.

    The abstraction does not span "two controllers", it spans two meanings of a frame
    update, and a caller must branch on `model` rather than on the name:

      - `LL_PANEL_PUSH` (SPI/QSPI -- ST7789, CO5300): pixels are written over a bus when
        something changes, so an idle screen costs nothing.  `blit` is the operation and
        `fb` is NULL.
      - `LL_PANEL_SCAN` (16-bit parallel RGB): a peripheral reads a framebuffer forever at
        the pixel clock, so an idle screen costs FULL REFRESH BANDWIDTH.  There is no bus to
        write to -- an update is a store into `fb()`, which the hardware finds on its next
        scan.  `blit` is NULL.

    Every pointer except `init` may be NULL, and a NULL operation is one this panel
    genuinely CANNOT perform -- never a no-op.  `set_brightness` is the case to understand:
    on the S3-EYE it is LEDC PWM on an active-low pin, on an AMOLED it is a panel register
    write with no backlight pin at all, and on some boards there is no control whatsoever.
    A `#define` cannot express that, and a copied driver yields a function that exists, is
    callable, and does nothing.  NULL means "this panel cannot", which a caller can report.
    @{ */

// The interface lived in ll_panel.h until 2026-10-06; every LambLisp element now takes it
// from the generic platform.  It was extracted from the camera driver before that.  [P264] [P231]
class Lamb;   // the operations below take `Lamb &`; function pointers need only an incomplete type

/*! LL_PANEL_ST7789_EYE -- the ESP32-S3-EYE's built-in ST7789 240x240 on SPI3.
    Turned on by the `panel_st7789_eye` feature in `w3_pio/boards.json`, NOT by this header:
    the `#ifndef` supplies only the OFF default, and a gate that no build defines is dead
    code that still compiles.  If you move where it is set, change this line in the same
    commit.  Deriving a panel from "is this an S3" would put GPIO 43/44/47/48 on every S3
    build the moment a second S3 camera board existed, so the gate is named for the board it
    actually describes rather than for the chip. */
#ifndef LL_PANEL_ST7789_EYE
  #define LL_PANEL_ST7789_EYE 0   // was documented as env-set while nothing set it [B557]
#endif

/*! LL_PANEL_ST7701_AITRIP -- the AITRIP / Guition ESP32-4848S040's ST7701S 480x480 on the
    S3's 16-bit parallel RGB peripheral.  The SCAN half of the abstraction above.  Turned on
    by `[env:esp32s3-aitrip-480x480]` from `w3_pio/boards.json`, for the reason given on the
    gate above.  A grep for the symbol across `platformio.ini` and `w3_pio/boards.json` is
    the whole check; `verify_truth.sh` section 26 now does it for EVERY `LL_*` gate. */
#ifndef LL_PANEL_ST7701_AITRIP
  #define LL_PANEL_ST7701_AITRIP 0
#endif

/*! LL_PANEL -- does THIS build have any panel at all?  Derived, never set by hand.
    It is here rather than in a .cpp because the CAMERA driver must also ask it, to decide
    whether `camera->lcd` exists, and a gate that two files must agree on has exactly one
    home. */
#if LL_PANEL_ST7789_EYE || LL_PANEL_ST7701_AITRIP
  #define LL_PANEL 1
#else
  #define LL_PANEL 0
#endif

//! Which of the two display models this panel is.  Decides whether `blit` or `fb` is live.
enum LL_PanelModel {
  LL_PANEL_PUSH = 0,   //!< bus-attached: `blit` sends pixels.  `fb` is NULL.
  LL_PANEL_SCAN = 1    //!< hardware scans a framebuffer: `fb` returns it.  `blit` is NULL.
};

//! A board's panel: geometry, model, and the five operations.  See the note above on NULL.
struct LL_Panel {
  const char   *name;          //!< "st7789-240x240-spi3", "co5300-410x502-qspi", "rgb565-480x480-par16"
  int           w, h;          //!< panel geometry in pixels
  LL_PanelModel model;         //!< PUSH or SCAN -- branch on this, not on the name
  bool        (*init)(Lamb &lamb);                                                  //!< bring up bus + controller; idempotent
  void        (*fill)(Lamb &lamb, int x, int y, int w, int h, uint16_t color);       //!< filled rect, RGB565
  void        (*blit)(Lamb &lamb, int x, int y, int w, int h, const uint16_t *src);  //!< PUSH only; NULL on SCAN
  uint16_t   *(*fb)(void);                                                          //!< SCAN only; NULL on PUSH
  void        (*set_brightness)(Lamb &lamb, int percent);                           //!< 0..100, or NULL if the panel has none
  void        (*on_off)(Lamb &lamb, bool on);                                       //!< display on/off, or NULL
};

//! The panel this board registered, or NULL when the board has none.
const LL_Panel *ll_panel_active(void);

//! Register the board's panel.  Called from the board's own section of ll_xmop3_Panel.cpp.
void            ll_panel_register(const LL_Panel *p);

//! True once `init` has run and succeeded.  A draw before this is a no-op, not an error.
bool            ll_panel_ready(void);

/*! Blit RGB565 to the active panel -- the call the CAMERA driver makes.  This is the whole
    reason the camera no longer needs to know what a panel is: it hands over pixels and
    geometry, and the panel decides whether that is a bus transaction or a store into a
    scanned framebuffer. */
void            ll_panel_blit_rgb565(Lamb &lamb, int x, int y, int w, int h, const uint16_t *src);

/*! @} */

/*! \class LL_File_System

  The "file system" type elides the differences between different underlying platforms.
  For example, it will deal with the leading '/' required by LittleFS.
  This minimal file system interface is platform-independent.
*/
class LL_File_System {
public:
  LL_File *open(const char *path, const char *mode);
  bool exists(const char *path);
  int rm(const char *path);
  int mv(const char *from, const char *to);
  int mkdir(const char *path);

private:
};

extern LL_File_System ll_file_system;

/*! \name WiFi and Wire, in case we need to rationalize conflicting implementations.
 */
//!@{
#if LL_WIRE
#include "Wire.h"
extern TwoWire *LL_Wire;
#endif

#if LL_WIFI
#include "WiFi.h"
#include "WiFiClientSecure.h"
extern WiFiClass *LL_WiFi;
#endif
//!@}

/*! \class LambStdioClass
  
  A wrapper around the underlying stdin/stdout implementation.
  On an embedded system, this class will use the primary serial in/out (`Serial` on Arduino-compatibles).
  On Linux, this class will set the terminal to byte-at-a-time mode (called "non-CANONICAL" mode).
*/
class LambStdioClass {
public:
  int setTxBufferSize(int n);
  int setRxBufferSize(int n);

  //! register a callback the TRANSPORT invokes when input arrives, so the console can be
  //! drained by something other than the reader.  This belongs on the SHIM, not at the call site:
  //! LL_Term talks to LambStdioClass and nothing else, and `onReceive` is a HardwareSerial method
  //! that the shim does not inherit -- calling it through LambStdio compiles nowhere.
  //! Returns true if the transport supports it.  POSIX returns false and the caller keeps its
  //! existing reader-driven path, which is correct there: stdin is already buffered by libc.
  bool set_input_callback(void (*cb)());
  
  void begin(void) { begin(115200); }
  void begin(unsigned long baudrate);
  void end();
  int available(void);
  int availableForWrite(void);
  int read(void);
  int write(uint8_t c);
  int write(char c) { return write((uint8_t) c); }
  
  void flush(void);

  int read(byte *buf, int max) {
    int nread = 0;
    while (max--) {
      int b = read();
      if (b == EOF) break;
      *buf++ = b;
    }
    return nread;
  }

  int read(char *s, int max)		{ return read((uint8_t *) s, max); }
  int write(const char *s, int n)	{ return write((uint8_t *) s, n); }
  int write(const byte *b, size_t n)	{ int i=n;  while (i--) write(*b++);  return n; }
  int write(const char *s)		{ int i=0;  while (*s) { write(*s++); i++; }  return i; }
  
  operator bool() { return true; }
};

extern LambStdioClass LambStdio;

typedef byte uuid_t[16];

//!Macro to do something once after the system awakens.
#define ll_once(_once_something) do {		\
    static bool _visited_ = false;		\
    if (!_visited_) {				\
      _visited_ = true;				\
      { _once_something; }			\
    }						\
  } while (0)					\
    //
//

//!Macro to do something every so often.
#define ll_every(_every_so_often_ms, _every_something_to_do) do {	\
    static unsigned long _every_next = 0;			\
    unsigned long _every_now = millis();			\
    if (_every_now >= _every_next) {				\
      { _every_something_to_do; }				\
      _every_next = _every_now + (_every_so_often_ms);		\
    }								\
  } while (0)							\
    //
//

/*
  The embedded debug catcher ensures that an address is available to be set as a BREAKPOINT for a
  hardware debugger.  This is useful in cases where the generated code has been heavily inlined --
  a catch arm that is inlined away has no address to break on, and this gives every catch one.
  Undefine this symbol if not using a hardware debugger, or redefine it to point to a different
  breakpoint target.

  IT IS NOT A NO-OP, WHICH MATTERS BECAUSE IT LOOKS LIKE ONE AT ALL TEN USE SITES.  A bare
  `ll_debug_catcher;` in a catch arm reads as a leftover, and in `main.cpp`'s hand-written copies
  of `ll_catch_terminal` it reads as a stray statement.  It calls `embedded_debug_catcher()`
  (`ll_vm_util.cpp:22`), which does:

      ll_every(10000, { magenta("%s caught\n", me); ll_term.flush(); delay(10); });

  `ll_every`'s argument is MILLISECONDS, not a call count -- so the common path is one `millis()`
  and a compare, and at most once per TEN SECONDS it prints a magenta `caught` line, flushes the
  terminal, and stalls 10 ms.

  TWO CONSEQUENCES WORTH KNOWING.  First, that 10 ms stall is on the FAULT path of SHIPPED
  firmware: nothing undefines this symbol anywhere in the tree despite the advice above, so a
  customer program that throws carries it.  For a hard-real-time node that is a real, if bounded,
  cost on the error path -- the same class of finding as [B765], where every raise logged.  Second,
  it is NOT the cause of [B839]'s 6.4-29 ms per caught raise: at one firing per 10 s against
  `guard-raise`'s 3.2 s for 500 raises it contributes about 10 ms to the whole benchmark, ~0.3%.
  Ruled out by arithmetic rather than left as a suspicion.
*/
void embedded_debug_catcher();
#define ll_debug_catcher embedded_debug_catcher()
//#define ll_debug_catcher ll_term.flush()

// ── Settings file paths ──────────────────────────────────────────────────────
#if LL_LITTLEFS
#define LAMB_SETTINGS_PATH  "/Settings-Lamb.scm"   //!< Pre-allocation settings (C++ pre-reader, before heap).
#define SETTINGS_PATH       "/Settings.scm"         //!< Runtime settings (Scheme reader, after startup).
#else
#define LAMB_SETTINGS_PATH  "Settings-Lamb.scm"   //!< cwd-relative (matches setup.scm's "Settings.scm"; program runs from data_staged/<env>)
#define SETTINGS_PATH       "Settings.scm"
#endif

// ── Pre-allocation settings ──────────────────────────────────────────────────
/*!
  Read from LAMB_SETTINGS_PATH before any cells exist.
  Default matches the hardcoded initial cell block size.
  Silent if file is missing (first-boot safe).
*/
struct LambPreSettings {
  LL_int32 cell_block_size      = 8 * 1024;   //!< Cells in the initial GC block (8K default; perf pushes 16K to match comps).
  LL_int32 extension_block_size = 4 * 1024;   //!< Cells per on-demand expansion block (4K).
  //! The LIVE SET the GC last measured, persisted across reboots so the heap can come back
  //! already sized for the workload.  NOT a block size -- the initial block is derived from it
  //! (round up to an extension_block_size multiple, then double).  0 = nothing persisted yet.
  //! Deliberately a separate key from cell_block_size: the old code wrote a multi-block TOTAL into
  //! cell_block_size, which means "size of the initial single block", and the units mismatch
  //! bricked boards.  Keeping the quantities in distinct fields makes that error unrepresentable.
  LL_int32 gc_total_marked      = 0;
  // [B610]
  //! THE POOL THE COLLECTOR ACTUALLY NEEDED, i.e. `yuasa_N` at the end of the last run that
  //! expanded -- not a live set to be multiplied later.  `gc_total_marked * 2` was a heuristic
  //! standing in for a ratio the VM computes exactly: measured on esp32s3-n8r2-01, N/Amax is 1.27,
  //! and the matching writer clamp of total_cells()/2 was therefore ~37% more conservative than
  //! necessary, persisting a short figure that forced extra settling boots.
  //! ZERO MEANS "NOT WRITTEN BY THIS VM", and the consumer falls back to the old 2x path -- so a
  //! Settings file written before this key existed still boots exactly as it did.
  LL_int32 gc_pool_target       = 0;
  LL_int32 ncg_frame_pool_size  = 16;        //!< NcgFrames pre-allocated into the free-list pool at startup.
  //! [P285] WHERE THE HOT PERMANENT STRUCTURES LIVE.  1 = internal SRAM (IRAM-capable D/IRAM on an
  //! ESP32), 0 = PSRAM.  Settings rather than build flags (owner, 2026-10-22) so one image can be
  //! reconfigured per boot and a benchmark can be run across placements without a rebuild.
  //!
  //! THE DEFAULTS ARE TODAY'S BEHAVIOUR, so a board with no key behaves exactly as before.
  //!
  //! ON AN LX6 THESE TRADE AGAINST THE CODE GENERATOR AND THE EXCHANGE RATE IS STEEP.  The sint
  //! cache is 73,740 B against 46,704 B of exec headroom -- it never fitted, and is placed only by
  //! evicting the compiler's working room.  Measured twice: [B718] on 2026-09-30 (free exec space
  //! 46,704 -> 3,696, up to 74% on recursion) and again on 2026-10-21, where qsort(200) NCG went
  //! 10,176 -> 44,186 us because `ncg_exec_floor` refuses every compile below its floor.  On a part
  //! with LL_NCG_PSRAM_EXEC native code spills to PSRAM and there is no contention at all.
  LL_int32 sint_in_iram         = 1;
  LL_int32 rootstack_in_iram    = 1;
  //! Hard cap on ONE FORMATTED LOG/ERROR MESSAGE (`va_list_expand()`), beyond which the text is
  //! TRUNCATED rather than an error raised -- see the note at that function for why this one
  //! buffer must truncate instead of throwing.  Separate from `toString_MAX_LENGTH` (which caps a
  //! printed DATUM) so a site that wants longer diagnostics does not also loosen the reader's cap.
  //! CLAMPED ON LOAD to [1024, 65536]; see `ll_va_list_max` for why the ceiling is not advisory.
  LL_int32 va_list_max          = (LL_int32) toString_MAX_LENGTH;
  // [B441] and [B417] for the keys it must preserve
  //! BOOT-ATTEMPT SENTINEL.  A persisted setting that makes the VM unbootable is
  //! self-perpetuating: the value is read before anything can judge it, the boot dies, and the next
  //! boot reads the same value.  `boot_pending` is written 1 just before a risky persisted value is
  //! USED and rewritten 0 once the VM is up.  Finding it still 1 at load means the previous boot
  //! did not survive what it read, so the risky value is discarded rather than retried forever.
  //! 0 = last boot completed.  1 = a boot was attempted with persisted values and did not finish.
  LL_int32 boot_pending         = 0;
  void load();               //!< Reads LAMB_SETTINGS_PATH_RAW via fopen; silent if missing.
  //! [B888] Assign one `key . value` pair.  Returns false if the key is not one of ours, which is
  //! what lets `load()` NAME a typo instead of discarding it.
  //!
  //! A SEPARATE FUNCTION BECAUSE `load()` NOW CALLS IT MORE THAN ONCE PER LINE.  Before B888 the
  //! dispatch was an `if`/`else if` chain written inline in the character loop and reached exactly
  //! once per line; parsing every pair on the line means calling it in an inner loop, and the
  //! alternative to a function was a second copy of a seven-way chain -- the hand-maintained
  //! second copy that a new key would silently be missing from.
  bool apply_key(const char *key, long val);
};

//! rewrite the settings file from `ll_pre_settings`, with `boot_pending` as given.
//! One writer, so a key added to the struct cannot be dropped by a caller that did not know of it.
void ll_pre_settings_write(LL_int32 boot_pending);
//! call once the VM is up; clears the sentinel so the next boot trusts what is stored.
void ll_settings_boot_ok(void);

//! THE LIVE COPY OF WHAT `load()` READ.  Two consumers need it long after the local
//! `LambPreSettings pre` in `Lamb::setup()` has gone out of scope: `va_list_expand()`, which is a
//! free function reachable before any object exists, and the GC's rewrite of the settings file,
//! which must PRESERVE the keys it does not own.  Written once by `load()`; treat as
//! read-only afterwards.
extern LambPreSettings ll_pre_settings;

//! [B859] THE `cell_block_size` THE HEAP WAS ACTUALLY BUILT WITH, when it is not the one that was
//! asked for.  0 = the request was honoured, so there is nothing to report.
//!
//! WHY A SEPARATE GLOBAL AND NOT A WRITE-BACK INTO `ll_pre_settings.cell_block_size`: that field is
//! what `ll_pre_settings_write()` PERSISTS, and the effective value here is derived from the boot
//! POOL TARGET, not from an initial-block size.  Writing a pool figure into the key that means
//! "size of the initial SINGLE block" is [B248] exactly -- it bricked an S3-EYE into a
//! re-enumerating boot loop, and a firmware-only reflash does not clear it ([B441]).  It would also
//! LATCH: the override only happens while the target exceeds the request, so persisting the target
//! makes the two equal next boot and the condition can never be reported again.  So the effective
//! value is reported and NEVER stored as a setting.
//!
//! Set by `LambMemoryManager`'s constructor at the point of replacement; read by
//! `ll_pre_settings_write()`, which records it as a COMMENT.  A comment is not a key: the loader
//! skips only `(`/`)`/space/tab/CR before its `%63[a-zA-Z0-9_]` scan and STOPS at anything else, so
//! a line beginning `;` cannot match and cannot be read back as a value.  [B888] added `)` to that
//! skip set and made the scan repeat across the line; the `;` guarantee is unchanged, because the
//! skip set deliberately does not include `;` -- a comment ENDS the line's parse rather than being
//! stepped over, so `(va_list_max . 8192) ; see B175` reads one key and no more.
extern LL_int32 ll_cell_block_size_effective;

//! The cap `va_list_expand()` actually enforces, published by `load()` from `va_list_max`.
//! A SEPARATE GLOBAL, not a read through `ll_pre_settings`, because this is consulted on the
//! logging path before `load()` has run at all -- it must have a valid value from static init.
//! CEILING IS NOT ADVISORY: `autobuf_t::upsize()` RAISES a Scheme error past its own `_max_`
//! (65536), and raising from inside the formatter means throwing while building the error
//! message that the throw will need to format.  Clamping on load keeps that unrepresentable.
extern LL_int32 ll_va_list_max;

//! @name C++ *try* and *catch* are used to process code faults detected by the LambLisp VM.
//! To cleanly unwind after an error is detected, each *catch* has uniform behavior, which is captured in this macro.
//!@{
#define ll_try try

//! LOG ONCE PER ERROR, NOT ONCE PER FRAME.
//! This macro is on every unwinding frame, so a deep error printed one line PER eval FRAME: a
//! recursion-depth trip at ~40k deep emitted ~20,000 identical lines, which buries the actual error
//! and, on a serial console, takes longer to drain than the computation took to fail.  It is not
//! specific to deep recursion -- any error raised there did this -- but the guard made it
//! routine, because tripping the guard is precisely the deep case.
//! The SAME T_ERROR object propagates up the whole unwind, so frame 2..n are recognisable by
//! pointer: log the first, then one "suppressing" line, then stay quiet.  A genuinely NEW error
//! resets the counter and prints normally, so nothing is lost except the repeats.
//! Pointer-compared as void*, deliberately: this must not resurrect a cell or touch GC state while
//! an exception is in flight.  mk_syserror reuses a singleton, so two DISTINCT errors can share a
//! pointer -- but only when separated by a completed unwind, and the count resets on the next
//! distinct object, so the worst case is one suppressed duplicate line, never a lost first report.
extern const void *ll_catch_last_err;   //!< most recently logged error object (compared, never dereferenced)
extern int         ll_catch_repeats;    //!< frames seen for that same object during this unwind
#define LL_CATCH_REPEAT_LIMIT 4

// [B665] why bad_alloc is converted at the first handler
//! A C++ ALLOCATION FAILURE IS AN OUT-OF-MEMORY ERROR, NOT A DIFFERENT KIND OF
//! EVENT -- SO CONVERT IT AT THE FIRST HANDLER AND LET THE REST OF THE UNWIND BE ORDINARY.
//! The VM had TWO out-of-memory paths and they disagreed.  Running out of CELLS throws
//! `NIL->mk_error("out of memory ...")` from `gc_urgent()` -- a `Sexpr_t`, so every `ll_catch` on
//! the way up runs its restore code and the root stack comes back balanced.  Running out of C++
//! HEAP threw `std::bad_alloc`, which `catch (Sexpr_t)` cannot see, so the SAME condition unwound
//! past every restore point to the one `catch (std::bad_alloc)` in `main.cpp`.
//! MEASURED with `w3 test oomsim`, three injected failures in one session: the root depth read
//! entry=1 catch=16, entry=16 catch=31, entry=31 catch=46 -- each event starting where the last
//! ended, because no frame in between was ever entered.  Not a leak in any one frame; the frames
//! simply never ran.
//! WHY HERE AND NOT AT EVERY `new`.  Converting at the allocation site is the same thing for every
//! function that has an `ll_try` -- its own `ll_catch` is the first handler either way, so the
//! restore happens at the identical frame -- and it is STRICTLY BETTER for the rest, because
//! `std::vector`/`std::string` growth throws `bad_alloc` from inside libstdc++ where no site of
//! ours exists to convert.  One macro covers both.
//! `Cell::mk_error()` fills the statically allocated OBJ_SYSERROR and ALLOCATES NOTHING, which is
//! the only kind of error object an out-of-memory path may construct.
//! The rethrow is `Sexpr_t`, so this arm fires at most ONCE per unwind, at the deepest handler;
//! every frame above it takes the ordinary arm below and the repeat suppression works unchanged.
// [B765] the 25,000-line noise measurement behind this
//! A USER `raise` IS CONTROL FLOW, NOT A FAULT -- DO NOT LOG IT IN EVERY UNWOUND FRAME.
//!
//! R7RS `raise`/`guard` is how a program handles an expected condition, so a loop that raises is a
//! loop, not a failure.  Every `*_catch` on the unwind path logged unconditionally, so 5,000 raises
//! produced 25,000 log lines and a benchmark rep took 3.34 SECONDS -- almost all of it our own
//! logging.  On a board that goes out over the UART, so a fault handler got slower the more faults
//! it handled, which is the wrong shape for a real-time runtime.
//!
//! THE REPEAT SUPPRESSOR BELOW CANNOT COVER THIS, and that is why a separate test is needed: it
//! keys on the ERROR OBJECT ADDRESS to collapse the many frames of ONE unwind, and each `raise`
//! allocates a FRESH `T_ERROR`, so the counter resets every time.  It is correct for a fault and
//! blind to a loop.
//!
//! `mop3_raise` ALREADY throws outside its own `mop3_try` "to avoid logging normal raises"
//! (ll_vm_ai_rxrs.cpp) -- so this finishes an intention that was already half-expressed, rather
//! than introducing a new policy.  The same marker is what `with-exception-handler` tests to
//! decide whether to hand the handler the raised value or the T_ERROR itself.
//!
//! AN UNCAUGHT RAISE IS STILL REPORTED: the top level prints `LambLisp fatal: uncaught error
//! object ...` from its own handler, which this does not touch.  Silencing the unwind does not
//! silence the outcome -- verified, not assumed.
#define LL_ERR_IS_USER_RAISE(__e__) \
  ((__e__)->type() == Cell::T_ERROR && strcmp((__e__)->error_get_chars(), "__raise__") == 0)

/* WHAT `ll_catch` DOES AND DOES NOT DO -- read this before deciding where a frame may go.
 *
 *  IT LOGS AND RETHROWS.  It does NOT touch the rootstack.  That is deliberate and it is NOT a
 *  gap: the rootstack is unwound at the MOP3 BOUNDARY, by `mop3_catch`, which restores the depth
 *  `mop3_try` saved on entry.  An `ll_catch` frame between the throw and that boundary has nothing
 *  to restore -- it did not save a depth, and restoring to its own would discard entries its
 *  caller still owns.
 *
 *  SO THE THREE HANDLERS DO THREE DIFFERENT JOBS:
 *    `mop3_catch`        APPLICATION error -- restore to the mop3 boundary so the REPL continues.
 *    `ll_catch`          intermediate frame -- log once, annotate, rethrow.  Rootstack untouched.
 *    `ll_catch_terminal` SYSTEM RESCUE -- something escaped everything; keep running.  Also does
 *                        not restore, because by the time it is reached the mop3 boundaries have.
 *
 *  MEASURED, 2026-10-13, because this was a standing uncertainty and reading could not settle it:
 *  a 50,000-deep recursion pushed 181,439 of 261,608 root slots and threw; the next error at the
 *  REPL reported `entry=1`, i.e. the depth was fully restored on the way out.  Six other error
 *  kinds all reported `entry=1 catch=1` at the terminal frame.  That is the [B652] probe in
 *  `main.cpp` answered: the rootstack is NOT leaked across an unwind.
 *
 *  IF YOU NEED A RESTORE ON THIS PATH, pass it in: `ll_catch(gc_root_setdepth(saved))` with the
 *  depth saved BEFORE the try.  `ll_vm_rdr.cpp:279` and `ll_vm_mop3_extra.cpp:54` do exactly that,
 *  and they are the right pattern for a frame whose cleanup cannot wait for the mop3 boundary. */
#define ll_catch(__code_before_rethrow__)				\
  catch (std::bad_alloc &) {						\
    Sexpr_t __err__ = NIL->mk_error("%s out of memory (C++ allocation failed)", me); \
    ll_catch_last_err = (const void *) __err__;				\
    ll_catch_repeats  = 1;						\
    global_printf("\r[%d] %s ll_catch(): %s\n", millis(), me, __err__->error_get_chars()); \
    ll_debug_catcher;							\
    __code_before_rethrow__;						\
    throw __err__;							\
  }									\
  catch (Sexpr_t __err__) {						\
    if (__err__->type() != Cell::T_ERROR)				\
      throw NIL->mk_error("ll_catch() BUG in %s bad type %s", me, __err__->dump().c_str()); \
    									\
    if (!LL_ERR_IS_USER_RAISE(__err__)) {				\
      if ((const void *) __err__ != ll_catch_last_err) {		\
        ll_catch_last_err = (const void *) __err__;			\
        ll_catch_repeats  = 0;						\
      }									\
      if (++ll_catch_repeats <= LL_CATCH_REPEAT_LIMIT)			\
        global_printf("\r[%d] %s ll_catch(): %s\n", millis(), me, __err__->error_get_chars()); \
      else if (ll_catch_repeats == LL_CATCH_REPEAT_LIMIT + 1)		\
        global_printf("\r[%d] %s ll_catch(): ... same error still unwinding; further frames suppressed\n", millis(), me); \
    }									\
									\
    ll_debug_catcher;							\
    __code_before_rethrow__;						\
    throw __err__;							\
  }									\
  //

#define ll_catch_terminal						\
  catch (std::bad_alloc &) {						\
    global_printf("\r[%d] %s ll_catch_terminal: out of memory (C++ allocation failed)\n", \
                  millis(), me);					\
    ll_debug_catcher;							\
  }									\
  catch (Sexpr_t __err__) {						\
    /* [B765] inside a macro a // comment would swallow the \ continuation, so block form. */ \
    /*! THE TERMINAL FRAME MUST SAY WHAT WAS RAISED: it is now the only place that says \
     *  anything, since the per-frame logging is gated on a user raise.  An uncaught raise \
     *  otherwise printed `non-error type 26` and nothing else. */			\
    if (LL_ERR_IS_USER_RAISE(__err__))					\
      global_printf("\r[%d] %s ll_catch_terminal: UNCAUGHT raise: %s\n",	\
                    millis(), me, __err__->error_get_irritants()->str().c_str()); \
    /*! NO `str()` on the non-error arm -- one such object str()s as 28,733 bytes of repeated \
     *  #UNDEF and blows the va_list cap.  The type number is the useful datum. */	\
    else if (__err__->type() != Cell::T_ERROR)				\
      global_printf("\r[%d] %s ll_catch_terminal: non-error type %d\n",	\
                    millis(), me, __err__->type());			\
    else								\
      global_printf("\r[%d] %s ll_catch_terminal: %s\n",		\
                    millis(), me, __err__->error_get_chars());		\
    ll_debug_catcher;							\
  }									\
  //
//!@}

/*! @name  R7RS feature identifiers
 *
 *  The single list behind BOTH `(features)` and `cond-expand`.  R7RS requires the two to agree by
 *  construction -- a clause `cond-expand` selects must name a feature `(features)` reports -- so
 *  they read one array instead of keeping two hand-maintained copies that agree only until
 *  someone adds a third.  Defined in `ll_xmop3_platform_generic.cpp`, next to the macros it is
 *  derived from.
 *
 *  Returns a pointer to a static array of @a count NUL-terminated names; never NULL.
 */
//!@{
#ifdef __cplusplus
const char *const *ll_feature_names(int *count);
#endif

/*! @name  ANSI terminal escapes -- one predicate, decided at run time
 *
 *  `ll_term_ansi()` means *escapes emitted to this terminal are INTERPRETED, not printed*.  It
 *  governs colour AND cursor control (`\e[K`, `\e[<n>D`) -- see the block comment in
 *  `ll_vm_term.cpp`, which is also where the 16 `fg_*` pointers it swaps are defined.
 *
 *  Declared HERE rather than in `ll_vm_term.h` because `ll_vm_*` is stripped from the customer
 *  package, and a customer-visible file (`ll_xmop3_*`) needs this.
 */
//!@{
#ifdef __cplusplus
bool ll_term_ansi(void);           //!< is ANSI output enabled right now?
void ll_term_ansi_set(bool on);    //!< turn it on/off; repoints every fg_* in one place
bool ll_term_ansi_detect(void);    //!< decide from LL_COLOR / NO_COLOR / the console, apply, return
#if LL_WINDOWS
int  ll_win_enable_vt(void);       //!< 1 = this console now interprets ANSI; 0 = it will not / is not a console
void ll_win_yield_ms(unsigned ms); //!< a REAL sleep.  NOTE delay_ms() on every host is a BUSY-WAIT
                                   //!< (`while (millis() < end);` -- see ll_platform_AMD64.cpp:41),
                                   //!< so it is the wrong tool for giving the CPU back.
#endif
#endif
//!@}
//!@}


#endif
