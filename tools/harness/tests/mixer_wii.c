/*
MIXER_WII.C (test)

The Wii's mixing in port/linux/src/dsound_sdl.c (its buses, upsampler, voices,
reverb and limiter, built with HALO_WII; test_mixer_wii.py takes them as
under_test.inc) on known signals:
	- a voice at a bus's rate is its frames: a 44 kHz voice's are the
	  output's, a 22 kHz voice's are the low bus's doubled by the upsampler,
	  against the same half-band low pass worked out in double precision;
	- sines through a 22 kHz voice as the ear hears them: flat to 9.5 kHz,
	  the images 70 dB down;
	- a voice played at another frequency (the cubic): the tone where it
	  should be, what else comes out well down;
	- a voice out of earshot moves on as one heard does, and is heard the
	  same once it comes into earshot;
	- mono voices heard as stereo ones with both channels the same are;
	- a scene mixed in pieces of any size, odd ones too, is the scene mixed
	  in one;
	- a voice that runs out of packets stops, and starts over with the next.
"bench" mixes a scene of voices in the Wii's pieces (instructions a frame under
qemu-ppc: test_mixer_wii.py's note).
*/

#include "harness.h"

#include <math.h>
#include <pthread.h>

typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef void *LPVOID;
typedef void (*LPFNXMEDIAOBJECTCALLBACK)(void *, void *, DWORD);
typedef struct { void *lpVtbl; } IDirectSoundStream;
typedef struct { void *pvBuffer; DWORD dwMaxSize; } XMEDIAPACKET;
typedef struct
{
	LONG lRoom, lRoomHF;
	float flRoomRolloffFactor, flDecayTime, flDecayHFRatio;
	LONG lReflections;
	float flReflectionsDelay;
	LONG lReverb;
	float flReverbDelay, flDiffusion, flDensity, flHFReference;
} DSI3DL2LISTENER;
#define DSBVOLUME_MIN (-10000)
#define DS3DMODE_NORMAL 0x00000000
#define DS3DMODE_HEADRELATIVE 0x00000001
#define DS3DMODE_DISABLE 0x00000002

/* the players' voices (voice_audio.c): none */
static void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

#include "under_test.inc"

#define PI 3.14159265358979323846
#define CHUNK 144 /* frames a mix, as WiiLibs' audio thread asks */
/* the frames the limiter delays the output by */
#define LIMITER_DELAY (LIMITER_LOOKAHEAD - 1)

/* ---------- voices */

static void voice_add(struct sdl_stream *stream)
{
	stream->next = streams;
	streams = stream;
}

static void voices_clear(void)
{
	while (streams)
	{
		struct sdl_stream *next = streams->next;

		free((void *)streams->packets[0].source);
		free(streams);
		streams = next;
	}
	/* (and what the mix holds of them: the low bus's last frames, the
	limiter's delay) */
	memset(&wii_mix, 0, sizeof(wii_mix));
	memset(&limiter, 0, sizeof(limiter));
}

/* count 16-bit samples as a packet keeps them: little-endian PCM */
static unsigned char *packet_pcm(const short *samples, unsigned long count)
{
	unsigned char *bytes = malloc(count * 2);
	unsigned long n;

	for (n = 0; n < count; n++)
	{
		bytes[n * 2] = (unsigned char)(samples[n] & 0xff);
		bytes[n * 2 + 1] = (unsigned char)((samples[n] >> 8) & 0xff);
	}
	return bytes;
}

/* a voice of frames frames of channels channels at rate, played at frequency
(0: its rate), from a packet of bytes (which it frees), Xbox ADPCM or PCM; a
2D voice heard at full volume in both speakers, mixed */
static struct sdl_stream *voice_from(unsigned char *bytes, BOOL adpcm, unsigned long frames, unsigned long channels,
	unsigned long rate, unsigned long frequency)
{
	struct sdl_stream *stream = calloc(1, sizeof(*stream));

	stream->adpcm = adpcm;
	stream->channels = channels;
	stream->sample_rate = rate;
	stream->frequency = frequency;
	stream->volume = 1.0f;
	stream->mix_left = stream->mix_right = 1.0f;
	stream->room = stream->room_hf = DSBVOLUME_MIN;
	stream->packets[0].source = bytes;
	stream->packets[0].frames = frames;
	stream->packet_count = 1;
	resampler_reset(stream);
	voice_add(stream);
	return stream;
}

/* the same, of 16-bit samples */
static struct sdl_stream *voice_new(const short *samples, unsigned long frames, unsigned long channels,
	unsigned long rate, unsigned long frequency)
{
	return voice_from(packet_pcm(samples, frames * channels), FALSE, frames, channels, rate, frequency);
}

/* frames of the mix, in pieces of CHUNK, stereo, with the limiter's delay
taken out */
static void render(float *output, unsigned long frames)
{
	static float buffer[CHUNK * 2];
	unsigned long done = 0, skip = LIMITER_DELAY, frame;

	while (done < frames)
	{
		mix(buffer, CHUNK);
		for (frame = skip; frame < CHUNK && done < frames; frame++, done++)
		{
			output[done * 2] = buffer[frame * 2];
			output[done * 2 + 1] = buffer[frame * 2 + 1];
		}
		skip = 0;
	}
}

/* ---------- signals */

static unsigned long random_state = 1;

