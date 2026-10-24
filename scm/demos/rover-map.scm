(define (rmap-signal! kind)
  (set! rmap-sightings (+ rmap-sightings 1))
  (set! rmap-last-id kind)
  (set! rmap-signal-ms (millis))
  (rmap-paint!)
  kind)

;;; ---------------------------------------------------------------------------------------------
;;; THREE ZONES ON THE RING (owner, 2026-10-09): outer front WHITE always, inner GREEN on an
;;; animal, middle BLUE on a person.
;;;
;;; INDICES ARE SETTINGS BECAUSE I CANNOT SEE THE CAR.  `WS2812.navLights` says 0/11 are port,
;;; 5/6 starboard, and 2/3 + 8/9 the two ends; the owner confirmed 2/3 reads as FRONT ("lights are
;;; good").  Beyond that the physical outer/inner/middle grouping is not derivable from the code,
;;; so these are a best mapping stated plainly rather than guessed silently -- correct any of them
;;; with (setting! 'rmap_led_* ...) and nothing else changes.
;;; RING GEOMETRY, FROM THE OWNER (2026-10-09): "0-11 is front left to back left, clockwise."
;;; So 0 is the front-left corner, numbering runs clockwise around the car, and 11 is back-left --
;;; the FRONT is the low indices and the REAR is the high ones.  `WS2812.navLights`'s comment
;;; ("0/11 port, 5/6 starboard, 2/3 and 8/9 white aft and forward") does NOT agree with that and
;;; should not be used as a map; it is left alone here because it is someone else's demo.
(define (rmap-led-outer) (setting 'rmap_led_outer '(0 3)))    ;;;!< outer front -- white, always on
(define (rmap-led-inner) (setting 'rmap_led_inner '(1)))      ;;;!< inner  -- green on animal
(define (rmap-led-mid)   (setting 'rmap_led_mid   '(2)))      ;;;!< middle -- blue on person
(define (rmap-led-rear)  (setting 'rmap_led_rear  '(9 10 11)));;;!< rear -- red, always on

;;; THE SIGNAL MUST DECAY, AND THAT IS WHY THIS EXISTS.  The first version painted the ring on a
;;; detection and nothing ever restored it: the owner reported "the LEDs turned blue and stayed
;;; blue".  A latched indicator stops being an indicator -- after the first person it says "person"
;;; forever, which is indistinguishable from a stuck LED and strictly worse than no light at all.
(define (rmap-signal-hold-ms) (setting 'rmap_signal_hold_ms 1200))
(define rmap-signal-ms 0)

(define (rmap-set-each! idxs colour)
  (when (defined? 'WS2812.setColor)
    (let loop ((l idxs))
      (when (pair? l) (WS2812.setColor (car l) colour) (loop (cdr l))))))

;;; ONE PAINTER FOR THE WHOLE RING.  Every zone is written on every repaint, so a cleared signal
;;; cannot leave a stale colour behind -- the bug above was a partial update with no inverse.
(define (rmap-paint!)
  (when (defined? 'WS2812.setAll)
    (guard (e (#t #f))
      (let* ((age   (- (millis) rmap-signal-ms))
             (live  (and rmap-last-id (< age (rmap-signal-hold-ms)))))
        (WS2812.setAll 0 0 0)
        (rmap-set-each! (rmap-led-outer) 'rgb-white)
        (rmap-set-each! (rmap-led-rear)  'rgb-red)
        (when live
          (cond ((eq? rmap-last-id 'animal) (rmap-set-each! (rmap-led-inner) 'rgb-green))
                ((eq? rmap-last-id 'person) (rmap-set-each! (rmap-led-mid)   'rgb-blue))))
        (unless live (set! rmap-last-id #f))
        (when (defined? 'WS2812.show) (WS2812.show))))))

(define (rmap-lights!) (set! rmap-last-id #f) (rmap-paint!))

;;; Copyright 2026 by Frobenius Norm LLC 2026-10-09
;;;
;;; ROAM AND MAP -- autonomous wander with sonar obstacle avoidance, building an occupancy bitmap.
;;; Layers on top of demos/rover.scm, which already owns the SAFETY half: `rover-stop-mm` (300),
;;; `rover-clear-mm` (450) and the `rover-blocked` latch with hysteresis.  Nothing here weakens
;;; that -- this file adds TURNING (the reflex previously only stopped) and the map.
;;;
;;; ############################################################################################
;;; THE POSE IS DEAD RECKONING FROM COMMANDED WHEEL SPEED, WITH NO FEEDBACK OF ANY KIND.
;;; ############################################################################################
;;; There is no encoder, no compass and no IMU on this vehicle (owner, 2026-10-09; confirmed by
;;; grep -- the board map names none).  So position is INTEGRATED FROM WHAT WE ASKED THE MOTORS TO
;;; DO, never from what they did.  Everything that makes wheels disagree with commands is invisible
;;; here: slip, carpet versus hardwood, a sagging battery, one motor weaker than its mate, the
;;; 180 deg of yaw a stalled wheel adds while the others push.
;;;
;;; CONSEQUENCE, AND IT IS NOT A CAVEAT BUT THE SHAPE OF THE RESULT: error in heading integrates
;;; into position LINEARLY WITH DISTANCE and never comes back.  A 5% speed error is a 5% scale
;;; error; a 5 deg/turn heading error after six turns is 30 deg, and every obstacle logged after
;;; that is in the wrong place by the full lever arm.  **This map is qualitative.**  It is good for
;;; "there is a wall roughly there and I have been roughly here"; it is NOT a metric map and two
;;; passes over the same room will not superimpose.  Do not build anything on it that needs the
;;; cells to be where they say they are.
;;;
;;; THE TWO CONSTANTS BELOW ARE THE WHOLE SCALE OF THE MAP AND THEY ARE GUESSES UNTIL MEASURED.
;;; `(rover-map-calibrate-help)` prints the procedure.  Until it is run, treat distances as
;;; "units", not millimetres.

(syslog "Loading rover-map\n")

(define (rmap-cells)       (setting 'rmap_cells        64))  ;;;!< grid is N x N cells
(define (rmap-cell-mm)     (setting 'rmap_cell_mm     100))  ;;;!< 64 x 100 mm = 6.4 m square
(define (rmap-beam-mm)     (setting 'rmap_beam_mm    4000))  ;;;!< treat >= this as "no echo"

;;; PROVISIONAL, UNMEASURED -- see the calibration note above.  Named so a grep finds them.
(define (rmap-mm-per-s)    (setting 'rmap_mm_per_s    300))  ;;;!< forward speed at rover-speed-pct
(define (rmap-degs-per-s)  (setting 'rmap_degs_per_s   90))  ;;;!< spin rate at rover-speed-pct

;;; THE VEHICLE DOES NOT DRIVE STRAIGHT -- IT SLICES LEFT, AND THE OWNER MEASURED IT.
;;; "over 9 feet it may shift left 1" (owner, 2026-10-09).  That is a constant-curvature ARC, not
;;; noise: lateral offset y over arc length s is y ~ s^2/(2R), so R = s^2/(2y) = 81/2 = 40.5 ft
;;; ~= 12345 mm.  Driving 1 m therefore steals ~4.6 deg of heading to the left, and dead reckoning
;;; that believes the commanded heading is wrong by the FULL accumulated angle -- which is why the
;;; first runs logged a straight corridor down +x that does not exist.
;;; Modelled here rather than only trimmed away, because a trim cancels what it cancels and the
;;; RESIDUAL still integrates; correcting the model costs nothing and survives a bad trim.
(define (rmap-drift-radius-mm) (setting 'rmap_drift_radius_mm 12345))

;;; Longest tick gap that may be integrated as motion, seconds.  Above this the loop was not
;;; running; see the note in rmap-integrate!.  0.5 s is ~3x the measured ~150 ms tick, so a slow
;;; tick still counts and a pause never does.
(define (rmap-max-dt) (setting 'rmap_max_dt 0.5))

;;; PHYSICAL TRIM.  Left is motors {0,3}, right is {1,2} (clockwise from front-left, PCA9685.scm).
;;; Slicing LEFT means the right side out-pushes the left, so the correction scales the RIGHT side
;;; down.  For a track W and radius R the needed difference is dV/V = W/R ~= 150/12345 ~= 1.2%.
;;; Default 1.0 = NO TRIM, because an uncalibrated trim is just a different unknown: set it, drive
;;; nine feet, and see whether the foot of drift shrank.
(define (rmap-right-trim) (setting 'rmap_right_trim 1.0))

(define rmap-grid   #f)        ;;;!< bytevector, one byte per cell
(define rmap-x-mm   0.0)       ;;;!< pose, millimetres, origin = where the map was created
(define rmap-y-mm   0.0)
(define rmap-th     0.0)       ;;;!< heading, radians, 0 = +x
(define rmap-last-ms 0)        ;;;!< millis at the last pose integration
(define rmap-drive   0.0)      ;;;!< commanded forward fraction, -1..1  (what we THINK we are doing)
(define rmap-spin    0.0)      ;;;!< commanded spin fraction,   -1..1  (+ = left)

(define RMAP-UNKNOWN 0)
(define RMAP-FREE    1)
(define RMAP-OCCUPIED 2)

;;; USE THE PLATFORM RNG.  `Platform.random-integer` is `esp_fill_random` on this part -- the
;;; hardware generator -- and it is the house primitive, so a hand-rolled LCG here would be a second
;;; source of randomness with worse properties and no reason to exist.
;;;
;;; I ALMOST SHIPPED THAT LCG.  `(defined? 'random)` is #f on this build, and I took one name
;;; answering no as the capability being absent.  It is exposed as `Platform.random-integer` /
;;; `-real` / `-bytevector`, which a grep for the SHAPE rather than the NAME finds immediately.
;;; Testing one spelling and concluding absence is the same error as reporting a missing verb from
;;; a usage comment.
;;;
;;; The cost, stated because it is a real loss: a hardware RNG has no seed, so a roam cannot be
;;; replayed exactly.  If a reproducible wander is ever wanted for debugging, swap this one function
;;; for a seeded generator and print the seed -- nothing else here depends on the source.
(define (rmap-rand! n) (modulo (abs (Platform.random-integer)) n))

(define (rmap-init!)
  (let ((n (rmap-cells)))
    (set! rmap-grid (make-bytevector (* n n) RMAP-UNKNOWN))
    (set! rmap-x-mm 0.0) (set! rmap-y-mm 0.0) (set! rmap-th 0.0)
    (set! rmap-last-ms (millis))
    (set! rmap-drive 0.0) (set! rmap-spin 0.0)
    (syslog "rover-map: ~a x ~a cells of ~a mm (~a m square)\n"
            n n (rmap-cell-mm) (/ (* n (rmap-cell-mm)) 1000.0))
    #t))

;;; Cell indexing.  The vehicle starts at the CENTRE so it can roam in any direction; a corner
;;; origin would throw away three quadrants the moment it backed up.
(define (rmap-ix mm) (+ (quotient (rmap-cells) 2) (exact (round (/ mm (rmap-cell-mm))))))
(define (rmap-in? cx cy) (and (>= cx 0) (>= cy 0) (< cx (rmap-cells)) (< cy (rmap-cells))))
(define (rmap-ref cx cy)
  (if (rmap-in? cx cy) (bytevector-u8-ref rmap-grid (+ cx (* cy (rmap-cells)))) RMAP-UNKNOWN))

;;; OCCUPIED IS STICKY, FREE IS NOT ALLOWED TO ERASE IT.  A single beam sweeping past a table leg
;;; will report the gap beside it as free on the very next tick; letting that clear the leg makes
;;; the map forget every obstacle it has ever seen.  One-way marking is crude and is the right
;;; crude: this map's job is "do not go there", and a false obstacle is cheaper than a missing one.
(define (rmap-set! cx cy v)
  (when (and (rmap-in? cx cy)
             (or (= v RMAP-OCCUPIED)
                 (= (rmap-ref cx cy) RMAP-UNKNOWN)))
    (bytevector-u8-set! rmap-grid (+ cx (* cy (rmap-cells))) v)))

;;; POSE INTEGRATION.  Called every tick with the elapsed time; uses the COMMANDED fractions, which
;;; is the only thing this vehicle can tell us.  Straight-line approximation per tick: at a 3 ms
;;; loop the arc and the chord differ by far less than the dead-reckoning error itself.
(define (rmap-integrate!)
  (let* ((now (millis))
         (raw (/ (- now rmap-last-ms) 1000.0))
         ;; CLAMP THE STEP.  `rmap-last-ms` is whatever it was when ticking last stopped, so the
         ;; first integrate of any new motion charges the WHOLE IDLE GAP at the commanded speed.
         ;; Observed twice, and the second time only because the number was absurd enough to see:
         ;; a 10 s roam reported 3,989 mm where 3,000 was commanded, and a 3 s drive reported
         ;; -465,951 mm -- 466 metres backwards in three seconds.
         ;;
         ;; I FIXED THIS ONCE AT `rover-map-roam!` BY RESETTING THE CLOCK THERE, AND IT CAME BACK
         ;; THROUGH A DIFFERENT DOOR -- a bare `rmap-drive!` with a hand-rolled loop never touches
         ;; that reset.  The cause is not "roam forgot to reset", it is that THIS function trusts a
         ;; timestamp of arbitrary age.  So the guard belongs here, where every caller gets it.
         ;;
         ;; A gap longer than the clamp means the loop was NOT RUNNING, and integrating absence as
         ;; motion is never right.  Discarding it under-reports travel during a stall, which is the
         ;; safe direction: a map that forgets ground it covered is recoverable, one that invents
         ;; half a kilometre is not.
         (dt  (if (> raw (rmap-max-dt)) 0.0 raw)))
    (set! rmap-last-ms now)
    (when (> dt 0)
      (unless (= rmap-spin 0.0)
        (set! rmap-th (+ rmap-th (* rmap-spin (rmap-degs-per-s) dt 0.017453293))))
      (unless (= rmap-drive 0.0)
        (let ((d (* rmap-drive (rmap-mm-per-s) dt)))
          ;; APPLY THE MEASURED LEFT DRIFT.  Forward travel curves the heading; see the radius
          ;; note above.  Sign: positive d (forwards) turns LEFT, which is +theta here.
          (let ((r (rmap-drift-radius-mm)))
            (unless (= r 0) (set! rmap-th (+ rmap-th (/ d r)))))
          (set! rmap-x-mm (+ rmap-x-mm (* d (cos rmap-th))))
          (set! rmap-y-mm (+ rmap-y-mm (* d (sin rmap-th)))))))))

;;; RAY CAST.  Mark the beam free up to the hit, and the hit cell occupied.
;;; THE BEAM IS TREATED AS A LINE AND IT IS A ~15 DEG CONE.  An HC-SR04 returns the NEAREST echo
;;; anywhere in that cone, so a reading of 1.2 m means "something is 1.2 m away somewhere in a
;;; 300 mm-wide arc", not "something is 1.2 m straight ahead".  Marking a line therefore puts the
;;; obstacle in one cell when the truth is a smear, and marks cells free that were never actually
;;; swept.  Accepted deliberately: the cone is wider than the cell, so the alternative is to mark
;;; three cells occupied per ping and fill the map with walls.
(define (rmap-observe! mm)
  (when rmap-grid
    (let* ((maxr (rmap-beam-mm))
           (hit? (and mm (< mm maxr)))
           (r    (if hit? mm maxr))
           (step (/ (rmap-cell-mm) 2.0))
           (n    (max 1 (exact (round (/ r step))))))
      ;; HOIST THE TRIG.  cos/sin of the heading are CONSTANT for the whole ray, and computing
      ;; them per step cost 96 ms per observation -- measured 2026-10-09, 957 ms for ten calls,
      ;; against 1.3 ms for Sonar.loop and 1.6 ms for rover-distance-mm.  That single mistake, not
      ;; the sonar, was throttling the control loop to 4.6 Hz: at the commanded 300 mm/s the
      ;; vehicle covered ~65 mm between obstacle checks, leaving only four or five updates of
      ;; margin inside a 300 mm stop threshold.  **A mapping routine was setting the braking
      ;; distance**, which is not a trade anyone chose.
      ;; I had also ASSERTED the sonar was the cost before measuring it, and was wrong by 60x.
      (let ((ct (cos rmap-th)) (st (sin rmap-th)))
        (let loop ((i 0))
          (when (<= i n)
            (let ((d (* i step)))
              (rmap-set! (rmap-ix (+ rmap-x-mm (* d ct)))
                         (rmap-ix (+ rmap-y-mm (* d st))) RMAP-FREE))
            (loop (+ i 1))))
        (when hit?
          (rmap-set! (rmap-ix (+ rmap-x-mm (* r ct)))
                     (rmap-ix (+ rmap-y-mm (* r st)))
                     RMAP-OCCUPIED))))))

;;; ---------------------------------------------------------------------------------------------
;;; MOTION PRIMITIVES.  Motor numbering is clockwise from front-left, so LEFT is {0,3} and RIGHT is
;;; {1,2} (PCA9685.scm).  MotorPins already inverts one side's pin ORDER, which is why equal values
;;; drive FORWARD rather than spinning -- so a spin is made by NEGATING one side, not by equalising.
;;; #############################################################################################
;;; `Motor.set` CAPS AT 50% DUTY, SO ITS "1.0" IS HALF POWER.  [found 2026-10-09, owner: "lugging"]
;;; #############################################################################################
;;; (Motor.set n 1.0) -> frac = 1.0/2.0 = 0.5 -> setChannelDutyCycle pin (* 100.0 0.5) = 50%.
;;; Its docstring says "Set the motor to the range -1..1", which reads as full scale; the /2.0 at
;;; PCA9685.scm carries no comment, so whether it is a deliberate cap or an old bipolar-PWM
;;; hangover is unknown.  CONSEQUENCES ALREADY OBSERVED ON THIS VEHICLE: 0.25 is 12.5% duty and
;;; does not move the car at all (read as stiction, and it is -- but at a duty nobody intended);
;;; 1.0 is 50% and the motors LUG under load.
;;;
;;; NOT FIXED IN `Motor.set` ON PURPOSE.  That is shared core code -- the behaviour queue, the move
;;; DSL, every demo and P123's reactive layer all call it -- and doubling its output would silently
;;; double every speed in the system, including ones chosen and measured against the current
;;; scaling.  That is a change to make deliberately with its callers reviewed, not as a side effect
;;; of wanting a car to move faster.  Filed as [B847]; this file takes the direct path.
;;;
;;; THE DIRECT PATH INVALIDATES THE WRITE-SUPPRESSION CACHE.  `Motor.last` exists so `Motor.set`
;;; touches I2C only on a change, and its own note warns that a stale entry makes the next
;;; identical command a silent no-op -- "a motor that will not respond to exactly one speed, which
;;; reads as a flat battery".  Writing the pins behind its back would do exactly that, so every
;;; direct write marks the cache 'unset and the next `Motor.set` (notably `Motor.estop!`) is
;;; guaranteed to go through.  The stop path must never be the thing that gets suppressed.
(define (rmap-pwm! m0 m1 m2 m3)
  (when (and (defined? 'Motor.setpins) (defined? 'pca9685-info))
    (let ((pins (pca9685-info 'MotorPins))
          (v    (vector m0 m1 m2 m3)))
      (do ((i 0 (+ i 1))) ((>= i 4))
        (let ((p (vector-ref pins i)))
          (Motor.setpins (car p) (cadr p) (vector-ref v i)))
        (when (defined? 'Motor.last) (vector-set! Motor.last i 'unset))))))

(define (rmap-drive! frac)
  (set! rmap-drive frac) (set! rmap-spin 0.0)
  (let ((r (* frac (rmap-right-trim))))        ;;;!< {1,2} are the right side -- see rmap-right-trim
    (rmap-pwm! frac r r frac)))

;;; BACK UP WHILE TURNING, as one manoeuvre (owner, 2026-10-09).  Reversing straight and then
;;; spinning in place needs clearance the vehicle does not have when it has just stopped in front
;;; of something -- the spin TRANSLATES (observed: a 1 s spin drove it into the wall), so an
;;; in-place turn is not in-place.  Backing on an arc uses the space behind, which is the space it
;;; just came from and therefore the only space known to be free.
(define (rmap-back-turn! frac dir)       ;;;!< frac > 0; dir +1 = swing left, -1 = right
  (set! rmap-drive (- frac))
  (set! rmap-spin  (* dir frac 0.5))
  (let* ((inner (* -1.0 frac 0.35))
         (outer (* -1.0 frac)))
    (if (> dir 0)
        (rmap-pwm! outer inner inner outer)        ;;;!< left side faster back -> swings left
        (rmap-pwm! inner outer outer inner))))

(define (rmap-spin! frac)              ;;;!< + = left (right side forward, left side back)
  (set! rmap-spin frac) (set! rmap-drive 0.0)
  (rmap-pwm! (- frac) frac frac (- frac)))

(define (rmap-halt!)
  (set! rmap-drive 0.0) (set! rmap-spin 0.0)
  (when (defined? 'Motor.estop!) (Motor.estop!)))

;;; ---------------------------------------------------------------------------------------------
;;; THE ROAM STATE MACHINE.  Four states, and the only one that moves forward is `drive`.
(define rmap-state     'idle)
(define rmap-state-ms  0)
(define rmap-deadline  0)     ;;;!< millis after which roaming stops, whatever else is true
(define rmap-turns     0)
(define (rmap-back-ms)  (setting 'rmap_back_ms  900))

;;; THE USABLE SPEED HAS A FLOOR AND IT IS HIGH.  Measured 2026-10-09: at 0.25 the vehicle does not
;;; move at all (stiction -- the motors cannot break away), at 1.0 it runs at ~880 mm/s.  So
;;; `rover-speed-pct`'s 35 is below the floor for this chassis on this floor, and a roam commanded
;;; at it sits still while every instrument reports a healthy drive.  Default full until someone
;;; measures where the floor actually is.
(define (rmap-speed) (/ (setting 'rmap_speed_pct 100) 100.0))
(define (rmap-turn-ms)  (setting 'rmap_turn_ms  600))   ;;;!< x1..x3, randomised

(define (rmap-state! s ms) (set! rmap-state s) (set! rmap-state-ms ms))

;;; EVERY EXIT FROM THIS FUNCTION LEAVES THE MOTORS COMMANDED, so a tick that falls through cannot
;;; leave the vehicle driving.  The deadline is checked FIRST for the same reason.
;;; #############################################################################################
;;; TWO DEFECTS FOUND BY DRIVING INTO A WALL, 2026-10-09.  BOTH WERE MINE.  READ BEFORE EDITING.
;;; #############################################################################################
;;;
;;; 1. NOTHING PUMPED THE SONAR.  `rover-distance-mm` reads the NON-BLOCKING `Sonar.start` /
;;;    `Sonar.result` pair, and `Sonar.loop` is what arms the next ping.  This tick never called it,
;;;    so `Sonar.result` answered "still waiting" forever.  Measured after the crash: 0 valid
;;;    readings in 60 samples without `Sonar.loop`, 30 of 30 with it.  `Sonar.scm` documents this
;;;    exact state ("idle, nothing armed") as [B625] and says `Sonar.latest` then freezes at the
;;;    rated range -- which READS AS A CONFIDENT CLEAR PATH.  It is the same defect as [B784] in
;;;    P262: two ticks that did not compose, the rover reading a channel nothing pumped.  I had
;;;    already hit it once on this vehicle.
;;;
;;; 2. AND THAT ONLY MATTERED BECAUSE THE SAFETY TEST FAILED OPEN.  The obstacle branch read
;;;    `(and (eq? state 'drive) mm (< mm stop-mm))`, so a `#f` distance -- NO READING -- fell
;;;    through to the drive branch and the vehicle carried on.  **Absence of a signal was encoded as
;;;    permission to move.**  Defect 1 made that happen every single tick.
;;;
;;;    Had the test failed SAFE, defect 1 would have been harmless: the car would have sat still and
;;;    the missing pump would have shown up as "it never moves" instead of as a wall.  That is the
;;;    whole argument for failing closed, and it is why the order below is deliberate -- NO READING
;;;    is checked BEFORE the distance comparison, not folded into it.
(define (rover-map-tick!)
  (rmap-integrate!)
  ;; PUMP FIRST.  Without this the readings never arrive; see defect 1 above.
  (when (defined? 'Sonar.loop) (guard (e (#t #f)) (Sonar.loop)))
  (let* ((now (millis))
         (mm  (if (defined? 'rover-distance-mm) (rover-distance-mm) #f))
         (stale (and (defined? 'rover-sonar-stale?) (rover-sonar-stale?)))
         ;; A reading we do not have, or one too old to act on, is an OBSTACLE as far as motion is
         ;; concerned.  Never a clear path.
         (blind (or (not mm) stale)))
    (rmap-observe! mm)
    (cond
      ;; 0. BLIND WHILE MOVING -> STOP.  First, above everything except the deadline, and it does
      ;;    not enter `back`/`turn`: those are manoeuvres chosen because we SAW something.  Not
      ;;    knowing is a different state and the honest response to it is to stop and re-arm.
      ((and blind (or (eq? rmap-state 'drive) (eq? rmap-state 'back) (eq? rmap-state 'turn))
            (not (and (> rmap-deadline 0) (> now rmap-deadline))))
       (rmap-halt!)
       (when (defined? 'rover-ping!) (guard (e (#t #f)) (rover-ping!)))
       (rmap-state! 'blind now))
      ((eq? rmap-state 'blind)
       (if blind
           (begin (rmap-halt!)
                  (when (defined? 'rover-ping!) (guard (e (#t #f)) (rover-ping!))))
           (begin (rmap-drive! (rmap-speed)) (rmap-state! 'drive now))))
      ;; 1. DEADLINE -- a roaming robot with no human attached needs a clock that stops it.
      ((and (> rmap-deadline 0) (> now rmap-deadline))
       (rmap-halt!) (rmap-state! 'done now) (set! rmap-deadline 0)
       (syslog "rover-map: deadline reached, stopped\n"))
      ((eq? rmap-state 'idle) (rmap-halt!))
      ((eq? rmap-state 'done) (rmap-halt!))
      ;; 2. OBSTACLE -- unconditional, checked before any state's own logic.
      ;; OBSTACLE -> BACK UP WHILE TURNING, one manoeuvre.  Direction is random so the vehicle
      ;; does not trace the same escape out of the same corner forever.
      ((and (eq? rmap-state 'drive) mm (< mm (rover-stop-mm)))
       (set! rmap-turns (+ rmap-turns 1))
       (rmap-back-turn! (rmap-speed) (if (= 0 (rmap-rand! 2)) 1.0 -1.0))
       (rmap-state! 'back now))
      ((eq? rmap-state 'back)
       (if (> (- now rmap-state-ms) (rmap-back-ms))
           (begin (rmap-drive! (rmap-speed)) (rmap-state! 'drive now))
           #t))
      ((eq? rmap-state 'turn)
       (if (> (- now rmap-state-ms) (* (rmap-turn-ms) (+ 1 (rmap-rand! 3))))
           (begin (rmap-drive! (rmap-speed)) (rmap-state! 'drive now))
           #t))
      ((eq? rmap-state 'drive) (rmap-drive! (/ (rover-speed-pct) 100.0)))
      (else (rmap-halt!)))

    ;; #########################################################################################
    ;; THE CAMERA LEGS RUN *AFTER* THE MOTION DECISION, AND THE ORDER IS A SAFETY PROPERTY.
    ;; #########################################################################################
    ;; They used to run at the TOP of this tick, before the sonar was even read.  [B854] (RELEASE,
    ;; 2026-10-14) measured that an open LLIP session freezes the main loop for its entire
    ;; duration -- `llip-run-session` runs synchronously inside the hooked `llip-server-poll`, so
    ;; control does not return until the client disconnects; a connected-but-IDLE client was enough
    ;; to stop the REPL answering, and it revived the instant the socket closed.
    ;;
    ;; WITH THE OLD ORDER THAT WOULD HAVE DELAYED BRAKING BY THE LENGTH OF A FRAME TRANSFER, on a
    ;; moving vehicle, every tick.  The obstacle test is the one piece of this loop that must not
    ;; wait for anything optional -- and a camera frame is optional, which is exactly why it was
    ;; guarded and non-fatal everywhere else.  Pumping it first undid that intent by ordering.
    ;;
    ;; The same argument is already written in `llip-reactive.scm`'s header: the reactive vetoes
    ;; must run EVERY main-loop iteration BEFORE the motor tick.  A transfer that holds the loop
    ;; does not merely add latency, it removes the safety step for that whole window.
    ;;
    ;; They are still pumped every tick -- a channel nothing pumps answers #f forever -- just last.
    (rmap-look!)
    (rmap-hear!)
    (rmap-paint!)
    rmap-state))

(define (rover-map-roam! secs)
  (unless rmap-grid (rmap-init!))
  (set! rmap-deadline (+ (millis) (* 1000 secs)))
  ;; RESET THE INTEGRATION CLOCK.  `rmap-last-ms` is whatever it was when ticking last stopped, so
  ;; the first integrate of a new roam would otherwise charge the whole idle gap to this run at the
  ;; commanded speed.  Observed 2026-10-09: a 10 s roam reported 3,989 mm of travel where 10 s at
  ;; the commanded 300 mm/s is 3,000 -- a third of the distance invented by a stale timestamp.
  (set! rmap-last-ms (millis))
  (set! rmap-turns 0)
  (rmap-drive! (rmap-speed))
  (rmap-state! 'drive (millis))
  (syslog "rover-map: roaming for ~a s -- (rover-map-stop!) to abort\n" secs)
  #t)

(define (rover-map-stop!) (rmap-halt!) (rmap-state! 'idle (millis)) (set! rmap-deadline 0) #t)

;;; ---------------------------------------------------------------------------------------------
;;; RENDERING.  '.' unknown, ' ' free, '#' occupied, '@' the vehicle.  Printed with +y UP, because
;;; a map printed upside-down is the [B814] mistake in a different medium.
(define (rover-map-show)
  (if (not rmap-grid)
      (syslog "rover-map: no grid -- (rmap-init!) first\n")
      (let ((n (rmap-cells)) (rx (rmap-ix rmap-x-mm)) (ry (rmap-ix rmap-y-mm)))
        (let yloop ((y (- n 1)))
          (when (>= y 0)
            (let ((row ""))
              (let xloop ((x 0))
                (when (< x n)
                  (set! row (string-append row
                    (cond ((and (= x rx) (= y ry)) "@")
                          ((= (rmap-ref x y) RMAP-OCCUPIED) "#")
                          ((= (rmap-ref x y) RMAP-FREE)     " ")
                          (else "."))))
                  (xloop (+ x 1))))
              (syslog "~a\n" row))
            (yloop (- y 1))))
        #t)))

(define (rover-map-status)
  (list 'state rmap-state 'x (exact (round rmap-x-mm)) 'y (exact (round rmap-y-mm))
        'heading-deg (exact (round (/ rmap-th 0.017453293))) 'turns rmap-turns
        'cells (if rmap-grid (rmap-cells) 0)
        'CAVEAT 'pose-is-dead-reckoned-uncalibrated))

(define (rover-map-calibrate-help)
  (syslog "rover-map calibration -- the map has NO SCALE until these are measured:\n")
  (syslog "  1. clear 2 m.  (rmap-drive! 0.35) wait 2 s (rmap-halt!).  Measure mm travelled.\n")
  (syslog "     rmap_mm_per_s = mm / 2\n")
  (syslog "  2. (rmap-spin! 0.35) wait 2 s (rmap-halt!).  Measure degrees turned.\n")
  (syslog "     rmap_degs_per_s = deg / 2\n")
  (syslog "  Set both with (setting! 'rmap_mm_per_s N) etc, then re-run.  Until then the grid\n")
  (syslog "  is in UNITS, not millimetres, and two passes will not superimpose.\n")
  #t)

(syslog "rover-map loaded: (rover-map-roam! SECS) (rover-map-stop!) (rover-map-show) (rover-map-status)\n")

;;; ---------------------------------------------------------------------------------------------
;;; SIGHTINGS: BLINK GREEN FOR A CAT, BLUE FOR A PERSON (owner, 2026-10-09).
;;;
;;; The recogniser does NOT run here -- it runs on the Jetson, which is the architecture the owner
;;; ruled for this system: the ESP32 offloads quickly and takes instruction back.  So this half is
;;; only (a) submit a frame when one is due, (b) colour the ring when an answer comes back.
;;;
;;; THE DETECTOR'S CLASSES ARE animal/person/vehicle, NOT SPECIES.  MegaDetector v5a answers
;;; `animal`; it does not distinguish a cat from a deer.  So "green for cats" is implemented as
;;; GREEN FOR `animal`, and that is a deliberate reading of the instruction rather than a silent
;;; substitution -- a species-level answer needs a classifier stage this system does not have yet
;;; (P260 "detect, then classify, geofenced").  If the intent was literally cats only, the
;;; classifier is the missing piece and this will over-report.
(define (rmap-look-ms) (setting 'rmap_look_ms 1500))   ;;;!< minimum gap between frame submissions
(define rmap-last-look 0)
(define rmap-sightings 0)
(define rmap-last-id   #f)

(define (rmap-signal! kind)
  (set! rmap-sightings (+ rmap-sightings 1))
  (set! rmap-last-id kind)
  (when (defined? 'WS2812.setAll)
    (guard (e (#t #f))
      (cond ((eq? kind 'animal) (WS2812.setAll 0 255 0))    ;;;!< green
            ((eq? kind 'person) (WS2812.setAll 0 0 255))    ;;;!< blue
            (else               (WS2812.setAll 0 0 0)))
      (when (defined? 'WS2812.show) (WS2812.show))))
  kind)


;;; Submit a frame if one is due and the transport is present.  GUARDED AND NON-FATAL: a vehicle
;;; whose recogniser is absent must still drive and still avoid walls -- the camera is an addition
;;; to the mission, not a precondition for it.  This is [B784]'s lesson from P262: the tick must
;;; not read a channel nothing pumps, and must not die when one is missing.
;;; COUNT THE FAILURES.  The first version of this swallowed every error silently, "so a missing
;;; recogniser cannot stop the vehicle driving" -- a correct intent implemented as SILENCE.  It
;;; then failed on 100% of calls for an hour and was indistinguishable from working: sightings 0,
;;; no log line, nothing to see.  When the owner asked "is it watching for cats?" the honest answer
;;; was that I could not tell, because I had guarded away the only evidence.  Non-fatal must not
;;; mean invisible -- that is the absence-as-success shape that drove this vehicle into a wall
;;; earlier the same afternoon.
(define rmap-look-sent 0)
(define rmap-look-failed 0)
(define rmap-look-last-err #f)

;;; AND THE CALL ITSELF WAS WRONG.  `(llip-vision-capture-send! port seq)` takes an ALREADY-OPEN
;;; port and a sequence number (llip-vision.scm:142; hud-car.scm:358 shows the live call).  I
;;; invoked it with NO arguments, which raises inside dict_add_keyval_frame as
;;; "Mismatched keyval pairs (keys (port seq)) (values nil)" -- an arity error wearing a dictionary
;;; error's clothes, and completely hidden by the guard above.
(define rmap-seq 0)
(define (rmap-look!)
  (let ((now (millis)))
    (when (and (> (- now rmap-last-look) (rmap-look-ms))
               (defined? 'llip-vision-capture-send!)
               (defined? 'hud-car-conn) hud-car-conn)
      (set! rmap-last-look now)
      (set! rmap-seq (+ rmap-seq 1))
      (if (guard (e (#t (set! rmap-look-last-err e) #f))
            (llip-vision-capture-send! hud-car-conn rmap-seq))
          (set! rmap-look-sent (+ rmap-look-sent 1))
          (set! rmap-look-failed (+ rmap-look-failed 1))))))

;;; Poll for an identification coming back from the appliance.  `vision-poll` is P262's reader.
;;; PUMP THE LINK BEFORE ASKING IT ANYTHING.  `vision-pending?` reads a CACHE VARIABLE
;;; (`hud-car-vision`); it does not touch the socket.  `hud-car-tick!` is what reads bytes,
;;; reassembles lines across TCP boundaries and fills that cache.  Without it the cache stays #f
;;; forever while the kernel socket fills up -- measured: `u8-ready?` #t and `char-ready?` #t on a
;;; connection with 21 bytes queued, and `vision-pending?` answering #f beside them.
;;;
;;; THIS IS THE THIRD TIME TODAY, IN ONE FILE, ON ONE VEHICLE.  The sonar had no pump and drove the
;;; car into a wall; the camera SENDER was called with the wrong arity behind a silent guard; and
;;; now the camera RECEIVER reads a cache nothing fills.  [B784] named this exact shape in P262 --
;;; "the two ticks did not compose -- the rover read a channel nothing pumped" -- and I have now
;;; rebuilt it three times.  The pattern to distrust is any `*-pending?` / `*-latest` /
;;; `*-result` accessor: ask what fills it, and call THAT, before trusting what it says.
(define (rmap-hear!)
  (when (and (defined? 'hud-car-tick!) (defined? 'hud-car-conn) hud-car-conn)
    (guard (e (#t #f)) (hud-car-tick!)))
  (when (and (defined? 'vision-pending?) (defined? 'vision-poll))
    (guard (e (#t #f))
      (when (vision-pending?)
        (let ((rec (vision-poll)))
          (when (pair? rec)
            (let ((what (car rec)))
              (cond ((memq what '(animal cat dog deer)) (rmap-signal! 'animal))
                    ((memq what '(person people human)) (rmap-signal! 'person))
                    (else #f)))))))))

(define (rover-map-sightings)
  (list 'sightings rmap-sightings 'last rmap-last-id
        'frames-sent rmap-look-sent 'frames-failed rmap-look-failed
        'conn (and (defined? 'hud-car-conn) (if hud-car-conn 'up 'down))))
