/*
 * Semibreve — layer 2. The PipeWire node.
 *
 * A virtual sink named Semibreve that applications can select. Audio routed
 * to it passes through FxSound's DSP and on to a real output device. Presets,
 * effects, the EQ and the output device are driven over D-Bus; see dbus.c.
 *
 * This file is the only place in the project that owns a real-time thread, and
 * the split exists to keep it that way. Everything inside on_capture_process()
 * obeys the layer 2 real-time rules: no allocation, no locks, no syscalls, no
 * logging.
 *
 *
 * Why two streams and not one filter
 *
 * The obvious shape — a single pw_filter with input and output DSP ports and
 * media.class = Audio/Sink — does not work, and fails in a way worth recording
 * because it looks like it is working. The node appears in the graph, `wpctl`
 * lists it as a sink, all four ports are correct and its state is `running`.
 * But `pactl` never lists it and WirePlumber will not route anything to it:
 * pw-play targeting it by node id links to the hardware sink instead. A raw
 * filter with DSP ports (`32 bit float mono audio`) has no negotiated audio
 * format for pipewire-pulse to describe and is not a valid routing target.
 *
 * Adding node.link-group makes it worse, not better — that marks a node as an
 * internal filter and WirePlumber then hides it from the device list entirely.
 *
 * What works is the shape pw-loopback uses: two adapter nodes sharing a link
 * group. A capture stream carrying media.class = Audio/Sink is the sink
 * applications select; a playback stream auto-connects onward to the real
 * output device. pw_stream gives us interleaved float, which is what DfxDsp
 * wants anyway, so the interleave/de-interleave a filter would have needed
 * disappears with it.
 *
 *
 * How the two halves stay in step
 *
 * Both streams are scheduled normally and a single-producer/single-consumer
 * ring carries audio from one callback to the other. The capture side runs the
 * DSP and writes; the playback side reads, and emits silence for any shortfall.
 *
 * PW_STREAM_FLAG_TRIGGER was tried first, on the theory that the capture
 * callback could dequeue and fill the playback buffer directly and drive it
 * with pw_stream_trigger_process() inside the same cycle — no ring, no added
 * latency. It does not work that way: the trigger raises a process event *on
 * the playback stream*, and that event is the only place its buffer may be
 * dequeued. Dequeuing from the capture callback instead got a buffer exactly
 * once and NULL forever after — 142 of 143 callbacks with no output buffer,
 * one buffer of audio through the whole node.
 *
 * The ring costs one quantum of latency. That is the right trade here: a
 * system-wide effects sink is not a monitoring path, and correctness under a
 * renegotiated quantum matters more than a couple of milliseconds.
 *
 *
 * Who may touch the DSP, and when
 *
 * Only the main loop thread mutates DfxDsp. The audio callback calls
 * processAudio() and nothing else. That leaves exactly two kinds of cross-
 * thread state, and neither needs a queue:
 *
 *   - Two atomics the audio thread reads: the target output gain and the
 *     bypass flag.
 *   - The DSP's own parameter slots, which the control thread writes while the
 *     audio thread reads them.
 *
 * The second sounds worse than it is. A single effect change bottoms out in
 * dfxp_CommunicateFidelity() and its siblings, each of which does a handful of
 * comRealWrite() calls — one aligned 32-bit float per channel handle, and for
 * stereo only the front handle is live. An aligned 4-byte store cannot tear on
 * x86-64, so the audio thread either sees the old value or the new one. This is
 * also what upstream does: FxSound's GUI mutates these from its own thread
 * while WASAPI processes.
 *
 * A preset load is the case that genuinely needs protecting. It rewrites some
 * forty parameters plus every EQ band and ends in dfxpCommunicateAll(), and a
 * buffer that caught half of that would be audible. So disruptive changes —
 * preset loads and bypass toggles — ramp the node's output gain to zero, apply
 * on a main-loop timer once the ramp has certainly landed, and ramp back up.
 * The ramp is ours, at the node's edge; it touches none of the DSP's
 * arithmetic.
 *
 * A lock-free SPSC queue was the alternative to atomics here. Atomics are what
 * this needs: the audio thread never applies a parameter, so there is nothing
 * for a queue to carry. The warning that a queue must carry values
 * explicitly was about not relying on a registry poll to propagate them, and
 * this design does not — the control thread calls the setters directly.
 *
 *
 * processTimer()
 *
 * Upstream calls it from inside processAudio(), where it can take the registry
 * mutex and walk a map. We removed that call. Anything that mutates DSP state
 * must run on the main loop thread and call dsp.processTimer() afterwards:
 * startup, every D-Bus setter, and on_apply_timer().
 */

#include "DfxDsp.h"
#include "control.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <xmmintrin.h>

#include <atomic>
#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/* Stereo only. FxSound is a stereo processor and the preset format carries no
 * channel map. */
constexpr uint32_t NCH = 2;

/* Largest run of frames handed to DfxDsp in one call. PipeWire renegotiates
 * the quantum at run time, so this is a chunking bound rather than a limit: a
 * larger buffer is processed in several passes. Comfortably under
 * DAW_MAX_BUFFER_SIZE (16384 sample sets), where the DSP chunks internally.
 *
 * Since the Play32 fix the DSP's output no longer depends on how the stream is
 * cut into buffers, and run_checks.sh guards that. Before that fix this
 * constant would have changed the voicing. */
constexpr uint32_t MAX_CHUNK = 8192;

struct Impl {
	pw_main_loop *loop = nullptr;

	pw_stream *capture  = nullptr;   /* media.class = Audio/Sink  */
	pw_stream *playback = nullptr;   /* Stream/Output/Audio       */
	spa_hook  capture_listener {};
	spa_hook  playback_listener {};

	DfxDsp *dsp = nullptr;

	/* SPSC ring between the two callbacks. Power-of-two frame count so the
	 * wrap is a mask. Sized for four maximum quanta, which is far more than
	 * the one quantum of slack the hand-off actually needs; it is 256 KB and
	 * allocated once, so there is no reason to cut it fine. */
	static constexpr uint32_t RING_FRAMES = 32768;      /* must be 2^n */
	static constexpr uint32_t RING_MASK   = RING_FRAMES - 1;
	float *ring = nullptr;                              /* RING_FRAMES * NCH */
	std::atomic<uint32_t> ring_w {0};                   /* capture writes  */
	std::atomic<uint32_t> ring_r {0};                   /* playback reads  */

	/* Contiguous staging for one chunk, so the DSP never sees the wrap. */
	float *scratch = nullptr;                           /* MAX_CHUNK * NCH */

	/* Last rate handed to setSignalFormat. */
	uint32_t rate = 0;

	/*
	 * Output gain, ramped in the audio thread. The control thread pulls
	 * target_gain to zero before a disruptive change and back to one after, so
	 * the change lands in silence instead of mid-signal. RAMP_MS is short
	 * enough to feel instant and long enough to be inaudible.
	 */
	static constexpr double RAMP_MS = 8.0;
	std::atomic<float> target_gain {1.0f};
	float gain = 1.0f;              /* audio thread only */
	float gain_step = 1.0f;         /* per-sample, set on format change */

	std::atomic<bool> bypass {false};

	/*
	 * Where the output may fade back in after a disruptive change: the ring
	 * position whose audio was processed after the change, plus the hold. The
	 * main loop sets it when it applies; the playback side stays silent until
	 * it has played past it. See on_apply_timer.
	 */
	std::atomic<uint32_t> release_at {0};
	std::atomic<bool>     release_pending {false};
	/* Frames in the capture side's last buffer: how much may be in flight
	 * across a change. Capture thread writes, main loop reads. */
	std::atomic<uint32_t> last_chunk {0};

