/*
 * Semibreve — the D-Bus control surface.
 *
 *   bus name    org.semibreve.Daemon
 *   object      /org/semibreve/Daemon
 *   interface   org.semibreve.Daemon
 *
 * Session bus, not system: this is a per-user audio daemon and has no business
 * on a machine-wide bus.
 *
 * Nothing here touches the DSP, the streams or the ring. It marshals, it calls
 * through control.h, it marshals back. See that header for why the boundary is
 * drawn here.
 *
 * Try it without a UI:
 *
 *   busctl --user introspect org.semibreve.Daemon /org/semibreve/Daemon
 *   busctl --user get-property org.semibreve.Daemon /org/semibreve/Daemon \
 *          org.semibreve.Daemon Fidelity
 *   busctl --user set-property org.semibreve.Daemon /org/semibreve/Daemon \
 *          org.semibreve.Daemon Fidelity d 7.5
 *   busctl --user call org.semibreve.Daemon /org/semibreve/Daemon \
 *          org.semibreve.Daemon LoadPreset s Jazz
 */

#include "control.h"

#include <systemd/sd-bus.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_NAME  "org.semibreve.Daemon"
#define SB_PATH  "/org/semibreve/Daemon"
#define SB_IFACE "org.semibreve.Daemon"

struct SbBus {
	sd_bus      *bus;
	sd_bus_slot *slot;
	SbControl   *ctl;
};

/* Property names map to effect indices. One getter and one setter for all five
 * keeps the vtable honest: adding an effect is a line here and a line there,
 * with no third place to forget. */
static int effect_index(const char *property)
{
	if (strcmp(property, "Fidelity")     == 0) return SB_FIDELITY;
	if (strcmp(property, "Ambience")     == 0) return SB_AMBIENCE;
	if (strcmp(property, "Surround")     == 0) return SB_SURROUND;
	if (strcmp(property, "DynamicBoost") == 0) return SB_DYNAMIC_BOOST;
	if (strcmp(property, "Bass")         == 0) return SB_BASS;
	return -1;
}

static int prop_get_effect(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *reply,
                           void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	int e = effect_index(property);
	(void)bus; (void)path; (void)interface;
	if (e < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_UNKNOWN_PROPERTY, "%s", property);
	return sd_bus_message_append(reply, "d", sb_get_effect(c, e));
}

static int prop_set_effect(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *value,
                           void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	int e = effect_index(property);
	double v;
	int r;
	(void)bus; (void)path; (void)interface;

	if (e < 0)
		return sd_bus_error_setf(err, SD_BUS_ERROR_UNKNOWN_PROPERTY, "%s", property);
	r = sd_bus_message_read(value, "d", &v);
	if (r < 0)
		return r;
	if (!sb_set_effect(c, e, v))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
		                         "%s must be between 0 and 10, got %g", property, v);
	return 1;
}

static int prop_get_bypass(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *reply,
                           void *userdata, sd_bus_error *err)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	return sd_bus_message_append(reply, "b", sb_get_bypass(userdata));
}

static int prop_set_bypass(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *value,
                           void *userdata, sd_bus_error *err)
{
	int on, r;
	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	r = sd_bus_message_read(value, "b", &on);
	if (r < 0)
		return r;
	sb_set_bypass(userdata, on);
	return 1;
}

static int prop_get_preset_id(sd_bus *bus, const char *path, const char *interface,
                              const char *property, sd_bus_message *reply,
                              void *userdata, sd_bus_error *err)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	return sd_bus_message_append(reply, "s", sb_preset_id(userdata));
}

static int prop_get_preset_name(sd_bus *bus, const char *path, const char *interface,
                                const char *property, sd_bus_message *reply,
                                void *userdata, sd_bus_error *err)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	return sd_bus_message_append(reply, "s", sb_preset_name(userdata));
}

/* a(dd): centre frequency and gain in dB, one entry per band. */
static int prop_get_eq(sd_bus *bus, const char *path, const char *interface,
                       const char *property, sd_bus_message *reply,
                       void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	int n = sb_num_eq_bands(c), i, r;
	(void)bus; (void)path; (void)interface; (void)property; (void)err;

	r = sd_bus_message_open_container(reply, 'a', "(dd)");
	if (r < 0)
		return r;
	for (i = 0; i < n; i++) {
		r = sd_bus_message_append(reply, "(dd)",
		                          sb_eq_frequency(c, i), sb_eq_gain(c, i));
		if (r < 0)
			return r;
	}
	return sd_bus_message_close_container(reply);
}

static int prop_get_output(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *reply,
                           void *userdata, sd_bus_error *err)
{
	(void)bus; (void)path; (void)interface; (void)property; (void)err;
	return sd_bus_message_append(reply, "s", sb_output_device(userdata));
}

static int prop_set_output(sd_bus *bus, const char *path, const char *interface,
                           const char *property, sd_bus_message *value,
                           void *userdata, sd_bus_error *err)
{
	const char *name;
	int r;
	(void)bus; (void)path; (void)interface; (void)property;

	r = sd_bus_message_read(value, "s", &name);
	if (r < 0)
		return r;
	if (!sb_set_output_device(userdata, name))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
		                         "no output device named '%s'", name);
	return 1;
}

/* a(ss): node name and the description a person should see. */
static int prop_get_outputs(sd_bus *bus, const char *path, const char *interface,
                            const char *property, sd_bus_message *reply,
                            void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	int n = sb_output_count(c), i, r;
	(void)bus; (void)path; (void)interface; (void)property; (void)err;

	r = sd_bus_message_open_container(reply, 'a', "(ss)");
	if (r < 0)
		return r;
	for (i = 0; i < n; i++) {
		r = sd_bus_message_append(reply, "(ss)",
		                          sb_output_name(c, i), sb_output_description(c, i));
		if (r < 0)
			return r;
	}
	return sd_bus_message_close_container(reply);
}

