;;; Copyright 2026 by Frobenius Norm LLC 2026-05-25
(define (error message . irritants)   (raise (list 'error-object message irritants)))
(define (error-object? obj)           (and (pair? obj) (eq? (car obj) 'error-object)))
(define (error-object-message obj)    (cadr obj))
(define (error-object-irritants obj)  (caddr obj))
(define-syntax guard
  (syntax-rules (else)
    ((guard (var (else e1 e2 ...)) body ...)
     (with-exception-handler (lambda (var) e1 e2 ...) (lambda () body ...)))
    ((guard (var clause ...) body ...)
     (with-exception-handler (lambda (var) (cond clause ... (else (raise var)))) (lambda () body ...)))))
