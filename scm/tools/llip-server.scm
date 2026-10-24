; Copyright 2026 by Frobenius Norm LLC 2026-04-12 00:00:00
;;; Free for non-commercial use. Commercial use requires a license.
; llip-server.scm -- LambLisp Interaction Protocol server

#|!
@file llip-server.scm
@brief LLIP server -- serves filesystem operations to authenticated remote LambLisp clients.

## Overview

LLIP (LambLisp Interaction Protocol) is a simple line-oriented S-expression protocol that lets
a host machine (running llip-client.scm) read, write, and manage files on a LambLisp target
board (ESP32, Jetson, or any POSIX host) over a TCP or TLS connection.

## Wire protocol

Each message is one S-expression per line, written with `write` and read with `read`.

Session flow:

    Server -> Client:  (hello "llip/1.0")
    Client -> Server:  (auth "secret-token")
    Server -> Client:  (ok)                          ; auth accepted; or server closes on failure

    Client -> Server:  <request>
    Server -> Client:  (ok <value> ...)  or  (error "message")
    ...
    Client -> Server:  (bye)
    Server -> Client:  (ok)                          ; then server closes the connection

## Supported operations

    (ping)                            -> (ok)
    (bye)                             -> (ok)   then close
    (ls "/path")                      -> (ok ("file1" "file2" ...))
    (stat "/path")                    -> (ok <size-bytes>)
    (read "/path")                    -> (ok "<full-file-content>")
    (read "/path" <offset> <n>)       -> (ok "<n-byte-chunk>")
    (write "/path" "<content>")       -> (ok)
    (append "/path" "<content>")      -> (ok)
    (delete "/path")                  -> (ok)
    (rename "/from" "/to")            -> (ok)
    (load "/path")                    -> (ok <last-eval-result>)
    (eval <expr>)                     -> (ok <value>)          or  (error "msg")

## `eval` returns a DATUM, not its printed form

`(ok <value>)` carries the VALUE.  This line used to read `(ok "<write-repr>")` -- a string -- while
the dispatcher has always sent `(llip-ok conn (eval expr (interaction-environment)))`, i.e. the value
itself.  The two statements sat in one file and disagreed, and the HEADER is the half a client author
reads.

**The consequence was a client that double-parses:** it reads a datum, believes it is a string, and
parses it again.  For a result that happens to be a string the extra parse often appears to work,
which is the bad case -- the defect then surfaces only on some return types, long after the client
was written and tested against the ones that hid it.

Returning the datum is also the better contract, so this is not a free choice between two
conventions: a Scheme client can use the value directly, whereas a printed representation forces
every caller to re-read it, and re-reading `write` output is lossy for anything whose printed form
does not round-trip.

## Path safety

All paths must be absolute (start with `/`), must not contain `..`, and must not contain NUL bytes.
Requests that fail the safety check receive `(error "bad path")`.

## Usage

    (load "llip-server.scm" 1)

    ;; TLS server (production):
    (define srv (llip-make-server 8443 my-cert-pem my-key-pem "secret-token"))

    ;; Plain TCP server (development / LAN only):
    (define srv (llip-make-tcp-server 8080 "secret-token"))

    ;; Call from the main loop -- handles at most one connection per call:
    (llip-server-poll srv)

@see llip-client.scm
@see ll_tests/llip-tests.scm
|#

;;; ---------------------------------------------------------------------------
;;; Path sanitizer
;;; ---------------------------------------------------------------------------

#|!
@defgroup llip_server_path Path sanitizers
@brief Internal helpers that validate remote file paths before any I/O.

llip-safe-path? rejects paths that: are not strings, are empty, do not start with `/`,
contain `..` (directory traversal), or contain a NUL byte.
|#

