// Copyright 2026 by Frobenius Norm LLC 2026-05-16
// Free for non-commercial use. Commercial use requires a license.
#include <new>            //!< [B520] std::bad_alloc
#include "LambLisp.h"
#include "ll_exec_cap.h"   //!< B360: LL_HAVE_EXEC_CAP / ll_exec_heap_free()
#include <exception>   //!< B349: name what escapes, instead of a mute abort() on an MCU
#include <cstdlib>
#if LL_ESP32 || LL_ESP32S3
#  include "esp_task_wdt.h"
#  include "esp_idf_version.h"
#  include "soc/timer_group_struct.h"
#  include "soc/timer_group_reg.h"
#endif

/*! @name These are the non-VM operators that are local to this application.

  To add new Lisp-compatible C++ operators, follow this recipe:

  1. Create your C++ function, having the "mop3" signature, as this:
  ```
  Sexpr_t my_function(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);

  ```
  In this example *my_function()* will receive a Lamb virtual machine, a symbolic expression *sexpr* of type *Sexpr_t* to be evaluated,
  and an environment in which to execute (which is also of type *Sexpr_t*).
  All the LambLisp native operators share this signature, so there is no "foreign function interface" as in other Lisps.
  All the functions with the *mop3* signature are native and will run at full C++ speed.

  **Warning:* To preserve the tail-recursion feature, *mop3* operators should not call other *mop3* operators.
  Recursive calls (direct or indirect) to the LambLisp S-expression partial evaluator will cause stack overflow.
  If you encounter a situation in which this seems desirable, you should instead factor out the common part
  and have the 2 *mop* operators call the common part separately.
 */

//!@{
Sexpr_t CommonIO_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t ESP32_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t PCA9685_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t WS2812_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t WiFi_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t Wire_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t Sonar_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t SPI_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t OneWire_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t LCD1602_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t Panel_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);   // P231: display panel, board-independent
Sexpr_t Lvgl_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);    // P231: LVGL, bound to the panel abstraction
Sexpr_t EyeCam_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
Sexpr_t SD_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);     // SD card in SPI mode; installs nothing without a card socket
Sexpr_t ota_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);   // OTA loader<->VM NVS handshake
#if LL_CUDA
Sexpr_t Cuda_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
#endif
#if LL_TENSORRT
Sexpr_t TensorRT_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);   // resident inference engine
#endif
#if LL_HIP
Sexpr_t Hip_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
#endif
//!@}

Sexpr_t install_local_native_operators(Lamb &lamb)
{
  ME("::install_local_native_operators()");

  /*! @name The LambLisp virtual machine operates by executing Lisp language native functions written in C++.
    
    This provides high performance at runtime, and scalability at compile/link time.
    The LambLisp virtual machine operations are referred to generically as *mops* or *mop3* (because they all take 3 parameters).
    In the Lamb VM code the C++ type name for a LambLisp-compatible function is *Mop3st_t* ("mop3 star type", with the st standing in for an asterisk).
    The type Mop3st_t is a pointer to a LambLisp-compatible native function.

    Installers for native procedures are themselves Mop3st types, but so far they do not return any useful value.
    It should be possible to reinstall these native operators at runtime, a feature of doubtful utility.
    In Lisp the function signature for an installer is ```(installer env-target)```.
    The C++ signature for *Mop3st_t* is ```Sexpr_t installer(Lamb &lamb, Sexpr_t list_containing_1_element_env_target, Sexpr_t env_execution);```

    Dictionaries are first-class objects in LambLisp.
    Dictionaries are hierarchical, implemented as a list of frames, each of which may be an alist or a vector of alists (used as a hash table).
    It is possible to add a new dictionary child frame on top of an existing parent dictionary.
    When a dictionary is searched, the topmost child is checked first, and the first matching key/value pair is returned.

    The LambLisp execution environment is a dictionary.  Dictionary keys may be any type, but when used as environments dictionary keys are all symbols.
    the LambLisp object system is based on dictionaries, which directly support the concept of *object inheritance*.
    Object keys are also symbols.
  */
    
  /*!These are the installers for additional language primitives (written in C++) required for this particular application.
    They do not require any header file; for each additional group of functions there is an installer that places the functions into the runtime environment.
    The installer is a global function, not a member of any class.
    All the installers have a same signature, of the *Mop3st_t* type.
   */
  const Lamb::Mop3st_t func[] = {
    CommonIO_install_mop3,
#if LL_CUDA
    Cuda_install_mop3,
#endif
#if LL_TENSORRT
    TensorRT_install_mop3,
#endif
#if LL_HIP
    Hip_install_mop3,
#endif
    ESP32_install_mop3,
    WiFi_install_mop3,
    Wire_install_mop3,
    Sonar_install_mop3,
    SPI_install_mop3,
    OneWire_install_mop3,
    PCA9685_install_mop3,
    WS2812_install_mop3,
    LCD1602_install_mop3,
    Panel_install_mop3,
    Lvgl_install_mop3,
    EyeCam_install_mop3,
    SD_install_mop3,
    ota_install_mop3,
  };
  const int Nfuncs = sizeof(func)/sizeof(func[0]);

  Sexpr_t env_exec      = lamb.r5_interaction_environment();
  Sexpr_t env_target_sx = lamb.cons(env_exec, NIL, env_exec);	//put env into a list for the Mop3st calling protocol

  //! A FRAME, NOT A PUSH/POP PAIR.  Each installer allocates, and a throw out of any of them --
  //! an installer that fails partway through -- skipped the old pop, because `ll_catch` rethrows
  //! without touching the rootstack.  One restore covers every exit.
  mop3_gc_frame({
    rootstack.push(env_target_sx);
    for (int i=0; i<Nfuncs; i++)
      Sexpr_t ignore = func[i](lamb, env_target_sx, env_exec);
  });
  
  return OBJ_UNDEF;
}

