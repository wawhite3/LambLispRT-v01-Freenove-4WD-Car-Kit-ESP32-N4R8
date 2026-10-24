;;; sd-recorder.scm -- a rolling flight recorder on the SD card, bounded in both size and cost.
;;; Copyright 2026 by Frobenius Norm LLC 2026-10-08 12:30:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; A FIXED RING OF FILES, NOT A GROWING LOG.  The recorder writes to `base.0`, `base.1`, ...
;;; `base.<slots-1>` in turn: when the current slot reaches its byte limit the next slot is
;;; TRUNCATED and becomes current.  So total card usage is bounded at `slots * max-bytes` and the
;;; oldest records are the ones overwritten.  A ring is used rather than renaming files because the
;;; card API has no rename -- and a design that needs no rename cannot be half-way through one when
;;; power is lost.
;;;
;;; WITHOUT A CARD IT IS A NO-OP, DELIBERATELY.  `sd-recorder-open!` returns #f and every later
;;; `sd-recorder-log!` returns #f and writes nothing.  A recorder that raised on a missing card would
;;; take down the program it is supposed to be observing, which is the opposite of a flight recorder.
;;; That is also why it is safe to load on a board with no SD socket at all: the sd-* procedures are
;;; simply absent and the guard below sees that.
;;;
;;; IT COSTS WHAT IT COSTS AND SAYS SO.  `sd-recorder-stats` reports writes, rotations and the
;;; microseconds spent inside the recorder, so the question "did logging perturb what I measured"
;;; has an answer from the same run rather than an opinion.  If the logger perturbs the thing it
;;; measures, it has failed -- so the cost is reported, not hidden.

(define sd-rec-base   #f)      ;;;!< filename stem, or #f when the recorder is disabled
(define sd-rec-max    0)       ;;;!< bytes per slot
(define sd-rec-slots  0)       ;;;!< how many slots in the ring
(define sd-rec-slot   0)       ;;;!< which slot is current
(define sd-rec-bytes  0)       ;;;!< bytes written into the current slot
(define sd-rec-writes 0)       ;;;!< records accepted
(define sd-rec-rots   0)       ;;;!< rotations performed
(define sd-rec-us     0)       ;;;!< total microseconds spent inside the recorder

;;; #t only when the card API is present AND a card is mounted.  Both halves are needed: the
;;; procedures are absent on a board with no socket, and present-but-unmounted on a board whose card
;;; was never inserted.
(define (sd-rec-available?)
  (and (defined? 'sd-append)
       (defined? 'sd-write)
       (defined? 'sd-info)
       (let ((info (sd-info)))
         (and (pair? info) (car info)))))

(define (sd-rec-name i)
  (string-append sd-rec-base "." (number->string i)))

;;; (sd-recorder-open! BASE MAX-BYTES SLOTS) -> #t enabled, #f disabled.
;;; Truncates slot 0 so a run starts on a known-empty file rather than appending to whatever the
;;; previous run left -- a recorder whose first record lands after unknown bytes cannot be read back
;;; positionally.
(define (sd-recorder-open! base max-bytes slots)
  (set! sd-rec-writes 0)
  (set! sd-rec-rots 0)
  (set! sd-rec-us 0)
  (set! sd-rec-slot 0)
  (set! sd-rec-bytes 0)
  (if (not (sd-rec-available?))
      (begin (set! sd-rec-base #f) #f)
      (begin
        (set! sd-rec-base base)
        (set! sd-rec-max (if (> max-bytes 0) max-bytes 4096))
        (set! sd-rec-slots (if (> slots 0) slots 2))
        (sd-write (sd-rec-name 0) "")
        #t)))

(define (sd-recorder-close!)
  (set! sd-rec-base #f)
  #t)

;;; Advance to the next slot and truncate it.  Separate from the logging path so the rotation cost
;;; is attributable: it is one write, and it happens once per max-bytes rather than per record.
(define (sd-rec-rotate!)
  (set! sd-rec-slot (modulo (+ sd-rec-slot 1) sd-rec-slots))
  (set! sd-rec-bytes 0)
  (set! sd-rec-rots (+ sd-rec-rots 1))
  (sd-write (sd-rec-name sd-rec-slot) ""))

;;; (sd-recorder-log! STRING) -> #t written, #f disabled or refused.
;;; A newline is appended so the slot is line-oriented and readable with sd-read.
(define (sd-recorder-log! s)
  (if (not sd-rec-base)
      #f
      (let ((t0 (micros))
            (rec (string-append s (string #\newline))))
        (if (> (+ sd-rec-bytes (string-length rec)) sd-rec-max)
            (sd-rec-rotate!))
        (let ((ok (sd-append (sd-rec-name sd-rec-slot) rec)))
          (if ok
              (begin (set! sd-rec-bytes (+ sd-rec-bytes (string-length rec)))
                     (set! sd-rec-writes (+ sd-rec-writes 1))))
          (set! sd-rec-us (+ sd-rec-us (- (micros) t0)))
          ok))))

;;; (sd-recorder-stats) -> (enabled? slot bytes writes rotations total-us us-per-write)
;;; THE LAST FIELD IS THE POINT.  A recorder is only usable if its own cost is known, so the figure
;;; that answers "can this sit in the loop I am measuring" is reported beside the counts.
(define (sd-recorder-stats)
  (list (if sd-rec-base #t #f)
        sd-rec-slot
        sd-rec-bytes
        sd-rec-writes
        sd-rec-rots
        sd-rec-us
        (if (> sd-rec-writes 0) (quotient sd-rec-us sd-rec-writes) 0)))

;;; (sd-recorder-read SLOT) -> string, or #f.  Convenience over sd-read for reading a slot back.
(define (sd-recorder-read i)
  (if (and sd-rec-base (defined? 'sd-read))
      (sd-read (sd-rec-name i))
      #f))