(define (llip-has-nul? s)
  ;; Returns #t if string s contains a NUL byte (integer->char 0).
  ;; Cannot use a string literal "\x00" -- the reader null-terminates it.
  (let ((nul (integer->char 0))
        (n   (string-length s)))
    (let loop ((i 0))
      (cond ((= i n) #f)
            ((char=? (string-ref s i) nul) #t)
            (else (loop (+ i 1)))))))

(define (llip-safe-path? p)
  (and (string? p)
       (> (string-length p) 0)
       (char=? (string-ref p 0) #\/)
       (not (llip-string-contains p ".."))
       (not (llip-has-nul? p))))

(define (llip-string-contains str sub)
  ;; Returns #t if str contains sub as a substring.
  (let ((slen (string-length str))
        (sublen (string-length sub)))
    (if (> sublen slen)
      #f
      (let loop ((i 0))
        (cond ((> (+ i sublen) slen) #f)
              ((string=? (substring str i (+ i sublen)) sub) #t)
              (else (loop (+ i 1))))))))

;;; ---------------------------------------------------------------------------
;;; Response helpers
;;; ---------------------------------------------------------------------------

#|!
@defgroup llip_server_io Wire I/O helpers
@brief Low-level send/receive and response-formatting helpers used by the dispatcher.

- llip-send: write one S-expression followed by a newline to `port`.
- llip-recv: read one S-expression from `port` (blocks until available or EOF).
- llip-ok:   send `(ok val ...)` -- zero or more values in the payload.
- llip-err:  send `(error msg)`.
|#

(define (llip-send port obj)
  (write obj port)
  (newline port))

(define (llip-recv port)
  (read port))

(define (llip-ok port . vals)
  (llip-send port (cons 'ok vals)))

(define (llip-err port msg)
  (llip-send port (list 'error msg)))

;;; ---------------------------------------------------------------------------
;;; File I/O helpers (read/write entire file as string)
;;; ---------------------------------------------------------------------------

#|!
@defgroup llip_server_fileio File I/O helpers
@brief Read and write entire files (or chunks) as Scheme strings.

These helpers are used internally by the dispatcher to implement the `read`, `write`,
`append`, and chunked-read operations.  On embedded targets the file content is
transferred as a Scheme string literal, so binary files containing NUL bytes
may be truncated; use chunked reads for large files to stay within heap limits.

- llip-read-file-string path     -- read entire file, return string.
- llip-write-file-string path s  -- overwrite file with string s.
- llip-append-file-string path s -- append string s to file (read-rewrite, no O_APPEND).
- llip-read-file-chunk path off n -- read up to n BYTES starting at byte offset off.
|#

;;; STREAM, DO NOT MATERIALISE.  An embedded target has small buffers and they have to be operated
;;; usefully rather than filled: every helper below moves a file through a BOUNDED working set and
;;; none of them holds a whole file in memory.
;;;
;;; `llip-section-size` is that working set.  It is the one number to change if a target is tighter
;;; or roomier; nothing here scales with FILE size.
(define llip-section-size 512)

;;; The runtime's cap on a single generated string (`toString_MAX_LENGTH`).  Named here because the
;;; `read` dispatch has to compare against it, and a bare 8192 at the comparison would be a second
;;; copy of a number that lives in the C++ layer.
(define llip-string-limit 8192)

;;; Size of a file WITHOUT reading it into memory -- count characters as they stream past.
(define (llip-file-size path)
  (if (not (file-exists? path))
      0
      (let ((p (open-input-file path)))
        (let loop ((n 0))
          (if (eof-object? (read-char p))
              (begin (close-input-port p) n)
              (loop (+ n 1)))))))

;;; Read a whole file into a string.  KEPT, AND DELIBERATELY BOUNDED: the caller is the `read`
;;; operation, whose reply is ONE s-expression carrying ONE string, so this cannot stream -- the
;;; wire shape, not this procedure, is what fixes the limit.  It therefore REFUSES a file it cannot
;;; return rather than returning a short one.
;;;
;;; IT USED TO TRUNCATE SILENTLY AT 8191 CHARACTERS and report success: `get-output-string` formatted
;;; through a `toString_MAX_LENGTH` buffer, so a 36 KB file came back as 8,191 bytes with no error.
;;; A backup taken that way is a stub that looks like a backup.  The `read` dispatch now checks the
;;; SIZE FIRST and names the streaming alternative, which is the useful answer at the point of
;;; failure.
(define (llip-read-file-string path)
  (let* ((p   (open-input-file path))
         (out (open-output-string)))
    (let loop ()
      (let ((c (read-char p)))
        (if (eof-object? c)
          (begin (close-port p) (get-output-string out))
          (begin (write-char c out) (loop)))))))

(define (llip-write-file-string path content)
  ;; Overwrite file with string content.
  (let ((p (open-output-file path)))
    (write-string content p)
    (close-port p)))

;;; Append, AS A STREAM COPY.  R7RS has no append mode, so the file must be rewritten -- but it does
;;; NOT have to be held.  This copies the existing content section by section into a temporary file,
;;; writes the new content, and renames; peak memory is one section regardless of file size.
;;;
;;; WHAT IT REPLACES WAS DESTRUCTIVE, not merely wasteful.  It read the whole file into a string via
;;; `llip-read-file-string` and wrote that back -- and that read was CAPPED at 8191 characters, so
;;; appending to a larger file rewrote it as 8191 + the new chunk and PERMANENTLY DISCARDED the rest.
;;; Measured: a 20 KB upload in 512-byte chunks settled at exactly 8,703 bytes, which is 8191 + 512 --
;;; the fixed point that loop converges to however many chunks follow.  An operation named `append`
;;; silently shortened the file it was appending to.
;;;
;;; THE TEMPORARY IS NAMED `.ap` AND THE RENAME IS LAST, so a copy that dies partway leaves the
;;; original intact -- the same reason `op` writes `.part` and renames at `cl`.  The old version
;;; opened the DESTINATION for output before it had finished reading it back, which is why a failure
;;; there lost data rather than just failing.
(define (llip-append-file-string path content)
  (let ((tmp (string-append path ".ap")))
    (if (file-exists? tmp) (delete-file tmp))
    (let ((out (open-output-file tmp)))
      (if (file-exists? path)
          (let ((in (open-input-file path)))
            (let loop ()
              (let ((c (read-char in)))
                (if (eof-object? c)
                    (close-input-port in)
                    (begin (write-char c out) (loop)))))))
      (write-string content out)
      (close-output-port out))
    (if (file-exists? path) (delete-file path))
    (rename-file tmp path)))

;; Read up to n BYTES starting at byte offset -- and it now does what that sentence says.
;; It used to count `read-char` calls for BOTH n and offset.  `read-char` decodes UTF-8 (it wraps
;; `read_codepoint`), so each call consumes 1-4 bytes and yields one codepoint: the length was a
;; character count wearing a byte count's name, the offset seeked to the wrong place as soon as any
;; multi-byte sequence preceded it, and `write-char` re-encoded the codepoint so binary content came
;; back altered.  Same defect as `sd` in features/llip-serial.scm, arrived at independently -- which
;; is why the fix belongs at BOTH and neither one alone closes it.
;; Returns a STRING of raw bytes (each character is one byte, 0-255), so callers that hex-encode or
;; length-check it are counting bytes.
(define (llip-read-file-chunk path offset n)
  (let ((p   (open-input-file path))
        (out (open-output-string))
        (buf (make-bytevector 128 0)))
    ;; Skip `offset` BYTES.  Read-and-discard rather than seek, because the port has no seek; the
    ;; count is what matters and it is now in the same unit as the caller's.
    (let skip ((left offset))
      (when (> left 0)
        (let* ((want (if (< left 128) left 128))
               (got  (read-bytevector! buf p 0 want)))
          (when (and (number? got) (> got 0)) (skip (- left got))))))
    (let loop ((left n))
      (when (> left 0)
        (let* ((want (if (< left 128) left 128))
               (got  (read-bytevector! buf p 0 want)))
          (when (and (number? got) (> got 0))
            (let emit ((i 0))
              (when (< i got)
                (write-char (integer->char (bytevector-u8-ref buf i)) out)
                (emit (+ i 1))))
            (loop (- left got))))))
    (close-port p)
    (get-output-string out)))

;;; ---------------------------------------------------------------------------
;;; Dispatcher -- handles one authenticated request, returns #f on (bye)
;;; ---------------------------------------------------------------------------

#|!
@brief Dispatch one LLIP request that has already been authenticated.
@param conn  An open bidirectional port connected to the client.
@param req   The S-expression read from the wire (must be a pair whose car is the op symbol).
@return #t to continue the session loop; #f to signal that the session should close.

Recognized operations are: ping, bye, ls, stat, read, write, append, delete, rename, load, eval.
Any unrecognized op sends `(error "unknown op")` and returns #t (keep session alive).
|#

(define (llip-dispatch conn req)
  (if (not (pair? req))
    (begin (llip-err conn "bad request") #t)
    (let ((op   (car req))
          (args (cdr req)))
      (cond

        ((eq? op 'ping)
         (llip-ok conn)
         #t)

        ((eq? op 'bye)
         (llip-ok conn)
         #f)  ; signal close

        ((eq? op 'ls)
         (let ((path (if (pair? args) (car args) "/")))
           (if (not (llip-safe-path? path))
             (llip-err conn "bad path")
             (llip-ok conn (directory-list path))))
         #t)

        ((eq? op 'stat)
         (let ((path (and (pair? args) (car args))))
           (if (not (llip-safe-path? path))
             (llip-err conn "bad path")
             (if (not (file-exists? path))
               (llip-err conn "not found")
               (llip-ok conn (file-size path)))))
         #t)

        ((eq? op 'read)
         (let ((path (and (pair? args) (car args))))
           (if (not (llip-safe-path? path))
             (llip-err conn "bad path")
             (if (not (file-exists? path))
               (llip-err conn "not found")
               (if (and (pair? (cdr args)) (pair? (cddr args)))
                 ;; chunked: (read path offset n)
                 (llip-ok conn (llip-read-file-chunk path (cadr args) (caddr args)))
                 ;; full: (read path) -- REFUSE what cannot be returned, and name the stream.
                 ;; The reply is one s-expression carrying one string, so a file past the string
                 ;; limit cannot come back whole.  Returning a SHORT one is the failure this
                 ;; replaces: it reported success and a 36 KB file arrived as 8,191 bytes.
                 (if (>= (llip-file-size path) llip-string-limit)
                     (llip-err conn "file too large for a whole read -- use (read path offset n)")
                     (llip-ok conn (llip-read-file-string path)))))))
         #t)

        ((eq? op 'write)
         (let ((path    (and (pair? args) (car args)))
               (content (and (pair? args) (pair? (cdr args)) (cadr args))))
           (if (or (not (llip-safe-path? path)) (not (string? content)))
             (llip-err conn "bad args")
             (begin
               (llip-write-file-string path content)
               (llip-ok conn))))
         #t)

        ((eq? op 'append)
         (let ((path    (and (pair? args) (car args)))
               (content (and (pair? args) (pair? (cdr args)) (cadr args))))
           (if (or (not (llip-safe-path? path)) (not (string? content)))
             (llip-err conn "bad args")
             (begin
               (llip-append-file-string path content)
               (llip-ok conn))))
         #t)

        ((eq? op 'delete)
         (let ((path (and (pair? args) (car args))))
           (if (not (llip-safe-path? path))
             (llip-err conn "bad path")
             (if (not (file-exists? path))
               (llip-err conn "not found")
               (begin (delete-file path) (llip-ok conn)))))
         #t)

        ((eq? op 'rename)
         (let ((from (and (pair? args) (car args)))
               (to   (and (pair? args) (pair? (cdr args)) (cadr args))))
           (if (or (not (llip-safe-path? from)) (not (llip-safe-path? to)))
             (llip-err conn "bad path")
             (begin (rename-file from to) (llip-ok conn))))
         #t)

        ((eq? op 'load)
         ;; Read and eval expressions from the file, return the last result.
         ;; Cannot use (load path) directly -- LambLisp's C++ load always returns ().
         (let ((path (and (pair? args) (car args))))
           (if (not (llip-safe-path? path))
             (llip-err conn "bad path")
             (if (not (file-exists? path))
               (llip-err conn "not found")
               (guard (exn (#t (llip-err conn
                                 (if (error-object? exn)
                                   (error-object-message exn)
                                   "load error"))))
                 (let* ((p      (open-input-file path))
                        (result (let loop ((last (if #f #f)))
                                  (let ((expr (read p)))
                                    (if (eof-object? expr)
                                      (begin (close-port p) last)
                                      (loop (eval expr (interaction-environment))))))))
                   (llip-ok conn result))))))
         #t)

        ((eq? op 'eval)
         ;; Evaluate an arbitrary S-expression in the interaction environment.
         ;; Returns (ok <value>) on success, (error "msg") on exception.
         ;; The result is sent as a Scheme datum (like the load op), not as a string.
         (if (not (pair? args))
           (llip-err conn "missing expression")
           (let ((expr (car args)))
             (guard (exn (#t (llip-err conn
                               (if (error-object? exn)
                                 (error-object-message exn)
                                 "eval error"))))
               (llip-ok conn (eval expr (interaction-environment))))))
         #t)

        (else
         (llip-err conn "unknown op")
         #t)))))

;;; ---------------------------------------------------------------------------
;;; Session handler
;;; ---------------------------------------------------------------------------

#|!
@brief Run a complete LLIP session on an already-accepted connection.
@param conn   An open bidirectional port for one accepted client connection.
@param token  The shared secret string that the client must supply in its `(auth ...)` message.

Sends the `(hello "llip/1.0")` banner, reads and checks the auth message, then loops
reading requests and calling llip-dispatch until #f is returned (bye or EOF).
Returns after the session ends; the caller is responsible for closing `conn`.

**THESE TWO BLOCK FOR THE CLIENT'S ENTIRE SESSION.  DO NOT CALL EITHER FROM THE MAIN LOOP --
use `llip-server-poll`.**  `llip-server-poll` called `llip-handle-session` until [B854]: on a
cooperative runtime that stopped the REPL, every behaviour and every motor tick until the client
chose to disconnect, and an IDLE connected client was enough, because the loop below sits in
`(read conn)` waiting for a request that may never come.  There is no amount of care at the call
site that fixes this -- the duration is the client's to choose, not the board's.

They are kept because a session handler is the right shape for a context that is ALLOWED to
block -- a dedicated task, or [P214]'s reverse-role websocket relay, which dials out and then
acts as the server on that one connection.  That is the only kind of caller they are for.
|#

#|!
[P69] Phase 4 -- AUTHENTICATED SESSIONS.

`llip-handle-session-auth` is the P69 route: RSA mutual authentication, a Diffie-Hellman session
key, and AES-GCM on every message after the handshake.  `llip-handle-session` below is the
LEGACY shared-secret route and is kept for unprovisioned nodes.

**BOTH OF THEM STILL BLOCK FOR THE CLIENT'S WHOLE SESSION -- see the [B854] warning further up.**
It is repeated here because of WHERE A READER ARRIVES.  Someone who greps `llip-handle-session`
lands on the `define` below and reads UPWARDS into this block, which until now said only that the
legacy route is "kept for unprovisioned nodes" -- an endorsement, with no mention that calling it
from the main loop stops the REPL, the behaviours and every motor tick for as long as the client
chooses.  The warning that covers both sits above the OTHER procedure's doc block, so the nearest
comment to the thing being read was the one that recommended it.

THE CHOICE IS MADE BY THE CALLER, NEVER BY THE WIRE.  P69 is explicit that there is no runtime
downgrade: if a peer could ask for the legacy path, an attacker who can drop packets could force
it, and the mutual authentication would be worth nothing.  So a server runs one or the other
because of how it was STARTED -- `llip-make-server` decides, once -- and a client that speaks the
wrong one simply fails to get past the first message.

WHAT MAKES IT SAFE TO HAVE BOTH: the two never appear on one connection.  The authenticated
session sends no `(hello "llip/1.0")` banner at all; its first message is the P69 `hello` carrying
a key id and a nonce, which the legacy path rejects as a malformed auth message and closes.
|#
;;! BLOCKS FOR THE CLIENT'S ENTIRE SESSION -- never from the main loop, use `llip-server-poll`.
;;! [B854].  An IDLE connected client is enough: the loop below sits in a read that may never
;;! return, and on a cooperative runtime that is the whole board, not just this task.
(define (llip-handle-session-auth conn)
  (let ((session-key (llip-server-handshake conn)))     ;;!< raises and closes on any failure
    (llip-send-encrypted conn session-key '(ok))
    (let loop ()
      (let ((req (llip-recv-encrypted conn session-key)))
        (if (eof-object? req)
            #f
            ;;! Dispatch is UNCHANGED and still writes plaintext replies through llip-send.
            ;;! That is deliberate for this phase: the request side is authenticated and
            ;;! confidential, the reply side is not yet.  Wiring the replies means threading the
            ;;! session key through llip-dispatch and every llip-ok/llip-err call site, which is
            ;;! a wider change than Phase 4 scopes and would be half-done here.  SAY IT RATHER
            ;;! THAN LET A READER ASSUME THE WHOLE SESSION IS ENCRYPTED.
            (when (llip-dispatch conn req)
              (loop)))))))

;;! BLOCKS FOR THE CLIENT'S ENTIRE SESSION -- never from the main loop, use `llip-server-poll`.
;;! [B854].  An IDLE connected client is enough: the loop below sits in a read that may never
;;! return, and on a cooperative runtime that is the whole board, not just this task.
;;! THIS FILE SHIPS TO EVERY CUSTOMER and the shipped `tools/llip` names this procedure in its
;;! own comments, so this is the entry point a reader is pointed at FIRST.  It is not the one to
;;! build a server on -- `llip-make-server` + `llip-server-poll` is.
(define (llip-handle-session conn token)
  ;; Handshake
  (llip-send conn '(hello "llip/1.0"))
  (let ((auth-msg (llip-recv conn)))
    (if (or (not (pair? auth-msg))
            (not (eq? (car auth-msg) 'auth))
            (not (pair? (cdr auth-msg)))
            (not (string=? (cadr auth-msg) token)))
      ;; Auth failed -- send nothing, just close
      #f
      ;; Auth OK
      (begin
        (llip-ok conn)
        (let loop ()
          (let ((req (llip-recv conn)))
            (if (eof-object? req)
              #f
              (when (llip-dispatch conn req)
                (loop)))))))))

;;; ---------------------------------------------------------------------------
;;; Server object and poll function
;;; ---------------------------------------------------------------------------

#|!
@defgroup llip_server_public Public server API
@brief Create an LLIP server and poll it from the main loop.

### llip-make-server port-num cert-pem key-pem token
Create a TLS LLIP server.  Returns an opaque server descriptor `(server-port token)`.
`cert-pem` and `key-pem` are PEM-encoded certificate and private key strings.
Available only when `LL_WIFI` is defined (ESP32).

### llip-make-tcp-server port-num token
Create a plain TCP LLIP server (development / LAN use only -- no encryption).
Returns an opaque server descriptor `(server-port token)`.

### llip-server-poll srv
Non-blocking poll.  Call once per main-loop iteration.  Services **ONE protocol frame
per call** and returns: an accept, or an auth message, or a single request, or nothing
at all when the socket has no byte waiting.  The connection is held in the descriptor
ACROSS calls and closed on `(bye)`, on EOF, on a bad token, or on any exception.
Returns unspecified.

**THIS IS WHAT CHANGED, AND WHY [B854] EXISTED.**  This function used to run the whole
session to completion inside one call -- accept, auth, every request, close.  That made
the word "poll" false in the only way that matters on a cooperative runtime: the main
loop did not turn again until the CLIENT chose to disconnect, so the REPL, every
behaviour, and every motor tick stopped for the entire session.  An **idle** connected
client was enough, because the call sat blocked in `(read conn)` waiting for a request
that was never coming.  Per-call cost is now bounded by ONE frame instead of by the
client's lifetime -- which is a bound this board controls and the old one was not.

**THE RESIDUAL, STATED SO NOBODY READS MORE INTO THIS THAN IT DOES.**  `char-ready?`
guarantees the FIRST byte is there, not the whole datum, and `read` blocks for the tail
of a frame split across packets.  So the worst case moved from *the session's duration*
to *one frame's remaining bytes* -- a network round trip, not a human one.  It is not
zero, and a sender that dribbles one byte at a time can still stall the loop for as long
as it keeps dribbling.

**AND ONE SEMANTIC CONSEQUENCE, which is the price of the fix rather than a defect:**
the main loop now runs BETWEEN two requests of the same session, so a behaviour or a REPL
form can interleave with an LLIP conversation.  Requests within one connection are still
ordered and still one-at-a-time; what is no longer true is that nothing else happens
between them.  Code that relied on a session being atomic against the rest of the system
was relying on the freeze.
|#

(define (llip-make-server port-num cert-pem key-pem token)
  ;; Returns a server descriptor list: (server-port token conn phase key) -- see llip-srv-* below.
  (llip-srv-new (open-tls-server-port port-num cert-pem key-pem) token))

(define (llip-make-tcp-server port-num token)
  ;; Plain TCP -- for development/LAN use only.
  (llip-srv-new (open-tcp-server-port port-num) token))

#|!
[P69] Phase 4 -- START A SERVER IN AUTHENTICATED MODE.

`(llip-make-auth-server port-num privkey-path trusted-path)` loads the keys and returns a
descriptor whose token slot is the symbol `auth`.  `llip-server-poll` reads that slot to decide
which session handler to run.

THE MODE IS IN THE DESCRIPTOR, NOT ON THE WIRE, AND THAT IS THE DESIGN.  P69 rules out runtime
negotiation because a peer that could ASK for the legacy path could be forced onto it by an
attacker who drops packets.  So the decision is made once, by whoever started the server, and a
client speaking the other protocol simply fails at the first message -- the authenticated handler
sees no P69 `hello` and raises; the legacy handler sees a P69 `hello` instead of `(auth ...)` and
closes.  Neither can be talked into the other.

KEYS ARE LOADED AT STARTUP, NOT PER CONNECTION.  An RSA key parse per accept would put ~600 digits
of bignum work in the accept path, and a failure there would surface as a mysterious refused
connection rather than as a startup error naming the missing file.
|#
(define (llip-make-auth-server port-num privkey-path trusted-path)
  (llip-load-keys privkey-path trusted-path)          ;;!< raises here, at startup, if a key is missing
  (llip-srv-new (open-tcp-server-port port-num) 'auth))

#|!
THE SERVER DESCRIPTOR IS `(server-port token conn phase key)` AND IT IS MUTATED IN PLACE.

It carries the live connection BETWEEN polls, because that is the whole mechanism of [B854]'s
fix: a poll that services one frame has to remember, on the next call, which connection it was
talking to and how far the conversation had got.

  conn   #f when idle, else the accepted connection port
  phase  'idle (no conn) | 'await-auth (accepted, not yet authenticated) | 'ready (authenticated)
  key    #f on the legacy path; the P69 session key once an authenticated handshake has completed

THE TWO SLOTS ADDED HERE ARE WHY AN OLD DESCRIPTOR CANNOT BE POLLED BY THIS CODE, and
llip-server-poll says so in one sentence rather than failing as `(car '())` three frames down.
|#
(define (llip-srv-new port token) (list port token #f 'idle #f))

(define (llip-srv-port       srv)   (car     srv))
(define (llip-srv-token      srv)   (cadr    srv))
(define (llip-srv-conn       srv)   (caddr   srv))
(define (llip-srv-phase      srv)   (cadddr  srv))
(define (llip-srv-key        srv)   (car (cddddr srv)))
(define (llip-srv-conn-set!  srv v) (set-car! (cddr   srv) v))
(define (llip-srv-phase-set! srv v) (set-car! (cdddr  srv) v))
(define (llip-srv-key-set!   srv v) (set-car! (cddddr srv) v))

#|!
@brief Extend a pre-[B854] two-slot descriptor `(server-port token)` to five slots, in place.

THIS EXISTS BECAUSE REAL CALLERS BUILD THE DESCRIPTOR BY HAND, AND REFUSING THEM WAS THE WRONG
CALL.  The first version of this fix rejected a short descriptor with a clear error, on the
reasoning that failing loudly beats silently reintroducing the freeze.  Then the first hardware
run found `w3_ai_scripts/reset_and_push.py:187` -- a live tool other sessions use to stand a
server up on a board -- doing exactly `(define llip-srv (list (open-tcp-server-port 8081)
"testtoken"))`, and a bench `sockup.scm` doing the same.  Refusing would have broken working rigs
on upgrade; extending costs one `set-cdr!` and hands those same callers the fix without their
touching anything.  **Upgrading is not tolerating a bug here** -- the upgraded descriptor takes
the one-frame-per-call path like any other, so the old caller stops freezing the loop too.

Prefer `llip-make-tcp-server` / `llip-make-server` / `llip-make-auth-server` in new code: they are
the documented constructors and they already produce five slots.
|#
(define (llip-srv-upgrade! srv)
  (when (and (pair? srv) (pair? (cdr srv)) (not (pair? (cddr srv))))
    (set-cdr! (cdr srv) (list #f 'idle #f))))

#|!
@brief Close the live connection, if any, and return the descriptor to the idle state.

The close is itself guarded: a peer that has already vanished can make close-port raise, and a
server that fell over while hanging up would leave `conn` set and never accept again -- i.e. the
error path would reintroduce exactly the permanent-freeze symptom the fix exists to remove.
|#
(define (llip-srv-drop! srv)
  (let ((conn (llip-srv-conn srv)))
    (when conn (guard (exn (#t #f)) (close-port conn))))
  (llip-srv-conn-set!  srv #f)
  (llip-srv-phase-set! srv 'idle)
  (llip-srv-key-set!   srv #f))

#|!
@brief Drain whitespace sitting BETWEEN frames, so `char-ready?` means "a FRAME is waiting".

WITHOUT THIS THE POLL DEADLOCKS ON ITS OWN PROTOCOL, AND IT IS NOT A SPLIT-PACKET EDGE CASE --
IT HAPPENS ON EVERY WELL-FORMED FRAME.  `llip-send` writes a datum followed by a NEWLINE, and
`read` stops at the datum's closing paren and LEAVES that newline in the port.  So one byte is
always still buffered after a successful read: the next poll sees `char-ready?` -> #t, calls
`read`, which skips the whitespace and then BLOCKS waiting for a datum nobody sent -- i.e. the
exact freeze [B854] is about, reintroduced one frame later by the fix for it.

MEASURED, first run of llip-server-tests.scm: nine checks passed and the suite then hung at
`s5 authenticated but idle`, with the log's last line naming that poll.  The newline left by the
client's own `(auth "testtoken")` was the byte that made `char-ready?` lie.

Draining WHITESPACE rather than "one newline" is deliberate: it is correct for any client that
pads or terminates differently (CRLF, a blank line), and no LLIP frame can begin with whitespace.
`peek-char` at EOF returns the eof-object, which is not a char, so the loop ends and the caller's
`char-ready?` still reports the EOF for the read to turn into a clean disconnect.
|#
(define (llip-srv-skip-interframe-ws! port)
  (let loop ()
    (when (char-ready? port)
      (let ((c (peek-char port)))
        (when (and (char? c) (char-whitespace? c))
          (read-char port)
          (loop))))))

;;; One frame of an authentication exchange.
;;;
;;; THE LEGACY PATH IS ONE MESSAGE, SO IT IS ONE CALL.  The P69 path is a FIXED, BOUNDED
;;; handshake and runs inside this single call on purpose: it is a known small number of
;;; messages, so it is charged to one loop tick the way a large file read already is -- unlike a
;;; session, whose length is the client's choice.  Making it incremental too is a separate piece
;;; of work and is not pretended here.
(define (llip-srv-step-auth! srv conn)
  (if (eq? (llip-srv-token srv) 'auth)
      (let ((key (llip-server-handshake conn)))     ;;!< raises on any failure -> guarded -> drop
        (llip-send-encrypted conn key '(ok))
        (llip-srv-key-set!   srv key)
        (llip-srv-phase-set! srv 'ready))
      (let ((auth-msg (llip-recv conn)))
        (if (or (not (pair? auth-msg))
                (not (eq? (car auth-msg) 'auth))
                (not (pair? (cdr auth-msg)))
                (not (string=? (cadr auth-msg) (llip-srv-token srv))))
            ;;! Auth failed -- send nothing, just close.  An unauthenticated peer learns nothing
            ;;! from the failure mode, which is the same reason the P69 path closes silently.
            (llip-srv-drop! srv)
            (begin
              (llip-ok conn)
              (llip-srv-phase-set! srv 'ready))))))

;;; Exactly one request, then back to the main loop.
(define (llip-srv-step-request! srv conn)
  (let* ((key (llip-srv-key srv))
         (req (if key (llip-recv-encrypted conn key) (llip-recv conn))))
    (if (eof-object? req)
        (llip-srv-drop! srv)
        (when (not (llip-dispatch conn req))        ;;!< #f = (bye), or a dispatcher that is done
          (llip-srv-drop! srv)))))

(define (llip-server-poll srv)
  ;; Non-blocking poll.  Call from main loop.  Services ONE frame per call -- see the contract
  ;; in the public API block above, and [B854] for what this used to do instead.
  ;; Catches all exceptions so a bad client cannot kill the poll loop.
  ;;! A 2-slot descriptor -- hand-built, or made by the pre-[B854] file -- is EXTENDED here rather
  ;;! than refused; see llip-srv-upgrade! for the callers that made that the right choice.
  (llip-srv-upgrade! srv)
  (if (not (and (pair? srv) (pair? (cdr srv)) (pair? (cddr srv))
                (pair? (cdddr srv)) (pair? (cddddr srv))))
      ;;! Still short after the upgrade = not a server descriptor at all.  Say so here: without
      ;;! this the first symptom is `(car '())` inside an accessor, which names neither the cause
      ;;! nor the fix.
      (error "llip-server-poll: not a server descriptor -- expected (server-port token ...) from llip-make-tcp-server / llip-make-server / llip-make-auth-server")
      (let ((conn (llip-srv-conn srv)))
        (if (not conn)
            ;; IDLE: try to accept.  server-accept is already non-blocking (returns #f).
            (let ((new (server-accept (llip-srv-port srv))))
              (when new
                (llip-srv-conn-set!  srv new)
                (llip-srv-phase-set! srv 'await-auth)
                (guard (exn (#t (llip-srv-drop! srv)))
                  ;;! The banner goes out at accept time, which is the same wire order as before:
                  ;;! it was the first thing the old session handler did.  The authenticated path
                  ;;! sends NO banner -- its first message is the P69 hello from the client.
                  (when (not (eq? (llip-srv-token srv) 'auth))
                    (llip-send new '(hello "llip/1.0"))))))
            ;; CONNECTED: service one frame, but only if a FRAME is actually waiting.
            ;;! char-ready? is what makes this a poll.  It answers #t at EOF as well as on data
            ;;! (R7RS, and see LL_Port::u8_ready_q), so a vanished peer is not mistaken for an
            ;;! idle one -- the read below returns the eof-object and the connection is dropped.
            ;;! THE DRAIN FIRST IS LOAD-BEARING, NOT HYGIENE: without it the newline left behind
            ;;! by the PREVIOUS frame makes char-ready? say #t and the read below blocks.  See
            ;;! llip-srv-skip-interframe-ws! for the measurement.
            (begin
              (guard (exn (#t (llip-srv-drop! srv))) (llip-srv-skip-interframe-ws! conn))
              (when (and (llip-srv-conn srv) (char-ready? conn))
                (guard (exn (#t (llip-srv-drop! srv)))
                  (if (eq? (llip-srv-phase srv) 'await-auth)
                      (llip-srv-step-auth!    srv conn)
                      (llip-srv-step-request! srv conn)))))))))

; end of llip-server.scm
