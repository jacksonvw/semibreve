/*
 * Semibreve — the control surface, absent.
 *
 * Compiled in place of dbus.c when libsystemd's headers are not installed, for
 * the same reason build.sh skips semibreved without pipewire-devel: the layers
 * below must never become unbuildable because a layer above them lacks a
 * development package. A daemon built this way passes audio and honours
 * --preset; it just cannot be talked to while it runs.
 *
 * The daemon already treats a missing bus as non-fatal, so there is nothing to
 * special-case at the call sites: sb_bus_new() returning NULL is a path it
 * takes anyway when there is no session bus.
 */

#include "control.h"

#include <stdio.h>
#include <stddef.h>

SbBus *sb_bus_new(SbControl *c)
{
	(void)c;
	fprintf(stderr, "semibreved: built without libsystemd — no D-Bus control "
	                "surface (install systemd-devel and rebuild)\n");
	return NULL;
}

int  sb_bus_fd(SbBus *b)                                 { (void)b; return -1; }
int  sb_bus_process(SbBus *b)                            { (void)b; return 0; }
void sb_bus_emit_changed(SbBus *b, const char *property) { (void)b; (void)property; }
void sb_bus_free(SbBus *b)                               { (void)b; }
