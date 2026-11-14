;;; Copyright 2026 by Frobenius Norm LLC 2026-09-25
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; TARGET SETUP for the `Freenove-ESP32-WROVER` env -- loaded by setup.scm as
;;; setup-<lamb-board>.scm, after every feature and peripheral is defined and before (loop) is
;;; built.  This is a WROVER-CAM: an OV2640 on a classic ESP32 with PSRAM.
;;;
;;; THE CAMERA IS BROUGHT UP HERE, AT THE END OF BOOT, AND THE TIMING IS THE WHOLE POINT [B608].
;;;
;;; `esp_camera_init` needs a CONTIGUOUS ~23 KB internal-DMA block for its line buffer.  Internal
;;; DRAM is also where Scheme definitions land, so every file loaded eats into the same pool --
;;; and the margin is far thinner than it looks.  Measured 2026-09-24, same board, minutes apart:
;;;
;;;   reset, then (camera-init 'jpeg)                        -> ESP_OK in milliseconds
;;;   reset, load ONE .scm file, then (camera-init 'jpeg)    -> never returns
;;;
;;; One additional file tips it.  So the camera must come up BEFORE user code loads, and the end
;;; of boot is the last moment that is still true.
;;;
;;; WHY NOT EARLIER, AND WHY NOT A RESERVATION.  Both were tried and both are worse.  Starting DVP
;;; capture while setup.scm is reading files from flash crashes on camera-DMA-vs-flash-cache
;;; contention.  Reserving the block at install time instead -- which the driver can still do --
;;; starved LittleFS boot reads and SILENTLY CORRUPTED whichever .scm file hit a starved read
;;; ([B88], fixed by switching it off).  A hang is bad; a quietly corrupted library file is worse.
;;;
;;; WHAT REMAINS WRONG.  This is placement, not a cure: the pool is still shared and the margin is
;;; still thin, so a target that grows its library can cross the line again.  The driver now
;;; refuses with #f rather than stalling when the block is too small, so the next time this bites
;;; it says so instead of taking the main loop with it.

(syslog "Target: Freenove-ESP32-WROVER -- OV2640 camera node\n")

;;; Bring the camera up now, in JPEG mode.
;;;
;;; JPEG RATHER THAN RGB565, AND NOT ONLY FOR SIZE.  A 240x240 frame is ~5 KB encoded against
;;; 115,200 raw, which over this radio is the difference between a video feed and a slideshow.
;;; But it is also what the frame header CLAIMS: `llip-vision-capture-send!` writes 'jpeg into
;;; every record regardless of what the sensor is producing, so a camera left in rgb565 puts raw
;;; pixels on the wire under a header that says JPEG.  The receiver hands that to a JPEG decoder,
;;; which rejects it, and the frame is counted bad -- a frame that arrived perfectly and was
;;; discarded because its label was wrong.  Initialising in 'jpeg makes the label true.
;;;
;;; Guarded: a board whose camera is absent or unhappy must still finish booting and reach its
;;; REPL, because that is where someone asks it why.  The result is logged either way -- "the
;;; camera did not come up" is a diagnosis, and silence is not.
(define wrover-camera-ok
  (guard (e (#t (syslog "[wrover] camera-init raised -- continuing without a camera\n") #f))
    (camera-init 'jpeg (setting 'cam_size (string->symbol "240")))))

(syslog "[wrover] camera ~a at ~a\n"
        (if wrover-camera-ok "ready (jpeg)" "NOT available")
        (setting 'cam_size (string->symbol "240")))

;;; `(string->symbol "240")` AND NOT `'240`, WHICH IS A NUMBER.  `camera-init` reads its size
;;; argument only when it `is_any_sym_atom()`, so a numeric default is SILENTLY IGNORED and the
;;; driver falls back to its own FRAMESIZE_240X240 -- the same answer, by a path that never
;;; consulted the argument.  Harmless exactly while the default is 240 and invisible the moment it
;;; is not: it would also swallow the "unknown name -- refusing" diagnostic the driver exists to
;;; print, so a typo would read as "the larger size made no difference".
;;;
;;; --- SENSOR TUNING, ALSO A BOOT PROPERTY ----------------------------------------------------
;;; These reach the OV2640's control block through `camera-set` and are applied once, here, so that
;;; a watcher's exposure is stated in its settings overlay rather than typed into a REPL and lost on
;;; the next reset.  All four default to 0, which is the sensor's own default, so a target that says
;;; nothing is unchanged.  Ranges: brightness/contrast -2..2, vflip/hmirror 0 or 1.
;;;
;;; *** RAISING THE GAIN CEILING IS WHAT MAKES THIS BOARD SEE INDOORS, AND IT IS THE DIFFERENCE
;;; BETWEEN NOTHING AND A RESULT.  Measured 2026-10-14, this board, this pose, 20 minutes apart,
;;; nothing changed but these two keys:
;;;
;;;     default (gainceiling 2x, aec2 off)   671 uxga frames -> 0 detections, frames near-black
;;;     gainceiling 128x + aec2 on            39 uxga frames -> 8 detections, scene fully legible
;;;
;;; The automatic exposure will not use a gain it is not permitted, and the driver's default ceiling
;;; is 2x -- a daylight setting.  A camera that does not move can afford what a long exposure and a
;;; high gain cost, which is noise and motion blur: there is nothing in the frame to smear, and a
;;; noisy bright frame carries far more for a recogniser than a clean black one.  This is the same
;;; argument as the frame size above, on a different axis.
;;;
;;; BRIGHTNESS IS NOT EXPOSURE, and trying it first cost an arm.  It is a digital offset on pixels
;;; the sensor has already captured, so it lifts the black level and the noise together and collects
;;; no extra light.  Measured on the same dark wall: `brightness 2` with `contrast 2` came back
;;; DARKER, because the added contrast crushed what little shadow detail there was.
;;;
;;; `camera-get` REPORTS WHAT WAS COMMANDED, NOT WHAT THE SENSOR IS DOING.  After 20 s of streaming
;;; with automatic gain enabled and a 128x ceiling -- the run that produced the 8 detections above --
;;; it still read `agc-gain 0` and `aec-value 168`, exactly its boot values, while the image had
;;; visibly transformed.  So it answers "what is this control set to", which is what you want when
;;; checking that a setting took, and it CANNOT tell you whether automatic exposure has run out of
;;; room.  Do not build a brightness policy on it.
;;;
;;; AND `frame-brightness` IS NOT AN INSTRUMENT HERE EITHER: it computes mean luma over an RGB565
;;; frame and returns -1 in jpeg mode, which is what a watcher runs in.  It refuses honestly rather
;;; than returning a plausible number, but it means exposure cannot be judged ON the board -- look
;;; at the image (`w3 vision fetch`).
;;;
;;; ONLY KEYS THAT ARE ACTUALLY SET ARE APPLIED, VIA AN `unset` SENTINEL, AND THAT IS NOT TIDINESS.
;;; Several of these controls default to ENABLED in the sensor -- `exposure-ctrl` and `gain-ctrl`
;;; are automatic exposure and automatic gain themselves -- so a numeric default of 0 here would
;;; TURN AUTOMATIC EXPOSURE OFF on every target that never mentioned the key, and the symptom would
;;; be a black image on a board nobody had configured.  A default that silently disables the thing
;;; it is defaulting is the worst available shape, so there is no default: unmentioned means
;;; untouched.
(define (wrover-cam-tune! key skey)
  (let ((v (setting skey 'unset)))
    (if (eq? v 'unset)
        #f
        (begin (syslog "[wrover] camera-set ~a ~a -> ~a\n" key v (camera-set key v)) #t))))

(when wrover-camera-ok
  (wrover-cam-tune! 'gainceiling   'cam_gainceiling)
  (wrover-cam-tune! 'aec2          'cam_aec2)
  (wrover-cam-tune! 'ae-level      'cam_ae_level)
  (wrover-cam-tune! 'exposure-ctrl 'cam_exposure_ctrl)
  (wrover-cam-tune! 'aec-value     'cam_aec_value)
  (wrover-cam-tune! 'gain-ctrl     'cam_gain_ctrl)
  (wrover-cam-tune! 'agc-gain      'cam_agc_gain)
  (wrover-cam-tune! 'brightness    'cam_brightness)
  (wrover-cam-tune! 'contrast      'cam_contrast)
  (wrover-cam-tune! 'quality       'cam_quality)
  (wrover-cam-tune! 'vflip         'cam_vflip)
  (wrover-cam-tune! 'hmirror       'cam_hmirror))

;;; THE FRAME SIZE IS SETTABLE HERE, AND ONLY HERE, BECAUSE THIS IS THE ONLY MOMENT IT IS SAFE
;;; [P260].  `camera-init` treats the SIZE as part of "this mode", so asking for a different one
;;; later DEINITS the sensor and frees the contiguous internal-DMA block it was holding, then tries
;;; to get a wider one back out of a heap that user code has since fragmented.  The size therefore
;;; belongs to BOOT, and an application states it as a SETTING rather than by calling camera-init
;;; again.  `cam_size` is one of qqvga qvga 240 hvga vga svga xga uxga; the default `240` is the
;;; historical hardcoded value, so a target that says nothing behaves exactly as before.
;;;
;;; WHY A STATIONARY RECOGNISER WANTS A BIG ONE, measured on THIS board (the 2026-09-24 numbers
;;; below it were taken on the moving vehicle, where the answer is different):
;;;
;;;     svga  800x600    ~24.8 KB JPEG        uxga 1600x1200   ~110 KB JPEG
;;;
;;; and against MegaDetector v5a, the same framed animal faces score 0.27 / 0.38 / 0.55 at
;;; vga / svga / uxga while QVGA produces **NO DETECTION AT ALL** -- not a weak one.  So on a board
;;; that does not have to steer, resolution is the whole game and the frame rate is nearly free to
;;; spend: an animal in front of a camera is there for seconds.
;;;
;;; MEASURED 2026-10-14 ON THIS BOARD, AND IT CONTRADICTS WHAT THE MANIFEST AND THE COMMENT ABOVE
;;; STILL IMPLY: uxga initialises AFTER loading hud-car.scm and llip-vision.scm, with the radio up
;;; and associated.  Largest free internal-DMA block 59,380 B; the two files together cost 232 B.
;;; [B608]'s margin was ~0 when it was written and is ~59 KB now -- [31d01dc] moved ~1,600 interned
;;; symbol names to PSRAM and took this board from 3,364 bytes free at boot to 57,624.  The hazard
;;; was real and is not triggering; the ORDERING above is kept anyway, because it costs nothing and
;;; the pool is still shared.  Do not read the note above as a reason to avoid a large frame -- read
;;; it as the reason the size is chosen HERE.
