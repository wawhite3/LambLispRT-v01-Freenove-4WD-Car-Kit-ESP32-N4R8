<!-- Copyright 2026 by Frobenius Norm LLC 2026-10-08 -->

# THIRD-PARTY BENCHMARK SOURCE -- NOT FROBENIUS NORM LLC WORK

Every `.scm` file in this directory is **third-party code**, included solely as a benchmark INPUT so
that LambLisp's performance can be stated beside other Scheme implementations' published figures.

**FROBENIUS NORM LLC CLAIMS NO OWNERSHIP OF THESE FILES AND LICENSES THEM TO NO ONE.**

## Why that sentence matters here, and is not boilerplate

Every other `.scm` in this tree carries `Free for non-commercial use.  Commercial use requires a
license.` -- a statement of OUR terms for OUR code.  **That line does not appear in these files and
must never be added to them.**  Applying our licence to someone else's work would be a
misstatement of terms, and it would do it in the most durable possible place: the full `scm/`
library is copied into every customer package, so anything written here is published to every
per-target repository.

## Provenance

Retrieved 2026-10-08 from `github.com/ecraven/r7rs-benchmarks` (`src/`), the commonly used R7RS
packaging of the Gabriel benchmark suite.  Authorship is as credited inside each file and is left
exactly as found -- principally Richard Gabriel's original suite, with the Scheme ports and
corrections attributed in the file headers (for `nboyer`: Bob Boyer, Bob Shaw, Will Clinger).

**THAT REPOSITORY CARRIES NO `LICENSE` FILE** (checked 2026-10-08: HTTP 404).  The Gabriel
benchmarks have circulated publicly and been redistributed by many Scheme implementations for
nearly forty years, but **no explicit grant of rights accompanies this copy**, and that is recorded
here rather than glossed, because a reader of a shipped package deserves to know which files in it
are ours and on what footing the rest sit.

Included on the owner's explicit decision (2026-10-08), after the absence of a licence file and the
fact that `scm/` ships to every customer were both put to them.

**No warranty of any kind is offered for these files, by Frobenius Norm LLC or otherwise.**  If a
rights holder objects to their inclusion, they will be removed on request.

## What was changed, and what was not

The ALGORITHMS and every DATA TABLE are verbatim -- that is the entire point, since a benchmark
whose tables differ from the canonical ones is not comparable to anyone's published numbers and
would defeat the reason for vendoring it.  Each file states its own modifications at the top, under
a banner that is clearly marked as added by us.  In every case the changes are confined to harness
glue:

* the `(import (scheme base) ...)` form, which this runtime does not use, is commented out rather
  than deleted, so the original text stays readable;
* the suite's `run-benchmark` driver, which reads its parameters from stdin via `(read)` and calls
  `run-r7rs-benchmark`, is replaced by a plain entry point taking its size as an argument.

**Nobody may edit the original notices in these files.**  They are someone else's copyright, the
same rule this tree already applies to the vendored chibi and CMSIS sources.
