// Copyright 2026 by Frobenius Norm LLC 2026-10-06 09:15:00
#ifndef LLLOADER_LED_H
#define LLLOADER_LED_H

#include <stdbool.h>

/*! The onboard RGB LED as the loader's STATUS DISPLAY.  Owner request, 2026-10-06.

  WHY THE LOADER NEEDS THIS MORE THAN THE VM DOES.  When the loader is running, the unit has already
  failed in some way, and the only thing reporting is a UART that one process at a time can hold.
  An operator at the bench cannot tell "waiting for you to push an image" from "latched and giving
  up" from "installing, do not unplug" without attaching a terminal -- and attaching one RESETS the
  board on a UART bridge, destroying the state they were trying to look at.  A colour is readable
  across the room, by anyone, while someone else holds the port.

  IT ALSO ANSWERS "WHICH BOARD IS THAT", which is what prompted this: six ESP32s on one bench, and
  no way to point at the one a verb is talking to.

  OFF BY DEFAULT, AND THE DEFAULT BUILD IS UNCHANGED.  Everything here compiles to nothing unless
  `LOADER_LED_GPIO` is defined, so adding this cannot alter a unit nobody opted in for.  The trusted
  root gets a new dependency only where someone asked for it.

  NO NEW TASK, DELIBERATELY.  A render thread in the trusted root is a scheduler interaction that
  can go wrong in a context where little is safe -- and this project has already had one task
  crash the loader by writing to a UART another task owned.  So: a colour is written at a state
  TRANSITION, and `llloader_led_tick()` is called from loops that already poll.  Nothing new is
  scheduled and nothing blocks.

  ON esp32-s3-devkitc-1 THE PIN IS 38, MEASURED -- NOT the 48 the board pin map records.  Driven on
  48 the lamp never changes; driven on 38 it runs a full red/green/blue/white sweep.  `scm/boards/
  pins/esp32-s3-devkitc-1.scm` has been corrected, and the VM's `ledrgb!` path read the same wrong
  number.

  AND A WS2812 LATCHES ITS LAST COLOUR INDEFINITELY, WHICH IS WHY THE WRONG PIN IS HARD TO SEE.
  Once anything has written a colour, a DEAD pin and a LIVE one look identical -- the lamp simply
  holds what it was given.  Finding 38 took five rebuilds because a build driving 48+38 left the LED
  blue, and the next build driving only 48 was credited with that blue.  If you are ever unsure a pin
  is right, write SEVERAL colours in sequence and watch for the change; a single lit colour proves
  nothing at all.

  THE PIN IS NOT HARDCODED AND NOT GUESSED.  `scm/boards/pins/esp32-s3-devkitc-1.scm:53` records
  `(pin-RGBLED . 48)`, and `scm/features/sonar-flash.scm` states the rule: "Boards declaring
  `pin-RGBLED` (S3 at 48, C5 at 27)".  Pass it as `-DLOADER_LED_GPIO=48`.  A DevKitC-1 v1.0 uses
  GPIO48 and some later revisions use 38, so a wrong guess gives a DARK LED -- which reads as
  "the status code does not work" rather than "the pin is wrong", and that is the expensive
  misreading this note exists to prevent.
*/

typedef enum {
    LLLOADER_LED_OFF = 0,
    LLLOADER_LED_BOOT,          //!< up, deciding what to do.  dim white.
    LLLOADER_LED_NORMAL,        //!< about to hand off to a good app.  green.
    LLLOADER_LED_INSTALL,       //!< writing an image.  blue.  DO NOT UNPLUG.
    LLLOADER_LED_RECOVER,       //!< escalated to recovery.  amber.
    LLLOADER_LED_CONSOLE_WAIT,  //!< waiting for an operator to push one.  cyan, pulsed.
    LLLOADER_LED_LATCHED,       //!< out of attempts / declared dead.  red.
} llloader_led_state_t;

/*! Does `nw` take the lamp from `cur`?  The whole display policy, as one pure predicate.

    SEPARABLE FROM THE HARDWARE BECAUSE THAT IS WHERE THIS PROJECT'S LOADER BUGS LIVED.  Step C's
    three hardware defects were all in the half that could not be host-tested, so the rule about
    which state deserves the lamp is compiled and tested regardless of whether any LED exists.
    Driven directly by `w3_loader/test/llloader_led_test.c`. */
bool llloader_led_takes(llloader_led_state_t cur, llloader_led_state_t nw);

//! Idempotent; safe to call before anything else and safe to call twice.
void llloader_led_init(void);

//! Write the colour for `st` now.  Cheap enough to call on every state change.
void llloader_led_set(llloader_led_state_t st);

/*! Advance a pulsing state.  Call from a loop that already polls; it rate-limits itself, so calling
    it per byte is fine.  A no-op for states that are solid. */
void llloader_led_tick(void);

#endif // LLLOADER_LED_H