	/* A disruptive change in flight. Main loop only.
	 *   1 = faded out, waiting to apply
	 *   2 = applied, holding silent while the transient clears
	 *   0 = idle */
	int         apply_stage = 0;
	spa_source *apply_timer = nullptr;
	bool        pending_preset = false;
	std::string pending_preset_path;
	bool        pending_bypass = false;
	bool        pending_bypass_value = false;

	/* The preset currently in force, for reporting over D-Bus. */
	std::string preset_id;
	std::string preset_name;

	/* The stock presets, enumerated once. D-Bus getters hand out these
	 * pointers, so the strings must outlive every reply — hence members
	 * rather than anything built per call. */
	std::vector<std::pair<std::string, std::string>> catalogue;

	SbBus *bus = nullptr;
	spa_source *bus_source = nullptr;

	/* Where our output goes, and the sinks the UI can pick from. Main-loop
	 * state, kept current by a registry listener. */
	std::string output_target;
	struct Sink { std::string name, description; int priority; };
	std::vector<Sink> sinks;
	std::map<uint32_t, std::string> id_to_name;
	pw_registry *registry = nullptr;
	spa_hook registry_listener {};

	/* A device change waiting for the fade, like a preset. */
	bool        pending_output = false;
	std::string pending_output_name;

	/* Every device asked for, on the command line or over D-Bus, most recent
	 * last. Losing the device in use falls back down this list, and the one at
	 * the end is returned to when it comes back. See on_playback_state. */
	std::vector<std::string> chosen_outputs;
	/* No device left to send to: the next sink to appear is used. */
	bool output_lost = false;
	/* Losses since the output last ran, so a device that fails as soon as we
	 * move to it cannot keep us bouncing between targets. */
	int output_losses = 0;

	/* Set when the capture side reports it has lost the server. See
	 * on_capture_state. */
	bool graph_lost = false;
	/* Set while we are deliberately taking the playback stream down to point
	 * it somewhere else, so its UNCONNECTED is not mistaken for a crash. */
	bool reconnecting = false;
	/* Set once the main loop has returned. Destroying the streams takes them
	 * through UNCONNECTED, and every clean stop used to log a lost graph. */
	bool stopping = false;

	/* Counters, reported once at shutdown. Relaxed atomics: the audio thread
	 * only ever increments them and nothing reads them until the loop has
	 * stopped, so there is no ordering to establish and no cost beyond the
	 * add itself. */
	std::atomic<uint64_t> cb_count   {0};
	std::atomic<uint64_t> frames_in  {0};
	std::atomic<uint64_t> no_out_buf {0};
	std::atomic<uint64_t> empty_cb   {0};
	std::atomic<uint64_t> frames_out {0};
	std::atomic<uint64_t> underruns  {0};
	std::atomic<uint64_t> overruns   {0};
};

Impl g_impl;

/*
 * Denormals.
 *
 * The DSP is scalar C with no SIMD anywhere, so nothing in it touches MXCSR and
 * the IIR filters can drift into denormal territory on a decaying tail, where
 * every operation costs a microcode assist and the audio thread starts missing
 * deadlines. Bit 15 is flush-to-zero, bit 6 is denormals-are-zero. Written
 * directly rather than through _MM_SET_FLUSH_ZERO_MODE and
 * _MM_SET_DENORMAL_ZERO_MODE: the latter lives in <pmmintrin.h> behind an
 * __SSE3__ guard, and the alternative is adding -msse3 to raise the baseline
 * ISA for two bits that have been in MXCSR since SSE2. It is an MXCSR write
 * with no syscall behind it, so doing it every callback is cheaper than
 * tracking whether we already did.
 */
constexpr unsigned MXCSR_FTZ = 1u << 15;
constexpr unsigned MXCSR_DAZ = 1u << 6;

inline void set_ftz_daz()
{
	_mm_setcsr(_mm_getcsr() | MXCSR_FTZ | MXCSR_DAZ);
}

