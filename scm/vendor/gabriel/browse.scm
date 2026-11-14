;;; ===========================================================================================
;;; THIRD-PARTY FILE -- NOT FROBENIUS NORM LLC WORK.  See NOTICE.md in this directory.
;;; ===========================================================================================
;;; BROWSE, from the Gabriel benchmark suite.  Author credit is in the original header below and
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
;;;      and calls `run-r7rs-benchmark` -- is removed, and `browse-bench` added at the END of the file.
;;;      IT IS REMOVED AS A BALANCED FORM, NOT BY TRUNCATING THE FILE: in this suite the driver sits
;;;      in the MIDDLE, with required definitions after it (none, but removed as a form for consistency), so cutting the
;;;      tail would silently drop setup and the benchmark would fail for a reason unrelated to it.
;;; EVERY ALGORITHM AND DATA TABLE IS VERBATIM.  That is the whole reason this is vendored rather
;;; than reimplemented: a benchmark whose tables differ from the canonical ones is comparable to
;;; nobody's published figure, which would defeat its only purpose here.
;;; ===========================================================================================

;;; BROWSE -- Benchmark to create and browse through
;;; an AI-like data base of units.

;;; [FN] import commented out, not deleted: this runtime has no (import) form.
;;; (import (scheme base) (scheme read) (scheme write) (scheme time))

(define (lookup key table)
  (let loop ((x table))
    (if (null? x)
        #f
        (let ((pair (car x)))
          (if (eq? (car pair) key)
              pair
              (loop (cdr x)))))))

(define *properties* '())

(define (get key1 key2)
  (let ((x (lookup key1 *properties*)))
    (if x
        (let ((y (lookup key2 (cdr x))))
          (if y
              (cdr y)
              #f))
        #f)))

(define (put key1 key2 val)
  (let ((x (lookup key1 *properties*)))
    (if x
        (let ((y (lookup key2 (cdr x))))
          (if y
              (set-cdr! y val)
              (set-cdr! x (cons (cons key2 val) (cdr x)))))
        (set! *properties*
              (cons (list key1 (cons key2 val)) *properties*)))))

(define *current-gensym* 0)

(define (generate-symbol)
  (set! *current-gensym* (+ *current-gensym* 1))
  (string->symbol (number->string *current-gensym*)))

(define (append-to-tail! x y)
  (if (null? x)
      y
      (do ((a x b)
           (b (cdr x) (cdr b)))
          ((null? b)
           (set-cdr! a y)
           x))))

(define (tree-copy x)
  (if (not (pair? x))
      x
      (cons (tree-copy (car x))
            (tree-copy (cdr x)))))

;;; n is # of symbols
;;; m is maximum amount of stuff on the plist
;;; npats is the number of basic patterns on the unit
;;; ipats is the instantiated copies of the patterns

(define *rand* 21)

(define (init n m npats ipats)
  (let ((ipats (tree-copy ipats)))
    (do ((p ipats (cdr p)))
        ((null? (cdr p)) (set-cdr! p ipats)))
    (do ((n n (- n 1))
         (i m (cond ((zero? i) m)
                    (else (- i 1))))
         (name (generate-symbol) (generate-symbol))
         (a '()))
        ((= n 0) a)
      (set! a (cons name a))
      (do ((i i (- i 1)))
          ((zero? i))
        (put name (generate-symbol) #f))
      (put name
           'pattern
           (do ((i npats (- i 1))
                (ipats ipats (cdr ipats))
                (a '()))
               ((zero? i) a)
             (set! a (cons (car ipats) a))))
      (do ((j (- m i) (- j 1)))
          ((zero? j))
        (put name (generate-symbol) #f)))))

(define (browse-random)
  (set! *rand* (remainder (* *rand* 17) 251))
  *rand*)

(define (randomize l)
  (do ((a '()))
      ((null? l) a)
    (let ((n (remainder (browse-random) (length l))))
      (cond ((zero? n)
             (set! a (cons (car l) a))
             (set! l (cdr l))
             l)
            (else
             (do ((n n (- n 1))
                  (x l (cdr x)))
                 ((= n 1)
                  (set! a (cons (cadr x) a))
                  (set-cdr! x (cddr x))
                  x)))))))

(define (my-match pat dat alist)
  (cond ((null? pat)
         (null? dat))
        ((null? dat) '())
        ((or (eq? (car pat) '?)
             (eq? (car pat)
                  (car dat)))
         (my-match (cdr pat) (cdr dat) alist))
        ((eq? (car pat) '*)
         (or (my-match (cdr pat) dat alist)
             (my-match (cdr pat) (cdr dat) alist)
             (my-match pat (cdr dat) alist)))
        (else (cond ((not (pair? (car pat)))
                     (cond ((eq? (string-ref (symbol->string (car pat)) 0)
                                 #\?)
                            (let ((val (assq (car pat) alist)))
                              (cond (val (my-match (cons (cdr val)
                                                         (cdr pat))
                                                   dat alist))
                                    (else (my-match (cdr pat)
                                                    (cdr dat)
                                                    (cons (cons (car pat)
                                                                (car dat))
                                                          alist))))))
                           ((eq? (string-ref (symbol->string (car pat)) 0)
                                 #\*)
                            (let ((val (assq (car pat) alist)))
                              (cond (val (my-match (append (cdr val)
                                                           (cdr pat))
                                                   dat alist))
                                    (else
                                     (do ((l '()
                                             (append-to-tail!
                                              l
                                              (cons (if (null? d)
                                                        '()
                                                        (car d))
                                                    '())))
                                          (e (cons '() dat) (cdr e))
                                          (d dat (if (null? d) '() (cdr d))))
                                         ((or (null? e)
                                              (my-match (cdr pat)
                                                        d
                                                        (cons
                                                         (cons (car pat) l)
                                                         alist)))
                                          (if (null? e) #f #t)))))))

                           ;; fix suggested by Manuel Serrano
                           ;; (cond did not have an else clause);
                           ;; this changes the run time quite a bit

                           (else #f)))
                    (else (and
                           (pair? (car dat))
                           (my-match (car pat)
                                     (car dat) alist)
                           (my-match (cdr pat)
                                     (cdr dat) alist)))))))

(define database
  (randomize
   (init 100 10 4 '((a a a b b b b a a a a a b b a a a)
                    (a a b b b b a a
                       (a a)(b b))
                    (a a a b (b a) b a b a)))))

(define (browse pats)
  (investigate
   database
   pats)
  (map string->number (map symbol->string database)))

(define (investigate units pats)
  (do ((units units (cdr units)))
      ((null? units))
    (do ((pats pats (cdr pats)))
        ((null? pats))
      (do ((p (get (car units) 'pattern)
              (cdr p)))
          ((null? p))
        (my-match (car pats) (car p) '())))))



;;; [FN] Entry point replacing the suite's stdin-driven `run-benchmark`.
;;; CANONICAL PARAMETERS, from `inputs/browse.input`.  `browse` takes a LIST OF PATTERNS, not a
;;; count -- passing it an integer makes it `car` a number, which is a defect in the CALLER and not
;;; in the runtime.  Recorded because that is exactly how it was first got wrong here.
(define browse-patterns
  '((*a ?b *b ?b a *a a *b *a)
    (*a *b *b *a (*a) (*b))
    (? ? * (b a) * ? ?)))
(define (browse-bench) (browse browse-patterns))
