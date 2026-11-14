;;; Copyright 2026 by Frobenius Norm LLC 2026-09-09
(define-syntax receive
  (syntax-rules ()
    ((_ formals mv-expr body0 body ...)
     (call-with-values (lambda () mv-expr) (lambda formals body0 body ...)))))
(define-syntax and-let*
  (syntax-rules ()
    ((_ ()) #t)
    ((_ () body0 body ...) (begin body0 body ...))
    ((_ ((var expr)))       (let ((var expr)) var))
    ((_ ((expr)))           expr)
    ((_ (bound-var))        bound-var)
    ((_ ((var expr) claw ...) body ...) (let ((var expr)) (and var (and-let* (claw ...) body ...))))
    ((_ ((expr)     claw ...) body ...) (and expr       (and-let* (claw ...) body ...)))
    ((_ (bound-var  claw ...) body ...) (and bound-var  (and-let* (claw ...) body ...)))))
(define-syntax cut
  (syntax-rules (<> <...>)
    ((_ . args) (%srfi26-cut (s01 s02 s03 s04 s05 s06 s07 s08 s09 s10 s11 s12) () () . args))))
(define-syntax %srfi26-cut
  (syntax-rules (<> <...>)
    ((_ supply (slot ...) (proc arg ...))          (lambda (slot ...) (proc arg ...)))
    ((_ supply (slot ...) (proc arg ...) <...>)    (lambda (slot ... . rest) (apply proc arg ... rest)))
    ((_ (s0 s ...) (slot ...) (pos ...) <> se ...) (%srfi26-cut (s ...) (slot ... s0) (pos ... s0) se ...))
    ((_ supply (slot ...) (pos ...) nse se ...)    (%srfi26-cut supply (slot ...) (pos ... nse) se ...))))
(define-syntax cute
  (syntax-rules (<> <...>)
    ((_ . args) (%srfi26-cute (s01 s02 s03 s04 s05 s06 s07 s08 s09 s10 s11 s12) () () () . args))))
(define-syntax %srfi26-cute
  (syntax-rules (<> <...>)
    ((_ supply (slot ...) binds (proc arg ...))         (let binds (lambda (slot ...) (proc arg ...))))
    ((_ supply (slot ...) binds (proc arg ...) <...>)   (let binds (lambda (slot ... . rest) (apply proc arg ... rest))))
    ((_ (s0 s ...) (slot ...) binds (pos ...) <> se ...) (%srfi26-cute (s ...) (slot ... s0) binds (pos ... s0) se ...))
    ((_ supply (slot ...) (bind ...) (pos ...) nse se ...)
     (%srfi26-cute supply (slot ...) (bind ... (b nse)) (pos ... b) se ...))))
(define-record-type <box> (box value) box? (value unbox set-box!))
(define-syntax assume
  (syntax-rules ()
    ((_ expr msg ...) (if expr #t (error "SRFI 145 assumption violated" (quote expr) msg ...)))))