static double random_uniform(void)
{
	random_state = random_state * 1103515245UL + 12345UL;
	return (double)((random_state >> 8) & 0xffffff) / 16777216.0 * 2.0 - 1.0;
}

static short *noise(unsigned long samples, double amplitude)
{
	short *signal = malloc(samples * sizeof(short));
	unsigned long n;

	random_state = 1;
	for (n = 0; n < samples; n++)
		signal[n] = (short)lround(amplitude * 32767.0 * random_uniform());
	return signal;
}

/* blocks of Xbox ADPCM of channels channels: random headers (a sample and a
step index of 0 to 88) and nibbles; the same each time */
static unsigned char *adpcm_noise(unsigned long blocks, unsigned long channels)
{
	unsigned long size = blocks * XBOX_ADPCM_BLOCK_BYTES * channels, n, channel;
	unsigned char *bytes = malloc(size);

	random_state = 3;
	for (n = 0; n < size; n++)
		bytes[n] = (unsigned char)(((random_uniform() + 1.0) * 128.0)) & 0xff;
	for (n = 0; n < blocks; n++)
	{
		for (channel = 0; channel < channels; channel++)
			bytes[n * XBOX_ADPCM_BLOCK_BYTES * channels + channel * 4 + 2] %= 89;
	}
	return bytes;
}

static short *sine(unsigned long frames, double hertz, unsigned long rate, double amplitude)
{
	short *signal = malloc(frames * sizeof(short));
	unsigned long n;

	for (n = 0; n < frames; n++)
		signal[n] = (short)lround(amplitude * 32767.0 * sin(2.0 * PI * hertz * n / rate));
	return signal;
}

/* ---------- figures */

/* the amplitude of a sine of frequency cycles a frame in signal[first, last)
(every other sample: one channel of stereo), and the power of the rest
relative to the sine's, in dB */
static void tone_fit(const float *signal, unsigned long first, unsigned long last, double frequency,
	double *amplitude, double *rest_db)
{
	double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0, a, b, determinant, rest = 0, tone = 0;
	unsigned long n;

	for (n = first; n < last; n++)
	{
		double s = sin(2.0 * PI * frequency * n), c = cos(2.0 * PI * frequency * n);

		ss += s * s;
		sc += s * c;
		cc += c * c;
		ys += signal[n * 2] * s;
		yc += signal[n * 2] * c;
	}
	determinant = ss * cc - sc * sc;
	a = (ys * cc - yc * sc) / determinant;
	b = (yc * ss - ys * sc) / determinant;
	for (n = first; n < last; n++)
	{
		double fit = a * sin(2.0 * PI * frequency * n) + b * cos(2.0 * PI * frequency * n);

		rest += (signal[n * 2] - fit) * (signal[n * 2] - fit);
		tone += fit * fit;
	}
	*amplitude = sqrt(a * a + b * b);
	*rest_db = 10.0 * log10(rest / tone + 1e-30);
}

/* a sine of hertz in a mono voice at rate played at frequency: its gain
through the mix (dB) and the power of everything else that comes out,
relative to it (dB) */
static void sine_figures(double hertz, unsigned long rate, unsigned long frequency, double *gain_db, double *rest_db)
{
	unsigned long frames = rate, outputs = OUTPUT_RATE / 2;
	double played = hertz * (frequency ? (double)frequency / rate : 1.0), amplitude;
	short *signal = sine(frames, hertz, rate, 0.5);
	float *output = calloc(outputs * 2, sizeof(float));

	voice_new(signal, frames, 1, rate, frequency);
	render(output, outputs);
	tone_fit(output, outputs / 4, outputs * 3 / 4, played / OUTPUT_RATE, &amplitude, rest_db);
	*gain_db = 20.0 * log10(amplitude / 0.5 + 1e-30);
	voices_clear();
	free(output);
	free(signal);
}

/* the upsampler's low pass in double precision: the value between low bus
frames n and n + 1 */
static double reference_between(const double *frames, long count, long n)
{
	double sum = 0.0, total = 0.0;
	int k;

	for (k = 0; k < UPSAMPLER_PAIRS; k++)
	{
		double distance = k + 0.5, edge = distance / UPSAMPLER_PAIRS, angle = PI * distance;
		double weight = sin(angle) / angle * bessel_i0(UPSAMPLER_KAISER_BETA * sqrt(1.0 - edge * edge)) /
			bessel_i0(UPSAMPLER_KAISER_BETA);

		total += 2.0 * weight;
		if (n - k >= 0 && n - k < count)
			sum += weight * frames[n - k];
		if (n + 1 + k >= 0 && n + 1 + k < count)
			sum += weight * frames[n + 1 + k];
	}
	return sum / total;
}

/* ---------- a scene, for "chunks" and "bench" */