void on_capture_process(void *userdata)
{
	Impl *d = static_cast<Impl *>(userdata);
	set_ftz_daz();

	d->cb_count.fetch_add(1, std::memory_order_relaxed);

	pw_buffer *in = pw_stream_dequeue_buffer(d->capture);
	if (in == nullptr) {
		d->empty_cb.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	spa_data *ind = &in->buffer->datas[0];
	const float *src = static_cast<const float *>(ind->data);

	uint32_t frames = 0;
	if (src != nullptr)
		frames = ind->chunk->size / (uint32_t)(sizeof(float) * NCH);

	if (frames > 0) {
		d->frames_in.fetch_add(frames, std::memory_order_relaxed);
		d->last_chunk.store(frames, std::memory_order_relaxed);

		/* Only this thread moves ring_w, so a relaxed load of our own index
		 * is fine; ring_r must be acquired to see the reader's progress. */
		const uint32_t w = d->ring_w.load(std::memory_order_relaxed);
		const uint32_t r = d->ring_r.load(std::memory_order_acquire);
		const uint32_t free_frames = Impl::RING_FRAMES - (w - r);

		if (frames > free_frames) {
			/* The reader has stalled. Dropping the newest audio keeps the
			 * ring coherent; the alternative is overwriting frames the
			 * playback side is mid-way through reading. */
			d->overruns.fetch_add(1, std::memory_order_relaxed);
			frames = free_frames;
		}

		for (uint32_t off = 0; off < frames; off += MAX_CHUNK) {
			const uint32_t m =
				(frames - off < MAX_CHUNK) ? (frames - off) : MAX_CHUNK;

			/* Stage contiguously: the DSP is stateful across the call and
			 * must never be handed a buffer that wraps mid-way. */
			memcpy(d->scratch, src + (size_t)off * NCH,
			       (size_t)m * NCH * sizeof(float));

			if (!d->bypass.load(std::memory_order_relaxed)) {
				/* setSignalFormat is not called here. It is the one DSP
				 * entry point not known to be allocation-free, and format
				 * negotiation already gives us a main-loop callback to do
				 * it from — see on_capture_param_changed.
				 *
				 * In-place is supported and is what upstream does. */
				d->dsp->processAudio(reinterpret_cast<short *>(d->scratch),
				                     reinterpret_cast<short *>(d->scratch),
				                     (int)m, 0);
			}

			const uint32_t pos   = (w + off) & Impl::RING_MASK;
			const uint32_t first = (m < Impl::RING_FRAMES - pos)
			                     ? m : Impl::RING_FRAMES - pos;
			memcpy(d->ring + (size_t)pos * NCH, d->scratch,
			       (size_t)first * NCH * sizeof(float));
			if (first < m)
				memcpy(d->ring, d->scratch + (size_t)first * NCH,
				       (size_t)(m - first) * NCH * sizeof(float));
		}

		/* Release: everything written above must be visible to the reader
		 * before it can see the advanced index. */
		d->ring_w.store(w + frames, std::memory_order_release);
	}

	pw_stream_queue_buffer(d->capture, in);
}

void on_playback_process(void *userdata)
{
	Impl *d = static_cast<Impl *>(userdata);
	set_ftz_daz();

	pw_buffer *out = pw_stream_dequeue_buffer(d->playback);
	if (out == nullptr) {
		d->no_out_buf.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	spa_data *outd = &out->buffer->datas[0];
	float *dst = static_cast<float *>(outd->data);

	uint32_t want = 0;
	if (dst != nullptr) {
		want = outd->maxsize / (uint32_t)(sizeof(float) * NCH);
		/* Honour the quantum when the graph tells us what it wants; maxsize
		 * is only the ceiling. */
		if (out->requested != 0 && out->requested < want)
			want = (uint32_t)out->requested;
	}

	uint32_t got = 0;
	if (want > 0) {
		const uint32_t r = d->ring_r.load(std::memory_order_relaxed);
		const uint32_t w = d->ring_w.load(std::memory_order_acquire);
		const uint32_t avail = w - r;

		got = (want < avail) ? want : avail;

		const uint32_t pos   = r & Impl::RING_MASK;
		const uint32_t first = (got < Impl::RING_FRAMES - pos)
		                     ? got : Impl::RING_FRAMES - pos;
		memcpy(dst, d->ring + (size_t)pos * NCH,
		       (size_t)first * NCH * sizeof(float));
		if (first < got)
			memcpy(dst + (size_t)first * NCH, d->ring,
			       (size_t)(got - first) * NCH * sizeof(float));

		/*
		 * Ramp toward the target. Done here rather than on the capture side so
		 * it covers the silence emitted during an underrun too — otherwise a
		 * fade could be interrupted by a gap and resume at the wrong level.
		 */
		{
			const float target = d->target_gain.load(std::memory_order_relaxed);
			/* Not before the audio processed after a change has reached us,
			 * to the sample: see on_apply_timer. */
			uint32_t held = 0;
			if (d->release_pending.load(std::memory_order_acquire)) {
				const int32_t left = (int32_t)(d->release_at.load(std::memory_order_relaxed) - r);
				if (left > 0)
					held = ((uint32_t)left < got) ? (uint32_t)left : got;
				else
					d->release_pending.store(false, std::memory_order_relaxed);
			}
			float g = d->gain;
			const float step = d->gain_step;
			for (uint32_t i = 0; i < got; i++) {
				const float t = (i < held) ? 0.0f : target;
				if (g < t) { g += step; if (g > t) g = t; }
				else if (g > t) { g -= step; if (g < t) g = t; }
				dst[(size_t)i * NCH]     *= g;
				dst[(size_t)i * NCH + 1] *= g;
			}
			d->gain = g;
		}

		if (got < want) {
			/* Nothing queued behind it — silence is the correct filler, and
			 * the usual reason is simply that no application is playing. */
			memset(dst + (size_t)got * NCH, 0,
			       (size_t)(want - got) * NCH * sizeof(float));
			if (got > 0)
				d->underruns.fetch_add(1, std::memory_order_relaxed);
		}

		d->ring_r.store(r + got, std::memory_order_release);
		d->frames_out.fetch_add(got, std::memory_order_relaxed);
	}

	outd->chunk->offset = 0;
	outd->chunk->stride = (int32_t)(sizeof(float) * NCH);
	outd->chunk->size   = want * (uint32_t)(sizeof(float) * NCH);

	pw_stream_queue_buffer(d->playback, out);
}

/*
 * A PipeWire restart takes our nodes with it and does not bring them back.
 *
 * The daemon survives as a process — it has a DfxDsp, a ring, a bus name — but
 * its streams are gone from the graph, so the sink simply vanishes from the
 * user's device list while `semibreved` still shows in `ps`. That is worse
 * than dying: nothing is obviously broken, so nothing gets restarted.
 *
 * Reconnecting in place would mean rebuilding both streams and renegotiating
 * the format from inside a state callback. Exiting is the honest move: the
 * window restarts a daemon it started, and the failure is in the log rather
 * than hidden behind a sink that has quietly gone.
 *
 * This is the capture side only. The capture stream is the sink itself, so its
 * loss is the graph's; the playback side is just our way out, below.
 */
void on_capture_state(void *userdata, enum pw_stream_state old,
                      enum pw_stream_state state, const char *error)
{
	Impl *d = static_cast<Impl *>(userdata);
	(void)old;

	if (d->stopping)
		return;

	if (state == PW_STREAM_STATE_ERROR ||
	    (state == PW_STREAM_STATE_UNCONNECTED && !d->graph_lost)) {
		d->graph_lost = true;
		fprintf(stderr, "semibreved: lost the PipeWire graph (%s) — exiting so "
		                "it can be restarted\n", error ? error : "disconnected");
		if (d->loop)
			pw_main_loop_quit(d->loop);
	}
}

/* Defined below with the other control-surface helpers. */
void control_set_output(Impl *d, const std::string &name);

/* Record a device someone asked for, most recent last, each name once. */
void remember_choice(Impl *d, const std::string &name)
{
	auto &chosen = d->chosen_outputs;
	chosen.erase(std::remove(chosen.begin(), chosen.end(), name), chosen.end());
	chosen.push_back(name);
	if (chosen.size() > 8)
		chosen.erase(chosen.begin());
}

bool have_sink(const Impl *d, const std::string &name)
{
	for (const auto &sink : d->sinks)
		if (sink.name == name)
			return true;
	return false;
}

/* Where to go when the device in use has gone: the most recent earlier choice
 * that still exists, or failing that the sink WirePlumber ranks highest. Never
 * the device that just failed. Empty when there is nowhere to go. */
std::string fallback_output(const Impl *d)
{
	for (auto it = d->chosen_outputs.rbegin(); it != d->chosen_outputs.rend(); ++it)
		if (*it != d->output_target && have_sink(d, *it))
			return *it;
	const Impl::Sink *best = nullptr;
	for (const auto &sink : d->sinks)
		if (sink.name != d->output_target && (best == nullptr || sink.priority > best->priority))
			best = &sink;
	return best ? best->name : std::string();
}

/*
 * The playback side is only our way out, and losing it is not losing the
 * graph. The usual cause is the device it was linked to going away: unplugged,
 * switched off, out of Bluetooth range. Told not to move the stream
 * (node.dont-reconnect, see main), WirePlumber errors it with "target not
 * found". That used to be taken for a lost server and the daemon exited,
 * taking the Semibreve sink with it: whatever was playing fell back to the
 * hardware unprocessed, and a daemon the window had not started never came
 * back.
 *
 * So move instead, under the same fade as any device change. With nowhere to
 * go, wait for the next sink to appear; and when the device the user chose
 * comes back, on_registry_global returns to it.
 */
constexpr int MAX_OUTPUT_LOSSES = 3;

void on_playback_state(void *userdata, enum pw_stream_state old,
                       enum pw_stream_state state, const char *error)
{
	Impl *d = static_cast<Impl *>(userdata);
	(void)old;

	/* Our own disconnect during an output change goes through UNCONNECTED on
	 * the way to the new device. Without this guard, switching device looked
	 * exactly like losing the output. */
	if (d->reconnecting || d->stopping || d->graph_lost)
		return;

	if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
		d->output_losses = 0;
		return;
	}
	if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_UNCONNECTED)
		return;
	if (d->pending_output || d->output_lost)
		return;     /* already on it */

	const std::string lost = d->output_target.empty() ? "(default)" : d->output_target;
	const std::string next = ++d->output_losses > MAX_OUTPUT_LOSSES
	                       ? std::string() : fallback_output(d);
	if (next.empty()) {
		fprintf(stderr, "semibreved: output %s went away (%s) and there is "
		                "nowhere else to send it; waiting for a device\n",
		        lost.c_str(), error ? error : "disconnected");
		d->output_lost = true;
		d->output_target.clear();
		sb_bus_emit_changed(d->bus, "OutputDevice");
		return;
	}
	fprintf(stderr, "semibreved: output %s went away (%s); moving to %s\n",
	        lost.c_str(), error ? error : "disconnected", next.c_str());
	control_set_output(d, next);
}

