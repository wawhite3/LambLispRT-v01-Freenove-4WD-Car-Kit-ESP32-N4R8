; Copyright 2026 by Frobenius Norm LLC 2026-10-31
; Settings-Lamb.scm for the Freenove 4WD Car Kit -- pre-heap GC config, read by the C++
; pre-reader (LambPreSettings::load) before the heap exists.  [B1030]
;
; WHY THIS TARGET HAS ITS OWN COPY, when every other target shares runtime-defaults/
; Settings-Lamb.scm: the pre-reader opens exactly ONE path (LAMB_SETTINGS_PATH =
; "/Settings-Lamb.scm"), so a per-target pool size can ONLY be stated by staging a
; different source file to that name.  `Settings-env.scm` is not consulted for these keys
; -- it is read by Scheme, long after the pool has been built.
;
; cell_block_size PINS the initial pool to a cell count, which is what this board needs and
; what the generic computation cannot know.  The default rule ([B1024]) is "all available
; memory less the heap-object reserve", with the reserve the larger of mem/8 and a 512 KB
; floor ([B1029]).  On this part that is max(4192124/8, 524288) = 524288 B, and the pool
; takes the remaining 37 x 8192 = 303,104 cells.
;
; THAT RESERVE IS A VM-LEVEL GUESS AND THIS BOARD IS AN APPLICATION RUNNER, which is the
; whole defect: the reserve has to cover what runs ON TOP of the VM, and here that is WiFi
; plus lwIP plus the camera, the vision codec, two listeners (TCP REPL 3141, LLIP 8080) and
; the bytecode for 304 auto-compiled procedures.  Measured with the radio on, the board
; finished booting with 19,424 B of heap left and the next lwIP allocation failed --
;   assert failed: sys_timeout_abs ... pool MEMP_SYS_TIMEOUT is empty
; -- which with MEMP_MEM_MALLOC=1 means mem_malloc returned NULL, not that lwIP ran out of
; timers.  The board then rebooted every ~18 s, continuously.  The same build with the radio
; OFF booted once and stayed up, with 51 KB more free after setup.scm -- so the shortfall is
; tens of KB, not megabytes.
;
; 131072 = 16 x 8192, AND IT IS THE NUMBER THIS BOARD ACTUALLY BUILDS.  It was 262144 until
; 2026-11-01, which this part cannot allocate: measured on the board, a 262144 request produced
; `Ncells 131072` -- [B248]'s ladder halved it once and succeeded, so the pin had never been
; honoured and nobody had read Ncells back to notice.  Total pool memory here is ~3.83 MB
; (1.57 MB of cells plus 2,253,176 B free), so 262144 cells = 3.1 MB was never available.
;
; THE POINT IS A SETTING THAT WORKS ON THIS BOARD, NOT A PIN THAT GETS MET (owner, 2026-11-01).
; Asking for a figure the ladder has to rescue is strictly worse than asking for the figure that
; allocates: same pool either way, minus the warning, the `ll_cell_block_size_effective` override
; record, and a settings file that disagrees with the running system.
;
; WHY A PIN AT ALL, GIVEN THE DEFAULT IS "ALL MEMORY LESS pool_reserve_pct".  Because the reserve
; is only used to COMPUTE the pool, so a pinned board bypasses it entirely -- and the computed
; value here would be ~262144 again, i.e. the figure that fails.  Expressing this board's need as
; a reserve percentage instead would need ~58%, which is a true statement about the application
; ([B1030]: WiFi + lwIP + camera + vision codec + two listeners + bytecode for 304 procedures) but
; makes the built pool depend on how much PSRAM happens to be free at boot, flipping a whole
; expansion block either way.  The pin is deterministic and measured; revisit if the application
; changes.  CONFIRM WITH Ncells AFTER ANY CHANGE -- that is how this one was found.
((va_list_max . 8192)
 (cell_block_size . 131072))
