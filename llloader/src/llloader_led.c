// Copyright 2026 by Frobenius Norm LLC 2026-10-06 09:20:00
// llloader_led.c -- the onboard RGB LED as the loader's status display.  See llloader_led.h for why.
#include "llloader_led.h"

#include <stdbool.h>

/*! HOW ACTIONABLE A STATE IS, which is what the lamp should be showing.

    [owner 2026-10-06] "i am not watching it all the time.  it is blue now."  That is the defect in
    the first design, stated exactly: `llloader_led_set` took the LATEST state, so the colour was
    whatever happened most recently rather than what matters.  A write is a ONE-SECOND event inside a
    sixty-second retry loop, so a board that is latched and failing to recover showed BLUE -- "do not
    unplug" -- to anyone who happened to glance during the write, while the fact they needed was that
    the unit is out of install attempts and waiting for a person.

    A STATUS LIGHT IS READ AT A RANDOM MOMENT.  It therefore has to show the highest-priority
    STANDING condition, not the newest transition, or it is only correct for whoever was watching
    when the transition happened -- which is nobody.

    BOOT and NORMAL are the exceptions and they RESET the display: a fresh boot starts a new
    decision cycle, and a successful handoff means the standing problem is over.  Everything else
    only takes the lamp if it outranks what is already there. */
static int led_rank(llloader_led_state_t st)
{
    switch (st) {
        case LLLOADER_LED_OFF:          return 0;
        case LLLOADER_LED_BOOT:         return 1;
        case LLLOADER_LED_NORMAL:       return 2;
        case LLLOADER_LED_RECOVER:      return 3;
        case LLLOADER_LED_INSTALL:      return 4;
        case LLLOADER_LED_CONSOLE_WAIT: return 5;   /* waiting for a person */
        case LLLOADER_LED_LATCHED:      return 6;   /* out of attempts: the most actionable fact */
        default:                        return 0;
    }
}

/*! Does `nw` take the lamp from `cur`?  PURE, AND OUTSIDE THE HARDWARE GUARD ON PURPOSE.

    The loader's own step-C lesson is that every hardware defect it had lived in the half that could
    not be host-tested, so the policy half is kept separable: this function knows nothing about RMT,
    GPIOs or a WS2812, and `llloader_led_test.c` drives it directly.  A rule about which state a
    status lamp should show is exactly the kind of thing that is easy to write, easy to believe, and
    wrong at a random glance -- which is how it was wrong the first time. */
bool llloader_led_takes(llloader_led_state_t cur, llloader_led_state_t nw)
{
    /* A fresh boot or a good handoff always resets; see led_rank's comment. */
    if (nw == LLLOADER_LED_BOOT || nw == LLLOADER_LED_NORMAL) { return true; }
    return led_rank(nw) >= led_rank(cur);
}


#ifndef LOADER_LED_GPIO

/* NOT CONFIGURED -> NOTHING.  Empty definitions rather than #if at every call site: the callers stay
   readable, and a build without the flag links the same symbols with no behaviour.  See the header
   for why this is off by default. */
void llloader_led_init(void) { }
void llloader_led_set(llloader_led_state_t st) { (void) st; }
void llloader_led_tick(void) { }

#else

#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_rom_sys.h"   //!< esp_rom_delay_us for the boot identify flash
#include "esp_timer.h"

/*! RMT, NOT A BIT-BANG.  A WS2812 bit is 1.25 us with +/-150 ns tolerance, which on a 240 MHz core
    means counting CPU cycles with interrupts disabled -- a critical section in the trusted root, to
    light an LED.  RMT is the supported path, the driver already ships in the IDF this project
    builds against, and it costs ~10 KB in a factory app currently at 74.4% of its partition.
    Correctness over bytes, where the bytes are available. */
#define LED_RES_HZ      10000000u          /* 100 ns per tick */