/* mono 22 kHz voices in the world (half muffled, some pitched), stereo 44
and 22 kHz ones, silent ones out of earshot; each a packet as long as the
scene */
static void scene(unsigned long mono, unsigned long stereo44, unsigned long stereo22, unsigned long silent,
	BOOL indoor, BOOL adpcm, unsigned long frames)
{
	short *monos = noise(frames, 0.25), *stereos = noise(frames * 2, 0.25);
	unsigned long v, blocks = frames / XBOX_ADPCM_BLOCK_SAMPLES;

	if (indoor)
	{
		DSI3DL2LISTENER room = { -1000, -100, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f };

		environment = room;
		environment_serial++;
	}
	for (v = 0; v < mono + silent + stereo44 + stereo22; v++)
	{
		BOOL heard = v < mono, world = v < mono + silent, wide = v >= mono + silent + stereo22;
		unsigned long channels = world ? 1 : 2, rate = wide ? 44100 : 22050;
		unsigned long frequency = world && v % 5 == 0 ? 22050 + 2000 * (v % 3) : 0;
		struct sdl_stream *stream = adpcm ?
			voice_from(adpcm_noise(blocks, channels), TRUE, blocks * XBOX_ADPCM_BLOCK_SAMPLES, channels, rate,
				frequency) :
			voice_new(channels == 1 ? monos : stereos, frames, channels, rate, frequency);

		/* (ADPCM noise is at full scale) */
		if (adpcm)
			stream->volume = 0.25f;
		if (!world)
			continue;
		stream->has_3d = TRUE;
		stream->mode = DS3DMODE_NORMAL;
		stream->position[0] = (float)(v % 7) - 3.0f;
		stream->position[2] = heard ? 2.0f : 1000.0f;
		stream->minimum_distance = 1.0f;
		stream->maximum_distance = heard ? 50.0f : 100.0f;
		stream->direct = heard ? 0 : DSBVOLUME_MIN;
		stream->direct_hf = v % 2 ? -500 : 0;
		stream->room = heard ? 0 : DSBVOLUME_MIN;
		stream->room_hf = 0;
	}
	free(monos);
	free(stereos);
}

/* ---------- the reverb */

#define REVERB_BLOCK 144 /* low bus frames a mix gives it */

static unsigned long reverb_serial;

static double random_number(void)
{
	static unsigned long state = 12345;

	state = state * 1664525UL + 1013904223UL;
	return (double)(state >> 8) / 16777216.0;
}

/* a room of the given decay time, in a reverb with nothing in it */
static void reverb_room(float decay, LONG reflections, LONG reverberation)
{
	DSI3DL2LISTENER room = { -1000, -100, 0.0f, decay, 0.83f, reflections, 0.007f, reverberation, 0.011f, 100.0f,
		100.0f, 5000.0f };

	reverb_clear();
	reverb_update(&room, ++reverb_serial);
	reverb.level = 1.0f;
}

/* frames of the low bus through the reverb in the mixer's blocks: stereo output in place of the output. With
alternate, the level it is moving to is a little over and a little under the level it is at, in turn: the reverb
does not take its steady path */
static void reverb_run_blocks(const float *input, float *output, unsigned long frames, float target, BOOL alternate)
{
	unsigned long done;

	memset(output, 0, frames * 2 * sizeof(float));
	for (done = 0; done + REVERB_BLOCK <= frames; done += REVERB_BLOCK)
	{
		float level = alternate ? nextafterf(target, (done / REVERB_BLOCK) % 2 ? 0.0f : 2.0f) : target;

		reverb_process(input + done, output + done * 2, REVERB_BLOCK, level);
	}
}

/* the time an impulse takes to decay by 60 dB (extrapolated from the fall from -5 to -25 dB of its energy,
backwards integrated), and the energy that comes out of low noise over the energy that went in (the room level is
0 mB, a gain of 1, so about 1) */
static void reverb_figures(float decay, float *rt60, float *ratio)
{
	const unsigned long frames = 22050UL * 16;
	float *input = calloc(frames, sizeof(float)), *output = calloc(frames * 2, sizeof(float));
	double remaining, total, in_energy = 0.0, out_energy = 0.0, low = 0.0;
	unsigned long n, t5 = 0, t25 = 0;
	double *energy = calloc(frames, sizeof(double));

	reverb_room(decay, -10000, 0);
	input[0] = 1.0f;
	reverb_run_blocks(input, output, frames, 1.0f, FALSE);
	for (n = 0; n < frames; n++)
		energy[n] = (double)output[2 * n] * output[2 * n];
	total = 0.0;
	for (n = frames; n-- > 0;)
	{
		total += energy[n];
		energy[n] = total;
	}
	for (n = 0; n < frames; n++)
	{
		remaining = 10.0 * log10(energy[n] / energy[0] + 1.0e-30);
		if (!t5 && remaining < -5.0)
			t5 = n;
		if (!t25 && remaining < -25.0)
			t25 = n;
	}
	*rt60 = 3.0f * (float)(t25 - t5) / 22050.0f;

	reverb_room(decay, -10000, 0);
	for (n = 0; n < 22050UL * 3; n++)
	{
		low = 0.995 * low + 0.005 * (random_number() - 0.5);
		input[n] = (float)low;
		in_energy += (double)input[n] * input[n];
	}
	for (; n < frames; n++)
		input[n] = 0.0f;
	reverb_run_blocks(input, output, frames, 1.0f, FALSE);
	for (n = 0; n < frames; n++)
		out_energy += (double)output[2 * n] * output[2 * n];
	*ratio = (float)(out_energy / in_energy);
	free(input);
	free(output);
	free(energy);
}

