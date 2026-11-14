;;; Settings.scm -- LambLisp runtime heap configuration.
;;; Copyright 2026 by Frobenius Norm LLC 2026-05-23 00:00:00
;;; Free for non-commercial use. Commercial use requires a license.
;;;
;;; Loaded by setup.scm at startup.  Keys:
;;;   n_initial_blocks     -- expand heap to this many blocks before loading Scheme files
;;;   (extension_block_size is in Settings-Lamb.scm -- it is PRE-HEAP.  It was ALSO here, at 4096,
;;;    and setup.scm applied it AFTER the library loads at lines 54-62, so the Scheme value took
;;;    effect only for expansions after boot while block 0 was already aligned to the pre-heap
;;;    8192.  One key, two values, two granularities inside one boot: the pool reached sizes like
;;;    28,672 = 24,576 + 4,096, a multiple of neither.  REMOVED 2026-11-09, [B1072] C1.)
;;;   gc_budget_ns         -- GC work budget per allocation event in ns (default 25000)
;;;   gcload_target_pct    -- GC load target as pct of allocation rate (default 50)
;;;   gc_idle_deadline_ms  -- hard loop ceiling (ms); spare time below it is donated to GC
;;;                          lookahead (loop-deadline-ms; runtime-settable; default 10)
;;;   max_cell_blocks      -- cap on GC block count before memory_cap_reached warning (default 64)
;;;                          (cell_block_size is in Settings-Lamb.scm; requires reboot to change)
;;;   wifi                 -- 1 = auto-connect WiFi at boot, 0 = do not (default 0; saves internal DRAM)
;;;   wifi_ssid / wifi_pass-- credentials used when wifi=1 (strings)

((n_initial_blocks     . 1)
 (gc_budget_ns         . 25000)
 (gcload_target_pct    . 50)
 (gc_idle_deadline_ms  . 10)
 (max_cell_blocks      . 64)
 (wifi                 . 0))
