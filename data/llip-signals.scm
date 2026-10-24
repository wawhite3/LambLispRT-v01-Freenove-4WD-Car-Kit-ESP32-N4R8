;;; Copyright 2026 by Frobenius Norm LLC 2026-06-21 15:38:00
(info "Loading llip-signals\n")
(define (robot-signal! event)
  (case event
    ((idle)        (led-base! (cycle (list 1000 (lambda () (WS2812.navLights))))))
    ((running)     (led-base! (led-blink led-signal-zone 'rgb-green 0 400 400)))
    ((dock)        (led-base! (led-blink led-signal-zone 'rgb-blue  0 400 400)))
    ((bad-move)    (led-emit! 2 (led-blink led-signal-zone 'rgb-red    3 120 120)))
    ((obstacle)    (led-emit! 3 (led-blink led-signal-zone 'rgb-red    5  80  80)))
    ((low-battery) (led-emit! 1 (led-blink led-signal-zone 'rgb-yellow 3 600 600)))
    ((deadman)     (led-emit! 4 (led-blink led-all 'rgb-red 0 100 100)))
    (else (error "robot-signal!: unknown event" event))))
(info "llip-signals Loaded\n")
