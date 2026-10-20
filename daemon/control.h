/*
 * Semibreve — the boundary between the D-Bus glue and the daemon.
 *
 * Two reasons this is a C header rather than a C++ one.
 *
 * sd-bus's vtable macros are built from designated initializers inside
 * anonymous unions, which is C99 and only awkwardly C++. Keeping the glue in
 * its own C translation unit avoids fighting that, and costs nothing.
 *
 * The better reason is that it draws the line in the right place. Everything
 * on the far side of this header is protocol: names, signatures, marshalling.
 * Everything on this side is the daemon, where the real-time rules apply. The
 * glue cannot reach DfxDsp, the streams, or the ring even by accident.
 *
 * Every function here is called on the main loop thread and only there.
 */
#ifndef SEMIBREVE_CONTROL_H
#define SEMIBREVE_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

/* The daemon, opaque to the glue. */
typedef struct SbControl SbControl;

/* Order matches DfxDsp::Effect, and the names FxSound uses. They are baked
 * into the preset files, so keeping them means schema, format and labels all
 * describe the same thing. */
enum {
	SB_FIDELITY = 0,
	SB_AMBIENCE,
	SB_SURROUND,
	SB_DYNAMIC_BOOST,
	SB_BASS,
	SB_NUM_EFFECTS
};

/* Effect values are the 0..10 the UI shows, not the DSP's internal scale. */
int    sb_set_effect(SbControl *c, int effect, double value);   /* 0 = out of range */
double sb_get_effect(SbControl *c, int effect);

int    sb_get_bypass(SbControl *c);
void   sb_set_bypass(SbControl *c, int on);

/* Takes a bare preset name or a path. 0 = no such preset. The change is
 * deferred by a few milliseconds so it can land under a fade; the reply goes
 * out immediately, because it reports that the request was accepted. */
int         sb_load_preset(SbControl *c, const char *name);
const char *sb_preset_id(SbControl *c);      /* "Jazz", "6" — stable identifier */
const char *sb_preset_name(SbControl *c);    /* "Classic Processing" — for display */

/* The stock presets, enumerated once at startup. */
int         sb_preset_count(SbControl *c);
const char *sb_preset_list_id(SbControl *c, int i);
const char *sb_preset_list_name(SbControl *c, int i);

/* Which device our processed audio goes to. The name is a PipeWire node name
 * such as "alsa_output.usb-...-00.analog-stereo"; the description is what a
 * person should see. Changing it relinks the output under a fade. */
const char *sb_output_device(SbControl *c);           /* current node name */
int         sb_set_output_device(SbControl *c, const char *name);  /* 0 = unknown */
int         sb_output_count(SbControl *c);
const char *sb_output_name(SbControl *c, int i);      /* node name */
const char *sb_output_description(SbControl *c, int i);

/* Stop the daemon. The UI owns this process's lifetime — quitting the app must
 * take the sink away with it, or the user has "exited" something that is still
 * their default output device. */
void   sb_quit(SbControl *c);

int    sb_num_eq_bands(SbControl *c);
double sb_eq_frequency(SbControl *c, int band);
double sb_eq_gain(SbControl *c, int band);
int    sb_set_eq_gain(SbControl *c, int band, double db);   /* 0 = bad band */

/* ---- the glue, from the daemon's side --------------------------------- */

typedef struct SbBus SbBus;

/* Connects to the session bus and claims the name. Returns NULL on failure
 * with a message on stderr; the daemon carries on without a control surface
 * rather than refusing to pass audio. */
SbBus *sb_bus_new(SbControl *c);

/* The fd to watch, and the pump to call when it is readable. */
int  sb_bus_fd(SbBus *b);
int  sb_bus_process(SbBus *b);
void sb_bus_free(SbBus *b);

/* Tell subscribers a property moved. Called after the daemon changes something
 * itself — a deferred preset landing, say — so a UI does not have to poll. */
void sb_bus_emit_changed(SbBus *b, const char *property);

#ifdef __cplusplus
}
#endif

#endif /* SEMIBREVE_CONTROL_H */
