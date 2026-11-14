;;; Copyright 2026 by Frobenius Norm LLC 2026-08-04 00:00:00
;;; Free for non-commercial use. Commercial use requires a license.
;;; Pin map for the standalone Freenove ESP32-WROVER-CAM (classic ESP32-D0WD)
;;; mounted on a Freenove ESP32 GPIO-extension board / breadboard panel.
;;; Shipped to the device as Pins.scm by this target's manifest -- NO runtime
;;; board dispatch.
;;;
;;; The WROVER-CAM MODULE pinout is identical to the 4WD car's brain (same
;;; module + OV2640 camera), so the camera / flash / core-peripheral pins below
;;; match Freenove-4WD-Car-Kit-ESP32.scm.  What differs is the CHASSIS vs
;;; breadboard wiring: tune the breadboard/extension-board peripheral pins
;;; (buzzer / WS2812 / sonar / photo / i2c) HERE without touching the 4WD map.
;;;
;;; SONAR ECHO IS 13 AND I2C SDA IS 15 -- THE REVERSE OF THE 4WD, DELIBERATELY (2026-10-16).
;;; The 4WD values (echo 15, SDA 13) were copied here when this map was forked and described no
;;; wiring: nothing was connected.  They are swapped because THIS MODULE ALSO HAS AN SD CARD
;;; (env Freenove-ESP32-WROVER-sd), and on a classic ESP32 `soc_caps.h` reports
;;; `SOC_SDMMC_USE_IOMUX` -- the SD host is hard-wired to CLK 14 / CMD 15 / DAT0 2 and cannot be
;;; routed.  Echo on 15 would therefore make the sonar and the card mutually exclusive on ONE
;;; PHYSICAL BOARD that two envs both flash.  Echo on 13 costs nothing and removes that choice.
;;;
;;; WHY 13 AND NOT 0, 32 OR 33 -- the free pool here is only {0 2 12 13 14 15 32 33}; everything
;;; else is SPI flash (6-11), PSRAM on the WROVER module (16/17), the camera, or the console UART.
;;;   13  MTCK, no boot strapping function   -> safe for an input that idles low.  CHOSEN.
;;;    0  pulls UP at reset; LOW at reset enters DOWNLOAD MODE.  An HC-SR04 ECHO IDLES LOW, so a
;;;       sonar on GPIO 0 stops the board booting.  Never put echo here.
;;;   32  WS2812 (consumed by core/WS2812.scm) -- in use.
;;;   33  photoresistor / ADC (core/LightSensor.scm, the Leds.scm fuel gauge) -- in use.
;;; TRIG STAYS ON 12 and that pin is NOT free of constraints: GPIO 12 is MTDI and selects the flash
;;; voltage at reset -- HIGH there picks 1.8 V and a 3.3 V WROVER will not boot.  An HC-SR04 TRIG
;;; input is high-impedance and 12 pulls down at reset, so it is fine; an I2C line, which needs a
;;; pull-up, is NOT.  That is why i2c took 15 rather than 12.
;;;
;;; SDA ON 15 IS FINE: GPIO 15 is MTDO and pulls UP at reset, which is the idle state of an I2C
;;; line anyway; LOW at reset only suppresses the boot log.  Only `pins-i2c` and `pins-sonar` are
;;; read by anything -- `pin-SDA0` / `pin-SCL0` have no consumer outside this file -- so keep the
;;; pair and the individual entries in step by hand.

(define Pins
  (alist->dict '(
		 (pin-IR     . 0)
		 (pin-U0TXD  . 1)
		 (pin-buzzer . 2)
		 (pin-U0RXD  . 3)
		 (pin-CSI_Y2 . 4)
		 (pin-CSI_Y3 . 5)

		 (pin-FLASH_CLK . 6)
		 (pin-FLASH_D0  . 7)
		 (pin-FLASH_D1  . 8)
		 (pin-FLASH_D2  . 9)
		 (pin-FLASH_D3  . 10)
		 (pin-FLASH_CMD . 11)

		 (pin-sonar-trig . 12)
		 (pin-sonar-echo . 13)
		 (pin-SCL0       . 14)
		 (pin-SDA0       . 15)

		 (pin-CSI_Y4    . 18)
		 (pin-CSI_Y5    . 19)

		 (pin-XCLK      . 21)
		 (pin-PCLK      . 22)
		 (pin-HREF      . 23)

		 (pin-CSI_VYSNC . 25)
		 (pin-SIOD      . 26)
		 (pin-SIOC      . 27)

		 (pin-WS2812 . 32)
		 (pin-ADC    . 33)
		 (pin-CSI_Y8 . 34)
		 (pin-CSI_Y9 . 35)
		 (pin-CSI_Y6 . 36)
		 (pin-CSI_Y7 . 39)

		 (pins-i2c   . (15 14))
		 (pins-sonar . (12 13))
		 (pin-photo  . 33)
		 )
	       )
  )
