;;; Copyright 2026 by Frobenius Norm LLC 2026-09-25
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; TARGET SETUP for the `Freenove-4WD-Car-Kit-ESP32` env -- the vehicle's MISSION LOOP.
;;;
;;; WHY THIS FILE HAD TO EXIST, AND WHAT ITS ABSENCE LOOKED LIKE [B625].
;;;
;;; `setup.scm` used to build a shared robot loop for every target.  That was removed (f7154315)
;;; because the S3-EYE has no sonar or motors and crashed on the unbound `Sonar.loop`; each target
;;; now "owns its mission via app-loop", and a target that installs none gets a deliberately safe
;;; IDLE loop that ticks nothing robotic.  That default is right for a camera node.  On the
;;; VEHICLE it silently removed the mission: `Sonar.loop` and `Motor.loop` stopped being called
;;; and nothing said so.
;;;
;;; The symptom was the dangerous kind -- a PLAUSIBLE reading rather than an absent one.
;;; `Sonar.latest` sat frozen at `Sonar.range-m` (4.0 m), which is exactly the value the code uses
;;; to mean "nothing in range", so an obstacle reflex read a confident CLEAR PATH forever and the
;;; HUD showed 4000 mm.  Measured 2026-09-25: eight samples over ~20 s, 4.0 every time, while the
;;; ISR pair driven BY HAND returned a real echo.  The sensor was fine; nothing was asking it.
;;;
;;; THE ORDER OF THESE CALLS IS LOAD-BEARING, and it is restored from the loop that was removed
;;; rather than reinvented.  `reactive-tick!` applies the P123 B5 safety vetoes -- reflexes and
;;; deadman -- and it MUST run BEFORE `Motor.loop`, so a veto lands in the same tick that acts on
;;; it.  Moving motion ahead of the veto would let the vehicle execute a move the safety layer had
;;; already decided against, once per tick, forever.
;;;
;;; SONAR USES THE INTERRUPT PAIR, NOT THE BLOCKING PING.  `Sonar.loop` drives P126 A3
;;; (`Sonar.start` / `Sonar.result`) which returns immediately; `Sonar.ping` BLOCKS for up to the
;;; ~50 ms timeout and is bench-only.  A blocking ping here would blow the 3 ms loop budget on
;;; every tick.

(syslog "Target: Freenove-4WD-Car-Kit-ESP32 -- 4WD vehicle node\n")

;;;!< Guard-dog cadence, as the removed shared loop had it.
(define fourwd-guard-dog-ms 1000)

