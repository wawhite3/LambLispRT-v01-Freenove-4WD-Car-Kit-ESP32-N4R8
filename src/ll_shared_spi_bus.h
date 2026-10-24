// Copyright 2026 by Frobenius Norm LLC 2026-10-07
#ifndef LL_SHARED_SPI_BUS_H
#define LL_SHARED_SPI_BUS_H

/*! @file
    @brief Serialises the two independent users of one set of SPI pins.

    On boards where the SD card socket and a display controller's serial
    initialisation share the same clock and data lines, the two are used by
    unrelated subsystems that know nothing about each other.  Each works
    perfectly alone.  Used at the same instant they corrupt each other, and the
    observable result is either a card I/O error or a scrambled display -- in
    both cases a symptom that does not name the other subsystem as the cause.

    Display initialisation normally runs once at startup, so in ordinary
    operation the two never meet.  The hazard is that re-initialising a display
    is a legitimate runtime operation, and a card transfer may be in flight when
    it is requested.

    `take` blocks for up to @p timeout_ms and returns false rather than
    proceeding without the bus, so a caller that cannot wait can say so instead
    of producing a corrupt transfer.  Every successful `take` must be matched by
    a `give`.

    WHEN NO SHARED USER IS COMPILED IN the calls below become inline no-ops, so
    a board without the SD feature carries no lock, no mutex and no behavioural
    change whatsoever.
*/

#if LL_SD_SPI

//! Acquire the shared bus.  @param who names the caller in diagnostics.
//! @return true if the bus is held on return; false on timeout.
bool ll_spi_bus_take(const char *who, unsigned timeout_ms);

//! Release the shared bus.  Only call after a `take` that returned true.
void ll_spi_bus_give(const char *who);

#else

static inline bool ll_spi_bus_take(const char *, unsigned) { return true; }
static inline void ll_spi_bus_give(const char *) { }

#endif

#endif /* LL_SHARED_SPI_BUS_H */
