;;; Copyright 2026 by Frobenius Norm LLC 2026-09-23
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; hud-car.scm -- the VEHICLE half of the mini-HUD: camera frames and telemetry to a panel.
;;;
;;; The panel half is demos/hud-panel.scm.  This side runs on the car, which is the only board
;;; carrying both the camera and the sonar, and pushes two kinds of record over one connection:
;;;
;;;   (hud-telemetry SONAR-MM LEFT RIGHT)   one line, no payload
;;;   (vision-frame SEQ LEN FMT) + LEN raw bytes   -- the codec in features/llip-vision.scm
;;;
;;; Both are line-framed s-expressions, so the receiver dispatches on the first element and a
;;; reader that does not understand one record can still find the start of the next.
;;;
;;; TELEMETRY IS SENT BEFORE THE FRAME IT DESCRIBES.  A distance and a picture taken a frame apart
;;; disagree while the vehicle is moving, and a HUD that shows them together is asserting they
;;; belong to the same instant.  Sending the reading first makes it the reading that was true when
;;; the shutter opened, not one sampled after the image had already been encoded and sent.
;;;
;;; SONAR IS SENT IN MILLIMETRES AS AN INTEGER.  `Sonar.latest` is a float in metres; a float
;;; crossing the wire costs its printed representation and arrives needing parsing on a board that
;;; is busy scanning a display.  Millimetres as an exact integer is the same information, and the
;;; panel can render it without touching the numeric tower.

(syslog "Loading HUD car\n")

