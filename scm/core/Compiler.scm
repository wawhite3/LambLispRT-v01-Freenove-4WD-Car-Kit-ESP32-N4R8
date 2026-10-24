;;; Copyright 2026 by Frobenius Norm LLC 2026-05-16
;;; Free for non-commercial use. Commercial use requires a license.
#|
The compiler is based on nlambda and macro expansion.

Recall that arguments to a lambda function are evaluated, and the results of the evaluation are the values to which the function is applied.
Arguments to an nlambda function are *not* evaluated; the body of the nlambda receives the arguments just as they are in the source code.

The nlambda feature forms the basis for syntax evaluation in LambLisp.  It is possible to implement the entirety of Scheme syntax (except syntax-rules macros)
using nlambda functions to implement the syntactic primitives.

In the interpreted mode, all source code symbols are retained, and their values looked up at run time.  This allows symbols to be reassigned new values at runtime.
Some might argue that it is not a useful feature to be able to replace, say, the *if* operator.  But this arrangement has some elegant features.

First, it can be used for overloading an operator to handle new argument types.
To do this, define the new type and its type predicate, and then define a function that checks the type and either operates on the new type, or applies the old operator to the object
Redefine the symbol in the environment, and now you have a more capable operator.
For example, this technique could be used to add support for complex numbers, or rational numbers, to an implementation that has only integers and floats.

Another important result is using the rebinding feature in the compilation chain.  LambLisp is implemented at a very high level.
The operations defined in the Scheme R5RS specification are coded directly in C++.
From that viewpoint, the system is nearly completely compiled already.

In interpreted mode, the symbols are looked up in the environment and their values (which are functions) are called.
LambLisp supports a fundamental T_MACRO type, which does the following:
At run-time, during eval, when a macro is encountered the macro body is applied to the arguments, and then the result of that application is further evaluated (tail-recursively).
Notably, at run-time macros may be expanded repeatedly, which may be desirable or may simply use additional time.

At read time, the result of macro evaluation is executed, and then that result is substituted into the program.
The result is that the symbol is compiled away, and replaced with the address of the associated function, which is then called directly at runtime.
This eliminates the lookup at run time, but also does not allow for further changes to the operations.
Also, when errors occur, the debug information is more difficult to interpret.
|#

(syslog "Loading Compiler\n")

;;; If sym is a symbol whose value in env is a procedure or nlambda,
;;; wrap it in a macro cell so the reader expands it at read time, eliminating
;;; the run-time symbol lookup.
;;; cons-fn must be the raw T_MOP3_PROC for cons, captured before any wrapping,
;;; so the macro body never looks up the symbol 'cons' at expansion time (B25).
(define (compile-symbol sym env cons-fn)
  (when (symbol? sym)
    (let ((proc (dict-ref env sym)))
      (when (or (procedure? proc) (nprocedure? proc))
        (dict-rebind! env sym (macro args (cons-fn proc args)))))))

;;; Compile a list of symbols in env.
(define (compile list-of-symbols env cons-fn)
  (unless (null? list-of-symbols)
    (compile-symbol (car list-of-symbols) env cons-fn)
    (compile (cdr list-of-symbols) env cons-fn)))