void on_capture_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
	Impl *d = static_cast<Impl *>(userdata);

	if (param == nullptr || id != SPA_PARAM_Format)
		return;

	struct spa_audio_info_raw info {};
	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;
	if (info.rate == 0)
		return;

	/* Format negotiation lands on the main loop, not the data thread, which
	 * is exactly where a setSignalFormat belongs: it is the one call on the
	 * DSP that is not known to be allocation-free. */
	if (info.rate != d->rate) {
		d->dsp->setSignalFormat(32, (int)NCH, (int)info.rate, 32);
		d->rate = info.rate;
		d->gain_step = (float)(1000.0 / (Impl::RAMP_MS * (double)info.rate));
		pw_log_info("semibreved: format now %u Hz, %u ch", info.rate, info.channels);
	}
}

const struct pw_stream_events capture_events = [] {
	struct pw_stream_events e {};
	e.version       = PW_VERSION_STREAM_EVENTS;
	e.process       = on_capture_process;
	e.param_changed = on_capture_param_changed;
	e.state_changed = on_capture_state;
	return e;
}();

const struct pw_stream_events playback_events = [] {
	struct pw_stream_events e {};
	e.version       = PW_VERSION_STREAM_EVENTS;
	e.process       = on_playback_process;
	e.state_changed = on_playback_state;
	return e;
}();

/* ---- presets ---------------------------------------------------------- */

/*
 * Preset handling is all control-thread work. Loading one at startup happens
 * before the streams connect and needs nothing more. Changing one while audio
 * runs means mutating DSP state the audio thread is reading, and the setters
 * are not atomic: that is what the fade in on_apply_timer() is for.
 */

std::wstring widen(const std::string &s)
{
	size_t n = mbstowcs(nullptr, s.c_str(), 0);
	if (n == (size_t)-1)
		return std::wstring();
	std::vector<wchar_t> w(n + 1);
	mbstowcs(w.data(), s.c_str(), n + 1);
	return std::wstring(w.data());
}

bool is_file(const std::string &p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* Where the stock presets live, found relative to the binary rather than the
 * working directory so the daemon behaves the same from a shell, a systemd
 * unit and a desktop launcher. */
std::string preset_root()
{
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0)
		return "presets";
	buf[n] = '\0';
	std::string exe(buf);
	size_t slash = exe.find_last_of('/');
	std::string dir = (slash == std::string::npos) ? "." : exe.substr(0, slash);
	return dir + "/../presets";
}

/* A bare name is matched against both stock directories, with and without the
 * extension, case-insensitively — "jazz", "Jazz" and "Jazz.fac" all work.
 * Anything containing a slash, or naming a real file, is taken as a path. */
std::string resolve_preset(const std::string &arg)
{
	if (is_file(arg))
		return arg;
	if (arg.find('/') != std::string::npos)
		return std::string();

	std::string want = arg;
	std::transform(want.begin(), want.end(), want.begin(), ::tolower);
	if (want.size() < 4 || want.compare(want.size() - 4, 4, ".fac") != 0)
		want += ".fac";

	const std::string root = preset_root();
	for (const char *sub : { "/factory", "/bonus" }) {
		const std::string dirpath = root + sub;
		DIR *d = opendir(dirpath.c_str());
		if (d == nullptr)
			continue;
		std::string hit;
		while (dirent *e = readdir(d)) {
			std::string name(e->d_name);
			std::string lower = name;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
			if (lower == want) { hit = dirpath + "/" + name; break; }
		}
		closedir(d);
		if (!hit.empty())
			return hit;
	}
	return std::string();
}

/* Defined below, next to the other preset helpers. */
std::string preset_display_name(DfxDsp &dsp, const std::string &path);

void list_presets()
{
	const std::string root = preset_root();
	DfxDsp probe;
	for (const char *sub : { "/factory", "/bonus" }) {
		const std::string dirpath = root + sub;
		DIR *d = opendir(dirpath.c_str());
		if (d == nullptr)
			continue;
		std::vector<std::string> files;
		while (dirent *e = readdir(d)) {
			std::string name(e->d_name);
			if (name.size() > 4 && name.compare(name.size() - 4, 4, ".fac") == 0)
				files.push_back(name);
		}
		closedir(d);
		std::sort(files.begin(), files.end());
		if (files.empty())
			continue;
		printf("%s:\n", sub + 1);
		for (const std::string &f : files) {
			/* The name inside the file is the one the UI shows, and it is not
			 * always the filename — the factory presets are numbered. Goes
			 * through the same helper as the D-Bus catalogue so both agree,
			 * CRLF trimming included. */
			printf("  %-16s %s\n", f.substr(0, f.size() - 4).c_str(),
			       preset_display_name(probe, dirpath + "/" + f).c_str());
		}
	}
}

std::string basename_noext(const std::string &path)
{
	size_t slash = path.find_last_of('/');
	std::string b = (slash == std::string::npos) ? path : path.substr(slash + 1);
	if (b.size() > 4 && b.compare(b.size() - 4, 4, ".fac") == 0)
		b.resize(b.size() - 4);
	return b;
}

std::string preset_display_name(DfxDsp &dsp, const std::string &path)
{
	DfxPreset info = dsp.getPresetInfo(widen(path));
	std::string out;
	for (wchar_t c : info.name) {
		if (c < 0x80) out += (char)c;
		else { /* keep it simple: D-Bus wants UTF-8 */
			char buf[8]; int n = wctomb(buf, c);
			if (n > 0) out.append(buf, n);
		}
	}
	/* The .fac files are CRLF — they came from Windows — and the name comes
	 * back with the carriage return still on it. Invisible in a terminal,
	 * visible in a Qt label as a box, so trim it here rather than in every
	 * client. Trailing spaces go too; a few stock presets have them. */
	while (!out.empty() && (out.back() == '\r' || out.back() == '\n' ||
	                        out.back() == ' '  || out.back() == '\t'))
		out.pop_back();
	return out;
}

/* Every stock preset, as (id, display name). Built once: the D-Bus getters
 * hand out pointers into it, so it has to outlive the replies. */
std::vector<std::pair<std::string, std::string>> preset_catalogue(DfxDsp &dsp)
{
	std::vector<std::pair<std::string, std::string>> out;
	const std::string root = preset_root();
	for (const char *sub : { "/factory", "/bonus" }) {
		const std::string dirpath = root + sub;
		DIR *dir = opendir(dirpath.c_str());
		if (dir == nullptr)
			continue;
		std::vector<std::string> files;
		while (dirent *e = readdir(dir)) {
			std::string name(e->d_name);
			if (name.size() > 4 && name.compare(name.size() - 4, 4, ".fac") == 0)
				files.push_back(name);
		}
		closedir(dir);
		std::sort(files.begin(), files.end());
		for (const std::string &f : files) {
			const std::string path = dirpath + "/" + f;
			out.emplace_back(f.substr(0, f.size() - 4), preset_display_name(dsp, path));
		}
	}
	return out;
}

/*
 * Apply a preset to the live chain.
 *
 * loadPreset() only populates DfxDsp's own state; it does not reach the
 * processing chain. Upstream's GUI follows every load with a get-then-set loop
 * over all effects and EQ bands, and that loop is what actually applies it. It
 * reads like a no-op and is not — see tools/dfxproc.cpp. The x10
 * undoes setEffectValue's internal divide by ten.
 */
bool apply_preset(DfxDsp &dsp, const std::string &path)
{
	const std::wstring wpath = widen(path);

	if (dsp.loadPreset(wpath) != 0) {
		fprintf(stderr, "semibreved: %s: loadPreset did not report success\n",
		        path.c_str());
		return false;
	}

	for (int e = 0; e < DfxDsp::NumEffects; e++)
		dsp.setEffectValue((DfxDsp::Effect)e,
		                   dsp.getEffectValue((DfxDsp::Effect)e) * 10.0f);
	for (int b = 0; b < dsp.getNumEqBands(); b++) {
		dsp.setEqBandFrequency(b, dsp.getEqBandFrequency(b));
		dsp.setEqBandBoostCut(b, dsp.getEqBandBoostCut(b));
	}
	dsp.eqOn(true);

	DfxPreset info = dsp.getPresetInfo(wpath);
	fprintf(stderr, "semibreved: preset \"%ls\"\n", info.name.c_str());
	return true;
}

