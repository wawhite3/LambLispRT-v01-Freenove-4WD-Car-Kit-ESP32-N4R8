// Copyright 2026 by Frobenius Norm LLC 2026-10-06
// llloader_button.c -- see the header for why this is pure and what it deliberately cannot check.
#include "llloader_button.h"

void llloader_btn_init(llloader_btn_t *b)
{
    b->armed       = false;
    b->held_ms     = 0;
    b->reported_ms = 0;
}

llloader_btn_action_t llloader_btn_step(llloader_btn_t *b, int level, int poll_ms, int hold_ms)
{
    llloader_btn_action_t act = LLLOADER_BTN_NOTHING;

    if (level != 0) {                       //!< released
        /*! A SHORT PRESS IS REPORTED, AND `held_ms > 0` IS THE LOAD-BEARING HALF OF THAT TEST.
            Without it every idle poll would report a release of zero milliseconds -- a line every
            50 ms forever on a console an operator is trying to read, which is the failure the
            console rung's ESP_LOG noise already taught ([B755]).  The arm for this is
            "a release with nothing held says nothing". */
        if (b->armed && b->held_ms > 0 && b->held_ms < hold_ms) {
            act = LLLOADER_BTN_SHORT;
            b->reported_ms = b->held_ms;   //!< BEFORE the reset below; see the header
        }
        else if (!b->armed) { b->armed = true; act = LLLOADER_BTN_ARM; }
        b->held_ms = 0;
    }
    else if (b->armed) {                    //!< pressed, and a release has been seen at least once
        b->held_ms += poll_ms;
        if (b->held_ms >= hold_ms) { act = LLLOADER_BTN_CLEAR; b->reported_ms = b->held_ms; }
    }
    //! else: pressed but never released since boot -- the strap case.  Ignored, deliberately.

    return act;
}
