;;; promote-named.scm -- promote NAMED procedures to pinned native code, and report what was refused.
;;; Copyright 2026 by Frobenius Norm LLC 2026-10-08 13:10:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; THE APPLICATION NAMES WHAT MATTERS; NOTHING INFERS IT.  An application knows which handler is on
;;; a control deadline, which path runs once per revolution and must not jitter, and which loop is
;;; merely hot and does not matter.  A call-frequency counter can see only the last of those.  So the
;;; set is a list of names supplied here, next to the procedures it names, and it can be computed --
;;; per mode, per attached hardware -- where a settings key could not.
;;;
;;; THE RETURN VALUE IS THE WHOLE INTERFACE.  `promote-named!` returns the names it could NOT pin.
;;; `compile-low-latency!` already returns #t only when the procedure's code actually landed in the
;;; low-latency pool, so the per-procedure fact exists and this just stops discarding it.  **Silently
;;; failing to promote a deadline-critical handler is the failure this exists to prevent**, and it is
;;; indistinguishable from success if the refusal only reaches a log.
;;;
;;; AN OVERSIZED SET IS NORMAL, NOT MISCONFIGURED.  The pool is small on the smallest parts, so a
;;; program that names more than fits is the ordinary case: the caller is expected to read the
;;; refusals and decide -- degrade a feature, raise, or continue knowingly.  Refusal is a return
;;; value, not an error.
;;;
;;; WHERE TO CALL IT: at the END of the application's own startup.  The procedures must exist and be
;;; bytecode-compiled, which rules out VM boot, and it must precede the first deadline, which rules
;;; out doing it lazily on first call.

;;; Promote one named procedure.  #t when its code is pinned in the pool on return, #f otherwise --
;;; including when the name is unbound, when it is not a procedure, and when there is no pool on this
;;; part at all.  Never raises: a promoter that throws on a stale name takes down the program it was
;;; meant to make predictable.
(define (promote-one! name)
  (and (defined? 'compile-low-latency!)
       (symbol? name)
       (let ((p (guard (e (#t #f)) (eval name (interaction-environment)))))
         (and (procedure? p)
              ;;; BYTECODE-COMPILE FIRST.  The native compiler takes a BYTECODE procedure; handed an
              ;;; interpreted one it compiles nothing, so `compile-low-latency!` would reach no
              ;;; allocation, never request the lazily-claimed exec arena, and truthfully report #f.
              ;;; That reads exactly like "the pool is full" and is instead "nothing was compiled".
              (eq? #t (guard (e (#t #f))
                        (compile-low-latency! (procedure->bytecode p))))))))

;;; (promote-named! '(a b c)) -> the sublist that was REFUSED, in the order given; '() when all
;;; landed.  Checking `(null? ...)` is therefore the "everything I asked for is pinned" test.
(define (promote-named! names)
  (let loop ((rest names) (refused '()))
    (if (null? rest)
        (reverse refused)
        (loop (cdr rest)
              (if (promote-one! (car rest))
                  refused
                  (cons (car rest) refused))))))

;;; (promote-report names) -> (asked pinned refused (names-refused ...))
;;; For a startup line that says what happened without the caller doing the arithmetic.
(define (promote-report names)
  (let ((bad (promote-named! names)))
    (list (length names)
          (- (length names) (length bad))
          (length bad)
          bad)))
