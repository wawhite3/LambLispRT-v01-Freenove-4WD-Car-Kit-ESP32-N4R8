;;; Copyright 2026 by Frobenius Norm LLC 2026-10-05
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; hud-species.scm -- [P260] the PANEL half of animal identification: render a RESULT, not an image.
;;;
;;; The vehicle sends frames to the Jetson; the Jetson sends this panel a short record and the panel
;;; draws what was recognised.  Owner, 2026-10-05: "the display can even be a caricature outline
;;; skeleton or other indicator rather than an image."
;;;
;;; WHY THE RECORD AND NOT THE PICTURE.  A 640x480 JPEG is ~15.8 KB and a result is a few dozen
;;; bytes, so the panel-bound traffic falls by three orders of magnitude -- and that traffic is what
;;; starves the RGB scan DMA and shows as flicker (see ll_xmop3_Panel.cpp).  It is also simply more
;;; useful: "barred owl 0.87" is legible across a room and a 240x240 thumbnail is not.
;;;
;;; THE WIRE RECORD, line-framed like hud-telemetry so one reader can dispatch on the head:
;;;
;;;     (vision-id GROUP "common name" CONFIDENCE X Y W H)
;;;
;;; GROUP is a symbol from `species-groups` below and selects the silhouette.  CONFIDENCE is 0..100
;;; as an exact integer -- a float costs its printed form on the wire and parsing on a board that is
;;; busy scanning a display, for precision nobody can read off a bar.  X Y W H is the detection box
;;; in the CAPTURED frame's coordinates; the panel scales it to whatever it is drawing on, so the
;;; sender never needs to know the display geometry.
;;;
;;; WHY GROUP IS ON THE WIRE RATHER THAN DERIVED HERE.  A classifier unsure between red fox and gray
;;; fox is still certain it is a canid.  Sending the group lets the panel draw an honest canid when
;;; the species is a coin flip, instead of showing a top-1 label as though it were a fact.  The panel
;;; cannot work that out from a name it may not recognise.

(syslog "Loading HUD species\n")

;;; ---------------------------------------------------------------------------------------------
;;; SILHOUETTES.  Each is a closed outline as a flat (x y x y ...) list in a 100x100 box, scaled at
;;; draw time.  A point list rather than a bitmap: tens of bytes, recolourable, and scalable without
;;; a second asset -- forty-odd northeastern species cost a few KB of Scheme instead of a sprite
;;; sheet this filesystem has no room for.
;;;
;;; THESE ARE CARICATURES ON PURPOSE.  The job is to be recognisable at a glance from across a room,
;;; not to be an accurate drawing: exaggerated ears, legs and tail carry more at 100 px than correct
;;; proportions do.

(define sil-deer
  (list 18 95  25 62  22 40  35 32  42 8  50 30  70 28  82 6  90 30  96 48
        94 88  86 95  82 60  60 62  45 58  38 90  33 95))

(define sil-canid                      ;;;!< fox, coyote: low body, long brush tail, pricked ears
  (list 10 92  14 60  8 44  20 48  26 28  34 48  52 44  72 46  86 30  95 40
        88 56  84 88  78 92  76 62  46 64  30 62  24 90))

(define sil-raptor                     ;;;!< hawk, owl: upright, broad head, hooked front
  (list 50 8  66 16  72 34  70 60  78 78  66 94  50 88  34 94  22 78  30 60
        28 34  34 16))

(define sil-rodent                     ;;;!< squirrel, chipmunk: hunched, big tail arc
  (list 30 92  26 66  32 50  44 44  52 30  62 42  74 48  80 66  76 90
        62 92  58 70  44 70  40 92))

(define sil-wading                     ;;;!< heron: long neck, long legs, dagger bill
  (list 44 96  46 56  38 40  34 20  52 10  66 14  96 8  70 22  64 40
        76 54  72 86  66 96))

(define sil-turtle
  (list 12 70  18 48  34 34  58 32  76 40  88 56  86 72  70 82  40 84  20 80))

