;;; vision-server.scm -- the appliance half: accept camera frames over LLIP, recognise them, and
;;; send the result back on the same connection.
;;; Copyright 2026 by Frobenius Norm LLC 2026-10-07 14:30:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; THE VEHICLE CARRIES THE CAMERA; THIS CARRIES THE MODEL.  A frame arrives as
;;; `(vision-frame SEQ LEN FMT)` followed by LEN raw bytes; a result goes back as
;;; `(vision-id GROUP "name" CONFIDENCE X Y W H)` -- the record the vehicle parses and the panel
;;; renders.  The vehicle never holds the weights, which is what lets a 4 MB board take part.
;;;
;;; IT RUNS ON A TARGET WITH `vision-detect`, i.e. one built with TensorRT.  Everything else about
;;; it is ordinary LambLisp, so the only thing that makes this an appliance is the mop3 underneath.
;;;
;;; THE ENGINE IS LOADED ONCE, BEFORE THE LISTENER OPENS.  Deserialisation costs seconds and the
;;; engine is hundreds of megabytes; loading it per frame would dominate every measurement and
;;; exhaust a shared GPU.  Loading it after accepting would drop the opening frames and present a
;;; MODEL failure as a TRANSPORT one, which is the harder of the two to diagnose.

(syslog "vision-server loading\n")