;;; THE CODEC MUST BE LOADED HERE, AND ITS ABSENCE MUST BE ANNOUNCED.
;;;
;;; This file calls `llip-vision-capture-send!`, which lives in features/llip-vision.scm.  BEING IN
;;; THE MANIFEST IS NOT ENOUGH: the manifest puts the file on the filesystem, and nothing loads it.
;;; Without this the symbol is unbound, the FIRST frame raises, and `hud-car-run` aborts having
;;; already connected and logged "streaming to ..." -- so the vehicle looks healthy from every angle
;;; and the panel reports `frames 0 bad 0`, which is exactly what a correct receiver shows when
;;; nobody is sending.  Two zeroes, opposite causes, no way to tell them apart from the bench.
;;;
;;; This is the panel half's defect in mirror image; that side was fixed to load the codec AND to say
;;; so when the file is missing, and this side was left as it was.  When one end of a protocol is
;;; repaired, check the other -- the two halves share a failure mode, not just a wire format.
(if (file-exists? "llip-vision.scm") (load "llip-vision.scm" 0) #f)
(if (defined? 'llip-vision-capture-send!)
    #t
    (warn "HUD car: llip-vision.scm absent -- telemetry will work, camera frames CANNOT\n"))

(define (hud-car-port)     (setting 'hud_port 8082))
(define (hud-car-host)     (setting 'hud_host "10.42.1.10"))
(define (hud-car-frame-w)  (setting 'hud_frame_w 240))
(define (hud-car-frame-h)  (setting 'hud_frame_h 240))

;;; Sonar, as an exact integer count of millimetres.  `Sonar.latest` is maintained by `Sonar.loop`;
;;; this reads it rather than pinging, so it never competes with the obstacle reflex for the sensor.
;;; Returns -1 when there is no sonar on this board, which the panel renders as "--" rather than as
;;; a distance -- an absent sensor and a clear path must not look the same.
(define (hud-car-sonar-mm)
  (if (defined? 'Sonar.latest)
      (exact (round (* 1000 Sonar.latest)))
      -1))

;;; Motor state as two small integers, -100..100 percent.  NO DRIVE DATA -> -101 -101, which is
;;; outside the valid range ON PURPOSE so the panel can render "unknown" rather than "stopped".
;;;
;;; [B785]  THESE READ `Motor.left-pct` / `Motor.right-pct`, WHICH EXIST NOWHERE IN THE TREE, AND
;;; THE `(defined? ...)` GUARD TURNED THAT INTO A CONSTANT 0.  The comment said "absent drive ->
;;; 0 0", so a HUD showing `0 0` looked like a correct reading of a stopped vehicle -- and it would
;;; have kept showing `0 0` at full throttle.  **A frozen plausible value is worse than a missing
;;; one**: zero is the single most believable motor reading there is, because a parked vehicle
;;; really does report it.  Hence the out-of-range sentinel: `0` is a claim, `-101` is an admission.
;;;
;;; The real source is `Motor.last` (scm/core/PCA9685.scm:113) -- a 4-vector of the last COMMANDED
;;; value per motor, -1..1, or the symbol `unset` before anything has been commanded.
;;;
;;; MOTOR NUMBERING IS DOCUMENTED AT `MotorPins` IN scm/core/PCA9685.scm, WHICH IS THE AUTHORITY:
;;; clockwise from front left, so 0 fl, 1 fr, 2 rr, 3 rl, and LEFT is {0,3} / RIGHT is {1,2}.  The
;;; defaults below take the FRONT motor of each side.  Deliberately a POINTER and not a restatement
;;; -- a second copy of a mapping is a second thing to drift, and this file is not where a rewired
;;; chassis would be corrected.  The settings stay overridable for one that is.
;;;
;;; [B785]  THIS WAS NEARLY GOT WRONG IN THE INSTRUCTIVE DIRECTION.  An earlier version of this
;;; comment said the mapping was "not established", reasoning from the SHAPE of the pin table --
;;; `#((14 15) (13 12) (11 10) (8 9))`, where 0 and 3 ascend and 1 and 2 descend.  That pattern
;;; gives the GROUPING and not the ORIENTATION, so it genuinely cannot say which group is left, and
;;; declining to guess a HUD number was correct.  What was not correct was concluding the fact did
;;; not exist: it was two lines above the table being read.  Assert absence by grepping, not by
;;; failing to derive.
;;;
;;; EXPECT L AND R TO READ IDENTICALLY FOR NOW, AND DO NOT READ THAT AS A BROKEN MAPPING.  The only
;;; driver in the tree is `rover-drive!`, which calls `Motor.setAll` -- one value to all four.  The
;;; two numbers diverge when differential steering exists; until then they agree because the DRIVE
;;; code commands them equal, which is a fact about the drive code and not about these accessors.
(define (hud-car-motor-ix ix)
  (if (or (not ix) (not (defined? 'Motor.last)))
      -101
      (let ((v (guard (e (#t 'bad)) (vector-ref Motor.last ix))))
        (if (number? v) (exact (round (* 100 v))) -101))))

(define (hud-car-motor-l) (hud-car-motor-ix (setting 'hud_motor_left_ix  0)))   ;;;!< front-left
(define (hud-car-motor-r) (hud-car-motor-ix (setting 'hud_motor_right_ix 1)))   ;;;!< front-right

;;; Write one telemetry record.  Cheap enough to send with every frame: one short line.
(define (hud-car-send-telemetry! port)
  (write (list 'hud-telemetry (hud-car-sonar-mm) (hud-car-motor-l) (hud-car-motor-r)) port)
  (write-string "\n" port)
  (flush-output-port port))

;;; ---------------------------------------------------------------------------------------------
;;; [B690] THE LINK IS STATE, AND IT IS OWNED BY A TICK -- NOT BY ONE BLOCKING CALL.
;;;
;;; This used to be a single `hud-car-run` that opened ONE connection and looped until its deadline,
;;; never asking whether the far end was still there.  When the panel hung up ([B689] drops the link
;;; if one frame body misses its 500 ms deadline) the vehicle noticed nothing: it did not reconnect,
;;; did not log, and did not return -- it held its REPL for the remaining ~57 minutes of its run
;;; while sending into a dead socket.  The panel showed the LAST DECODED FRAME as a still image,
;;; which looks exactly like a live picture of a stationary scene, and both boards looked healthy
;;; from every angle.  It took a frame counter on the glass to tell the two apart.
;;;
;;; EITHER DEFECT ALONE IS SURVIVABLE -- a receiver that drops a bad link is correct, and a sender
;;; that reconnects makes a drop cost one gap.  Together they turn one late frame body into a dead
;;; HUD for the rest of the session.  This is the sender half.
;;;
;;; THE SHAPE IS `wifi-tick!`'s, DELIBERATELY: the entry named it as the thing to copy, and it is
;;; the same problem -- an asynchronous link that can drop at any time, on a board nobody can reach
;;; once it stops answering.  Non-blocking, cheap, everything behind a time gate, backoff on retry,
;;; and a message on the DOWN edge because a link that dropped and a link that never came up look
;;; identical from outside.

(define hud-car-conn       #f)   ;;;!< live port, or #f when disconnected
(define hud-car-seq        0)    ;;;!< frames sent across ALL connections this session
(define hud-car-drops      0)    ;;;!< how many times the link went away -- the number worth seeing
(define hud-car-was-up     #f)   ;;;!< for down-edge logging
(define hud-car-last-try   0)    ;;;!< millis of the last connect attempt
(define hud-car-backoff    0)    ;;;!< current retry interval, doubling to hud_retry_max_ms
(define hud-car-last-frame 0)    ;;;!< millis of the last frame sent, for the rate gate
(define hud-car-cam-ready  #f)   ;;;!< camera-init is once per boot, NOT once per connection
(define hud-car-acks       0)    ;;;!< [B690] acks seen on THIS connection
(define hud-car-last-ack   0)    ;;;!< millis of the most recent ack, for the overdue deadline
(define hud-car-rxbuf      "")   ;;;!< [P262] partial return line carried across ticks
(define hud-car-vision     #f)   ;;;!< most recent (vision-id ...) record, or #f
(define hud-car-vision-n   0)    ;;;!< how many have arrived this session
;;; THE LATEST RECORD PER GROUP, BESIDE THE LATEST RECORD OVERALL.  One frame can produce several
;;; records -- the recogniser returns one line per detection -- and a tick drains all of them, so a
;;; single slot keeps only whichever arrived last.  A frame holding an animal AND a whole-frame
;;; artefact therefore presents as the artefact and the animal is lost, silently, in exactly the
;;; case a recogniser exists to catch.  Three slots is the whole fix: the detector emits three
;;; classes and no more, so this cannot grow with traffic.
(define hud-car-vision-animal  #f)
(define hud-car-vision-person  #f)
(define hud-car-vision-vehicle #f)

;;; ELAPSED, WRAP-TOLERANT -- the same reasoning as `wifi-since`, and it is not theoretical here
;;; either: `millis` is a 32-bit millisecond counter that wraps after ~49 days, a bare subtraction
;;; goes NEGATIVE across the wrap, and a retry gated on `>=` would then wait out the whole counter.
;;; A vehicle would stop reconnecting exactly once every seven weeks.  Treat negative as "due now".
(define (hud-car-since then)
  (let ((d (- (millis) then))) (if (negative? d) 999999 d)))

;;; Drop the link and say so ONCE.  Safe to call when already disconnected.
(define (hud-car-disconnect! why)
  (when hud-car-conn
    (guard (e (#t #f)) (close-port hud-car-conn))
    (set! hud-car-conn #f)
    (set! hud-car-drops (+ hud-car-drops 1))
    (set! hud-car-last-try (millis))
    (when hud-car-was-up
      (warn "HUD car: link LOST (~a) after ~a frame(s), drop ~a -- retrying\n"
            why hud-car-seq hud-car-drops))
    (set! hud-car-was-up #f)))

;;; Try to connect, behind the backoff gate.  Returns #t if the link is up on return.
;;;
;;; `open-tcp-client-port` RETURNS #f ON FAILURE and that is load-bearing here -- it did not always.
;;; Until [B474] every opener returned a truthy port wrapping a dead socket, so `(if conn ...)`
;;; passed with nothing listening and writes vanished.  A reconnect loop built on the old behaviour
;;; would "succeed" instantly, forever, and spin frames into nowhere.
(define (hud-car-connect!)
  (if hud-car-conn
      #t
      (if (< (hud-car-since hud-car-last-try) hud-car-backoff)
          #f                                   ;;;!< still backing off: cheap, no radio work
          (begin
            (set! hud-car-last-try (millis))
            (let ((c (guard (e (#t #f))
                            (open-tcp-client-port (hud-car-host) (hud-car-port)))))
              (if (not c)
                  (begin
                    ;; BACK OFF.  A vehicle that cannot reach the panel is doing real radio work on
                    ;; every attempt, and it shares a board with a camera; retrying flat out is
                    ;; sustained interference rather than an occasional cost.  Same argument as
                    ;; `wifi-tick!`'s, which measured it against a display on the receiving end.
                    (set! hud-car-backoff
                          (min (setting 'hud_retry_max_ms 60000)
                               (max (setting 'hud_retry_ms 2000) (* 2 hud-car-backoff))))
                    #f)
                  (begin
                    (set! hud-car-conn c)
                    (set! hud-car-backoff (setting 'hud_retry_ms 2000))  ;;;!< next drop recovers fast
                    (set! hud-car-was-up #t)
                    ;; ONCE PER BOOT, NOT ONCE PER CONNECTION.  Re-initialising the camera on every
                    ;; reconnect would make a flapping link reset the sensor repeatedly, and the
                    ;; mode is a property of the CAMERA, not of the socket.
                    ;; JPEG, NOT RGB565, AND THE LABEL DEPENDS ON IT: `llip-vision-capture-send!`
                    ;; writes 'jpeg into every frame header regardless of what the sensor produces,
                    ;; so a camera left in rgb565 puts 115,200 raw bytes on the wire under a header
                    ;; that says JPEG.  The receiver hands that to a JPEG decoder, which rejects it,
                    ;; and counts the frame bad -- a frame that arrived perfectly, discarded because
                    ;; its label was wrong.  It is also ~5 KB instead of ~115 KB, which over this
                    ;; radio is the difference between video and a slideshow.
                    ;; [P260] THE CAPTURE SIZE IS A SETTING, not a constant.  `cam_size` is one
                    ;; of qqvga qvga 240 hvga vga svga xga uxga and reaches `camera-init` as its
                    ;; second argument.  Measured on this vehicle, 10 frames each, capture only:
                    ;;     240x240  5,366 B  20.6 ms   QVGA  5,723 B  25.2 ms
                    ;;     VGA     15,818 B  50.2 ms   SVGA 25,799 B  50.3 ms
                    ;; Capture is NOT the limit -- SVGA runs at 20 fps against a link that carries
                    ;; about 3.
                    ;;
                    ;; *** THIS COMMENT USED TO END "QVGA is 33% more pixels for 6.6% more bytes
                    ;; ... and strictly better for anything that has to RECOGNISE what it is looking
                    ;; at".  THAT WAS AN INFERENCE FROM BYTES AND LATENCY, AND THE RECOGNISER
                    ;; REFUTED IT (measured 2026-10-14, MegaDetector v5a, captured NATIVELY at each
                    ;; size -- never resampled, which invalidated an earlier sweep):
                    ;;     vga 0.27   svga 0.38   uxga 0.55   QVGA: NO DETECTION AT ALL
                    ;; Not a weaker hit -- nothing.  So QVGA is below this detector's floor for a
                    ;; framed animal face, and "cheap enough to be strictly better" was the wrong
                    ;; axis: the cost that matters is whether the frame can be recognised. ***
                    ;;
                    ;; THE DEFAULT STAYS QVGA HERE, ON PURPOSE.  This file drives a MOVING vehicle,
                    ;; which needs frames faster than it needs them recognisable -- the obstacle
                    ;; reflex is local and does not consult the network.  A board that does not have
                    ;; to steer sets `cam_size` high instead; see demos/watcher.scm.
                    ;;
                    ;; ONE KEY, NOT TWO, AND THAT IS WHAT MAKES THIS SAFE.  `cam_size` is the same
                    ;; key the board's boot setup reads, so a board that brought the camera up at
                    ;; that size hits `camera-init`'s "already up this way" path and NOTHING IS
                    ;; REINITIALISED.  It was `hud_cam_size` -- a second name for one property, set
                    ;; nowhere and read only here -- so the demo silently DOWNGRADED a board that had
                    ;; booted at svga or uxga back to QVGA, i.e. below the detection floor above,
                    ;; and paid a deinit + re-init of the sensor to do it.  A size change frees the
                    ;; contiguous internal-DMA line buffer and has to win a wider one back from a
                    ;; fragmented heap ([B608]), so the downgrade also carried the risk of losing the
                    ;; camera outright.  Two names for one value is how they came to disagree.
                    (unless hud-car-cam-ready
                      (guard (e (#t #f)) (camera-init 'jpeg (setting 'cam_size 'qvga)))
                      (set! hud-car-cam-ready #t))
                    ;; [B690] PER-CONNECTION, NOT PER-SESSION.  `hud-car-seq` deliberately counts
                    ;; across all connections, but the ack state must NOT: carrying a stale
                    ;; `hud-car-last-ack` into a fresh link makes the deadline fire immediately on a
                    ;; connection that has had no chance to say anything, and carrying a stale
                    ;; `hud-car-acks > 0` arms the deadline against a peer that may never ack.
                    (set! hud-car-acks 0)
                    (set! hud-car-last-ack (millis))
                    (syslog "HUD car: streaming to ~a:~a\n" (hud-car-host) (hud-car-port))
                    #t))))))) 

;;; (hud-car-tick!) -- call from an application loop.  Non-blocking and cheap.
;;;
;;; IT MUST NOT BLOCK, which is the half of [B690] that is NOT about reconnecting.  The old blocking
;;; run held the REPL for its whole duration, so the board could not be asked anything and could not
;;; be told to stop -- `neteval` connected and returned no output, and recovery meant a serial reset,
;;; which reboots the board and destroys the state being investigated.  A sender that blocks for an
;;; hour with no way in is its own hazard, independent of the reconnect question.
;;;
;;; A FAILED CAPTURE IS NOT A FAILED LINK.  `llip-vision-capture-send!` returns #f when the camera
;;; yields no frame, which happens transiently under load; dropping the connection on one missing
;;; frame would turn a hiccup into a dead HUD.  Only a RAISE is treated as the link going away.
;;;
;;; WHAT THIS CANNOT DO, STATED SO NOBODY RELIES ON IT: a TCP write to a socket the peer has closed
;;; is buffered locally and succeeds, so the drop surfaces on a LATER write, not the one after the
;;; close.  Expect to lose a frame or two before the reconnect starts.  That is a property of TCP,
;;; not something this code can tighten -- the alternative is an application-level ack, which costs
;;; a round trip per frame on the link that is already the bottleneck.
;;; [B690] DRAIN THE ACK CHANNEL -- a POSITIVE liveness signal, which is the one thing a sender
;;; cannot manufacture for itself.  A TCP write to a peer that has stopped reading is buffered
;;; locally and SUCCEEDS, so "no error occurred" is not evidence of a live link and cannot be made
;;; into evidence; only something arriving FROM the far end is.  Measured on this pair 2026-10-04:
;;; 32 frames sent, 2 received, zero errors raised.
;;;
;;; NON-BLOCKING BY CONSTRUCTION.  `u8-ready?` gates every read, so this never waits -- `hud-car-tick!`
;;; is called from a cooperative loop beside `(wifi-tick!)` and a blocking read here would stall the
;;; whole board, which is the failure the tick shape exists to avoid.  The 512-byte cap bounds the
;;; drain so a chatty peer cannot hold the tick either.
;;;
;;; EOF IS THE OTHER HALF, AND IT IS FREE HERE.  `read-u8` returning an eof-object means the peer
;;; hung up cleanly -- B690's candidate (2) -- so one drain gets both the ack signal and the clean
;;; FIN, which are different failures: a receiver that DIED announces itself, a receiver that merely
;;; STOPPED CONSUMING does not, and only the ack catches the second.
(define hud-car-ack-marker "vision-ack")

;;; One literal scan, used for both markers.  A second hand-written copy of this loop is how the
;;; two would drift apart.
(define (hud-car-contains? s sub)
  (let ((n (string-length s)) (m (string-length sub)))
    (let loop ((i 0))
      (cond ((> (+ i m) n) #f)
            ((string=? (substring s i (+ i m)) sub) #t)
            (else (loop (+ i 1)))))))

(define (hud-car-ack-line? s)
  ;; A literal scan, because there is no string-search in the core and this needs none: the only
  ;; thing the far end sends is `(vision-ack SEQ BYTES)`, so the marker is enough.
  ;;
  ;; THE LENGTH IS DERIVED FROM THE MARKER, NOT TYPED.  The first version hard-coded 11 for a
  ;; 10-character string, so `substring` produced an 11-char slice that could never equal it and
  ;; this predicate returned #f for EVERY ack.  Nothing else looked wrong -- the file loaded, all
  ;; three procedures were defined, the parens balanced -- and the consequence would have been the
  ;; worst available: `hud-car-acks` stays 0, so the overdue deadline never arms, so [B690] reads as
  ;; fixed while the mechanism is dead.  A detector that cannot fire is indistinguishable from a
  ;; link that never fails.  Caught only by asking it about a string it SHOULD match.
  (hud-car-contains? s hud-car-ack-marker))

;;; [P262] THE RETURN CHANNEL CARRIES TWO KINDS OF LINE, and the vehicle has to tell them apart:
;;;   (vision-ack SEQ BYTES)                     -- liveness and flow, from [B690]
;;;   (vision-id GROUP "name" CONF X Y W H)      -- what the recogniser SAW
;;; Both arrive on the SAME connection the frames go out on, interleaved with nothing else.
;;;
;;; A RESULT COUNTS AS LIVENESS TOO.  A `vision-id` is the far end speaking just as much as an ack
;;; is, so it refreshes the overdue deadline.  Without that, a receiver busy enough to answer with
;;; detections instead of acks would be torn down for being quiet -- punished for doing the work.
;;;
;;; LINES ARE REASSEMBLED ACROSS TICKS.  A tick reads whatever has arrived, which is NOT a whole
;;; number of lines: TCP splits wherever it likes, and `hud-car-rxbuf` carries the tail into the
;;; next tick.  Parsing what happened to arrive would drop every record that straddled a read.
(define (hud-car-handle-line! line)
  (cond
    ((hud-car-ack-line? line)
     (set! hud-car-acks (+ hud-car-acks 1))
     (set! hud-car-last-ack (millis)))
    ((hud-car-visionid-line? line)
     ;; GUARDED: a truncated or malformed record must not take the vehicle down.  One bad line is
     ;; not a reason to stop recognising, and `read` raises on anything it cannot parse.
     (guard (e (#t #f))
       (let ((rec (read (open-input-string line))))
         (when (pair? rec)
           (set! hud-car-vision rec)
           (set! hud-car-vision-n (+ hud-car-vision-n 1))
           (hud-car-file-by-group! rec)
           (set! hud-car-last-ack (millis))))))
    (else #f)))

(define (hud-car-drain-acks!)
  (when hud-car-conn
    (let loop ((n 0) (buf (list)))
      (if (and (< n 512) (u8-ready? hud-car-conn))
          (let ((b (read-u8 hud-car-conn)))
            (if (eof-object? b)
                (hud-car-disconnect! "peer closed the connection (EOF)")
                (loop (+ n 1) (cons (integer->char b) buf))))
          (when (> n 0)
            (set! hud-car-rxbuf (string-append hud-car-rxbuf (list->string (reverse buf))))
            (hud-car-split-lines!))))))

;;; Pull every COMPLETE line out of the buffer and hand it to the handler; leave the tail behind.
(define (hud-car-split-lines!)
  (let loop ()
    (let ((i (hud-car-index-of hud-car-rxbuf #\newline)))
      (when i
        (let ((line (substring hud-car-rxbuf 0 i)))
          (set! hud-car-rxbuf (substring hud-car-rxbuf (+ i 1) (string-length hud-car-rxbuf)))
          (hud-car-handle-line! line)
          (loop))))))

(define (hud-car-index-of s ch)
  (let ((n (string-length s)))
    (let loop ((i 0))
      (cond ((>= i n) #f)
            ((char=? (string-ref s i) ch) i)
            (else (loop (+ i 1)))))))

(define (hud-car-visionid-line? s) (hud-car-contains? s "vision-id"))

;;; --- the vehicle's view of what the recogniser saw --------------------------------------------
;;; `vision-poll` CONSUMES: it hands back the latest record and clears it, so a caller cannot act
;;; twice on one sighting.  A behaviour that steers on a stale detection is worse than one that
;;; waits, and "is there something new" is the question a control loop actually asks.
(define (vision-pending?) (if hud-car-vision #t #f))
(define (vision-poll)
  (let ((v hud-car-vision))
    (set! hud-car-vision #f)
    v))

;;; THE PER-GROUP CHANNEL.  Same CONSUMING contract as `vision-poll` -- a caller cannot act twice
;;; on one sighting -- but asked one group at a time, so a reading for `animal` is never displaced
;;; by a `vehicle` record that happened to arrive in the same tick.
;;;
;;; A GROUP THIS DOES NOT KNOW IS DROPPED FROM THE PER-GROUP VIEW AND KEPT IN `hud-car-vision`,
;;; which is the conservative half: a reader asking by name sees only names it asked for, and
;;; nothing is lost from the general channel.  The three names are MegaDetector v5a's whole class
;;; list; a different model means a different list here.
;;; FILE BY GROUP -- AND ANYTHING THAT IS NOT A PERSON OR A VEHICLE IS AN ANIMAL.
;;;
;;; [P260] stage two names the species, and the record then carries the PANEL's silhouette group --
;;; `deer`, `canid`, `raptor`, `rodent`, `mustelid`, `bear`, `wading`, `turtle`, `amphibian` (see
;;; hud-species.scm) -- where the detector alone would have said `animal`.  This used to test for
;;; `animal` by name and drop everything else on the `else` branch, so the FIRST species record
;;; would have been filed nowhere: `vision-poll-group 'animal` returns #f forever, `watcher-tick!`
;;; sees no sightings, and a night's report reads `animal records 0` while the records stream past.
;;; Nothing anywhere would have said so -- the line arrives, parses, increments `hud-car-vision-n`
;;; and is then discarded one line later.
;;;
;;; THE FALLBACK IS THE DETECTOR'S OWN TAXONOMY, NOT A CATCH-ALL.  MegaDetector reports exactly
;;; three things and two of them are `person` and `vehicle`; everything else it can report IS an
;;; animal.  So a group added to the panel tomorrow needs no change here, which is the property
;;; this file wants given it ships to boards that are not reflashed often.
;;;
;;; A RECORD WITH NO GROUP IS STILL DROPPED.  `vision-group` returns #f for a truncated record, and
;;; filing that as an animal would turn a parse failure into a sighting.
(define (hud-car-file-by-group! rec)
  (let ((g (vision-group rec)))
    (cond ((eq? g 'person)  (set! hud-car-vision-person  rec))
          ((eq? g 'vehicle) (set! hud-car-vision-vehicle rec))
          ((symbol? g)      (set! hud-car-vision-animal  rec))
          (else #f))))

(define (vision-poll-group g)
  (cond ((eq? g 'animal)  (let ((v hud-car-vision-animal))  (set! hud-car-vision-animal  #f) v))
        ((eq? g 'person)  (let ((v hud-car-vision-person))  (set! hud-car-vision-person  #f) v))
        ((eq? g 'vehicle) (let ((v hud-car-vision-vehicle)) (set! hud-car-vision-vehicle #f) v))
        (else #f)))

;;; Accessors, so a caller does not hard-code field positions of the wire record
;;; `(vision-id GROUP "name" CONFIDENCE X Y W H)`.
(define (vision-group  v) (if (and (pair? v) (pair? (cdr v))) (cadr v) #f))
(define (vision-name   v) (if (and (pair? v) (>= (length v) 3)) (caddr v) #f))
(define (vision-conf   v) (if (and (pair? v) (>= (length v) 4)) (cadddr v) 0))
(define (vision-box    v) (if (and (pair? v) (>= (length v) 8)) (list-tail v 4) #f))

(define (hud-car-tick!)
  (when (hud-car-connect!)
    (when (>= (hud-car-since hud-car-last-frame) (setting 'hud_frame_ms 200))
      (set! hud-car-last-frame (millis))
      (let* ((t0 (millis))
             (r  (guard (e (#t 'raised))
                        (hud-car-send-telemetry! hud-car-conn)
                        (llip-vision-capture-send! hud-car-conn hud-car-seq)
                        'sent))
             (dt (- (millis) t0)))
        (cond
          ;; A RAISE IS THE EASY CASE AND THE RARE ONE.  Kept first because when it does fire it is
          ;; unambiguous, but do not expect it: see the stall arm below for why.
          ((eq? r 'raised) (hud-car-disconnect! "write raised"))
          ;; THE STALL IS THE REAL DETECTOR, AND IT EXISTS BECAUSE THE RAISE DOES NOT FIRE.
          ;; [B690] Measured 2026-10-04 on this pair: a write to a socket the panel had stopped
          ;; draining took 14 ms, then 2020 ms, then 10017 ms on three successive frames -- rising,
          ;; and those are TIMEOUT values, not transfer times.  The sender raised NOTHING, logged
          ;; `streaming` throughout, and put 33 of 35 frames into a socket the panel never read:
          ;; `hud-frames` stood at 2 with `hud-bad` 0, so they were not corrupt, they never arrived.
          ;; A TCP write to a peer that has stopped reading is buffered locally and SUCCEEDS, so
          ;; "no error occurred" is not evidence of a live link and cannot be made into evidence.
          ;; DURATION is the one signal a sender gets for free: at `hud_frame_ms` 200 a healthy
          ;; frame costs ~14 ms, so a write past `hud_write_stall_ms` is not slow, it is stuck.
          ;; This is still one-sided and therefore still a HEURISTIC -- a genuinely slow link on a
          ;; bad radio day will trip it and reconnect, which costs a gap and is the cheaper error.
          ;; The principled fix is an ACK from the panel, which would also measure the receiver's
          ;; true rate instead of inferring it; that is a protocol change and is noted in [B690].
          ((>= dt (setting 'hud_write_stall_ms 1500))
           (hud-car-disconnect! (string-append "write stalled " (number->string dt) " ms")))
          (else (set! hud-car-seq (+ hud-car-seq 1))))))
    ;; [B690] AFTER the send, so an ack for the frame just written can land on this same tick.
    (hud-car-drain-acks!)
    ;; THE DEADLINE IS ONLY ENFORCED AGAINST A PEER THAT HAS ACKED AT LEAST ONCE, and that guard is
    ;; load-bearing rather than defensive: the panel does not implement acks at all, so an
    ;; unconditional deadline would tear down every healthy board-to-board link once per interval --
    ;; a detector that breaks the working case to catch the broken one.  A peer that never acks is
    ;; simply never judged by acks, and the write-stall heuristic above still covers it.
    (when (and hud-car-conn
               (> hud-car-acks 0)
               (>= (hud-car-since hud-car-last-ack) (setting 'hud_ack_overdue_ms 5000)))
      (hud-car-disconnect!
        (string-append "ack overdue " (number->string (hud-car-since hud-car-last-ack)) " ms")))))

;;; (hud-car-run SECS) -- stream for SECS seconds, reconnecting as needed.  Returns
;;; (frames-sent drops seconds).
;;;
;;; THIS STILL BLOCKS, AND THAT IS NOW A CHOICE RATHER THAN THE ONLY OPTION.  It is a bounded driver
;;; over `hud-car-tick!`, kept because it is what a bench session types; a board that must stay
;;; answerable while streaming calls the TICK from its own loop instead, beside `(wifi-tick!)`.
;;; What it no longer does is treat the first drop as the end of the session.
(define (hud-car-run secs)
  (let ((t-end (+ (millis) (* secs 1000)))
        (seq0  hud-car-seq)
        (drop0 hud-car-drops))
    (let loop ()
      (if (>= (millis) t-end)
          (begin
            (hud-car-disconnect! "run complete")
            (syslog "HUD car: done, ~a frame(s), ~a drop(s)\n"
                    (- hud-car-seq seq0) (- hud-car-drops drop0))
            (list (- hud-car-seq seq0) (- hud-car-drops drop0) secs))
          (begin
            (hud-car-tick!)
            (delay-ms 20)                      ;;;!< yield; the rate gate lives in the tick
            (loop))))))

(syslog "HUD car loaded\n")
