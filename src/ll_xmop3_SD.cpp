// Copyright 2026 by Frobenius Norm LLC 2026-10-07
/*! @file
    @brief SD card in SPI mode, exposed to Scheme as `sd-*`.

    WHY A CARD AT ALL.  Every other channel to a board -- the serial console,
    the TCP REPL, a remote evaluator -- requires the board to be healthy enough
    to answer.  A fault that stops it answering therefore erases its own
    evidence, and the operator learns only that something went wrong some
    minutes ago.  A card is the one observer that is present while it happens
    and still readable afterwards.

    PINS COME FROM THE BOARD DESCRIPTION, NOT FROM HERE.  `LL_SD_PIN_CS`,
    `LL_SD_PIN_MOSI`, `LL_SD_PIN_SCK` and `LL_SD_PIN_MISO` are supplied by the
    build for the board being built.  A pin number written into a driver is
    wrong on the next board and right nowhere.

    THE BUS MAY BE SHARED -- see ll_shared_spi_bus.h.  On at least one supported
    board the clock and data lines are also a display controller's serial
    initialisation lines.  Every entry point here takes that lock, so a card
    transfer and a display re-initialisation cannot overlap.

    MOUNTING IS BEST-EFFORT AND IT REPORTS.  A board with no card, or with a
    card that is not seated, is a normal configuration: it must carry on, and it
    must say so ONCE rather than either failing the boot or going quiet.  A
    silent failure here would be indistinguishable from a working recorder that
    happens to have nothing to say, which is the worst available outcome for an
    instrument whose whole purpose is to be trusted later.

    IT NEVER FORMATS.  `format_if_mount_failed` is false and stays false: a card
    that will not mount may be a card with somebody's data on it, and an
    instrument that erases evidence to make itself work is not an instrument.
*/

#include "LambLisp.h"
#include "ll_shared_spi_bus.h"

#if LL_SD_SPI || LL_SD_MMC

#if LL_SD_SPI
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#endif
#if LL_SD_MMC
#include "driver/sdmmc_host.h"
#endif
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <sys/stat.h>

#define LL_SD_MOUNTPOINT "/sd"

//! HOW LONG A CALLER WAITS FOR THE SHARED BUS.  Long enough that a display
//! re-initialisation in progress is waited out rather than failed; short enough
//! that a caller is never parked indefinitely by a subsystem that did not
//! release.  A refusal is a reportable outcome; a hang is not.
#define LL_SD_BUS_WAIT_MS 2000

static SemaphoreHandle_t ll_sd_bus_mutex = NULL;
static sdmmc_card_t     *ll_sd_card      = NULL;
static bool              ll_sd_mounted   = false;
static bool              ll_sd_said_absent = false;   //!< the "no card" notice is printed ONCE

