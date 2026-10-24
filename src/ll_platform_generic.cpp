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

/*! B95: allocate a bulk VM payload, preferring PSRAM (see ll_platform_generic.h).
  The PSRAM probe is done ONCE and cached: on a part without PSRAM every payload would otherwise
  pay a failing heap_caps_malloc before falling back.
*/
void *ll_payload_alloc_bytes(size_t nbytes)
{
  if (nbytes == 0) nbytes = 1;      //!< new T[0] yields a unique non-null pointer; match that
#if LL_ESP32
  static int psram_ok = -1;
  if (psram_ok < 0) {
    void *probe = heap_caps_malloc(8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psram_ok = probe ? 1 : 0;
    if (probe) heap_caps_free(probe);
  }
  if (psram_ok) {
    void *p = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;                //!< PSRAM full -> fall through to internal, never fail here
  }
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

  NO PER-PART GUARD, deliberately (owner, 2026-09-30: "also get put in dram on s3 and wrover").
  Every ESP32 env takes this path -- S3, classic WROVER, RISC-V C5 -- exactly as
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
void *ll_dram_alloc_bytes(size_t nbytes)
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

void ll_dram_free(void *p)
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
//! `autobuf_t`'s own `_max_`: past it `upsize()` raises (B175), and raising inside the formatter
//! means throwing while building the message the throw needs formatted.
#define LL_VA_LIST_MAX_FLOOR   1024
#define LL_VA_LIST_MAX_CEILING 65536

//! [B888] One `key . value` pair -> one field.  Returns false when the key is none of ours, so the
//! caller can SAY SO; the chain used to live inline in `load()` and an unrecognised key simply fell
//! off the end of it.
bool LambPreSettings::apply_key(const char *key, long val)
{
  if      (strcmp(key, "cell_block_size")      == 0) cell_block_size      = (LL_int32) val;
  else if (strcmp(key, "extension_block_size") == 0) extension_block_size = (LL_int32) val;
  else if (strcmp(key, "gc_total_marked")      == 0) gc_total_marked      = (LL_int32) val;   //!< B247
  else if (strcmp(key, "gc_pool_target")       == 0) gc_pool_target       = (LL_int32) val;   //!< B610
  else if (strcmp(key, "ncg_frame_pool_size")  == 0) ncg_frame_pool_size  = (LL_int32) val;
  else if (strcmp(key, "sint_in_iram")         == 0) sint_in_iram         = (LL_int32) val;   //!< P285
  else if (strcmp(key, "rootstack_in_iram")    == 0) rootstack_in_iram    = (LL_int32) val;   //!< P285
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
        //! [B888] PARSE EVERY PAIR ON THE LINE, NOT THE FIRST ONE ONLY.  THIS LOOP IS THE FIX --
        //! if you collapse it back to a single `sscanf` you restore a defect that MANUFACTURES
        //! CONTROL ARMS THAT LOOK LIKE TREATMENT ARMS.  A hand-written
        //!     ((cell_block_size . 32768) (gc_pool_target . 32768) (va_list_max . 8301))
        //! is a correct s-expression -- it is how this file's own shipped two keys are laid out,
        //! only without the newline -- and a single `sscanf` takes `cell_block_size` and SILENTLY
        //! DISCARDS the rest.  The run then measures the default configuration while the file on
        //! disk goes on displaying the numbers that were asked for, so the operator reads "that
        //! setting has no effect" -- a conclusion about the VM drawn from this parser.  Measured
        //! 2026-10-15 on the host AND on esp32-s3-devkitc-1: five keys on one line left
        //! `va_list_max` at the compiled 8192 while the file said 8301.
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
                          " -- IGNORED, and dropped from the file on the next rewrite (B888)\n",
                          key, ascii.dec((LL_int64) val));
          npairs++;
          p += consumed;
        }
        //! [B888] AND SAY SO WHEN SOMETHING ON THE LINE WAS NOT UNDERSTOOD.  Parsing the whole line
        //! fixes the alist layout; this catches the rest -- a mistyped separator, a non-integer
        //! value, a stray word -- which the loop above would otherwise discard just as silently as
        //! the old single `sscanf` discarded trailing pairs.  The property that matters is LOUD,
        //! not correct: a settings file is the input to every provisioning experiment here, and an
        //! input that is quietly not what it says produces a measurement nobody can tell from a
        //! real one.  Trailing `)`/whitespace is consumed above and a `;` comment is accepted, so
        //! this is silent on every file the VM writes and on both shipped defaults.
        while (*p == '(' || *p == ')' || *p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p != '\0' && *p != ';')
          global_printf("[settings] " LAMB_SETTINGS_PATH ": %s pair(s) read from this line, then "
                        "unparsed text IGNORED: '%s' (B888)\n", ascii.dec((LL_int32) npairs), p);
        //! [B888] A LINE LONGER THAN THE BUFFER IS TRUNCATED, AND THAT ONLY BECAME DANGEROUS WHEN
        //! THE WHOLE LINE STARTED TO MATTER.  The character loop below has always dropped bytes
        //! past `sizeof(line)-1` in silence; while only the FIRST pair was ever read, a long line
        //! could lose nothing that was going to be used.  Now every pair counts, so a 256-byte cut
        //! can drop later keys -- and if it lands mid-number, `%ld` happily reads the surviving
        //! digits and stores a value that is in the file NOWHERE.  That is the same
        //! silently-wrong-input failure this bug is about, one layer down, so it is reported.
        //! Comment lines are exempt: prose is allowed to be long and nothing reads it.
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (overflow && *q != ';')
          global_printf("[settings] " LAMB_SETTINGS_PATH ": a line longer than %s bytes was "
                        "TRUNCATED -- keys past the cut are LOST and a value cut mid-digit reads "
                        "WRONG.  Put one key per line. (B888)\n",
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

  //! B441: SELF-RESET A SETTING THAT DID NOT SURVIVE ITS OWN BOOT.
  //! `gc_total_marked` is a persisted LIVE SET and the heap is sized at 2x it (ll_vm_mem.cpp).  A
  //! run that drove the live set above what this board can hold therefore writes a number the NEXT
  //! boot cannot satisfy -- measured 2026-09-16 on esp32-s3-devkitc-1: a gcpause bench at 53% fill
  //! left 75,980, the next reset asked for 151,960 cells against 2.09 MB of PSRAM, and `setup()`
  //! threw std::bad_alloc 50 times in a row.  Nothing linked the brick to the bench, because the
  //! bench had SUCCEEDED, and a firmware-only reflash does not clear it.
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
                    "discarding it and using the default heap size (B441)\n",
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

//! B441: one writer for the settings file, so a key added to LambPreSettings cannot be silently
//! dropped by a caller that predates it.  Best-effort: a board with no writable FS simply keeps
//! whatever it had, which is the pre-B441 behaviour and is safe.
void ll_pre_settings_write(LL_int32 boot_pending)
{
  LL_File *f = ll_file_system.open(LAMB_SETTINGS_PATH, "w");
  if (!f || !f->isOpen()) { delete f; return; }
    auto put = [&](const char *t) { f->write((const byte *) t, strlen(t)); };
  put("; Settings-Lamb.scm -- auto-written by the VM.  Reboot required.\n");
  put("; boot_pending 1 means a boot was attempted with these values and did not finish (B441).\n");
  //! [B859] SAY SO IN THE FILE WHEN THE FILE IS NOT WHAT RAN.  `cell_block_size` below the boot
  //! pool target is replaced by the target, and this file used to go on displaying the request --
  //! so a probe that varied the setting and read the file back saw the setting move and the
  //! behaviour stay put, on every workload, which is indistinguishable from "no workload provokes
  //! it".  That cost [B761] a week of INCONCLUSIVE and mislabelled a [B858] arm's contrast (an
  //! 8192-vs-262144 ratio that was really 40960-vs-262144: 6.4x reported as 32x).
  //!
  //! A COMMENT, NOT THE KEY -- see `ll_cell_block_size_effective` for why writing it back as a
  //! value would be [B248]/[B441] and would latch.  The line is informational, is regenerated from
  //! scratch on every write, and is absent whenever the request was honoured, so its PRESENCE is
  //! the signal.
  if (ll_cell_block_size_effective > 0
      && ll_cell_block_size_effective != ll_pre_settings.cell_block_size) {
    AsciiConverter a1, a2;
    put("; B859 cell_block_size override: requested "); put(a1.dec(ll_pre_settings.cell_block_size));
    put(", EFFECTIVE ");                                put(a2.dec(ll_cell_block_size_effective));
    put(" -- the value below is the REQUEST and is not what the heap was built with.\n");
    put("; The knob that binds the initial block below the pool target is gc_pool_target, not this one.\n");
  }
  put("((cell_block_size . ");      put(ascii.dec(ll_pre_settings.cell_block_size));      put(")\n");
  put(" (gc_pool_target . ");       put(ascii.dec(ll_pre_settings.gc_pool_target));       put(")\n");
  put(" (extension_block_size . "); put(ascii.dec(ll_pre_settings.extension_block_size)); put(")\n");
  put(" (ncg_frame_pool_size . ");  put(ascii.dec(ll_pre_settings.ncg_frame_pool_size));  put(")\n");
  put(" (va_list_max . ");          put(ascii.dec(ll_pre_settings.va_list_max));          put(")\n");
  put(" (gc_total_marked . ");      put(ascii.dec(ll_pre_settings.gc_total_marked));      put(")\n");
  //! [P285] THESE TWO MUST BE WRITTEN BACK, AND LEAVING THEM OUT IS [B417] ALL OVER AGAIN.  This
  //! writer opens the file "w" and emits a FIXED list, so any key it does not name is ERASED the
  //! first time the VM rewrites -- `ll_settings_boot_ok()` on boot, or the GC on heap expansion
  //! (ll_vm_mem.cpp, at ll_pre_settings_write).  A placement key dropped that way does not fail:
  //! the boot that set it measures correctly and EVERY LATER BOOT SILENTLY RUNS THE CONTROL while
  //! the file still looks edited.  That is a manufactured control arm wearing a treatment arm's
  //! label -- the same shape B888 fixed on the READ side.
  //!
  //! THE REAL DEFECT IS THAT THIS LIST IS A HAND-MAINTAINED SECOND COPY of `apply_key`'s.  The
  //! reader knows nine keys and this knew seven, with nothing making them agree; the next key
  //! added will be missing from whichever side its author did not have open.  One table driving
  //! both is the fix and is scoped in [P285] -- do not add a tenth key to two places instead.
  put(" (sint_in_iram . ");         put(ascii.dec(ll_pre_settings.sint_in_iram));         put(")\n");
  put(" (rootstack_in_iram . ");    put(ascii.dec(ll_pre_settings.rootstack_in_iram));    put(")\n");
  put(" (boot_pending . ");         put(ascii.dec(boot_pending));                         put("))\n");
  f->close();
  delete f;
}

//! B441: the VM is up, so whatever it read was survivable.  Clearing the sentinel is what makes the
//! next boot TRUST the stored values rather than discard them -- without this the mechanism would
//! reset `gc_total_marked` on every single boot and the persistence would be pointless.
void ll_settings_boot_ok(void)
{
  if (!ll_pre_settings.boot_pending && ll_pre_settings.gc_total_marked == 0) return;  //!< nothing armed
  ll_pre_settings.boot_pending = 0;
  ll_pre_settings_write(0);
}

//! B70: state for ll_catch()'s one-line-per-ERROR logging (see the macro in ll_platform_generic.h).
//! Not thread-local and not reset anywhere: it only ever needs to answer "is this the same error I
//! just logged?", and a stale value from a previous unwind can at worst suppress one duplicate line
//! before the counter resets on the next distinct object.
const void *ll_catch_last_err = 0;
int         ll_catch_repeats  = 0;
