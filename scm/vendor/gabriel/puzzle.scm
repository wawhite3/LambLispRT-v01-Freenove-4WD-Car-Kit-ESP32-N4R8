;;; ===========================================================================================
;;; THIRD-PARTY FILE -- NOT FROBENIUS NORM LLC WORK.  See NOTICE.md in this directory.
;;; ===========================================================================================
;;; PUZZLE (Forest Baskett's), from the Gabriel benchmark suite.  Author credit is in the original header below and
;;; is left exactly as found.  Retrieved 2026-10-08 from github.com/ecraven/r7rs-benchmarks, which
;;; carries NO LICENSE FILE; no grant of rights accompanies this copy, and Frobenius Norm LLC claims
;;; no ownership of it and licenses it to no one.  NO WARRANTY.
;;;
;;; *** DO NOT ADD THE "Free for non-commercial use" LINE TO THIS FILE. ***  That states OUR terms
;;; for OUR code; putting it here would misstate someone else's -- and `scm/` is copied into every
;;; customer package, so it would be published to every per-target repository.
;;;
;;; MODIFICATIONS BY FROBENIUS NORM LLC, each marked `[FN]` at the site:
;;;   1. the `(import ...)` form is commented out (this runtime has no `import`);
;;;   2. the suite's `run-benchmark` driver -- which reads its parameters off stdin with `(read)`
;;;      and calls `run-r7rs-benchmark` -- is removed, and `puzzle-bench` added at the END of the file.
;;;      IT IS REMOVED AS A BALANCED FORM, NOT BY TRUNCATING THE FILE: in this suite the driver sits
;;;      in the MIDDLE, with required definitions after it (the `for-each` that fills *p* with vectors), so cutting the
;;;      tail would silently drop setup and the benchmark would fail for a reason unrelated to it.
;;; EVERY ALGORITHM AND DATA TABLE IS VERBATIM.  That is the whole reason this is vendored rather
;;; than reimplemented: a benchmark whose tables differ from the canonical ones is comparable to
;;; nobody's published figure, which would defeat its only purpose here.
;;; ===========================================================================================

;;; PUZZLE -- Forest Baskett's Puzzle benchmark, originally written in Pascal.

;;; [FN] import commented out, not deleted: this runtime has no (import) form.
;;; (import (scheme base) (scheme read) (scheme write) (scheme time))

(define (my-iota n)
  (do ((n n (- n 1))
       (list '() (cons (- n 1) list)))
      ((zero? n) list)))

(define *size* 511)
(define classmax 3)
(define typemax 12)

(define *iii* 0)
(define *kount* 0)
(define *d* 8)

(define *piececount* (make-vector (+ classmax 1) 0))
(define *class* (make-vector (+ typemax 1) 0))
(define *piecemax* (make-vector (+ typemax 1) 0))
(define *puzzle* (make-vector (+ *size* 1)))
(define *p* (make-vector (+ typemax 1)))

(define (fit i j)
  (let ((end (vector-ref *piecemax* i)))
    (do ((k 0 (+ k 1)))
        ((or (> k end)
             (and (vector-ref (vector-ref *p* i) k)
                  (vector-ref *puzzle* (+ j k))))
         (if (> k end) #t #f)))))

(define (place i j)
  (let ((end (vector-ref *piecemax* i)))
    (do ((k 0 (+ k 1)))
        ((> k end))
      (cond ((vector-ref (vector-ref *p* i) k)
             (vector-set! *puzzle* (+ j k) #t)
             #t)))
    (vector-set! *piececount*
                 (vector-ref *class* i)
                 (- (vector-ref *piececount* (vector-ref *class* i)) 1))
    (do ((k j (+ k 1)))
        ((or (> k *size*) (not (vector-ref *puzzle* k)))
         (if (> k *size*) 0 k)))))

(define (puzzle-remove i j)
  (let ((end (vector-ref *piecemax* i)))
    (do ((k 0 (+ k 1)))
        ((> k end))
      (cond ((vector-ref (vector-ref *p* i) k)
             (vector-set! *puzzle* (+ j k) #f)
             #f)))
    (vector-set! *piececount*
                 (vector-ref *class* i)
                 (+ (vector-ref *piececount* (vector-ref *class* i)) 1))))

(define (trial j)
  (let ((k 0))
    (call-with-current-continuation
     (lambda (return)
       (do ((i 0 (+ i 1)))
           ((> i typemax) (set! *kount* (+ *kount* 1)) #f)
         (cond
          ((not
            (zero?
             (vector-ref *piececount* (vector-ref *class* i))))
           (cond
            ((fit i j)
             (set! k (place i j))
             (cond
              ((or (trial k) (zero? k))
               (set! *kount* (+ *kount* 1))
               (return #t))
              (else (puzzle-remove i j))))))))))))

(define (definePiece iclass ii jj kk)
  (let ((index 0))
    (do ((i 0 (+ i 1)))
        ((> i ii))
      (do ((j 0 (+ j 1)))
          ((> j jj))
        (do ((k 0 (+ k 1)))
            ((> k kk))
          (set! index (+ i (* *d* (+ j (* *d* k)))))
          (vector-set! (vector-ref *p* *iii*) index  #t))))
    (vector-set! *class* *iii* iclass)
    (vector-set! *piecemax* *iii* index)
    (cond ((not (= *iii* typemax))
           (set! *iii* (+ *iii* 1))))))

(define (start size)
  (set! *kount* 0)
  (do ((m 0 (+ m 1)))
      ((> m size))
    (vector-set! *puzzle* m #t))
  (do ((i 1 (+ i 1)))
      ((> i 5))
    (do ((j 1 (+ j 1)))
        ((> j 5))
      (do ((k 1 (+ k 1)))
          ((> k 5))
        (vector-set! *puzzle* (+ i (* *d* (+ j (* *d* k)))) #f))))
  (do ((i 0 (+ i 1)))
      ((> i typemax))
    (do ((m 0 (+ m 1)))
        ((> m size))
      (vector-set! (vector-ref *p* i) m #f)))
  (set! *iii* 0)
  (definePiece 0 3 1 0)
  (definePiece 0 1 0 3)
  (definePiece 0 0 3 1)
  (definePiece 0 1 3 0)
  (definePiece 0 3 0 1)
  (definePiece 0 0 1 3)

  (definePiece 1 2 0 0)
  (definePiece 1 0 2 0)
  (definePiece 1 0 0 2)

  (definePiece 2 1 1 0)
  (definePiece 2 1 0 1)
  (definePiece 2 0 1 1)

  (definePiece 3 1 1 1)

  (vector-set! *piececount* 0 13)
  (vector-set! *piececount* 1 3)
  (vector-set! *piececount* 2 1)
  (vector-set! *piececount* 3 1)
  (let ((m (+ (* *d* (+ *d* 1)) 1))
        (n 0))
    (cond ((fit 0 m) (set! n (place 0 m)))
          (else (begin (newline) (display "Error."))))
    (if (trial n)
        *kount*
        #f)))



(for-each (lambda (i) (vector-set! *p* i (make-vector (+ *size* 1))))
          (my-iota (+ typemax 1)))

;;; [FN] Entry point replacing the suite's stdin-driven `run-benchmark`.
;;; CANONICAL PARAMETERS, from `inputs/puzzle.input`: input1=511, expected result 2005.
;;; NOTE the `for-each` above, which allocates every row of *p*, runs at LOAD time and is required;
;;; it sits after the driver in the original file, which is why the driver was removed as a balanced
;;; form rather than by truncating the tail.
(define puzzle-expected 2005)
(define (puzzle-bench) (start 511))
