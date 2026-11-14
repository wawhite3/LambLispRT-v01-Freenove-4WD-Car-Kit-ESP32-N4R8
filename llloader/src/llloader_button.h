// Copyright 2026 by Frobenius Norm LLC 2026-10-06
// llloader_button.h -- [P253 step B] the press-and-hold exit's POLICY, with no GPIO in it.
//
// WHY THIS IS A SEPARATE FILE RATHER THAN A LOOP BODY.  Step B's three acceptance criteria are all
// statements about a DECISION -- a short press clears nothing, a hold clears the pending action and
// not the death count, one hold buys one boot -- and until this file existed the decision lived
// inside a FreeRTOS task that reads a pin, writes NVS and reboots.  Nothing about it could be
// checked without a board and a finger.  That is the same shape as step C's five hardware defects,
// every one of which was in the half that could not be host-tested (the status provider gathered
// once per received BYTE; the button task's ESP_LOG raced the console rung's UART driver; both were
// registered in do_console(), reached only after two 300 s waits).  The lesson that cost those five
// is: move the decision out of the half that needs a board.
//
// WHAT IS STILL NOT COVERED, SAID PLAINLY so a green suite does not read as more than it is: that
// GPIO0 is the right pin, that the pull-up works, that a human press is seen at all, and that the
// clear-and-restart actually clears NVS.  Those need the board, and two of them are closed on one
// (`BUTTON: held 3000 ms ...` then `pending_update unset -> NORMAL` with `count=3` surviving).
#ifndef LLLOADER_BUTTON_H
#define LLLOADER_BUTTON_H

#include <stdbool.h>

typedef enum {
    LLLOADER_BTN_NOTHING = 0,  //!< nothing to say this sample
    LLLOADER_BTN_ARM,          //!< first release observed -- the sampler is live from here
    LLLOADER_BTN_SHORT,        //!< released before the hold elapsed: REPORT IT, clear nothing
    LLLOADER_BTN_CLEAR,        //!< held long enough: clear the pending action and restart
} llloader_btn_action_t;

typedef struct {
    bool armed;        //!< has a release been observed since boot?
    int  held_ms;      //!< how long the current press has been held, in poll steps
    /*! HOW LONG THE PRESS BEING REPORTED LASTED, captured BEFORE `held_ms` is reset.
        It exists because the obvious caller is wrong: a release zeroes `held_ms`, so printing
        `held_ms` alongside a SHORT verdict reports "released after 0 ms" on every tap -- which is
        the same unreadable transcript the missing line produced, arriving by a different route. */
    int  reported_ms;
} llloader_btn_t;

//! Zeroed state: not armed, nothing held.  `armed` starts FALSE on purpose -- see the strap note.
void llloader_btn_init(llloader_btn_t *b);

/*! Feed ONE poll sample.  `level` is the pin: 0 = pressed (active-low to ground), non-zero =
    released.  `poll_ms` is how long since the last sample, `hold_ms` the threshold.

    THE ARMING RULE IS NOT A NICETY -- IT IS WHAT STOPS A STRAP FROM REBOOTING THE BOARD.  GPIO0 is
    also the boot strap: it is LOW while a host holds the board in the download bootloader, and an
    unconnected pin reads whatever its pull-up says.  So a press is only counted after a RELEASE has
    been observed at least once; a button (or a programmer) already holding the pin at startup is
    ignored forever rather than read as a 3-second hold the operator never performed.  Measured
    2026-10-05: `BUTTON: armed (GPIO0 observed released)` is ALSO what an unconnected pin prints, so
    arming proves the pin reads high -- it does not prove a button is attached.

    `LLLOADER_BTN_CLEAR` IS RETURNED ON EVERY SAMPLE PAST THE THRESHOLD, NOT LATCHED ONCE.  The real
    caller never returns from it -- it clears NVS and calls esp_restart() -- so a latch would be dead
    code that nothing could exercise.  A caller that does NOT restart must latch for itself; this is
    stated because the staleness watch next door made the opposite choice (one report per stall) and
    the two sit ten lines apart in the same loader. */
llloader_btn_action_t llloader_btn_step(llloader_btn_t *b, int level, int poll_ms, int hold_ms);

#endif // LLLOADER_BUTTON_H