;;; FROG AND TOAD SHARE ONE OUTLINE, AND FOR TOADS IT IS ALL THERE IS.  The classifier's whole
;;; label set holds four amphibian labels -- `cane toad`, `rana species`, `amphibian`, `frogs` --
;;; and no salamanders; `cane toad` is the only Bufonidae and the New England fence correctly
;;; removes it.  So an American toad arrives as `frogs` or `amphibian`, which is true and is not
;;; its name.  This silhouette is therefore the whole of what the panel can say about a toad, and
;;; a toad is a thing this site actually has (owner, 2026-10-15).
(define sil-amphibian                  ;;;!< frog, toad: squat, splayed limbs
  (list 20 80  12 60  24 48  38 36  56 34  72 44  86 58  80 78  64 70  48 74  34 72))

;;; SNAKE -- tail tapering to a point at the lower left, two body bends, and a HEAD WIDER THAN THE
;;; NECK at the upper right.  The head is the load-bearing part: without it a curved band reads as
;;; a cable or a branch at 100 px, which was the first two attempts.  Garter snakes are what this
;;; is for (owner, 2026-10-15), and the classifier reaches it by FAMILY -- Natricidae (Thamnophis)
;;; and Colubridae -- because `squamata` is snakes AND lizards and would draw this for a skink.
(define sil-snake
  (list 8 85  22 93  40 88  49 70  55 55  64 47  75 48  83 38  84 37  89 36  90 27  85 24  77 28
        78 32  77 34  73 40  60 37  45 49  39 66  32 80  22 87  8 83))

;;; THE FOUR COMMONEST NORTHEASTERN SUBJECTS THAT HAD NO OUTLINE UNTIL 2026-10-15, and three of
;;; them are what this site produces: the 13-hour watch of 2026-10-14 returned 1,552 animal records
;;; and every one was a CAT, which until now drew the generic blob.
;;;
;;; EACH EXAGGERATES THE ONE FEATURE THAT SEPARATES IT FROM ITS NEIGHBOURS IN THIS SET, because at
;;; 100 px that feature is the whole of what a viewer gets: the cat's pricked ears and upright
;;; tail, the raccoon's thick brush swept up behind, the opossum's long snout and NAKED thin tail
;;; (the one cue that tells it from the raccoon), the rabbit's ears.
;;;
;;; AND THEY ALL HAVE LEGS.  The first attempt drew the raccoon and the opossum as smooth humps and
;;; both read as a blob -- the deer and canid outlines already here have legs, and that is why they
;;; work.  Judged by rendering each one and looking at it, not by reading the numbers.
(define sil-felid
  (list 26 96  24 78  26 62  22 48  24 34  18 22  28 28  34 16  42 28  52 30  56 42  54 56
        58 72  64 84  72 88  78 76  80 58  84 40  90 26  94 34  90 52  88 70  82 86  70 96
        50 98  36 98))

(define sil-raccoon
  (list 4 66  12 58  22 52  30 40  42 34  54 34  64 40  70 50  78 46  88 38  96 34  94 46  86 56
        78 64  74 76  70 90  62 90  64 76  56 70  48 90  40 90  42 70  32 70  26 90  18 90
        22 68  12 74))

(define sil-opossum
  (list 2 58  12 50  22 42  34 36  48 34  60 38  68 48  74 58  84 62  96 68  96 74  84 70  74 66
        72 80  68 92  62 92  64 76  54 70  48 90  42 90  44 70  34 70  28 90  22 90  26 66
        14 66))

(define sil-lagomorph
  (list 14 94  14 76  20 64  28 54  32 40  28 18  34 6  42 18  44 36  52 32  58 14  66 4  70 16
        64 38  74 48  86 58  92 74  88 90  80 94  78 80  68 82  62 94  54 94  56 80  40 84
        34 94  24 96))

