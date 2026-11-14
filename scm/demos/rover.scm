;;; rover.scm -- LambLisp-centric control of the mobile animal recogniser.  [P262]
;;; Copyright 2026 by Frobenius Norm LLC 2026-10-06 16:45:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; THE VEHICLE CARRIES THE CAMERA; THE APPLIANCE CARRIES THE MODEL.  Frames go out over LLIP and
;;; `(vision-id GROUP "name" CONF X Y W H)` records come back on the same connection.  The vehicle
;;; never holds the weights, which is what lets a 4 MB ESP32 take part in recognition at all.
;;;
;;; ONE TICK, NON-BLOCKING, NO EXCEPTIONS -- the same shape as `wifi-tick!` and `hud-car-tick!`.
;;; A blocking driver once held the REPL for 57 minutes: the board could not be asked anything and
;;; could not be told to stop.  **A field robot that cannot be interrogated while running is a robot
;;; you recover by walking to it.**
;;;
;;; EVERY HARDWARE CALL IS GUARDED BY `defined?`.  These peripherals are per-target -- a board with
;;; no motors or no sonar must still LOAD this file and run the parts it can, rather than failing at
;;; load time over a component it was never going to have.

(syslog "rover loading\n")

;;; --- tunables, all overridable per board via Settings ------------------------------------------
(define (rover-stop-mm)      (setting 'rover_stop_mm     300))   ;;;!< below this: stop, no exceptions
(define (rover-clear-mm)     (setting 'rover_clear_mm    450))   ;;;!< above this: free to drive again
(define (rover-look-ms)      (setting 'rover_look_ms    2000))   ;;;!< minimum gap between captures
(define (rover-speed-pct)    (setting 'rover_speed_pct     35))

;;; HYSTERESIS IS NOT A REFINEMENT.  One threshold makes a vehicle sitting at exactly `stop_mm`
;;; alternate between driving and stopping every tick, which on a sonar whose reading jitters by a
;;; few millimetres is most of the time.  Stop low, resume high, and the two numbers must differ.

;;; --- state ------------------------------------------------------------------------------------
(define rover-running    #f)   ;;;!< the policy drives only while this is #t
(define rover-blocked    #f)   ;;;!< latched by the safety step, cleared only above rover-clear-mm
(define rover-last-look   0)   ;;;!< millis of the last capture
(define rover-looks       0)   ;;;!< captures submitted this session
(define rover-sightings   0)   ;;;!< vision records acted on
(define rover-last-state '())  ;;;!< last LED state applied, so the lights are not re-pushed per tick
(define rover-mm         #f)   ;;;!< the distance the LAST TICK ACTED ON -- see rover-status

;;; --- the LED language ---------------------------------------------------------------------------
;;; THE PANEL IS NOT ON THE VEHICLE, so the LEDs are the instrument panel and the vocabulary is
;;; designed rather than accreted.  It has to be readable from across a clearing.
;;;
;;;   safe, idle                     slow breathe, green
;;;   driving                        steady green
;;;   obstacle, stopped              solid red
;;;   captured, awaiting a result    amber blink
;;;   animal detected                green flash, N times for the confidence band
;;;   person / vehicle detected      blue
;;;   link down                      red/amber alternate
;;;
;;; THE LAST ROW IS THE ONE THAT MATTERS.  A robot stopped for an obstacle and a robot stopped
;;; because it lost the appliance look identical from a distance and need OPPOSITE interventions --
;;; one is cleared by moving the obstacle, the other by fixing the network.  Giving them the same
;;; signal would send an operator to do the wrong thing, which is worse than no signal at all.
;;;
;;; PERSISTENT CONDITIONS GO TO `led-base!`, EVENTS TO `led-emit!`.  The arbiter restores the base
;;; mode after a transient finishes, so a detection flash cannot leave the vehicle showing the wrong
;;; standing condition -- which is exactly how a status light becomes untrustworthy.
(define (rover-leds-available?) (and (defined? 'led-base!) (defined? 'led-blink)))

(define (rover-state! state)
  ;; Re-pushing an identical base task every tick would restart its animation continuously, so a
  ;; "slow breathe" would never breathe.  Apply only on CHANGE.
  (when (and (rover-leds-available?) (not (equal? state rover-last-state)))
    (set! rover-last-state state)
    (led-base!
      (cond ((eq? state 'driving)   (led-blink led-signal-zone 'rgb-green  0 1000    1))
            ((eq? state 'blocked)   (led-blink led-signal-zone 'rgb-red    0 1000    1))
            ((eq? state 'looking)   (led-blink led-signal-zone 'rgb-yellow 0  200  200))
            ((eq? state 'link-down) (rover-alternate 'rgb-red 'rgb-yellow 300))
            (else                   (led-blink led-signal-zone 'rgb-green  0  120 1800))))))

;;; Red/amber alternate: two colours, never dark.  Distinct at a glance from the solid red of an
;;; obstacle AND from the amber blink of a capture in flight, which is the whole point of the row.
(define (rover-alternate a b ms)
  (cycle (list ms (lambda () (zone-color! led-signal-zone a)))
         (list ms (lambda () (zone-color! led-signal-zone b)))))

;;; A sighting is an EVENT, so it preempts and then hands the lights back.
;;; Animals flash green, N times for the confidence band -- a count carries across a clearing where
;;; a hue does not.  People and vehicles are blue: the distinction matters because the vehicle must
;;; not treat a person as quarry to approach.
(define (rover-signal-sighting! rec)
  (when (rover-leds-available?)
    (let* ((grp  (vision-group rec))
           (conf (vision-conf rec))
           (band (cond ((>= conf 80) 3) ((>= conf 50) 2) (else 1))))
      (if (eq? grp 'animal)
          (led-emit! 2 (led-blink led-signal-zone 'rgb-green band 150 150))
          (led-emit! 2 (led-blink led-signal-zone 'rgb-blue  2    300 150))))))

;;; --- the vehicle's half of the vision interface --------------------------------------------------
;;; `vision-poll` / `vision-pending?` / the accessors live in hud-car.scm, which owns the link and
;;; therefore owns the return channel.  `vision-ask!` is here because ASKING is a policy decision.
;;;
;;; IT NEVER BLOCKS AND IT NEVER QUEUES BEHIND ITSELF: one frame in flight at a time.  Queueing a
;;; second capture while the first is unanswered spends airtime on an image the policy has already
;;; stopped caring about, and the answers then arrive out of step with where the vehicle is.
;;; ONE PREDICATE FOR THE WHOLE DEPENDENCY, NOT A GUARD PER NAME.  This file uses ELEVEN
;;; identifiers that live in hud-car.scm -- `hud-car-conn`, `hud-car-seq`, `hud-car-drain-acks!`,
;;; `vision-poll`, `vision-pending?`, `vision-group/name/conf`, `llip-vision-capture-send!` and
;;; more -- and it can be loaded without that file.  Guarding them one at a time is how the first
;;; attempt at this went: `vision-poll` was guarded, `hud-car-conn` two steps earlier was not, and
;;; the tick still raised.  Ask once, about the capability.
;;;
;;; THREE NAMES ARE ENOUGH AND MORE WOULD BE WORSE: the eleven all arrive together from one file,
;;; so a longer list tests the same fact repeatedly while reading as though it tested more.  These
;;; three span the two things the vision steps actually need -- the link (`hud-car-conn`), the
;;; reader (`vision-poll`) and the in-flight test (`vision-pending?`).
(define (rover-vision-available?)
  (and (defined? 'hud-car-conn)
       (defined? 'vision-poll)
       (defined? 'vision-pending?)))

(define (vision-ask!)
  (if (or (not hud-car-conn) (vision-pending?))
      #f
      (let ((n (guard (e (#t #f)) (llip-vision-capture-send! hud-car-conn hud-car-seq))))
        (when n
          (set! hud-car-seq (+ hud-car-seq 1))
          (set! rover-looks (+ rover-looks 1))
          (set! rover-last-look (millis)))
        n)))

;;; --- the tick -------------------------------------------------------------------------------------
;;; ORDER IS LOAD-BEARING, and step 2 is the reason:
;;;   1 sonar result   -- collect the ping fired LAST tick; never ping-and-wait
;;;   2 safety         -- stop below the threshold.  LOCAL.  NEVER CONSULTS THE NETWORK.
;;;   3 motion         -- drive policy, only if safe
;;;   4 look           -- only while stopped, only if due, only if the link is up
;;;   5 leds           -- status
;;;   6 sonar start    -- fire the next ping so step 1 has something to collect
;;;
;;; *** STEP 2 NEVER CONSULTS THE NETWORK.  The vehicle must be safe with the appliance switched
;;; off, the WiFi down, or the recogniser simply WRONG.  Recognition is an ENRICHMENT; obstacle
;;; avoidance is not, and wiring the two together would make a network fault into a collision. ***
(define (rover-tick!)
  ;; 1 + 2: the distance that matters is the one already measured.
  (let ((mm (rover-distance-mm)))
    (set! rover-mm mm)
    (cond ((and mm (< mm (rover-stop-mm)))
           (unless rover-blocked
             (set! rover-blocked #t)
             (rover-halt! "obstacle"))
           (rover-state! 'blocked))
          ((and mm (> mm (rover-clear-mm)))
           (set! rover-blocked #f))
          (else #f))

    ;; 3: drive only when running, unblocked, AND actually holding a reading.  `mm` being #f means
    ;; the sensor said nothing -- missing, or stale past the point where a ping could still be in
    ;; flight -- and a vehicle that drives on no information is worse than one that waits.
    (cond ((and rover-running (not rover-blocked) mm)
           (rover-drive!)
           (rover-state! 'driving))
          ((not rover-blocked)
           (rover-state! 'idle))
          (else #f))

    ;; 4: look while STOPPED, which is a measurement rather than a preference -- the binding
    ;; constraint on image quality is vehicle motion, not the camera, so a frame taken while rolling
    ;; is a blurred frame that cost a second of airtime.  The vehicle was going to stop anyway.
    (let ((stopped (or rover-blocked (not rover-running))))
      (cond ((not (rover-vision-available?)) #f)   ;;;!< no vision files loaded: steps 4+5 are absent
            ((not hud-car-conn) (rover-state! 'link-down))
            ((and stopped
                  (>= (- (millis) rover-last-look) (rover-look-ms))
                  (not (vision-pending?)))
             (when (vision-ask!) (rover-state! 'looking)))
            (else #f)))

    ;; PUMP THE CHANNEL BEFORE READING IT.  [B784]  This step used to begin "the reader in
    ;; hud-car.scm has already parsed it", which was false and is the whole defect: parsing happens
    ;; in `hud-car-drain-acks!`, whose ONLY other caller is inside `hud-car-tick!` -- a function
    ;; this tick does not call.  So a vehicle running the P262 loop asked for looks forever and
    ;; could not receive one; `rover-looks` climbed, `rover-sightings` stayed 0, and nothing errored
    ;; or logged, because the appliance was replying correctly into a socket nobody drained.
    ;;
    ;; MEASURED, 35 s per arm against the live appliance, same connection and scene: without this
    ;; call `(conn 1 looks 18 acks 0 rxbuf 0)` -- 18 frames out, none of the 18 acks read; with it
    ;; `(conn 1 looks 18 acks 36)`, the 36 being this arm's 18 plus the 18 the previous arm had left
    ;; unread in the socket.
    ;;
    ;; THE ARM HAD TO COUNT ACKS, NOT SIGHTINGS.  The appliance acks every frame whatever it sees,
    ;; so acks measure the CHANNEL; sightings measure the SCENE, and an indoor scene yields so few
    ;; that both arms read 0 and the test proves nothing.
    ;;
    ;; Calling it here is safe beside `hud-car-tick!` as well: a second drain in the same tick reads
    ;; whatever is in the socket and finds nothing.
    ;;; THE WHOLE VISION STEP IS SKIPPED WHEN THE RETURN CHANNEL IS ABSENT, and that guard is on
    ;;; `vision-poll` rather than only on the pump.  Both live in hud-car.scm; this file can be
    ;;; loaded without it, and then `(vision-poll)` is an unbound variable.
    ;;;
    ;;; WHY IT MATTERS MORE THAN A MISSING FEATURE: the raise lands at THIS step, which is BEFORE
    ;;; step 6 re-arms the sonar.  So the ping never goes out, `rover-distance-mm` returns #f on the
    ;;; next tick -- correctly, a stale sensor is not a clear path -- and the vehicle refuses to
    ;;; drive, with the LEDs frozen at whatever the last completed tick set and nothing anywhere
    ;;; saying why.  A vehicle that silently stops because a RECOGNITION file is missing is the exact
    ;;; inversion of this design's rule that *recognition is an enrichment and obstacle avoidance is
    ;;; not*.  Measured on the 4WD: rover.scm loaded alone, every `(rover-tick!)` raised.
    (when (rover-vision-available?)
      (when (defined? 'hud-car-drain-acks!) (hud-car-drain-acks!))
      (let ((rec (vision-poll)))
        (when rec
          (set! rover-sightings (+ rover-sightings 1))
          (rover-signal-sighting! rec)
          (syslog "rover: saw ~a (~a%)\n" (vision-name rec) (vision-conf rec)))))

    ;; 5 + 6.
    (when (defined? 'Led.loop)  (Led.loop))
    (rover-ping!)))

;;; --- hardware shims, each guarded so a board without the peripheral still runs -------------------
;;; A MISSING SONAR READS AS "NO READING", NOT AS "CLEAR".  `#f` makes the safety step take neither
;;; branch, so the vehicle does not drive; returning a large number instead would make a board with
;;; no sensor behave as though the way ahead were open, which is the most dangerous possible default.
(define (rover-distance-mm)
  (if (not (defined? 'Sonar.latest))
      #f
      (if (rover-sonar-stale?)
          #f
          (let ((m Sonar.latest)) (if (number? m) (inexact->exact (round (* m 1000))) #f)))))

;;; A SONAR THAT STOPPED REPORTING LOOKS EXACTLY LIKE A CLEAR PATH, AND THAT IS THE DANGEROUS
;;; DIRECTION.  `Sonar.latest` initialises to `Sonar.range-m` -- "nothing in range" -- and keeps that
;;; value if no ping ever completes.  So a disconnected, unpowered or wedged sensor reads as 4000 mm
;;; of open ground, forever, and the safety step happily lets the vehicle drive into whatever is
;;; actually there.  Absence of a signal must not be encoded as the safe answer.
;;;
;;; The module already tracks what is needed: `Sonar.last-start-ms` is set by every arm and
;;; `Sonar.stale-ms` is deliberately several times the ping timeout, so exceeding it means the
;;; reading is not merely old but IMPOSSIBLE to still be in flight.  Past that, this returns #f --
;;; "no reading" -- and `rover-tick!` then takes neither the stop branch nor the clear branch, so the
;;; vehicle does not drive.  Refusing to move on a silent sensor is the correct failure.
(define (rover-sonar-stale?)
  (if (not (and (defined? 'Sonar.last-start-ms) (defined? 'Sonar.stale-ms)))
      #f
      (and (> Sonar.last-start-ms 0)
           (> (- (millis) Sonar.last-start-ms) Sonar.stale-ms))))

(define (rover-ping!)  (when (defined? 'Sonar.arm!)  (guard (e (#t #f)) (Sonar.arm!))))

;;; [B785]  THE `Motor.stop` BRANCH IS GONE BECAUSE IT WAS NEVER REACHABLE.  No such procedure
;;; exists anywhere in the tree; it read as a fallback for a board with a different motor API and
;;; was dead code the `cond` could never select.  A fallback that cannot fire is worse than no
;;; fallback, because it makes the first branch look optional.
(define (rover-halt! why)
  (syslog "rover: HALT (~a)\n" why)
  (when (defined? 'Motor.estop!)
    (guard (e (#t #f)) (Motor.estop!))))

;;; [B785]  THIS CALLED `Motor.forward`, WHICH HAS NEVER EXISTED ON ANY BOARD, AND THE GUARD MADE
;;; THAT SILENT.  The real API is in scm/core/PCA9685.scm: `Motor.set` (one motor, -1..1),
;;; `Motor.setAll` (all four), `Motor.motion`, `Motor.estop!`.  `(defined? 'Motor.forward)` was
;;; written to mean "a board without the motor peripheral still runs" -- a good intention -- but
;;; what it tests is whether a NAME exists, so a typo became a permanent, universal, silent no-op:
;;; the vehicle could brake (`Motor.estop!` does exist, so halt always worked) and could never
;;; accelerate.  It fails SAFE, which is why it produced no incident and no bug report, and why
;;; every stationary P262 run was read as a property of the board rather than as this defect.
;;;
;;; `w3_ai_scripts/scm_guard_check.py` now sweeps the tree for this shape -- a `(defined? 'X)` guard
;;; whose X is defined nowhere -- because the only input that makes such a guard disagree is a name
;;; that is RIGHT, which is exactly the input nobody feeds a guard they just wrote.
;;;
;;; UNITS: `Motor.set`/`Motor.setAll` take -1..1; `rover_speed_pct` is a PERCENTAGE, so it is scaled
;;; here.  Passing 35 straight through would saturate at full forward and read in the diff as a
;;; speed setting being honoured.
(define (rover-drive!)
  (when (defined? 'Motor.setAll)
    (guard (e (#t #f)) (Motor.setAll (/ (rover-speed-pct) 100.0)))))

;;; --- operator surface ------------------------------------------------------------------------------
(define (rover-start!) (set! rover-running #t)  (syslog "rover: running\n")  #t)
(define (rover-stop!)  (set! rover-running #f)  (rover-halt! "operator")     #t)

;;; Status as DATA, not a printed line: a remote caller reads it over LLIP and decides for itself.
;;;
;;; IT REPORTS THE DISTANCE THE LAST TICK ACTED ON, NOT A FRESH READING, and the difference is not
;;; cosmetic.  This used to call `(rover-distance-mm)` at status time -- so after a tick that saw a
;;; STALE sensor (`#f`, and correctly refused to act) the status re-read the sonar, which the tick's
;;; own final step had just re-armed, and reported a confident `mm 413`.  **The display showed a
;;; measurement the policy had never seen**, and an operator reading it would conclude the vehicle
;;; was ignoring a clear obstacle when it was in fact refusing to move on a silent sensor.
;;; Measured on the 4WD 2026-10-06: `blocked #f` beside `mm 413` with the stop threshold at 500.
;;;
;;; A status line that recomputes its inputs is not reporting the system, it is reporting a second
;;; system that happens to share a sensor.
(define (rover-status)
  (list 'running   rover-running
        'blocked   rover-blocked
        'mm        rover-mm
        'link      (if hud-car-conn 'up 'down)
        'looks     rover-looks
        'sightings rover-sightings
        'pending   (vision-pending?)))

(syslog "rover loaded\n")
