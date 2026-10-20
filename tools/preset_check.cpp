/*
 * Semibreve — Milestone 0 preset round-trip check.
 *
 * Loads a .fac preset through the ported DfxDsp, reads every parameter back
 * through the public API, and prints it beside the value written in the file.
 *
 * This is the verification that replaces a null test against Windows FxSound.
 * The two builds compile the same C, so the coefficients cannot differ; what
 * a port can plausibly break is the path that gets numbers out of a preset
 * file and into the DSP — wide-character handling, the swprintf compatibility
 * layer, file I/O, scaling. All of that sits between the text on the left and
 * the value on the right, and .fac files are plain ASCII, so the expected
 * answer is legible without a reference implementation.
 *
 * Note the asymmetry in the effect API: setEffectValue divides its argument
 * by ten while getEffectValue returns the raw stored value, so a "Main N"
 * field of 50 in the file reads back as 5.0. Upstream's own GUI compensates
 * with a get-then-set-times-ten round trip on load. The x10 column below
 * undoes it so the comparison against the file is direct.
 */

#include "DfxDsp.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char *EFFECT_NAME[] = {
	"Fidelity", "Ambience", "Surround", "DynamicBoost", "Bass"
};

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: preset_check <preset.fac>\n");
		return 2;
	}

	/* glibc decodes the preset file's bytes through LC_CTYPE when reading
	 * wide lines, so a non-ASCII preset name needs a UTF-8 locale. See
	 * dsp/linux/pt_stream.h. */
	setlocale(LC_ALL, "");

	/* mbstowcs is fine here: this is the harness's own argv, not the DSP's
	 * internal conversion path. */
	std::wstring path;
	{
		size_t n = mbstowcs(NULL, argv[1], 0);
		if (n == (size_t)-1) {
			fprintf(stderr, "bad path encoding\n");
			return 2;
		}
		std::vector<wchar_t> w(n + 1);
		mbstowcs(w.data(), argv[1], n + 1);
		path.assign(w.data());
	}

	DfxDsp dsp;
	for (int i = 0; i < 4; i++)
		dsp.setSignalFormat(32, 2, 48000, 32);

	DfxPreset info = dsp.getPresetInfo(path);
	printf("getPresetInfo name: \"%ls\"\n", info.name.c_str());

	int rc = dsp.loadPreset(path);
	printf("loadPreset -> %d  (0 = OKAY)\n", rc);
	if (rc != 0)
		fprintf(stderr, "WARNING: loadPreset did not report success\n");

	/*
	 * loadPreset only populates DfxDsp's own state — it does not reach the
	 * processing chain. Upstream's GUI follows every load with this loop, and
	 * it is not the no-op it looks like: the setters are what push values into
	 * the live filters. Reading a value back and setting it again is the
	 * documented way to apply a preset, and the x10 undoes setEffectValue's
	 * internal divide by ten.
	 *
	 * Without it the EQ loads, reads back correctly, and processes nothing —
	 * every SOS section stays flagged off and audio passes through untouched.
	 */
	for (int e = 0; e < DfxDsp::NumEffects; e++)
		dsp.setEffectValue((DfxDsp::Effect)e,
		                   dsp.getEffectValue((DfxDsp::Effect)e) * 10.0f);
	for (int b = 0; b < dsp.getNumEqBands(); b++) {
		dsp.setEqBandFrequency(b, dsp.getEqBandFrequency(b));
		dsp.setEqBandBoostCut(b, dsp.getEqBandBoostCut(b));
	}
	dsp.eqOn(true);
	printf("preset applied to processing chain\n\n");

	printf("effects (file 'Main N' should equal the x10 column):\n");
	for (int e = 0; e < DfxDsp::NumEffects; e++) {
		float v = dsp.getEffectValue((DfxDsp::Effect)e);
		printf("  %-13s raw %8.4f    x10 %8.2f\n", EFFECT_NAME[e], v, v * 10.0f);
	}

	printf("\nmasters:\n");
	printf("  master gain      %8.3f\n", dsp.getMasterGain());
	printf("  volume leveling  %8.3f\n", dsp.getVolumeLeveling());
	printf("  normalization    %8.3f\n", dsp.getNormalization());
	printf("  balance          %8.3f\n", dsp.getBalance());
	printf("  filter Q         %8.3f\n", dsp.getFilterQ());

	int bands = dsp.getNumEqBands();
	printf("\neq: %d bands (file lists its own count and Band N entries)\n", bands);
	for (int b = 0; b < bands; b++) {
		float f = dsp.getEqBandFrequency(b);
		float g = dsp.getEqBandBoostCut(b);
		printf("  band %2d   %10.3f Hz   %+8.4f dB\n", b, f, g);
	}

	/* Does a loaded preset actually alter the signal? */
	const int FRAMES = 4800;
	std::vector<float> in((size_t)FRAMES * 2), work;
	for (int i = 0; i < FRAMES; i++) {
		float s = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 48000.0f);
		in[(size_t)i * 2] = in[(size_t)i * 2 + 1] = s;
	}

	dsp.powerOn(true);
	/* processTimer() no longer runs inside processAudio(); the control thread
	 * drives it after mutation. Without this the preset set above never
	 * reaches the filters. See DfxDsp.h. */
	dsp.processTimer();
	work = in;
	dsp.processAudio((short *)work.data(), (short *)work.data(), FRAMES, 0);

	size_t changed = 0, nonfinite = 0;
	double sum_sq_in = 0.0, sum_sq_out = 0.0;
	for (size_t i = 0; i < work.size(); i++) {
		if (!std::isfinite(work[i])) { nonfinite++; continue; }
		if (work[i] != in[i]) changed++;
		sum_sq_in  += (double)in[i] * in[i];
		sum_sq_out += (double)work[i] * work[i];
	}
	printf("\nprocessing with this preset:\n");
	printf("  power on         %s\n", dsp.isPowerOn() ? "yes" : "no");
	printf("  samples changed  %zu / %zu\n", changed, work.size());
	printf("  rms in / out     %.6f / %.6f\n",
	       sqrt(sum_sq_in / (double)work.size()),
	       sqrt(sum_sq_out / (double)work.size()));
	printf("  non-finite       %zu\n", nonfinite);

	return (nonfinite == 0) ? 0 : 1;
}
