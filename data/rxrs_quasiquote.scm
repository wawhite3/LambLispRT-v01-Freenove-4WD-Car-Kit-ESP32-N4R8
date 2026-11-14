;;; Copyright 2026 by Frobenius Norm LLC 2026-05-25
(define (qq-do-depth tmpl depth)
  (cond
    ((vector? tmpl)
     (list 'list->vector (qq-do-depth (vector->list tmpl) depth)))
    ((not (pair? tmpl))
     (list 'quote tmpl))
    ((and (eq? (car tmpl) 'unquote) (= depth 1))
     (cadr tmpl))
    ((eq? (car tmpl) 'unquote)
     (list 'list (list 'quote 'unquote)
                 (qq-do-depth (cadr tmpl) (- depth 1))))
    ((eq? (car tmpl) 'quasiquote)
     (list 'list (list 'quote 'quasiquote)
                 (qq-do-depth (cadr tmpl) (+ depth 1))))
    ((and (pair? (car tmpl))
          (eq? (caar tmpl) 'unquote-splicing)
          (= depth 1))
     (list 'append (car (cdr (car tmpl)))
                   (qq-do-depth (cdr tmpl) depth)))
    ((and (pair? (car tmpl))
          (eq? (caar tmpl) 'unquote-splicing))
     (list 'cons (list 'list (list 'quote 'unquote-splicing)
                             (qq-do-depth (car (cdr (car tmpl))) (- depth 1)))
                 (qq-do-depth (cdr tmpl) depth)))
    (else
     (list 'cons (qq-do-depth (car tmpl) depth)
                 (qq-do-depth (cdr tmpl) depth)))))
(define (qq-do tmpl)
  (qq-do-depth tmpl 1))
(define quasiquote
  (macro use-form
    (qq-do-depth (car use-form) 1)))
(define qq
  (macro use-form
    (qq-do-depth (car use-form) 1)))