(define vision-server-conf 0.25)        ;;;!< detection threshold; below this a hit is not reported
(define vision-server-frames 0)         ;;;!< frames read this session
(define vision-server-detframes 0)      ;;;!< frames the recogniser actually ran on
;;; Telemetry and unparsed lines are counted SEPARATELY from frames, because "it sent nothing"
;;; and "it sent plenty and none of it was a frame" are different faults with different fixes.
(define vision-server-tlines 0)    ;;;!< (hud-telemetry ...) lines seen
(define vision-server-skipped 0)   ;;;!< lines we could not parse at all
(define vision-server-telemetry #f);;;!< most recent telemetry fields, or #f
(define vision-server-hits 0)           ;;;!< detections returned

;;; COUNTED SEPARATELY FROM `frames`, and that distinction is the whole instrument: `detframes`
;;; equal to `frames` with `hits` 0 means the recogniser RAN and the scene held nothing -- a result
;;; about the world.  `detframes` SHORT of `frames` means it did not run -- a fault in the
;;; recogniser.  One aggregate count cannot tell those apart, and "0 detections" alone reads like a
;;; broken model when it is usually an empty field of view.

;;; Send every detection for one frame, then an ack.
;;;
;;; THE ACK GOES EVEN WHEN NOTHING WAS SEEN, and that is not symmetry for its own sake: a sender
;;; cannot distinguish "received and found nothing" from "stopped listening" by silence.  A TCP
;;; write to a peer that has stopped reading is buffered locally and SUCCEEDS, so the vehicle's only
;;; evidence that this end is alive is this end speaking.  An empty scene is the common case, so a
;;; design that is silent when it sees nothing is silent nearly always.
(define (vision-server-reply! conn seq hits nbytes)
  (for-each
    (lambda (h)
      (write (cons 'vision-id h) conn)
      (write-string "\n" conn))
    hits)
  (write (list 'vision-ack seq nbytes) conn)
  (write-string "\n" conn)
  (flush-output-port conn))

;;; One connection, served to completion.  Returns when the peer closes or the deadline passes.
(define (vision-server-session conn t-end)
  (let loop ()
    (if (>= (millis) t-end)
        'deadline
        (let ((f (llip-vision-read conn)))
          (cond
            ;; THE PEER IS DONE, OR THE STREAM CAN NO LONGER BE FRAMED.  Both end the session and
            ;; only these two do.  Keeping them apart matters on the way out: 'eof is the normal
            ;; end of a run, 'desync means a frame body came up short and anything still in the
            ;; socket is at an unknown offset.
            ((eq? f 'eof)    'eof)
            ((eq? f 'desync) 'desync)
            ;; NOT EVERY LINE IS A FRAME, AND THAT IS THE NORMAL CASE, NOT AN ERROR.  The 4WD
            ;; writes `(hud-telemetry SONAR L R)` before every frame.  Reading that as a hang-up
            ;; is what made this server close on the first line of a healthy connection and
            ;; report `frames 0` -- indistinguishable from a camera that sent nothing.
            ((eq? f 'malformed) (set! vision-server-skipped (+ vision-server-skipped 1)) (loop))
            ((and (pair? f) (eq? 'telemetry (car f)))
             (set! vision-server-telemetry (cdr f))
             (set! vision-server-tlines (+ vision-server-tlines 1))
             (loop))
            (else
             (let* ((fr  (cdr f))                       ;;;!< (SEQ LEN FMT BV)
                    (seq (car fr))
                    (len (cadr fr))
                    (bv  (cadddr fr)))
               (set! vision-server-frames (+ vision-server-frames 1))
               ;; A BAD FRAME IS NOT A REASON TO STOP SERVING.  One undecodable JPEG in a stream
               ;; must not end the session -- the vehicle is still sending and still waiting.
               (let ((hits (guard (e (#t (list)))
                             (let ((r (vision-detect bv vision-server-conf)))
                               (set! vision-server-detframes (+ vision-server-detframes 1))
                               r))))
                 (set! vision-server-hits (+ vision-server-hits (length hits)))
                 (vision-server-reply! conn seq hits len)
                 (when (pair? hits)
                   (syslog "vision-server: seq ~a -> ~a\n" seq (car hits))))
               (loop))))))))

(define (vision-server-run port engine secs . conf-arg)
  (when (pair? conf-arg) (set! vision-server-conf (car conf-arg)))
  (set! vision-server-frames 0)
  (set! vision-server-detframes 0)
  (set! vision-server-hits 0)
  (set! vision-server-tlines 0)
  (set! vision-server-skipped 0)
  (if (not (defined? 'vision-detect))
      (begin
        ;; SAY WHICH THING IS MISSING.  "It did not work" on an appliance with a GPU sends someone
        ;; to the model, the engine file and the driver before the build flags.
        (warn "vision-server: this build has no `vision-detect` -- it needs a TensorRT target\n")
        #f)
      (let ((io (vision-engine-load! engine)))
        (syslog "vision-server: engine ~a\n" io)
        (let ((srv (open-tcp-server-port port)))
          (if (not srv)
              (begin (warn "vision-server: cannot listen on ~a\n" port) #f)
              (let ((t-end (+ (millis) (* secs 1000))))
                (syslog "vision-server: listening on ~a for ~a s\n" port secs)
                ;; SERVE UNTIL THE DEADLINE, NOT UNTIL THE FIRST PEER LEAVES.  This used to accept
                ;; ONE connection and then close the listener, so a vehicle that dropped and
                ;; reconnected -- which is the designed behaviour of `hud-car-tick!`, and happens
                ;; on every radio hiccup -- found nothing listening and backed off to 60 s.  The
                ;; session end and the SERVER end are different events; only the clock ends both.
                (let serve ((conns 0))
                  (if (>= (millis) t-end)
                      (begin
                        (close-port srv)
                        (list 'vision-server
                              'conns     conns
                              'frames    vision-server-frames
                              'detframes vision-server-detframes
                              'hits      vision-server-hits
                              'tlines    vision-server-tlines
                              'skipped   vision-server-skipped
                              'conf      vision-server-conf))
                      (let ((conn (server-accept srv)))
                        (if (not conn)
                            (begin (delay-ms 20) (serve conns))
                            (let ((why (vision-server-session conn t-end)))
                              (syslog "vision-server: session ended (~a) after ~a frame(s)\n"
                                      why vision-server-frames)
                              (guard (e (#t #f)) (close-port conn))
                              (serve (+ conns 1)))))))))))))

(syslog "vision-server loaded\n")