/* ---- the sinks we can send to ------------------------------------------ */

/*
 * A registry listener, so the device list is the graph's own and stays current
 * as things are plugged in. Asking PulseAudio would have meant shelling out;
 * hard-coding would have meant lying the first time a DAC appeared.
 *
 * Our own nodes are skipped by name. Sending our output to our own input is a
 * feedback loop, and it is the one mistake this list must not offer.
 */
void on_registry_global(void *userdata, uint32_t id, uint32_t /*permissions*/,
                        const char *type, uint32_t /*version*/,
                        const struct spa_dict *props)
{
	Impl *d = static_cast<Impl *>(userdata);
	if (props == nullptr || type == nullptr || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (media_class == nullptr || strcmp(media_class, "Audio/Sink") != 0)
		return;

	const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (name == nullptr || strcmp(name, "semibreve") == 0)
		return;

	const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	if (desc == nullptr)
		desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
	if (desc == nullptr)
		desc = name;

	if (have_sink(d, name))
		return;

	/* WirePlumber's own ranking, for when a lost device leaves no earlier
	 * choice to fall back to. */
	const char *prio = spa_dict_lookup(props, PW_KEY_PRIORITY_SESSION);

	d->sinks.push_back({ name, desc, prio ? atoi(prio) : 0 });
	d->id_to_name[id] = name;
	sb_bus_emit_changed(d->bus, "OutputDevices");

	/* Somewhere to go at last, or the device the user chose coming back. With
	 * no output, every arrival re-picks the best: the registry announces the
	 * existing sinks one at a time, and the move waits out the fade anyway. */
	if (d->output_lost) {
		control_set_output(d, fallback_output(d));
	} else if (!d->chosen_outputs.empty() && d->chosen_outputs.back() == name &&
	           d->output_target != name) {
		fprintf(stderr, "semibreved: %s is back; returning to it\n", name);
		control_set_output(d, name);
	}
}

void on_registry_global_remove(void *userdata, uint32_t id)
{
	Impl *d = static_cast<Impl *>(userdata);
	auto it = d->id_to_name.find(id);
	if (it == d->id_to_name.end())
		return;
	const std::string gone = it->second;
	d->id_to_name.erase(it);
	for (auto i = d->sinks.begin(); i != d->sinks.end(); ++i) {
		if (i->name == gone) { d->sinks.erase(i); break; }
	}
	sb_bus_emit_changed(d->bus, "OutputDevices");
}

const struct pw_registry_events registry_events = [] {
	struct pw_registry_events e {};
	e.version       = PW_VERSION_REGISTRY_EVENTS;
	e.global        = on_registry_global;
	e.global_remove = on_registry_global_remove;
	return e;
}();

/* ---- the control surface ---------------------------------------------- */

/*
 * Everything below runs on the main loop thread. Nothing here is called from
 * the audio callback, and nothing here may be — the setters reach the registry
 * mutex and, for a preset, dfxpCommunicateAll().
 */

/*
 * A preset change is silent for two reasons, and the second is the one that is
 * easy to miss.
 *
 * APPLY_DELAY_MS is the wait before applying: comfortably past RAMP_MS so the
 * ramp has certainly reached zero.
 *
 * HOLD_MS is the wait *after* applying, before fading back up, and it exists
 * because fading out beforehand does nothing about the transient the DSP emits
 * once its coefficients change underneath its filter state. The ring also still
 * holds audio voiced by the old preset. Ramping up immediately plays both.
 * Measured: with no hold, a preset swap under a steady tone produced a 0.13
 * sample step where the signal's own maximum is 0.047; with the hold it stays
 * inside the signal's own range.
 *
 * The hold is counted in the ring, not on the clock. As a timer it assumed
 * the ring held about 20 ms, but the ring holds one quantum, and the quantum
 * is whatever the graph runs at: 2048 frames, 43 ms, as soon as a client asks
 * for a large buffer, as paplay does. The fade then came back up over audio
 * the old preset had voiced, and the new preset's transient arrived after it
 * at full level, a 0.07 to 0.13 step at the swap. So the apply also marks the
 * ring position the transient can start at, and the playback side stays
 * silent until it has played HOLD_MS past it, whatever the quantum.
 *
 * Total mute is therefore about 70 ms plus whatever the ring holds. That is
 * audible as a gap and inaudible as a click, which is the right way round for
 * a change of voicing.
 */
constexpr int APPLY_DELAY_MS = 20;
constexpr int HOLD_MS        = 40;

void arm_timer(Impl *d, int ms)
{
	/* Never zero. An all-zero timespec *disarms* a timerfd rather than firing
	 * it immediately, so a zero delay here would mean the callback never runs
	 * — and since the stages leave the gain at zero until the last one, the
	 * node would mute permanently on the first preset change. Found by setting
	 * HOLD_MS to 0 for a negative test and watching audio stop dead. */
	if (ms < 1)
		ms = 1;

	struct timespec ts {};
	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	pw_loop_update_timer(pw_main_loop_get_loop(d->loop), d->apply_timer,
	                     &ts, nullptr, false);
}

void arm_apply_timer(Impl *d)
{
	/* Fade first; the timer callback does the work. Re-arming an already
	 * pending change simply restarts the clock, which is what a user clicking
	 * between presets should get. */
	d->target_gain.store(0.0f, std::memory_order_relaxed);
	d->apply_stage = 1;
	arm_timer(d, APPLY_DELAY_MS);
}

/* Defined below with the other control-surface helpers. */
bool reconnect_output(Impl *d, const std::string &name);

void on_apply_timer(void *userdata, uint64_t /*expirations*/)
{
	Impl *d = static_cast<Impl *>(userdata);

	/* An output loss can schedule a move just before the capture side reports
	 * the whole graph gone; there is nothing to move to then. */
	if (d->graph_lost || d->stopping)
		return;

	if (d->apply_stage == 2) {
		/* The transient has had its hold. Safe to come back. */
		d->apply_stage = 0;
		d->target_gain.store(1.0f, std::memory_order_relaxed);
		return;
	}

	const bool announce_bypass = d->pending_bypass;
	const bool announce_preset = d->pending_preset;
	const bool announce_output = d->pending_output;

	if (d->pending_bypass) {
		d->bypass.store(d->pending_bypass_value, std::memory_order_relaxed);
		d->pending_bypass = false;
	}

	if (d->pending_output) {
		reconnect_output(d, d->pending_output_name);
		d->pending_output = false;
	}

	if (d->pending_preset) {
		if (apply_preset(*d->dsp, d->pending_preset_path)) {
			d->preset_id = basename_noext(d->pending_preset_path);
			d->preset_name = preset_display_name(*d->dsp, d->pending_preset_path);
		}
		d->pending_preset = false;
	}

	/* Every mutation above is followed by exactly one processTimer, on this
	 * thread; processAudio() no longer calls it. */
	d->dsp->processTimer();

	/* The audio voiced by what was just applied starts at most one capture
	 * buffer past the current write position. Hold HOLD_MS beyond that. */
	{
		const uint32_t hold = d->rate ? (uint32_t)((uint64_t)d->rate * HOLD_MS / 1000) : 0;
		d->release_at.store(d->ring_w.load(std::memory_order_acquire) +
		                    d->last_chunk.load(std::memory_order_relaxed) + hold,
		                    std::memory_order_relaxed);
		d->release_pending.store(true, std::memory_order_release);
	}

	/* Stay silent a little longer: see HOLD_MS. */
	d->apply_stage = 2;
	arm_timer(d, HOLD_MS);

	/* The change was deferred, so whoever asked for it has already had their
	 * reply. Tell every subscriber what actually landed — a preset moves all
	 * five effects and the whole EQ, and a UI should not have to poll. */
	if (announce_preset) {
		sb_bus_emit_changed(d->bus, "PresetId");
		sb_bus_emit_changed(d->bus, "PresetName");
		sb_bus_emit_changed(d->bus, "Fidelity");
		sb_bus_emit_changed(d->bus, "Ambience");
		sb_bus_emit_changed(d->bus, "Surround");
		sb_bus_emit_changed(d->bus, "DynamicBoost");
		sb_bus_emit_changed(d->bus, "Bass");
		sb_bus_emit_changed(d->bus, "EqBands");
	}
	if (announce_bypass)
		sb_bus_emit_changed(d->bus, "Bypass");
	if (announce_output)
		sb_bus_emit_changed(d->bus, "OutputDevice");
}

/*
 * Effect values are applied straight away: one aligned float store reaches the
 * audio thread, which cannot tear, so there is nothing to fade around. See the
 * note at the top of this file.
 *
 * The scale is the trap here, and it is asymmetric. setEffectValue() takes
 * 0..10 and divides by ten before handing the knob a 0..1, which is what
 * DFX_UI_MAX_VALUE allows; getEffectValue() returns that already-divided
 * value, so it reads back 0..1. Upstream's own load path compensates with the
 * get-then-set-times-ten loop in apply_preset().
 *
 * This was got wrong first time: multiplying by ten on the way in handed the
 * knob a 7.25 for a UI value of 7.25, dfxpSetKnobValue() rejected it as out of
 * range, and the value sat in DfxDsp's own struct field where getEffectValue()
 * read it straight back. A perfect round trip through state the DSP never saw
 * — and no readback test can tell the difference, which is why the check for
 * this compares audio.
 */
bool control_set_effect(Impl *d, DfxDsp::Effect e, double value_0_10)
{
	if (!(value_0_10 >= 0.0 && value_0_10 <= 10.0))
		return false;
	d->dsp->setEffectValue(e, (float)value_0_10);
	d->dsp->processTimer();
	return true;
}

double control_get_effect(Impl *d, DfxDsp::Effect e)
{
	return (double)d->dsp->getEffectValue(e) * 10.0;
}

/*
 * Move our output to a different device.
 *
 * pw_stream has no "change target": the stream is disconnected and reconnected
 * with a new PW_KEY_TARGET_OBJECT. That is a graph reconfiguration, so it runs
 * on the main loop and under the same fade as a preset change — otherwise the
 * audio would cut mid-sample on the way from one device to the other.
 */
bool reconnect_output(Impl *d, const std::string &name)
{
	d->reconnecting = true;
	pw_stream_disconnect(d->playback);

	/* pw_stream_get_properties() hands back a const dict, so the update goes
	 * in as its own two-item dict rather than by mutating the stream's. */
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, name.c_str()),
		SPA_DICT_ITEM_INIT(PW_KEY_NODE_DONT_RECONNECT, "true"),
	};
	struct spa_dict update = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	pw_stream_update_properties(d->playback, &update);

	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_audio_info_raw raw {};
	raw.format      = SPA_AUDIO_FORMAT_F32;
	raw.channels    = NCH;
	raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

	int rc = pw_stream_connect(d->playback, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		(enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
		                       PW_STREAM_FLAG_MAP_BUFFERS |
		                       PW_STREAM_FLAG_RT_PROCESS),
		params, 1);
	d->reconnecting = false;
	if (rc < 0) {
		pw_log_warn("semibreved: could not move output to %s: %s",
		            name.c_str(), spa_strerror(rc));
		return false;
	}
	d->output_target = name;
	d->output_lost = false;
	return true;
}

