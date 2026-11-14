// Copyright 2026 by Frobenius Norm LLC 2026-05-16
// Free for non-commercial use. Commercial use requires a license.
#include "ll_platform_generic.h"

#include <stdio.h>
#include <string.h>

AsciiConverter ascii;

#include <stdlib.h>
#if LL_ESP32
#include "esp_heap_caps.h"
#endif

/*! Allocate a bulk VM payload, preferring PSRAM (see ll_platform_generic.h).  [B95]
  The PSRAM probe is done ONCE and cached: on a part without PSRAM every payload would otherwise
  pay a failing heap_caps_malloc before falling back.
*/
#if LL_ESP32
static bool ll_payload_have_psram()
{
  static int psram_ok = -1;
  if (psram_ok < 0) {
    void *probe = heap_caps_malloc(8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psram_ok = probe ? 1 : 0;
    if (probe) heap_caps_free(probe);
  }
  return psram_ok;
}
#endif

/*! PSRAM ONLY on a part that has it: NULL when PSRAM is full, NEVER internal DRAM.  Internal DRAM
  is not a heap extension for payloads -- it holds only what is placed there on purpose.  A miss is
  the caller's to handle: `Lamb::payload_alloc()` collects and retries, then raises.  [B1041] */
void *ll_payload_alloc_bytes(size_t nbytes)
{
  if (nbytes == 0) nbytes = 1;      //!< new T[0] yields a unique non-null pointer; match that
#if LL_ESP32
  if (ll_payload_have_psram()) return heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  return malloc(nbytes);
}

void ll_payload_free(void *p)
{
  free(p);                          //!< free() handles heap_caps_malloc blocks on the IDF
}

/*! [P254] Allocate a HOT, LONG-LIVED, SMALL VM structure, preferring INTERNAL DRAM.  The mirror of
  ll_payload_alloc_bytes() above -- read the header block beside its declaration for which
  structures qualify and why, and for why uPy's speed on small integers is NOT this mechanism.

  NO PER-PART GUARD, deliberately.  Every ESP32 env takes this path -- S3, classic WROVER, RISC-V
  C5 -- exactly as
  `heap_caps_malloc_extmem_enable()` in identification() has no per-part guard either.  The parts
  differ in how much internal SRAM they have, not in whether a hot structure belongs there, and the
  fallback below is what handles the difference.

  FALLS BACK TO PSRAM AND THEN TO malloc, AND THE ORDER MATTERS.  Internal first (the point);
  PSRAM next, so a part that cannot spare 72 KB of internal still BOOTS with the cache merely slow
  rather than absent; malloc last for the hosts and any non-PSRAM part.  Returning null here would
  make a performance preference into a boot failure, which is the opposite of the trade P254 makes.

  The PSRAM probe is NOT cached the way ll_payload_alloc_bytes caches its own: this runs a handful
  of times per process (two structures, plus a rootstack upsize now and then), so a cache would add
  a static to save nothing measurable.
*/
void *ll_fastram_alloc_bytes(size_t nbytes)
{
  if (nbytes == 0) nbytes = 1;      //!< new T[0] yields a unique non-null pointer; match that
#if LL_ESP32
  void *p = heap_caps_malloc(nbytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (p) return p;                  //!< the intended placement
  p = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;                  //!< internal is full -- slow is better than dead (see above)
#endif
  return malloc(nbytes);
}

void ll_fastram_free(void *p)
{
  free(p);                          //!< free() handles heap_caps_malloc blocks on the IDF
}

//! Definitions for the two globals declared beside `LambPreSettings` -- see the header for why each
//! exists.  `ll_va_list_max` MUST hold a usable value from static init: `va_list_expand()` formats
//! log lines long before `load()` is reachable (there is no filesystem yet), so a zero here would
//! cap every early message at nothing.
LambPreSettings ll_pre_settings;
LL_int32        ll_va_list_max = (LL_int32) toString_MAX_LENGTH;
LL_int32        ll_cell_block_size_effective = 0;   //!< [B859] 0 = the request was honoured

//! Inclusive bounds for `va_list_max`.  The FLOOR is the start size -- a cap below the buffer the
//! function begins with would make the very first message a truncation.  The CEILING is
//! `autobuf_t`'s own `_max_`: past it `upsize()` raises [B175], and raising inside the formatter
//! means throwing while building the message the throw needs formatted.
#define LL_VA_LIST_MAX_FLOOR   1024
#define LL_VA_LIST_MAX_CEILING 65536

//! One `key . value` pair -> one field.  Returns false when the key is none of ours, so the caller
//! can SAY SO rather than let an unrecognised key fall off the end of the chain.  [B888]
bool LambPreSettings::apply_key(const char *key, long val)
{
  if      (strcmp(key, "cell_block_size")      == 0) cell_block_size      = (LL_int32) val;
  else if (strcmp(key, "extension_block_size") == 0) extension_block_size = (LL_int32) val;
  else if (strcmp(key, "gc_total_marked")      == 0) gc_total_marked      = (LL_int32) val;   //!< B247
  else if (strcmp(key, "gc_pool_target")       == 0) gc_pool_target       = (LL_int32) val;   //!< B610
  else if (strcmp(key, "ncg_frame_pool_size")  == 0) ncg_frame_pool_size  = (LL_int32) val;
  else if (strcmp(key, "pool_reserve_pct")      == 0) pool_reserve_pct      = (LL_int32) val;
  else if (strcmp(key, "pool_reserve_bytes")    == 0) pool_reserve_bytes    = (LL_int32) val;
  else if (strcmp(key, "gc_persist")           == 0) gc_persist           = (LL_int32) val;   //!< [P311] 4.1
  else if (strcmp(key, "sint_in_iram")         == 0) sint_in_iram         = (LL_int32) val;   //!< P285
  else if (strcmp(key, "rootstack_in_iram")    == 0) rootstack_in_iram    = (LL_int32) val;   //!< P285
  else if (strcmp(key, "rootstack_max")        == 0) rootstack_max        = (LL_int32) val;   //!< P298
  else if (strcmp(key, "rootstack_init")       == 0) rootstack_init       = (LL_int32) val;   //!< P298
  else if (strcmp(key, "markstack_init")       == 0) markstack_init       = (LL_int32) val;   //!< P298
  else if (strcmp(key, "va_list_max")          == 0) va_list_max          = (LL_int32) val;
  else if (strcmp(key, "boot_pending")         == 0) boot_pending         = (LL_int32) val;   //!< B441
  else return false;
  return true;
}

void LambPreSettings::load()
{
  LL_File *f = ll_file_system.open(LAMB_SETTINGS_PATH, "r");
  if (!f || !f->isOpen()) { delete f; return; }
  char line[256], key[64];
  long val;
  int len = 0, c;
  bool overflow = false;
  while (true) {
    c = f->read();
    if (c == '\n' || c == EOF) {
      line[len] = '\0';
      if (len > 0) {
        const char *p = line;
        //! PARSE EVERY PAIR ON THE LINE, NOT THE FIRST ONE ONLY.  THIS LOOP IS LOAD-BEARING -- if
        //! you collapse it back to a single `sscanf` you restore a defect that MANUFACTURES CONTROL
        //! ARMS THAT LOOK LIKE TREATMENT ARMS.  A hand-written
        //!     ((cell_block_size . 32768) (gc_pool_target . 32768) (va_list_max . 8301))
        //! is a correct s-expression -- it is how this file's own shipped two keys are laid out,
        //! only without the newline -- and a single `sscanf` takes `cell_block_size` and SILENTLY
        //! DISCARDS the rest.  The run then measures the default configuration while the file on
        //! disk goes on displaying the numbers that were asked for, so the operator reads "that
        //! setting has no effect" -- a conclusion about the VM drawn from this parser.  [B888]
        //!
        //! THE ADVANCE IS THE PART THAT IS EASY TO GET WRONG, so it is spelt out.  After matching
        //! `a . 1` in `a . 1) (b . 2))` the remaining text starts `) (` -- so stepping past the
        //! matched characters is not enough; the skip must tolerate the closing paren, the next
        //! opening paren and the whitespace between them.  `%n` (which does NOT count toward
        //! sscanf's return, so the `== 2` test is unaffected) reports how far the match reached.
        //! A fix that only handles the unparenthesised `a . 1 b . 2` passes a naive test and fails
        //! every real file.
        //!
        //! `;` IS DELIBERATELY NOT IN THE SKIP SET.  A comment must STOP the line's parse, not be
        //! stepped over, or `; 8192 = default block 1` in the shipped file's own prose would be
        //! mined for keys.  That is also what keeps [B859]'s effective-size line a comment rather
        //! than a value (see `ll_cell_block_size_effective`).
        int npairs = 0;
        while (true) {
          while (*p == '(' || *p == ')' || *p == ' ' || *p == '\t' || *p == '\r') p++;
          int consumed = -1;
          if (sscanf(p, "%63[a-zA-Z0-9_] . %ld%n", key, &val, &consumed) != 2) break;
          if (consumed <= 0) break;         //!< cannot happen (%63[...] needs >=1 char); never loop forever on it
          if (!apply_key(key, val))
            global_printf("[settings] unknown key '%s' (value %s) in " LAMB_SETTINGS_PATH
                          " -- IGNORED, and dropped from the file on the next rewrite\n",
                          key, ascii.dec((LL_int64) val));
          npairs++;
          p += consumed;
        }
        //! AND SAY SO WHEN SOMETHING ON THE LINE WAS NOT UNDERSTOOD.  Parsing the whole line
        //! handles the alist layout; this catches the rest -- a mistyped separator, a non-integer
        //! value, a stray word -- which the loop above would otherwise discard in silence.
        //! The property that matters is LOUD,
        //! not correct: a settings file is the input to every provisioning experiment here, and an
        //! input that is quietly not what it says produces a measurement nobody can tell from a
        //! real one.  Trailing `)`/whitespace is consumed above and a `;` comment is accepted, so
        //! this is silent on every file the VM writes and on both shipped defaults.  [B888]
        while (*p == '(' || *p == ')' || *p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p != '\0' && *p != ';')
          global_printf("[settings] " LAMB_SETTINGS_PATH ": %s pair(s) read from this line, then "
                        "unparsed text IGNORED: '%s'\n", ascii.dec((LL_int32) npairs), p);
        //! A LINE LONGER THAN THE BUFFER IS TRUNCATED, AND EVERY PAIR ON IT COUNTS.  The character
        //! loop below drops bytes past `sizeof(line)-1`, so a 256-byte cut can lose later keys --
        //! and if it lands mid-number, `%ld` happily reads the surviving digits and stores a value
        //! that is in the file NOWHERE.  That is the same silently-wrong-input failure one layer
        //! down, so it is reported rather than absorbed.
        //! Comment lines are exempt: prose is allowed to be long and nothing reads it.  [B888]
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (overflow && *q != ';')
          global_printf("[settings] " LAMB_SETTINGS_PATH ": a line longer than %s bytes was "
                        "TRUNCATED -- keys past the cut are LOST and a value cut mid-digit reads "
                        "WRONG.  Put one key per line.\n",
                        ascii.dec((LL_int32) (sizeof(line) - 1)));
      }
      len = 0;
      overflow = false;
      if (c == EOF) break;
    }
    else if (len < (int) sizeof(line) - 1) line[len++] = (char) c;
    else overflow = true;   //!< [B888] see the report above; never drop bytes in silence
  }
  f->close();
  delete f;

  //! CLAMP, THEN PUBLISH -- in that order, and never publish the raw value.  An out-of-range
  //! `va_list_max` is not a user error worth refusing a boot over, but it IS unrepresentable
  //! downstream: below the start size every message truncates, above `autobuf_t::_max_` the
  //! formatter raises while formatting.  Clamping here means no consumer has to re-check.
  if (va_list_max < LL_VA_LIST_MAX_FLOOR)   va_list_max = LL_VA_LIST_MAX_FLOOR;
  if (va_list_max > LL_VA_LIST_MAX_CEILING) va_list_max = LL_VA_LIST_MAX_CEILING;
  ll_va_list_max  = va_list_max;

  //! Clamp here so neither reserve site re-checks.  0 would leave payloads nothing; >100 would
  //! make the usable pool negative.  `pool_reserve_bytes` cannot be clamped here -- the pool size
  //! is not known until the heap is built -- so it is capped at use.  [B1030]
  if (pool_reserve_pct < 1)  pool_reserve_pct = 1;
  if (pool_reserve_pct > 90) pool_reserve_pct = 90;
  if (pool_reserve_bytes < 0) pool_reserve_bytes = 0;
  //! NORMALISE TO 0/1 so every consumer can test it as a boolean and a typo cannot mean "maybe".
  //! Any non-zero value is ON, which is the safe direction: a garbled key leaves persistence
  //! WORKING rather than silently disabling the shipping default.  [P311] 4.1
  if (gc_persist != 0) gc_persist = 1;
  //! [P298] Clamp the GC-stack sizes here so neither consumer re-checks; 0 is the
  //! "use the compiled default" sentinel and must survive.
  if (rootstack_max  < 0) rootstack_max  = 0;
  if (rootstack_init < 0) rootstack_init = 0;
  if (markstack_init < 0) markstack_init = 0;
  if (rootstack_max  > 0 && rootstack_max < 1024) rootstack_max = 1024;   //!< below this the VM cannot boot
  if (rootstack_init > 0 && rootstack_max > 0 && rootstack_init > rootstack_max) rootstack_init = rootstack_max;

  //! SELF-RESET A SETTING THAT DID NOT SURVIVE ITS OWN BOOT.  [B441]
  //! `gc_total_marked` is a persisted LIVE SET and the heap is sized at 2x it (ll_vm_mem.cpp).  A
  //! run that drove the live set above what this board can hold therefore writes a number the NEXT
  //! boot cannot satisfy, and `setup()` then throws std::bad_alloc on every reset.  TWO THINGS MAKE
  //! THAT HARD TO ATTRIBUTE: the run that wrote the number SUCCEEDED, and a firmware-only reflash
  //! does not clear the file.
  //! The sentinel makes that self-correcting: if the previous boot set `boot_pending` and never
  //! cleared it, the values it was using did not survive, so drop the risky one and carry on with
  //! the built-in default.  A boot that ignores a bad hint is recoverable; a boot that dies on one
  //! is not, and it dies identically every time.
  //! ONLY `gc_total_marked` IS DISCARDED.  The others are operator intent -- a hand-set
  //! cell_block_size or va_list_max should survive a crash it probably did not cause -- whereas
  //! this one is a machine-written hint whose only defence is that it is cheap to recompute.
  if (boot_pending) {
    if (gc_total_marked > 0) {
      global_printf("[settings] previous boot did not complete with gc_total_marked=%s -- "
                    "discarding it and using the default heap size\n",
                    ascii.dec(gc_total_marked));
      gc_total_marked = 0;
      gc_pool_target  = 0;   //!< [B610] same reasoning: a boot that did not complete proves nothing
    }
  }
  ll_pre_settings = *this;     //!< B417: the GC's rewrite reads this to preserve keys it does not own
  //! ARM THE SENTINEL BEFORE THE RISKY VALUE IS USED, not after.  Written here because everything
  //! downstream -- the heap sizing, the exec pool, setup.scm -- happens after load() returns, and a
  //! sentinel armed after the dangerous step would never see the failure it exists to catch.
  ll_pre_settings_write(1);
}

//! [B1085] Counters for the changed-check below.  `skipped` is the one worth reading: it is how
//! many flash writes the comparison removed, and a run where it stays 0 while `done` climbs means
//! something really is changing every cycle and the stall is back by another route.
LL_int32 _ll_settings_writes_done    = 0;
LL_int32 _ll_settings_writes_skipped = 0;

//! [B1085] Does the file on disk already hold exactly these bytes?  A read of ~1 KB is far cheaper
//! than a flash rewrite, and it is the only way to answer the question across a reboot.
//! FALSE ON ANY DOUBT -- no file, a short file, a long file, or a read that disagrees anywhere.
//! The expensive direction is skipping a write that was needed, so every uncertain case writes.
static bool ll_settings_file_matches(const char *buf, size_t len)
{
  LL_File *f = ll_file_system.open(LAMB_SETTINGS_PATH, "r");
  if (!f || !f->isOpen()) { delete f; return false; }
  bool same = true;
  for (size_t i = 0; i < len; i++) {
    const int c = f->read();
    if (c == EOF || (char) c != buf[i]) { same = false; break; }
  }
  if (same && f->read() != EOF) same = false;      //!< a LONGER file is not a match either
  f->close();
  delete f;
  return same;
}

//! One writer for the settings file, so a key added to LambPreSettings cannot be silently dropped
//! by a caller that does not know about it.  Best-effort: a board with no writable FS simply keeps
//! whatever it had, which is safe.  [B441]
void ll_pre_settings_write(LL_int32 boot_pending)
{
  //! `gc_persist 0` MAKES THIS FILE READ-ONLY.  Every write goes through here -- the geometry
  //! persist from `persist_geometry()`, and the two `boot_pending` writes that bracket startup --
  //! so one gate is the whole switch, and a write added later is covered without being told.
  //! Gating `persist_geometry()` instead would leave `boot_pending` toggling and the file would
  //! still change on every boot.  [P311] 4.1
  if (ll_pre_settings.gc_persist == 0) {
    //! ONCE PER BOOT, not per call: this is reached from the idle task, so a per-call line would
    //! flood the transcript a measurement is trying to read.
    static bool _said = false;
    if (!_said) {
      _said = true;
      global_printf("[settings] gc_persist 0 -- " LAMB_SETTINGS_PATH " NOT written.  The geometry "
                    "will not adapt across boots, and boot_pending is not maintained, so a failed "
                    "startup is not detected.  This is a MEASUREMENT configuration.\n");
    }
    return;
  }
  //! [B1085] RENDER FIRST, THEN WRITE ONLY IF THE CONTENT CHANGED (owner, 2026-11-11).
  //! This used to open the file and `put` straight into it, so every call was a flash write
  //! whether or not a single byte differed -- and the GC calls it on every pool expansion and
  //! every new live-set peak.  Measured on WROVER: ONE such write cost 20-171 ms, and because the
  //! end-of-sweep call site runs on the stack of the `cons` that closed the cycle, that was a
  //! 138 ms stall inside one allocation.  Deferring the call (ll_vm_mem.cpp) takes it off the
  //! mutator's stack; this takes away the write itself when there is nothing new to say.
  //!
  //! THE COMPARISON IS AGAINST THE FILE, NOT AGAINST THE LAST WRITE THIS BOOT.  A remembered
  //! buffer would make the first write of every boot unconditional, which is precisely the case
  //! that matters once the geometry has converged -- the steady state is "nothing has changed
  //! since last time the board ran", and that is a cross-boot statement.
  //!
  //! IT GATES THE ONE WRITER, so every caller and every key added later is covered without being
  //! told -- the same reason `gc_persist` gates here rather than at `persist_geometry()`.
  //! The `boot_pending` bracket writes are NOT suppressed by this and must not be: those two
  //! genuinely differ in content, which is the whole point of the sentinel.
  char   _buf[2560];
  size_t _len = 0;
  bool   _ovf = false;
  auto put = [&](const char *t) {
    const size_t n = strlen(t);
    if (_len + n >= sizeof(_buf)) { _ovf = true; return; }   //!< never silently truncate
    memcpy(_buf + _len, t, n); _len += n;
  };
  put("; Settings-Lamb.scm -- auto-written by the VM.  Reboot required.\n");
  put("; boot_pending 1 means a boot was attempted with these values and did not finish.\n");
  //! SAY SO IN THE FILE WHEN THE FILE IS NOT WHAT RAN.  `cell_block_size` below the boot pool
  //! target is REPLACED BY THE TARGET, so a file that only displayed the request would let a probe
  //! vary the setting, read the file back, and see the setting move while the behaviour stayed put
  //! -- on every workload, which is indistinguishable from "no workload provokes it".  An arm
  //! mislabelled that way reports the wrong contrast rather than no contrast.  [B859] [B761] [B858]
  //!
  //! A COMMENT, NOT THE KEY -- see `ll_cell_block_size_effective` for why writing it back as a
  //! value would be [B248]/[B441] and would latch.  The line is informational, is regenerated from
  //! scratch on every write, and is absent whenever the request was honoured, so its PRESENCE is
  //! the signal.
  if (ll_cell_block_size_effective > 0
      && ll_cell_block_size_effective != ll_pre_settings.cell_block_size) {
    AsciiConverter a1, a2;
    put("; cell_block_size override: requested "); put(a1.dec(ll_pre_settings.cell_block_size));
    put(", EFFECTIVE ");                                put(a2.dec(ll_cell_block_size_effective));
    put(" -- the value below is the REQUEST and is not what the heap was built with.\n");
    put("; The knob that binds the initial block below the pool target is gc_pool_target, not this one.\n");
  }
  put("((cell_block_size . ");      put(ascii.dec(ll_pre_settings.cell_block_size));      put(")\n");
  put(" (gc_pool_target . ");       put(ascii.dec(ll_pre_settings.gc_pool_target));       put(")\n");
  put(" (extension_block_size . "); put(ascii.dec(ll_pre_settings.extension_block_size)); put(")\n");
  put(" (ncg_frame_pool_size . ");  put(ascii.dec(ll_pre_settings.ncg_frame_pool_size));  put(")\n");
  //! WRITE EVERY KEY THE READER ACCEPTS.  This writer REPLACES the file, so a key it does not emit
  //! is DELETED on the first heap expansion -- the [B417] failure, where only `gc_total_marked` was
  //! written and everything else was truncated away.  Caught on the host: a `pool_reserve_pct` set
  //! by hand survived until the first expansion and then vanished, leaving the default in force
  //! with nothing in the file to say the setting had ever been made.  [B1030]
  put(" (pool_reserve_pct . ");     put(ascii.dec(ll_pre_settings.pool_reserve_pct));     put(")\n");
  put(" (pool_reserve_bytes . ");   put(ascii.dec(ll_pre_settings.pool_reserve_bytes));   put(")\n");
  //! EMITTED EVEN THOUGH REACHING HERE MEANS IT IS 1 -- this writer replaces the file, so a key it
  //! does not name is ERASED ([B1030] [B417]).  Writing it keeps the knob discoverable in the file
  //! an operator actually reads, instead of only in the header above.
  put(" (gc_persist . ");          put(ascii.dec(ll_pre_settings.gc_persist));          put(")\n");
  put(" (va_list_max . ");          put(ascii.dec(ll_pre_settings.va_list_max));          put(")\n");
  put(" (gc_total_marked . ");      put(ascii.dec(ll_pre_settings.gc_total_marked));      put(")\n");
  //! THESE TWO MUST BE WRITTEN BACK; LEAVING THEM OUT ERASES THEM.  [P285] [B417]  This
  //! writer opens the file "w" and emits a FIXED list, so any key it does not name is ERASED the
  //! first time the VM rewrites -- `ll_settings_boot_ok()` on boot, or the GC on heap expansion
  //! (ll_vm_mem.cpp, at ll_pre_settings_write).  A placement key dropped that way does not fail:
  //! the boot that set it measures correctly and EVERY LATER BOOT SILENTLY RUNS THE CONTROL while
  //! the file still looks edited.  That is a manufactured control arm wearing a treatment arm's
  //! label -- the same shape as the READ-side defect in `apply_key`'s caller.  [B888]
  //!
  //! THE REAL DEFECT IS THAT THIS LIST IS A HAND-MAINTAINED SECOND COPY of `apply_key`'s, with
  //! nothing making the two agree: the next key added will be missing from whichever side its
  //! author did not have open.  One table driving both is the fix and is scoped in [P285] -- do
  //! not add a key to two places instead.
  put(" (sint_in_iram . ");         put(ascii.dec(ll_pre_settings.sint_in_iram));         put(")\n");
  put(" (rootstack_in_iram . ");    put(ascii.dec(ll_pre_settings.rootstack_in_iram));    put(")\n");
  //! [P298] NAMED HERE BECAUSE THIS WRITER ERASES WHAT IT DOES NOT NAME ([B888]).  Omitting a
  //! key does not fail: the boot that set it measures correctly and every later boot silently
  //! runs the default while the file still looks edited.
  put(" (rootstack_max . ");        put(ascii.dec(ll_pre_settings.rootstack_max));        put(")\n");
  put(" (rootstack_init . ");       put(ascii.dec(ll_pre_settings.rootstack_init));       put(")\n");
  put(" (markstack_init . ");       put(ascii.dec(ll_pre_settings.markstack_init));       put(")\n");
  put(" (boot_pending . ");         put(ascii.dec(boot_pending));                         put("))\n");

  //! OVERFLOW FAILS TOWARDS WRITING.  If the content outgrew the buffer we cannot say whether it
  //! changed, and a skipped write loses an operator's setting silently where a redundant write
  //! only costs time.  Say so, once, rather than letting the buffer size become a silent cap.
  if (_ovf) {
    //! ONCE PER BOOT, like the `gc_persist 0` notice above: this is reached from the idle task,
    //! so a per-call line would flood the transcript a measurement is trying to read.
    static bool _said_ovf = false;
    if (!_said_ovf) {
      _said_ovf = true;
      global_printf("[settings] rendered settings exceeded the render buffer -- writing "
                    "unconditionally and skipping the changed-check; raise _buf in "
                    "ll_pre_settings_write()\n");
    }
  }
  else if (ll_settings_file_matches(_buf, _len)) {
    _ll_settings_writes_skipped++;
    return;                       //!< identical on disk: nothing to record, so nothing to write
  }

  //! [B1092] WRITE A TEMPORARY FILE, THEN RENAME IT OVER THE REAL ONE.  Opening the real file "w"
  //! truncates it on the spot, so a reset landing before the data is committed leaves it EMPTY --
  //! measured on esp32-c5: the next boot read 0 bytes, sized the pool at the 32,768-cell default
  //! instead of the requested 516,096, and then persisted that default over the file, so every
  //! later boot inherited it.  The deferred write ([B1085]) fires about 1 s after a new live-set
  //! peak, which is exactly when a test harness ends a benchmark and resets the board.  A rename
  //! is atomic in LittleFS: a reset leaves the old file or the new one, never neither.
  //! A short write is refused rather than renamed in -- LL_File::write(buf, n) reports nothing,
  //! so the bytes are counted here.
  static const char *const _tmp = LAMB_SETTINGS_PATH ".tmp";
  LL_File *f = ll_file_system.open(_tmp, "w");
  if (!f || !f->isOpen()) { delete f; return; }
  size_t _put = 0;
  for (size_t i = 0; i < _len; i++) _put += (f->write((byte) _buf[i]) == 1) ? 1 : 0;
  f->close();
  delete f;
  if (_put != _len) {
    global_printf("[settings] short write of %s (%s of %s bytes) -- " LAMB_SETTINGS_PATH
                  " left as it was\n", _tmp, ascii.dec((LL_int64) _put), ascii.dec((LL_int64) _len));
    ll_file_system.rm(_tmp);
    return;
  }
  if (ll_file_system.mv(_tmp, LAMB_SETTINGS_PATH) != 0) {
    global_printf("[settings] rename %s -> " LAMB_SETTINGS_PATH " failed -- file left as it was\n", _tmp);
    return;
  }
  _ll_settings_writes_done++;
}

//! The VM is up, so whatever it read was survivable.  Clearing the sentinel is what makes the next
//! boot TRUST the stored values rather than discard them -- without this the mechanism would reset
//! `gc_total_marked` on every single boot and the persistence would be pointless.  [B441]
void ll_settings_boot_ok(void)
{
  if (!ll_pre_settings.boot_pending && ll_pre_settings.gc_total_marked == 0) return;  //!< nothing armed
  ll_pre_settings.boot_pending = 0;
  ll_pre_settings_write(0);
}

//! State for ll_catch()'s one-line-per-ERROR logging (see the macro in ll_platform_generic.h).  [B70]
//! Not thread-local and not reset anywhere: it only ever needs to answer "is this the same error I
//! just logged?", and a stale value from a previous unwind can at worst suppress one duplicate line
//! before the counter resets on the next distinct object.
const void *ll_catch_last_err = 0;
int         ll_catch_repeats  = 0;
