// Copyright 2026 by Frobenius Norm LLC 2026-05-16
// Free for non-commercial use. Commercial use requires a license.
#if LL_ESP32

#include "LittleFS.h"   //!< fs_total()/fs_used() [B627]
#include "soc/soc_memory_types.h"   //!< esp_ptr_external_ram() for mem_placement() [B593]
#include "ll_platform_generic.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
//! esp_chip_info.h / esp_flash.h must be included EXPLICITLY.  Up to ESP-IDF 4.4 both rode in
//! transitively via esp_system.h; IDF 5 stopped doing that, and the failure names the SYMBOLS
//! rather than the header -- "'esp_chip_info_t' was not declared in this scope",
//! "'esp_flash_get_size' was not declared", "'CHIP_FEATURE_EMB_FLASH' was not declared" -- which
//! reads like the API was removed rather than like a missing #include.  Both headers exist in 4.4
//! as well, so including them unconditionally is correct on either IDF.
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"   //!< ESP_IDF_VERSION_MAJOR -- esp_chip_info_t differs across IDF 4/5

void LambPlatform::begin()
{
  ME("LambPlatform::LambPlatform()");
  
  loop_start_ms = millis();
  loop_start_us = micros();
  identification();
}

void LambPlatform::end() {}

void LambPlatform::loop(void)	//call this 1st thing in Lamb::loop().
{
  loop_start_ms = millis();
  loop_start_us = micros();
}