void control_set_output(Impl *d, const std::string &name)
{
	d->pending_output = true;
	d->pending_output_name = name;
	arm_apply_timer(d);
}

void control_set_bypass(Impl *d, bool on)
{
	if (d->bypass.load(std::memory_order_relaxed) == on && !d->pending_bypass)
		return;
	d->pending_bypass = true;
	d->pending_bypass_value = on;
	arm_apply_timer(d);
}

bool control_load_preset(Impl *d, const std::string &name, std::string &err)
{
	const std::string path = resolve_preset(name);
	if (path.empty()) {
		err = "no preset named '" + name + "'";
		return false;
	}
	d->pending_preset = true;
	d->pending_preset_path = path;
	arm_apply_timer(d);
	return true;
}

/* ---- control.h, implemented -------------------------------------------- */

/*
 * The C boundary the D-Bus glue calls through. Main loop thread only, which is
 * the same thread sd-bus is pumped on, so there is no synchronisation here and
 * none is needed.
 */


/*
 * sd-bus exposes its fd, so the control surface lives on the PipeWire main
 * loop rather than a thread of its own. That is worth more than it looks: it
 * means every D-Bus handler runs on the same thread as the preset timer and
 * the format callback, so the "main loop thread only" rule at the top of this
 * file is enforced by construction rather than by discipline.
 *
 * Only POLLIN is watched. This daemon answers calls and emits signals; it
 * never makes a call that would need a reply timeout, so sd-bus's timeout
 * handling is not wired up. If that changes, sd_bus_get_timeout() has to be
 * honoured with a loop timer.
 */
void on_bus_io(void *userdata, int /*fd*/, uint32_t /*mask*/)
{
	Impl *d = static_cast<Impl *>(userdata);
	int r = sb_bus_process(d->bus);
	if (r < 0)
		pw_log_warn("semibreved: D-Bus processing failed: %s", spa_strerror(r));
}

void on_signal(void *userdata, int /*signal_number*/)
{
	pw_main_loop_quit(static_cast<Impl *>(userdata)->loop);
}

} /* namespace */

namespace {

DfxDsp::Effect to_effect(int e)
{
	switch (e) {
	case SB_FIDELITY:      return DfxDsp::Fidelity;
	case SB_AMBIENCE:      return DfxDsp::Ambience;
	case SB_SURROUND:      return DfxDsp::Surround;
	case SB_DYNAMIC_BOOST: return DfxDsp::DynamicBoost;
	default:               return DfxDsp::Bass;
	}
}

} /* namespace */

extern "C" {

int sb_set_effect(SbControl *c, int effect, double value)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (effect < 0 || effect >= SB_NUM_EFFECTS)
		return 0;
	return control_set_effect(d, to_effect(effect), value) ? 1 : 0;
}

double sb_get_effect(SbControl *c, int effect)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (effect < 0 || effect >= SB_NUM_EFFECTS)
		return 0.0;
	return control_get_effect(d, to_effect(effect));
}

int  sb_get_bypass(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->bypass.load(std::memory_order_relaxed) ? 1 : 0;
}

void sb_set_bypass(SbControl *c, int on)
{
	control_set_bypass(reinterpret_cast<Impl *>(c), on != 0);
}

int sb_load_preset(SbControl *c, const char *name)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	std::string err;
	return control_load_preset(d, name ? name : "", err) ? 1 : 0;
}

const char *sb_preset_id(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->preset_id.c_str();
}

const char *sb_preset_name(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->preset_name.c_str();
}

int sb_preset_count(SbControl *c)
{
	return (int)reinterpret_cast<Impl *>(c)->catalogue.size();
}

