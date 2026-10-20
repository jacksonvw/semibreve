/*
 * Semibreve — Milestone 0 harness: WAV in, DfxDsp, WAV out.
 *
 * Throwaway by design. It exists to prove libdfxdsp processes real audio
 * correctly on Linux, and to generate the byte-identical outputs the
 * cross-compiler differential compares. Nothing downstream depends on it.
 *
 * No libsndfile: canonical RIFF/WAVE is about forty lines, and Milestone 0
 * stays dependency-free. Accepts 16-bit PCM and 32-bit float, mono or stereo,
 * and always hands DfxDsp 32-bit float — the format FxSound itself runs it at,
 * and the one PipeWire will deliver.
 *
 *   dfxproc in.wav out.wav [preset.fac] [--bypass]
 */

#include "DfxDsp.h"

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Wav {
	int sample_rate = 0;
	int channels    = 0;
	std::vector<float> samples;      /* interleaved */
};

uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8)); }

bool read_wav(const char *path, Wav &out, std::string &err)
{
	FILE *f = fopen(path, "rb");
	if (!f) { err = "cannot open input"; return false; }

	std::vector<uint8_t> buf;
	{
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (n <= 44) { fclose(f); err = "file too small to be a WAV"; return false; }
		buf.resize((size_t)n);
		size_t got = fread(buf.data(), 1, buf.size(), f);
		fclose(f);
		if (got != buf.size()) { err = "short read"; return false; }
	}

	if (memcmp(buf.data(), "RIFF", 4) != 0 || memcmp(buf.data() + 8, "WAVE", 4) != 0) {
		err = "not a RIFF/WAVE file";
		return false;
	}

	uint16_t fmt = 0, bits = 0;
	size_t pos = 12, data_off = 0, data_len = 0;
	while (pos + 8 <= buf.size()) {
		const uint8_t *ck = buf.data() + pos;
		uint32_t len = rd32(ck + 4);
		if (memcmp(ck, "fmt ", 4) == 0 && len >= 16) {
			fmt          = rd16(ck + 8);
			out.channels = rd16(ck + 10);
			out.sample_rate = (int)rd32(ck + 12);
			bits         = rd16(ck + 22);
		} else if (memcmp(ck, "data", 4) == 0) {
			data_off = pos + 8;
			data_len = len;
			if (data_off + data_len > buf.size())
				data_len = buf.size() - data_off;   /* tolerate a truncated tail */
		}
		pos += 8 + len + (len & 1);                 /* chunks are word-aligned */
	}
	if (!data_off || out.channels <= 0) { err = "missing fmt or data chunk"; return false; }

	/* 1 = PCM, 3 = IEEE float, 0xFFFE = extensible (subformat assumed to match bits) */
	if (fmt == 1 && bits == 16) {
		size_t n = data_len / 2;
		out.samples.resize(n);
		for (size_t i = 0; i < n; i++) {
			int16_t s = (int16_t)rd16(buf.data() + data_off + i * 2);
			out.samples[i] = (float)s / 32768.0f;
		}
	} else if ((fmt == 3 || fmt == 0xFFFE) && bits == 32) {
		size_t n = data_len / 4;
		out.samples.resize(n);
		for (size_t i = 0; i < n; i++) {
			uint32_t u = rd32(buf.data() + data_off + i * 4);
			float v;
			memcpy(&v, &u, 4);
			out.samples[i] = v;
		}
	} else {
		char msg[96];
		snprintf(msg, sizeof(msg), "unsupported format tag %u / %u bits", fmt, bits);
		err = msg;
		return false;
	}
	return true;
}

bool write_wav_f32(const char *path, const Wav &w, std::string &err)
{
	FILE *f = fopen(path, "wb");
	if (!f) { err = "cannot open output"; return false; }

	uint32_t data_bytes  = (uint32_t)(w.samples.size() * 4);
	uint32_t byte_rate   = (uint32_t)(w.sample_rate * w.channels * 4);
	uint16_t block_align = (uint16_t)(w.channels * 4);
	uint8_t h[44];

	memcpy(h, "RIFF", 4);
	uint32_t riff = 36 + data_bytes;
	h[4] = riff & 255; h[5] = (riff >> 8) & 255; h[6] = (riff >> 16) & 255; h[7] = (riff >> 24) & 255;
	memcpy(h + 8, "WAVEfmt ", 8);
	h[16] = 16; h[17] = h[18] = h[19] = 0;              /* fmt chunk size */
	h[20] = 3;  h[21] = 0;                              /* IEEE float */
	h[22] = (uint8_t)w.channels; h[23] = 0;
	h[24] = w.sample_rate & 255; h[25] = (w.sample_rate >> 8) & 255;
	h[26] = (w.sample_rate >> 16) & 255; h[27] = (w.sample_rate >> 24) & 255;
	h[28] = byte_rate & 255; h[29] = (byte_rate >> 8) & 255;
	h[30] = (byte_rate >> 16) & 255; h[31] = (byte_rate >> 24) & 255;
	h[32] = block_align & 255; h[33] = (block_align >> 8) & 255;
	h[34] = 32; h[35] = 0;                              /* bits per sample */
	memcpy(h + 36, "data", 4);
	h[40] = data_bytes & 255; h[41] = (data_bytes >> 8) & 255;
	h[42] = (data_bytes >> 16) & 255; h[43] = (data_bytes >> 24) & 255;

	bool ok = fwrite(h, 1, 44, f) == 44 &&
	          fwrite(w.samples.data(), 4, w.samples.size(), f) == w.samples.size();
	fclose(f);
	if (!ok) err = "write failed";
	return ok;
}