static int method_load_preset(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	const char *name;
	int r = sd_bus_message_read(m, "s", &name);
	if (r < 0)
		return r;
	if (!sb_load_preset(c, name))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
		                         "no preset named '%s'", name);
	return sd_bus_reply_method_return(m, "");
}

static int method_list_presets(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	sd_bus_message *reply = NULL;
	int n = sb_preset_count(c), i, r;
	(void)err;

	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	r = sd_bus_message_open_container(reply, 'a', "(ss)");
	if (r < 0)
		goto out;
	for (i = 0; i < n; i++) {
		r = sd_bus_message_append(reply, "(ss)",
		                          sb_preset_list_id(c, i), sb_preset_list_name(c, i));
		if (r < 0)
			goto out;
	}
	r = sd_bus_message_close_container(reply);
	if (r < 0)
		goto out;
	r = sd_bus_send(NULL, reply, NULL);
out:
	sd_bus_message_unref(reply);
	return r;
}

static int method_quit(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	(void)err;
	/* Reply first: the caller should hear "yes" before we tear the bus down. */
	int r = sd_bus_reply_method_return(m, "");
	sb_quit(userdata);
	return r;
}

static int method_set_eq_band(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	SbControl *c = userdata;
	uint32_t band;
	double db;
	int r = sd_bus_message_read(m, "ud", &band, &db);
	if (r < 0)
		return r;
	if (!sb_set_eq_gain(c, (int)band, db))
		return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
		                         "band %u out of range (0..%d)",
		                         band, sb_num_eq_bands(c) - 1);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable sb_vtable[] = {
	SD_BUS_VTABLE_START(0),

	SD_BUS_WRITABLE_PROPERTY("Bypass", "b", prop_get_bypass, prop_set_bypass, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

	SD_BUS_WRITABLE_PROPERTY("Fidelity", "d", prop_get_effect, prop_set_effect, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("Ambience", "d", prop_get_effect, prop_set_effect, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("Surround", "d", prop_get_effect, prop_set_effect, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("DynamicBoost", "d", prop_get_effect, prop_set_effect, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_WRITABLE_PROPERTY("Bass", "d", prop_get_effect, prop_set_effect, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

	SD_BUS_PROPERTY("PresetId",   "s",     prop_get_preset_id,   0,
	                SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("PresetName", "s",     prop_get_preset_name, 0,
	                SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("EqBands",    "a(dd)", prop_get_eq,          0,
	                SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),

	SD_BUS_WRITABLE_PROPERTY("OutputDevice", "s", prop_get_output, prop_set_output, 0,
	                         SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("OutputDevices", "a(ss)", prop_get_outputs, 0,
	                SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),

	SD_BUS_METHOD("LoadPreset",  "s",  "",       method_load_preset,  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ListPresets", "",   "a(ss)",  method_list_presets, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetEqBand",   "ud", "",       method_set_eq_band,  SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Quit",        "",   "",       method_quit,         SD_BUS_VTABLE_UNPRIVILEGED),

	SD_BUS_VTABLE_END
};

SbBus *sb_bus_new(SbControl *c, int *name_taken)
{
	SbBus *b = calloc(1, sizeof(*b));
	int r;

	*name_taken = 0;
	if (b == NULL)
		return NULL;
	b->ctl = c;

	r = sd_bus_open_user(&b->bus);
	if (r < 0) {
		fprintf(stderr, "semibreved: no session bus (%s); "
		                "running without a control surface\n", strerror(-r));
		goto fail;
	}

	r = sd_bus_add_object_vtable(b->bus, &b->slot, SB_PATH, SB_IFACE, sb_vtable, c);
	if (r < 0) {
		fprintf(stderr, "semibreved: could not publish %s (%s)\n",
		        SB_IFACE, strerror(-r));
		goto fail;
	}

	/* No replace, no queue: a second daemon should fail loudly rather than
	 * quietly take the name from the one already carrying audio. */
	r = sd_bus_request_name(b->bus, SB_NAME, 0);
	if (r == -EEXIST) {
		fprintf(stderr, "semibreved: another semibreved already owns %s — "
		                "not starting a second Semibreve sink\n", SB_NAME);
		*name_taken = 1;
		goto fail;
	}
	if (r < 0) {
		fprintf(stderr, "semibreved: could not take the name %s (%s)\n",
		        SB_NAME, strerror(-r));
		goto fail;
	}

	return b;

fail:
	sb_bus_free(b);
	return NULL;
}

int sb_bus_fd(SbBus *b)
{
	return b ? sd_bus_get_fd(b->bus) : -1;
}

int sb_bus_process(SbBus *b)
{
	int r;
	if (b == NULL)
		return 0;
	/* Drain: one readable event can carry several messages, and sd_bus_process
	 * handles one per call. */
	do {
		r = sd_bus_process(b->bus, NULL);
	} while (r > 0);
	if (r < 0)
		return r;
	return sd_bus_flush(b->bus);
}

void sb_bus_emit_changed(SbBus *b, const char *property)
{
	if (b == NULL)
		return;
	sd_bus_emit_properties_changed(b->bus, SB_PATH, SB_IFACE, property, NULL);
}

void sb_bus_free(SbBus *b)
{
	if (b == NULL)
		return;
	if (b->slot)
		sd_bus_slot_unref(b->slot);
	if (b->bus)
		sd_bus_unref(b->bus);
	free(b);
}
