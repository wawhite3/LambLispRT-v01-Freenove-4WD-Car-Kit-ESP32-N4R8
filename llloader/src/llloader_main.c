// Copyright 2026 by Frobenius Norm LLC 2026-07-07 00:00:00
//
// OTA llloader -- the Tier-1 `factory` app: a tiny, trusted, Arduino-core-free
// ESP-IDF recovery root.  It is NOT in the normal boot path: otadata points
// straight at ota_0 (the LambLisp VM), and this app runs ONLY as a fallback --
// first-ever boot (ota_0 empty) or after an ESP-IDF native rollback flipped
// otadata back to factory.
//
// Its single job: get a VALID VM image into ota_0 and hand control to it
// (set_boot_partition(ota_0) + reboot).  THE IMAGE-SOURCE LADDER (aux-storage ->
// network -> UART console -> BLE console) AND THE OTA WRITER ARE IMPLEMENTED --
// this said "everything here is a skeleton stub ... land in Ph2b/Ph6" long after
// both had landed (corrected 2026-10-06).  Measured that day: a 4WD recovered
// itself over rung 2 with no cable attached, fetching its image and sha256 sidecar
// over WiFi, so the text called a stub was the path that did the work.
//
// Constant-overhead OTA justification: the VM self-updates in place (N bytes);
// this loader is the fixed-k fallback, so total cost is N + k, never the
// universal 2N of an A/B-only scheme on 4 MB parts.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/gpio.h"   //!< [P253] step B: the press-and-hold exit
#include "esp_rom_sys.h" //!< esp_rom_printf -- the ONLY safe way to print from the button task
#include "llloader_button.h" //!< [P253 step B] the press-and-hold DECISION, host-tested
#include "llloader_led.h" //!< [owner 2026-10-06] the onboard RGB LED as a status display

#include "nvs.h"
#include "nvs_flash.h"

#include "image_source.h"
#include "ota_writer.h"
#include "llloader_llip.h"
#include "llloader_query.h"
#include "src_console.h"   //!< [P253] step C: register the status provider

static const char *TAG = "llloader";

// --- OTA NVS contract -------------------------------------------------------
// Namespace "ota-p125" holds the loader<->VM handshake.  Key "pending_update" is a
// u8 request set by the VM (or a provisioning tool) before rebooting into us:
//   0 = none            -> NORMAL  (nothing to do; try to boot ota_0 if valid)
//   1 = install         -> INSTALL (fetch + write ota_0 from the source ladder)
//   2 = recover         -> RECOVER (ota_0 bad/rolled-back; re-fetch a known-good)
//   3 = console         -> CONSOLE (operator-driven BLE/UART fallback)
#define OTA_NVS_NAMESPACE "ota-p125"
#define OTA_KEY_PENDING   "pending_update"

// Install-retry anti-loop counter (OTA). DISTINCT from ESP-IDF's native otadata rollback
// (PENDING_VERIFY / mark_valid), which independently reverts a VM that boots but never confirms.
// This counter guards the *install* side: without it, a VM image that keeps failing would be
// re-installed from the ladder forever (fall back to factory -> re-install -> bad -> fall back...).
// Semantics: the loader increments it on ENTERING an install; once it exceeds the max, the loader
// stops re-installing the same failing image and escalates (RECOVER golden, else CONSOLE). It is
// cleared when a healthy image is confirmed -- by do_normal here (ota_0 state VALID) and, as the
// real "the installed image actually boots" proof, by the VM on the host side (OTA item D:
// (ota-boot-ok!) at VM startup clears both pending_update and install_tries).
#define OTA_KEY_INSTALL_TRIES "install_tries"
#define OTA_MAX_INSTALL_TRIES 3

typedef enum {
    LOADER_NORMAL  = 0,
    LOADER_INSTALL = 1,
    LOADER_RECOVER = 2,
    LOADER_CONSOLE = 3,
} loader_action_t;

// --- Small NVS u8 helpers (namespace "ota-p125") --------------------------------
static uint8_t ota_get_u8(const char *key, uint8_t dflt)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return dflt;
    uint8_t v = dflt;
    if (nvs_get_u8(h, key, &v) != ESP_OK) v = dflt;
    nvs_close(h);
    return v;
}

static void ota_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open rw failed; cannot persist %s", key);
        return;
    }
    esp_err_t e = nvs_set_u8(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "nvs write %s: %s", key, esp_err_to_name(e));
    nvs_close(h);
}

static void ota_erase_key(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_erase_key(h, key);   // ESP_ERR_NVS_NOT_FOUND is benign
    if (e == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// Returns true when we have already tried to install too many times (caller must escalate rather
// than re-install). Otherwise records this attempt (increment + persist) and returns false.
static bool install_tries_exhausted(const char *what)
{
    uint8_t tries = ota_get_u8(OTA_KEY_INSTALL_TRIES, 0);
    if (tries >= OTA_MAX_INSTALL_TRIES) {
        ESP_LOGE(TAG, "%s: install_tries=%u >= max %d -- NOT re-installing; escalating",
                 what, (unsigned) tries, OTA_MAX_INSTALL_TRIES);
        return true;
    }
    ota_set_u8(OTA_KEY_INSTALL_TRIES, (uint8_t) (tries + 1));
    ESP_LOGW(TAG, "%s: install attempt %u of %d",
             what, (unsigned) (tries + 1), OTA_MAX_INSTALL_TRIES);
    return false;
}

static void install_tries_clear(void)
{
    ota_erase_key(OTA_KEY_INSTALL_TRIES);
}

// --- Partition inspection ----------------------------------------------------
// Log the running slot and the state of ota_0 so a serial watcher can see why
// the loader woke up.  A valid, non-empty ota_0 with a good OTA state means we
// can just boot it; anything else escalates to INSTALL/RECOVER.
static const esp_partition_t *inspect_partitions(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running slot: %s @ 0x%08lx (%lu bytes)",
             running ? running->label : "?",
             (unsigned long) (running ? running->address : 0),
             (unsigned long) (running ? running->size : 0));

    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota0 == NULL) {
        ESP_LOGW(TAG, "ota_0 partition not found in table");
        return NULL;
    }

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(ota0, &st);
    ESP_LOGI(TAG, "ota_0 @ 0x%08lx (%lu bytes) state=%d err=%s",
             (unsigned long) ota0->address, (unsigned long) ota0->size,
             (int) st, esp_err_to_name(err));
    return ota0;
}

