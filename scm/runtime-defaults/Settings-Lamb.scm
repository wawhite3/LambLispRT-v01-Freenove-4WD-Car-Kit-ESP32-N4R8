; Settings-Lamb.scm -- pre-heap GC config (read by the C++ pre-reader before the heap exists).
; Shipped as a runtime default so it ALWAYS exists on the FS (no "failed to open" at boot);
; the GC may rewrite it. Reboot required to apply cell_block_size, which is ABSENT HERE on
; purpose: with no explicit value the VM sizes the initial pool from available memory less the
; heap-object reserve, in whole extension_block_size units ([B1024]).  A value pins it instead.
;
; DO NOT PUT A PER-TARGET VALUE IN THIS FILE -- IT IS SHARED BY EVERY TARGET ([B1030]).  The C++
; pre-reader opens exactly ONE filename (LAMB_SETTINGS_PATH = "/Settings-Lamb.scm"), so for a long
; time this file was the only place these keys could live and a board needing its own pool had
; nowhere to say so.  It does now: stage a different SOURCE file to this NAME from that target's
; manifest, the same `file: SRC -> DEST` idiom Settings-env uses --
;     file: runtime-defaults/Settings-Lamb.4wd.scm -> Settings-Lamb.scm
; Settings-env.scm CANNOT carry these keys: it is read by Scheme, long after the pool is built at
; ~200 ms.  Worked example and the measurement behind it: Settings-Lamb.4wd.scm, where a 512 KB
; reserve did not cover WiFi + lwIP + a camera + a vision codec + two listeners + bytecode for 304
; procedures, and the board finished booting with 19 KB of heap and rebooted every ~18 s.
;
; pool_reserve_pct -- PERCENT of pool memory held back from the cell pool for heap objects:
;   strings, bytevectors, bignum digits, vectors and bytecode bodies, which come out of the same
;   PSRAM the cell blocks do.  DEFAULT 25.  Clamped on load to [1, 90].  Read by both reserve sites
;   in ll_vm_mem.cpp, so the pool cannot be built to a size expand() then refuses to grow into.
;
;   THIS IS THE KNOB TO TURN IF A BOARD RAN OUT OF HEAP -- better than pinning cell_block_size,
;   which has to be re-derived per part.  The right value is a property of the APPLICATION, not the
;   chip.  No floor: a target that says 5 gets 5.  [B1030]
;
; pool_reserve_bytes -- the same reserve stated ABSOLUTELY.  Wins over pool_reserve_pct when > 0;
;   0 (the default) means use the percent.  Capped at use to 90% of the pool.  Use bytes when the
;   demand is absolute -- a camera frame buffer, a TLS record, a fixed codec working set -- and the
;   percent when the setting should travel across parts of different sizes.  [B1030]
;
;   A ZERO RESERVE CANNOT BE SAID IN BYTES: 0 IS the unset test, so it is indistinguishable from
;   the key being absent and you get the percent instead.  The nearest expressible value is
;   (pool_reserve_pct . 1), since pct is clamped to [1, 90] on load.  Both knobs refuse to leave
;   the payload allocators nothing, deliberately -- a pool with no payload headroom fails as a NULL
;   from whichever string or bignum asked next, not as a pool error.  [B1030]
;
; THE GC REWRITES THIS FILE IN FULL (on the first heap expansion), preserving every key it finds.
; Before B417 it wrote gc_total_marked ALONE, truncating the rest away -- if you are reading an
; older board's file and a setting you made has vanished, that is why.
;
; va_list_max -- cap on ONE formatted log/error message; longer text is TRUNCATED, not an error.
;   Clamped on load to [1024, 65536].  The ceiling is autobuf_t's own limit, past which the
;   formatter would RAISE while formatting -- i.e. throw while building the message the throw
;   needs.  Raise this if diagnostics are being clipped; it costs one buffer, grown on demand.
((va_list_max . 8192))
