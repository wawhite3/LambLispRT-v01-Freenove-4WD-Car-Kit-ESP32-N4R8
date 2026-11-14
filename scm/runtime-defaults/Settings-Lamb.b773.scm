;;; Copyright 2026 by Frobenius Norm LLC 2026-10-31
; Settings-Lamb.b773.scm -- the [B773] reproduction geometry, as a PIN.
;
; Staged to Settings-Lamb.scm from a target manifest, the idiom Settings-Lamb.4wd.scm uses:
;     file: runtime-defaults/Settings-Lamb.b773.scm -> Settings-Lamb.scm
;
; WHY A PIN AND NOT AN EDIT TO data_staged.  b773_real_fixture.sh wrote the pin into
; data_staged/<env>/ and then called `w3 make flash`, which runs w3_assemble FIRST -- regenerating
; data_staged from scm/ and discarding the pin.  Ten boots on esp32s3-n32r16-00 scored
; WRONG-FIXTURE at blocks=1/64 pool=1220608 because the board had the DEFAULT geometry.  A pin is
; delivered BY assemble, so it cannot be erased by it.
;
; THE GEOMETRY, from w3_logs/b773-repro-20261015/perf-arch-LL-c5-01-20261015_141357.log:
;   gc-nblocks-warm 1   gc-pool-cells 524288   gc-yuasa-N 659967 (the untouched boot seed)
; 524288 cells x 12 bytes = 6.3 MB, so this needs an 8 MB PSRAM part or larger.
;
; max_cell_blocks lives in Settings.scm and is NOT pinned here; with a 6.3 MB initial block the
; recorded blocks=1/1 comes from the block size, not from the cap.  Confirm it on the board:
; the arm reads back gc-nblocks-warm and refuses to measure if it is not 1.
((va_list_max . 8192)
 (cell_block_size . 524288))