(define sil-mustelid                   ;;;!< fisher, mink, otter: long low tube
  (list 8 70  16 54  30 50  54 52  76 50  92 44  96 56  86 64  62 68  36 70  20 76))

(define sil-bear
  (list 16 92  18 56  26 36  38 30  44 14  56 16  62 32  80 38  92 56  90 88
        80 92  76 62  48 60  30 62  28 90))

(define sil-unknown                    ;;;!< a question-mark bar: drawn when GROUP is not one we hold
  (list 40 20  60 20  60 50  50 50  50 66  50 80  50 86))

;;; GROUP -> silhouette.  Unknown groups render `sil-unknown` rather than nothing: a detection the
;;; panel cannot illustrate is still a detection, and showing nothing would read as "no animal".
;;; [P260] MEGADETECTOR'S OWN THREE CLASSES ARE GROUPS TOO.  Stage one detects animal/person/vehicle
;;; and does NOT identify species; stage two (a species classifier on the crop) supplies the rest.
;;; Without these entries a real stage-one detection renders as the UNKNOWN glyph, which is honest
;;; but useless -- found 2026-10-06 by running the actual engine and watching a 97%-confidence
;;; animal come out as a question mark.
;;;
;;; `animal` deliberately gets its OWN generic outline rather than borrowing the deer: the detector
;;; has not said deer, and drawing one would assert something it did not claim.
(define sil-animal                     ;;;!< generic quadruped: stage one knows it is an animal, no more
  (list 14 82  18 56  28 48  46 46  66 48  84 52  90 70  86 82
        78 84  76 60  52 58  34 58  30 84))

(define sil-person
  (list 50 10  58 22  58 34  72 46  58 40  58 62  64 92  52 92  50 66
        48 92  36 92  42 62  42 40  28 46  42 34  42 22))

(define sil-vehicle
  (list 10 70  14 52  30 46  40 32  66 32  78 46  92 52  94 70  82 70
        78 78  66 78  62 70  38 70  34 78  22 78  18 70))

