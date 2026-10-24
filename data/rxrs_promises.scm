;;; Copyright 2026 by Frobenius Norm LLC 2026-05-25
(define %promise-tag (list 'promise))
(define (%promise-make forced? val)  (cons %promise-tag (cons forced? val)))
(define (%promise-forced? p)         (car (cdr p)))
(define (%promise-value   p)         (cdr (cdr p)))
(define (%promise-set-forced! p)     (set-car! (cdr p) #t))
(define (%promise-set-value!  p v)   (set-cdr! (cdr p) v))
(define (%promise-redirect!   p q)   (set-cdr! p (cdr q)))
(define (promise? x)
  (and (pair? x) (eq? (car x) %promise-tag)))
(define (make-promise x)
  (if (promise? x)
    x
    (%promise-make #t x)))
(define-syntax delay
  (syntax-rules ()
    ((delay expr)
     (%promise-make #f (lambda () expr)))))
(define-syntax delay-force
  (syntax-rules ()
    ((delay-force expr)
     (%promise-make #f (lambda () expr)))))
(define-syntax lazy
  (syntax-rules ()
    ((lazy expr)
     (%promise-make #f (lambda () expr)))))
(define (force p)
  (if (not (promise? p))
    p
    (let loop ((promise p))
      (if (%promise-forced? promise)
        (%promise-value promise)
        (let ((val ((%promise-value promise))))
          (cond
            ((%promise-forced? promise)
             (%promise-value promise))
            ((promise? val)
             (%promise-redirect! promise val)
             (loop promise))
            (else
             (%promise-set-forced! promise)
             (%promise-set-value!  promise val)
             val)))))))
