// Copyright 2026 by Frobenius Norm LLC 2026-10-03
// GENERATED from scm/features/llloader-vocab.scm -- DO NOT EDIT
//
// The VM's half of the loader<->VM NVS contract (namespace "ota-p125").  The factory
// loader is a separate ESP-IDF project, so the two ends cannot share a header -- they
// share a SOURCE, and each gets a generated one.  Editing this file instead of the
// vocabulary puts the two ends back out of step, which is the failure it exists to stop:
// a key name wrong on one side is not an error anywhere, it just reads as "nothing
// pending" forever.

#pragma once

// The value written into `pending_update`.  THESE ARE INDICES INTO THE ACTION LIST, so
// the order in the vocabulary is the wire protocol; the loader's `loader_action_t` is
// generated from the same list and the generator refuses if the two disagree.
#define LL_OTA_PENDING_NORMAL   0
#define LL_OTA_PENDING_INSTALL  1
#define LL_OTA_PENDING_RECOVER  2
#define LL_OTA_PENDING_CONSOLE  3


// The anti-loop limit, from the vocabulary.  The VM's Scheme `dead-latch-after` must
// agree and llip_vocab_gen checks that it does -- see check_dead_latch_against_setup.
// The STALENESS WATCH needs it in C: it fires when the evaluator cannot run, so it
// cannot ask Scheme what the limit is.
#define LL_OTA_DEAD_LATCH_AFTER  3

// NVS keys.  Spelled once, here, from the vocabulary.
#define LL_OTA_KEY_PENDING_UPDATE  "pending_update"   // u8
#define LL_OTA_KEY_INSTALL_TRIES   "install_tries"   // u8
#define LL_OTA_KEY_DEATH_REASON    "death_reason"   // str
#define LL_OTA_KEY_DEATH_COUNT     "death_count"   // u8
#define LL_OTA_KEY_WIFI_SSID       "wifi_ssid"   // str
#define LL_OTA_KEY_WIFI_PASS       "wifi_pass"   // str
#define LL_OTA_KEY_IMG_URL         "img_url"   // str
#define LL_OTA_KEY_IMG_TOKEN       "img_token"   // str
#define LL_OTA_KEY_IMG_URL_GOLD    "img_url_gold"   // str

// A death reason is a SYMBOL carried in an `ota-death` frame, so it obeys the frame
// grammar's symbol bound.  Write a longer one and the loader's reader refuses the frame
// at the exact moment it matters -- hence the bound is shared, not restated.
#define LL_OTA_MAX_REASON 24