(define species-groups
  (list (cons 'animal sil-animal)   (cons 'person sil-person)
        (cons 'vehicle sil-vehicle)
        (cons 'deer sil-deer)       (cons 'canid sil-canid)
        (cons 'raptor sil-raptor)   (cons 'rodent sil-rodent)
        (cons 'wading sil-wading)   (cons 'turtle sil-turtle)
        (cons 'amphibian sil-amphibian) (cons 'snake sil-snake)
        (cons 'mustelid sil-mustelid) (cons 'bear sil-bear)
        (cons 'felid sil-felid)     (cons 'raccoon sil-raccoon)
        (cons 'opossum sil-opossum) (cons 'lagomorph sil-lagomorph)))

(define (sil-for group)
  (let ((hit (assq group species-groups)))
    (if hit (cdr hit) sil-unknown)))

;;; Scale a 100x100 outline into a box at (ox,oy) of side `side`.
(define (sil-scale pts ox oy side)
  (let loop ((p pts) (odd #f) (acc (list)))
    (if (null? p)
        (reverse acc)
        (loop (cdr p) (not odd)
              (cons (+ (if odd oy ox) (quotient (* (car p) side) 100)) acc)))))

;;; ---------------------------------------------------------------------------------------------
;;; THE WIDGETS.  Created once and REUSED -- creating them per detection would exhaust the 48-slot
;;; LVGL handle table in under a minute of sightings, and the failure would be a silent #f from
;;; lv-line-create rather than anything that names the cause.

(define hud-sp-line  #f)
(define hud-sp-name  #f)
(define hud-sp-conf  #f)
(define hud-sp-ready #f)

(define (hud-species-init!)
  (if hud-sp-ready
      #t
      (let ((scr (lv-screen-active)))
        (set! hud-sp-line (lv-line-create scr))
        (set! hud-sp-name (lv-label-create scr))
        (set! hud-sp-conf (lv-bar-create scr))
        (if (and hud-sp-line hud-sp-name hud-sp-conf)
            (begin
              ;; LVGL'S DEFAULT LINE WIDTH IS 0, so a line with valid points draws NOTHING and
              ;; reports no error -- indistinguishable from being hidden behind something else.
              ;; Set it explicitly; see [P260] and the note at mop3_lv_line_style.
              (lv-line-style hud-sp-line 5 2016)
              ;; LAYOUT: THE TOP-LEFT IS TAKEN.  The HUD owns that corner for its link line, and a
              ;; label placed at 20,20 lands underneath it -- observed on the glass 2026-10-05 as
              ;; "silhouette yes, no animal name", which reads as a label that failed to render
              ;; rather than one that is simply behind something.
              ;; So the species name sits BELOW the centred image box (which spans roughly y 120-360)
              ;; and above the confidence bar, where nothing else draws.
              (lv-obj-set-pos hud-sp-name 20 372)
              ;; AN EXPLICIT COLOUR, for the same reason the line width is explicit: a label
              ;; inheriting a theme colour that matches the background is invisible and reports
              ;; nothing.  White on this panel's dark HUD.
              (lv-obj-set-style-text-color hud-sp-name 65535)
              (lv-obj-set-pos hud-sp-conf 20 430)
              (lv-obj-set-size hud-sp-conf 440 16)
              (lv-bar-set-range hud-sp-conf 0 100)
              (set! hud-sp-ready #t)
              #t)
            (begin (warn "hud-species: widget creation failed -- LVGL table full?\n") #f)))))

;;; ---------------------------------------------------------------------------------------------
;;; PHOTOGRAPHS.  Owner, 2026-10-16: *"i think the panel silhuettes is not great.  better to use a
;;; stock picture."*
;;;
;;; THE ARGUMENT THE SILHOUETTES WERE CHOSEN ON WAS SIMPLY WRONG.  This file said forty-odd species
;;; "cost a few KB of Scheme instead of a sprite sheet this filesystem has no room for".  Measured
;;; 2026-10-16: the storage partition is 3.31 MB and the staged filesystem uses 98 of 832 blocks,
;;; so about 90% of it is free, and a 240x240 JPEG costs under 4 KB.  Fifty of them are a quarter
;;; of one percent of the partition.  There was room all along.
;;;
;;; KEYED ON THE CLASSIFIER'S OWN NAME, so no second mapping can drift: [P260] stage two returns
;;; `white-tailed deer`, and the file is `sp/white-tailed-deer.jpg`.  Lower-case, non-alphanumerics
;;; collapsed to a dash.  A TAXON-LEVEL answer (`canis species`, `colubridae family`) finds no such
;;; file and falls through to the group photo `sp/_canid.jpg`.
;;;
;;; AND THE LINE DRAWING IS STILL THE LAST RESORT, DELIBERATELY.  `animal`, `person` and `vehicle`
;;; are not taxa and no photograph represents them; a generic "animal" picture would be a specific
;;; animal, which is the misreading this panel exists to avoid.  So the order is PHOTO OF THE
;;; SPECIES, then photo of the family, then the outline -- each step less specific than the last,
;;; which is the same honesty the group mechanism was built for.
;;;
;;; NO DECODE HAPPENS IF THE FILE IS ABSENT.  `file-exists?` is checked first, because a missing
;;; asset must leave the previous picture alone rather than blank the panel.

(define (hud-sp-slug name)
  (let loop ((i 0) (acc (list)) (dash #f))
    (if (>= i (string-length name))
        (list->string (reverse (if (and (pair? acc) (char=? (car acc) #\-)) (cdr acc) acc)))
        (let* ((c (string-ref name i))
               (d (char-downcase c))
               (ok (or (char-alphabetic? d) (char-numeric? d))))
          (cond (ok        (loop (+ i 1) (cons d acc) #f))
                (dash      (loop (+ i 1) acc #t))
                ((null? acc) (loop (+ i 1) acc #t))
                (else      (loop (+ i 1) (cons #\- acc) #t)))))))

(define (hud-sp-show-jpeg path)
  (and (file-exists? path)
       (let* ((n  (file-size path))
              (p  (open-binary-input-file path))
              (bv (read-bytevector n p)))
         (and bv (lvgl-frame-jpeg! bv)))))

;;; -> #t if a photograph was drawn, #f if the caller should fall back to the outline.
(define (hud-sp-picture! group name)
  (or (hud-sp-show-jpeg (string-append "sp/" (hud-sp-slug name) ".jpg"))
      (hud-sp-show-jpeg (string-append "sp/_" (symbol->string group) ".jpg"))))

;;; (hud-species-show! GROUP NAME CONF) -- draw one detection.
(define (hud-species-show! group name conf)
  (when (hud-species-init!)
    (if (hud-sp-picture! group name)
        (lv-line-set-points hud-sp-line (list 0 0 0 0))   ;;; photo shown: retire the outline
        (lv-line-set-points hud-sp-line (sil-scale (sil-for group) 150 120 240)))
    ;; COLOUR CARRIES THE CONFIDENCE TOO, because a bar and a number are both easy to not read.
    ;; Green above 70, amber 40-70, red below -- the same thresholds the bar is graded on.
    (lv-line-style hud-sp-line 5
                   (cond ((>= conf 70) 2016) ((>= conf 40) 65504) (else 63488)))
    (lv-label-set-text hud-sp-name name)
    (lv-bar-set-value hud-sp-conf conf)
    (syslog "species: ~a (~a) conf ~a\n" name group conf)))

;;; (hud-species-record! REC) -- dispatch one `(vision-id GROUP NAME CONF X Y W H)` record.
;;; Returns #t if it was ours.  A record we do not understand is NOT an error here: the panel shares
;;; this connection with telemetry and frames, and a reader that rejects what it does not own would
;;; drop the link for someone else's traffic.
(define (hud-species-record! rec)
  (if (and (pair? rec) (eq? (car rec) 'vision-id) (>= (length rec) 4))
      (begin (hud-species-show! (cadr rec) (caddr rec) (cadddr rec)) #t)
      #f))

;;; ---------------------------------------------------------------------------------------------
;;; (hud-species-demo! [MS]) -- cycle every silhouette with a plausible name and confidence.
;;;
;;; THIS IS THE SIMULATED RECOGNISER, and it exists so the whole result path can be exercised with
;;; NO Jetson and NO animals.  Everything from the wire record to the glass is real -- only the
;;; classifier is stubbed -- so the protocol, the layout, the handle budget and the legibility of
;;; the caricatures can all be settled before a model is chosen.  That ordering is deliberate: the
;;; parts that are cheap to get wrong and expensive to discover late are the ones not involving
;;; machine learning at all.
;;;
;;; The confidences are varied on purpose, spanning the colour thresholds (>=70 green, 40-69 amber,
;;; <40 red), so a reviewer can see what a WEAK detection looks like rather than only a strong one.
(define hud-species-demo-set
  (list (list 'deer      "white-tailed deer"  92)
        (list 'bear      "black bear"         78)
        (list 'canid     "red fox"            64)
        (list 'canid     "coyote"             38)
        (list 'mustelid  "fisher"             55)
        (list 'rodent    "eastern gray squirrel" 85)
        (list 'raptor    "barred owl"         71)
        (list 'wading    "great blue heron"   88)
        (list 'turtle    "painted turtle"     46)
        (list 'amphibian "American bullfrog"  33)
        (list 'unicorn   "something unlisted" 25)))

(define (hud-species-demo! . ms)
  (let ((hold (if (null? ms) 2500 (car ms))))
    (let loop ((xs hud-species-demo-set))
      (if (null? xs)
          (syslog "species demo: done\n")
          (let ((x (car xs)))
            (hud-species-show! (car x) (cadr x) (caddr x))
            (delay-ms hold)
            (loop (cdr xs)))))))

(syslog "HUD species loaded\n")
