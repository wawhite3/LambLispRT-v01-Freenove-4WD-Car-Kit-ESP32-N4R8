;;; Copyright 2026 by Frobenius Norm LLC 2026-10-22
;;;
;;; panel-bitskew.scm -- test patterns for the AITRIP 480x480 panel's red-MSB skew [B662].
;;;
;;; WHY THESE PATTERNS AND NOT A PHOTOGRAPH.  The instrument is a human eye, so everything else has
;;; to be pinned: one variable per pattern, flat fields rather than content, and the suspect bit
;;; isolated from its four siblings.  A "does this picture look good" test cannot distinguish the
;;; three candidate causes (bit skew, quantisation stepping, panel gamma) and has repeatedly been
;;; read as whichever one the reader already suspected -- [B667] was [B662] misread exactly that way.
;;;
;;; THE MECHANISM UNDER TEST.  `ait_data_gpios` (ll_xmop3_Panel.cpp) maps R4 -- red's MOST
;;; SIGNIFICANT bit -- to GPIO0, the ESP32-S3 BOOT strapping pin, which carries its boot-button
;;; network.  That line settles later than R0..R3, so a fringe appears wherever R4 TOGGLES between
;;; neighbouring pixels.  R4 flips between red 15 and red 16.  Every pattern here is built around
;;; that one boundary.
;;;
;;; RGB565 on this panel: (r << 11) | (g << 5) | b, red in the TOP five bits.  The panel is BGR and
;;; RGB666 fed 16 lines, so blue's and red's LSBs are dropped IN THE COPPER -- see the pin-map note.
(define (pb-565 r g b) (+ (* r 2048) (* g 32) b))
(define (pb-red r)     (pb-565 r 0 0))
(define (pb-green g)   (pb-565 0 g 0))

;;; P1 -- SEAM.  Two flat red fields differing in R4 ALONE: 15 is 01111, 16 is 10000.  No gradient,
;;; no other channel.  A fringe here is the skew with nothing else in the frame to explain it.
;;; The seam is at x=240 deliberately: that is where [B667] saw its "thin magenta line", and for a
;;; 0..31 ramp across 480px the MSB flips at the midpoint -- so this puts the same transition at the
;;; same place with no ramp involved.
(define (pb-seam)
  (lcd-clear 0)
  (lcd-box   0 0 240 480 (pb-red 15))
  (lcd-box 240 0 240 480 (pb-red 16))
  (display "PB-SEAM r15|r16 @x240") (newline))

;;; P2 -- HALF RAMPS.  Red 0..15 only, then 16..31 only: the same visual weight as a full ramp but
;;; R4 never toggles.  If P3's line vanishes here, crossing is the cause rather than the level.
(define (pb-ramp-lo)
  (lcd-clear 0)
  (let loop ((i 0)) (when (< i 16) (lcd-box (* i 30) 0 30 480 (pb-red i)) (loop (+ i 1))))
  (display "PB-RAMP-LO r0..15") (newline))
(define (pb-ramp-hi)
  (lcd-clear 0)
  (let loop ((i 0)) (when (< i 16) (lcd-box (* i 30) 0 30 480 (pb-red (+ i 16))) (loop (+ i 1))))
  (display "PB-RAMP-HI r16..31") (newline))

;;; P3 -- FULL RAMP.  0..31 across the width; the MSB flips at x=240.  This is the [B667] pattern.
(define (pb-ramp-full)
  (lcd-clear 0)
  (let loop ((i 0)) (when (< i 32) (lcd-box (* i 15) 0 15 480 (pb-red i)) (loop (+ i 1))))
  (display "PB-RAMP-FULL r0..31, MSB flips @x240") (newline))

;;; P4 -- GREEN CONTROL.  The same seam in green (31|32 of 6 bits), whose pins are unloaded.  A
;;; fringe HERE would refute the GPIO0 mechanism, so this is the arm that can disagree -- without it
;;; a fringe on P1 only proves "something is wrong at a flat boundary".
(define (pb-seam-green)
  (lcd-clear 0)
  (lcd-box   0 0 240 480 (pb-green 31))
  (lcd-box 240 0 240 480 (pb-green 32))
  (display "PB-SEAM-GREEN g31|g32 @x240 -- CONTROL, expect NO fringe") (newline))

;;; P5 -- STROKE vs BLOCK.  1px red lines on black, then the same red as wide bars.  The fringe is
;;; ~1px, so it destroys a stroke and vanishes under a block: this is what decides whether a hot
;;; colour is usable as text or only as fill.
(define (pb-strokes)
  (lcd-clear 0)
  (let loop ((x 20)) (when (< x 240) (lcd-box x 0 1 480 (pb-red 29)) (loop (+ x 20))))
  (let loop ((x 260)) (when (< x 470) (lcd-box x 0 12 480 (pb-red 29)) (loop (+ x 40))))
  (display "PB-STROKES left=1px r29, right=12px r29") (newline))

(display "panel-bitskew loaded: (pb-seam) (pb-ramp-lo) (pb-ramp-hi) (pb-ramp-full) (pb-seam-green) (pb-strokes)")
(newline)
