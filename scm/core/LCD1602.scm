;;; Copyright 2026 by Frobenius Norm LLC 2026-05-16
;;; Free for non-commercial use. Commercial use requires a license.
(syslog "Loading LCD1602 functions\n")

(define have-LCD1602 (dict-ref? (current-environment) 'LCD1602.begin))

(unless have-LCD1602
  (define LCD1602.begin Lambda0)
  (define LCD1602.print Lambda0)
  )
  
  

(LCD1602.begin #x27 16 2)
(LCD1602.print "Test")

