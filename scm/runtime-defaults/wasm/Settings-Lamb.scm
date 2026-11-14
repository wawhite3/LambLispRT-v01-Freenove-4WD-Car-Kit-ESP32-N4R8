;;; Copyright 2026 by Frobenius Norm LLC 2026-09-16
;;;
;;; Settings-Lamb.scm for the BROWSER demo (wasm32_browser) -- pre-heap GC config, read by the C++
;;; pre-reader before the heap exists.  Staged over the shared runtime-defaults/Settings-Lamb.scm
;;; by scm/targets/wasm32_browser.manifest; every other target keeps the shared 8K default.
;;;
;;; WHY THE BROWSER STILL HAS ITS OWN FILE (B437, revised 2026-10-29 for [B1024]).  It no longer
;;; names a pool: the VM sizes the initial pool from available memory less the heap-object reserve,
;;; which on a browser tab is ample, so the 65,536-cell constant this file used to ship is gone
;;; (owner: *"nothing should get shipped with 65536"*).  What remains target-specific is the
;;; EXPANSION GRANULARITY.
;;;
;;; THE DEFECT B437 RECORDED IS WHAT THE COMPUTED DEFAULT FIXES.  On the shared 8K default, merely
;;; LOADING the staged .scm library outran the collector and printed
;;;     LambMemoryManager::gc_urgent() INCREMENTAL GC FELL BEHIND (synchronous catch-up)
;;; on the customer-facing demo page -- an unbounded pause, the one thing the real-time claim
;;; forbids, on the TERM channel where `quiet` cannot hide it.
;;;
;;; AND THE BROWSER CANNOT LEARN ITS POOL THE WAY A BOARD DOES.  The GC rewrites Settings on a heap
;;; expansion and a board reads it back next boot; the demo's /data is MEMFS, rebuilt from the
;;; preloaded image on every page load, so that write is discarded and it ALWAYS cold-starts.  A
;;; computed default is therefore the only mechanism that can work here -- it needs no memory of
;;; the last run, which is exactly what this target does not have.
;;;
;;; DELIBERATELY NO gc_total_marked.  Shipping the measured live set would size the heap perfectly
;;; and go stale silently as the library grows -- a hand-maintained copy of a figure the GC
;;; measures for itself.

((extension_block_size . 16384)
 (va_list_max . 8192))