static void start(void)
{
	upsampler_initialize();
	reverb_initialize();
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	unsigned long n;

	start();

	/* a voice at a bus's rate is its frames: a 44 kHz one's the output's,
	a 22 kHz one's the low bus's doubled by the upsampler, exactly as its
	low pass worked out in double precision does */
	CASE("native")
	{
		short *signal = noise(20000, 0.4);
		double *exact = malloc(20000 * sizeof(double)), worst = 0.0;
		float *output = calloc(40000 * 2, sizeof(float));

		voice_new(signal, 20000, 1, 44100, 0);
		render(output, 20000);
		for (n = 0; n < 20000; n++)
		{
			CHECK(output[n * 2] == output[n * 2 + 1], "frame %lu: the channels differ", n);
			CHECK(fabsf(output[n * 2] - signal[n] / 32768.0f) < 1.0e-7f, "44 kHz: frame %lu is %f, not %f", n,
				output[n * 2], signal[n] / 32768.0f);
		}
		voices_clear();
		for (n = 0; n < 20000; n++)
			exact[n] = signal[n] / 32768.0;
		voice_new(signal, 20000, 1, 22050, 0);
		render(output, 40000);
		/* (the low bus is UPSAMPLER_PAIRS frames late) */
		for (n = 2 * UPSAMPLER_PAIRS; n + 2 * UPSAMPLER_PAIRS < 40000; n++)
		{
			long low = (long)n / 2 - UPSAMPLER_PAIRS;
			double expected = n % 2 ? reference_between(exact, 20000, low) : exact[low];
			double error = fabs(output[n * 2] - expected);

			worst = error > worst ? error : worst;
		}
		CHECK(20.0 * log10(worst / 0.4) < -110.0, "22 kHz: the upsampler's error peaks at %.1f dB",
			20.0 * log10(worst / 0.4));
		free(output);
		free(exact);
		free(signal);
		return 0;
	}
	/* sines in a 22 kHz voice as heard: flat within a twentieth of a dB to
	9.5 kHz, the rest (the images, and 16-bit's rounding) 65 to 80 dB down; at
	10.5 kHz, in the upsampler's transition, within 2 dB */
	CASE("sines")
	{
		static const struct { double hertz, gain_db, rest_db; } sines[] = {
			{ 100.0, 0.05, -85.0 }, { 1000.0, 0.05, -80.0 }, { 5000.0, 0.05, -80.0 }, { 8000.0, 0.05, -80.0 },
			{ 9500.0, 0.05, -65.0 }, { 10500.0, 2.0, -10.0 },
		};
		unsigned long index;

		for (index = 0; index < sizeof(sines) / sizeof(sines[0]); index++)
		{
			double gain_db, rest_db;

			sine_figures(sines[index].hertz, 22050, 0, &gain_db, &rest_db);
			CHECK(fabs(gain_db) < sines[index].gain_db && rest_db < sines[index].rest_db,
				"%.0f Hz: gain %.3f dB, the rest %.1f dB", sines[index].hertz, gain_db, rest_db);
		}
		return 0;
	}
	/* a voice played at another frequency: the tone moved with it, what else
	the cubic leaves (its images, near the output's top for a voice played
	near its rate) 30 to 65 dB down, a tone at a fifth of the voice's rate a
	quarter of a dB down; a 22 kHz voice played at 44 kHz is the high bus's
	frames as they are */
	CASE("pitched")
	{
		static const struct { unsigned long rate, frequency; double hertz, gain_db, rest_db; } sines[] = {
			{ 22050, 23456, 1000.0, 0.1, -60.0 }, { 22050, 20000, 1000.0, 0.1, -60.0 },
			{ 22050, 23456, 4000.0, 0.25, -30.0 }, { 22050, 44100, 2000.0, 0.05, -85.0 },
			{ 44100, 48000, 2000.0, 0.1, -60.0 }, { 44100, 40000, 8000.0, 0.25, -30.0 },
		};
		unsigned long index;

		for (index = 0; index < sizeof(sines) / sizeof(sines[0]); index++)
		{
			double gain_db, rest_db;

			sine_figures(sines[index].hertz, sines[index].rate, sines[index].frequency, &gain_db, &rest_db);
			CHECK(fabs(gain_db) < sines[index].gain_db && rest_db < sines[index].rest_db,
				"%.0f Hz at %lu Hz, played at %lu: gain %.3f dB, the rest %.1f dB", sines[index].hertz,
				sines[index].rate, sines[index].frequency, gain_db, rest_db);
		}
		return 0;
	}
	/* a voice out of earshot moves on as one heard does: once it is audible
	again, both play the same, but for the frames its window held as it came
	back (silence in place of the frames before, at a gain rising from 0) */
	CASE("silent-voice")
	{
		static const unsigned long frequencies[][2] = { { 22050, 0 }, { 22050, 23456 }, { 22050, 70000 },
			{ 44100, 0 }, { 44100, 30000 } };
		short *signal = noise(60000, 0.4);
		unsigned long index, sizes[] = { 1, 7, 512, 333, 300, 144 };
		/* each voice's buses: low, its send, high (twice as long), its send */
		static float buses[2][4][1024 * 2];

		/* (each rate's voices of PCM, then of ADPCM) */
		for (index = 0; index < 2 * sizeof(frequencies) / sizeof(frequencies[0]); index++)
		{
			unsigned long rate = frequencies[index / 2][0], frequency = frequencies[index / 2][1], done = 0, size = 0;
			BOOL adpcm = index % 2;
			struct sdl_stream *loud = adpcm ? voice_from(adpcm_noise(940, 1), TRUE, 940 * 64, 1, rate, frequency) :
				voice_new(signal, 60000, 1, rate, frequency);
			struct sdl_stream *quiet = adpcm ? voice_from(adpcm_noise(940, 1), TRUE, 940 * 64, 1, rate, frequency) :
				voice_new(signal, 60000, 1, rate, frequency);
			int voice;

			/* the quiet one turned down for a while, at various mix sizes,
			then up */
			quiet->volume = 0.0f;
			for (done = 0; done < 10000; done += sizes[size++ % 6])
			{
				unsigned long count = sizes[size % 6];

				memset(buses, 0, sizeof(buses));
				mix_voice_wii(quiet, buses[0][0], buses[0][1], buses[0][2], buses[0][3], count);
				mix_voice_wii(loud, buses[1][0], buses[1][1], buses[1][2], buses[1][3], count);
				for (n = 0; n < 4 * 1024 * 2; n++)
					CHECK((&buses[0][0][0])[n] == 0.0f, "a voice turned down was heard");
			}
			CHECK(quiet->phase == loud->phase && quiet->cursor == loud->cursor && quiet->silence == loud->silence,
				"%lu Hz played at %lu: turned down, the voice is at %lu + %f, heard at %lu + %f", rate, frequency,
				quiet->cursor, quiet->phase, loud->cursor, loud->phase);
			/* (it fades in over a mix from 0; the loud one does the same) */
			quiet->volume = 1.0f;
			voice_parameters_serial++;
			loud->current_left = loud->current_right = 0.0f;
			for (done = 0; done < 5000; done += 300)
			{
				memset(buses, 0, sizeof(buses));
				for (voice = 0; voice < 2; voice++)
				{
					mix_voice_wii(voice ? loud : quiet, buses[voice][0], buses[voice][1], buses[voice][2],
						buses[voice][3], 300);
				}
				/* (the first mix: a hundredth at most, a window's frames at a
				gain of a few hundredths) */
				for (n = 0; n < 4 * 1024 * 2; n++)
				{
					float difference = fabsf((&buses[0][0][0])[n] - (&buses[1][0][0])[n]);

					CHECK(done ? difference == 0.0f : difference < 0.01f,
						"%lu Hz played at %lu: sample %lu of mix %lu differs (%f, %f)", rate, frequency, n,
						done / 300, (&buses[0][0][0])[n], (&buses[1][0][0])[n]);
				}
			}
			voices_clear();
		}
		free(signal);
		return 0;
	}
	/* a mono voice is heard as a stereo one with the same two channels */
	CASE("mono")
	{
		static const unsigned long rates[][2] = { { 22050, 0 }, { 44100, 0 }, { 22050, 23456 }, { 44100, 30000 },
			{ 22050, 60000 } };
		short *signal = noise(30000, 0.4), *pairs = malloc(30000 * 2 * sizeof(short));
		static float mono[30000 * 2], stereo[30000 * 2];
		unsigned long index;

		for (n = 0; n < 30000; n++)
			pairs[n * 2] = pairs[n * 2 + 1] = signal[n];
		for (index = 0; index < sizeof(rates) / sizeof(rates[0]); index++)
		{
			voice_new(signal, 30000, 1, rates[index][0], rates[index][1]);
			render(mono, 30000);
			voices_clear();
			voice_new(pairs, 30000, 2, rates[index][0], rates[index][1]);
			render(stereo, 30000);
			voices_clear();
			for (n = 0; n < 30000 * 2; n++)
				CHECK(fabsf(mono[n] - stereo[n]) < 1.0e-6f, "%lu Hz played at %lu: frame %lu differs (%f, %f)",
					rates[index][0], rates[index][1], n / 2, mono[n], stereo[n]);
		}
		free(pairs);
		free(signal);
		return 0;
	}
	/* a scene (reverb and limiter too) mixed in pieces of any size is the
	scene mixed in pieces of one: the gains of a 2D voice and of one that
	does not move do not ramp */
	CASE("chunks")
	{
		static float whole[30000 * 2], pieces[30000 * 2];
		unsigned long sizes[] = { 1, 7, 144, 333, 2, 1000, 3 }, size = 0, done = 0, count;
		/* the mixer's state, to start the second mix from */
		static struct { __typeof__(reverb) reverb; __typeof__(limiter) limiter; __typeof__(wii_mix) wii_mix;
			DSI3DL2LISTENER environment; unsigned long environment_serial; } saved;

		reverb_enabled = TRUE;
		saved.reverb = reverb;
		saved.limiter = limiter;
		saved.wii_mix = wii_mix;
		saved.environment = environment;
		saved.environment_serial = environment_serial;
		scene(6, 1, 1, 2, TRUE, TRUE, 40000);
		for (done = 0; done < 30000; done += count)
		{
			count = 30000 - done < 1000 ? 30000 - done : 1000;
			mix(whole + done * 2, count);
		}
		CHECK(!reverb.silent, "the reverb did not run");
		voices_clear();
		reverb = saved.reverb;
		limiter = saved.limiter;
		wii_mix = saved.wii_mix;
		environment = saved.environment;
		environment_serial = saved.environment_serial;
		scene(6, 1, 1, 2, TRUE, TRUE, 40000);
		for (done = 0; done < 30000; done += count)
		{
			count = sizes[size++ % 7];
			count = 30000 - done < count ? 30000 - done : count;
			mix(pieces + done * 2, count);
		}
		for (n = 0; n < 30000 * 2; n++)
			CHECK(fabsf(whole[n] - pieces[n]) < 1.0e-6f, "frame %lu differs (%f, %f)", n / 2, whole[n], pieces[n]);
		return 0;
	}
	/* the reverb (11,025 Hz, four lines): the decay time the room asks for, its
	level against the room's, the steady path against the crossfading one,
	a change of room, and nothing from nothing. "reverb-figures" prints them. */
	CASE("reverb")
	{
		static const float decays[] = { 0.6f, 1.49f, 3.0f };
		unsigned long index;

		for (index = 0; index < 3; index++)
		{
			float rt60, ratio;

			reverb_figures(decays[index], &rt60, &ratio);
			CHECK(fabsf(rt60 - decays[index]) < 0.15f * decays[index], "decay time %.2f s: the reverb decays in %.2f s",
				decays[index], rt60);
			/* (the room level is 0 mB, a gain of 1: the energy out is the energy in, less what the
			high frequencies lose; the eight lines at 22,050 Hz gave 0.7 to 0.9) */
			CHECK(ratio > 0.45f && ratio < 1.3f, "decay time %.2f s: the energy out is %.2f of the energy in",
				decays[index], ratio);
		}
		return 0;
	}
	CASE("reverb-figures")
	{
		static const float decays[] = { 0.3f, 0.6f, 1.0f, 1.49f, 3.0f, 8.0f };

		for (n = 0; n < 6; n++)
		{
			float rt60, ratio;

			reverb_figures(decays[n], &rt60, &ratio);
			printf("  decay %.2f s: decays in %.2f s, energy out / in %.2f\n", decays[n], rt60, ratio);
		}
		printf("  the reverb's state: %lu bytes\n", (unsigned long)sizeof(reverb));
		return 0;
	}
	CASE("reverb-steady")
	{
		/* the steady path is the crossfading one with nothing to crossfade */
		static float input[REVERB_BLOCK * 40], steady[REVERB_BLOCK * 40 * 2], general[REVERB_BLOCK * 40 * 2];
		static __typeof__(reverb) saved;
		unsigned long frame;
		float worst = 0.0f, loudest = 0.0f;

		reverb_room(1.49f, -1500, -1000);
		for (frame = 0; frame < sizeof(input) / sizeof(input[0]); frame++)
			input[frame] = (float)(random_number() - 0.5) * 0.5f;
		saved = reverb;
		reverb_run_blocks(input, steady, sizeof(input) / sizeof(input[0]), 1.0f, FALSE);
		reverb = saved;
		reverb_run_blocks(input, general, sizeof(input) / sizeof(input[0]), 1.0f, TRUE);
		for (frame = 0; frame < sizeof(steady) / sizeof(steady[0]); frame++)
		{
			worst = fmaxf(worst, fabsf(steady[frame] - general[frame]));
			loudest = fmaxf(loudest, fabsf(steady[frame]));
		}
		CHECK(loudest > 0.01f, "the reverb is silent (%g)", loudest);
		CHECK(worst < 1.0e-5f * loudest, "the steady path differs from the general one by %g of %g", worst, loudest);
		return 0;
	}
	CASE("reverb-change")
	{
		/* a new room is faded to, once; the old one's sound carries on meanwhile */
		static float input[REVERB_BLOCK * 80], output[REVERB_BLOCK * 80 * 2];
		DSI3DL2LISTENER other = { -1000, -100, 0.0f, 4.0f, 0.5f, -1500, 0.02f, -500, 0.04f, 80.0f, 60.0f, 3000.0f };
		unsigned long frame, fade_frames;
		float loudest = 0.0f;

		reverb_room(1.49f, -1500, -1000);
		for (frame = 0; frame < sizeof(input) / sizeof(input[0]); frame++)
			input[frame] = (float)(random_number() - 0.5) * 0.5f;
		reverb_run_blocks(input, output, sizeof(input) / sizeof(input[0]), 1.0f, FALSE);
		reverb_update(&other, ++reverb_serial);
		CHECK(reverb.fade == REVERB_FADE_FRAMES, "no fade to the new room (%lu)", reverb.fade);
		reverb_run_blocks(input, output, sizeof(input) / sizeof(input[0]), 1.0f, FALSE);
		fade_frames = 2 * REVERB_FADE_FRAMES; /* (low bus frames) */
		CHECK(fade_frames < sizeof(input) / sizeof(input[0]), "the test is too short");
		CHECK(reverb.fade == 0, "the fade to the new room did not end (%lu left)", reverb.fade);
		for (frame = 0; frame < sizeof(output) / sizeof(output[0]); frame++)
		{
			CHECK(isfinite(output[frame]), "frame %lu is not a number", frame);
			loudest = fmaxf(loudest, fabsf(output[frame]));
		}
		CHECK(loudest > 0.01f && loudest < 10.0f, "the reverb is %g loud", loudest);
		return 0;
	}
	/* a mono voice sending to the reverb (voice_apply's own loop for it), its direct path muffled or not, with
	every gain ramping, against the same sums in double precision */
	CASE("send-voice")
	{
		unsigned long frames = 300, kind, frame;

		for (kind = 0; kind < 2; kind++)
		{
			static struct sdl_stream stream;
			static float input[300], output[300 * 2], send[300];
			struct voice_ramp ramp = { 0.5f, 0.3f, 0.4f, kind ? 0.5f : 0.0f, 0.6f,
				0.001f, -0.0005f, -0.0007f, kind ? 0.0001f : 0.0f, 0.0002f, { 0.2f, 0.2f }, 0.1f };
			double left = ramp.left, right = ramp.right, room = ramp.room, direct = ramp.direct_lowpass;
			double room_lowpass = ramp.room_lowpass, direct_state = 0.2, room_state = 0.1, worst = 0.0;

			stream.channels = 1;
			for (frame = 0; frame < frames; frame++)
				input[frame] = (float)(random_number() - 0.5);
			memset(output, 0, sizeof(output));
			memset(send, 0, sizeof(send));
			voice_apply(&stream, input, input, output, send, frames, &ramp);
			for (frame = 0; frame < frames; frame++)
			{
				double sample = input[frame], muffled = sample, expected_send, error;

				room_state = sample + room_lowpass * (room_state - sample);
				expected_send = room_state * room;
				if (kind)
					muffled = sample + direct * (direct_state - sample);
				direct_state = muffled;
				error = fabs(send[frame] - expected_send);
				worst = error > worst ? error : worst;
				error = fabs(output[frame * 2] - muffled * left);
				worst = error > worst ? error : worst;
				error = fabs(output[frame * 2 + 1] - muffled * right);
				worst = error > worst ? error : worst;
				left += 0.001;
				right += -0.0005;
				room += -0.0007;
				direct += kind ? 0.0001 : 0.0;
				room_lowpass += 0.0002;
			}
			CHECK(worst < 1.0e-5, "%s voice sending to the reverb: %g off", kind ? "a muffled" : "an unmuffled", worst);
			CHECK(fabs(ramp.room_state - room_state) < 1.0e-5 && fabs(ramp.direct_state[0] - direct_state) < 1.0e-5 &&
				ramp.direct_state[0] == ramp.direct_state[1], "its states are not where they should be");
			CHECK(fabs(ramp.left - left) < 1.0e-4 && fabs(ramp.room - room) < 1.0e-4 &&
				fabs(ramp.room_lowpass - room_lowpass) < 1.0e-4 && fabs(ramp.direct_lowpass - direct) < 1.0e-4,
				"its gains did not ramp to where they should");
		}
		return 0;
	}
	CASE("mono-output")
	{
		/* the console's mono setting: both channels the mean of the mix's, never louder than the louder */
		static float stereo[1000 * 2], mixed[1000 * 2];
		unsigned long frame;

		for (frame = 0; frame < 1000; frame++)
		{
			stereo[frame * 2] = (float)sin(frame * 0.05) * 0.8f;
			stereo[frame * 2 + 1] = frame % 3 ? 0.0f : (float)cos(frame * 0.11) * 0.6f;
		}
		memcpy(mixed, stereo, sizeof(mixed));
		mix_mono(mixed, 1000);
		for (frame = 0; frame < 1000; frame++)
		{
			float mean = 0.5f * (stereo[frame * 2] + stereo[frame * 2 + 1]);
			float louder = fmaxf(fabsf(stereo[frame * 2]), fabsf(stereo[frame * 2 + 1]));

			CHECK(mixed[frame * 2] == mean && mixed[frame * 2 + 1] == mean, "frame %lu is not the mean (%f, %f; %f)",
				frame, mixed[frame * 2], mixed[frame * 2 + 1], mean);
			CHECK(fabsf(mixed[frame * 2]) <= louder, "frame %lu is louder than the louder channel", frame);
		}
		/* (a mix that is the same in both channels is unchanged) */
		for (frame = 0; frame < 1000; frame++)
			stereo[frame * 2 + 1] = stereo[frame * 2];
		memcpy(mixed, stereo, sizeof(mixed));
		mix_mono(mixed, 1000);
		CHECK(!memcmp(mixed, stereo, sizeof(mixed)), "a centred mix moved");
		return 0;
	}
	CASE("reverb-silent")
	{
		/* with nothing sent it makes nothing, and stops once it is quiet */
		static float input[REVERB_BLOCK * 8], output[REVERB_BLOCK * 8 * 2];
		unsigned long frame;

		reverb_room(1.49f, -1500, -1000);
		reverb_run_blocks(input, output, sizeof(input) / sizeof(input[0]), 1.0f, FALSE);
		for (frame = 0; frame < sizeof(output) / sizeof(output[0]); frame++)
			CHECK(output[frame] == 0.0f, "frame %lu is not silent (%g)", frame, output[frame]);
		CHECK(reverb.quiet >= sizeof(input) / sizeof(input[0]) / 2, "silence is not counted (%lu)", reverb.quiet);
		return 0;
	}
	/* a voice that runs out of packets stops, and starts over with the next */
	CASE("ends")
	{
		short *signal = noise(3000, 0.4);
		static float output[20000 * 2];
		struct sdl_stream *stream = voice_new(signal, 3000, 1, 22050, 23456);

		render(output, 10000);
		CHECK(stream->ended && stream->packets[0].finished, "the voice did not stop (%lu frames of silence)",
			stream->silence);
		for (n = 7000 * 2; n < 10000 * 2; n++)
			CHECK(output[n] == 0.0f, "the voice is heard at frame %lu after it ran out", n / 2);
		/* the next packet (the finished one not yet completed) */
		stream->packets[1].source = packet_pcm(signal, 3000);
		stream->packets[1].frames = 3000;
		stream->packet_count = 2;
		render(output, 1000);
		CHECK(!stream->ended && stream->cursor > 0, "the voice did not start over");
		free((void *)stream->packets[1].source);
		voices_clear();
		free(signal);
		return 0;
	}
	/* an ADPCM voice at a bus's rate is its blocks as decoded, mono and
	stereo, read a block at a time from where the packet keeps them */
	CASE("adpcm")
	{
		unsigned long channels;

		for (channels = 1; channels <= 2; channels++)
		{
			unsigned char *bytes = adpcm_noise(100, channels);
			static short decoded[100 * XBOX_ADPCM_BLOCK_SAMPLES * 2];
			static float output[100 * XBOX_ADPCM_BLOCK_SAMPLES * 2];
			struct sdl_stream *stream;
			unsigned long block, channel;

			for (block = 0; block < 100; block++)
			{
				decode_adpcm_block(bytes + block * XBOX_ADPCM_BLOCK_BYTES * channels, channels,
					decoded + block * XBOX_ADPCM_BLOCK_SAMPLES * channels);
				/* the decoder's tables are ima_expand nibble by nibble */
				for (channel = 0; channel < channels; channel++)
				{
					const unsigned char *data = bytes + block * XBOX_ADPCM_BLOCK_BYTES * channels;
					const short *samples = decoded + block * XBOX_ADPCM_BLOCK_SAMPLES * channels;
					int predictor = (short)(data[channel * 4] | data[channel * 4 + 1] << 8);
					int index = data[channel * 4 + 2], sample;

					CHECK(samples[channel] == predictor, "block %lu: the header's sample", block);
					for (sample = 1; sample < XBOX_ADPCM_BLOCK_SAMPLES; sample++)
					{
						int nibble = sample - 1, group = nibble / 8, within = nibble % 8;
						int byte = data[4 * channels + ((unsigned long)group * channels + channel) * 4 + within / 2];
						int expected = ima_expand(within % 2 ? byte >> 4 : byte & 0xf, &predictor, &index);

						CHECK(samples[sample * channels + channel] == expected,
							"block %lu channel %lu sample %d: %d, not %d", block, channel, sample,
							samples[sample * channels + channel], expected);
					}
				}
			}
			stream = voice_from(bytes, TRUE, 100 * XBOX_ADPCM_BLOCK_SAMPLES, channels, 44100, 0);
			/* (under the limiter's ceiling) */
			stream->volume = 0.25f;
			render(output, 100 * XBOX_ADPCM_BLOCK_SAMPLES);
			for (n = 0; n < 100 * XBOX_ADPCM_BLOCK_SAMPLES; n++)
			{
				for (channel = 0; channel < 2; channel++)
				{
					float expected = decoded[n * channels + (channels == 2 ? channel : 0)] * (1.0f / 32768.0f) * 0.25f;

					CHECK(output[n * 2 + channel] == expected, "%lu channels: frame %lu is %f, not %f", channels, n,
						output[n * 2 + channel], expected);
				}
			}
			voices_clear();
		}
		return 0;
	}
	/* the figures: sines through voices at each rate and frequency */
	CASE("metrics")
	{
		static const unsigned long rates[][2] = { { 22050, 0 }, { 22050, 21000 }, { 22050, 23456 },
			{ 22050, 26000 }, { 22050, 30000 }, { 22050, 44100 }, { 44100, 0 }, { 44100, 40000 }, { 44100, 48000 } };
		static const double tones[] = { 100, 1000, 4000, 8000, 9500, 10500, 15000, 20000 };
		unsigned long index, tone;

		for (index = 0; index < sizeof(rates) / sizeof(rates[0]); index++)
		{
			unsigned long rate = rates[index][0], frequency = rates[index][1] ? rates[index][1] : rate;

			printf("%lu Hz played at %lu:\n", rate, frequency);
			for (tone = 0; tone < sizeof(tones) / sizeof(tones[0]); tone++)
			{
				double gain_db, rest_db;

				if (tones[tone] >= 0.5 * rate || tones[tone] * frequency / rate >= 0.5 * OUTPUT_RATE)
					continue;
				sine_figures(tones[tone], rate, rates[index][1], &gain_db, &rest_db);
				printf("  %6.0f Hz: gain %+.3f dB, the rest %.1f dB\n", tones[tone], gain_db, rest_db);
			}
		}
		return 0;
	}
	/* a scene in the Wii's pieces: bench mono stereo44 stereo22 silent flags
	chunks, flags 1 indoors (the reverb), 2 ADPCM, 4 the reverb off, 8 in mono */
	CASE("bench")
	{
		static float output[CHUNK * 2];
		unsigned long chunks = strtoul(argv[7], NULL, 0), chunk;
		int flags = atoi(argv[6]);
		double energy = 0.0;

		reverb_enabled = (flags & 4) == 0;
		scene(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0),
			strtoul(argv[5], NULL, 0), (flags & 1) != 0, (flags & 2) != 0, 120000);
		for (chunk = 0; chunk < chunks; chunk++)
		{
			mix(output, CHUNK);
			if (flags & 8)
				mix_mono(output, CHUNK);
			for (n = 0; n < CHUNK * 2; n++)
				energy += output[n] * output[n];
		}
		printf("frames %lu energy %f\n", chunks * CHUNK, energy);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