std::wstring widen(const char *s)
{
	size_t n = mbstowcs(NULL, s, 0);
	if (n == (size_t)-1) return std::wstring();
	std::vector<wchar_t> w(n + 1);
	mbstowcs(w.data(), s, n + 1);
	return std::wstring(w.data());
}

} /* namespace */

int main(int argc, char **argv)
{
	/* glibc decodes preset bytes through LC_CTYPE; see dsp/linux/pt_stream.h. */
	setlocale(LC_ALL, "");

	if (argc < 3) {
		fprintf(stderr, "usage: dfxproc <in.wav> <out.wav> [preset.fac] [--bypass]\n");
		return 2;
	}
	const char *in_path  = argv[1];
	const char *out_path = argv[2];
	const char *preset   = NULL;
	bool bypass = false;
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--bypass") == 0) bypass = true;
		else preset = argv[i];
	}

	Wav w;
	std::string err;
	if (!read_wav(in_path, w, err)) {
		fprintf(stderr, "dfxproc: %s: %s\n", in_path, err.c_str());
		return 1;
	}
	size_t frames = w.channels ? w.samples.size() / (size_t)w.channels : 0;
	fprintf(stderr, "in:  %d Hz, %d ch, %zu frames\n", w.sample_rate, w.channels, frames);

	DfxDsp dsp;
	/* Called every buffer, exactly as the real callback does — it no-ops
	 * internally unless the format actually changed. */
	for (int i = 0; i < 4; i++)
		dsp.setSignalFormat(32, w.channels, w.sample_rate, 32);

	if (preset) {
		if (dsp.loadPreset(widen(preset)) != 0)
			fprintf(stderr, "dfxproc: warning: loadPreset did not report success\n");
		/* loadPreset only fills DfxDsp's state; the setters are what reach the
		 * live filters. Upstream's GUI does exactly this after every load. */
		for (int e = 0; e < DfxDsp::NumEffects; e++)
			dsp.setEffectValue((DfxDsp::Effect)e,
			                   dsp.getEffectValue((DfxDsp::Effect)e) * 10.0f);
		for (int b = 0; b < dsp.getNumEqBands(); b++) {
			dsp.setEqBandFrequency(b, dsp.getEqBandFrequency(b));
			dsp.setEqBandBoostCut(b, dsp.getEqBandBoostCut(b));
		}
		dsp.eqOn(true);
		DfxPreset info = dsp.getPresetInfo(widen(preset));
		fprintf(stderr, "preset: \"%ls\"\n", info.name.c_str());
	}
	dsp.powerOn(!bypass);
	fprintf(stderr, "mode: %s\n", bypass ? "bypass" : "processing");

	/* processAudio() used to call processTimer() on entry, so the very first
	 * block picked up everything set above. It no longer does — that put a
	 * mutex and a map walk inside what will be PipeWire's callback. The
	 * control thread drives it now, and here that means: after all mutation,
	 * before the first block. Without this the preset never reaches the
	 * filters and the output is silently wrong. */
	dsp.processTimer();

	/* Fixed 1024-frame blocks so output does not depend on file length —
	 * the differential compares files byte for byte. */
	const size_t BLOCK = 1024;
	for (size_t off = 0; off < frames; off += BLOCK) {
		size_t n = (off + BLOCK <= frames) ? BLOCK : frames - off;
		float *p = w.samples.data() + off * (size_t)w.channels;
		dsp.processAudio((short *)p, (short *)p, (int)n, 0);
	}

	size_t nonfinite = 0;
	double sum_sq = 0.0;
	float peak = 0.0f;
	for (size_t i = 0; i < w.samples.size(); i++) {
		if (!std::isfinite(w.samples[i])) { nonfinite++; continue; }
		sum_sq += (double)w.samples[i] * w.samples[i];
		if (fabsf(w.samples[i]) > peak) peak = fabsf(w.samples[i]);
	}
	fprintf(stderr, "out: peak %.6f, rms %.6f, non-finite %zu\n",
	        peak, w.samples.empty() ? 0.0 : sqrt(sum_sq / (double)w.samples.size()),
	        nonfinite);

	if (!write_wav_f32(out_path, w, err)) {
		fprintf(stderr, "dfxproc: %s: %s\n", out_path, err.c_str());
		return 1;
	}
	return nonfinite ? 1 : 0;
}
