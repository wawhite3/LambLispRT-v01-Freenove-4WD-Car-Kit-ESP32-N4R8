;;; Copyright 2026 by Frobenius Norm LLC 2026-05-16
;;; Free for non-commercial use. Commercial use requires a license.
#| @name PCA9685 Interface

PCA9685 originally designed as a 16 channel LED controller, now used as a general-purpose PWM controller.

|#
(news "Loading PCA9685 Interface\n")

(define have-pca9685 (dict-ref? (current-environment) 'PCA9685.begin))

(news "Loading PCA9685 Interface 10\n")
(unless have-pca9685
  (define PCA9685.begin Lambda0)
  (define PCA9685.setChannelServoPulseDuration Lambda0)
  (define PCA9685.setChannelDutyCycle Lambda0)
  ;;; B259: delay_ms IS NOT STUBBED HERE, and must never be again.  It is a core commonio
  ;;; builtin (mop3_cio_delay_ms), not a PCA9685 procedure, and a top-level define binds in the
  ;;; interaction environment -- so stubbing it here silently disabled EVERY delay in the image
  ;;; for the rest of the session, on every target lacking a PCA9685.  A module may stub only
  ;;; its OWN procedures.  The comment further down this file assumes real delays
  ;;; ("6 x (delay_ms 2000) = ~12 s"), which is how it went unnoticed: the boards that HAVE a
  ;;; PCA9685 never take this branch.
  )

(news "Loading PCA9685 Interface 30\n")

(define pca9685-info (2list->obj
		      `(
			;;device attributes
			(default-address #x5f)
			(servo-frequency 50.0)
			
			;;servo name, pin number, operating region pulse width min & max in us
			(Xservo  (0 500 2500))
			(Yservo  (1 1500 2500))	;can't go all the way down due to mechanical interference
			(Servo2  (2 500 2500))
			(Servo3  (3 500 2500))
			(Servo4  (4 500 2500))
			(Servo5  (5 500 2500))
			(Servo6  (6 500 2500))
			(Servo7  (7 100 2500))

			;;Motors are listed clockwise from fl, fr, rr, rl
			;;Motors are operated by duty cycle 0 .. 100% at the PWM level.
			;;!THIS LINE IS THE AUTHORITY ON MOTOR NUMBERING -- 0 fl, 1 fr, 2 rr, 3 rl, clockwise
			;;!from front left viewed from above.  **So LEFT is {0,3} and RIGHT is {1,2}**, and that
			;;!derived grouping is the part callers actually need; write it down once, here, rather
			;;!than re-deriving it per call site.  `hud-car-motor-l`/`-r` in scm/demos/hud-car.scm
			;;!point at this line for their defaults (0 and 1, the front motor of each side).
			;;!
			;;!AND THE PAIRS BELOW CORROBORATE IT: 0 and 3 ascend, 1 and 2 descend, which is exactly
			;;!{left} against {right}.  A mirror-image motor needs opposite wiring to turn the same
			;;!way, so the right side's pairs are reversed IN THE TABLE.  Consequence: `Motor.setAll
			;;!+x` drives all four FORWARD -- a caller never applies the reversal itself.
			;;!
			;;![B785]  Worth one line on how this was nearly got wrong.  The numbering was read OFF
			;;!THE PIN TABLE instead of off this comment, and the pin table gives the GROUPING
			;;!({0,3} vs {1,2}) without the ORIENTATION -- so it cannot say which group is left, and
			;;!a session concluded the mapping was "not established" with the answer two lines above
			;;!what it was reading.  Declining to guess was right; not grepping for the fact was not.
			(MotorPins ,'#((14 15) (13 12) (11 10) (8  9)))
			)
		      )
  )

(news "Loading PCA9685 Interface 35\n")

(PCA9685.begin (pca9685-info 'default-address))

(news "Loading PCA9685 Interface 40\n")
;;(PCA9685.setSingleDeviceToFrequency (dict-ref 'PCA9685-Info 'servo-frequency))

;;Set a channel to a pulsewidth given in microseconds.
;(define pcapw PCA9685.setChannelServoPulseDuration)

;;Return a (lambda (frac) ...) that will set the servo channel pulse width to the range -1..1, scaled into the operating region

(define (make-servo chan pwmin pwmax)
  (let* ((pwspan (- pwmax pwmin)))
    (lambda (plusminus1) (PCA9685.setChannelServoPulseDuration chan (+ pwmin (* (+ 0.5 (/ plusminus1 2.0)) pwspan))))
    )
  )

(news "Creating servos\n")

(define xservo
  (let ((xsv (apply make-servo (pca9685-info 'Xservo))))
    (lambda (plusminus1) (xsv (- plusminus1)))
    )
  )

(define yservo (apply make-servo (pca9685-info 'Yservo)))
(define servo2 (apply make-servo (pca9685-info 'Servo2)))
(define servo3 (apply make-servo (pca9685-info 'Servo3)))
(define servo4 (apply make-servo (pca9685-info 'Servo4)))
(define servo5 (apply make-servo (pca9685-info 'Servo5)))
(define servo6 (apply make-servo (pca9685-info 'Servo6)))
(define servo7 (apply make-servo (pca9685-info 'Servo7)))

(syslog "all servos created\n")

;; Startup: just CENTER the servos -- no exercise sweep.  The old sweep
;; (center/L/R/center/tilt-down/up/mid/ahead) cost 6 x (delay_ms 2000) = ~12 s
;; of every boot and moves nothing useful (and nothing at all on a flat battery).
(syslog "pan/tilt center (startup sweep removed)\n")
(xservo 0)
(yservo 0)

#|!
The motors respond over the full range of the PWM at 50 Hz (which is what it's set to).
Therefore we can use the duty cycle setter rather than scaling the pulsewidth over an application-specific operating range, as was done above.
|#
(define (Motor.setpins pin-fwd pin-bak frac)
  (if (< frac 0) (let ((tmp pin-fwd))
		   (set! pin-fwd pin-bak)
		   (set! pin-bak tmp)
		   (set! frac (- frac))
		   )
      )
  (PCA9685.setChannelDutyCycle pin-fwd (* 100.0 frac) 0)	;0 == no phase shift
  (PCA9685.setChannelDutyCycle pin-bak 0 0)
  )

;;!Last value written to each motor channel ('unset forces the first write).  The
;;!loop and the motion queue re-assert the same value every tick, so without this
;;!Motor.set would spam 8 I2C writes per loop -- the dominant loop cost (and far
;;!worse when the bus NAKs on a flat battery).  I2C only on change.
;;;!DO NOT WRITE TO THIS VECTOR TO SIMULATE MOTOR STATE.  It is a WRITE-SUPPRESSION CACHE, not a
;;;!readout: `Motor.set` compares against it and touches I2C only on a change, so a hand-set entry
;;;!makes the next IDENTICAL command a silent no-op while the hardware sits at the old value.  The
;;;!symptom is a motor that will not respond to exactly one speed, which reads as a flat battery or
;;;!a bus fault.  Observed 2026-10-07 while testing [B785]'s telemetry fix: two entries were poked
;;;!to produce readable values, which left the cache claiming 42% and -17% on stopped motors.
;;;!`(Motor.estop!)` resynchronises it (0 differs from the poked value, so the write goes through);
;;;!to force the next command through regardless, set an entry to `'unset`.
(define Motor.last (make-vector 4 'unset))

;;!Set the motor to the range -1..1, where 1 is FULL duty.  Touches I2C ONLY when the command
;;!changes.
;;
;;; [B847] THE `/ 2.0` THAT USED TO BE HERE WAS A SERVO TRANSFORM, AND IT HALVED EVERY MOTOR.
;;;
;;; It read `(frac (/ plusminus1 2.0))`, so a commanded 1.0 reached `setChannelDutyCycle` as 50%.
;;; The owner's intent is the range this docstring has always claimed: "settable -1..+1", 1 = full.
;;;
;;; WHERE THE HALVING CAME FROM, because it was not arbitrary.  `make-servo`, two definitions above,
;;; is `(+ pwmin (* (+ 0.5 (/ plusminus1 2.0)) pwspan))` -- the CENTRED transform a servo needs,
;;; mapping -1..1 onto 0..1 of the pulse span because mid-travel is the middle of that span.  This
;;; line was the SCALE half of that expression with its `+ 0.5` companion missing.  A motor needs
;;; NEITHER half: `Motor.setpins` handles sign by SWAPPING the forward and reverse pins and negating,
;;; so magnitude must pass straight through.  The file's own comment above `Motor.setpins` says
;;; motors use the duty-cycle setter "rather than scaling the pulsewidth over an
;;; application-specific operating range, AS WAS DONE ABOVE" -- naming the servo code it was
;;; borrowed from.
;;;
;;; EVERY CALL SITE ALREADY ASSUMED THE DOCUMENTED RANGE, which is why this is a fix and not a
;;; rescaling.  Surveyed 2026-10-14 across 8 files: `Motor.motion 1000 1 1 1 1`, `-0.5`, `0.3`,
;;; `(/ (rover-speed-pct) 100.0)` = 0.35.  **Nobody ever wrote 2.0 to compensate.**  They wrote the
;;; contract and silently got half of it -- and `1 1 1 1` plainly means full speed.
;;; Measured consequence on the 4WD: 0.25 commanded = 12.5% duty and the vehicle DOES NOT MOVE AT
;;; ALL (read as stiction, at a duty nobody intended); 1.0 = 50% and the motors lug under load.
;;;
;;; THE CLAMP IS NEW AND IS NOT COSMETIC.  The library maps linearly and DOES NOT CLAMP:
;;; `pulse_width = TIME_MAX * duty_cycle / PERCENT_MAX` (PCA9685.cpp:881, PERCENT_MAX = 100).  While
;;; the halving stood, an out-of-range 2.0 arrived as 100% and looked harmless; at full scale the
;;; same call would ask for 200% and a pulse width past TIME_MAX.
(define (Motor.clamp1 x) (cond ((> x 1.0) 1.0) ((< x -1.0) -1.0) (else x)))

;;; #############################################################################################
;;; THE CEILING IS NOW EXPLICIT, NAMED AND SETTABLE -- AND IT DEFAULTS TO TODAY'S BEHAVIOUR.
;;; #############################################################################################
;;; The owner's intent is "settable -1..+1" (2026-10-14), and then, correctly: *"possibly not; it may
;;; be that the actual values accepted by the motors need the halving."*  BOTH can be true, and the
;;; `/ 2.0` was wrong for a reason that does not depend on which:
;;;
;;;   * IF a ceiling is wanted, it is a MOTOR/BATTERY fact -- a 2S pack is ~7.4 V into motors
;;;     commonly rated 6 V, so a deliberate cap is entirely reasonable.  But then it deserves a NAME,
;;;     a value chosen against the datasheet, and a comment.  Encoded as `/ 2.0` inside the setter it
;;;     is invisible, it silently redefines the documented -1..1 domain, and every caller writes the
;;;     contract and gets half: `Motor.motion 1000 1 1 1 1` plainly means full speed.
;;;   * IF no ceiling is wanted, set this to 1.0 and the stated contract holds exactly.
;;;
;;; **0.8 IS THE OWNER'S DECISION (2026-10-14): "make it +- 80% scale".**  So a commanded 1.0 is 80%
;;; duty, and -1..+1 spans the full usable range of the chassis rather than half of it.
;;;
;;; AND THE NUMBER AGREES WITH THE HARDWARE, which is why it is the right KIND of ceiling: a 2S pack
;;; is ~7.4 V into motors commonly rated 6 V, and 6/7.4 = 0.81.  A cap at 80% holds the average motor
;;; voltage at about its rating instead of 23% over it.  That is a motor/battery fact with a reason
;;; attached -- which the `/ 2.0` never had.
;;;
;;; THIS CHANGES HARDWARE BEHAVIOUR: 1.0 was 50% duty and is now 80%, a 1.6x increase at every call
;;; site, all of which already write values inside -1..1 (`Motor.motion 1000 1 1 1 1`, `-0.5`, `0.3`,
;;; rover's 0.35).  Nothing has been driven since the change -- the 4WD was disconnected -- so the
;;; arm below is OWED, not done.
;;;
;;; THE CONFIRMING ARM, and the owner has already supplied its losing half: at the old ceiling a
;;; commanded **0.25 did not move the vehicle at all** (12.5% duty, below stiction).  At 0.8 the same
;;; command is 20% duty.  If 0.25 now moves it, the change is proven by an observation that has
;;; already been made the other way; if it still does not, the stiction floor is higher than 20% and
;;; that is itself the measurement.  Full scale 1.0 should also stop lugging -- it was 50%, now 80%.
(define (Motor.duty-max) (setting 'motor_duty_max 0.8))

(define Motor.set
  (let ((allmotorpins (pca9685-info 'MotorPins)))
    (lambda (n plusminus1)
      (unless (eqv? plusminus1 (vector-ref Motor.last n))
	(vector-set! Motor.last n plusminus1)
	(let* ((motorpins (vector-ref allmotorpins n))
	       (frac (* (Motor.clamp1 plusminus1) (Motor.duty-max))))
	  (Motor.setpins (car motorpins) (cadr motorpins) frac))))))

(define (Motor.set4 m0 m1 m2 m3) (Motor.set 0 m0) (Motor.set 1 m1) (Motor.set 2 m2) (Motor.set 3 m3))

(define (Motor.setAll speed)
  (letrec ((fn (lambda (Mz)
		 (Motor.set Mz speed)
		 (when (< Mz 3) (fn (+ 1 Mz)))
		 )
	       )
	   )
    (fn 0)
    )
  )

(Motor.setAll 0)

;;; Motion behaviour queue (P123 Ph3) -- replaces the bespoke MotionQueue/MotionExpiry.
;;; SHARED with the LLIP executor (llip-executor.scm); ticked by Motor.loop each main loop.
(define motor-bq (make-behavior-queue))

;;!Queue a timed motion: hold (m0 m1 m2 m3) for `duration` ms, then the next task runs.
(define (Motor.motion duration m0 m1 m2 m3)
  (motor-bq 'push (once-for duration (lambda () (Motor.set4 m0 m1 m2 m3)))))

;;!Tick the motion queue; call once per main-loop iteration.  Auto-stops the motors
;;!whenever the queue is idle (no motion queued) -- the safe default on empty.
(define Motor.loop
  (let ((was-idle #t))   ;; motors are 0 at load; auto-stop only on the running->idle EDGE,
    (lambda ()           ;; not every idle loop (that re-ran Motor.set4 = 5 wasted closure calls/loop)
      (motor-bq 'tick)
      (let ((idle (motor-bq 'idle?)))
        (when (and idle (not was-idle)) (Motor.set4 0 0 0 0))
        (set! was-idle idle)))))

;;!Emergency stop (P123 B3/B5): drop all queued/running motion and cut all four motors NOW.
(define (Motor.estop!)
  (motor-bq 'abort)
  (do ((i 0 (+ i 1))) ((= i 4)) (vector-set! Motor.last i 'unset))  ;; force the stop to the bus (bypass the change-cache)
  (Motor.set4 0 0 0 0))

;;; B312: register the motor stop for the escaping-exception path.  Motor.estop! is the right
;;; thunk rather than Motor.set4: it also ABORTS the motion queue, so a queued move cannot
;;; re-command the wheels on the next tick after we have just stopped them.
(when have-pca9685 (register-safe-state! Motor.estop!))

(when have-pca9685
  (Motor.motion 1000 0.5 0.5 0.5 0.5)
  (Motor.motion 1000 0 0 0 0)
  (Motor.motion 1000 -0.5 -0.5 -0.5 -0.5)
  (Motor.motion 1000 0 0 0 0))

(syslog "PCA9685 loaded\n")