/*!
  Allocate Lamb virtual machines dynamically, not statically.
  The constructors may depend on serial port availability or other operating system facility that is not available at static construction time.
*/
Lamb *lamb = 0;

//! THE MCU ENTRY POINTS MUST CATCH MORE THAN `Sexpr_t`, OR AN MCU FAULT IS MUTE.
//!
//! `ll_catch_terminal` is `catch (Sexpr_t)` and nothing else, so without the `...` arms below a
//! std::bad_alloc / std::length_error out of any ordinary allocation -- `new uint8_t[newcap]` in
//! NcgBuffer::ensure, `new NcgXtensa()`, `new NcgCode{}`, a std::vector growth in the NCG
//! backend -- unwinds past EVERY handler in the VM to std::terminate, and the board prints
//! nothing but
//!     abort() was called at PC 0x... / Backtrace: ... / Rebooting...
//! WHAT THAT COSTS is not the crash, it is the DIAGNOSIS: decoding that backtrace needs the exact
//! firmware.elf, which is a build artifact that is usually already overwritten.
//! So: NAME IT, THEN GO DOWN.  Do not "recover" and continue -- an escape this deep leaves the GC
//! root stack unbalanced; a reboot is the honest outcome, an EXPLAINED reboot is the useful one.
//! POSIX main() at the bottom of this file catches `Sexpr_t` and `...` for the same reason.
//! [B349] [B182] [B220]
static void ll_setup_body();

#if LL_ESP32C5
//! GUARD ON `LL_ESP32C5`, THE `-D` FROM platformio.ini -- NOT on CONFIG_IDF_TARGET_ESP32C5.  Those
//! macros live in `sdkconfig.h`, which this translation unit does not reliably see, so a CONFIG_-
//! keyed guard SILENTLY COMPILES TO NOTHING: no `ll_b340_rearm_psram_barrier` in the ELF and no
//! re-arm line in the boot log, which is indistinguishable from the code not being there.
//! `LL_ESP32C5` is on the compiler command line and cannot go missing.  The headers below are
//! included explicitly for the same reason.  [B340]
#  include "sdkconfig.h"
#  include "esp_heap_caps.h"
//! [B340] RE-ARM THE PSRAM MSPI MEMORY BARRIER, WHICH THE IDF TRIED TO ARM BEFORE PSRAM EXISTED.
//!
//! WHAT GOES WRONG.  `esp_psram_init()` registers PSRAM with the heap at esp_psram.c:129 and arms
//! the barrier at :140 -- but only the FIRST is inside
//! `#if CONFIG_SPIRAM_BOOT_INIT && (CONFIG_SPIRAM_USE_CAPS_ALLOC || CONFIG_SPIRAM_USE_MALLOC)`.
//! The Arduino prebuilt libs we link against define CONFIG_SPIRAM=1 and CONFIG_SPIRAM_USE_MALLOC=1
//! but do NOT define CONFIG_SPIRAM_BOOT_INIT AT ALL, so that guard is 0, the heap-add is compiled
//! out, and `esp_psram_mspi_mb_init()` runs anyway -- asking for 32 bytes of MALLOC_CAP_SPIRAM from
//! a heap that has no SPIRAM region yet.  Hence the one early boot line:
//!     E (1349) MSPI Timing: Failed to allocate dummy cacheline for PSRAM memory barrier!
//! PSRAM is full LATER because ARDUINO registers it at system-init priority 99
//! (esp32-hal-misc.c:299 -> psramInit -> psramAddToHeap), long after cpu_start.c:646.
//!
//! WHY CALLING IT AGAIN WORKS.  `esp_psram_mspi_mb_init()` is not static and simply redoes the same
//! calloc into its own file-static pointer.  By the time `setup()` runs, Arduino has registered
//! PSRAM, so the identical request now succeeds.  Nothing about the allocation was ever unusual --
//! it is 32 bytes, 32-byte aligned; there was simply no SPIRAM to take it from.
//!
//! THE BARRIER IS NOT COSMETIC: `esp_psram_mspi_mb()` is a DMA/PSRAM ordering barrier
//! (esp_dma_utils.c:138, uhci.c:123/153) and is a silent NO-OP while unarmed, for the whole run.
//! It is an erratum workaround for C5 silicon BELOW v1.02; our bench parts are v1.02 (ECO3) so it
//! is inert HERE, but the shipped image declares support down to v1.00 where it is load-bearing.
//!
//! WE PROBE BEFORE RE-ARMING BECAUSE THE IDF CANNOT REPORT ITS OWN FAILURE: `esp_psram_mspi_mb_init()`
//! logs and then `return ESP_OK;` regardless, so its result says nothing.  The probe performs the
//! IDENTICAL allocation and reports it, which is the only way this line can be seen to have worked.
extern "C" esp_err_t esp_psram_mspi_mb_init(void);

