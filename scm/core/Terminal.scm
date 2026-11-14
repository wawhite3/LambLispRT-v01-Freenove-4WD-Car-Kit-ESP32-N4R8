;;; Copyright 2026 by Frobenius Norm LLC 2026-05-16
;;; Free for non-commercial use. Commercial use requires a license.
(syslog "Loading Terminal settings\n")

;;; ANSI ESCAPES ARE A FEATURE -- ON WHEREVER THE TERMINAL OBEYS THEM, OFF WHERE IT DOES NOT.
;;;
;;; `(ansi?)` is the one predicate, decided in C++ at startup before the first byte is printed:
;;; an explicit LL_COLOR=0/1, else the no-color.org NO_COLOR convention, else the console's own
;;; answer.  On Windows the console is genuinely ASKED (SetConsoleMode with
;;; ENABLE_VIRTUAL_TERMINAL_PROCESSING), so a modern console gets full colour; Wine's console
;;; refuses to be asked and gets none.  Nothing here tests the PLATFORM, because the platform is
;;; not what varies -- one Windows binary can meet all three answers.
;;;
;;; WHY THE TEST IS AT RUN TIME AND NOT A BUILD-TIME EDIT: this file ships to every target in every
;;; customer package.  There is one copy and no per-target variant to change.
;;;
;;; WHAT A WRONG ANSWER LOOKS LIKE, in both directions: too eager and a terminal that cannot obey
;;; them shows `[97m` as text, which is the bug this fixes and is visible ONLY to an eye on a
;;; console -- a piped run strips nothing and looks identical either way.  Too shy and a perfectly
;;; good terminal loses its colours, which is cosmetic and immediately obvious.  Check by looking.
(define esc-char (integer->char #x1b))

;;; Recompute the 16 colour strings from the CURRENT `(ansi?)`.  Called once at load, and again by
;;; hand after `(ansi! ...)` -- the C++ `fg_*` are repointed the instant `ansi!` runs, but these
;;; Scheme bindings hold whatever they were computed from, so the two agree only if this is re-run.
;;; Stated plainly rather than hidden: the ordinary case is LL_COLOR set before startup, where load
;;; time IS the only time and no refresh is needed.
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

;;; Defined before the first `terminal-colors!` call so `set!` has bindings to assign.
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

;;; THE SEVEN EMITTERS -- AND SINCE [B874] THEY HONOUR `(quiet ...)`, WHICH THEY DID NOT BEFORE.
;;;
;;; WHAT WAS WRONG, because it is invisible from either side alone.  These seven were IDENTICAL
;;; EXCEPT FOR COLOUR: every one called `syslog`, which lands in `LL_Port::log_string()` and is
;;; ungated.  Meanwhile `src/ll_vm_term.h` defines macros with THE SAME SEVEN NAMES, each gated on
;;; `ll_term.muted(LL_CH_*)`.  So the P243 channel mask governed the C++ `news()` and not the Scheme
;;; `(news ...)` -- and `(quiet 'news)` left 381 Scheme call sites talking.  Read the C++ and the
;;; mechanism looks complete; read this file and the names look like they must be the same thing.
;;; Neither reading is wrong on its own, which is why it survived.
;;;
;;; THE TEST COMES BEFORE `(apply format args)` ON PURPOSE -- that is the whole point, not a
;;; micro-optimisation.  The cost of a suppressed message is its FORMATTING, not its transmission
;;; ([B231]: a message formatted and then discarded cost a ~280us spike on a mutator cons, which was
;;; the entire unexplained tail of the gcpause benchmark).  A real `(news ...)` line here measured
;;; 1,715 bytes.  Gating at the `syslog` call instead would suppress the output and keep every byte
;;; of the cost, which is exactly backwards from what the caller assumes when they ask for quiet.
;;;
;;; `warn` AND `errmsg` ARE UNGATED, AND THAT IS THE OWNER'S CALL (2026-10-17, [B874]):
;;; *"warnings and errors seem not beneficial to suppress, only info."*  Errors were already
;;; unsilenceable -- deliberately, and this tree has paid twice for the alternative ([B264] three
;;; diagnoses, [B370] a day).  Warnings now join them: `LL_CH_WARN` is out of `LL_CH_APP`, there is
;;; no `(quiet 'warn)` selector, and the C++ `warn()` macro has no mask test either.  So these two
;;; lines are ungated for the same reason, at both ends of the language boundary.
;;; If a warning is too chatty, it is MISCLASSIFIED -- make it `info`.  Five C++ sites were moved
;;; that way in the same commit, the 15-second min/max-free-mem report chief among them.
;;;
;;; `(muted? ...)` is a C++ mop3 reading the ONE mask (`ll_term.quiet_mask`); this file keeps no
;;; copy of the channel bits, so adding a channel does not mean editing two places.
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
    ;; f is self-recursive -> letrec (strict R7RS let* does not scope a binding in its own init)
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
(set! banner #f)	;purge the banner to reclaim space

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