;;; Traverse env and compile every symbol whose value is a procedure or nlambda.
;;; Uses dict-keys to collect all keys across all frames of the environment dict,
;;; then iterates with for-each to avoid recursion-depth growth on large environments.
;;; cons-fn is captured before the loop so that after 'cons' itself is wrapped,
;;; macro bodies still hold the raw T_MOP3_PROC value and never recurse (B25 fix).
(define (compile-environment env)
  (let ((keys    (dict-keys env))
        (cons-fn (dict-ref env 'cons)))
    (syslog "Compiler: compiling ~a symbols\n" (length keys))
    (for-each (lambda (sym) (compile-symbol sym env cons-fn)) keys)
    (syslog "Compiler: done\n")))

;;; ---------------------------------------------------------------------------------------
;;; PROMOTING A BINDING IN PLACE -- the plain form takes a VALUE, the `!` form takes a NAME.
;;;
;;; `procedure->bytecode` and `ncg-compile` each return a NEW procedure and leave the caller's
;;; binding exactly as it was.  Discarding that return value is SILENT: the call succeeds, the
;;; value it hands back is a real compiled procedure, and the name still holds the interpreted
;;; one.  Nothing raises and nothing warns.  The only symptom is a tier-shaped hole in a number
;;; -- which reads as a missing feature, a bad board profile or a build flag, i.e. as anything
;;; except a missing assignment.
;;;
;;; So the pair is split by ARGUMENT KIND, which is what the bang conventionally means in Scheme
;;; -- the bang form takes a NAME and mutates the binding, exactly as `set!` does:
;;;
;;;     (procedure->bytecode f)    ;; a PROCEDURE -> a new procedure.  `f` is UNTOUCHED.
;;;     (procedure->bytecode! f)   ;; a NAME      -> `f` now holds the bytecode procedure.
;;;
;;; Nothing is renamed and nothing is aliased: the plain forms keep their current spelling and
;;; their current meaning, so existing code and the manual stay correct.
;;;
;;; THEY ARE MACROS, AND THAT IS NOT A STYLE CHOICE.  The same operation could be written as an
;;; nlambda, and must not be: an nlambda in call position is substituted by the READER before
;;; the bytecode or native compiler ever sees the symbol, so ANY procedure containing the form
;;; stops being compilable -- a promotion verb whose presence in a procedure prevents THAT
;;; procedure from being promoted.  Measured: a fixture built that way reported three identical
;;; tier numbers with no native code anywhere, i.e. the tiers were ABSENT, not slower.  A macro
;;; expanding to `set!` has neither problem, and it reaches whatever `set!` reaches -- a local
;;; `let` binding included, which a form taking a quoted symbol could not.
;;;
;;; THE RETURN VALUE IS THE TIER, NOT THE PROCEDURE, so the one thing that used to be silent is
;;; the thing the caller gets back: #t only when the BINDING is at the requested tier on return.
;;; Gate a tier measurement on it.  A procedure the compiler declines answers #f -- true and
;;; actionable -- and so does a C++ builtin, which has no Scheme tier to be promoted to.

;;!Promote the procedure bound to `name` to BYTECODE, rebinding `name`.  #t when the binding
;;!holds bytecode on return; #f when the compiler declined it or it was never a Scheme procedure.
(define-syntax procedure->bytecode!
  (syntax-rules ()
    ((_ name) (begin (set! name (procedure->bytecode name)) (bytecode? name)))))

;;!Promote the procedure bound to `name` to NATIVE code, rebinding `name`, and promote every
;;!global it calls.  #t when the binding is native on return, #f otherwise.
;;!
;;!THREE STEPS, IN THIS ORDER, AND EVERY ONE OF THEM IS LOAD-BEARING.
;;!
;;!1. THE ROOT TO BYTECODE, REBOUND.  Handed an interpreted procedure the native compiler has
;;!   nothing to compile and hands the same procedure straight back -- so climbing this rung here
;;!   is what stops it ever being asked that question.  Rebinding is what makes the bang true.
;;!2. THE CALLEES TO BYTECODE, IN PLACE.  **A NATIVE WALK CANNOT ENTER AN INTERPRETED CALLEE** --
;;!   it steps from a procedure to its callee through that callee's BINDING, and returns at once
;;!   for anything that is not already bytecode.  Measured here before this line existed: root
;;!   natively compiled at 352 bytes, its one callee still reporting no native code at all.  That
;;!   is a NATIVE SHELL AROUND AN INTERPRETED BODY -- the root's code size and the native binding
;;!   count both report real numbers, the timing is the interpreter's, and nothing a caller can
;;!   reach disagrees.  Do not drop this step because step 3 "is transitive": step 3 only runs
;;!   this one itself when its ROOT is interpreted, which after step 1 it never is.
;;!3. ROOT AND CALLEES TO NATIVE, IN PLACE.  One walk over a tree that is bytecode throughout.
;;!
;;!THE `and` IS THE REFUSAL, AND IT IS A VALUE RATHER THAN A THROW.  If the root could not be made
;;!bytecode -- a C++ builtin, or a procedure the bytecode compiler declines -- the answer is #f,
;;!the same word the bytecode form uses, instead of a raise out of a callee written to treat a
;;!non-bytecode argument as a caller mistake.  Measured: without it, `(procedure->native! car)`
;;!raised *wants a procedure or bytecode, got type 19* and aborted the enclosing load.  #f is not
;;!a quiet success -- it is the one answer a caller gating a tier measurement acts on.
(define-syntax procedure->native!
  (syntax-rules ()
    ((_ name) (begin (set! name (procedure->bytecode name))
                     (and (bytecode? name)
                          (begin (bytecode-compile-transitive! name)
                                 (ncg-compile-transitive! name)))))))

(syslog "Compiler loaded\n")