;;; THE GUARD-DOG IS OFF BY DEFAULT, AND IT IS NOT A WATCHDOG -- IT DRIVES THE CAR.
;;;
;;; The name invites the assumption that it is a safety check.  It is not.  `guard-dog`
;;; (`scm/core/setup.scm`) fires when `Sonar.latest` drops below 0.100 m and then:
;;;
;;;     (Buzzer.arf)                             ; ONE SHORT CLICK -- see the correction below
;;;     (Led.blinkAll 'rgb-red 5000 1)
;;;     (Motor.motion 1000  1    1    1    1)    ; all four motors FORWARD for 1 s
;;;     (Motor.motion 2000 -0.5 -0.5 -0.5 -0.5)  ; then reverse
;;;
;;; It is a dog barking and lunging -- a demo behaviour, not a reflex.  **It commands the motors.**
;;;
;;; TWO CORRECTIONS TO THE ABOVE, both since this block was written, and both in the direction of
;;; making it LESS alarming than it reads -- which is why they are stated rather than edited away:
;;;   * `Buzzer.arf` is now `(Buzzer.click)`, a single ~5 ms 400 Hz tick, NOT the full-second
;;;     60->4000 Hz sweep quoted above.  Changed 2026-09-25 at the owner's instruction after the
;;;     bench heard it ("no more than 1 chirp <10 ms ... 1 5 ms beep at 400 hz").
;;;   * the MOTION is now behind its own setting, `guard_dog_drive`, default 0 ([B632]).  So even
;;;     with `guard_dog 1` the dog barks and does NOT move; moving takes a second deliberate
;;;     setting.  The alert and the drive were one decision and are now two, because whether ONE
;;;     reading from ONE ultrasonic sensor should move a vehicle is a design question that a
;;;     default cannot answer -- a sonar reports 4.0 m for a timeout, an over-range echo AND a dead
;;;     5 V rail, so it cannot distinguish "clear" from "not answering".
;;;
;;; WHY IT HAD NEVER BITTEN BEFORE, AND WHY FIXING THE SONAR ARMED IT.  `Sonar.latest` was frozen
;;; at `Sonar.range-m` for months ([B625]), so the `< 0.100` test could never be true and nothing
;;; in the loop called it anyway.  Restoring the mission loop made the sonar live AND restored the
;;; guard-dog call in the same change -- so a board that had been silent for months began sounding
;;; a one-second alarm, and would have driven itself, on the first close reading.  Reported from
;;; the bench 2026-09-25: "the car just beeped loudly twice".
;;;
;;; A LOW READING IS NOT EVIDENCE OF AN OBSTACLE RIGHT NOW.  The 5 V rail is dead on this vehicle,
;;; so sonar values are not yet trustworthy -- and an untrustworthy reading that can start the
;;; motors is exactly the combination to refuse by default.
;;;
;;; The real obstacle REFLEX is `reactive-tick!` further down: it trips at 0.15 m, re-arms at
;;; 0.25 m with hysteresis, and VETOES motion rather than commanding it.  That is the safety path
;;; and it stays on.  This is the noisy demo, and it is opt-in.
(define fourwd-guard-dog? (positive? (setting 'guard_dog 0)))

;;; LLIP IS THE CONTROL CHANNEL FOR THIS ROBOT (owner, 2026-09-25) -- so its poll belongs HERE,
;;; in the mission loop, and not in some other session's ad-hoc socket.
;;;
;;; `llip-server-poll` is deliberately NON-BLOCKING and documented "call from main loop"; without
;;; a loop calling it, the board has the whole LLIP server on its filesystem and answers nothing.
;;; That was the state this board was in: `llip-server.scm` present, `llip-make-tcp-server`
;;; unbound, and every control path therefore going through the TCP REPL instead -- which is an
;;; observation channel, not an authenticated control one.
;;;
;;; ONE CONNECTION PER CALL, AND THE SESSION RUNS TO COMPLETION.  That is the poll's contract, and
;;; it has a consequence worth stating rather than discovering: a large LLIP file transfer holds
;;; the mission loop for its duration, so sonar, reflexes and motor ticks all pause.  Do not
;;; command motion across a big push.  The alternative -- draining a transfer a few bytes per tick
;;; -- would put partially-applied file state in the robot's hot loop, which is worse.
;;; --- CAMERA, AND IT COMES UP BEFORE ANYTHING ELSE LOADS -------------------------------
;;; THE VEHICLE USED TO BOOT UNABLE TO SEND A SINGLE FRAME, AND NOTHING SAID SO.  Neither the
;;; codec nor the camera was brought up here, so after every reboot a human had to type
;;; `(load "llip-vision.scm" 0)` and `(camera-init 'jpeg)` over the REPL before the vehicle could
;;; send anything.  Being in the manifest only puts the file on the flash; something has to load
;;; it.  The panel had the identical gap at the other end of the same link.
;;;
;;; ORDER IS LOAD-BEARING: CAMERA FIRST, CODEC SECOND.  `esp_camera_init` needs a CONTIGUOUS
;;; ~23 KB internal-DMA block, and internal DRAM is also where Scheme definitions land -- see the
;;; measurement in setup-Freenove-ESP32-WROVER.scm, where ONE additional loaded .scm file was
;;; enough to make `(camera-init 'jpeg)` never return.  This board has tolerated the other order,
;;; but that is one observation and not a margin worth designing against, so the camera goes
;;; first and the codec after it.
;;;
;;; 'jpeg, NOT the default 'rgb565: rgb565 drives an LCD, and this vehicle's job is to CAPTURE.
;;; Guarded and logged both ways -- a vehicle whose camera did not come up must still reach its
;;; REPL, because that is where someone asks it why, and "no camera" is a diagnosis while silence
;;; is not.
(define fourwd-camera-ok
  (guard (e (#t (syslog "4wd: camera-init raised -- continuing without a camera\n") #f))
    (if (defined? 'camera-init) (camera-init 'jpeg) #f)))
(syslog "4wd: camera ~a\n" (if fourwd-camera-ok "ready (jpeg)" "NOT available"))

;;; [B814] THIS CAMERA IS MOUNTED UPSIDE-DOWN AND NOTHING WAS CORRECTING IT.
;;;
;;; Every frame this vehicle produced arrived rotated 180 deg -- monitor text in the captures reads
;;; inverted and mirrored, in the 2026-10-05 run and again on 2026-10-08.  The correction already
;;; existed as `eye-orient!` in `scm/features/eye-cam.scm`, but its only callers are that file's own
;;; demo procedures (`eye-preview`, `eye-demo`), and `eye-cam.scm` is NOT in this target's manifest.
;;; So the orientation was right in the interactive demo and wrong in every frame the recognition
;;; pipeline consumes.  A capability reachable only from a demo is not a capability the product has.
;;;
;;; IT BELONGS HERE, PER-BOARD, AND NOT IN A SHARED DEFAULT.  Measured the same day: the WROVER's
;;; frames read UPRIGHT.  Two boards, two mountings -- so a global `vflip`/`hmirror` would fix this
;;; vehicle by breaking the other one.  This file is per-board by construction, which is exactly the
;;; scope the fact has.
;;;
;;; IT IS NOT COSMETIC -- MEASURED ON MegaDetector v5a, same 12 frames, the only difference being a
;;; 180 deg rotation applied offline:
;;;   as delivered (inverted):  10 of 12 frames hallucinate `person` at conf 0.01; false positives
;;;                             persist to between 0.10 and 0.15
;;;   rotated upright:           2 of 12 at conf 0.01; false positives gone by 0.05
;;; So the inversion costs roughly 3x the confidence margin and makes a spurious `person` five times
;;; more likely.  Today's corpus is all-negative, so the damage shows up as FALSE POSITIVES; the same
;;; degradation applies to RECALL on a real animal, where it shows up as a MISSED detection and
;;; nothing in an all-`none` set can see it.  That is why this is a precondition for any recall
;;; figure, not a tidy-up.
;;;
;;; Guarded like the init above: a board whose camera did not come up must still reach its REPL.
(define (fourwd-camera-orient!)
  (guard (e (#t (syslog "4wd: camera orientation NOT applied -- ~a\n" e) #f))
    (if (defined? 'camera-set)
        (begin (camera-set 'vflip 1)
               (camera-set 'hmirror 1)
               #t)
        (begin (syslog "4wd: camera-set absent -- orientation NOT corrected [B814]\n") #f))))

;;; [B814, second half] APPLYING IT ONCE AT BOOT IS NOT ENOUGH -- `camera-init` RESETS THE SENSOR.
;;;
;;; Measured 2026-10-09, and this is the case that made the first fix look right and do nothing.
;;; The boot log said `camera orientation corrected (vflip+hmirror = 180 deg)` -- it really did run,
;;; 26 ms after camera-init -- and the very next capture still came back rotated 180 deg, confirmed
;;; by rotating the frame and finding the monitor text readable.  The cause is that the capture path
;;; re-initialises the sensor every time: `w3.sh` sends `(camera-init 'jpeg '<size>)` before grabbing
;;; (it must -- that is how the frame size is chosen), and a fresh `esp_camera_init` restores the
;;; sensor's own register defaults, orientation among them.
;;;
;;; So the orientation is not boot state, it is a PROPERTY OF EVERY INITIALISATION, and it belongs
;;; wherever initialisation happens rather than once beside it.  Shadowing `camera-init` here keeps
;;; that per-board: the host never has to know how this particular camera is mounted, and any future
;;; caller -- the capture verb, a demo, a REPL user -- gets the correction without knowing to ask.
;;; The same wrap-by-value pattern `setup.scm` uses for `tone`.
;;;
;;; IT MUST CAPTURE THE ORIGINAL BY VALUE FIRST, or the wrapper calls itself forever.
(when (and fourwd-camera-ok (defined? 'camera-init))
  (let ((fourwd-camera-init-raw camera-init))
    (set! camera-init
          (lambda args
            (let ((r (apply fourwd-camera-init-raw args)))
              (fourwd-camera-orient!)
              r)))))

(when fourwd-camera-ok
  (if (fourwd-camera-orient!)
      (syslog "4wd: camera orientation corrected, and camera-init wrapped to re-apply it [B814]\n")
      (syslog "4wd: camera orientation NOT corrected -- frames will be upside-down [B814]\n")))

;;; The binary frame codec.  Loaded even when the camera did not come up: the receive half and
;;; the wire format are useful without one, and a missing procedure raising mid-flight is far
;;; harder to read than a line at boot saying it is absent.
(define fourwd-vision-ok
  (guard (e (#t (syslog "4wd: llip-vision.scm failed to load -- cannot send frames\n") #f))
    (if (file-exists? "llip-vision.scm")
        (begin (load "llip-vision.scm" 0) (defined? 'llip-vision-capture-send!))
        (begin (syslog "4wd: llip-vision.scm not staged -- cannot send frames\n") #f))))
(syslog "4wd: vision codec ~a\n" (if fourwd-vision-ok "loaded" "UNAVAILABLE"))

(define fourwd-llip-port  (setting 'llip_port 3142))   ;;;!< NOT 3141: that is the plain TCP REPL
(define fourwd-llip-token (setting 'llip_token "testtoken"))

;;; Guarded, and #f is a legitimate answer: a board with no llip-server.scm staged, or a port
;;; already taken, must still boot to a working vehicle rather than dying in setup.
(define fourwd-llip-srv
  (guard (e (#t (syslog "4wd: LLIP server NOT started -- control channel unavailable\n") #f))
    (if (file-exists? "llip-server.scm")
        (begin (load "llip-server.scm" 0)
               (llip-make-tcp-server fourwd-llip-port fourwd-llip-token))
        (begin (syslog "4wd: llip-server.scm not staged -- no LLIP control\n") #f))))

(syslog "4wd: LLIP control ~a (port ~a)\n"
        (if fourwd-llip-srv "listening" "UNAVAILABLE") fourwd-llip-port)

;;; app-loop is a loop MAKER: setup.scm does `(define loop (if app-loop (app-loop) <default>))`,
;;; so this returns a procedure rather than being one.  Installing the tick directly here is the
;;; mistake that shape invites, and it costs the board its REPL.
;;;
;;; THE COMPONENTS ARE RESOLVED ONCE, IN THE MAKER, AND THAT IS DELIBERATE TWICE OVER:
;;;   * an unbound component throws HERE, at boot, where the `guard` below turns it into a named
;;;     diagnostic and a safe idle loop -- instead of throwing on the first tick, out of the main
;;;     loop, where it takes the REPL and every remote path down with it;
;;;   * the tick then calls locals rather than re-resolving six globals 333 times a second.
(set! app-loop
  (guard (e (#t (syslog "4wd: mission components unavailable -- idle app-loop, REPL stays up\n")
                (lambda () (lambda () #f))))
    (lambda ()
      (let ((llip-poll (if fourwd-llip-srv
                           (lambda () (llip-server-poll fourwd-llip-srv))
                           (lambda () #f)))   ;;;!< resolved ONCE; no per-tick branch on a global
            (sonar     Sonar.loop)
            (led       Led.loop)         ;;;!< led-bq: WS2812 blink + A2 arbiter
            (buzzer    Buzzer.loop)      ;;;!< Buzzer.bq
            (reactive  reactive-tick!)   ;;;!< P123 B5 safety vetoes -- BEFORE motion
            (motor     Motor.loop)       ;;;!< motor-bq: Motor.motion + B3 executor moves
            ;; Resolved once: an off guard-dog costs a no-op call, not a per-tick settings lookup.
            (watchdog  (if fourwd-guard-dog? guard-dog (lambda () #f)))
            (t_guard   (AutoTimer_ms fourwd-guard-dog-ms)))
        ;;; ORDER: sense -> receive commands -> safety vetoes -> act.  LLIP sits after sensing so
        ;;; a command is decided against fresh sonar, and before `reactive`/`motor` so a move it
        ;;; queues is vetted and executed in the SAME tick rather than the next one.
        (lambda ()
          (sonar)
          (unless (t_guard) (watchdog))
          (llip-poll)
          (led)
          (buzzer)
          (reactive)
          (motor))))))

(syslog "4wd: mission loop installed (sonar ISR, led, buzzer, reflexes, motor); guard-dog ~a\n"
        (if fourwd-guard-dog? "ON -- it will BARK AND DRIVE" "off (set guard_dog 1 to enable)"))