const char *sb_preset_list_id(SbControl *c, int i)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (i < 0 || i >= (int)d->catalogue.size())
		return "";
	return d->catalogue[(size_t)i].first.c_str();
}

const char *sb_preset_list_name(SbControl *c, int i)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (i < 0 || i >= (int)d->catalogue.size())
		return "";
	return d->catalogue[(size_t)i].second.c_str();
}

const char *sb_output_device(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->output_target.c_str();
}

int sb_set_output_device(SbControl *c, const char *name)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (name == nullptr || *name == '\0')
		return 0;
	for (const auto &sink : d->sinks) {
		if (sink.name == name) {
			remember_choice(d, sink.name);
			control_set_output(d, sink.name);
			return 1;
		}
	}
	return 0;
}

int sb_output_count(SbControl *c)
{
	return (int)reinterpret_cast<Impl *>(c)->sinks.size();
}

const char *sb_output_name(SbControl *c, int i)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (i < 0 || i >= (int)d->sinks.size()) return "";
	return d->sinks[(size_t)i].name.c_str();
}

const char *sb_output_description(SbControl *c, int i)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (i < 0 || i >= (int)d->sinks.size()) return "";
	return d->sinks[(size_t)i].description.c_str();
}

void sb_quit(SbControl *c)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	fprintf(stderr, "semibreved: asked to quit\n");
	if (d->loop)
		pw_main_loop_quit(d->loop);
}

unsigned long long sb_underruns(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->underruns.load(std::memory_order_relaxed);
}

int sb_num_eq_bands(SbControl *c)
{
	return reinterpret_cast<Impl *>(c)->dsp->getNumEqBands();
}

double sb_eq_frequency(SbControl *c, int band)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (band < 0 || band >= d->dsp->getNumEqBands())
		return 0.0;
	return (double)d->dsp->getEqBandFrequency(band);
}

double sb_eq_gain(SbControl *c, int band)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (band < 0 || band >= d->dsp->getNumEqBands())
		return 0.0;
	return (double)d->dsp->getEqBandBoostCut(band);
}

int sb_set_eq_gain(SbControl *c, int band, double db)
{
	Impl *d = reinterpret_cast<Impl *>(c);
	if (band < 0 || band >= d->dsp->getNumEqBands())
		return 0;
	/* One band is one filter's gain — the same size of change as an effect
	 * knob, so it goes straight in rather than through the fade. */
	d->dsp->setEqBandBoostCut(band, (float)db);
	d->dsp->processTimer();
	return 1;
}

} /* extern "C" */