// --- NVS request read --------------------------------------------------------
static loader_action_t read_pending_action(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no %s namespace yet (%s) -> NORMAL",
                 OTA_NVS_NAMESPACE, esp_err_to_name(err));
        return LOADER_NORMAL;
    }
    uint8_t pending = LOADER_NORMAL;
    err = nvs_get_u8(h, OTA_KEY_PENDING, &pending);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "%s unset (%s) -> NORMAL",
                 OTA_KEY_PENDING, esp_err_to_name(err));
        return LOADER_NORMAL;
    }
    ESP_LOGI(TAG, "%s = %u", OTA_KEY_PENDING, (unsigned) pending);
    return (loader_action_t) pending;
}

// --- Action stubs ------------------------------------------------------------
// Each returns true if it handed off (rebooted / will reboot), false to fall
// through to CONSOLE.  All real logic (source ladder, OTA writer, image auth)
// is deferred; these only trace the intended control flow.

// Boot into ota_0: mark it the boot partition and restart. Never returns on success.
static bool boot_into(const esp_partition_t *ota0)
{
    esp_err_t err = esp_ota_set_boot_partition(ota0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition(%s): %s", ota0->label, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "handoff -> %s @ 0x%08lx; restarting",
             ota0->label, (unsigned long) ota0->address);
    esp_restart();   // no return
    return true;
}

// --- [P253] reply-only frame output -------------------------------------------------------------
// The loader SAYS things on the console it already logs to.  It answers nothing: no listener, no
// parser exposed to the wire, nothing to authenticate.  See llloader_llip.h.

//! The slot name for a frame, derived from the SUBTYPE and not the label.
//!
//! The two are not the same string.  On this loader's own table (partitions_8M_ota.csv) the label
//! happens to be `ota_0`, but on partitions_8M.csv the same subtype is labelled `app1` -- so a
//! frame built from `->label` would emit a symbol absent from the vocabulary's slot list on some
//! boards and present on others, which is the worst kind of wrong: it works where you test it.
//! Returns NULL for anything that is not an OTA slot, and the caller then emits NO frame rather
//! than a frame with a made-up field.
static const char *slot_name(const esp_partition_t *p)
{
    if (p == NULL) { return NULL; }
    switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "ota_0";
    case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "ota_1";
    default:                              return NULL;
    }
}

static const char *digest_name(llloader_digest_t d)
{
    switch (d) {
    case LLLOADER_DIGEST_OK:       return "ok";
    case LLLOADER_DIGEST_MISMATCH: return "mismatch";
    default:                       return "absent";
    }
}

//! `(ota-result (seq 0) (rung R) (digest D) (action A) (slot S))` -- one per completed write
//! attempt, success or failure.  The verdict a reader wants is the combination: a `mismatch`
//! digest and an `install` action say the image arrived and was rejected, which no single field
//! says on its own.
static void emit_result(const char *action, const esp_partition_t *dst)
{
    const char *slot = slot_name(dst);
    if (slot == NULL) {
        //! NOT AN OTA SLOT -- say so rather than emit a frame with an invented field.  This should
        //! not happen (the writer requires an app/ota partition), which is exactly why it is
        //! reported instead of defaulted.
        ESP_LOGW(TAG, "ota-result suppressed: target is not an OTA slot");
        return;
    }
    const char *rung = llloader_ota_last_rung();
    if (rung == NULL || rung[0] == '\0') { rung = "aux-fs"; }
    char buf[LLLOADER_MAX_FRAME_BYTES];
    llloader_wr_t w;
    llloader_wr_begin(&w, buf, sizeof(buf), "ota-result");
    llloader_wr_int(&w, "seq", 0);                                  //!< 0 = unsolicited
    llloader_wr_sym(&w, "rung",   rung);
    llloader_wr_sym(&w, "digest", digest_name(llloader_ota_last_digest()));
    llloader_wr_sym(&w, "action", action);
    llloader_wr_sym(&w, "slot",   slot);
    if (llloader_wr_end(&w) > 0) { llloader_llip_emit(buf); }
}

//! `(ota-death (seq 0) (reason R) (count N))` -- why this unit is in the loader, from the record
//! the VM's `(dead!)` left in NVS ([P255]).  Emitted once at boot, when a record exists.
//!
//! THIS IS THE FIRST TIME THE DEATH RECORD IS VISIBLE WITHOUT RUNNING THE VM, which is the point:
//! a latched unit never reaches the VM, so until now the only reader of that record was the thing
//! that could not run.
static void emit_death_if_any(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) { return; }
    char   reason[LLLOADER_MAX_SYMBOL + 1] = "";
    size_t len = sizeof(reason);
    esp_err_t e = nvs_get_str(h, LLLOADER_KEY_DEATH_REASON, reason, &len);
    uint8_t count = 0;
    if (nvs_get_u8(h, LLLOADER_KEY_DEATH_COUNT, &count) != ESP_OK) { count = 0; }
    nvs_close(h);
    if (e != ESP_OK || reason[0] == '\0') { return; }   //!< no record: nothing to say
    char buf[LLLOADER_MAX_FRAME_BYTES];
    llloader_wr_t w;
    llloader_wr_begin(&w, buf, sizeof(buf), "ota-death");
    llloader_wr_int(&w, "seq", 0);
    llloader_wr_sym(&w, "reason", reason);
    llloader_wr_int(&w, "count", (int32_t) count);
    if (llloader_wr_end(&w) > 0) { llloader_llip_emit(buf); }
    else { ESP_LOGW(TAG, "ota-death not emitted: reason '%s' does not fit the frame grammar", reason); }
}

