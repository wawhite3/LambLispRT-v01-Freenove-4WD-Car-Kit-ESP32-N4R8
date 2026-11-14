;;; Copyright 2026 by Frobenius Norm LLC 2026-10-14
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; watcher.scm -- [P260] A STATIONARY ANIMAL RECOGNISER.  The camera does not move.
;;;
;;; Same hierarchy as the vehicle (owner, 2026-10-07): "the center of intelligence in the animal
;;; recognition app is the jetson.  esp32 should expect to offload their work quickly to jetson and
;;; receive new instructions."  So this board captures, offloads, and acts on what comes back -- it
;;; holds no model.  The transport is demos/hud-car.scm, unchanged and not reimplemented here: the
;;; frames go out as `(vision-frame SEQ LEN FMT)` + payload and `(vision-id GROUP "name" CONF X Y
;;; W H)` comes back on the same connection.
;;;
;;; WHAT NOT ROVING BUYS, AND IT IS THE WHOLE DESIGN.  A moving vehicle needs frames FASTER than it
;;; needs them RECOGNISABLE -- it has to steer, and its obstacle reflex is local and never consults
;;; the network.  A watcher has no such deadline, so it spends everything on the axis the recogniser
;;; actually cares about.  Measured 2026-10-14 against MegaDetector v5a, the same framed animal
;;; faces, each size captured NATIVELY (an earlier sweep was void because re-encoding alone moved
;;; accuracy more than size did):
;;;
;;;     vga 0.27     svga 0.38     uxga 0.55     QVGA: NO DETECTION AT ALL
;;;
;;; Nothing, not a weak hit.  So the vehicle's QVGA default is BELOW this detector's floor for the
;;; thing this application exists to find, and a watcher's first duty is to ask for a big frame.
;;;
;;; THE SIZE IS A BOOT SETTING -- `cam_size` -- AND THIS FILE CANNOT CHANGE IT.  `camera-init`
;;; treats the size as part of "this mode", so asking for a different one deinits the sensor and has
;;; to win a wider contiguous internal-DMA line buffer back out of a heap user code has already
;;; fragmented ([B608]).  Set `cam_size` in the target's settings overlay and reboot; the board's
;;; setup file reads it at the end of boot, which is the last moment it is safe.  This file only
;;; READS the key, to know how big a frame the box coordinates below are measured in.
;;;
;;; EVENTS, NOT SAMPLES.  An animal in front of a camera produces a detection on every frame for as
;;; long as it stays, and a log of two hundred sightings does not say an animal ARRIVED once.  This
;;; keeps an arrival/departure state machine per group so the record is "animal present 14:22:07 ->
;;; 14:22:41" rather than a tally nobody can read.
;;;
;;; Deps: demos/hud-car.scm (transport + `vision-poll-group`), features/llip-vision.scm (codec,
;;;       loaded by hud-car.scm).  No camera calls of its own.

(syslog "Loading watcher\n")