static void ll_b340_rearm_psram_barrier()
{
  const size_t line = CONFIG_CACHE_L1_CACHE_LINE_SIZE;
  void *probe = heap_caps_calloc(1, line, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
  unsigned long psfree = (unsigned long) heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (probe) {
    heap_caps_free(probe);
    esp_psram_mspi_mb_init();                 //!< same call the IDF made; now it has memory
    global_printf("\r[%lu] PSRAM MSPI barrier re-armed (%lu B probe ok, PSRAM free=%lu)\n",
                  millis(), (unsigned long) line, psfree);
  }
  else {
    //! Still failing HERE would mean PSRAM is not in the heap even at setup() -- a different fault
    //! from B340, and worth saying so rather than reporting a re-arm that did not happen.
    global_printf("\r[%lu] PSRAM MSPI barrier NOT re-armed: %lu B SPIRAM probe FAILED "
                  "at setup() (PSRAM free=%lu) -- PSRAM is not in the heap here either\n",
                  millis(), (unsigned long) line, psfree);
  }
}
#endif
void setup()
{
#if LL_POSIX
  ll_setup_body();          //!< POSIX: main()'s own try/catch already covers this (B182)
#else
  try {
    ll_setup_body();
  }
  catch (Sexpr_t err) {
    //! DO NOT abort() HERE.  abort() restarts the chip, and because the cause of a failed setup is
    //! almost always a CONFIGURATION the next boot will meet again, the result is a BOOT LOOP --
    //! each attempt printing this line and resetting before an operator can type.
    //!
    //! A BOARD YOU CAN CONNECT TO BEATS ONE THAT REBOOTS EVERY 1.3 SECONDS.  Continuing gives a
    //! REPL to interrogate -- `(Platform.free-internal)`, the `sint_cache:` placement line, the
    //! pool size -- instead of a transcript to read afterwards.
    //!
    //! THIS IS A SAFETY NET AND NOT THE CURE, and must not be mistaken for one: the runtime past
    //! this point is DEGRADED and its state is whatever a half-finished setup left.  A silently
    //! crippled runtime would be WORSE than a boot loop -- a boot loop at least announces itself --
    //! so the banner is deliberately seven loud lines immediately before the prompt appears, which
    //! is where an operator is already looking.  The cure is to stop the cell pool being starved;
    //! see [B805].  Do not let this net be read as that cure.
    global_printf("\r[%lu] ::setup() FATAL uncaught LambLisp error object\n", millis());
    if (err) { try { global_printf("  %s\n", err->str().c_str()); } catch (...) {} }
    global_printf("\r[%lu] ::setup() ####################################################\n", millis());
    global_printf("\r[%lu] ::setup() SETUP DID NOT COMPLETE -- THIS RUNTIME IS DEGRADED\n", millis());
    global_printf("\r[%lu] ::setup() Continuing to the REPL anyway so the board can be asked what\n", millis());
    global_printf("\r[%lu] ::setup() happened.  Do NOT treat any result from this boot as valid.\n", millis());
    global_printf("\r[%lu] ::setup() Previously this path called abort(), which boot-looped the\n", millis());
    global_printf("\r[%lu] ::setup() board and left nothing to interrogate.\n", millis());
    global_printf("\r[%lu] ::setup() ####################################################\n", millis());
  }
  catch (std::exception &e) {
    global_printf("\r[%lu] ::setup() FATAL uncaught C++ exception: %s -- aborting\n", millis(), e.what());
    abort();
  }
  catch (...) {
    global_printf("\r[%lu] ::setup() FATAL uncaught C++ exception of unknown type -- aborting\n", millis());
    abort();
  }
#endif
}

static void ll_setup_body()
{
  unsigned long t_start = millis();
  ME("::setup()");

#if LL_ESP32 || LL_ESP32S3
  // Disable watchdogs so long Scheme evaluations (benchmarks, compilation)
  // don't trigger TG0WDT (TWDT) or TG1WDT (IWDT) system resets.
  esp_task_wdt_deinit();                          // disable TWDT (TG0WDT)
  //! THE SPLIT HERE IS BY IDF VERSION AS WELL AS BY SoC.  Under IDF 4.4 the two SoCs spelled these
  //! TIMERG1 fields differently: the S3 used wdtwprotect / wdtconfig0.wdt_en, while the classic
  //! ESP32 (LX6) used the underscore forms wdt_wprotect / wdt_config0.en -- hence the original
  //! `#if LL_ESP32S3` split.  IDF 5 REGENERATED the classic ESP32's soc/timer_group_struct.h from
  //! the same template as the newer parts, so the LX6 now uses the S3 spelling too and the
  //! SoC-only split is wrong there.  Verified in the IDF 5.5 header: timg_dev_t carries
  //! `wdtconfig0` and `wdtwprotect`, and the enable bit is `wdt_en` at bitpos 31, identical to the
  //! S3.  The symptom of getting this wrong is LX6-only and looks like a typo, because GCC helpfully
  //! suggests the very name you wanted:
  //!     error: 'timg_dev_t' has no member named 'wdt_wprotect'; did you mean 'wdtwprotect'?
  //! -- so it reads as a misspelling rather than as a header that was regenerated under you.
#if LL_ESP32S3 || ESP_IDF_VERSION_MAJOR >= 5
  TIMERG1.wdtwprotect.val = TIMG_WDT_WKEY_V;   // unlock TG1WDT write-protect
  TIMERG1.wdtconfig0.wdt_en = 0;               // disable TG1WDT (IWDT)
  TIMERG1.wdtwprotect.val = 0;                  // re-lock
#else  // LX6 on IDF 4.x only (original, wdt_ prefix registers)
  TIMERG1.wdt_wprotect = TIMG_WDT_WKEY_V;      // unlock TG1WDT write-protect
  TIMERG1.wdt_config0.en = 0;                  // disable TG1WDT (IWDT)
  TIMERG1.wdt_wprotect = 0;                    // re-lock
#endif
#endif

  LambStdio.begin();
  global_printf("[%lu] %s LambLisp starting, 1st light @%lu ms\n", millis(), me, t_start);

#if LL_ESP32C5
  //! AFTER `LambStdio.begin()`, NOT BEFORE IT.  The re-arm itself is position-independent: it only
  //! needs Arduino's priority-99 `psramInit()` to have run, which it has long before `setup()` is
  //! entered.  What is NOT position-independent is its REPORT -- placed ahead of first light the
  //! re-arm runs and says nothing, and the boot log then looks exactly as if the code were not
  //! compiled in.  So put it where it can be SEEN.  [B340]
  ll_b340_rearm_psram_barrier();
#endif
#if LL_ESP32
  //! B360: ll_exec_heap_free(), not heap_caps_get_free_size(MALLOC_CAP_EXEC) -- under ESP-IDF 6 with
  //! CONFIG_ESP_SYSTEM_MEMPROT the macro is UNDEFINED and this boot banner, which has nothing to do
  //! with native code generation, would fail the build of the whole firmware.  It reports 0 there,
  //! which is the truth: no capability, no executable heap.
  global_printf("[%lu] %s free EXEC/IRAM heap: %u bytes\n", millis(), me,
                ll_exec_heap_free());
#endif
  void ll_heap_note(const char *tag);   // B95 boot-phase internal-DRAM tracer
  ll_heap_note("boot: before new Lamb");
  lamb = new Lamb;	//avoid static allocation due to possible lack of terminal at static construct time
  lamb->setup();
  
  lamb->log("%s Installing local native operators\n", me);
  install_local_native_operators(*lamb);

  //! PHASE ORDER: the system's one-shot claims have now been made (drivers installed, radio
  //! reservation taken), so LambLisp sizes its own discretionary pool from what ACTUALLY remains.
  //! This is the "system first, LambLisp from the remainder" rule, and it is the one ordering
  //! LambLisp fully controls.  [P211]
  //! THE EXEC POOL IS NOT CLAIMED HERE.  Native code generation must not auto-allocate internal
  //! RAM; it is requested selectively at run time.  `alloc_exec()` claims the pool on the FIRST
  //! `compile-low-latency!` compile and never otherwise, so a program that does not ask pays
  //! nothing.  That does not weaken the ordering above -- it strengthens it, because the remainder
  //! is then measured even later.  [B789]
  ll_heap_note("before setup.scm load");
  lamb->log("%s Loading setup.scm\n", me);
  Sexpr_t ignored = lamb->load("setup.scm", lamb->r5_interaction_environment(), 0);
  ll_heap_note("after setup.scm load");
  //! Anything still reserved was never claimed -- give it back.  This is what makes the
  //! installer-time reservation "first IF it is used, otherwise not" rather than a tax every
  //! board pays.  On a part whose internal DRAM is tight it can decide whether a radio starts at
  //! all.  [P211] [B411]
  ll_fastram_release_unclaimed();

  //! Allocate the console spill reservoir HERE, not at LambStdio.begin() -- system services
  //! (WiFi's radio buffers, LittleFS DMA) have finished claiming internal DRAM by now, so LambLisp
  //! takes its discretionary memory from what actually remains.  Claiming earlier lands ahead of
  //! `WiFi.begin()`'s own claim and can boot-loop a part whose internal DRAM is tight.
  //! [B410] [P208] [B411]
  //! The reservoir itself is PSRAM, so this costs no internal DRAM at all; the point of the late
  //! placement is that a board WITHOUT PSRAM degrades to the driver ring rather than competing.
  global_spill_begin();

  //! THE VM IS UP -- clear the boot-attempt sentinel.  [B441]  Placed AFTER setup.scm has loaded and
  //! after the memory-hungry startup claims, because that is the span a bad persisted value kills:
  //! a gcpause run's `gc_total_marked` sizes the heap at 2x and `setup()` throws std::bad_alloc
  //! before reaching here.  Clearing it earlier would declare success while the dangerous part was
  //! still ahead, which is the failure this sentinel exists to catch.
  ll_settings_boot_ok();

#if LL_AUTOCOMPILE >= 1
  // Auto-compile all T_PROC/T_NPROC bindings in the interaction environment.
  {
    extern Sexpr_t mop3_compile_environment(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
    Sexpr_t env = lamb->r5_interaction_environment();
    Sexpr_t n = NIL;                    //!< read after the frame, so declared outside it
    lambp_gc_frame({
      lambp_gcroot_Sexpr_t(arg, lamb->cons(env, NIL, env));
      n = mop3_compile_environment(*lamb, arg, env);
    });
    lamb->log("%s auto-compile: %s procedures compiled\n", me, n->str().c_str());
  }
#endif
#if LL_AUTOCOMPILE >= 2
  // NCG-compile all T_BYTECODE bindings to native code.
  {
    extern Sexpr_t mop3_ncg_compile_env(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec);
    Sexpr_t env = lamb->r5_interaction_environment();
    Sexpr_t n = NIL;                    //!< read after the frame, so declared outside it
    lambp_gc_frame({
      lambp_gcroot_Sexpr_t(arg, lamb->cons(env, NIL, env));
      n = mop3_ncg_compile_env(*lamb, arg, env);
    });
    lamb->log("%s ncg-compile: %s procedures compiled\n", me, n->str().c_str());
  }
#endif

  // Execution-mode census.  Reports how many bindings are AST (interpreted), BC (bytecode) and NCG
  // (native) once the startup compile phase has finished.
  //
  // DELIBERATELY OUTSIDE BOTH #if BLOCKS, not tucked in after compile-environment!.  Two reasons.
  // LL_AUTOCOMPILE is set only by the linux_x86_64_bc and linux_x86_64_ncg diagnostic envs, so on
  // every shipping build both blocks compile away -- and "all AST" is exactly the fact worth
  // printing, not one to hide behind a flag nobody ships.  And when LL_AUTOCOMPILE is 2 the NCG
  // pass runs AFTER the bytecode pass, so a census placed between them would report NCG 0 on every
  // boot and read as "native code is not working" when it simply had not run yet.
  {
    extern void ll_env_exec_census(Lamb &lamb, Sexpr_t dict, int &n_ast, int &n_bc, int &n_ncg);
    int n_ast = 0, n_bc = 0, n_ncg = 0;
    ll_env_exec_census(*lamb, lamb->r5_interaction_environment(), n_ast, n_bc, n_ncg);
    lamb->log("%s execution modes: AST %d  BC %d  NCG %d  (%d procedures)\n",
              me, n_ast, n_bc, n_ncg, n_ast + n_bc + n_ncg);
  }

}

void loop()
{
  ME("::loop()");
  //! `lamb` IS A RAW POINTER THAT IS NULL UNTIL `ll_setup_body()` REACHES `new Lamb` -- and since
  //! that function can fail WITHOUT ABORTING, this is reachable.  Every arm below dereferences
  //! `lamb` unguarded, so without this guard the degraded-boot net above would trade a boot loop
  //! for a NULL DEREFERENCE, which is strictly worse: it prints nothing at all.
  //!
  //! A `Sexpr_t` throw can only come from a constructed VM, so where one was built `lamb` is
  //! non-null.  That is an argument for why this rarely fires, NOT an argument for omitting it --
  //! the guard costs one compare per loop and covers every future caller of the degraded path.
  //! THE ASYMMETRY IS THE POINT: the net above offers a REPL when the throw came from inside
  //! `lamb->setup()`, because the interpreter then exists and can be asked questions.  With no VM
  //! at all there is nothing to interrogate and nothing a prompt could do, so a reset is the
  //! honest outcome rather than a board sitting in a loop pretending to be alive.  [B805]
  if (!lamb) {
    global_printf("\r[%lu] ::loop() NO VM -- setup() failed before the interpreter was constructed, "
                  "so there is no REPL to offer and nothing to ask.  Resetting.\n", millis());
    abort();
  }
#if LL_POSIX
  //! THE POSIX ARM RECOVERS FROM `std::bad_alloc` TOO, matching the embedded arm below.
  //!
  //! `ll_catch_terminal` is `catch (Sexpr_t)` and nothing else, so without this arm a
  //! `std::bad_alloc` on a host reaches `std::terminate` and the PROCESS DIES -- while the embedded
  //! arm reports it and carries on.  Two behaviours for one condition, and the divergence is hard
  //! to notice because an allocation failure is hard to reach on a host by accident.
  //!
  //! IT IS ALSO WHAT MAKES THE POST-CATCH STATE DEBUGGABLE.  "What survives the unwind when the
  //! bad_alloc is CAUGHT and the REPL continues" is constructible on a host only if the host
  //! catches it -- and under `linux_x86_64_asan` a use-after-free is then reported AT THE READ with
  //! the allocating and freeing stacks attached, instead of inferred from a `gcstate 0x04` in a
  //! serial transcript.  [B652]
  //!
  //! It is also a real behaviour gap in its own right: a host tool that hits the memory cap should
  //! be able to say so and keep its session, for the same reason [B520] gives for the board.
  const LL_int32 __depth_in__ = rootstack.depth();
  try {
    lamb->loop();
  }
  catch (Sexpr_t __err__) {
    //! AN UNCAUGHT USER `raise` NAMES WHAT WAS RAISED.  The per-frame `ll_catch` logging is gated
    //! off for a user raise -- it is control flow, not a fault, and 5,000 raises emit 25,000 lines
    //! -- so THIS is the only place that says anything about one.  [B765]
    //!
    //! KEPT IN STEP WITH THE MACRO ON PURPOSE.  These blocks are the hand-written copy the comment
    //! below is about.  Adding the branch here as well as in `ll_catch_terminal` is the lesser evil
    //! while the collapse waits on [B652]; adding it to only one copy is how the two drift apart.
    if (__err__ && LL_ERR_IS_USER_RAISE(__err__))
      global_printf("\r[%lu] %s ll_catch_terminal: UNCAUGHT raise: %s\n",
                    millis(), me, __err__->error_get_irritants()->str().c_str());
    else if (__err__ && __err__->type() == Cell::T_ERROR)
      global_printf("\r[%lu] %s ll_catch_terminal: %s\n", millis(), me, __err__->error_get_chars());
    else
      //! PRINT THE TYPE.  These two blocks are a HAND-WRITTEN COPY of the `ll_catch_terminal`
      //! macro in ll_platform_generic.h.  The macro prints `non-error type %d`; a copy that prints
      //! `non-error type` with no number leaves a transcript that cannot say WHAT arrived, which is
      //! the hand-maintained-second-copy defect CLAUDE.md names.  Collapse the copies onto the
      //! macro once the depth probe below is settled.  [B652]
      //! DO NOT PRINT `str()` OF THIS OBJECT.  An uncaught `(raise 'kaboom)` reaches here as a
      //! NON-T_ERROR of type 26 whose `str()` is **28,733 bytes of repeated `#UNDEF`**, which blows
      //! the 8,192-byte va_list cap and buries the one useful datum -- the type -- under a
      //! screenful of nothing.  The type number alone is strictly more informative than the value
      //! when the value prints like that.  Why the `__raise__` T_ERROR wrapper is not what arrives
      //! here is a separate defect; see [B765].
      global_printf("\r[%lu] %s ll_catch_terminal: non-error type %d\n",
                    millis(), me, (int) __err__->type());
    global_printf("  rootstack.depth: entry=%s catch=%s\n",
                  ascii.dec(__depth_in__), AsciiConverter().dec(rootstack.depth()));
    ll_debug_catcher;
  }
  catch (std::bad_alloc &) {
    global_printf("\r[%lu] %s OUT OF MEMORY (std::bad_alloc) -- allocation refused, continuing.\n",
                  millis(), me);
    global_printf("  rootstack.depth: entry=%s catch=%s\n",
                  ascii.dec(__depth_in__), AsciiConverter().dec(rootstack.depth()));
  }
#else
  //! See the note above setup().  `ll_catch_terminal` is `catch (Sexpr_t)` and nothing else, so
  //! without this arm every other exception type reaches std::terminate and the board reboots
  //! mute.  [B349]
    //! MEASURE THE ROOT STACK ACROSS THE UNWIND -- the depth at this catch against the depth at
    //! entry.  It is INSTRUMENTATION, not a fix: it is here to answer whether an unwind through
    //! `std::bad_alloc` leaves the root stack where it found it, and equal depths are as useful an
    //! answer as unequal ones.  [B652]
    const LL_int32 __depth_in__ = rootstack.depth();
  try {
    lamb->loop();
  }
  catch (Sexpr_t __err__) {
    //! PRINT THE TYPE.  These two blocks are a HAND-WRITTEN COPY of the `ll_catch_terminal` macro
    //! in ll_platform_generic.h.  The macro prints `non-error type %d`; a copy that prints
    //! `non-error type` with no number leaves a transcript that cannot say WHAT arrived, which is
    //! the hand-maintained-second-copy defect CLAUDE.md names.  Collapse the copies onto the
    //! macro.  [B652]
    if (__err__ && __err__->type() == Cell::T_ERROR) {
      global_printf("\r[%lu] %s ll_catch_terminal: %s\n", millis(), me, __err__->error_get_chars());
    }
    else {
      global_printf("\r[%lu] %s ll_catch_terminal: non-error type %d\n",
                    millis(), me, (int) __err__->type());
    }
    //! THE DEPTH LINE IS DELIBERATELY OUTSIDE BOTH ARMS, and the braces above exist to say so.
    //! It was already unconditional -- the `else` took only the printf above it -- but the
    //! INDENTATION read as though it belonged to the else, so the code and its appearance
    //! disagreed about which arm prints the probe.  Unbraced, that is one added line away from a
    //! silent behaviour change.  The probe WANTS both arms: a T_ERROR unwind is the common case
    //! and reporting depth only for the exotic non-error one would have measured almost nothing.
    global_printf("  rootstack.depth: entry=%s catch=%s\n",
                  ascii.dec(__depth_in__), AsciiConverter().dec(rootstack.depth()));
    ll_debug_catcher;
  }
  //! OUT OF MEMORY IS NOT A REASON TO REBOOT THE BOARD.  It is a CONDITION, not a corruption: the
  //! allocation failed, nothing was written, nothing is inconsistent.  The runtime claims ~1.2 MB of
  //! a 2 MB part before user code runs, so exhausting the remainder is arithmetic a program can
  //! legitimately hit -- and a program that hits it should be able to be TOLD, not silently
  //! restarted underneath its author.
  //!
  //! SO: report and RETURN.  `loop()` is re-entered by the Arduino main loop, so returning ends this
  //! iteration and keeps the REPL, the session and the serial link alive -- which is the difference
  //! between a program that reports "out of memory" and one that reports nothing because the board
  //! rebooted mid-sentence.  We do NOT abort and we do NOT pretend it succeeded.
  //!
  //! AND A SCHEME PROGRAM CAN CATCH THIS ITSELF: `ll_catch` converts `std::bad_alloc` to an ordinary
  //! LambLisp error at the first handler on the way up, so `(guard (e (#t ...)) ...)` around an
  //! allocating expression works.  Reaching this arm means nothing in the program's own code chose
  //! to handle it.
  catch (std::bad_alloc &) {
    global_printf("\r[%lu] %s OUT OF MEMORY (std::bad_alloc) -- allocation refused, continuing.\n"
                  "  This is a memory BUDGET limit, not corruption.  The board is NOT being\n"
                  "  reset; the failed operation is abandoned and the REPL continues.\n", millis(), me);
      global_printf("  rootstack.depth: entry=%s catch=%s\n",
                    ascii.dec(__depth_in__), AsciiConverter().dec(rootstack.depth()));
    ll_debug_catcher;
  }
  catch (std::exception &e) {
    global_printf("\r[%lu] %s FATAL uncaught C++ exception: %s -- aborting\n", millis(), me, e.what());
    abort();
  }
  catch (...) {
    global_printf("\r[%lu] %s FATAL uncaught C++ exception of unknown type -- aborting\n", millis(), me);
    abort();
  }
#endif
}

#if !LL_POSIX
size_t getArduinoLoopTaskStackSize(void) { return 49152; }  //!< 48 KB loopTask stack: leaves ~48 KB more internal DRAM for LittleFS DMA reads.  Eval guard LL_EVAL_STACK_BUDGET is sized to match.  [B95]
#endif

#if LL_POSIX

// R7RS (scheme process-context) command-line: capture argv so (command-line) can return it.
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

int          g_ll_argc = 0;
const char **g_ll_argv = nullptr;

/*! @brief `--teardown-test N` -- construct and destroy N Lamb instances, reporting RSS each time.

  Exercises ~Lamb(), which until 2026-08-24 did not exist: nothing in the tree ever tore an
  instance down, so every allocation the VM makes had an untested release path.  A flat RSS across
  cycles means the teardown returns what setup took; a rising one names the leak's size per cycle.

  SEQUENTIAL ONLY.  `sint_cache_cells` is a global, so two LIVE instances would share it and the
  first teardown would free it under the survivor.  This loop destroys each instance before
  building the next.
*/
static int ll_teardown_test(int cycles)
{
  //! RSS cannot separate a real leak from allocator retention, so report mallinfo's in-use bytes
  //! alongside it: uordblks rising per cycle is a genuine leak, RSS alone rising is not.
  auto inuse_kb = []() -> long {
#if defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
    return (long) (mi.uordblks / 1024);
#else
    return -1;
#endif
  };
  auto rss_kb = []() -> long {
#if LL_WINDOWS
    //! P174: -1 == "this host cannot tell us", the SAME convention inuse_kb() above already uses
    //! for a non-glibc libc, and reported as such rather than as a plausible 0.  Windows has
    //! neither /proc/self/statm nor sysconf(); its answer is GetProcessMemoryInfo() from psapi,
    //! which is not written because --teardown-test is a LEAK-HUNTING DIAGNOSTIC for the hosts we
    //! develop on, and P174 Phase 1 is a language demo.  Add it to ll_platform_Windows.cpp if a
    //! leak ever needs chasing on Windows specifically; until then a missing number is honest and
    //! a wrong one is not.
    return -1;
#else
    long sz = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &sz, &res) != 2) res = 0; fclose(f); }
    return res * (sysconf(_SC_PAGESIZE) / 1024);
#endif
  };
  printf("teardown-test: %d cycles\n", cycles);
  long base = 0;
  for (int i = 1; i <= cycles; i++) {
    lamb = new Lamb;
    lamb->setup();
    delete lamb;
    lamb = nullptr;
    long r = rss_kb();
    if (i == 1) base = r;
    long u = inuse_kb();
    static long ubase = 0; if (i == 1) ubase = u;
    printf("  cycle %2d  rss=%ld kB (%+ld)  malloc-inuse=%ld kB (%+ld)\n",
           i, r, r - base, u, u - ubase);
    fflush(stdout);
  }
  return 0;
}

int main(int argc, const char **v)
{
  g_ll_argc = argc;
  g_ll_argv = v;
  if (argc >= 2 && strcmp(v[1], "--teardown-test") == 0)
    return ll_teardown_test(argc >= 3 ? atoi(v[2]) : 5);
  // Never let a thrown LambLisp error object escape main -> std::terminate -> SIGABRT.  A startup
  // file-error (mk_input_file_port's %file-error) or an OOM syserror under memory pressure would
  // otherwise abort the whole process with no diagnostic; report it and exit cleanly instead.
  // [B182]
  try {
    setup();
    while (true) loop();
  }
  catch (Sexpr_t err) {
    fprintf(stderr, "\nLambLisp fatal: uncaught error object\n");
    if (err) { try { String s = err->str(); fprintf(stderr, "  %s\n", s.c_str()); } catch (...) {} }
    return 1;
  }
  catch (...) {
    fprintf(stderr, "\nLambLisp fatal: uncaught C++ exception\n");
    return 1;
  }
}
#endif