/*! WS2812B TIMINGS, FROM THE DATASHEET -- and the first set was out of spec in the direction that
    produces a SELECTIVE failure, which is the hardest kind to read off an LED.

    Datasheet: T0H 0.4 us, T0L 0.85 us, T1H 0.8 us, T1L 0.45 us, +/-150 ns, bit period ~1.25 us.
    The first draft used T0H 0.3 / T1H 0.6, both BELOW the low tolerance bound -- a 0.6 us high
    pulse sits under the part's 1-vs-0 discrimination threshold, so some bit patterns latch and
    others do not.

    THAT IS WHY THE LAMP LOOKED STUCK.  Measured 2026-10-06: the policy was provably correct
    (`led: 1 -> 6 take`, `led: 6 -> 4 BLOCK` on the wire), so red WAS written -- and the LED kept
    showing the blue it had latched earlier.  A WS2812 holds its last value indefinitely, including
    across a reset, so a write whose bits are misread leaves the previous colour standing and the
    display looks frozen rather than wrong.  Two rebuilds were spent on the policy half because of
    it, and the arms over the policy all passed, correctly, the whole time. */
#define T0H  4   /* 0.4 us */
#define T0L  8   /* 0.8 us */
#define T1H  8   /* 0.8 us */
#define T1L  5   /* 0.5 us */

static rmt_channel_handle_t  s_chan;
#ifdef LOADER_LED_GPIO2
static rmt_channel_handle_t  s_chan2;
#endif
static rmt_encoder_handle_t  s_enc;
static bool                  s_ready;
static llloader_led_state_t  s_state;
static int64_t               s_next_tick_us;
static bool                  s_pulse_dim;

//! GRB, which is the WS2812's wire order and NOT RGB -- a swap here shows as the wrong colour for
//! every state at once, so it is worth stating rather than leaving to the reader to spot.
static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) return;
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    //! REPORT A FAILED TRANSMIT.  Discarding it made a silently-unchanged lamp indistinguishable
    //! from a lamp the policy chose not to change -- and a WS2812 holds its last colour, so neither
    //! shows anything.  One line, only on the error path, so a working lamp stays quiet.
    esp_err_t e = rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    if (e != ESP_OK) { esp_rom_printf("led: rmt_transmit failed (0x%x)\n", (int) e); }
    (void) rmt_tx_wait_all_done(s_chan, 50);
#ifdef LOADER_LED_GPIO2
    /*! A SECOND CANDIDATE PIN, DRIVEN IN PARALLEL.  The DevKitC-1 moved its onboard WS2812 between
        revisions -- 48 on v1.0, 38 on later ones -- and a wrong pin gives a DARK LED, which reads as
        "the status code is broken" rather than "the pin is wrong".  Driving both costs one extra RMT
        channel (the S3 has four) and removes a whole class of misdiagnosis: if either pin is right,
        the operator sees a light.  Set LOADER_LED_GPIO2 only while identifying a board. */
    if (s_chan2 != NULL) {
        (void) rmt_transmit(s_chan2, s_enc, grb, sizeof(grb), &tx);
        (void) rmt_tx_wait_all_done(s_chan2, 50);
    }
#endif
}