// Shared INSTALL/RECOVER core: pull an image off the ladder, stream it into ota_0, and on a
// fully verified write hand control to it. Returns true only if we rebooted (or are about to).
//! `action` is the vocabulary's action symbol (`install` / `recover`) for the emitted frame, and is
//! passed rather than derived from `what` or from `want_golden`: `what` is an uppercase log string,
//! and want_golden is a SOURCE preference that happens to correlate with RECOVER today.  Deriving
//! the action from either would be right by coincidence.
static bool fetch_write_and_boot(const esp_partition_t *ota0, bool want_golden, const char *what,
                                 const char *action)
{
    if (ota0 == NULL) {
        ESP_LOGE(TAG, "%s: no ota_0 partition -- cannot install", what);
        return false;
    }
    image_source_t *src = llloader_select_image_source(want_golden);
    if (src == NULL) {
        ESP_LOGW(TAG, "%s: no image source available -> CONSOLE", what);
        return false;
    }
    esp_err_t err = llloader_ota_write(ota0, src);   // consumes + closes src
    //! EMITTED ON BOTH ARMS, BEFORE THE BRANCH.  A result frame only on success would make the
    //! interesting case -- the write that failed -- the silent one, which is backwards: that is
    //! the run somebody is trying to understand.
    emit_result(action, ota0);
    if (err != ESP_OK) {
        /*! CLEAR THE BLUE.  `ota_writer` sets INSTALL (blue, "do not unplug") when a write starts
            and nothing reset it on the failure path -- so a board whose write had FAILED, and which
            had moved on to retries, sat there telling the operator it was mid-install.  A status
            light that is wrong is worse than none: it is the stale-record defect with a lamp on it,
            and the one state where "do not unplug" is actively bad advice.
            Found 2026-10-06, the first time anyone could see the LED: the owner reported a steady
            blue while the log read `esp_ota_write at 0: ESP_ERR_OTA_VALIDATE_FAILED`. */
        llloader_led_set(LLLOADER_LED_RECOVER);
        ESP_LOGE(TAG, "%s: write failed (%s) -> CONSOLE", what, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%s: image verified in ota_0; booting it", what);
    return boot_into(ota0);
}

static bool do_install(const esp_partition_t *ota0);
static bool do_recover(const esp_partition_t *ota0);

/*! [P265] ONE implementation of "can ota_0 be handed off to", with the escalation made optional.

    The owner's ruling is that a latched unit waits in the loader and then boots a valid image ANYWAY,
    so `do_console()` needs the handoff decision WITHOUT its consequences: escalating to INSTALL from
    inside the console fallback would bump `install_tries` and spend another 300 s window, which is
    not what "boot it if you can" means.

    `escalate` is the whole difference.  The alternative was a second predicate beside this function
    answering "is it bootable?", and that is the hand-maintained-second-copy defect aimed at the one
    decision in the loader that must not drift: the three handoff cases here (VALID, UNDEFINED, and
    no-otadata-state) were arrived at by three separate hardware findings, one of which bricked a
    healthy board for an operator with a UART ([B724] and the ruling below). */
static bool do_normal_ex(const esp_partition_t *ota0, bool escalate)
{
    // otadata should already point at ota_0; factory only runs on fallback. If ota_0 already
    // holds a valid image, just hand off. If it's empty/undefined, escalate to INSTALL.
    if (ota0 == NULL) {
        if (!escalate) return false;
        ESP_LOGW(TAG, "NORMAL: no ota_0 -> escalate to INSTALL");
        return do_install(ota0);
    }
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_err_t e = esp_ota_get_state_partition(ota0, &st);
    if (e == ESP_OK && st == ESP_OTA_IMG_VALID) {
        // ota_0 is confirmed-good: the system is healthy, so any prior install succeeded.
        // Clear the install-retry counter.
        //
        // THIS IS THE ONLY PLACE THE LOADER CAN CLEAR IT, AND UNTIL 2026-10-01 IT WAS UNREACHABLE.
        // The comment here used to read "belt-and-suspenders; the VM also clears it on boot".  That
        // was FALSE, and false in the direction that costs most: a reader who checked found an
        // explicit assurance that another path covered them.  `ota-boot-ok!` is the VM side, and it
        // had ZERO call sites -- every mention in the tree was documentation telling the reader to
        // call it.  Since `ota-boot-ok!` is also the only thing that ever sets VALID, the two paths
        // were not redundant, they were ONE path, and it was not wired.  `install_tries` therefore
        // only ever rose, and at OTA_MAX_INSTALL_TRIES this rung was exhausted for the life of the
        // unit ([B724]).  `scm/core/setup.scm` now calls `(ota-boot-ok!)` at the end of startup.
        install_tries_clear();
        llloader_led_set(LLLOADER_LED_NORMAL);
        ESP_LOGI(TAG, "NORMAL: ota_0 VALID; handoff");
        return boot_into(ota0);
    }
    // UNCONFIRMED IS NOT BROKEN -- hand off, and do NOT clear the anti-loop counter.  Two states
    // mean "nobody has told me this image is good", and they must be treated alike:
    //
    //   ESP_OTA_IMG_UNDEFINED  written through the OTA API, not yet self-confirmed.
    //   ESP_ERR_NOT_FOUND      no otadata entry for this slot AT ALL -- which is what an image
    //                          flashed with esptool looks like, i.e. EVERY development build.
    //
    // THE SECOND ONE USED TO FALL THROUGH TO "not bootable -> INSTALL", AND THAT WAS A TRAP WITH A
    // ONE-COMMAND REPRODUCTION (owner's ruling, 2026-10-01): `w3 make upload_app <env>` followed by
    // `(ota-reboot-to-loader)` reliably walked a HEALTHY board down the whole ladder -- INSTALL
    // (exhausted, see [B724]), RECOVER (no golden image on a dev board), console -- and the board
    // then needed an operator with a UART and a 1.8 MB image push.  Measured on esp32s3-n32r16-00
    // 2026-09-30, on a board that booted that very image perfectly by itself a minute earlier.
    //
    // The distinction the old code drew was not "good vs bad", it was "written by the OTA API vs
    // written by anything else", which is not a statement about the image.  The counter is still not
    // cleared here, so the anti-loop guarantee is unchanged: confirming an image remains the VM's
    // job and nothing here pretends to do it.
    if (e == ESP_OK && st == ESP_OTA_IMG_UNDEFINED) {
        llloader_led_set(LLLOADER_LED_NORMAL);
        ESP_LOGI(TAG, "NORMAL: ota_0 UNDEFINED (unconfirmed); handoff");
        return boot_into(ota0);
    }
    if (e == ESP_ERR_NOT_FOUND) {
        llloader_led_set(LLLOADER_LED_NORMAL);
        ESP_LOGI(TAG, "NORMAL: ota_0 has no otadata state (esptool-written?); unconfirmed, handoff");
        return boot_into(ota0);
    }
    if (!escalate) return false;
    ESP_LOGW(TAG, "NORMAL: ota_0 state=%d err=%s not bootable -> INSTALL",
             (int) st, esp_err_to_name(e));
    return do_install(ota0);
}

//! The ladder's NORMAL rung: decide, and escalate to INSTALL when there is nothing to boot.
static bool do_normal(const esp_partition_t *ota0) { return do_normal_ex(ota0, true); }

static bool do_install(const esp_partition_t *ota0)
{
    // Anti-loop: if we've already burned our install attempts on a failing image, stop
    // re-installing it and escalate to RECOVER (which prefers a known-good golden image).
    if (install_tries_exhausted("INSTALL")) {
        /*! LATCHED, NOT RECOVER, AND THE DISTINCTION IS THE WHOLE POINT OF A STATUS LIGHT.  Out
            of install attempts is the state an operator must ACT on -- it is where a unit sits until
            someone holds BOOT or pushes an image -- and it looked identical to every other loader
            state from across the bench.  Red here; the amber below is the ordinary recovery path. */
        llloader_led_set(LLLOADER_LED_LATCHED);
        ESP_LOGW(TAG, "INSTALL exhausted -> escalate to RECOVER (golden)");
        return do_recover(ota0);
    }
    ESP_LOGI(TAG, "INSTALL: fetch VM from source ladder -> ota_0");
    return fetch_write_and_boot(ota0, false, "INSTALL", "install");
}

static bool do_recover(const esp_partition_t *ota0)
{
    // RECOVER is the escalation target and biases toward a locally-pinned golden image, so it is
    // intentionally NOT gated by install_tries. If it too finds no source / fails, we drop to
    // CONSOLE (fetch_write_and_boot returns false and app_main falls through to do_console).
    llloader_led_set(LLLOADER_LED_RECOVER);
    ESP_LOGI(TAG, "RECOVER: ota_0 rolled back; prefer known-good golden image");
    return fetch_write_and_boot(ota0, true, "RECOVER", "recover");
}

// THIS IS THE TERMINAL STATE OF THE TRUSTED ROOT.  IT IS NO LONGER A ONE-WAY DOOR ([B731] fix,
// 2026-10-03) -- IT OFFERS THE IMAGE LADDER, FOREVER, IN BOUNDED PASSES.
//
// Every failed path in app_main() falls through to here (`if (!handed_off) do_console(ota0)`), and so
// does an explicit `pending_update = CONSOLE` request.  Nothing here clears `pending_update` and the
// VM cannot run to clear it, so a unit that arrives here STAYS here across reboots -- that part is
// unchanged and is correct: it is the trusted root holding a unit it cannot vouch for.  What changed
// is what "staying" MEANS.  It used to mean idling in a `for(;;) vTaskDelay(1000)` stub, answering
// nothing, recoverable only over USB by someone holding the board.  It now means re-offering the
// console rung, so anyone who later attaches a UART (or BLE) can push an image and the unit installs
// it through the same writer, the same digest check and the same `(ota-result ...)` frame as a normal
// install.  See do_console() below for the reasoning and the one decision it embeds.
//
// WHAT THE OLD STUB COST, so nobody restores it as "simpler": a rejected image reached it.  Pushing a
// deliberately mis-digested image on 2026-10-01 produced `SHA-256 MISMATCH`, the correct refusal, and
// then the stub -- the loader did its safety job and bricked the unit for doing it.  One corrupted
// download was enough, with no operator error anywhere.
//
// STILL OPEN AND NOT ADDRESSED HERE -- the DEAD-latch exit:
//   * the VM's DEAD mode latches into this state on its Nth death ([P255]).  That latch was proven
//     on hardware for deaths 1 and 2 on 2026-09-30; death 3 was deliberately not run, because it
//     would have needed a reflash to undo.  `dead-latch-after` in scm/core/setup.scm carries the
//     matching warning.
//   * the exit from it is TBD by the owner -- a press-and-hold on a board button, sampled HERE,
//     because this is the only actor alive on a latched unit ([P255], parked).
//     THIS IS A SEPARATE QUESTION FROM THE ONE ABOVE, and conflating the two is what delayed this
//     fix: a latched unit needs a way for a person with no tools to say "try again", which is the
//     button.  A unit that merely failed an install needs a way to be GIVEN a good image, which is
//     the console rung and needed no ruling at all.
//
// The LLIP frame reader for a query/reply console (`(ota-state)`, `(ota-ladder)`) also already exists
// and is tested -- llloader_llip.{h,c}, 70 host arms, `w3 test loader` ([P253] phase 1).  That is
// [P253] steps B/C and is still unbuilt: what this fix adds is the IMAGE path, not the QUERY path.
/*! --- [P253] step B: the press-and-hold exit from the terminal state ---

    WHAT IT IS FOR.  A unit in the terminal state stays there across reboots, because nothing clears
    `pending_update` and the VM cannot run to clear it.  [B731] made that survivable -- the state now
    offers the image ladder rather than idling -- but it still requires an operator with a UART and an
    image.  THIS is the path for a person with NO tools: hold the board button, get one more attempt
    at a normal boot.

    IT RUNS AS ITS OWN TASK, AND THAT IS FORCED RATHER THAN CHOSEN.  The console rung blocks for up to
    300 s inside `read_exact()`, so a button polled from that thread would be sampled only between
    passes -- an operator would have to hold it at exactly the right instant and would otherwise
    conclude the button does nothing.  An independent input wants an independent sampler.

    IT ARMS ONLY AFTER SEEING THE PIN HIGH, which is the safety property to read twice.  A board whose
    GPIO0 is strapped or shorted low would otherwise read as "held from the moment we booted" and
    reboot the unit, forever, in the trusted root -- turning a recovery aid into a boot loop on
    exactly the hardware least able to recover.  So a release must be OBSERVED before a hold counts.

    IT CLEARS `pending_update` AND NOT `death_count`, which [P253] step B asks for in as many words
    ("a hold that also cleared the count would pass a test that only checked the boot").  The two
    answer different questions: pending_update is "where should I go next", death_count is "has this
    application been failing".  Clearing the count would grant an amnesty the operator did not ask
    for -- after a hold, one more `(dead!)` must latch again immediately. */
#define BTN_GPIO        0        //!< the BOOT button on every devkit this loader targets.  A strap
                                 //!< pin at reset and an ordinary input afterwards.
#define BTN_HOLD_MS     3000     //!< deliberate: long enough that no accidental brush triggers it
#define BTN_POLL_MS     50

/*! IT MUST NOT USE ESP_LOG, AND THAT IS THE WHOLE REASON THIS FUNCTION LOOKS ODD.

    MEASURED 2026-10-03 on esp32-s3-devkitc-1: with this task logging normally the loader asserted
    `xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)` five lines after the console rung
    printed its banner, rebooted, and LOOPED -- boot, ready, assert, reboot, forever.  The task never
    managed to emit even its first message.  A one-variable control (this task not started, nothing
    else changed) booted clean with zero asserts, which is what pinned it here.

    THE MECHANISM: the console rung calls `uart_driver_install(UART_NUM_0, ...)` and owns UART0 for
    its 300 s window, with a tx buffer of 0.  ESP_LOG from ANOTHER task then reaches the same UART
    through the driver's mutex, and the rung installs and DELETES that driver around each pass -- so
    a log write from here races a handle that is being torn down.  Two tasks, one peripheral, one of
    them owning it.
    I wrote in step B's own comment that "an independent input wants an independent sampler" and did
    not follow it through: an independent LOGGER on a contended peripheral is a different matter, and
    that is the half I got wrong.

    `esp_rom_printf` writes directly to the UART FIFO with no driver, no mutex and no allocation --
    it is what the IDF's own panic handler uses, for the same reason.  It is less pretty than
    ESP_LOGW and it cannot be turned off by a log level, which is acceptable for three messages that
    only ever appear in the terminal state. */
static void button_exit_task(void *arg)
{
    (void) arg;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     //!< active-low button to ground
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        esp_rom_printf("BUTTON: cannot configure GPIO%d -- no press-and-hold exit\n", BTN_GPIO);
        vTaskDelete(NULL);
        return;
    }

    /*! THE DECISION LIVES IN `llloader_button.c`, AND THIS TASK IS THE BRIDGE TO IT.
        What stays here is everything that needs hardware: the pin, the printing, the NVS erase and
        the restart.  What left is the arithmetic -- what counts as a hold, what a release means,
        when the sampler starts trusting the pin -- which now carries 15 host arms in
        `w3 test loader`, including the strap case and both sides of the threshold.

        WHY THE SPLIT, IN ONE SENTENCE: step C's five defects were ALL in the half that could not be
        host-tested, and step B's first criterion sat open for a week because checking it needed a
        board and a finger.  Moving the decision out is the cheapest form of that lesson.

        THERE MUST BE EXACTLY ONE COPY OF THIS LOGIC.  If this loop ever re-implements the timing
        "for clarity", the host arms are then testing a parallel implementation and will keep passing
        while the loader does something else -- which is the hand-maintained-second-copy defect, in
        the one component whose job is to work when everything else does not. */
    llloader_btn_t btn;
    llloader_btn_init(&btn);
    esp_rom_printf("BUTTON: hold GPIO%d for %d s to clear the pending action and retry a normal boot\n",
                   BTN_GPIO, BTN_HOLD_MS / 1000);
    for (;;) {
        switch (llloader_btn_step(&btn, gpio_get_level(BTN_GPIO), BTN_POLL_MS, BTN_HOLD_MS)) {
        case LLLOADER_BTN_ARM:
            esp_rom_printf("BUTTON: armed (GPIO%d observed released)\n", BTN_GPIO);
            break;
        case LLLOADER_BTN_SHORT:
            /*! REPORT A SHORT PRESS, because silence made a real test unrunnable.
                [P253 step B] criterion 1 is "a tap clears nothing", and a tap produced NO output at
                all -- so a press and no press left identical transcripts, and the criterion could
                only be checked by asking a person to press again and trusting them.  The owner
                pressed four times across three capture windows and none of it was readable; that was
                this missing line, not their pressing.
                An absence is not an observation.  A short press now says so itself.
                `reported_ms`, NOT `held_ms`: the release has already zeroed the latter, so the
                obvious field prints "released after 0 ms" -- unreadable again, by a new route. */
            esp_rom_printf("BUTTON: released after %d ms -- short of %d ms, nothing cleared\n",
                           btn.reported_ms, BTN_HOLD_MS);
            break;
        case LLLOADER_BTN_CLEAR:
            //! DO NOT TOUCH death_count.  See the note above: clearing it would be an amnesty.
            ota_erase_key(OTA_KEY_PENDING);
            esp_rom_printf("BUTTON: held %d ms -- cleared the pending action, NOT the death"
                           " count; restarting for one normal boot\n", btn.reported_ms);
            vTaskDelay(pdMS_TO_TICKS(100));  //!< let the log line reach the UART
            esp_restart();
            break;
        case LLLOADER_BTN_NOTHING:
        default:
            break;
        }
        /*! TICK THE STATUS LAMP FROM HERE, because this is the one loop that runs no matter which
            rung the loader is in.  The first placement ticked only from the console rung's byte
            loop, so a LATCHED board stuck in the network rung's retry cycle showed SOLID red
            instead of blinking -- and solid-versus-blinking is the entire distinction between
            "this is the situation" and "this situation needs you".  A lamp that cannot blink where
            the board actually sits is not carrying the signal it was added for.
            Cheap and bounded: rate-limited to 400 ms inside, and a 24-bit WS2812 frame is ~30 us. */
        llloader_led_tick();
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

/*! --- [P253] step C: describe this unit, for the console's query answerer ---

    THIS IS THE ONLY PART OF STEP C THAT NEEDS A BOARD, and it is deliberately the smallest part.
    All the POLICY -- which query is answered, with which fields, in what order -- is in
    llloader_query.c, which is pure and has 27 host arms.  This function is the narrow bridge: it
    reads NVS and the OTA state, which only this file has the handles for, and hands over plain
    values.  Keeping the split here means a change to what we ANSWER is testable on a laptop and only
    a change to what we can SEE needs hardware.

    EVERY FIELD DEGRADES TO A HONEST VALUE RATHER THAN A PLAUSIBLE ONE.  An unreadable NVS gives
    `tries 0` and `reason none`, which is what a fresh unit also reports -- that ambiguity is
    accepted deliberately, because the alternative is inventing a distinct "unknown" symbol for a
    case the operator cannot act on differently.  What must NOT happen is reporting a stale or
    fabricated value as a measurement. */
static void gather_status(llloader_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    out->slot = (run != NULL && run->label != NULL) ? run->label : "factory";

    //! ota_0 as THIS loader sees it -- the same three-way reading do_normal() acts on, so a query
    //! and the boot decision cannot disagree about the same slot ([B724]'s ESP_ERR_NOT_FOUND case
    //! is `undefined`, not `absent`: the image is there, nobody has vouched for it).
    const esp_partition_t *ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota0 == NULL) {
        out->image = "absent";
    }
    else {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        esp_err_t e = esp_ota_get_state_partition(ota0, &st);
        if (e == ESP_OK && st == ESP_OTA_IMG_VALID) { out->image = "valid"; }
        else                                        { out->image = "undefined"; }
    }

    //! The pending action by NAME, from the generated table, so a renumbered enum cannot silently
    //! report the wrong one.  Out of range reads as `normal`, which is what the loader itself does.
    uint8_t pending = ota_get_u8(OTA_KEY_PENDING, LOADER_NORMAL);
    out->pending = (pending < (uint8_t) LLLOADER_ACT_COUNT)
                 ? llloader_action_t_names[pending]
                 : llloader_action_t_names[LLLOADER_ACT_NORMAL];

    out->tries = (int32_t) ota_get_u8(OTA_KEY_INSTALL_TRIES, 0);

    //! The death record ([P255]).  `none` when absent -- NOT an empty symbol, which the grammar
    //! forbids anyway (a symbol is 1..LLLOADER_MAX_SYMBOL chars), so an absent reason must have a
    //! spelling or the whole frame would fail to build.
    static char reason[LLLOADER_MAX_SYMBOL + 1];
    reason[0] = '\0';
    uint8_t count = 0;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(reason);
        if (nvs_get_str(h, LLLOADER_KEY_DEATH_REASON, reason, &len) != ESP_OK) { reason[0] = '\0'; }
        if (nvs_get_u8(h, LLLOADER_KEY_DEATH_COUNT, &count) != ESP_OK)         { count = 0; }
        nvs_close(h);
    }
    out->reason = (reason[0] != '\0') ? reason : "none";
    out->count  = (int32_t) count;
}

//! Seconds to wait before offering the console again, when a pass produced no usable image.
//! A FLOOR, NOT A POLICY: the console rung bounds its own wait at 300 s, but a rung that fails
//! IMMEDIATELY (no ota_0 to write to, no UART driver) would turn the loop below into a tight spin,
//! which is worse than the idle it replaces.  This makes the worst case "offers a window every
//! minute" rather than "melts the watchdog".
#define CONSOLE_RETRY_S 60

/*! [P265] HOW LONG A LATCHED UNIT WAITS BEFORE BOOTING A VALID IMAGE ANYWAY.  Owner ruling
    2026-10-07: *wait in the loader for an operator or an image, then boot a valid `ota_0`.*

    COUNTED IN PASSES, NOT MINUTES, because a pass is the grain this loop actually has: each one
    offers the whole ladder and the console rung alone listens for `CON_WAIT_HDR_MS` (300 s) before
    the `CONSOLE_RETRY_S` (60 s) gap.  Five passes is therefore ~30 minutes -- long enough for
    somebody who saw the reboot to attach a cable, short enough that an unattended unit is back in
    service within the hour.  Expressing it in minutes would invite someone to set 5 and get one pass.

    WHAT IT BUYS IS A BOUNDED RETRY RATE, WHICH IS NOT THE SAME AS PATIENCE.  Before [B772] a
    repeatedly-dying unit flapped as fast as its application could die.  With the latch honoured it
    waited forever.  With this it retries AT MOST ONCE PER N: wedge -> the watchdog records the death
    and redirects to factory -> the loader offers the ladder for N -> the valid image boots -> wedge.
    So this number is the duty cycle of a failing unit, and the interval an operator's arrival has to
    beat.  It is not a politeness setting.

    IT APPLIES TO EVERY ROUTE INTO do_console(), AND THAT IS FORCED RATHER THAN CHOSEN.  An operator's
    `(ota-request-console!)` and the [P255] latch write the SAME `pending_update` value, so the loader
    cannot tell "a person asked for the console" from "the unit took itself out of service".  Until
    something distinguishes them, a timeout that spared the operator's case would have to spare both.
    The press-and-hold exit already covers *let me out now*; **nothing covers *let me stay*** -- an
    operator who needs longer than N must push an image, which is what they came to do. */
/*! OVERRIDABLE AT BUILD TIME SO THE ACCEPTANCE ARMS ARE RUNNABLE AT ALL.  At the shipped value each
    arm costs ~30 minutes of board time, and the DISAGREEING arm (a unit with nothing bootable must
    still be in the loader after 2N) costs an hour -- which is how an arm ends up never run and the
    behaviour believed instead.  `-DCONSOLE_FALLBACK_PASSES=1` makes a pass-and-fallback cycle ~6
    minutes, bounded by the console rung's own 300 s listen rather than by this number.
    Same reasoning as `LLLOADER_NO_BUTTON`: a control that lives in a build flag is one the next
    session can reproduce, where a hand-edited constant is one they have to be told about. */
#ifndef CONSOLE_FALLBACK_PASSES
#define CONSOLE_FALLBACK_PASSES 5
#endif

static void do_console(const esp_partition_t *ota0)
{
    //! THE TERMINAL STATE NOW OFFERS THE CONSOLE RUNG INSTEAD OF IDLING FOREVER ([B731]).
    //!
    //! WHAT THIS REPLACED, and why it was not merely unfinished: a `for(;;) vTaskDelay(1000)` stub
    //! carrying `TODO(Ph2b)`.  Every failed path in app_main() falls through to here, so that stub
    //! was the terminal state of the trusted root -- a unit arriving here answered nothing, and
    //! returned here on every subsequent boot because nothing clears `pending_update`.  From the
    //! outside it was indistinguishable from a brick: one warning, then silence forever.
    //!
    //! THE ROUTE THAT MATTERS IS NOT THE DELIBERATE ONE.  `(ota-request-console!)` reaching this is
    //! an operator's choice.  **A REJECTED IMAGE reaching it is not:** measured 2026-10-01, pushing
    //! a mis-digested image produced `SHA-256 MISMATCH`, the correct refusal, and then this stub --
    //! so the loader did its safety job and bricked the unit for it.  One corrupted download was
    //! enough.  That is what makes this a defect rather than a missing feature.
    //!
    //! WHY THE FIX IS SMALL: the thing the stub's TODO describes ALREADY EXISTS and is tested.
    //! `llloader_select_image_source()` rung 3 is the UART console (`src_console.c`), which
    //! resynchronises on the `LLIM` magic, bounds itself at 300 s and installs what arrives; rung 3b
    //! is the same over BLE.  It recovered this bench's board four times on 2026-10-01 to 10-03.  So
    //! this is not new transport -- it is calling the implementation from the place that had a stub.
    //!
    //! IT GOES THROUGH `fetch_write_and_boot()` RATHER THAN OPENING THE RUNG DIRECTLY, so an image
    //! arriving here gets the identical treatment to one arriving via INSTALL: the same writer, the
    //! same digest check, and the same `(ota-result ...)` frame on BOTH arms.  A second write path
    //! would be a second set of bugs, and the digest check is the one branch that must not have a
    //! variant.
    //!
    //! RETRYING RATHER THAN IDLING AFTER A WINDOW CLOSES IS A DECISION, and it is the only one here
    //! that was not already implied by the existing code -- flagged for the owner rather than
    //! buried.  A unit that re-offers a window every minute is RECOVERABLE by anyone who later
    //! attaches a UART; a unit that idles is recoverable only over USB, by someone holding the
    //! board.  Re-entering the ladder each pass is deliberate too: an `aux-fs` or network image
    //! that appears AFTER this unit gave up is then found, where the old stub could never see it.
    //! `install_tries` is NOT bumped here -- the bump belongs to do_install(), so looping cannot
    //! exhaust the counter that guards against install loops.
    //! The status provider is registered in app_main() -- see the note there.  It was registered at
    //! THIS point and that was the same defect as the button task, two lines away, which I fixed
    //! without noticing its twin ([B743]).

    //! The press-and-hold sampler is started in app_main() rather than here -- see the note there.
    //! It was briefly started at this point and that was WRONG: do_console() is reached only AFTER
    //! INSTALL and RECOVER have each spent up to 300 s in the console rung, so the button was
    //! unavailable during exactly the waits an operator would be standing there pressing through.

    /*! SAY THE POLICY OUT LOUD, ONCE.  Two reasons, and the second is why it is not decoration:
        an operator standing at a board needs to know whether it will wait for them or boot without
        them -- and a build-time override (`-DCONSOLE_FALLBACK_PASSES=N`) is otherwise INVISIBLE in
        the binary.  The flash size does not change when the constant does, so without this line a
        test arm cannot prove the flag it is varying ever reached the compiler, which is the
        [P204]-era trap of A/B-ing two identical builds and reading "no effect". */
    ESP_LOGW(TAG, "CONSOLE: will try a normal boot after %d pass(es) if ota_0 is bootable",
             CONSOLE_FALLBACK_PASSES);

    for (unsigned pass = 1; ; pass++) {
        if (ota0 != NULL) {
            ESP_LOGW(TAG, "CONSOLE: operator fallback -- offering the image ladder, pass %u", pass);
            //! Returns only on FAILURE: a successful write boots the new image and never comes back.
            (void) fetch_write_and_boot(ota0, false, "CONSOLE", "install");
        }
        else {
            //! No slot to write into, so no pass can succeed.  Say so every time rather than once:
            //! this is the one state where the loader genuinely cannot help itself, and a single
            //! warning followed by silence is exactly what made the old stub look like a brick.
            ESP_LOGE(TAG, "CONSOLE: no ota_0 partition -- nothing can be installed (pass %u)", pass);
        }
        ESP_LOGW(TAG, "CONSOLE: pass %u offered no usable image; retrying in %d s",
                 pass, CONSOLE_RETRY_S);
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_RETRY_S * 1000));

        /*! [P265] AFTER N PASSES, BOOT A VALID IMAGE ANYWAY -- the owner's ruling.  See
            CONSOLE_FALLBACK_PASSES for why this is counted in passes and what the bound buys.

            `do_normal_ex(ota0, false)` is the handoff decision WITHOUT the escalation: it returns
            false and logs nothing when there is nothing bootable, so a unit with no usable image
            keeps offering the ladder forever, which is today's behaviour and is correct.  A timeout
            that "recovered" a unit with nothing to boot would reset-loop it, which is worse than
            waiting -- that is the disagreeing arm this fallback has to survive.

            `pending_update` IS LEFT ALONE ON PURPOSE.  Clearing it here would make the fallback
            indistinguishable from the press-and-hold exit, and the hold means something stronger: a
            person decided.  If the image boots and confirms itself, `(ota-boot-ok!)` clears the key
            as part of marking the VM healthy; if it dies again, the key is still set and the unit
            comes back here -- which is exactly the bounded retry the ruling asks for. */
        if (pass >= CONSOLE_FALLBACK_PASSES) {
            ESP_LOGW(TAG, "CONSOLE: %u passes with no operator and no image -- trying a normal boot",
                     pass);
            if (do_normal_ex(ota0, false)) {
                /*! UNREACHABLE IN PRACTICE -- `boot_into()` restarts the chip.  Looping rather than
                    returning is the safe residual: `app_main()` calls this function as its terminal
                    state and treats it as never returning, so falling out of it would end
                    `main_task` with no decision made at all. */
                ESP_LOGE(TAG, "CONSOLE: handoff reported success WITHOUT restarting -- staying put");
            }
            else {
                ESP_LOGW(TAG, "CONSOLE: nothing bootable in ota_0 -- staying in the loader");
            }
        }
    }
}