/*  ---- the shared bus lock ------------------------------------------------
    Declared in ll_shared_spi_bus.h and defined here because this is the
    subsystem that introduces the contention: a board without a card socket
    compiles the inline no-ops instead and carries no mutex.
*/
#if LL_SD_SPI
bool ll_spi_bus_take(const char *who, unsigned timeout_ms)
{
  bool res = true;
  if (ll_sd_bus_mutex == NULL) {
    //! CREATED ON FIRST USE RATHER THAN AT STARTUP.  There is no initialisation
    //! order to get wrong this way, and the first caller is by definition not
    //! racing a second one: both subsystems that use this bus are driven from
    //! the main loop.
    ll_sd_bus_mutex = xSemaphoreCreateMutex();
  }
  if (ll_sd_bus_mutex == NULL) {
    //! A MUTEX WE COULD NOT CREATE MUST NOT READ AS A BUS WE HOLD.  Internal
    //! memory is the scarcest pool on these parts, so this is a real outcome
    //! and not a defensive flourish.
    res = false;
  }
  else if (xSemaphoreTake(ll_sd_bus_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    res = false;
  }
  (void) who;
  return res;
}

void ll_spi_bus_give(const char *who)
{
  (void) who;
  if (ll_sd_bus_mutex != NULL) xSemaphoreGive(ll_sd_bus_mutex);
}
#endif /* LL_SD_SPI -- a card on dedicated pins contends with nothing */

/*  ---- mount / unmount ----------------------------------------------------

    THE PREPROCESSOR SELECTS BETWEEN TWO WHOLE FUNCTIONS, NEVER PARTS OF ONE.
    To add a host mode, add a complete `ll_sd_mount_card()` beside these two --
    do not carve one function into `#if` arms.  An arm that opens a brace whose
    match lies outside the `#endif` leaves each branch individually unbalanced
    while the file still balances AS WRITTEN, which no lexical brace check can
    detect: it counts the text, not the preprocessed translation unit.  The
    compiler then reports `expected declaration before '}'` far from the cause.

    Verify any change to these guards PER BRANCH, not just on the file:

        cpp -P -DLL_SD_MMC=1 src/ll_xmop3_SD.cpp | <count { and }>
        cpp -P -DLL_SD_SPI=1 src/ll_xmop3_SD.cpp | <count { and }>

    Both must balance.  Keeping each mode a complete function makes the error
    unavailable in the first place, which is why it is structured this way.
*/

#if LL_SD_MMC
/*! Mount in 1-bit SD_MMC mode (classic ESP32).

    THE PINS ARE NOT CONFIGURABLE AND THAT IS SILICON, NOT SIMPLIFICATION: this
    part reports `SOC_SDMMC_USE_IOMUX`, so the host is hard-wired to CLK 14 /
    CMD 15 / DAT0 2.  A part reporting `SOC_SDMMC_USE_GPIO_MATRIX` can place
    them anywhere, which is why the SPI branch below takes its pins from the
    board description and this one cannot.

    ONE BIT, NOT FOUR, DELIBERATELY.  Four-bit mode claims GPIO 4, 12 and 13 as
    well, and a recorder is not worth three more pins on a module whose carrier
    may want them.  It costs nothing that matters: a line-oriented log writes
    far below what either width sustains.

    GPIO 2 IS A STRAPPING PIN HERE.  A card holding DAT0 low at reset selects a
    different boot mode, so a board that will not start with a card inserted is
    a wiring characteristic and not a fault in this driver.
*/
static esp_err_t ll_sd_mount_card(void)
{
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;

  esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;   //!< never, see the file comment
  mcfg.max_files              = 2;
  mcfg.allocation_unit_size   = 0;

  return esp_vfs_fat_sdmmc_mount(LL_SD_MOUNTPOINT, &host, &slot, &mcfg, &ll_sd_card);
}
#else
/*! Mount in SPI mode, pins from the board description. */
static esp_err_t ll_sd_mount_card(void)
{
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;

  spi_bus_config_t bus = {};
  bus.mosi_io_num   = LL_SD_PIN_MOSI;
  bus.miso_io_num   = LL_SD_PIN_MISO;
  bus.sclk_io_num   = LL_SD_PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  //! A SMALL TRANSFER CEILING IS A MEMORY DECISION, NOT A SPEED ONE.  The
  //! DMA-capable internal pool is the scarcest on this part and a card is an
  //! instrument, not a data path: one sector at a time costs a few hundred
  //! bytes of descriptors where a large ceiling costs kilobytes.
  bus.max_transfer_sz = 4096;

  //! EARLY RETURN BEFORE ANYTHING IS ACQUIRED -- a guard clause, so there is
  //! nothing owed to an unwind.  `INVALID_STATE` means somebody already
  //! initialised this host, which is the shared-bus case working as intended
  //! rather than an error.
  esp_err_t err = spi_bus_initialize((spi_host_device_t) host.slot, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = (gpio_num_t) LL_SD_PIN_CS;
  slot.host_id = (spi_host_device_t) host.slot;

  esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;   //!< never, see the file comment
  mcfg.max_files              = 2;
  mcfg.allocation_unit_size   = 0;

  return esp_vfs_fat_sdspi_mount(LL_SD_MOUNTPOINT, &host, &slot, &mcfg, &ll_sd_card);
}
#endif /* LL_SD_MMC */

//! -> #t when a card is mounted on return (including when it already was).
//! NO PREPROCESSOR IN HERE: the mode difference is entirely inside
//! `ll_sd_mount_card()`, so this function reads the same on every board.
static bool ll_sd_do_mount(Lamb &lamb)
{
  bool res = ll_sd_mounted;
  if (!res) {
    if (!ll_spi_bus_take("sd-mount", LL_SD_BUS_WAIT_MS)) {
      lamb.log("sd-mount: the shared SPI bus is busy -- not mounting\n");
    }
    else {
      const esp_err_t err = ll_sd_mount_card();
      if (err == ESP_OK) {
        ll_sd_mounted     = true;
        ll_sd_said_absent = false;
        res               = true;
        lamb.log("sd-mount: mounted at %s\n", LL_SD_MOUNTPOINT);
      }
      //! SAY IT ONCE.  A board with no card is a normal configuration, and a
      //! notice on every attempt trains the reader to ignore the channel that
      //! is supposed to carry the diagnosis.
      else if (!ll_sd_said_absent) {
        ll_sd_said_absent = true;
        lamb.log("sd-mount: no card mounted (%s) -- carrying on without one\n",
                 esp_err_to_name(err));
      }
      ll_spi_bus_give("sd-mount");
    }
  }
  return res;
}

static Sexpr_t mop3_sd_mount(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_mount()");
  (void) sexpr;
  Sexpr_t res = OBJ_UNDEF;
  mop3_try {
    mop3_gc_frame({
      res = lamb.mk_bool(ll_sd_do_mount(lamb), env_exec);
    });
    return res;
  }
  mop3_catch();
}

static Sexpr_t mop3_sd_unmount(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_unmount()");
  (void) sexpr;
  mop3_try {
    mop3_gc_frame({
      bool res = false;
      if (ll_sd_mounted && ll_spi_bus_take("sd-unmount", LL_SD_BUS_WAIT_MS)) {
        esp_vfs_fat_sdcard_unmount(LL_SD_MOUNTPOINT, ll_sd_card);
        ll_sd_card    = NULL;
        ll_sd_mounted = false;
        res           = true;
        ll_spi_bus_give("sd-unmount");
      }
      return lamb.mk_bool(res, env_exec);
    });
  }
  mop3_catch();
}

/*! -> (mounted? NAME-or-#f CAPACITY-BYTES SECTOR-BYTES) so a caller can tell a
    missing card from a mounted one WITHOUT a second call that might race it. */
static Sexpr_t mop3_sd_info(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_info()");
  (void) sexpr;
  Sexpr_t res = OBJ_UNDEF;
  mop3_try {
    mop3_gc_frame({
      res = NIL;
      if (ll_sd_mounted && ll_sd_card != NULL) {
        const LL_int64 cap = (LL_int64) ll_sd_card->csd.capacity
                           * (LL_int64) ll_sd_card->csd.sector_size;
        res = lamb.cons(lamb.mk_integer((LL_int32) ll_sd_card->csd.sector_size, env_exec), res, env_exec);
        res = lamb.cons(lamb.mk_integer(cap, env_exec), res, env_exec);
        res = lamb.cons(lamb.mk_string(env_exec, "%s", ll_sd_card->cid.name), res, env_exec);
        res = lamb.cons(HASHT, res, env_exec);
      }
      else {
        res = lamb.cons(lamb.mk_integer((LL_int32) 0, env_exec), res, env_exec);
        res = lamb.cons(lamb.mk_integer((LL_int32) 0, env_exec), res, env_exec);
        res = lamb.cons(HASHF, res, env_exec);
        res = lamb.cons(HASHF, res, env_exec);
      }
    });
    return res;
  }
  mop3_catch();
}

/*  ---- file access ---------------------------------------------------------

    The card is mounted through the FAT VFS, so these are ordinary stdio calls
    under LL_SD_MOUNTPOINT.  Three properties they all share, and each exists
    for a reason worth stating:

    THEY TAKE THE SHARED SPI BUS for the duration of the operation.  On a board
    whose display shares the bus, a read that did not hold it would interleave
    with panel traffic and corrupt both.

    THEY RETURN #f RATHER THAN RAISING.  A recorder that aborts when the card
    is missing is worse than one that notices and carries on; the caller can
    always raise if it would rather.

    THEY REFUSE A PATH THAT LEAVES THE CARD.  An absolute path or one
    containing ".." is rejected before anything opens, so a caller cannot
    address the device filesystem through this interface by accident.
*/

//! Join a caller's relative path onto the mountpoint, or return false if the
//! path could escape it.  `out` is untouched on refusal.
static bool ll_sd_path(const char *rel, char *out, size_t outlen)
{
  bool res = false;
  if (rel != NULL) {
    //! A LEADING SLASH IS ACCEPTED, AND REJECTING IT WAS NEVER PART OF THE SANDBOX.  The sandbox is
    //! the two facts below it: the mountpoint is prepended UNCONDITIONALLY, and ".." is banned
    //! anywhere in the path.  So "/foo" reduced to "foo" still resolves inside the mountpoint --
    //! the old `rel[0] != '/'` test was redundant with the prepend and cost a correct answer.
    //! WHY IT MATTERED ENOUGH TO CHANGE: "/" is the obvious spelling for a filesystem root, and the
    //! SIBLING path API on the same board REQUIRES it -- the LittleFS note says ESP32/SPIFFS needs a
    //! leading / on all paths.  So a caller moving from LittleFS to SD writes the slash by habit and
    //! gets `#f`, which is ALSO this API's value for "nothing there".  An empty card, an unmounted
    //! card, a nonexistent path and a CORRECT-BUT-SLASHED path were one answer, and on a fresh card
    //! the natural reading of `#f` is "the card is empty".
    //! It cost two board sessions to find, and the second nearly confirmed the wrong conclusion:
    //! four spellings all returned `#f`, which read as "ls is broken", when in fact `dev_eval_seq`
    //! resets the board between calls so the previous `(sd-mount)` was gone and nothing was mounted.
    //! Only mounting and listing inside ONE session separated "wrong path" from "not mounted".
    while (*rel == '/') rel++;              //!< several are fine: "//flt.0" is the same file
    if (strstr(rel, "..") == NULL) {
      const int n = snprintf(out, outlen, "%s/%s", LL_SD_MOUNTPOINT, rel);
      res = (n > 0 && (size_t) n < outlen);
    }
  }
  return res;
}

//! The most a single `sd-read` will return unless the caller asks for less.
//! Deliberately finite: a card holds gigabytes and a cell pool does not, so an
//! unbounded read is an out-of-memory waiting for a large file.
#define LL_SD_READ_MAX_DEFAULT 4096

/*! (sd-ls [SUBDIR]) -> list of (NAME . BYTES), or #f if unmounted/unreadable. */
static Sexpr_t mop3_sd_ls(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_ls()");
  Sexpr_t res = OBJ_UNDEF;
  mop3_try {
    mop3_gc_frame({
      res = HASHF;
      char path[160];
      const char *sub = (sexpr != NIL && lamb.car(sexpr) != NIL)
                      ? lamb.car(sexpr)->any_str_get_chars() : "";
      if (!ll_sd_mounted) {
        lamb.log("sd-ls: no card mounted\n");
      }
      else if (!ll_sd_path(sub, path, sizeof(path))) {
        lamb.log("sd-ls: refusing path '%s' -- must be relative and free of '..'\n", sub);
      }
      else if (!ll_spi_bus_take("sd-ls", LL_SD_BUS_WAIT_MS)) {
        lamb.log("sd-ls: the shared SPI bus is busy\n");
      }
      else {
        DIR *d = opendir(path);
        if (d == NULL) {
          lamb.log("sd-ls: cannot open %s\n", path);
        }
        else {
          Sexpr_t acc = NIL;
          //! [P275] acc is built by cons and is NOT reachable from sexpr, so it is live across
          //! mk_string/mk_integer without being an argument to either -> it needs a root.  The frame
          //! is now INSIDE the loop and re-roots acc's CURRENT value each iteration, which is the
          //! part a protect around the whole loop could not do: that form
          //! pushes BY VALUE, so it rooted the initial NIL and every list acc held from the first
          //! entry onwards was unrooted.  One directory entry was enough to expose it.
          //! `nm` needs a root too, and for the reason the procedure's step 2 warns about: the call
          //! it is held across is the INNER mk_integer (an argument of the cons, so it runs first),
          //! not the cons that finally consumes it.  `pr` needs nothing -- the next cons is the only
          //! call after it and takes it as an argument.
          struct dirent *e;
          while ((e = readdir(d)) != NULL) {
            char full[224];
            struct stat st;
            LL_int32 bytes = 0;
            if (snprintf(full, sizeof(full), "%s/%s", path, e->d_name) > 0
                && stat(full, &st) == 0)
              bytes = (LL_int32) st.st_size;
            mop3_gc_frame({
                mop3_gcroot_Sexpr_t(acc_live, acc);
                mop3_gcroot_Sexpr_t(nm, lamb.mk_string(env_exec, "%s", e->d_name));   //!< "%s": a FILENAME
                //!< off the card is DATA, and the two-argument mk_string is the PRINTF one -- a
                //!< file named `%s.txt` would be expanded against an argument nobody passed.
                //!< Same defect as the read path below, and reachable by anyone who can name a
                //!< file on a removable card.
                Sexpr_t pr = lamb.cons(nm, lamb.mk_integer(bytes, env_exec), env_exec);
                acc = lamb.cons(pr, acc, env_exec);
            });
          }
          closedir(d);
          res = acc;
        }
        ll_spi_bus_give("sd-ls");
      }
    });
    return res;
  }
  mop3_catch();
}

/*! Shared body for `sd-read` and `sd-read-bytevector`: identical but for the TYPE of the
    value handed back, so the two cannot drift in their locking, their path checks or their
    bounds -- the same reason `ll_sd_put` exists on the write side.

    WHY THERE ARE TWO VERBS RATHER THAN ONE CLEVER ONE.  `sd-write` can tell text from bytes by
    LOOKING AT WHAT IT WAS GIVEN; a read has nothing to look at, so the caller has to say.  The
    pair mirrors R7RS's own `read-string` / `read-bytevector` split, and the port API in
    `ll_vm_mop3_port.cpp` already spells it that way.

    USE THE BYTEVECTOR ONE FOR ANYTHING THAT IS NOT TEXT.  A string here carries its own length,
    so a zero byte no longer truncates it -- but `sd-write`'s string path still measures with
    `strlen`, so a round trip through the STRING verb still loses everything after the first zero.
    A bytevector survives both halves.
*/
static Sexpr_t ll_sd_get(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec,
                         bool as_bytes, const char *who)
{
  Sexpr_t res = HASHF;
  char path[160];
  const char *rel = (sexpr != NIL) ? lamb.car(sexpr)->any_str_get_chars() : NULL;
  LL_int32 cap = LL_SD_READ_MAX_DEFAULT;
  LL_int32 off = 0;
  Sexpr_t rest = (sexpr != NIL) ? lamb.cdr(sexpr) : NIL;
  if (rest != NIL) {
    const LL_int32 want = lamb.car_mustbe_int32(rest);
    if (want > 0 && want < cap) cap = want;
    if (lamb.cdr(rest) != NIL) {
      const LL_int32 at = lamb.car_mustbe_int32(lamb.cdr(rest));
      if (at > 0) off = at;
    }
  }
  if (!ll_sd_mounted) {
    lamb.log("%s: no card mounted\n", who);
  }
  else if (!ll_sd_path(rel, path, sizeof(path))) {
    lamb.log("%s: refusing path '%s' -- must be relative and free of '..'\n",
             who, rel ? rel : "");
  }
  else if (!ll_spi_bus_take(who, LL_SD_BUS_WAIT_MS)) {
    lamb.log("%s: the shared SPI bus is busy\n", who);
  }
  else {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
      lamb.log("%s: cannot open %s\n", who, path);
    }
    else if (off > 0 && fseek(f, (long) off, SEEK_SET) != 0) {
      //! A SEEK PAST THE END IS NOT AN ERROR ON EVERY LIBC, so this catches only a real seek
      //! failure; a legal seek beyond EOF falls through and reads zero bytes, which is the
      //! EMPTY value rather than #f.  The two are worth keeping distinct: empty means "nothing
      //! there", #f means "the call did not happen".
      lamb.log("%s: cannot seek %s to %s\n", who, path, ascii.dec(off));
      fclose(f);
    }
    else {
      char *buf = (char *) malloc((size_t) cap + 1);
      if (buf == NULL) {
        lamb.log("%s: no room for %s bytes\n", who, ascii.dec(cap));
      }
      else {
        const size_t got = fread(buf, 1, (size_t) cap, f);
        buf[got] = '\0';
        if (got == (size_t) cap) {
          AsciiConverter a1;
          lamb.log("%s: %s truncated at %s bytes -- pass MAX and OFFSET to read the rest\n",
                   who, path, a1.dec(cap));
        }
        /*! THE LENGTH IS `got`, AND IT IS NOT A STYLE CHOICE.  This line used to read
            `mk_string(env_exec, buf)`, which selects the PRINTF overload: the file's own bytes
            became a format string with no arguments behind them, so a card holding `%s` made the
            runtime dereference an uninitialised argument slot (measured: a 7-byte ASCII file came
            back as 20 bytes, one of them a DRAM stack address).  It also measured with `strlen`,
            so every file with a zero byte in it was silently shortened AND reported as a success.
            Both defects die with the length-carrying overload, which the sibling slurp in
            `ll_xmop3_platform_generic.cpp` was already using. */
        res = as_bytes ? lamb.mk_bytevector((LL_int32) got, (Bytest_t) buf, env_exec)
                       : lamb.mk_string((LL_int32) got, (Charst_t) buf, env_exec);
        free(buf);
      }
      fclose(f);
    }
    ll_spi_bus_give(who);
  }
  return res;
}

/*! (sd-read PATH [MAX [OFFSET]]) -> string, or #f.  TEXT.  Reads at most MAX bytes starting at
    OFFSET, default LL_SD_READ_MAX_DEFAULT from byte 0; a longer file is truncated rather than
    refused, and the log says so, because a recorder reading its own tail wants the bytes more
    than it wants an error.  MAX can only LOWER the cap, never raise it -- the cap is what stops
    a gigabyte card from being read into a cell pool that is not one -- so a file bigger than the
    cap is read in successive chunks by advancing OFFSET. */
static Sexpr_t mop3_sd_read(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_read()");
  mop3_try { return ll_sd_get(lamb, sexpr, env_exec, false, "sd-read"); }
  mop3_catch();
}

/*! (sd-read-bytevector PATH [MAX [OFFSET]]) -> bytevector, or #f.  BYTES: the counterpart of
    `sd-write` with a bytevector, and the only way to read back a file this board wrote that was
    not text.  Same cap and same chunking rule as `sd-read`. */
static Sexpr_t mop3_sd_read_bytevector(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_read_bytevector()");
  mop3_try { return ll_sd_get(lamb, sexpr, env_exec, true, "sd-read-bytevector"); }
  mop3_catch();
}

//! Shared body for sd-write and sd-append: identical but for the open mode,
//! so the two cannot drift in their locking or their path checks.
static Sexpr_t ll_sd_put(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec,
                         const char *mode, const char *who)
{
  Sexpr_t res = HASHF;
  char path[160];
  const char *rel  = (sexpr != NIL) ? lamb.car(sexpr)->any_str_get_chars() : NULL;
  /*! A STRING OR A BYTEVECTOR, AND THE DIFFERENCE IS LENGTH.  A string's length is found with
      `strlen`, which stops at the first zero byte -- correct for text and wrong for everything
      else.  A JPEG frame from `camera-capture` is a bytevector full of zero bytes, so writing one
      through the string path stores a few hundred bytes of a quarter-megabyte image and reports
      success.  A bytevector carries its own length and is written whole. */
  Sexpr_t    arg   = (sexpr != NIL && lamb.cdr(sexpr) != NIL) ? lamb.car(lamb.cdr(sexpr)) : NIL;
  const char *data = NULL;
  size_t      dlen = 0;
  if (arg != NIL && arg->is_any_bvec_atom()) {
    LL_int32 nbytes; ByteVec_t elems;
    arg->any_bvec_get_info(nbytes, elems);
    data = (const char *) elems;
    dlen = (size_t) nbytes;
  }
  else if (arg != NIL) {
    data = arg->any_str_get_chars();
    dlen = data ? strlen(data) : 0;
  }
  if (!ll_sd_mounted) {
    lamb.log("%s: no card mounted\n", who);
  }
  else if (data == NULL) {
    lamb.log("%s: nothing to write\n", who);
  }
  else if (!ll_sd_path(rel, path, sizeof(path))) {
    lamb.log("%s: refusing path '%s' -- must be relative and free of '..'\n",
             who, rel ? rel : "");
  }
  else if (!ll_spi_bus_take(who, LL_SD_BUS_WAIT_MS)) {
    lamb.log("%s: the shared SPI bus is busy\n", who);
  }
  else {
    FILE *f = fopen(path, mode);
    if (f == NULL) {
      lamb.log("%s: cannot open %s for '%s'\n", who, path, mode);
    }
    else {
      const size_t len = dlen;
      const size_t put = fwrite(data, 1, len, f);
      //! FLUSH AND CHECK BEFORE CLAIMING SUCCESS.  fwrite can report the full
      //! count into a buffer the close then fails to commit, which on a
      //! removable card is the ordinary case rather than a rare one.
      const bool ok = (put == len) && (fflush(f) == 0);
      if (fclose(f) != 0 || !ok) {
        AsciiConverter a1, a2;
        lamb.log("%s: wrote %s of %s bytes to %s\n",
                 who, a1.dec((LL_int32) put), a2.dec((LL_int32) len), path);
      }
      else {
        res = HASHT;
      }
    }
    ll_spi_bus_give(who);
  }
  return res;
}

/*! (sd-write PATH DATA) -> #t/#f.  Truncates.  DATA may be a string or a bytevector; a
    bytevector is written whole, including any zero bytes it contains. */
static Sexpr_t mop3_sd_write(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_write()");
  mop3_try { return ll_sd_put(lamb, sexpr, env_exec, "w", "sd-write"); }
  mop3_catch();
}

/*! (sd-append PATH DATA) -> #t/#f.  The recorder's call.  DATA may be a string or a
    bytevector, as for sd-write. */
static Sexpr_t mop3_sd_append(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::mop3_sd_append()");
  mop3_try { return ll_sd_put(lamb, sexpr, env_exec, "a", "sd-append"); }
  mop3_catch();
}

#endif /* LL_SD_SPI || LL_SD_MMC */

/*  ---- installer -----------------------------------------------------------
    Always present so the startup sequence can call it unconditionally; it
    installs nothing on a board without a card socket.
*/
Sexpr_t SD_install_mop3(Lamb &lamb, Sexpr_t sexpr, Sexpr_t env_exec)
{
  ME("::SD_install_mop3()");
  mop3_try {
#if LL_SD_SPI || LL_SD_MMC
    static const struct { Lamb::Mop3st_t func; const char *name; bool syntax; } sd_procs[] = {
      { mop3_sd_mount,   "sd-mount",   false },
      { mop3_sd_unmount, "sd-unmount", false },
      { mop3_sd_info,    "sd-info",    false },
      { mop3_sd_ls,      "sd-ls",      false },
      { mop3_sd_read,    "sd-read",    false },
      { mop3_sd_read_bytevector, "sd-read-bytevector", false },
      { mop3_sd_write,   "sd-write",   false },
      { mop3_sd_append,  "sd-append",  false },
    };
    const int Nsd_procs = sizeof(sd_procs)/sizeof(sd_procs[0]);
    lamb.log("%s defining %s Mops\n", me, ascii.dec((LL_int32) Nsd_procs));
    //! [P275] `env_target` is read by dict_bind_bang every iteration, after mk_Mop3_procst_t and
    //! mk_symbol allocate, and is an argument to neither -> it needs a root spanning the loop.
    //! The per-iteration frame goes inside the loop so the depth returns each time round; `sym` is
    //! consumed by dict_bind_bang, which protects its arguments, so it needs nothing.
    mop3_gc_frame({
        Sexpr_t env_target = lamb.car(sexpr);
        for (int i = 0; i < Nsd_procs; i++) {
          const auto &p = sd_procs[i];
          mop3_gc_frame({
              mop3_gcroot_Sexpr_t(proc, lamb.mk_Mop3_procst_t(p.func, env_exec));
              Sexpr_t sym = lamb.mk_symbol(p.name, env_exec);
              lamb.dict_bind_bang(env_target, sym, proc, env_exec);
          });
        }
    });
#else
    (void) sexpr;
    (void) env_exec;
#endif
    return NIL;
  }
  mop3_catch();
}
