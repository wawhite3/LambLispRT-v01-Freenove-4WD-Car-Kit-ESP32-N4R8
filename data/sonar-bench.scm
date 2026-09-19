;;; sonar-bench.scm -- P126 R6 arms A2 and A3: the SAME sonar, blocking vs interrupt-driven.
;;; Copyright 2026 by Frobenius Norm LLC 2026-09-17 00:00:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; A2 -- BLOCKING (Sonar.ping).  The apples-to-apples control against MicroPython's
;;;       machine.time_pulse_us: same algorithm, different interpreter.
;;; A3 -- INTERRUPT (Sonar.start / Sonar.result).  A FALLING-edge IRAM_ATTR ISR timestamps the
;;;       echo, so the call returns immediately and the runtime stays available.
;;;
;;; Both arms live in ONE file deliberately: they must sample the same target, in the same
;;; session, with the same constants, or the comparison is between two setups rather than two
;;; architectures.
;;;
;;; THE METRIC THAT MATTERS IS PAUSE, not throughput.  With a target in range every arm is bounded
;;; by flight time and the blocking arm looks respectable; what separates them is how much of that
;;; time the runtime is UNAVAILABLE.  That is the sonar analogue of the published GC-pause figure.
;;;
;;; Output is one RESULT line per arm, machine-parseable, so a harness can join it to a result cell.

(define (sb-sqrt x) (sqrt x))

;;; Sample statistics over a list of real numbers -> (n mean sd min max)
(define (sb-stats xs)
  (let ((n (length xs)))
    (if (= n 0)
        (list 0 0 0 0 0)
        (let loop ((l xs) (sum 0) (lo 1e9) (hi -1e9))
          (if (pair? l)
              (loop (cdr l) (+ sum (car l))
                    (if (< (car l) lo) (car l) lo)
                    (if (> (car l) hi) (car l) hi))
              (let ((mean (/ sum n)))
                (let loop2 ((l xs) (acc 0))
                  (if (pair? l)
                      (loop2 (cdr l) (+ acc (let ((d (- (car l) mean))) (* d d))))
                      (list n mean (sb-sqrt (/ acc (if (> n 1) (- n 1) 1))) lo hi)))))))))

;;; --- A2: blocking -------------------------------------------------------------------
;;; Every microsecond between the two `micros` reads is time the VM cannot do anything else,
;;; so for this arm CALL TIME *IS* PAUSE.  They are one number by construction, not by measurement.
(define (sb-blocking n)
  (let loop ((i 0) (ds '()) (pauses '()) (t-start (micros)))
    (if (>= i n)
        (list ds pauses (- (micros) t-start))
        (let* ((t0 (micros))
               (us (Sonar.ping))
               (t1 (micros))
               (d  (if us (Sonar.us->distance us) Sonar.range-m)))
          (loop (+ i 1) (cons d ds) (cons (- t1 t0) pauses) t-start)))))

;;; --- A3: interrupt-driven -----------------------------------------------------------
;;; PAUSE here is ONLY the time inside Sonar.start / Sonar.result.  The polling gap between them
;;; is time the VM is free -- in the real system it is running the behaviour queue and the B5
;;; reflex -- so it is deliberately NOT counted as pause.  Counting it would be measuring this
;;; benchmark's idle loop instead of the driver.
(define (sb-isr n)
  (let loop ((i 0) (ds '()) (pauses '()) (busy 0) (t-start (micros)))
    (if (>= i n)
        (list ds pauses (- (micros) t-start) busy)
        (let poll ((b 0))
          (let* ((t0 (micros))
                 (r  (Sonar.result))
                 (t1 (micros))
                 (b2 (+ b (- t1 t0))))
            (cond
              ((not r) (poll b2))                       ;;; still in flight -- VM is free here
              (else
               (let* ((d  (if (zero? r) Sonar.range-m (Sonar.us->distance r)))
                      (s0 (micros))
                      (_  (Sonar.start))                ;;; re-arm: part of the pause
                      (s1 (micros))
                      (p  (+ b2 (- s1 s0))))
                 (loop (+ i 1) (cons d ds) (cons p pauses) (+ busy p) t-start)))))))))

;;; --- A3g: interrupt-driven WITH the settle gap the blocking path already honours ------
;;; THE CONTROL THAT IDENTIFIES THE DEFECT.  ll_xmop3_Sonar.cpp:172 makes ping() wait
;;; min_interval_us before firing, with the comment "honor the HC-SR04 inter-ping settle gap so
;;; consecutive blocking reads don't alternate echo/timeout (transducer ring-down)".  start() does
;;; NOT wait -- it only RECORDS last_trigger_us.  So the ISR path re-fires the instant a result is
;;; dispositioned, which at close range is every ~2 ms against a sensor that wants 60 ms.
;;; If that is the cause, gating the re-arm here should collapse the spread to A2's.  If it does
;;; not, the diagnosis is wrong and the spread is something else.
(define (sb-wait-us gap)
  (let ((t0 (micros)))
    (let spin () (if (< (- (micros) t0) gap) (spin)))))

(define (sb-isr-gated n gap)
  (let loop ((i 0) (ds '()) (pauses '()) (t-start (micros)))
    (if (>= i n)
        (list ds pauses (- (micros) t-start))
        (let poll ((b 0))
          (let* ((t0 (micros))
                 (r  (Sonar.result))
                 (t1 (micros))
                 (b2 (+ b (- t1 t0))))
            (cond
              ((not r) (poll b2))
              (else
               (let* ((d  (if (zero? r) Sonar.range-m (Sonar.us->distance r))))
                 (sb-wait-us gap)                     ;;; the settle gap start() omits
                 (let* ((s0 (micros))
                        (_  (Sonar.start))
                        (s1 (micros))
                        (p  (+ b2 (- s1 s0))))
                   (loop (+ i 1) (cons d ds) (cons p pauses) t-start))))))))))

(define (sb-report arm ds pauses total-us)
  (let ((ds-st (sb-stats ds))
        (pa-st (sb-stats pauses)))
    (display "RESULT arm=") (display arm)
    (display " n=")          (display (car ds-st))
    (display " mean_m=")     (display (exact->inexact (cadr ds-st)))
    (display " sd_m=")       (display (caddr ds-st))
    (display " min_m=")      (display (cadddr ds-st))
    (display " max_m=")      (display (car (cddddr ds-st)))
    (display " pause_mean_us=") (display (exact->inexact (cadr pa-st)))
    (display " pause_max_us=")  (display (car (cddddr pa-st)))
    (display " total_us=")   (display total-us)
    (display " per_sec=")    (display (if (> total-us 0) (/ (* 1.0 (car ds-st) 1000000) total-us) 0))
    (newline)))

(define (sonar-bench n)
  (if (not have-Sonar)
      (begin (display "RESULT arm=NONE error=no-sonar-on-this-board") (newline))
      (begin
        (display "SONARBENCH start n=") (display n) (newline)
        (let ((a2 (sb-blocking n)))
          (sb-report "A2-blocking" (car a2) (cadr a2) (caddr a2)))
        (Sonar.start)                                   ;;; re-arm before the ISR arm
        (let ((a3 (sb-isr n)))
          (sb-report "A3-isr" (car a3) (cadr a3) (caddr a3)))
        (Sonar.start)
        (let ((a3g (sb-isr-gated n 60000)))           ;;; 60 ms = the C++ min_interval_us default
          (sb-report "A3g-isr-gated" (car a3g) (cadr a3g) (caddr a3g)))
        (display "SONARBENCH done") (newline))))
