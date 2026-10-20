/*
 * Semibreve — Milestone 0 smoke test.
 *
 * Answers the only question that matters right now: does the ported DSP
 * construct, accept a format, and process a buffer without crashing? Startup
 * is the risky moment, because that is where dfxpInit runs and where comInit
 * reaches into the outboard-card layer that no longer exists.
 *
 * Also runs the bypass null test. With powerOn(false) the DSP is meant to be
 * a pass-through, so output must equal input bit for bit. That is a genuine
 * null test available without any reference implementation: any deviation is
 * unambiguously a defect in the port.
 */

#include "DfxDsp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static const int SRATE  = 48000;
static const int NCH    = 2;
static const int FRAMES = 4800;          /* 100 ms */

/* Interleaved stereo sine, the same content every run. */
static void fill_sine(std::vector<float> &buf)
{
	for (int i = 0; i < FRAMES; i++) {
		float s = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)SRATE);
		buf[(size_t)i * 2 + 0] = s;
		buf[(size_t)i * 2 + 1] = s;
	}
}

int main(void)
{
	printf("constructing DfxDsp...\n");
	fflush(stdout);
	DfxDsp dsp;
	printf("  constructed\n");

	/* Upstream notes that this can report failure on the first calls and
	 * settle later, so it is called the way the real callback calls it:
	 * every buffer, not once. */
	int rc = 0;
	for (int i = 0; i < 4; i++) {
		rc = dsp.setSignalFormat(32, NCH, SRATE, 32);
		printf("  setSignalFormat(32, %d, %d, 32) -> %d\n", NCH, SRATE, rc);
	}

	printf("power on: %s\n", dsp.isPowerOn() ? "yes" : "no");
	printf("eq bands: %d\n", dsp.getNumEqBands());
	for (int e = 0; e < DfxDsp::NumEffects; e++)
		printf("  effect %d = %.3f\n", e, dsp.getEffectValue((DfxDsp::Effect)e));

	std::vector<float> input((size_t)FRAMES * NCH);
	std::vector<float> work((size_t)FRAMES * NCH);
	fill_sine(input);

	/* ---- 1. Bypass must be bit-exact ---------------------------------- */
	/* processTimer() no longer runs inside processAudio(); the control thread
	 * drives it after mutation. See DfxDsp.h. */
	dsp.powerOn(false);
	dsp.processTimer();
	work = input;
	dsp.processAudio((short *)work.data(), (short *)work.data(), FRAMES, 0);

	size_t differing = 0;
	for (size_t i = 0; i < work.size(); i++)
		if (memcmp(&work[i], &input[i], sizeof(float)) != 0)
			differing++;
	printf("\nbypass null test: %zu / %zu samples differ -> %s\n",
	       differing, work.size(), differing == 0 ? "PASS" : "FAIL");

	/* ---- 2. Processing must change the signal, and stay finite -------- */
	dsp.powerOn(true);
	dsp.processTimer();
	work = input;
	dsp.processAudio((short *)work.data(), (short *)work.data(), FRAMES, 0);

	size_t changed = 0, nonfinite = 0;
	float peak = 0.0f;
	for (size_t i = 0; i < work.size(); i++) {
		if (!std::isfinite(work[i]))
			nonfinite++;
		else if (fabsf(work[i]) > peak)
			peak = fabsf(work[i]);
		if (work[i] != input[i])
			changed++;
	}
	printf("processing:  %zu / %zu samples changed, peak %.4f, non-finite %zu\n",
	       changed, work.size(), peak, nonfinite);

	/* ---- 3. All effects at zero: unity boost, less the output ceiling -- */
	/*
	 * A regression guard for the LP64 struct misalignment (see codedefs.h).
	 * While the parameter structs were misaligned this read 0.1634 against an
	 * input peak of 0.2500 — audible, plausible, and completely wrong.
	 *
	 * The expected ratio is 0.9661, not 1.0. The Maximizer is never bypassed —
	 * Play32.c says so outright — and it always applies its output ceiling,
	 * max_output = 0.966051, which is exactly -0.300 dB. With every effect at
	 * zero the boost is unity and that ceiling is all that remains.
	 *
	 * This check read 1.0000 until the stereo-flag slot-index bug was fixed
	 * (see Play32.c), and passed for the wrong reason: the Maximizer was
	 * taking its mono path and leaving part of every buffer untouched, so the
	 * peak survived unscaled. The old threshold was calibrated against broken
	 * output. Peak is still the tell; exact equality is not expected, since
	 * the chain still runs.
	 */
	{
		DfxDsp flat;
		for (int i = 0; i < 4; i++)
			flat.setSignalFormat(32, NCH, SRATE, 32);
		for (int e = 0; e < DfxDsp::NumEffects; e++)
			flat.setEffectValue((DfxDsp::Effect)e, 0.0f);
		flat.powerOn(true);
		flat.processTimer();

		work = input;
		flat.processAudio((short *)work.data(), (short *)work.data(), FRAMES, 0);

		float in_peak = 0.0f, out_peak = 0.0f;
		for (size_t i = 0; i < work.size(); i++) {
			if (fabsf(input[i]) > in_peak) in_peak = fabsf(input[i]);
			if (std::isfinite(work[i]) && fabsf(work[i]) > out_peak)
				out_peak = fabsf(work[i]);
		}
		/* Maximizer max_output, -0.300 dB. See the note above. */
		const float MAXI_OUTPUT_CEILING = 0.966051f;
		float ratio = (in_peak > 0.0f) ? out_peak / in_peak : 0.0f;
		bool transparent = (ratio > MAXI_OUTPUT_CEILING * 0.99f &&
		                    ratio < MAXI_OUTPUT_CEILING * 1.01f);
		printf("\ntransparency (all effects 0): in %.4f -> out %.4f (x%.4f, "
		       "expected x%.4f) -> %s\n",
		       in_peak, out_peak, ratio, MAXI_OUTPUT_CEILING,
		       transparent ? "PASS" : "FAIL");
		if (!transparent)
			differing++;   /* fold into the overall result */
	}

	printf("\nresult: %s\n",
	       (differing == 0 && nonfinite == 0) ? "smoke test PASSED"
	                                          : "smoke test FAILED");
	return (differing == 0 && nonfinite == 0) ? 0 : 1;
}