int main(int argc, char **argv)
{
	/* glibc decodes preset bytes through LC_CTYPE; see dsp/linux/pt_stream.h.
	 * The DfxDsp constructor walks the preset directory, so the locale has to
	 * be right before it runs, not just before a preset is loaded. */
	setlocale(LC_ALL, "");

	std::string preset_arg;
	std::string output_arg;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bypass") == 0) {
			g_impl.bypass.store(true);
		} else if (strcmp(argv[i], "--preset") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "semibreved: --preset needs a name or path\n");
				return 2;
			}
			preset_arg = argv[++i];
		} else if (strncmp(argv[i], "--preset=", 9) == 0) {
			preset_arg = argv[i] + 9;
		} else if (strcmp(argv[i], "--output") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "semibreved: --output needs a sink name\n");
				return 2;
			}
			output_arg = argv[++i];
		} else if (strcmp(argv[i], "--list-presets") == 0) {
			setlocale(LC_ALL, "");
			list_presets();
			return 0;
		} else if (strcmp(argv[i], "--help") == 0) {
			printf("usage: semibreved [--preset NAME|PATH] [--output SINK] [--bypass]\n"
			       "       semibreved --list-presets\n\n"
			       "  Registers a PipeWire sink named \"Semibreve\" and runs\n"
			       "  FxSound's DSP over everything routed to it.\n\n"
			       "  --preset NAME    load a stock preset by name (\"Jazz\", \"6\")\n"
			       "  --preset PATH    load a .fac file from anywhere\n"
			       "  --list-presets   show the stock presets and their names\n"
			       "  --output SINK    send our output to this sink by name\n"
			       "  --bypass         pass audio through untouched, for A/B\n\n"
			       "  Without --preset the built-in defaults are used. Everything\n"
			       "  can be changed while it runs, over D-Bus: see\n"
			       "  busctl --user introspect org.semibreve.Daemon /org/semibreve/Daemon\n\n"
			       "  If the output device goes away, output moves to another one,\n"
			       "  and back again when it returns.\n");
			return 0;
		} else {
			fprintf(stderr, "semibreved: unknown argument '%s'\n", argv[i]);
			return 2;
		}
	}

	pw_init(&argc, &argv);

	DfxDsp dsp;
	g_impl.dsp = &dsp;

	g_impl.catalogue = preset_catalogue(dsp);

	if (!preset_arg.empty()) {
		const std::string path = resolve_preset(preset_arg);
		if (path.empty()) {
			fprintf(stderr, "semibreved: no preset named '%s' "
			                "(try --list-presets)\n", preset_arg.c_str());
			return 1;
		}
		if (!apply_preset(dsp, path))
			return 1;
		g_impl.preset_id   = basename_noext(path);
		g_impl.preset_name = preset_display_name(dsp, path);
	} else {
		/*
		 * Defaults when no preset is named. These are the 0..10 values the UI
		 * shows; setEffectValue takes that scale directly and divides by ten
		 * itself to reach the knob. Deliberately modest: the point is to hear
		 * that the chain is live, not to voice it.
		 */
		dsp.setEffectValue(DfxDsp::Fidelity,     5.0f);
		dsp.setEffectValue(DfxDsp::Ambience,     3.0f);
		dsp.setEffectValue(DfxDsp::Surround,     3.0f);
		dsp.setEffectValue(DfxDsp::DynamicBoost, 4.0f);
		dsp.setEffectValue(DfxDsp::Bass,         4.0f);
		dsp.eqOn(true);
		g_impl.preset_id   = "";
		g_impl.preset_name = "Custom";
	}
	dsp.powerOn(true);

	/* Control thread drives processTimer, always after mutation and always
	 * before the audio thread can run. */
	dsp.processTimer();

	/* Preallocate before either stream connects: once a process callback can
	 * run, allocating would be an allocation on the audio thread. */
	g_impl.ring    = new float[(size_t)Impl::RING_FRAMES * NCH]();
	g_impl.scratch = new float[(size_t)MAX_CHUNK * NCH]();

	g_impl.loop = pw_main_loop_new(nullptr);
	if (g_impl.loop == nullptr) {
		fprintf(stderr, "semibreved: could not create main loop\n");
		return 1;
	}
	g_impl.apply_timer = pw_loop_add_timer(pw_main_loop_get_loop(g_impl.loop),
	                                       on_apply_timer, &g_impl);
	if (g_impl.apply_timer == nullptr) {
		fprintf(stderr, "semibreved: could not create apply timer\n");
		return 1;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(g_impl.loop), SIGINT,  on_signal, &g_impl);
	pw_loop_add_signal(pw_main_loop_get_loop(g_impl.loop), SIGTERM, on_signal, &g_impl);

	/*
	 * The control surface comes up before the streams, and its failure is not
	 * fatal. A daemon that passes audio without D-Bus is still doing its job;
	 * one that refuses to start because no session bus was found is not.
	 *
	 * The one failure that is fatal is the name being taken, which means
	 * another semibreved is running. Carrying on used to register a second
	 * Semibreve sink that nothing could control; finding out before the
	 * streams exist means there never is one. No handler runs until the main
	 * loop does, by which time the streams are connected.
	 */
	int name_taken = 0;
	g_impl.bus = sb_bus_new(reinterpret_cast<SbControl *>(&g_impl), &name_taken);
	if (name_taken)
		return 1;
	if (g_impl.bus != nullptr) {
		const int fd = sb_bus_fd(g_impl.bus);
		g_impl.bus_source = pw_loop_add_io(pw_main_loop_get_loop(g_impl.loop),
		                                   fd, SPA_IO_IN, false, on_bus_io, &g_impl);
		if (g_impl.bus_source == nullptr) {
			fprintf(stderr, "semibreved: could not watch the D-Bus fd\n");
			sb_bus_free(g_impl.bus);
			g_impl.bus = nullptr;
		}
	}

	/*
	 * node.group puts the two nodes on the same driver, so the ring between
	 * them is filled and drained in step, and node.virtual marks the pair as
	 * not backed by hardware. These come from the property set pw-loopback
	 * uses; a sink that got them wrong was invisible or unroutable, both of
	 * which were observed getting here. pw-loopback's node.link-group is left
	 * out on purpose: see the note on the playback side below.
	 */
	pw_properties *cap_props = pw_properties_new(
		PW_KEY_MEDIA_TYPE,       "Audio",
		PW_KEY_MEDIA_CATEGORY,   "Duplex",
		PW_KEY_MEDIA_CLASS,      "Audio/Sink",
		PW_KEY_MEDIA_NAME,       "Semibreve",
		PW_KEY_NODE_NAME,        "semibreve",
		PW_KEY_NODE_DESCRIPTION, "Semibreve",
		PW_KEY_NODE_GROUP,       "semibreve-group",
		PW_KEY_NODE_VIRTUAL,     "true",
		"audio.position",        "FL,FR",
		nullptr);

	/*
	 * Where our output goes.
	 *
	 * Left to autoconnect, the playback side follows the default sink — and
	 * the moment Semibreve *is* the default, that is itself. The graph really
	 * does make the link: semibreve-output -> semibreve, a feedback loop.
	 *
	 * node.link-group used to prevent that, and cost too much for it:
	 * WirePlumber files a link-grouped pair under Filters, and Plasma's audio
	 * applet does not show filters. A sink the user cannot select in the
	 * obvious place is not a sink.
	 *
	 * So the target is named explicitly instead. The launcher passes the
	 * hardware sink that was default before Semibreve took over; without
	 * --output we fall back to autoconnect, which is correct as long as we are
	 * not the default ourselves.
	 */
	pw_properties *play_props = pw_properties_new(
		PW_KEY_MEDIA_TYPE,       "Audio",
		PW_KEY_MEDIA_CATEGORY,   "Playback",
		PW_KEY_MEDIA_CLASS,      "Stream/Output/Audio",
		PW_KEY_MEDIA_NAME,       "Semibreve output",
		PW_KEY_NODE_NAME,        "semibreve-output",
		PW_KEY_NODE_DESCRIPTION, "Semibreve",
		PW_KEY_NODE_GROUP,       "semibreve-group",
		PW_KEY_NODE_VIRTUAL,     "true",
		"audio.position",        "FL,FR",
		nullptr);

	g_impl.output_target = output_arg;
	if (!output_arg.empty()) {
		remember_choice(&g_impl, output_arg);
		pw_properties_set(play_props, PW_KEY_TARGET_OBJECT, output_arg.c_str());
		/* A target on its own is only a preference: WirePlumber still moves the
		 * stream when the default sink changes, which is exactly the moment we
		 * become the default and it lands back on us. dont-reconnect pins it. */
		pw_properties_set(play_props, PW_KEY_NODE_DONT_RECONNECT, "true");
	}

	g_impl.capture = pw_stream_new_simple(
		pw_main_loop_get_loop(g_impl.loop), "Semibreve",
		cap_props, &capture_events, &g_impl);
	g_impl.playback = pw_stream_new_simple(
		pw_main_loop_get_loop(g_impl.loop), "Semibreve output",
		play_props, &playback_events, &g_impl);

	if (g_impl.capture == nullptr || g_impl.playback == nullptr) {
		fprintf(stderr, "semibreved: could not create streams\n");
		return 1;
	}

	/* Interleaved F32, which is what DfxDsp takes. Rate is left unspecified so
	 * we follow whatever the graph is already running at rather than forcing a
	 * change on every other client. */
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_audio_info_raw raw {};
	raw.format   = SPA_AUDIO_FORMAT_F32;
	raw.channels = NCH;
	raw.position[0] = SPA_AUDIO_CHANNEL_FL;
	raw.position[1] = SPA_AUDIO_CHANNEL_FR;
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

	/* Scheduled normally, not PW_STREAM_FLAG_TRIGGER — see the note at the top
	 * of this file for why that shape did not work. */
	int rc = pw_stream_connect(g_impl.playback, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		(enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
		                       PW_STREAM_FLAG_MAP_BUFFERS |
		                       PW_STREAM_FLAG_RT_PROCESS),
		params, 1);
	if (rc < 0) {
		fprintf(stderr, "semibreved: playback connect failed: %s\n", spa_strerror(rc));
		return 1;
	}

	/* Rebuild the pod: the builder was consumed by the call above. */
	b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);

	rc = pw_stream_connect(g_impl.capture, PW_DIRECTION_INPUT, PW_ID_ANY,
		(enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS |
		                       PW_STREAM_FLAG_RT_PROCESS),
		params, 1);
	if (rc < 0) {
		fprintf(stderr, "semibreved: capture connect failed: %s\n", spa_strerror(rc));
		return 1;
	}

	/* The registry gives us the device list. The core only exists once a
	 * stream has connected, which is why this is here and not earlier. */
	{
		pw_core *core = pw_stream_get_core(g_impl.capture);
		if (core != nullptr) {
			g_impl.registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
			if (g_impl.registry != nullptr)
				pw_registry_add_listener(g_impl.registry, &g_impl.registry_listener,
				                         &registry_events, &g_impl);
		}
		if (g_impl.registry == nullptr)
			fprintf(stderr, "semibreved: no registry — the output device list "
			                "will be empty\n");
	}

	fprintf(stderr, "semibreved: sink \"Semibreve\" registered, %s, out -> %s%s\n",
	        g_impl.bypass.load() ? "bypassed" : "processing",
	        output_arg.empty() ? "default sink" : output_arg.c_str(),
	        g_impl.bus ? ", org.semibreve.Daemon on the session bus" : ", no D-Bus");

	pw_main_loop_run(g_impl.loop);

	g_impl.stopping = true;
	const int exit_code = g_impl.graph_lost ? 1 : 0;

	fprintf(stderr,
	        "semibreved: %llu callbacks, %llu frames in, %llu frames out, "
	        "%llu underruns, %llu overruns, %llu no-output-buffer\n",
	        (unsigned long long)g_impl.cb_count.load(),
	        (unsigned long long)g_impl.frames_in.load(),
	        (unsigned long long)g_impl.frames_out.load(),
	        (unsigned long long)g_impl.underruns.load(),
	        (unsigned long long)g_impl.overruns.load(),
	        (unsigned long long)g_impl.no_out_buf.load());

	if (g_impl.bus_source)
		pw_loop_destroy_source(pw_main_loop_get_loop(g_impl.loop), g_impl.bus_source);
	sb_bus_free(g_impl.bus);

	pw_stream_destroy(g_impl.capture);
	pw_stream_destroy(g_impl.playback);
	pw_main_loop_destroy(g_impl.loop);
	pw_deinit();
	delete[] g_impl.ring;
	delete[] g_impl.scratch;
	return exit_code;
}