void llloader_led_init(void)
{
    if (s_ready) return;
    rmt_tx_channel_config_t cc = {
        .gpio_num          = LOADER_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = LED_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    /*! SAY WHEN THE LED HARDWARE DOES NOT COME UP.  The first draft returned silently on every
        failure, which left a DARK LED with two indistinguishable causes -- wrong pin, or RMT refused
        -- and the operator with no way to tell them apart.  That is the absence-as-success shape in
        the one place it is most expensive: a diagnostic that cannot report its own absence.
        esp_rom_printf, not ESP_LOG: this runs before anything else and may run while a rung owns
        UART0 ([B755]). */
    esp_err_t e = rmt_new_tx_channel(&cc, &s_chan);
    if (e != ESP_OK) {
        esp_rom_printf("led: rmt_new_tx_channel(GPIO%d) failed (0x%x) -- NO status LED\n",
                       (int) LOADER_LED_GPIO, (int) e);
        return;
    }
    rmt_bytes_encoder_config_t ec = {
        .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
        .flags = { .msb_first = 1 },     /* WS2812 is MSB-first per byte */
    };
    if (rmt_new_bytes_encoder(&ec, &s_enc) != ESP_OK) {
        esp_rom_printf("led: rmt_new_bytes_encoder failed -- NO status LED\n");
        return;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        esp_rom_printf("led: rmt_enable failed -- NO status LED\n");
        return;
    }
    s_ready = true;
#ifdef LOADER_LED_GPIO2
    rmt_tx_channel_config_t cc2 = cc;
    cc2.gpio_num = LOADER_LED_GPIO2;
    if (rmt_new_tx_channel(&cc2, &s_chan2) == ESP_OK) { (void) rmt_enable(s_chan2); }
    else { s_chan2 = NULL; }
#endif
    led_write(0, 0, 0);
#ifdef LOADER_LED_GPIO2
    esp_rom_printf("led: ready on GPIO%d and GPIO%d (identify: 3 white blinks)\n",
                   (int) LOADER_LED_GPIO, (int) LOADER_LED_GPIO2);
#else
    esp_rom_printf("led: ready on GPIO%d (identify: 3 white blinks)\n", (int) LOADER_LED_GPIO);
#endif


    /*! AN IDENTIFY FLASH AT BOOT -- three quick white blinks, ~600 ms, blocking.
        WHY IT IS WORTH BLOCKING FOR: the first question at a bench with six ESP32s on it is "which
        one is that", and no colour answers it if the operator did not happen to be looking when the
        state last changed.  A recognisable pattern at every boot does, and it also distinguishes
        "the status LED works" from "that red light is the power LED", which is a real ambiguity on
        this devkit and cost exactly one round trip to discover.
        600 ms before any decision is made is affordable in a recovery root that is about to spend
        300 s waiting for a console push. */
    for (int i = 0; i < 3; i++) {
        led_write(40, 40, 40);
        esp_rom_delay_us(120000);
        led_write(0, 0, 0);
        esp_rom_delay_us(80000);
    }
}

/*! DIM ON PURPOSE.  A WS2812 at full scale is painful to look at on a bench and washes out in a
    photo, which is how these boards get documented.  32/255 is clearly visible across a room. */
static void colour_for(llloader_led_state_t st, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (st) {
        case LLLOADER_LED_BOOT:         *r = 12; *g = 12; *b = 12; break;  /* dim white  */
        case LLLOADER_LED_NORMAL:       *r =  0; *g = 32; *b =  0; break;  /* green      */
        case LLLOADER_LED_INSTALL:      *r =  0; *g =  0; *b = 40; break;  /* blue       */
        case LLLOADER_LED_RECOVER:      *r = 40; *g = 20; *b =  0; break;  /* amber      */
        case LLLOADER_LED_CONSOLE_WAIT: *r =  0; *g = 28; *b = 28; break;  /* cyan       */
        case LLLOADER_LED_LATCHED:      *r = 48; *g =  0; *b =  0; break;  /* red        */
        case LLLOADER_LED_OFF:
        default:                        *r =  0; *g =  0; *b =  0; break;
    }
}


void llloader_led_set(llloader_led_state_t st)
{
    /* A new boot or a good handoff resets; otherwise a lower-ranked transient does not steal the
       lamp from a standing condition someone needs to see. */
    if (!llloader_led_takes(s_state, st)) { return; }
    s_state = st;
    s_pulse_dim = false;
    s_next_tick_us = 0;
    uint8_t r, g, b;
    colour_for(st, &r, &g, &b);
    led_write(r, g, b);
}

void llloader_led_tick(void)
{
    /* SOLID means "this is what is happening"; BLINKING means "this needs a person".  Two states
       qualify and both now blink -- CONSOLE_WAIT (waiting for a push) and LATCHED (out of install
       attempts).  The first draft pulsed only CONSOLE_WAIT and left LATCHED solid, which is the
       state an operator is most likely to walk up to and the one least distinguishable from the
       board's own power LED.  Different rates so they are told apart without a reference card:
       cyan fast-ish, red slow and emphatic. */
    if (!s_ready) return;
    if (s_state != LLLOADER_LED_CONSOLE_WAIT && s_state != LLLOADER_LED_LATCHED) return;
    int64_t now = esp_timer_get_time();
    if (now < s_next_tick_us) return;            /* rate-limited: safe to call per byte */
    s_next_tick_us = now + (s_state == LLLOADER_LED_LATCHED ? 900000 : 400000);
    s_pulse_dim = !s_pulse_dim;
    if (s_pulse_dim) { led_write(0, 0, 0); }   /* fully off, so a blink reads as a blink */
    else {
        uint8_t r, g, b;
        colour_for(s_state, &r, &g, &b);
        led_write(r, g, b);
    }
}

#endif // LOADER_LED_GPIO
