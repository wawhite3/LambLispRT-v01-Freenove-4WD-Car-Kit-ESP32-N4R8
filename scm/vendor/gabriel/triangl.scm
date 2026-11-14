;;; ===========================================================================================
;;; THIRD-PARTY FILE -- NOT FROBENIUS NORM LLC WORK.  See NOTICE.md in this directory.
;;; ===========================================================================================
;;; TRIANGL, from the Gabriel benchmark suite.  Author credit is in the original header below and
;;; is left exactly as found.  Retrieved 2026-10-08 from github.com/ecraven/r7rs-benchmarks, which
;;; carries NO LICENSE FILE; no grant of rights accompanies this copy and Frobenius Norm LLC claims
;;; no ownership of it and licenses it to no one.  NO WARRANTY.
;;;
;;; *** DO NOT ADD THE "Free for non-commercial use" LINE TO THIS FILE. ***  That states OUR terms
;;; for OUR code; putting it here would misstate someone else's -- and `scm/` is copied into every
;;; customer package, so it would be published to every per-target repository.
;;;
;;; MODIFICATIONS BY FROBENIUS NORM LLC, each marked `[FN]` at the site:
;;;   1. the `(import ...)` form is commented out (this runtime has no `import`);
;;;   2. the suite's `run-benchmark` driver -- which reads its parameters off stdin with `(read)`
;;;      and calls `run-r7rs-benchmark` -- is removed, and `triangl-bench` added in its place.
;;; THE ALGORITHM AND ALL FOUR DATA VECTORS (*board* *a* *b* *c*) ARE VERBATIM.  They are the whole
;;; reason this file is vendored instead of reimplemented: a triangle benchmark whose move tables
;;; differ from the canonical ones is not comparable to anybody's published figure, which would
;;; defeat the only purpose it has here.
;;; ===========================================================================================

;;; TRIANGL -- Board game benchmark.

;;; [FN] import commented out, not deleted: this runtime has no (import) form.
;;; (import (scheme base) (scheme read) (scheme write) (scheme time))

(define *board*
  (list->vector '(1 1 1 1 1 0 1 1 1 1 1 1 1 1 1 1)))

(define *sequence*
  (list->vector '(0 0 0 0 0 0 0 0 0 0 0 0 0 0)))

(define *a*
  (list->vector '(1 2 4 3 5 6 1 3 6 2 5 4 11 12
                    13 7 8 4 4 7 11 8 12 13 6 10
                    15 9 14 13 13 14 15 9 10
                    6 6)))

(define *b*
  (list->vector '(2 4 7 5 8 9 3 6 10 5 9 8
                    12 13 14 8 9 5 2 4 7 5 8
                    9 3 6 10 5 9 8 12 13 14
                    8 9 5 5)))

(define *c*
  (list->vector '(4 7 11 8 12 13 6 10 15 9 14 13
                    13 14 15 9 10 6 1 2 4 3 5 6 1
                    3 6 2 5 4 11 12 13 7 8 4 4)))

(define *answer* '())

(define (attempt i depth)
  (cond ((= depth 14)
         (set! *answer*
               (cons (cdr (vector->list *sequence*)) *answer*))
         #t)
        ((and (= 1 (vector-ref *board* (vector-ref *a* i)))
              (= 1 (vector-ref *board* (vector-ref *b* i)))
              (= 0 (vector-ref *board* (vector-ref *c* i))))
         (vector-set! *board* (vector-ref *a* i) 0)
         (vector-set! *board* (vector-ref *b* i) 0)
         (vector-set! *board* (vector-ref *c* i) 1)
         (vector-set! *sequence* depth i)
         (do ((j 0 (+ j 1))
              (depth (+ depth 1)))
             ((or (= j 36) (attempt j depth)) #f))
         (vector-set! *board* (vector-ref *a* i) 1)
         (vector-set! *board* (vector-ref *b* i) 1)
         (vector-set! *board* (vector-ref *c* i) 0) #f)
        (else #f)))

(define (test i depth)
  (set! *answer* '())
  (attempt i depth)
  (car *answer*))


;;; [FN] Entry point replacing the suite's stdin-driven `run-benchmark`.
;;; CANONICAL PARAMETERS, from the suite's own `inputs/triangl.input`: input1=22, input2=1, and the
;;; EXPECTED RESULT is the 13-element list below.  Taking the expected value from the suite rather
;;; than from a first run is the whole point -- a benchmark that asserts whatever it happened to
;;; produce cannot detect a wrong answer, and this one currently produces a WRONG answer on this
;;; runtime ([B809]).
(define triangl-expected '(22 34 31 15 7 1 20 17 25 6 5 13 32))
(define (triangl-bench) (test 22 1))