void app_main(void)
{
    /*! LED FIRST, BEFORE NVS AND BEFORE ANY DECISION.  The states worth seeing are the ones that
        happen in the first second -- and if the loader dies during its own startup the operator
        should be left with dim white rather than an unlit board, which is indistinguishable from no
        power.  A no-op unless LOADER_LED_GPIO is set; see llloader_led.h. */
    llloader_led_init();
    llloader_led_set(LLLOADER_LED_BOOT);

    ESP_LOGI(TAG, "OTA llloader boot (factory / trusted recovery root)");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs erase+reinit (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const esp_partition_t *ota0 = inspect_partitions();
    emit_death_if_any();          //!< [P253]/[P255] say WHY we are here, before deciding anything
    loader_action_t action = read_pending_action();

    /*! [P253] step C: register who can describe this unit, for EVERY path below.

        THIS WAS THE B743 DEFECT, AND IT IS THE SAME ONE AS THE BUTTON TASK BELOW.  Both were
        originally set up inside do_console(), which is reached only after INSTALL and RECOVER have
        each had their turn -- so on a unit sitting in RECOVER's console rung the provider was NULL,
        `reply_status()` refused for want of a snapshot, and `(ota-state)` was never answered while
        `(ota-ladder)` -- which needs no snapshot -- answered perfectly.  That asymmetry is what the
        bug looked like from outside.

        I FIXED THE BUTTON TASK'S PLACEMENT AND WALKED PAST THIS LINE TWO LINES AWAY.  The lesson is
        not "be careful": it is that `do_console()` LOOKS like the console's setup function and is
        not -- the console RUNG is reached from the ladder, by three different callers, long before
        do_console() runs.  Anything the rung needs belongs where every caller passes, which is here.
        If a third thing is ever added for the console, this is the place. */
    llloader_console_set_status_provider(gather_status);

    /*! [P253] step B: start the press-and-hold sampler, for EVERY path below.

        MOVED HERE FROM do_console() 2026-10-03, and the first placement was a real defect rather
        than a tidiness question.  `do_console()` is reached only after INSTALL and RECOVER have each
        had their turn, and each of those can sit in the console rung for 300 s -- so the button did
        not exist during the two five-minute windows an operator is most likely to be standing at the
        board pressing it.  Measured: a unit in RECOVER printed `CONSOLE READY` and the task had not
        been created, so not one `BUTTON:` line ever appeared.

        AND MY REASON FOR THE OLD PLACEMENT WAS WRONG, not merely over-cautious.  I argued a button
        that reboots a HEALTHY unit would be a new way to lose a running robot.  But this is the
        LOADER: a healthy unit boots the VM out of `ota_0` and `app_main()` never runs at all.  Every
        path that reaches this line is a unit that is already not running its application.  The only
        exposure is the ~200 ms of `do_normal()` handing off, where a hold would clear a
        `pending_update` that is already `normal` and restart -- harmless, and arguably what the
        operator meant.

        It is started AFTER read_pending_action() so the log order reads sensibly (why we are here,
        then what the operator can do about it), and that is the only reason for the position. */
#if LLLOADER_NO_BUTTON
    /*! [B755] THE ONE-VARIABLE CONTROL, as a build flag rather than a commented-out line.
        `assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)` fires during a
        console-rung image push and reboots the loader mid-transfer.  The same assert was traced to
        this task once before (2026-10-03) by disabling it and counting asserts, so that is the first
        arm to run again -- and last time the control was a line commented out by hand, which is not
        a control anyone else can reproduce.  Build with `-DLLLOADER_NO_BUTTON=1` to omit the task.
        KEEP THIS GATE after B755 closes: the next assert on this UART will want the same arm, and a
        flag that already exists costs nothing to use. */
    ESP_LOGW(TAG, "BUTTON: exit task OMITTED (LLLOADER_NO_BUTTON) -- [B755] control build, not for a unit");
#else
    xTaskCreate(button_exit_task, "btn_exit", 2560, NULL, 5, NULL);
#endif

    bool handed_off = false;
    switch (action) {
    case LOADER_NORMAL:  handed_off = do_normal(ota0);      break;
    case LOADER_INSTALL: handed_off = do_install(ota0);     break;
    case LOADER_RECOVER: handed_off = do_recover(ota0);     break;
    case LOADER_CONSOLE: default:                           break;
    }

    if (!handed_off) {
        do_console(ota0);  // never returns
    }
}
