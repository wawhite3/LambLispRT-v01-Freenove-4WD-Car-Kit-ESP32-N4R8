;;; Copyright 2026 by Frobenius Norm LLC 2026-05-16
(syslog "Loading Terminal settings\n")
(define esc-char (integer->char #x1b))
(define (terminal-colors!)
  (let ((fg (lambda (code) (if (ansi?) (format "~a~a" esc-char code) ""))))
    (set! fg_black     (fg "[30m"))
    (set! fg_blue      (fg "[94m"))
    (set! fg_green     (fg "[92m"))
    (set! fg_cyan      (fg "[96m"))
    (set! fg_red       (fg "[91m"))
    (set! fg_magenta   (fg "[95m"))
    (set! fg_brown     (fg "[93m"))
    (set! fg_white     (fg "[97m"))
    (set! fg_grey      (fg "[90m"))
    (set! fg_ltBlue    (fg "[34m"))
    (set! fg_ltGreen   (fg "[32m"))
    (set! fg_ltCyan    (fg "[36m"))
    (set! fg_ltRed     (fg "[31m"))
    (set! fg_ltMagenta (fg "[35m"))
    (set! fg_yellow    (fg "[93m"))
    (set! fg_ltWhite   (fg "[37m"))
    (ansi?)))
(define fg_black     "")
(define fg_blue      "")
(define fg_green     "")
(define fg_cyan      "")
(define fg_red	     "")
(define fg_magenta   "")
(define fg_brown     "")
(define fg_white     "")
(define fg_grey      "")
(define fg_ltBlue    "")
(define fg_ltGreen   "")
(define fg_ltCyan    "")
(define fg_ltRed     "")
(define fg_ltMagenta "")
(define fg_yellow    "")
(define fg_ltWhite   "")
(terminal-colors!)
(define (news    . args) (if (muted? 'news)    (if #f #f) (syslog "~a~a~a" fg_green     (apply format args) fg_white)))
(define (warn    . args)                                  (syslog "~a~a~a" fg_yellow    (apply format args) fg_white))
(define (term    . args) (if (muted? 'term)    (if #f #f) (syslog "~a~a~a" fg_cyan      (apply format args) fg_white)))
(define (info    . args) (if (muted? 'info)    (if #f #f) (syslog "~a~a~a" fg_white     (apply format args) fg_white)))
(define (blue    . args) (if (muted? 'blue)    (if #f #f) (syslog "~a~a~a" fg_ltBlue    (apply format args) fg_white)))
(define (magenta . args) (if (muted? 'magenta) (if #f #f) (syslog "~a~a~a" fg_ltMagenta (apply format args) fg_white)))
(define (errmsg  . args)                                  (syslog "~a~a~a" fg_red       (apply format args) fg_white))
(define banner
  (let* (
	 (b0 '(
	      "\n"
	      "8                          8 \n"
	      "8     eeeee eeeeeee eeeee  8     e  eeeee eeeee \n"
	      "8e    8   8 8  8  8 8   8  8e    8  8   ' 8   8 \n"
	      "88    8eee8 8e 8  8 8eee8e 88    8e 8eeee 8eee8 \n"
	      "88    88  8 88 8  8 88   8 88    88    88 88 \n"
	      "88eee 88  8 88 8  8 88eee8 88eee 88 8ee88 88 \n"
	      "\n"
	      ))
	 (b1 '(
	      "\n"
	      ":                          : \n"
	      ":     ..... ....... .....  :     .  ..... ..... \n"
	      ":.    :   : :  :  : :   :  :.    :  :   ' :   : \n"
	      "::    :...: :. :  : :...:. ::    :. :.... :...: \n"
	      "::    ::  : :: :  : ::   : ::    ::    :: :: \n"
	      "::... ::  : :: :  : ::...: ::... :: :..:: :: \n"
	      "\n"
	      ))
	 (b b0)
	 )
    (letrec ((f (lambda (l) (if (null? l)
			    #f
			    (begin
			      (news (car l))
			      (f (cdr l))
			      )
			    )
		    )))
      (info "LambLisp is Copyright 2023-2025 Frobenius Norm LLC\n")
      (lambda () (f b))
      )
    )
  )
(banner)
(set! banner #f)
(news "Good news in green\n")
(info "Info in white\n")
(warn "Warnings in yellow\n")
(errmsg "Errors in red\n")
(define (print-alist alist)
  (letrec ((print1 (lambda (p) (info "  (~a . ~a)\n" (car p) (cdr p))))
	   (printrest (lambda (l)
			(unless (null? l)
			  (print1 (car l))
			  (printrest (cdr l))
			  )
			)
		      )
	   )
    (info "(\n")
    (printrest alist)
    info ")\n")
  )