;;; THE TRANSPORT MUST BE LOADED HERE AND ITS ABSENCE MUST BE ANNOUNCED -- being in the manifest
;;; only puts a file on the filesystem, and nothing loads it.  Without this `hud-car-tick!` is
;;; unbound, `watcher-tick!` raises on its first call, and a watcher that raises on every tick looks
;;; from the outside exactly like a watcher in front of an empty room: no detections, no complaints.
;;; hud-car.scm makes the same argument about the codec it depends on, for the same reason.
(if (file-exists? "hud-car.scm") (load "hud-car.scm" 0) #f)
(if (defined? 'hud-car-tick!)
    #t
    (warn "watcher: hud-car.scm absent -- there is NO transport, nothing will be sent\n"))
;;; The per-group return channel is newer than the rest of hud-car.scm.  An older copy on the
;;; filesystem still has `vision-poll` and not this, and the failure would be silent in the
;;; expensive direction: every group would read as never seen.
(if (defined? 'vision-poll-group)
    #t
    (warn "watcher: hud-car.scm is too old -- no vision-poll-group, detections CANNOT be read\n"))

;;; --- frame geometry ----------------------------------------------------------------------------
;;; THE BOX IN A `vision-id` RECORD IS IN ORIGINAL-FRAME PIXELS, not a fraction.  The appliance
;;; undoes its letterbox and clips to the frame it decoded (jetson_vision_detect.py), so X Y W H are
;;; only interpretable against the size the camera was initialised at -- which is `cam_size`, the
;;; same key the board's setup file read at boot.  Deriving it from that key rather than restating
;;; the dimensions is the point: a second copy of the frame size is a second thing to drift.
(define watcher-frame-dims
  ;; name, width, height -- the names `camera-init` accepts (ll_xmop3_EyeCam.cpp).
  (list (list 'qqvga 160 120)  (list 'qvga 320 240)  (list (string->symbol "240") 240 240)
        (list 'hvga  480 320)  (list 'vga  640 480)  (list 'svga 800 600)
        (list 'xga  1024 768)  (list 'uxga 1600 1200)))

(define (watcher-cam-size) (setting 'cam_size (string->symbol "240")))

;;; Returns (W H), or (0 0) for a size this table does not know.  ZERO RATHER THAN A GUESS: every
;;; consumer below treats a zero frame as "cannot judge geometry" and says so, instead of computing
;;; a plausible percentage against a made-up denominator.
(define (watcher-frame-wh)
  (let ((e (assq (watcher-cam-size) watcher-frame-dims)))
    (if e (cdr e) (list 0 0))))

;;; --- per-group state ---------------------------------------------------------------------------
;;; MegaDetector v5a emits exactly three classes (MD_CLASSES in jetson_vision_detect.py).  Named
;;; here in ITS order so an index is meaningful in both files.
(define watcher-groups (vector 'animal 'person 'vehicle))

;;; THE GROUP ON THE WIRE IS NOT ALWAYS ONE OF THESE THREE, AND A STRANGER IS AN ANIMAL.
;;; [P260] stage two names the species, and the record then carries the PANEL's silhouette group --
;;; `deer`, `canid`, `raptor`, `rodent`, `mustelid`, `bear`, `wading`, `turtle`, `amphibian` (see
;;; hud-species.scm) -- in place of the detector's bare `animal`.  This used to return #f for every
;;; one of those, and a #f index means the record is DROPPED: a night of species records would have
;;; reported `animal records 0` while the sightings streamed past, with nothing saying so.
;;;
;;; THE FALLBACK IS NOT A PATCH, IT IS THE DETECTOR'S OWN TAXONOMY.  MegaDetector knows exactly
;;; three things and two of them are `person` and `vehicle`; everything else it reports IS an
;;; animal.  So an unrecognised group is counted as one rather than discarded, and a group added to
;;; the panel tomorrow needs no change here.
;;;
;;; AND THIS IS NOT THE LIVE DISPATCH, WHICH IS WHY THAT CHANGE ALONE WOULD HAVE BEEN INERT.  The
;;; record never passes through here: `watcher-tick!` POLLS by group (`vision-poll-group`), and the
;;; incoming record is bucketed by `hud-car-file-by-group!` in hud-car.scm.  This function exists
;;; so the slot layout has a name, and its only callers are the tests.  The two must agree anyway
;;; -- a reader who finds this one first will believe it.
(define (watcher-group-index g)
  (let ((n (vector-length watcher-groups)))
    (let loop ((i 0))
      (cond ((>= i n) 0)                   ;;; not person, not vehicle -> the animal slot
            ((eq? g (vector-ref watcher-groups i)) i)
            (else (loop (+ i 1)))))))

;;; One slot per group.  Field order is fixed by these constants and nothing else indexes it.
(define watcher-f-hits     0)   ;;;!< sightings in the current window, reset when the window lapses
(define watcher-f-last     1)   ;;;!< millis of the most recent sighting
(define watcher-f-best     2)   ;;;!< best confidence seen this presence, percent
(define watcher-f-present  3)   ;;;!< #t between a confirmed arrival and its departure
(define watcher-f-arrivals 4)   ;;;!< confirmed arrivals since boot
(define watcher-f-records  5)   ;;;!< every record for this group, accepted or not
(define watcher-f-suspect  6)   ;;;!< records rejected as whole-frame artefacts (see below)
(define watcher-f-since    7)   ;;;!< millis the current presence began
(define watcher-f-width    8)   ;;;!< box of the most recent ACCEPTED record
(define watcher-f-height   9)
(define watcher-n-fields  10)

(define watcher-state
  (let ((v (make-vector (vector-length watcher-groups) #f)))
    (let loop ((i 0))
      (if (>= i (vector-length v))
          v
          (begin (vector-set! v i (make-vector watcher-n-fields 0)) (loop (+ i 1)))))))

(define (watcher-slot i) (vector-ref watcher-state i))

;;; Clear every slot.  An operator starts a fresh watch with it; the tests need it between cases,
;;; and a test that cannot reset its subject is testing the order its cases happen to run in.
(define (watcher-reset!)
  (let loop ((i 0))
    (if (>= i (vector-length watcher-state))
        #t
        (begin (vector-set! watcher-state i (make-vector watcher-n-fields 0)) (loop (+ i 1))))))
(define (watcher-get i f) (vector-ref (watcher-slot i) f))
(define (watcher-set! i f x) (vector-set! (watcher-slot i) f x))

;;; PRESENCE IS ASKED THROUGH THIS, NEVER READ RAW.  A fresh slot is all zeroes, so the field holds
;;; `0` before anything has ever been seen and `#f` after a departure -- both false in Scheme and
;;; NOT `equal?` to each other, which is the kind of difference a test comparing against #f trips
;;; over while the code it is testing is correct.  One predicate, one answer.
(define (watcher-present? i) (eq? #t (watcher-get i watcher-f-present)))

;;; --- tunables ----------------------------------------------------------------------------------
(define (watcher-confirm-n)  (setting 'watcher_confirm_n 2))
(define (watcher-clear-ms)   (setting 'watcher_clear_ms 6000))
(define (watcher-min-conf)   (setting 'watcher_min_conf 30))
(define (watcher-max-area)   (setting 'watcher_max_area_pct 90))
(define (watcher-signal-on)  (setting 'watcher_signal_on 'animal))

;;; --- the whole-frame artefact ------------------------------------------------------------------
;;; A RECORD COUNT IS NOT A DETECTION COUNT, and this is the measurement that proved it [P260].
;;;
;;; FIRST, THE NAME, BECAUSE IT MISLEADS EVERY READER ONCE.  `vehicle` is one of MegaDetector's
;;; three CLASS LABELS (animal / person / vehicle).  When this file says a whole-frame `vehicle`
;;; record, it does NOT mean there is a vehicle -- there is no vehicle, and in a hallway there
;;; could not be.  It means the model drew a box around the entire image and attached that label
;;; to it.  Saying "the vehicle artefact" out loud reads as a claim about the room, which is the
;;; opposite of what the record is worth.
;;; Sweeping frame sizes on 2026-10-13 produced a `vehicle` record on EVERY frame at EVERY size,
;;; with a box covering the entire image, and it got STRONGER with resolution (0.42 -> 0.54, peaking
;;; at 72%).  So "49 records" meant "nothing happened": the strongest, most consistent thing in the
;;; corpus was an artefact of the detector looking at a flat evenly-lit rectangle.
;;;
;;; IT IS CLASSIFIED AND COUNTED, NOT DISCARDED, AND THAT IS DELIBERATE.  A filter that drops a
;;; record leaves nothing behind, so a threshold that is wrong -- or a real animal that genuinely
;;; fills the frame at close range -- becomes indistinguishable from an empty room.  `watcher-report`
;;; prints `suspect` beside `records`, so the two can always be told apart and the default below is
;;; not load-bearing.
(define (watcher-whole-frame? w h)
  (let* ((wh (watcher-frame-wh)) (fw (car wh)) (fh (cadr wh)))
    (cond ((or (= fw 0) (= fh 0)) #f)        ;;;!< unknown frame: cannot judge, so do not claim
          ((or (not (integer? w)) (not (integer? h))) #f)
          (else (>= (* 100 w h) (* (watcher-max-area) fw fh))))))

;;; --- the signal --------------------------------------------------------------------------------
;;; A HOOK, DEFAULTING TO A LOG LINE, BECAUSE THIS BOARD HAS NOTHING TO FLASH.  The WROVER-CAM
;;; breakout has no WS2812 ring, no motors and nothing wired to its buzzer pins, so an arrival can
;;; only be ANNOUNCED.  Rebind `watcher-on-event` on a board that has an indicator; it is called
;;; with (EVENT GROUP CONF) where EVENT is 'arrived or 'departed.  Keep it SHORT: it runs inside the
;;; tick, which must not block.
(define watcher-on-event
  (lambda (ev group conf)
    (syslog "[watcher] ~a ~a conf ~a%\n" ev group conf)))

(define (watcher-emit! ev i conf)
  (let ((g (vector-ref watcher-groups i)))
    (guard (e (#t (warn "[watcher] event hook raised on ~a ~a\n" ev g) #f))
      (watcher-on-event ev g conf))))

;;; --- the tick ----------------------------------------------------------------------------------
;;; NON-BLOCKING, like `hud-car-tick!` which it drives.  [B854] (RELEASE, 2026-10-14) measured that
;;; an open LLIP session freezes the main loop for its whole duration, so anything on this board
;;; that must stay answerable -- the REPL above all, which is where someone asks the board what it
;;; has seen -- depends on no step here waiting.
;;;
;;; THE SIGHTING IS READ PER GROUP, NOT AS "THE LATEST RECORD".  One frame can yield several
;;; records, a tick drains all of them, and a single-slot buffer keeps only the last -- so a frame
;;; containing an animal AND a whole-frame false positive labelled `vehicle` would present as
;;; animal would be lost, silently, exactly in the case this application exists to catch.
(define (watcher-tick!)
  (hud-car-tick!)
  (let ((now (millis)) (n (vector-length watcher-groups)))
    (let loop ((i 0))
      (when (< i n)
        (let ((v (if (defined? 'vision-poll-group) (vision-poll-group (vector-ref watcher-groups i)) #f)))
          (when (pair? v) (watcher-note! i v now))
          (watcher-age! i now))
        (loop (+ i 1))))))

;;; One record for group i.  Every record counts toward `records`; only an ACCEPTED one moves the
;;; state machine.
(define (watcher-note! i v now)
  (watcher-set! i watcher-f-records (+ 1 (watcher-get i watcher-f-records)))
  (let* ((conf (vision-conf v))
         (box  (vision-box v))
         (w    (if (and (pair? box) (>= (length box) 4)) (caddr box) 0))
         (h    (if (and (pair? box) (>= (length box) 4)) (cadddr box) 0)))
    (cond
      ((watcher-whole-frame? w h)
       (watcher-set! i watcher-f-suspect (+ 1 (watcher-get i watcher-f-suspect))))
      ;; BELOW THE CONFIDENCE FLOOR IS NOT A SIGHTING, but it is not an artefact either: it stays in
      ;; `records` and moves nothing, so a scene producing a steady stream of 12% guesses reads as
      ;; records-without-arrivals rather than as silence.
      ((< conf (watcher-min-conf)) #f)
      (else
       (watcher-set! i watcher-f-hits (+ 1 (watcher-get i watcher-f-hits)))
       (watcher-set! i watcher-f-last now)
       (watcher-set! i watcher-f-width w)
       (watcher-set! i watcher-f-height h)
       (when (> conf (watcher-get i watcher-f-best))
         (watcher-set! i watcher-f-best conf))
       ;; CONFIRMATION BEFORE ARRIVAL.  One record is not an arrival: the corpus above shows a
       ;; detector will produce a confident wrong box on a flat wall, and a watcher that announces
       ;; every first record announces those too.  `watcher_confirm_n` records inside one
       ;; `watcher_clear_ms` window is the cheapest available filter that a real animal passes
       ;; trivially -- it is in front of the camera for seconds -- and a one-frame fluke does not.
       (when (and (not (watcher-present? i))
                  (>= (watcher-get i watcher-f-hits) (watcher-confirm-n)))
         (watcher-set! i watcher-f-present #t)
         (watcher-set! i watcher-f-since now)
         (watcher-set! i watcher-f-arrivals (+ 1 (watcher-get i watcher-f-arrivals)))
         (watcher-emit! 'arrived i (watcher-get i watcher-f-best)))))))

;;; Departure is a TIMEOUT, and it has to be: nothing tells a camera that an animal left.  The same
;;; lapse also resets an UNCONFIRMED run, so `watcher_confirm_n` means "n sightings within a window"
;;; rather than "n sightings ever" -- without that, two flukes an hour apart would confirm.
(define (watcher-age! i now)
  (let ((last (watcher-get i watcher-f-last)))
    (when (and (> last 0) (>= (- now last) (watcher-clear-ms)))
      (if (watcher-present? i)
          (begin
            (watcher-set! i watcher-f-present #f)
            (watcher-emit! 'departed i (watcher-get i watcher-f-best))
            (watcher-set! i watcher-f-best 0))
          #f)
      (watcher-set! i watcher-f-hits 0))))

;;; --- reading it back ---------------------------------------------------------------------------
;;; ONE LINE, PARSEABLE, AND IT REPORTS THE TRANSPORT TOO.  `w3 neteval` takes a single short form
;;; and gives back what it printed, so this is what an operator actually types; and the counters are
;;; separate because they fail independently.  `frames` 0 is a transport fault; `frames` healthy with
;;; `records` 0 is a fact about the ROOM; `records` healthy with `arrivals` 0 means everything that
;;; arrived was an artefact or below the floor.  One aggregate number could not tell those apart.
(define (watcher-report)
  (list 'watcher
        'size (watcher-cam-size)
        'frames hud-car-seq
        'drops hud-car-drops
        'linked (if hud-car-conn #t #f)
        'groups
        (let loop ((i (- (vector-length watcher-groups) 1)) (acc (list)))
          (if (< i 0)
              acc
              (loop (- i 1)
                    (cons (list (vector-ref watcher-groups i)
                                'records  (watcher-get i watcher-f-records)
                                'suspect  (watcher-get i watcher-f-suspect)
                                'arrivals (watcher-get i watcher-f-arrivals)
                                'present  (watcher-present? i)
                                'best     (watcher-get i watcher-f-best)
                                'box      (list (watcher-get i watcher-f-width)
                                                (watcher-get i watcher-f-height)))
                          acc))))))

;;; (watcher-run SECS) -- watch for SECS seconds.  Returns `watcher-report`'s list.
;;;
;;; THIS BLOCKS, deliberately and only here: it is what a bench session types.  A board that must
;;; stay answerable while watching calls `watcher-tick!` from its own main loop instead -- that is
;;; the shape this file is built for, and the reason the tick is separate from the driver.
(define (watcher-run secs)
  (let ((t-end (+ (millis) (* secs 1000))))
    (let loop ()
      (if (>= (millis) t-end)
          (begin
            (hud-car-disconnect! "watch complete")
            (let ((r (watcher-report))) (syslog "[watcher] ~a\n" r) r))
          (begin
            (watcher-tick!)
            (delay-ms 20)              ;;;!< yield; the send rate gate lives in hud-car-tick!
            (loop))))))

(syslog "watcher loaded\n")

; end of watcher.scm
