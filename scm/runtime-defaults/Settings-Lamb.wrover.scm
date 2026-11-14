;;; Copyright 2026 by Frobenius Norm LLC 2026-11-01
; Settings-Lamb.wrover.scm -- pre-heap GC config for the Freenove WROVER (and its -noradio / -sd
; variants, which extend that manifest).  Staged as Settings-Lamb.scm; see that file for every key.
;
; pool_reserve_bytes 1.5 MB: with this board's camera + vision config (Settings-env.watcher) boot
; alone uses ~1 MB of non-cell PSRAM, so the 25% default (~1 MB here) left ~5 KB free at idle -- too
; little for an LLIP transfer once payloads stopped spilling into internal DRAM.  [B1041]
((va_list_max . 8192)
 (pool_reserve_bytes . 1572864))