void LambPlatform::reboot()			{ esp_restart(); }
#if LL_ARDUINO && !LL_ESP_ARDUINO
//! LLArduino timing on the ESP32, over Apache-2.0 IDF primitives -- P176 Tier 1.  The vendor
//! Arduino core is NOT linked in this build, so millis/micros/delay come from esp_timer here.
//! delay_ms YIELDS (vTaskDelay), matching what Arduino's own delay() does under FreeRTOS: other
//! tasks, the idle task and the watchdog run during a delay -- a busy-wait would starve them on a
//! real-time runtime.  delayMicroseconds / delay_us BUSY-WAIT (esp_rom_delay_us) because sub-tick
//! precision cannot come from vTaskDelay, whose resolution is one FreeRTOS tick (typ. 1 ms).  This
//! REPLACES the shared POSIX delayMicroseconds in ll_llarduino_null.cpp, which is guarded off on
//! LL_ESP32 for exactly this reason.
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
unsigned long micros(void)                { return (unsigned long) esp_timer_get_time(); }
unsigned long millis(void)                { return micros() / 1000UL; }
void delay_us(unsigned long us)           { esp_rom_delay_us(us); }
void delayMicroseconds(unsigned long us)  { esp_rom_delay_us(us); }
void delay_ms(unsigned long ms)           { vTaskDelay(pdMS_TO_TICKS(ms)); }
#else
void delay_ms(unsigned long ms)			{ delay(ms); }
#endif
const char *LambPlatform::name()		{ return "ESP32"; }
LL_int32 LambPlatform::free_heap()			{ return (LL_int32) esp_get_free_heap_size(); }
//! The clock the chip is ACTUALLY running at.  `board_build.f_cpu` in the board JSON sets the
//! `F_CPU` macro and does NOT set the hardware clock -- the IDF boot configuration does -- so a
//! 240 MHz build can boot at 160.  `identification()` prints this once at platform init, which is
//! ~3.4 s before a perf transcript starts capturing, so a benchmark run must ask for it again at
//! a moment of its own choosing if the figure is to travel with the timing.
LL_int32 LambPlatform::cpu_mhz()			{ return (LL_int32) ESP.getCpuFreqMHz(); }
//! B129: cell blocks come from PSRAM (heap_caps_malloc MALLOC_CAP_SPIRAM, ll_vm_mem.cpp), so the
//! expansion guard must measure PSRAM -- not the aggregate.  Falls back to internal when the part
//! has no PSRAM, matching expand()'s own allocation fallback.
LL_int32 LambPlatform::cell_pool_free()
{
  size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (ps > 0) return (LL_int32) ps;
  return (LL_int32) heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

/*! [B593] Cell-block allocation, the ESP32 arm.  See ll_platform_generic.h for why this exists.
    B115 (a B95 sibling): cell blocks, like payloads, must be allocated EXPLICITLY from PSRAM.
    Plain `new`/malloc does not reliably land there -- the IDF keeps allocations internal below the
    threshold `identification()` sets -- so a large single cell block exhausts internal DRAM and
    starves the littlefs read buffers, which surfaces as ENOMEM (err 257) in a subsystem that never
    allocated a cell.  Prefer PSRAM; fall back to internal only if PSRAM is absent, so a part
    without PSRAM still runs. */
void *LambPlatform::cell_block_alloc(unsigned long nbytes)
{
  void *blk = heap_caps_malloc((size_t) nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!blk) blk = heap_caps_malloc((size_t) nbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return blk;
}

void  LambPlatform::cell_block_free(void *blk)
{ if (blk) heap_caps_free(blk); }

//! INTERNAL-vs-PSRAM placement.  Behind the platform interface so the memory report in
//! ll_vm_mem.cpp needs no `#if LL_ESP32` of its own.  [B593] [B117]
//! BINARY: internal-vs-PSRAM, nothing finer.  A buffer taken from `ll_fastram_alloc_bytes()` and
//! one from an ordinary `new[]` that happened to land in internal DRAM both answer `INTERNAL`, so
//! this cannot confirm that a `use_fastram()` request was honoured.  [B975]
const char *LambPlatform::mem_placement(void *p)
{ return esp_ptr_external_ram(p) ? "PSRAM   " : "INTERNAL"; }

//! [B627] LittleFS knows both numbers and nothing exposed them.  See ll_platform_generic.h for the
//! measured delta this makes visible.
LL_int64 LambPlatform::fs_total()  { return (LL_int64) LittleFS.totalBytes(); }
LL_int64 LambPlatform::fs_used()   { return (LL_int64) LittleFS.usedBytes(); }
//! [B508] INTERNAL DRAM, SEPARATELY.  `free_heap()` is `esp_get_free_heap_size()` -- the AGGREGATE
//! of internal DRAM and PSRAM -- and `cell_pool_free()` reports PSRAM.  Neither can see internal
//! DRAM going, and that is where lwIP puts its small allocations.
//! lwIP's PCBs and socket structures are a few hundred bytes, so they fall UNDER the runtime
//! internal-DRAM threshold set in `identification()` below -- read that block for the number;
//! do not restate it here.  A TCP send buffer (CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5744) does not,
//! and CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is unset, so nothing steers lwIP to PSRAM wholesale.
//! EXPOSED TO SCHEME ON PURPOSE: without it a probe can only watch `free_heap()` and
//! `cell_pool_free()`, i.e. the two heaps that are fine while this one drains.  [B508]
LL_int32 LambPlatform::free_internal()   { return (LL_int32) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
LL_int32 LambPlatform::free_dma()        { return (LL_int32) heap_caps_get_free_size(MALLOC_CAP_DMA); }
//! [B609] LARGEST CONTIGUOUS BLOCK, WHICH IS A DIFFERENT QUESTION FROM HOW MUCH IS FREE.
//! free_internal() falls when memory is OCCUPIED and also when it is merely FRAGMENTED, and those
//! need opposite fixes: a leak means something is not being released, fragmentation means the same
//! bytes are free but unusable in one piece.  A ~100 B/cycle fall that survives a forced collection
//! is consistent with BOTH, so the pair has to be read together -- if this stays flat while
//! free_internal() falls, there is no leak.  An allocator fails on THIS number, not on free size.
LL_int32 LambPlatform::largest_free_internal() { return (LL_int32) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
//! [B676] MALLOC_CAP_DMA, not MALLOC_CAP_INTERNAL.  DMA-capable is a STRICTER capability than
//! internal on this part, and it is the one a camera, an I2S ring or a FreeRTOS queue actually
//! needs -- so internal headroom can look adequate while every DMA request refuses.  Reported as
//! LARGEST CONTIGUOUS for the reason ncg_exec_pool_init() already records: free is not the number
//! that decides a contiguous request, and printing it explained a refusal with the wrong figure.
LL_int32 LambPlatform::largest_free_dma() { return (LL_int32) heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }
LL_int32 LambPlatform::free_stack()		{ return (LL_int32) uxTaskGetStackHighWaterMark(NULL); }
void  LambPlatform::rand(byte *buf, LL_int32 len)	{ esp_fill_random(buf, (size_t) len); }


Bool_t LambPlatform::heap_integrity_check(Bool_t complain)
{
  //ll_every(10000, global_printf("LambPlatform::heap_integrity_check() happening now\n")); return heap_caps_check_integrity_all(true);
  const char msg[] = "==> LambPlatform::heap_integrity_check() failed <==\n";
  bool valid = heap_caps_check_integrity_all(complain);
  if (!valid) {
    LambStdio.write(msg, strlen(msg));
    delay(100);
  }
  
  return valid;
}

void LambPlatform::identification(void)
{
  ME("LambPlatform::identification()");
  
  const char *chip_names[] = { "NONE0", "ESP32", "ESP32_S2", "NONE3", "NONE4", "ESP32-C3","ESP32-H2", "NONE7", "NONE8", "ESP32-S3", "NONE10" };
  const unsigned Nchips    = sizeof(chip_names)/sizeof(chip_names[0]);

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  if (chip_info.model >= Nchips) chip_info.model = (esp_chip_model_t) 0;
  
  bool hasFlash = chip_info.features & CHIP_FEATURE_EMB_FLASH; 
  uint32_t flash_size = 0;

  if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) global_printf("%s Get flash size failed", me);

  //! esp_chip_info_t CHANGED SHAPE IN ESP-IDF 5, and the two fields did not merely get renamed --
  //! `revision` CHANGED MEANING.  Up to 4.4 it was a uint8_t holding the wafer MAJOR version, with a
  //! separate uint16_t `full_revision` holding MXX (major*100 + minor).  IDF 5 deleted
  //! `full_revision` and widened `revision` to uint16_t carrying MXX itself.  So a build that just
  //! drops the missing field does NOT keep printing the same thing: `rev` silently changes from 0
  //! to 0..99-scaled MXX, and nothing warns, because both are integers that print fine.  Derive
  //! both values explicitly so the two IDFs produce identical output.
#if ESP_IDF_VERSION_MAJOR >= 5
  const unsigned chip_rev_major = (unsigned) chip_info.revision / 100u;   //!< MXX -> M
  const unsigned chip_rev_full  = (unsigned) chip_info.revision;          //!< already MXX
#else
  const unsigned chip_rev_major = (unsigned) chip_info.revision;          //!< 4.x: major only
  const unsigned chip_rev_full  = (unsigned) chip_info.full_revision;     //!< 4.x: MXX
#endif

  global_printf("\n");
  global_printf("%s IDF_target : %s, chip_model : %d, model_name %s, rev %u, full_rev %u\n",
		me, CONFIG_IDF_TARGET, chip_info.model, chip_names[((unsigned) chip_info.model) % Nchips], chip_rev_major, chip_rev_full);
  global_printf("%s cores : %d, embedded flash : %lu, features 0x%08x\n",
		me, chip_info.cores, (unsigned long) flash_size, chip_info.features);

#define yesno(_tf_) ((_tf_) ? "yes" : "no")
  global_printf("%s EMB_FLASH : %s, EMB_PSRAM : %s, 80211 : %s, 802154 : %s, BT : %s, BLE : %s\n",
		me,
		yesno(chip_info.features & CHIP_FEATURE_EMB_FLASH),
		yesno(chip_info.features & CHIP_FEATURE_EMB_PSRAM),
		yesno(chip_info.features & CHIP_FEATURE_WIFI_BGN),
		yesno(chip_info.features & CHIP_FEATURE_IEEE802154),
		yesno(chip_info.features & CHIP_FEATURE_BT),
		yesno(chip_info.features & CHIP_FEATURE_BLE)
		);
#undef yesno
  //! THE INTERNAL-DRAM THRESHOLD FOR THE WHOLE PROJECT IS SET HERE, AND IT IS 1 KB.  Every
  //! capability-less allocation -- plain `new`, `malloc`, an Arduino `String`, a `realloc` --
  //! resolves to `heap_caps_malloc_default()`, which consults this one static and TRIES PSRAM
  //! FIRST only for sizes STRICTLY GREATER than the limit.  At or below it, internal DRAM first.
  //! This is the ONE place the number lives; every other site names this call and points here.
  //!
  //! DO NOT QUOTE `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` AS THE THRESHOLD.  That symbol is only
  //! what the IDF installs while bringing PSRAM up (4096 in the Arduino sdkconfig).  THIS CALL
  //! RUNS LATER AND OVERWRITES IT WITH 1024 -- same static, last writer wins -- on every ESP32
  //! env, with no per-part guard: S3, classic WROVER and RISC-V C5 alike.
  //!
  //! IT IS A PREFERENCE, NOT A PIN.  After choosing a pool by size,
  //! `heap_caps_malloc_default()` retries "while being less picky", so a sub-1 KB allocation
  //! LANDS IN PSRAM once internal DRAM is exhausted.  The consequence is the nasty one:
  //! INTERNAL EXHAUSTION DOES NOT FAIL AT THE MALLOC THAT EXHAUSTED IT.  It degrades silently and
  //! the error surfaces at some later caller that genuinely needed DMA-capable or interrupt-safe
  //! memory -- different subsystem, different stack, no mention of the allocation that spent the
  //! budget.  Assume this shape before blaming the reporter.
  //!
  //! THE DEFAULT IS "EXTERNAL DISABLED", which is why the call takes a limit at all: the static
  //! initialises to MALLOC_DISABLE_EXTERNAL_ALLOCS (-1), in which state EVERY capability-less
  //! allocation is internal-only at any size.  Enabling PSRAM for `malloc` and setting the
  //! threshold are ONE operation -- there is no way to say "enable, keep the current limit".
  //!
  //! THE RULE THAT FOLLOWS: allocate a bulk VM payload with an explicit `MALLOC_CAP_SPIRAM` --
  //! use `ll_payload_alloc_bytes` -- never with `new`/`malloc`.  Sizing something past a
  //! threshold is NOT a way to move it to PSRAM, and for anything allocated with an explicit
  //! capability (the UART rings, lwIP's pools) this threshold does not apply at all.
  //!
  //! UNRELATED TO `-mfix-esp32-psram-cache-issue`, though both are PSRAM and both are ours: that
  //! is the CPU-3.2 silicon erratum workaround for ESP32-classic (LX6) rev < 3, a code-generation
  //! filter.  It changes what the compiler EMITS, not where the allocator PUTS things, and it
  //! lives in the `chip_flags` of the two LX6 boards in `boards.json`.
  //!
  //! THE API IS UNDOCUMENTED UPSTREAM: `heap_caps_malloc_extmem_enable` appears in
  //! `esp_heap_caps.h` and `heap_caps.c` and NOWHERE in the ESP-IDF programming guide.  Read the
  //! implementation, not the docs -- there are none.  [P211] has why the value is 1 KB (deduced
  //! from the direction of the change, never written down by anyone), the three measurements that
  //! agree with it -- [B95], [P211], [B411] -- and the 4096-vs-1024 confusion this replaced.
  heap_caps_malloc_extmem_enable(0x400);   //!< 1 KB -- see the block above before changing this.
  global_printf("Heap free:       %d / %d\n",  ESP.getFreeHeap(), ESP.getHeapSize());
  global_printf("Heap max alloc:  %d\n",       ESP.getMaxAllocHeap());
  global_printf("Heap min free:   %d\n",       ESP.getMinFreeHeap());
  global_printf("Chip model       %s rev %d\n",ESP.getChipModel(), ESP.getChipRevision());
  global_printf("PSRAM free:      %d / %d\n",  ESP.getFreePsram(), ESP.getPsramSize());
  global_printf("PSRAM max alloc: %d\n",       ESP.getMaxAllocPsram());
  global_printf("PSRAM min free:  %d\n",       ESP.getMinFreePsram());
  global_printf("Flash size:      %dK\n",      ESP.getFlashChipSize() / 1024);
  global_printf("Flash speed:     %u\n",       ESP.getFlashChipSpeed());
  global_printf("Flash mode:      %d\n",       ESP.getFlashChipMode());
  global_printf("Chip revision:   %d\n",       ESP.getChipRevision());
  global_printf("Chip model:      %s\n",       ESP.getChipModel());
  global_printf("Chip cores:      %d\n",       ESP.getChipCores());
  global_printf("Chip freq:       %u Mhz\n",   ESP.getCpuFreqMHz());
  global_printf("Cycle count:     %u\n",       ESP.getCycleCount());
  global_printf("SDK version:     %s\n",       ESP.getSdkVersion());
  global_printf("Sketch size:     %u\n",       ESP.getSketchSize());
  global_printf("Sketch MD5:      <skipped>\n" /*%s, ESP.getSketchMD5().c_str()*/);	//takes too long
  global_printf("Sketch free:     %u\n",       ESP.getFreeSketchSpace());
  delay(10);
  //DEBT review above for completeness
}

#endif
