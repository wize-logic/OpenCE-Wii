/*
DSOUND_SDL.C

Xbox DirectSound for the Linux build: a software mixer on an SDL3 audio
stream.

The game plays everything through DirectSound streams: 16-bit stereo PCM
(music and other uncompressed sounds) and Xbox ADPCM, mono or stereo, at 22
or 44 kHz. A packet is read where the game keeps it (the sound cache, which
does not reuse its memory until the packet completes), as it plays: an ADPCM
packet a block of 64 frames at a time, into the voice's own block, so that
no voice holds more than that decoded (decoding each packet whole held a
megabyte or two of the Wii's heap with a level's sounds playing). The mixer
runs on SDL's audio thread; for every voice it resamples to the output rate
with a windowed sinc low pass (which is how SetFrequency changes pitch;
resampling) and applies:
	- the stream volume (millibels),
	- the front left and right mix bin volumes of 2D voices,
	- for 3D voices, DirectSound's inverse distance rolloff between the
	  minimum and maximum distance, an equal power pan from the source's
	  direction in listener space, and the I3DL2 direct path, obstruction
	  and occlusion levels, their high frequency levels a low pass at the
	  environment's HF reference: a sound behind a wall is muffled.
3D voices also send to an I3DL2 reverb of the environment the listener is in
(reverb). Doppler and cones are not modelled. A look-ahead limiter keeps the
sum under full scale (limit). The Wii mixes its voices at their own rates
instead of resampling each, on two buses (the Wii's mixing).

Packets the mixer has finished are completed from DirectSoundDoWork, which
the game calls every frame, and from Flush, never from the audio thread:
the game's completion callback is not meant to run concurrently with it.

Without an audio device, a clock thread runs the same mixer into a scratch
buffer, so streams still drain at their real rate.

audio.volume sets the master volume (default 1.0); audio.reverb = false
turns the reverb off; audio.enabled = false skips opening a device
(port_config.c).
*/

#include "platform.h"
#ifndef HALO_WII
#include "sdl_platform.h"
#endif
#include "port_config.h"
#include "voice_audio.h"

#ifdef HALO_WII
/* the Wii plays through WiiLibs' audio output (wl/platform: AESND) */
#include "wl/platform/platform.h"
#define PLATFORM_AUDIO_DEVICE_NAME_SIZE 128
#else
#include <SDL3/SDL.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HALO_WII
/* (twice the low bus's rate, at which the game's 44 kHz voices play as they
are: the Wii's mixing; its DSP plays it at 48 kHz) */
#define OUTPUT_RATE 44100
#define LOW_RATE 22050
#else
#define OUTPUT_RATE 48000
#endif
#define OUTPUT_CHANNELS 2
#define MAXIMUM_STREAM_PACKETS 64
#define MIX_CHUNK_FRAMES 1024

#define XBOX_ADPCM_BLOCK_BYTES 36
#define XBOX_ADPCM_BLOCK_SAMPLES 64

/* the resampler (resampling) */
#define RESAMPLER_ZERO_CROSSINGS 24
#define RESAMPLER_TABLE_STEPS 256
#define RESAMPLER_CUTOFF 0.96
#define RESAMPLER_KAISER_BETA 6.5
#define RESAMPLER_MAXIMUM_STRETCH 2
#define RESAMPLER_HISTORY 128
#if defined(__GNUC__) || defined(__clang__)
#define RESAMPLER_ALIGNED __attribute__((aligned(16)))
#else
#define RESAMPLER_ALIGNED
#endif

/* ---------- voices */

struct voice_packet
{
	XMEDIAPACKET packet;
	/* its bytes, the game's until it completes: little-endian 16-bit PCM or
	Xbox ADPCM, interleaved, the stream's channel count */
	const unsigned char *source;
	unsigned long frames;
	BOOL finished;            /* played out by the mixer, not yet completed */
};

struct sdl_stream
{
	/* must be first: in C an IDirectSoundStream is just { lpVtbl } */
	IDirectSoundStream object;
	struct sdl_stream *next;
	ULONG reference_count;
	LPFNXMEDIAOBJECTCALLBACK callback;
	LPVOID context;

	/* format */
	BOOL adpcm;
	unsigned long channels;
	DWORD sample_rate;
	DWORD frequency;

	BOOL paused;

	/* 2D gains */
	float volume;             /* SetVolume */
	float mix_left, mix_right;
	/* a stereo voice of a sound in the world: panned towards it, -1 left
	to 1 right, its distance, and the fade of its volume with distance that
	the game made (dsound_sdl_stream_set_stereo_position) */
	BOOL stereo_positioned;
	float stereo_pan;
	float stereo_distance;
	float stereo_distance_fade;
	float headroom;

	/* 3D */
	BOOL has_3d;
	DWORD mode;
	float position[3];
	float minimum_distance, maximum_distance;
	/* the I3DL2 direct path's level and high frequency level, in millibels
	with obstruction and occlusion (SetI3DL2Source) */
	LONG direct, direct_hf;
	/* the I3DL2 room (reverb) send's level and high frequency level, in
	millibels with occlusion, and its rolloff factor (SetI3DL2Source) */
	LONG room, room_hf;
	float room_rolloff_factor;

	struct voice_packet packets[MAXIMUM_STREAM_PACKETS];
	unsigned long packet_head;
	unsigned long packet_count;
	/* the next frame to take from the first packet not finished */
	unsigned long cursor;
	/* the ADPCM block the cursor is in, decoded (take_frame, voice_run), and the bytes
	it was decoded from (NULL: none) */
	short block[XBOX_ADPCM_BLOCK_SAMPLES * 2];
	const unsigned char *block_source;
#ifdef HALO_WII
	/* the Wii's mixing (resampler_reset): the bus the voice plays on, its
	last four frames (each channel's, the newest last), the time of the
	output past the second of them in frames, how many frames of silence
	were taken since the packets ran out, and whether it has stopped; the
	gains it ramps to (voice_gains) and the voice_parameters_serial they
	were worked out at */
	BOOL high_bus;
	float window[2][4];
	double phase;
	unsigned long silence;
	BOOL ended;
	float target_left, target_right, target_room;
	float target_direct_lowpass, target_room_lowpass;
	unsigned long gains_serial;
#else
	/* the resampler's (resampler_reset): the last RESAMPLER_HISTORY frames
	taken, each channel's apart and each frame twice, RESAMPLER_HISTORY apart,
	so that the frames a low pass reaches are side by side however the ring
	wraps (resampler_dot); how many were taken, the frame the output is at and
	how far past it, and the frames of silence taken since the packets ran
	out */
	float history[2][2 * RESAMPLER_HISTORY];
	unsigned long history_count;
	unsigned long center;
	double phase;
	unsigned long silence;
#endif
	/* gains and low pass coefficients the mixer is ramping from, to avoid
	clicks */
	float current_left, current_right, current_room;
	float current_direct_lowpass, current_room_lowpass;
	BOOL gains_valid;
	/* the direct path's low pass, for each channel, and the room send's */
	float direct_lowpass[2];
	float room_lowpass;
};

static pthread_mutex_t mixer_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sdl_stream *streams;

/* the listener, in DirectSound's left-handed +y up space */
static struct
{
	float position[3];
	float front[3];
	float top[3];
	float rolloff_factor;
	/* meters a unit (SetDistanceFactor): the game sets 3.048, a world unit
	being 10 feet. Only Doppler, which is not modelled, would use it */
	float distance_factor;
} listener = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0f, 1.0f };

static float master_volume = 1.0f;

/* counts the changes to what a voice's gains come from (voice_gains): its
own settings, the listener's, the environment's, the master volume and
audio.reverb. The Wii's mixing works a voice's gains out again only when it
has moved on */
static unsigned long voice_parameters_serial;

/* the I3DL2 listener properties (SetI3DL2Listener), which the reverb models:
DirectSound's defaults until the game sets an environment, their room level
leaving it silent; serial counts the changes */
static DSI3DL2LISTENER environment =
{
	DSBVOLUME_MIN, 0, 0.0f, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 100.0f, 100.0f, 5000.0f
};
static unsigned long environment_serial;
/* audio.reverb */
static BOOL reverb_enabled = TRUE;

/* the rate the reverb runs at: the Wii's, its low bus's (the Wii's mixing) */
#ifdef HALO_WII
#define REVERB_RATE LOW_RATE
#else
#define REVERB_RATE OUTPUT_RATE
#endif

static float gain_from_millibels(LONG millibels)
{
	if (millibels <= DSBVOLUME_MIN)
		return 0.0f;
	return powf(10.0f, (float)millibels / 2000.0f);
}

/* value, or 0 once it is far too small to hear: the state of a filter or of
the reverb fed silence decays into the denormals, which many CPUs work out
slowly, and neither x86 nor ARM flushes them to zero by default */
static float flush_denormal(float value)
{
	return fabsf(value) < 1.0e-20f ? 0.0f : value;
}

/* cos(2 pi f / rate) of a filter's frequency f, at the rate it runs at (the
output's, or the Wii's buses'), within what that rate holds */
static float frequency_cosine(float frequency, float rate)
{
	if (frequency < 20.0f)
		frequency = 20.0f;
	if (frequency > 0.45f * rate)
		frequency = 0.45f * rate;
	return cosf(2.0f * 3.14159265f * frequency / rate);
}

/* the coefficient a of the one-pole low pass y = x + a (y' - x) whose gain is
gain (at most 1) at the frequency of cosine (frequency_cosine) */
static float lowpass_coefficient(float gain, float cosine)
{
	float a, b;

	if (gain >= 1.0f)
		return 0.0f;
	if (gain < 0.001f)
		gain = 0.001f;
	a = 1.0f - gain * gain;
	b = 1.0f - gain * gain * cosine;
	return (b - sqrtf(b * b - a * a)) / a;
}

/* ---------- decoding */

static const int ima_index_table[16] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static const int ima_step_table[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
	253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
	11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767,
};

static int ima_expand(int nibble, int *predictor, int *index)
{
	int step = ima_step_table[*index];
	int difference = step >> 3;

	if (nibble & 1) difference += step >> 2;
	if (nibble & 2) difference += step >> 1;
	if (nibble & 4) difference += step;
	if (nibble & 8) difference = -difference;
	*predictor += difference;
	if (*predictor > 32767) *predictor = 32767;
	if (*predictor < -32768) *predictor = -32768;
	*index += ima_index_table[nibble];
	if (*index < 0) *index = 0;
	if (*index > 88) *index = 88;
	return *predictor;
}

/* Xbox ADPCM: per block, a 4-byte header per channel (first sample, step
index), then 4-byte groups of eight nibbles, low nibble first, alternating
between channels; 64 samples per channel. The header's sample is the block's
first, and the nibbles code the 63 after it: the 64th nibble only pads the
block (the maps' sounds always have 0 there), and decoding it in place of the
header's sample put a wrong sample in every 64, a buzz at 344 Hz in 22 kHz
sounds. */
/* ima_expand worked out ahead: the difference a nibble's low three bits make
at each step index, and the step index after each nibble. A sample costs a
table look-up, an add and a clamp, a third of what working each out did (the
Wii decodes every sound as it plays) */
static unsigned short ima_differences[89][8];
static unsigned char ima_next_index[89][16];
static BOOL ima_tables_ready;

static void ima_tables_initialize(void)
{
	int index, nibble;

	for (index = 0; index <= 88; index++)
	{
		int step = ima_step_table[index];

		for (nibble = 0; nibble < 16; nibble++)
		{
			int predictor = 0, next = index;

			ima_expand(nibble, &predictor, &next);
			ima_next_index[index][nibble] = (unsigned char)next;
		}
		for (nibble = 0; nibble < 8; nibble++)
		{
			ima_differences[index][nibble] = (unsigned short)((step >> 3) + (nibble & 1 ? step >> 2 : 0) +
				(nibble & 2 ? step >> 1 : 0) + (nibble & 4 ? step : 0));
		}
	}
	ima_tables_ready = TRUE;
}

/* the sample after predictor that nibble codes, at the step index (which it
moves on), as ima_expand */
#define IMA_EXPAND(nibble, predictor, index) \
	do \
	{ \
		int difference_ = ima_differences[index][(nibble) & 7]; \
		int sign_ = -((nibble) >> 3); \
		\
		predictor += (difference_ ^ sign_) - sign_; \
		if (predictor > 32767) \
			predictor = 32767; \
		if (predictor < -32768) \
			predictor = -32768; \
		index = ima_next_index[index][nibble]; \
	} while (0)

static void decode_adpcm_block(const unsigned char *data, unsigned long channels, short *output)
{
	unsigned long channel;

	if (!ima_tables_ready)
		ima_tables_initialize();
	for (channel = 0; channel < channels; channel++)
	{
		const unsigned char *header = data + channel * 4;
		int predictor = (short)(header[0] | (header[1] << 8));
		int index = header[2] > 88 ? 88 : header[2];
		short *sample = output + channel;
		unsigned long group, byte;

		*sample = (short)predictor;
		sample += channels;
		for (group = 0; group < 8; group++)
		{
			const unsigned char *nibbles = data + 4 * channels + (group * channels + channel) * 4;

			/* nibble n codes sample n + 1 */
			for (byte = 0; byte < 4; byte++)
			{
				int low = nibbles[byte] & 0xf, high = nibbles[byte] >> 4;

				IMA_EXPAND(low, predictor, index);
				*sample = (short)predictor;
				sample += channels;
				if (group == 7 && byte == 3)
					break;
				IMA_EXPAND(high, predictor, index);
				*sample = (short)predictor;
				sample += channels;
			}
		}
	}
}

/* the frames in size bytes: whole ADPCM blocks, or 16-bit PCM */
static unsigned long packet_frames(BOOL adpcm, unsigned long size, unsigned long channels)
{
	if (adpcm)
		return size / (XBOX_ADPCM_BLOCK_BYTES * channels) * XBOX_ADPCM_BLOCK_SAMPLES;
	return size / (2 * channels);
}

/* ---------- 3D */

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* DirectSound's inverse distance law, held beyond the maximum distance. The
game sets a voice's maximum distance before its minimum, so the maximum can
be under the minimum for a moment: it is held at the minimum */
static float distance_attenuation(const struct sdl_stream *stream, float distance, float rolloff_factor)
{
	float minimum = stream->minimum_distance;
	float maximum = stream->maximum_distance > minimum ? stream->maximum_distance : minimum;

	if (minimum <= 0.0f || distance <= minimum)
		return 1.0f;
	if (distance > maximum)
		distance = maximum;
	return minimum / (minimum + rolloff_factor * (distance - minimum));
}

/* the gains of a 3D voice to the left and right, and the room rolloff's
attenuation of its room send */
static void spatialize(const struct sdl_stream *stream, float *left, float *right, float *room)
{
	float offset[3], right_axis[3], distance, attenuation, pan, side, ahead;
	int axis;

	if (stream->mode == DS3DMODE_HEADRELATIVE)
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis];
		side = offset[0];
		ahead = offset[2];
	}
	else
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = stream->position[axis] - listener.position[axis];
		/* left-handed: right = top x front */
		right_axis[0] = listener.top[1] * listener.front[2] - listener.top[2] * listener.front[1];
		right_axis[1] = listener.top[2] * listener.front[0] - listener.top[0] * listener.front[2];
		right_axis[2] = listener.top[0] * listener.front[1] - listener.top[1] * listener.front[0];
		side = dot3(offset, right_axis);
		ahead = dot3(offset, listener.front);
	}
	/* in the game's units, those of the minimum and maximum distances: the
	distance factor turns units into meters for Doppler, and scaling by it
	here put every 3D sound 3 times as far away, up to 10 dB quieter */
	distance = sqrtf(dot3(offset, offset));
	attenuation = distance_attenuation(stream, distance, listener.rolloff_factor);
	*room = distance_attenuation(stream, distance, environment.flRoomRolloffFactor + stream->room_rolloff_factor);

	/* equal power pan; close sources and sources straight ahead or behind
	stay centred, and neither ear drops below a quarter */
	{
		float horizontal = sqrtf(side * side + ahead * ahead);
		float angle;

		pan = horizontal > 1.0e-4f ? side / horizontal : 0.0f;
		if (distance < stream->minimum_distance && stream->minimum_distance > 0.0f)
			pan *= distance / stream->minimum_distance;
		pan *= 0.75f;
		angle = (pan + 1.0f) * 0.25f * 3.14159265f;
		*left = cosf(angle) * 1.41421356f * 0.70710678f;
		*right = sinf(angle) * 1.41421356f * 0.70710678f;
	}
	*left *= attenuation * gain_from_millibels(stream->direct);
	*right *= attenuation * gain_from_millibels(stream->direct);
}

/* the voice's gains to the left and right, and to the reverb (only 3D voices
send to it, as their I3DL2 mix bin did on the Xbox), with the coefficients of
the low passes of its direct path (3D voices: obstruction and occlusion
muffle it) and of the send, which run at rate */
static void voice_gains(const struct sdl_stream *stream, float rate, float *left, float *right, float *room,
	float *direct_lowpass, float *room_lowpass)
{
	*room = 0.0f;
	*direct_lowpass = 0.0f;
	*room_lowpass = 0.0f;
	if (stream->has_3d && stream->mode != DS3DMODE_DISABLE)
	{
		float rolloff, cosine = frequency_cosine(environment.flHFReference, rate);

		spatialize(stream, left, right, &rolloff);
		if (stream->direct_hf < stream->direct)
			*direct_lowpass = lowpass_coefficient(gain_from_millibels(stream->direct_hf - stream->direct), cosine);
		if (reverb_enabled)
		{
			LONG level = environment.lRoom + stream->room;
			LONG high_level = environment.lRoom + environment.lRoomHF + stream->room_hf;

			*room = gain_from_millibels(level) * rolloff;
			if (*room > 0.0f && high_level < level)
				*room_lowpass = lowpass_coefficient(gain_from_millibels(high_level - level), cosine);
		}
	}
	else if (stream->channels == 2 && stream->stereo_positioned)
	{
		/* the equal power pan of a 3D voice, at the gains of a 2D one when
		centred (the game fades it with distance), its direct path muffled
		and its room send made as a 3D voice's are (SetI3DL2Source) */
		float angle = (stream->stereo_pan + 1.0f) * 0.25f * 3.14159265f;
		float direct = gain_from_millibels(stream->direct);
		float cosine = frequency_cosine(environment.flHFReference, rate);

		*left = cosf(angle) * 1.41421356f * stream->mix_left * direct;
		*right = sinf(angle) * 1.41421356f * stream->mix_right * direct;
		if (stream->direct_hf < stream->direct)
			*direct_lowpass = lowpass_coefficient(gain_from_millibels(stream->direct_hf - stream->direct), cosine);
		if (reverb_enabled)
		{
			LONG level = environment.lRoom + stream->room;
			LONG high_level = environment.lRoom + environment.lRoomHF + stream->room_hf;

			/* the room's rolloff with distance, a 3D voice's; the volume
			holds the game's fade with it, which a 3D voice's room send does
			not take, so it is taken back out (no further than a twentieth:
			past that the send fades out with the sound) */
			*room = gain_from_millibels(level) *
				distance_attenuation(stream, stream->stereo_distance,
					environment.flRoomRolloffFactor + stream->room_rolloff_factor) /
				(stream->stereo_distance_fade > 0.05f ? stream->stereo_distance_fade : 0.05f);
			if (*room > 0.0f && high_level < level)
				*room_lowpass = lowpass_coefficient(gain_from_millibels(high_level - level), cosine);
		}
	}
	else
	{
		*left = stream->mix_left;
		*right = stream->mix_right;
	}
	*left *= stream->volume * master_volume;
	*right *= stream->volume * master_volume;
	*room *= stream->volume * master_volume;
}

/* the modified Bessel function I0, of the Kaiser windows of the low passes
(resampling, and the Wii's upsampler) */
static double bessel_i0(double x)
{
	double sum = 1.0, term = 1.0;
	int k;

	for (k = 1; k < 64 && term > 1.0e-12 * sum; k++)
	{
		term *= (x / (2.0 * k)) * (x / (2.0 * k));
		sum += term;
	}
	return sum;
}

#ifndef HALO_WII
/* ---------- resampling

Each voice is resampled to the output rate by band-limited interpolation (J.
O. Smith's): an output sample is the source frames around its moment, each
weighted by a windowed sinc low pass centred there. The low pass keeps
RESAMPLER_CUTOFF of the source's band (a 22 kHz voice is 0.5 dB down at 10
kHz) and takes the images of it out (80 dB down), but for those of its last
few hundred hertz, which lie beside them, about 20 dB down. Linear
interpolation, which the mixer did before, left the images only 8 to 20 dB
down, a gritty haze above 11 kHz over every 22 kHz voice. A voice
played faster than the output rate takes its frames (a step over 1) gets the
low pass narrowed to match, up to RESAMPLER_MAXIMUM_STRETCH times, so it
does not alias. The frames come from the voice's packets in turn, so the low
pass reads straight across a packet's end into the next. */

/* the low pass's one side, RESAMPLER_TABLE_STEPS values a source frame */
static float resampler_table[RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS + 2];
/* the same low pass as the weights of a voice's taps 1 - RESAMPLER_ZERO_CROSSINGS
to RESAMPLER_ZERO_CROSSINGS, at each of RESAMPLER_TABLE_STEPS phases between two
source frames (and one row more, for the last's blend): a voice at the output
rate or slower blends two rows by its phase, which is what the table gave tap
by tap, a third of the work. resampler_deltas holds each row's step to the
next, so the blend is a multiply and an add a tap */
#define RESAMPLER_TAPS (2 * RESAMPLER_ZERO_CROSSINGS)
static float resampler_phases[RESAMPLER_TABLE_STEPS + 1][RESAMPLER_TAPS] RESAMPLER_ALIGNED;
static float resampler_deltas[RESAMPLER_TABLE_STEPS][RESAMPLER_TAPS] RESAMPLER_ALIGNED;

static void resampler_initialize(void)
{
	unsigned long index;

	for (index = 0; index <= RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS; index++)
	{
		double distance = (double)index / RESAMPLER_TABLE_STEPS;
		double edge = distance / RESAMPLER_ZERO_CROSSINGS;
		double angle = 3.14159265358979 * RESAMPLER_CUTOFF * distance;
		double sinc = index ? sin(angle) / angle : 1.0;
		double window = bessel_i0(RESAMPLER_KAISER_BETA * sqrt(1.0 - edge * edge)) / bessel_i0(RESAMPLER_KAISER_BETA);

		resampler_table[index] = (float)(RESAMPLER_CUTOFF * sinc * window);
	}
	resampler_table[RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS + 1] = 0.0f;
}

/* the low pass, distance source frames from its centre times
RESAMPLER_TABLE_STEPS */
static float resampler_weight(float distance)
{
	unsigned long index = (unsigned long)distance;
	float fraction;

	if (index >= RESAMPLER_ZERO_CROSSINGS * RESAMPLER_TABLE_STEPS)
		return 0.0f;
	fraction = distance - (float)index;
	return resampler_table[index] + (resampler_table[index + 1] - resampler_table[index]) * fraction;
}

static void resampler_phases_initialize(void)
{
	unsigned long phase, tap;

	for (phase = 0; phase <= RESAMPLER_TABLE_STEPS; phase++)
	{
		for (tap = 0; tap < 2 * RESAMPLER_ZERO_CROSSINGS; tap++)
		{
			float distance = fabsf((float)((long)tap + 1 - RESAMPLER_ZERO_CROSSINGS) -
				(float)phase / RESAMPLER_TABLE_STEPS);

			resampler_phases[phase][tap] = resampler_weight(distance * RESAMPLER_TABLE_STEPS);
		}
	}
	for (phase = 0; phase < RESAMPLER_TABLE_STEPS; phase++)
	{
		for (tap = 0; tap < RESAMPLER_TAPS; tap++)
			resampler_deltas[phase][tap] = resampler_phases[phase + 1][tap] - resampler_phases[phase][tap];
	}
}

/* The low pass over a voice's frames, the sum of each frame times its weight:
four taps at a time where the compiler has vectors (SSE, NEON), summed in four
lanes, then the lanes together. Only the order of the additions differs from
tap by tap, a difference of the order of float's rounding (-115 dB or less);
RESAMPLER_SCALAR adds them tap by tap (tools/harness/tests/test_mixer.py). */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(RESAMPLER_SCALAR)
typedef float resampler_vector __attribute__((vector_size(16)));
typedef float resampler_unaligned_vector __attribute__((vector_size(16), aligned(4)));
/* (each row of the phase tables whole vectors, aligned) */
typedef char resampler_taps_in_fours[RESAMPLER_TAPS % 4 ? -1 : 1];
#define RESAMPLER_LANES 4
#else
#define RESAMPLER_LANES 1
#endif

/* frames[0, RESAMPLER_TAPS) weighted by the phase table's row blended blend of
the way to the next */
static float resampler_dot(const float *frames, const float *weights, const float *deltas, float blend)
{
#if RESAMPLER_LANES == 4
	resampler_vector sum = { 0.0f, 0.0f, 0.0f, 0.0f }, blends = { blend, blend, blend, blend };
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap += 4)
	{
		resampler_vector weight = *(const resampler_vector *)(weights + tap) +
			*(const resampler_vector *)(deltas + tap) * blends;

		sum += *(const resampler_unaligned_vector *)(frames + tap) * weight;
	}
	return (sum[0] + sum[1]) + (sum[2] + sum[3]);
#else
	float sum = 0.0f;
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap++)
		sum += frames[tap] * (weights[tap] + deltas[tap] * blend);
	return sum;
#endif
}

/* resampler_dot of two channels' frames, with the same weights */
static void resampler_dot2(const float *left_frames, const float *right_frames, const float *weights,
	const float *deltas, float blend, float *left, float *right)
{
#if RESAMPLER_LANES == 4
	resampler_vector left_sum = { 0.0f, 0.0f, 0.0f, 0.0f }, right_sum = left_sum;
	resampler_vector blends = { blend, blend, blend, blend };
	int tap;

	for (tap = 0; tap < RESAMPLER_TAPS; tap += 4)
	{
		resampler_vector weight = *(const resampler_vector *)(weights + tap) +
			*(const resampler_vector *)(deltas + tap) * blends;

		left_sum += *(const resampler_unaligned_vector *)(left_frames + tap) * weight;
		right_sum += *(const resampler_unaligned_vector *)(right_frames + tap) * weight;
	}
	*left = (left_sum[0] + left_sum[1]) + (left_sum[2] + left_sum[3]);
	*right = (right_sum[0] + right_sum[1]) + (right_sum[2] + right_sum[3]);
#else
	*left = resampler_dot(left_frames, weights, deltas, blend);
	*right = resampler_dot(right_frames, weights, deltas, blend);
#endif
}

/* a voice starting (over): silence before its first frame, which the output
starts at */
static void resampler_reset(struct sdl_stream *stream)
{
	memset(stream->history, 0, sizeof(stream->history));
	stream->history_count = RESAMPLER_ZERO_CROSSINGS * RESAMPLER_MAXIMUM_STRETCH;
	stream->center = stream->history_count;
	stream->phase = 0.0;
	stream->silence = 0;
}

/* the packet's frame, its first and last channel (the same for a mono
voice): ADPCM from the voice's decoded block, which is the frame's block's
once this has run; PCM little-endian, as the maps keep it, on any host */
static void packet_frame(struct sdl_stream *stream, const struct voice_packet *packet, unsigned long frame,
	float *taken)
{
	unsigned long channels = stream->channels;

	if (stream->adpcm)
	{
		const unsigned char *block = packet->source +
			frame / XBOX_ADPCM_BLOCK_SAMPLES * XBOX_ADPCM_BLOCK_BYTES * channels;
		const short *samples = stream->block + frame % XBOX_ADPCM_BLOCK_SAMPLES * channels;

		if (stream->block_source != block)
		{
			decode_adpcm_block(block, channels, stream->block);
			stream->block_source = block;
		}
		taken[0] = samples[0] * (1.0f / 32768.0f);
		taken[1] = samples[channels - 1] * (1.0f / 32768.0f);
	}
	else
	{
		const unsigned char *first = packet->source + frame * channels * 2;
		const unsigned char *last = first + (channels - 1) * 2;

		taken[0] = (short)(first[0] | first[1] << 8) * (1.0f / 32768.0f);
		taken[1] = (short)(last[0] | last[1] << 8) * (1.0f / 32768.0f);
	}
}

/* the voice's next frame, finishing the packets it passes; FALSE once they
run out */
static BOOL take_frame(struct sdl_stream *stream, float *frame)
{
	for (;;)
	{
		struct voice_packet *packet = NULL;
		unsigned long position;

		for (position = 0; position < stream->packet_count; position++)
		{
			struct voice_packet *candidate = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

			if (!candidate->finished)
			{
				packet = candidate;
				break;
			}
		}
		if (!packet)
			return FALSE;
		if (stream->cursor < packet->frames)
		{
			packet_frame(stream, packet, stream->cursor, frame);
			stream->cursor++;
			return TRUE;
		}
		stream->cursor = 0;
		packet->finished = TRUE;
	}
}

/* whether the voice has a frame to take; the packets with none left are
marked finished on the way, as take_frame marks them */
static BOOL voice_has_frames(struct sdl_stream *stream)
{
	unsigned long position;

	for (position = 0; position < stream->packet_count; position++)
	{
		struct voice_packet *packet = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

		if (packet->finished)
			continue;
		if (stream->cursor < packet->frames)
			return TRUE;
		stream->cursor = 0;
		packet->finished = TRUE;
	}
	return FALSE;
}

/* ---------- mixing */

/* mixes one voice into output (frames of stereo float), and into the reverb's
send (frames of mono) */
static void mix_voice(struct sdl_stream *stream, float *output, float *send, unsigned long frames)
{
	double step;
	float target_left, target_right, target_room, left, right, room, ramp_left, ramp_right, ramp_room, scale;
	float target_direct_lowpass, target_room_lowpass, direct_lowpass, room_lowpass;
	float ramp_direct_lowpass, ramp_room_lowpass;
	float direct_state[2], room_state;
	long width;
	unsigned long frame;
	/* (a stereo voice panned towards its sound: voice_gains) */
	BOOL positioned = stream->channels == 2 && stream->stereo_positioned;

	if (stream->paused || !stream->packet_count || !stream->sample_rate)
		return;
	step = (double)(stream->frequency ? stream->frequency : stream->sample_rate) / OUTPUT_RATE;
	/* the low pass narrowed for a voice taking frames faster than the output
	rate, and the frames it reaches on each side */
	scale = step > 1.0 ? (float)(1.0 / (step < RESAMPLER_MAXIMUM_STRETCH ? step : RESAMPLER_MAXIMUM_STRETCH)) : 1.0f;
	width = (long)ceilf(RESAMPLER_ZERO_CROSSINGS / scale);
	/* a voice that ran dry and stopped (below) starts over once it has
	frames again: stream_process starts over only a stream with no packets,
	and the next can come before the finished ones are completed */
	if (stream->silence > (unsigned long)(2 * width))
	{
		if (!voice_has_frames(stream))
			return;
		resampler_reset(stream);
		stream->gains_valid = FALSE;
	}
	voice_gains(stream, OUTPUT_RATE, &target_left, &target_right, &target_room, &target_direct_lowpass, &target_room_lowpass);
	if (!stream->gains_valid)
	{
		stream->current_left = target_left;
		stream->current_right = target_right;
		stream->current_room = target_room;
		stream->current_direct_lowpass = target_direct_lowpass;
		stream->current_room_lowpass = target_room_lowpass;
		stream->direct_lowpass[0] = stream->direct_lowpass[1] = 0.0f;
		stream->room_lowpass = 0.0f;
		stream->gains_valid = TRUE;
	}
	left = stream->current_left;
	right = stream->current_right;
	room = stream->current_room;
	direct_lowpass = stream->current_direct_lowpass;
	room_lowpass = stream->current_room_lowpass;
	/* the low passes' states, here while the mix writes the output (which
	the compiler cannot tell from them) */
	direct_state[0] = stream->direct_lowpass[0];
	direct_state[1] = stream->direct_lowpass[1];
	room_state = stream->room_lowpass;
	ramp_left = (target_left - left) / (float)frames;
	ramp_right = (target_right - right) / (float)frames;
	ramp_room = (target_room - room) / (float)frames;
	ramp_direct_lowpass = (target_direct_lowpass - direct_lowpass) / (float)frames;
	ramp_room_lowpass = (target_room_lowpass - room_lowpass) / (float)frames;

	for (frame = 0; frame < frames; frame++)
	{
		float sample_left = 0.0f, sample_right = 0.0f;
		long tap;

		/* the frames ahead the low pass reaches; after the last packet,
		silence, until what was taken has played out */
		while ((long)(stream->history_count - stream->center) <= width)
		{
			unsigned long slot = stream->history_count % RESAMPLER_HISTORY;
			float taken[2];

			if (take_frame(stream, taken))
			{
				stream->silence = 0;
			}
			else
			{
				taken[0] = taken[1] = 0.0f;
				stream->silence++;
			}
			stream->history[0][slot] = stream->history[0][slot + RESAMPLER_HISTORY] = taken[0];
			stream->history[1][slot] = stream->history[1][slot + RESAMPLER_HISTORY] = taken[1];
			stream->history_count++;
		}
		if (stream->silence > (unsigned long)(2 * width))
			break;

		if (!left && !right && !room && !ramp_left && !ramp_right && !ramp_room)
		{
			/* a voice turned all the way down (out of earshot), and not
			sending to the reverb, only moves on */
		}
		else if (scale == 1.0f)
		{
			double position = stream->phase * RESAMPLER_TABLE_STEPS;
			unsigned long row = (unsigned long)position;
			float blend = (float)(position - (double)row);
			unsigned long first = (stream->center + 1 - RESAMPLER_ZERO_CROSSINGS) % RESAMPLER_HISTORY;

			/* (a mono voice's two channels are the same: take_frame) */
			if (stream->channels == 1)
			{
				sample_left = sample_right = resampler_dot(stream->history[0] + first, resampler_phases[row],
					resampler_deltas[row], blend);
			}
			else
			{
				resampler_dot2(stream->history[0] + first, stream->history[1] + first, resampler_phases[row],
					resampler_deltas[row], blend, &sample_left, &sample_right);
			}
		}
		else
		{
			unsigned long first = (stream->center + 1 - (unsigned long)width) % RESAMPLER_HISTORY;
			const float *left_frames = stream->history[0] + first, *right_frames = stream->history[1] + first;

			for (tap = 1 - width; tap <= width; tap++)
			{
				float weight = scale * resampler_weight(fabsf((float)tap - (float)stream->phase) * scale * RESAMPLER_TABLE_STEPS);

				sample_left += left_frames[tap + width - 1] * weight;
				if (stream->channels == 2)
					sample_right += right_frames[tap + width - 1] * weight;
			}
			if (stream->channels == 1)
				sample_right = sample_left;
		}
		/* the room send, with its own low pass */
		if (room || ramp_room)
		{
			float mono = stream->channels == 1 ? sample_left : 0.5f * (sample_left + sample_right);

			room_state = flush_denormal(mono + room_lowpass * (room_state - mono));
			send[frame] += room_state * room;
		}
		/* the direct path, muffled, or as it is; a mono voice's mix bins or pan
		split it across the speakers */
		if (direct_lowpass || ramp_direct_lowpass)
		{
			sample_left = flush_denormal(sample_left + direct_lowpass * (direct_state[0] - sample_left));
			sample_right = flush_denormal(sample_right + direct_lowpass * (direct_state[1] - sample_right));
		}
		direct_state[0] = sample_left;
		direct_state[1] = sample_right;
		if (stream->channels == 1)
		{
			output[frame * 2] += sample_left * left;
			output[frame * 2 + 1] += sample_left * right;
		}
		else if (positioned)
		{
			/* the channels' middle panned, and their difference kept as
			wide as the far ear's gain: the sound comes from where it is,
			still stereo */
			float middle = 0.5f * (sample_left + sample_right);
			float side = 0.5f * (sample_left - sample_right);
			float far_gain = left < right ? left : right;

			output[frame * 2] += middle * left + side * far_gain;
			output[frame * 2 + 1] += middle * right - side * far_gain;
		}
		else
		{
			output[frame * 2] += sample_left * left;
			output[frame * 2 + 1] += sample_right * right;
		}
		left += ramp_left;
		right += ramp_right;
		room += ramp_room;
		direct_lowpass += ramp_direct_lowpass;
		room_lowpass += ramp_room_lowpass;
		stream->phase += step;
		while (stream->phase >= 1.0)
		{
			stream->phase -= 1.0;
			stream->center++;
		}
	}
	stream->direct_lowpass[0] = direct_state[0];
	stream->direct_lowpass[1] = direct_state[1];
	stream->room_lowpass = room_state;
	stream->current_left = target_left;
	stream->current_right = target_right;
	stream->current_room = target_room;
	stream->current_direct_lowpass = target_direct_lowpass;
	stream->current_room_lowpass = target_room_lowpass;
}
#endif

/* ---------- reverb

The Xbox ran an I3DL2 reverb on its audio DSP (the effects image the game
downloads), fed by every 3D voice's I3DL2 mix bin. The game sets the listener
properties from the sound environment the camera is in (sound_dsound_xbox.c
dsound_set_listener_properties), and each 3D voice's room send from its
reverb attenuation and occlusion (dsound_channel_set_I3DL2_properties). This
is a reverb of the I3DL2 model on those properties, not the DSP's program:
	- each 3D voice sends to a mono bus at the listener's and its own room
	  levels, occlusion included, their high frequency levels a low pass at the
	  HF reference, attenuated with distance by the room rolloff factors
	  (voice_gains);
	- the early reflections are REVERB_TAPS taps of the bus, starting the
	  reflections delay later, at the reflections level;
	- the late reverberation starts the reverb delay after the reflections:
	  all-pass diffusers as strong as the diffusion, then a feedback delay
	  network of REVERB_LINES lines, as long as the density makes them, each
	  losing what decays it by 60 dB in the decay time, and its high
	  frequencies in the decay time times the HF ratio, at the reverb level.
Both are normalized to the energy sent, so their levels are relative to the
room level, as I3DL2 gives them. A change of environment crossfades each
delay's read from its old length to its new one, and each gain, over
REVERB_FADE_FRAMES: what is reverberating carries on, without the click of a
jump or the pitch shift of a sliding delay. audio.reverb = false fades the
reverb out over as long and stops it. Once nothing has been sent for
REVERB_QUIET_FRAMES and the reverberation is 120 dB down, the reverb stops
until something is sent again (outdoors, in the menus). */

#define REVERB_DELAY_SIZE 32768 /* the reflections and reverb delays, 0.3 + 0.1 s at most, and the taps */
#define REVERB_TAPS 6
#define REVERB_DIFFUSERS 4
#define REVERB_DIFFUSER_SIZE 1024
#define REVERB_LINES 8
#define REVERB_LINE_SIZE 4096
#define REVERB_FADE_FRAMES 4096 /* 85 ms at 48 kHz (the Wii's reverb runs at 22 kHz: 186 ms) */
/* the input delays, then the diffusers, played out */
#define REVERB_QUIET_FRAMES (REVERB_DELAY_SIZE + REVERB_DIFFUSERS * REVERB_DIFFUSER_SIZE)
#define REVERB_QUIET_PEAK 1.0e-6f

/* in milliseconds: the taps after the reflections delay, the diffusers, and
the lines at full density */
static const float reverb_tap_times[REVERB_TAPS] = { 0.0f, 3.1f, 5.3f, 7.9f, 11.2f, 14.9f };
static const float reverb_tap_signs[REVERB_TAPS] = { 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
static const float reverb_diffuser_times[REVERB_DIFFUSERS] = { 4.7f, 3.6f, 12.7f, 9.3f };
static const float reverb_line_times[REVERB_LINES] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.1f };
/* which lines go in negated, and the two outputs' signs of the lines */
static const float reverb_input_signs[REVERB_LINES] = { 1, -1, -1, 1, 1, -1, -1, 1 };
static const float reverb_left_signs[REVERB_LINES] = { 1, -1, 1, -1, 1, -1, 1, -1 };
static const float reverb_right_signs[REVERB_LINES] = { 1, 1, -1, -1, 1, 1, -1, -1 };

/* what an environment sets: delays in samples, and gains */
struct reverb_parameters
{
	unsigned long taps[REVERB_TAPS];
	unsigned long late_delay;
	unsigned long lengths[REVERB_LINES];
	/* each line's gain a pass, and its low pass's coefficient */
	float feedback[REVERB_LINES];
	float damping[REVERB_LINES];
	float reflections_gain, late_gain;
	float diffusion;
};

static struct
{
	/* the bus of the current mix */
	float send[MIX_CHUNK_FRAMES];
	/* the environment serial and properties of the parameters faded to */
	unsigned long serial;
	DSI3DL2LISTENER properties;
	/* the parameters fading from and to, and the frames of the fade left
	(none: from is to) */
	struct reverb_parameters from, to;
	unsigned long fade;
	/* the output's level, 1 while audio.reverb is on, moving to 0 when it
	is turned off; whether the buffers are silent; and the frames since the
	send or the output was last over REVERB_QUIET_PEAK */
	float level;
	BOOL silent;
	unsigned long quiet;

	/* the bus, delayed for the taps and the late reverberation */
	float delay[REVERB_DELAY_SIZE];
	unsigned long delay_position;

	float diffusers[REVERB_DIFFUSERS][REVERB_DIFFUSER_SIZE];
	unsigned long diffuser_lengths[REVERB_DIFFUSERS];
	unsigned long diffuser_position;

	float lines[REVERB_LINES][REVERB_LINE_SIZE];
	unsigned long line_position;
	/* each line's low pass */
	float lowpass[REVERB_LINES];
} reverb;

static float clamp_real(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

/* whole samples */
static unsigned long reverb_samples(float seconds)
{
	return (unsigned long)floorf(seconds * REVERB_RATE + 0.5f);
}

/* the sample buffer got delay samples before position, crossfaded from the
from delay to the to delay by fade (0 to 1) */
static float reverb_read(const float *buffer, unsigned long mask, unsigned long position,
	unsigned long from_delay, unsigned long to_delay, float fade)
{
	float from_sample = buffer[(position - from_delay) & mask];

	if (from_delay == to_delay)
		return from_sample;
	return from_sample + (buffer[(position - to_delay) & mask] - from_sample) * fade;
}

static float reverb_blend(float from, float to, float fade)
{
	return from + (to - from) * fade;
}

static void reverb_parameters_set(struct reverb_parameters *parameters, const DSI3DL2LISTENER *properties)
{
	float decay = clamp_real(properties->flDecayTime, 0.1f, 20.0f);
	/* a low pass loses high frequencies; a ratio over 1 would need them to
	last longer */
	float high_decay = decay * clamp_real(properties->flDecayHFRatio, 0.1f, 1.0f);
	float scale = 0.25f + 0.75f * clamp_real(properties->flDensity / 100.0f, 0.0f, 1.0f);
	float cosine = frequency_cosine(properties->flHFReference, REVERB_RATE);
	float mean_length = 0.0f, mean_feedback;
	unsigned long reflections_delay;
	int index;

	for (index = 0; index < REVERB_LINES; index++)
	{
		float length = floorf(reverb_line_times[index] * 0.001f * REVERB_RATE * scale + 0.5f);
		float high_feedback;

		parameters->lengths[index] = (unsigned long)length;
		parameters->feedback[index] = powf(10.0f, -3.0f * length / (decay * REVERB_RATE));
		high_feedback = powf(10.0f, -3.0f * length / (high_decay * REVERB_RATE));
		parameters->damping[index] = lowpass_coefficient(high_feedback / parameters->feedback[index], cosine);
		mean_length += length / REVERB_LINES;
	}
	mean_feedback = powf(10.0f, -3.0f * mean_length / (decay * REVERB_RATE));
	reflections_delay = reverb_samples(clamp_real(properties->flReflectionsDelay, 0.0f, 0.3f));
	for (index = 0; index < REVERB_TAPS; index++)
		parameters->taps[index] = reflections_delay + reverb_samples(reverb_tap_times[index] * 0.001f);
	parameters->late_delay = reflections_delay + reverb_samples(clamp_real(properties->flReverbDelay, 0.0f, 0.1f));
	/* the taps' energy, half of them to each side; the network's, whose
	sound circulates until it decays */
	parameters->reflections_gain = gain_from_millibels(properties->lReflections) /
		sqrtf((float)REVERB_TAPS / OUTPUT_CHANNELS);
	parameters->late_gain = gain_from_millibels(properties->lReverb) * sqrtf(1.0f - mean_feedback * mean_feedback);
	parameters->diffusion = 0.7f * clamp_real(properties->flDiffusion / 100.0f, 0.0f, 1.0f);
}

/* a new environment, faded to unless the reverb is silent. The game sets the
same one again several times a second in some places (Derelict), which
changes nothing */
static void reverb_update(const DSI3DL2LISTENER *properties, unsigned long serial)
{
	reverb.serial = serial;
	if (!reverb.silent && !memcmp(properties, &reverb.properties, sizeof(*properties)))
		return;
	reverb.properties = *properties;
	reverb_parameters_set(&reverb.to, properties);
	if (reverb.silent)
		reverb.from = reverb.to;
	reverb.fade = reverb.silent ? 0 : REVERB_FADE_FRAMES;
}

static void reverb_clear(void)
{
	memset(reverb.delay, 0, sizeof(reverb.delay));
	memset(reverb.diffusers, 0, sizeof(reverb.diffusers));
	memset(reverb.lines, 0, sizeof(reverb.lines));
	memset(reverb.lowpass, 0, sizeof(reverb.lowpass));
	reverb.from = reverb.to;
	reverb.fade = 0;
	reverb.silent = TRUE;
	reverb.quiet = 0;
}

static void reverb_initialize(void)
{
	int index;

	for (index = 0; index < REVERB_DIFFUSERS; index++)
		reverb.diffuser_lengths[index] = reverb_samples(reverb_diffuser_times[index] * 0.001f);
	reverb.silent = TRUE;
	reverb.level = reverb_enabled ? 1.0f : 0.0f;
	reverb_update(&environment, environment_serial);
}

/* adds the reverberation of send to output, its level moving to
target_level */
static void reverb_process(const float *send, float *output, unsigned long frames, float target_level)
{
	const struct reverb_parameters *from = &reverb.from, *to = &reverb.to;
	float input_scale = 1.0f / sqrtf((float)REVERB_LINES), peak = 0.0f;
	unsigned long frame;
	int index;

	reverb.silent = FALSE;
	for (frame = 0; frame < frames; frame++)
	{
		/* how far the fade from the old environment to the new is */
		float fade = 1.0f - (float)reverb.fade / REVERB_FADE_FRAMES;
		float early[OUTPUT_CHANNELS] = { 0.0f, 0.0f };
		float outputs[REVERB_LINES], late_left = 0.0f, late_right = 0.0f, feedback_sum = 0.0f, value;
		float diffusion = reverb_blend(from->diffusion, to->diffusion, fade);

		reverb.delay[reverb.delay_position & (REVERB_DELAY_SIZE - 1)] = send[frame];
		for (index = 0; index < REVERB_TAPS; index++)
		{
			early[index % OUTPUT_CHANNELS] += reverb_tap_signs[index] * reverb_read(reverb.delay, REVERB_DELAY_SIZE - 1,
				reverb.delay_position, from->taps[index], to->taps[index], fade);
		}

		/* diffusers: w = x + g w', y = w' - g w */
		value = reverb_read(reverb.delay, REVERB_DELAY_SIZE - 1, reverb.delay_position,
			from->late_delay, to->late_delay, fade);
		reverb.delay_position++;
		for (index = 0; index < REVERB_DIFFUSERS; index++)
		{
			float *buffer = reverb.diffusers[index];
			float delayed = buffer[(reverb.diffuser_position - reverb.diffuser_lengths[index]) & (REVERB_DIFFUSER_SIZE - 1)];
			float written = flush_denormal(value + diffusion * delayed);

			buffer[reverb.diffuser_position & (REVERB_DIFFUSER_SIZE - 1)] = written;
			value = delayed - diffusion * written;
		}
		reverb.diffuser_position++;

		/* the network: each line's output is damped and heard, then decayed
		and mixed back into every line by a Householder reflection */
		for (index = 0; index < REVERB_LINES; index++)
		{
			float line_output = reverb_read(reverb.lines[index], REVERB_LINE_SIZE - 1, reverb.line_position,
				from->lengths[index], to->lengths[index], fade);
			float damping = reverb_blend(from->damping[index], to->damping[index], fade);

			reverb.lowpass[index] = flush_denormal(line_output + damping * (reverb.lowpass[index] - line_output));
			late_left += reverb_left_signs[index] * reverb.lowpass[index];
			late_right += reverb_right_signs[index] * reverb.lowpass[index];
			outputs[index] = reverb.lowpass[index] * reverb_blend(from->feedback[index], to->feedback[index], fade);
			feedback_sum += outputs[index];
		}
		feedback_sum *= 2.0f / REVERB_LINES;
		for (index = 0; index < REVERB_LINES; index++)
		{
			reverb.lines[index][reverb.line_position & (REVERB_LINE_SIZE - 1)] =
				flush_denormal(outputs[index] - feedback_sum + value * reverb_input_signs[index] * input_scale);
		}
		reverb.line_position++;

		{
			float reflections_gain = reverb_blend(from->reflections_gain, to->reflections_gain, fade);
			float late_gain = reverb_blend(from->late_gain, to->late_gain, fade);
			float left = early[0] * reflections_gain + late_left * late_gain;
			float right = early[1] * reflections_gain + late_right * late_gain;

			output[frame * 2] += left * reverb.level;
			output[frame * 2 + 1] += right * reverb.level;
			peak = fabsf(left) > peak ? fabsf(left) : peak;
			peak = fabsf(right) > peak ? fabsf(right) : peak;
			peak = fabsf(send[frame]) > peak ? fabsf(send[frame]) : peak;
		}
		if (reverb.fade && !--reverb.fade)
			reverb.from = reverb.to;
		reverb.level = clamp_real(target_level, reverb.level - 1.0f / REVERB_FADE_FRAMES,
			reverb.level + 1.0f / REVERB_FADE_FRAMES);
	}
	reverb.quiet = peak > REVERB_QUIET_PEAK ? 0 : reverb.quiet + frames;
}

/* whether anything is sent to the reverb in send */
static BOOL reverb_sent(const float *send, unsigned long frames)
{
	unsigned long frame;

	for (frame = 0; frame < frames; frame++)
	{
		if (fabsf(send[frame]) > REVERB_QUIET_PEAK)
			return TRUE;
	}
	return FALSE;
}

/* ---------- limiter

The game sets its mix bins' headroom to 0 (sound_dsound_xbox.c), so the voices
sum at their full level, as on the Xbox, and a pile of loud ones goes over
full scale. Clipping each sample, or bending it near full scale, distorts the
sound: dialogue over gunfire crackled. Instead the whole mix is turned down
for as long as it would go over, both channels alike. The output is delayed
LIMITER_LOOKAHEAD - 1 frames (1.3 ms), so that the gain comes down smoothly
before each peak: the smallest gain the frames ahead need, averaged over the
last LIMITER_LOOKAHEAD frames, is never more than a peak needs when it plays.
The gain comes back up over LIMITER_RELEASE_SECONDS. */

#define LIMITER_CEILING 0.891f /* -1 dBFS */
#define LIMITER_LOOKAHEAD 64
#define LIMITER_RELEASE_SECONDS 0.1f

static struct
{
	/* the frames the output is delayed by, and the gain each needs */
	float delay[LIMITER_LOOKAHEAD][OUTPUT_CHANNELS];
	float needed[LIMITER_LOOKAHEAD];
	/* the smallest of them, looked for again only when the frame that needed
	it has played (looking every frame cost the Wii's mixer more than all
	its voices while gunfire was limited) */
	float lowest;
	/* the gain held down to what the frames ahead need, coming back up */
	float held;
	/* its last LIMITER_LOOKAHEAD values, their sum, and how many are under 1
	(none: the sum is LIMITER_LOOKAHEAD, whatever its additions rounded) */
	float history[LIMITER_LOOKAHEAD];
	double history_sum;
	unsigned long history_below;
	unsigned long position;
	BOOL initialized;
} limiter;

static void limit(float *output, unsigned long frames)
{
	float release = 1.0f - expf(-1.0f / (LIMITER_RELEASE_SECONDS * OUTPUT_RATE));
	unsigned long frame, index, position = limiter.position, history_below;
	float loudest = 0.0f, lowest, held;
	double history_sum;

	if (!limiter.initialized)
	{
		for (index = 0; index < LIMITER_LOOKAHEAD; index++)
		{
			limiter.needed[index] = 1.0f;
			limiter.history[index] = 1.0f;
		}
		limiter.lowest = 1.0f;
		limiter.held = 1.0f;
		limiter.history_sum = LIMITER_LOOKAHEAD;
		limiter.history_below = 0;
		limiter.initialized = TRUE;
	}
	/* nothing limited, and nothing here to limit: the frames only delayed,
	as below with every gain 1 */
	for (frame = 0; frame < frames * OUTPUT_CHANNELS; frame++)
		loudest = fabsf(output[frame]) > loudest ? fabsf(output[frame]) : loudest;
	if (loudest <= LIMITER_CEILING && limiter.lowest == 1.0f && limiter.held == 1.0f && !limiter.history_below)
	{
		for (frame = 0; frame < frames; frame++)
		{
			float *sample = output + frame * OUTPUT_CHANNELS, left = sample[0], right = sample[1];
			unsigned long oldest = (position + 1) % LIMITER_LOOKAHEAD;

			sample[0] = limiter.delay[oldest][0];
			sample[1] = limiter.delay[oldest][1];
			limiter.delay[position][0] = left;
			limiter.delay[position][1] = right;
			position = oldest;
		}
		limiter.position = position;
		return;
	}
	/* (the limiter's state here while the frames are written, which the
	compiler cannot tell from it) */
	lowest = limiter.lowest;
	held = limiter.held;
	history_sum = limiter.history_sum;
	history_below = limiter.history_below;
	for (frame = 0; frame < frames; frame++)
	{
		float *sample = output + frame * OUTPUT_CHANNELS, left = sample[0], right = sample[1];
		unsigned long oldest = (position + 1) % LIMITER_LOOKAHEAD;
		float peak = fabsf(left) > fabsf(right) ? fabsf(left) : fabsf(right), played, needed, gain;

		/* the new frame takes the slot of the oldest, which has played */
		limiter.delay[position][0] = left;
		limiter.delay[position][1] = right;
		played = limiter.needed[position];
		needed = peak > LIMITER_CEILING ? LIMITER_CEILING / peak : 1.0f;
		limiter.needed[position] = needed;
		if (needed <= lowest)
		{
			lowest = needed;
		}
		else if (played == lowest)
		{
			lowest = limiter.needed[0];
			for (index = 1; index < LIMITER_LOOKAHEAD; index++)
			{
				if (limiter.needed[index] < lowest)
					lowest = limiter.needed[index];
			}
		}
		if (lowest < held)
			held = lowest;
		else
			held += (lowest - held) * release;
		history_sum += held - limiter.history[position];
		history_below += (held < 1.0f) - (limiter.history[position] < 1.0f);
		limiter.history[position] = held;
		if (!history_below)
			history_sum = LIMITER_LOOKAHEAD;
		gain = (float)(history_sum / LIMITER_LOOKAHEAD);
		/* the frame LIMITER_LOOKAHEAD - 1 frames old plays */
		sample[0] = limiter.delay[oldest][0] * gain;
		sample[1] = limiter.delay[oldest][1] * gain;
		position = oldest;
	}
	limiter.lowest = lowest;
	limiter.held = held;
	limiter.history_sum = history_sum;
	limiter.history_below = history_below;
	limiter.position = position;
}

#ifdef HALO_WII
/* ---------- the Wii's mixing

Resampling every voice by its low pass took two thirds of the Wii's processor
in b30 (a 22 kHz voice cost 530 instructions an output frame at 32 kHz, a 44
kHz one, its low pass stretched, 4000). The Wii mixes each voice at the rate
of one of two buses instead, the low one at 22050 Hz and the high one at the
output's 44100 Hz, as the game's voices play at their own rates unless their
pitch is changed: the game's 22 kHz voices go to the low bus, its 44 kHz ones
to the high one, each frame as it is. A voice played at another frequency
(SetFrequency: a sound's random pitch, an engine) is interpolated onto the
high bus by a cubic (Catmull-Rom) through four of its frames. The low bus is
then doubled to the output's rate by a half-band low pass (the upsampler),
the one windowed sinc left, run once for all its voices: it keeps a 22 kHz
voice flat to 9.5 kHz, and takes the images of it out from 12.5 kHz, 70 dB
down. The voices' gains, pans, low passes and room sends are the other
platforms', at their bus's rate, worked out again only when what they come
from changes; the reverb runs at the low bus's rate, and the limiter on the
output. A voice out of earshot only moves on, not decoded. A scene of 16
voices heard and 8 not, counted under qemu-ppc, cost 910 instructions an
output frame where it cost 15,100 (port/wii/README.md, "Sound";
tools/harness/tests/test_mixer_wii.py) */

/* the upsampler's taps on each side of the moment it interpolates */
#define UPSAMPLER_PAIRS 16
#define UPSAMPLER_KAISER_BETA 7.0
/* the frames of the low bus the upsampler reads before a mix's */
#define LOW_HISTORY (2 * UPSAMPLER_PAIRS)
/* the output frames mixed at a time (an even number: mix) */
#define MIX_AHEAD_FRAMES 288
#define LOW_CHUNK_FRAMES (MIX_AHEAD_FRAMES / 2)
/* the frames of a voice's window: one before the moment played, three from it */
#define WINDOW_FRAMES 4

/* the upsampler's weights of the low bus's frames k + 1/2 from the moment
between two of them */
static float upsampler_weights[UPSAMPLER_PAIRS];

static struct
{
	/* the low bus, stereo: LOW_HISTORY frames of the mixes before, then
	this one's */
	float frames[(LOW_HISTORY + LOW_CHUNK_FRAMES) * OUTPUT_CHANNELS];
	/* the high bus's send to the reverb (the low bus's is the reverb's), and
	whether any voice sent to each in this mix */
	float high_send[MIX_CHUNK_FRAMES];
	BOOL low_sent, high_sent;
	/* whether anything was mixed into the low bus in this mix, and for how
	many of its frames before nothing has been */
	BOOL low_written;
	unsigned long low_quiet;
	/* the output mixed ahead of what was asked for (mix): how much, and how
	much of it has been given */
	float ahead[MIX_AHEAD_FRAMES * OUTPUT_CHANNELS];
	unsigned long ahead_count, ahead_used;
} wii_mix;

/* the half-band low pass: a sinc whose zeros fall on the low bus's frames,
so that every other output frame is the bus's own, Kaiser windowed; the
weights sum to 1 */
static void upsampler_initialize(void)
{
	double weights[UPSAMPLER_PAIRS], sum = 0.0;
	int k;

	for (k = 0; k < UPSAMPLER_PAIRS; k++)
	{
		double distance = k + 0.5, edge = distance / UPSAMPLER_PAIRS, angle = 3.14159265358979 * distance;

		weights[k] = sin(angle) / angle * bessel_i0(UPSAMPLER_KAISER_BETA * sqrt(1.0 - edge * edge)) /
			bessel_i0(UPSAMPLER_KAISER_BETA);
		sum += 2.0 * weights[k];
	}
	for (k = 0; k < UPSAMPLER_PAIRS; k++)
		upsampler_weights[k] = (float)(weights[k] / sum);
}

/* a voice starting (over): silence before its first frame, three of which
are taken before its first output frame (the window's second, at phase 0) */
static void resampler_reset(struct sdl_stream *stream)
{
	memset(stream->window, 0, sizeof(stream->window));
	stream->phase = WINDOW_FRAMES - 1;
	stream->silence = 0;
	stream->ended = FALSE;
}

/* the frames in a row at the voice's cursor in the first packet not
finished: how many, and where, in the voice's ADPCM block (decoded first if
decode) or the packet's little-endian PCM; 0 once the packets run out. The
packets with none left are marked finished on the way */
static unsigned long voice_run(struct sdl_stream *stream, BOOL decode, const short **block,
	const unsigned char **pcm)
{
	unsigned long position, channels = stream->channels;

	for (position = 0; position < stream->packet_count; position++)
	{
		struct voice_packet *packet = &stream->packets[(stream->packet_head + position) % MAXIMUM_STREAM_PACKETS];

		if (packet->finished)
			continue;
		if (stream->cursor < packet->frames)
		{
			if (stream->adpcm)
			{
				unsigned long offset = stream->cursor % XBOX_ADPCM_BLOCK_SAMPLES;
				const unsigned char *source = packet->source +
					stream->cursor / XBOX_ADPCM_BLOCK_SAMPLES * XBOX_ADPCM_BLOCK_BYTES * channels;

				if (decode && stream->block_source != source)
				{
					decode_adpcm_block(source, channels, stream->block);
					stream->block_source = source;
				}
				*block = stream->block + offset * channels;
				*pcm = NULL;
				return XBOX_ADPCM_BLOCK_SAMPLES - offset;
			}
			*block = NULL;
			*pcm = packet->source + stream->cursor * channels * 2;
			return packet->frames - stream->cursor;
		}
		stream->cursor = 0;
		packet->finished = TRUE;
	}
	return 0;
}

/* whether the voice has a frame to take */
static BOOL voice_has_frames(struct sdl_stream *stream)
{
	const short *block;
	const unsigned char *pcm;

	return voice_run(stream, FALSE, &block, &pcm) != 0;
}

/* passes over count of the voice's frames, a run at a time, not decoded:
the frames mix_voice_wii would take while the voice is out of earshot. Its
window falls silent; it comes back into earshot from a gain of 0 (its gains
ramp from those of the mix before), so the frames of silence its window
holds then are not heard */
static void voice_pass(struct sdl_stream *stream, unsigned long count)
{
	memset(stream->window, 0, sizeof(stream->window));
	while (count)
	{
		const short *block;
		const unsigned char *pcm;
		unsigned long run = voice_run(stream, FALSE, &block, &pcm);

		if (!run)
		{
			stream->silence += count;
			break;
		}
		if (run > count)
			run = count;
		stream->cursor += run;
		stream->silence = 0;
		count -= run;
	}
}

/* the bus frames a voice is mixed in at a time, and the most frames it takes
for each (the frequency it plays at, at most this many times its bus's) */
#define VOICE_PIECE_FRAMES 128
#define VOICE_MAXIMUM_STEP 8

/* a piece of a voice (mix_voice_wii): its window, then the frames it takes,
each channel's; and what it plays, where it is not those frames as they are */
static float voice_taken[2][WINDOW_FRAMES + VOICE_PIECE_FRAMES * VOICE_MAXIMUM_STEP + 2];
/* (voice_take: 1.5 * 2^23 and its bits; a 16-bit sample added to them is
the float 1.5 * 2^23 plus the sample, exactly) */
#define VOICE_SAMPLE_BIAS 12582912.0f
#define VOICE_SAMPLE_BIAS_BITS 0x4B400000U
static float voice_played[2][VOICE_PIECE_FRAMES];

/* the voice's gains to its bus, ramped across a mix from those of the last */
struct voice_ramp
{
	float left, right, room, direct_lowpass, room_lowpass;
	float ramp_left, ramp_right, ramp_room, ramp_direct_lowpass, ramp_room_lowpass;
	float direct_state[2], room_state;
};

/* takes count of the voice's frames into voice_taken after its window, and
moves its window to the last four */
static void voice_take(struct sdl_stream *stream, unsigned long count)
{
	unsigned long channels = stream->channels, taken = 0, frame;
	float *left = voice_taken[0], *right = voice_taken[channels == 2];
	int index;

	for (index = 0; index < WINDOW_FRAMES; index++)
	{
		left[index] = stream->window[0][index];
		right[index] = stream->window[1][index];
	}
	left += WINDOW_FRAMES;
	right += WINDOW_FRAMES;
	while (taken < count)
	{
		const short *block;
		const unsigned char *pcm;
		unsigned long run = voice_run(stream, TRUE, &block, &pcm);
		float *left_run = left + taken, *right_run = right + taken;

		if (!run)
		{
			/* after the last packet, silence */
			for (frame = taken; frame < count; frame++)
				left[frame] = right[frame] = 0.0f;
			stream->silence += count - taken;
			break;
		}
		if (run > count - taken)
			run = count - taken;
		/* the samples made floats without the processor's conversion (the
		750 has none from an integer register; the compiler's goes through
		memory with a double for each): written as the bits of the float
		1.5 * 2^23 plus the sample, then that float taken away */
		if (block && channels == 2)
		{
			for (frame = 0; frame < run; frame++)
			{
				((unsigned int *)left_run)[frame] = VOICE_SAMPLE_BIAS_BITS + block[frame * 2];
				((unsigned int *)right_run)[frame] = VOICE_SAMPLE_BIAS_BITS + block[frame * 2 + 1];
			}
		}
		else if (block)
		{
			for (frame = 0; frame < run; frame++)
				((unsigned int *)left_run)[frame] = VOICE_SAMPLE_BIAS_BITS + block[frame];
		}
		else if (channels == 2)
		{
			for (frame = 0; frame < run; frame++)
			{
				((unsigned int *)left_run)[frame] = VOICE_SAMPLE_BIAS_BITS + (short)(pcm[frame * 4] | pcm[frame * 4 + 1] << 8);
				((unsigned int *)right_run)[frame] = VOICE_SAMPLE_BIAS_BITS + (short)(pcm[frame * 4 + 2] | pcm[frame * 4 + 3] << 8);
			}
		}
		else
		{
			for (frame = 0; frame < run; frame++)
				((unsigned int *)left_run)[frame] = VOICE_SAMPLE_BIAS_BITS + (short)(pcm[frame * 2] | pcm[frame * 2 + 1] << 8);
		}
		for (frame = 0; frame < run; frame++)
			left_run[frame] = (left_run[frame] - VOICE_SAMPLE_BIAS) * (1.0f / 32768.0f);
		if (channels == 2)
		{
			for (frame = 0; frame < run; frame++)
				right_run[frame] = (right_run[frame] - VOICE_SAMPLE_BIAS) * (1.0f / 32768.0f);
		}
		stream->cursor += run;
		stream->silence = 0;
		taken += run;
	}
	for (index = 0; index < WINDOW_FRAMES; index++)
	{
		stream->window[0][index] = left[(long)count - WINDOW_FRAMES + index];
		stream->window[1][index] = right[(long)count - WINDOW_FRAMES + index];
	}
}

/* mixes frames of a voice's channels into output (and its send) with its
gains: the direct path, muffled, or as it is, a mono voice's mix bins or pan
splitting it across the speakers, and the room send with its own low pass */
static void voice_apply(const struct sdl_stream *stream, const float *left_frames, const float *right_frames,
	float *output, float *send, unsigned long frames, struct voice_ramp *ramp)
{
	BOOL stereo = stream->channels == 2;
	/* (a stereo voice panned towards its sound: voice_gains) */
	BOOL positioned = stereo && stream->stereo_positioned;
	BOOL filtered = ramp->direct_lowpass || ramp->ramp_direct_lowpass;
	BOOL sent = ramp->room || ramp->ramp_room;
	float left = ramp->left, right = ramp->right, ramp_left = ramp->ramp_left, ramp_right = ramp->ramp_right;
	unsigned long frame;

	if (!filtered && !sent && !positioned)
	{
		/* (most voices: their frames into the bus at their gains) */
		if (stereo)
		{
			for (frame = 0; frame < frames; frame++)
			{
				output[frame * 2] += left_frames[frame] * left;
				output[frame * 2 + 1] += right_frames[frame] * right;
				left += ramp_left;
				right += ramp_right;
			}
		}
		else
		{
			for (frame = 0; frame < frames; frame++)
			{
				output[frame * 2] += left_frames[frame] * left;
				output[frame * 2 + 1] += left_frames[frame] * right;
				left += ramp_left;
				right += ramp_right;
			}
		}
		ramp->direct_state[0] = left_frames[frames - 1];
		ramp->direct_state[1] = right_frames[frames - 1];
		ramp->room += ramp->ramp_room * frames;
		ramp->direct_lowpass += ramp->ramp_direct_lowpass * frames;
		ramp->room_lowpass += ramp->ramp_room_lowpass * frames;
	}
	else if (!sent && !positioned && !stereo)
	{
		/* (a mono voice in the world behind something, the reverb off) */
		float direct_lowpass = ramp->direct_lowpass, ramp_direct_lowpass = ramp->ramp_direct_lowpass;
		float state = ramp->direct_state[0];

		for (frame = 0; frame < frames; frame++)
		{
			state = left_frames[frame] + direct_lowpass * (state - left_frames[frame]);
			output[frame * 2] += state * left;
			output[frame * 2 + 1] += state * right;
			left += ramp_left;
			right += ramp_right;
			direct_lowpass += ramp_direct_lowpass;
		}
		ramp->direct_lowpass = direct_lowpass;
		ramp->direct_state[0] = ramp->direct_state[1] = state;
		ramp->room_lowpass += ramp->ramp_room_lowpass * frames;
	}
	else
	{
		float room = ramp->room, direct_lowpass = ramp->direct_lowpass, room_lowpass = ramp->room_lowpass;
		float direct_left = ramp->direct_state[0], direct_right = ramp->direct_state[1], room_state = ramp->room_state;

		for (frame = 0; frame < frames; frame++)
		{
			float sample_left = left_frames[frame], sample_right = right_frames[frame];

			if (sent)
			{
				float mono = stereo ? 0.5f * (sample_left + sample_right) : sample_left;

				room_state = mono + room_lowpass * (room_state - mono);
				send[frame] += room_state * room;
			}
			if (filtered)
			{
				sample_left = sample_left + direct_lowpass * (direct_left - sample_left);
				sample_right = sample_right + direct_lowpass * (direct_right - sample_right);
			}
			direct_left = sample_left;
			direct_right = sample_right;
			if (positioned)
			{
				/* the channels' middle panned, and their difference kept as
				wide as the far ear's gain: the sound comes from where it is,
				still stereo */
				float middle = 0.5f * (sample_left + sample_right);
				float side = 0.5f * (sample_left - sample_right);
				float far_gain = left < right ? left : right;

				output[frame * 2] += middle * left + side * far_gain;
				output[frame * 2 + 1] += middle * right - side * far_gain;
			}
			else
			{
				output[frame * 2] += sample_left * left;
				output[frame * 2 + 1] += sample_right * right;
			}
			left += ramp_left;
			right += ramp_right;
			room += ramp->ramp_room;
			direct_lowpass += ramp->ramp_direct_lowpass;
			room_lowpass += ramp->ramp_room_lowpass;
		}
		ramp->room = room;
		ramp->direct_lowpass = direct_lowpass;
		ramp->room_lowpass = room_lowpass;
		ramp->direct_state[0] = direct_left;
		ramp->direct_state[1] = direct_right;
		ramp->room_state = room_state;
	}
	ramp->left = left;
	ramp->right = right;
}

/* mixes one voice into its bus: low_frames frames of the low bus and its
send to the reverb, or twice as many of the high bus and its */
static void mix_voice_wii(struct sdl_stream *stream, float *low_output, float *low_send, float *high_output,
	float *high_send, unsigned long low_frames)
{
	unsigned long frequency, rate, frames, done, frame;
	float *output, *send;
	double step, phase;
	BOOL native, stereo = stream->channels == 2;
	struct voice_ramp ramp;

	if (stream->paused || !stream->packet_count || !stream->sample_rate)
		return;
	frequency = stream->frequency ? stream->frequency : stream->sample_rate;
	/* a voice that ran dry and stopped starts over once it has frames again:
	stream_process starts over only a stream with no packets, and the next
	can come before the finished ones are completed */
	if (stream->ended)
	{
		if (!voice_has_frames(stream))
			return;
		resampler_reset(stream);
		stream->gains_valid = FALSE;
	}
	/* only a voice played at the low bus's rate plays on it; one played at
	another frequency is interpolated onto the high bus, where the cubic's
	images fall above what most hear (onto the low one, they fold back
	beside the sound). It keeps its bus while it plays: a change of bus would
	move it in time by the upsampler's delay */
	if (!stream->gains_valid)
		stream->high_bus = frequency != LOW_RATE;
	rate = stream->high_bus ? OUTPUT_RATE : LOW_RATE;
	frames = stream->high_bus ? 2 * low_frames : low_frames;
	output = stream->high_bus ? high_output : low_output;
	send = stream->high_bus ? high_send : low_send;
	step = (double)frequency / rate;
	if (step > VOICE_MAXIMUM_STEP)
		step = VOICE_MAXIMUM_STEP;
	/* played as it is: a frame an output frame, at whole frames */
	native = frequency == rate && stream->phase == (double)(unsigned long)stream->phase;

	/* the gains, worked out again only when something they come from has
	changed (voice_parameters_serial) */
	if (!stream->gains_valid || stream->gains_serial != voice_parameters_serial)
	{
		voice_gains(stream, (float)rate, &stream->target_left, &stream->target_right, &stream->target_room,
			&stream->target_direct_lowpass, &stream->target_room_lowpass);
		stream->gains_serial = voice_parameters_serial;
	}
	if (!stream->gains_valid)
	{
		stream->current_left = stream->target_left;
		stream->current_right = stream->target_right;
		stream->current_room = stream->target_room;
		stream->current_direct_lowpass = stream->target_direct_lowpass;
		stream->current_room_lowpass = stream->target_room_lowpass;
		stream->direct_lowpass[0] = stream->direct_lowpass[1] = 0.0f;
		stream->room_lowpass = 0.0f;
		stream->gains_valid = TRUE;
	}
	ramp.left = stream->current_left;
	ramp.right = stream->current_right;
	ramp.room = stream->current_room;
	ramp.direct_lowpass = stream->current_direct_lowpass;
	ramp.room_lowpass = stream->current_room_lowpass;
	ramp.ramp_left = (stream->target_left - ramp.left) / (float)frames;
	ramp.ramp_right = (stream->target_right - ramp.right) / (float)frames;
	ramp.ramp_room = (stream->target_room - ramp.room) / (float)frames;
	ramp.ramp_direct_lowpass = (stream->target_direct_lowpass - ramp.direct_lowpass) / (float)frames;
	ramp.ramp_room_lowpass = (stream->target_room_lowpass - ramp.room_lowpass) / (float)frames;
	ramp.direct_state[0] = stream->direct_lowpass[0];
	ramp.direct_state[1] = stream->direct_lowpass[1];
	ramp.room_state = stream->room_lowpass;
	stream->current_left = stream->target_left;
	stream->current_right = stream->target_right;
	stream->current_room = stream->target_room;
	stream->current_direct_lowpass = stream->target_direct_lowpass;
	stream->current_room_lowpass = stream->target_room_lowpass;

	if (!ramp.left && !ramp.right && !ramp.room && !ramp.ramp_left && !ramp.ramp_right && !ramp.ramp_room)
	{
		/* a voice turned all the way down (out of earshot), and not sending
		to the reverb, only moves on: the frames it would take, taken */
		unsigned long count = 0;

		phase = stream->phase;
		if (native)
		{
			/* (as below: the frames up to a whole frame of phase, then one
			each) */
			count = (unsigned long)phase + frames - 1;
			phase = 1.0;
		}
		else
		{
			for (frame = 0; frame < frames; frame++)
			{
				while (phase >= 1.0)
				{
					phase -= 1.0;
					count++;
				}
				phase += step;
			}
		}
		stream->phase = phase;
		voice_pass(stream, count);
		stream->ended = stream->silence >= WINDOW_FRAMES;
		return;
	}
	if (!stream->high_bus)
		wii_mix.low_written = TRUE;
	if (ramp.room || ramp.ramp_room)
	{
		if (stream->high_bus)
			wii_mix.high_sent = TRUE;
		else
			wii_mix.low_sent = TRUE;
	}

	for (done = 0; done < frames; done += VOICE_PIECE_FRAMES)
	{
		unsigned long count = frames - done < VOICE_PIECE_FRAMES ? frames - done : VOICE_PIECE_FRAMES;
		const float *left_frames, *right_frames;

		phase = stream->phase;
		if (native)
		{
			/* the window's second frame plays, after the frames up to a
			whole frame of phase, then one each */
			unsigned long ahead = (unsigned long)phase;

			voice_take(stream, ahead + count - 1);
			left_frames = voice_taken[0] + 1 + ahead;
			right_frames = voice_taken[stereo] + 1 + ahead;
			stream->phase = 1.0;
		}
		else
		{
			/* the frames before each output frame's moment, counted, taken,
			and the cubic (Catmull-Rom) through the four around it */
			unsigned long count_taken = 0, base = 0;
			const float *left_taken = voice_taken[0], *right_taken = voice_taken[stereo];

			for (frame = 0; frame < count; frame++)
			{
				while (phase >= 1.0)
				{
					phase -= 1.0;
					count_taken++;
				}
				phase += step;
			}
			voice_take(stream, count_taken);
			phase = stream->phase;
			for (frame = 0; frame < count; frame++)
			{
				float t, w0, w1, w2, w3;
				const float *left_window, *right_window;

				while (phase >= 1.0)
				{
					phase -= 1.0;
					base++;
				}
				t = (float)phase;
				w0 = 0.5f * t * ((2.0f - t) * t - 1.0f);
				w1 = 0.5f * (t * t * (3.0f * t - 5.0f) + 2.0f);
				w2 = 0.5f * t * ((4.0f - 3.0f * t) * t + 1.0f);
				w3 = 0.5f * t * t * (t - 1.0f);
				left_window = left_taken + base;
				right_window = right_taken + base;
				voice_played[0][frame] = left_window[0] * w0 + left_window[1] * w1 + left_window[2] * w2 +
					left_window[3] * w3;
				if (stereo)
				{
					voice_played[1][frame] = right_window[0] * w0 + right_window[1] * w1 + right_window[2] * w2 +
						right_window[3] * w3;
				}
				phase += step;
			}
			stream->phase = phase;
			left_frames = voice_played[0];
			right_frames = voice_played[stereo];
		}
		voice_apply(stream, left_frames, right_frames, output + done * 2, send + done, count, &ramp);
	}
	stream->ended = stream->silence >= WINDOW_FRAMES;
	/* (the states of the low passes fed silence decay into the denormals:
	flush_denormal) */
	stream->direct_lowpass[0] = flush_denormal(ramp.direct_state[0]);
	stream->direct_lowpass[1] = flush_denormal(ramp.direct_state[1]);
	stream->room_lowpass = flush_denormal(ramp.room_state);
}

/* mixes 2 pairs frames of output: the voices into the buses, the reverb into
the low bus, the low bus doubled into the output */
static void mix_pairs(float *output, unsigned long pairs)
{
	struct sdl_stream *stream;
	DSI3DL2LISTENER properties;
	BOOL enabled = reverb_enabled, changed = FALSE;
	unsigned long serial = 0, frames = 2 * pairs, pair;
	float *low = wii_mix.frames + LOW_HISTORY * OUTPUT_CHANNELS;

	memset(output, 0, frames * OUTPUT_CHANNELS * sizeof(float));
	memset(low, 0, pairs * OUTPUT_CHANNELS * sizeof(float));
	memset(reverb.send, 0, pairs * sizeof(float));
	memset(wii_mix.high_send, 0, frames * sizeof(float));
	wii_mix.low_sent = wii_mix.high_sent = wii_mix.low_written = FALSE;
	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
		mix_voice_wii(stream, low, reverb.send, output, wii_mix.high_send, pairs);
	/* a new environment, once the fade to the last is over */
	if (reverb.serial != environment_serial && !reverb.fade)
	{
		properties = environment;
		serial = environment_serial;
		changed = TRUE;
	}
	pthread_mutex_unlock(&mixer_lock);
	if (changed)
		reverb_update(&properties, serial);
	/* the high bus's send, at the low bus's rate */
	if (wii_mix.high_sent)
	{
		for (pair = 0; pair < pairs; pair++)
			reverb.send[pair] += 0.5f * (wii_mix.high_send[pair * 2] + wii_mix.high_send[pair * 2 + 1]);
		wii_mix.low_sent = TRUE;
	}
	/* the reverb runs while something reverberates, and stops once it is
	turned off and faded out, or quiet */
	if (!reverb.silent || (enabled && wii_mix.low_sent && reverb_sent(reverb.send, pairs)))
	{
		reverb_process(reverb.send, low, pairs, enabled ? 1.0f : 0.0f);
		wii_mix.low_written = TRUE;
		if ((!enabled && reverb.level <= 0.0f) || reverb.quiet > REVERB_QUIET_FRAMES)
			reverb_clear();
	}
	/* (a low bus silent for as long as the upsampler reaches back adds
	nothing) */
	if (wii_mix.low_written)
	{
		wii_mix.low_quiet = 0;
	}
	else if (wii_mix.low_quiet >= LOW_HISTORY)
	{
		voice_audio_mix(output, frames);
		limit(output, frames);
		return;
	}
	else
	{
		wii_mix.low_quiet += pairs;
	}
	/* the low bus doubled: the frame UPSAMPLER_PAIRS before the newest as it
	is, then the moment after it, from the frames on each side */
	for (pair = 0; pair < pairs; pair++)
	{
		const float *center = low + ((long)pair - UPSAMPLER_PAIRS) * OUTPUT_CHANNELS;
		float between_left = 0.0f, between_right = 0.0f;
		int k;

		for (k = 0; k < UPSAMPLER_PAIRS; k++)
		{
			between_left += upsampler_weights[k] * (center[-2 * k] + center[2 * (k + 1)]);
			between_right += upsampler_weights[k] * (center[1 - 2 * k] + center[3 + 2 * k]);
		}
		output[pair * 4] += center[0];
		output[pair * 4 + 1] += center[1];
		output[pair * 4 + 2] += between_left;
		output[pair * 4 + 3] += between_right;
	}
	memmove(wii_mix.frames, wii_mix.frames + pairs * OUTPUT_CHANNELS, LOW_HISTORY * OUTPUT_CHANNELS * sizeof(float));
	/* the players' voices (voice_audio.c), dry, under the limiter */
	voice_audio_mix(output, frames);
	limit(output, frames);
}

/* frames of output, from mixes of MIX_AHEAD_FRAMES (what each voice costs a
mix, its gains and set-up, is then paid every 6.5 ms, not as often as the
output asks), the frames mixed past those asked for kept for the next */
static void mix(float *output, unsigned long frames)
{
	while (frames)
	{
		unsigned long count;

		if (wii_mix.ahead_used == wii_mix.ahead_count)
		{
			mix_pairs(wii_mix.ahead, MIX_AHEAD_FRAMES / 2);
			wii_mix.ahead_count = MIX_AHEAD_FRAMES;
			wii_mix.ahead_used = 0;
		}
		count = wii_mix.ahead_count - wii_mix.ahead_used;
		if (count > frames)
			count = frames;
		memcpy(output, wii_mix.ahead + wii_mix.ahead_used * OUTPUT_CHANNELS, count * OUTPUT_CHANNELS * sizeof(float));
		wii_mix.ahead_used += count;
		output += count * OUTPUT_CHANNELS;
		frames -= count;
	}
}
#else
static void mix(float *output, unsigned long frames)
{
	struct sdl_stream *stream;
	DSI3DL2LISTENER properties;
	BOOL enabled = reverb_enabled, changed = FALSE;
	unsigned long serial = 0;

	memset(output, 0, frames * OUTPUT_CHANNELS * sizeof(float));
	memset(reverb.send, 0, frames * sizeof(float));
	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
		mix_voice(stream, output, reverb.send, frames);
	/* a new environment, once the fade to the last is over */
	if (reverb.serial != environment_serial && !reverb.fade)
	{
		properties = environment;
		serial = environment_serial;
		changed = TRUE;
	}
	pthread_mutex_unlock(&mixer_lock);
	if (changed)
		reverb_update(&properties, serial);
	/* the reverb runs while something reverberates, and stops once it is
	turned off and faded out, or quiet */
	if (!reverb.silent || (enabled && reverb_sent(reverb.send, frames)))
	{
		reverb_process(reverb.send, output, frames, enabled ? 1.0f : 0.0f);
		if ((!enabled && reverb.level <= 0.0f) || reverb.quiet > REVERB_QUIET_FRAMES)
			reverb_clear();
	}
	/* the players' voices (voice_audio.c), dry, under the limiter */
	voice_audio_mix(output, frames);
	limit(output, frames);
}
#endif

/* ---------- output */

#ifdef HALO_WII
static BOOL audio_started = FALSE;
/* the loudest sample since the last report (dsound_wii_audio_report) */
static volatile int audio_peak;

/* WiiLibs' audio thread asks for frames: mixed, and made 16-bit */
static void audio_fill(void *user, int16_t *stereo, uint32_t frames)
{
	float buffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];
	int peak = audio_peak;

	(void)user;
	while (frames)
	{
		uint32_t chunk = frames > MIX_CHUNK_FRAMES ? MIX_CHUNK_FRAMES : frames;
		uint32_t index;

		mix(buffer, chunk);
		for (index = 0; index < chunk * OUTPUT_CHANNELS; index++)
		{
			float sample = buffer[index] * 32767.0f;

			stereo[index] = (int16_t)(sample > 32767.0f ? 32767 : sample < -32768.0f ? -32768 : (int)sample);
			if (stereo[index] > peak || -stereo[index] > peak)
				peak = stereo[index] < 0 ? -stereo[index] : stereo[index];
		}
		stereo += chunk * OUTPUT_CHANNELS;
		frames -= chunk;
	}
	audio_peak = peak;
}

/* the output's health in the log: blocks mixed and played, underruns, the
time a block takes to mix, the loudest sample since the last report */
void dsound_wii_audio_report(void)
{
	WlPlatformAudioStats stats;

	if (!audio_started)
		return;
	wl_platform_audio_stats(&stats);
	platform_log("audio: %lu blocks mixed, %lu played, %lu underruns, mixing %lu us a block (at most %lu), "
		"peak %d of 32767", (unsigned long)stats.blocks_filled, (unsigned long)stats.blocks_played,
		(unsigned long)stats.underruns, (unsigned long)(stats.blocks_filled ? stats.fill_us_total / stats.blocks_filled : 0),
		(unsigned long)stats.fill_us_max, audio_peak);
	audio_peak = 0;
}

/* without output, drain voices in real time */
static void *silent_clock_thread(void *parameter)
{
	/* (10 ms of output) */
	float buffer[OUTPUT_RATE / 100 * OUTPUT_CHANNELS];
	struct timespec period = { 0, 10000000L };

	(void)parameter;
	for (;;)
	{
		mix(buffer, OUTPUT_RATE / 100);
		nanosleep(&period, NULL);
	}
	return NULL;
}

static void audio_start(void)
{
	if (audio_started)
		return;
	audio_started = TRUE;
	master_volume = (float)config_real("audio.volume");
	reverb_enabled = config_boolean("audio.reverb");
	upsampler_initialize();
	reverb_initialize();
	if (config_boolean("audio.enabled") && wl_platform_audio_start(OUTPUT_RATE, 256, audio_fill, NULL) == WL_OK)
		return;
	platform_log("cannot start the audio output; sound is silent");
	{
		pthread_t thread;

		pthread_create(&thread, NULL, silent_clock_thread, NULL);
		pthread_detach(thread);
	}
}

/* (the Wii has one output) */
void dsound_sdl_output_device_check(void)
{
}
#else

static SDL_AudioStream *audio_stream;
static BOOL audio_started = FALSE;
/* the device it plays on (audio.output_device), and when it was looked at */
static char audio_device_name[PLATFORM_AUDIO_DEVICE_NAME_SIZE];
static unsigned long audio_device_read_at = (unsigned long)-1;

/* audio.output_device ("default": the system's; none on Android) */
static const char *audio_device_setting(void)
{
	const char *name = config_string("audio.output_device");

	return name && name[0] ? name : "default";
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	float buffer[MIX_CHUNK_FRAMES * OUTPUT_CHANNELS];

	(void)userdata;
	(void)total_amount;
	while (additional_amount > 0)
	{
		unsigned long frames = (unsigned long)additional_amount / (OUTPUT_CHANNELS * sizeof(float));

		if (frames > MIX_CHUNK_FRAMES)
			frames = MIX_CHUNK_FRAMES;
		if (!frames)
			frames = 1;
		mix(buffer, frames);
		SDL_PutAudioStreamData(stream, buffer, (int)(frames * OUTPUT_CHANNELS * sizeof(float)));
		additional_amount -= (int)(frames * OUTPUT_CHANNELS * sizeof(float));
	}
}

/* without a device, drain voices in real time */
static void *silent_clock_thread(void *parameter)
{
	float buffer[480 * OUTPUT_CHANNELS];
	struct timespec next;

	(void)parameter;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		mix(buffer, 480);
		next.tv_nsec += 10000000L;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	return NULL;
}

static void audio_start(void)
{
	SDL_AudioSpec spec;

	if (audio_started)
		return;
	audio_started = TRUE;
	master_volume = (float)config_real("audio.volume");
	reverb_enabled = config_boolean("audio.reverb");
	resampler_initialize();
	resampler_phases_initialize();
	reverb_initialize();

	if (config_boolean("audio.enabled") && platform_sdl_initialize())
	{
		spec.format = SDL_AUDIO_F32;
		spec.channels = OUTPUT_CHANNELS;
		spec.freq = OUTPUT_RATE;
#ifdef HALO_ARM64_GUEST
		/* frames per callback: on Android each callback is handed to a thread
		that can run the guest (host_sdl.c): 512 left it too little time and
		the menus' music broke up, which 1024 does not (about 21 ms at 48 kHz,
		11 ms more than 512) */
		SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
#else
		SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
#endif
		snprintf(audio_device_name, sizeof(audio_device_name), "%s", audio_device_setting());
		audio_device_read_at = config_changes();
		audio_stream = SDL_OpenAudioDeviceStream(platform_audio_device(FALSE, audio_device_name), &spec,
			audio_callback, NULL);
		if (!audio_stream && strcmp(audio_device_name, "default"))
			audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
		if (audio_stream)
		{
			SDL_ResumeAudioStreamDevice(audio_stream);
			return;
		}
		platform_log("cannot open an audio device (%s); sound is silent", SDL_GetError());
	}
	{
		pthread_t thread;

		pthread_create(&thread, NULL, silent_clock_thread, NULL);
		pthread_detach(thread);
	}
}

/* (the event thread, each frame: sdl_platform.c) audio.output_device
changed (Settings > Audio): the sound goes on on the new device, else the
system's default */
void dsound_sdl_output_device_check(void)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *stream;

	if (!audio_stream || audio_device_read_at == config_changes())
		return;
	audio_device_read_at = config_changes();
	if (!strcmp(audio_device_name, audio_device_setting()))
		return;
	snprintf(audio_device_name, sizeof(audio_device_name), "%s", audio_device_setting());
	spec.format = SDL_AUDIO_F32;
	spec.channels = OUTPUT_CHANNELS;
	spec.freq = OUTPUT_RATE;
	SDL_DestroyAudioStream(audio_stream);
	stream = SDL_OpenAudioDeviceStream(platform_audio_device(FALSE, audio_device_name), &spec, audio_callback, NULL);
	if (!stream)
		stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, NULL);
	audio_stream = stream;
	if (audio_stream)
	{
		SDL_ResumeAudioStreamDevice(audio_stream);
		platform_log("audio: playing on %s", audio_device_name);
	}
	else
	{
		pthread_t thread;

		/* (none at all: the voices drained in real time, as at the start) */
		platform_log("audio: cannot open an audio device (%s); sound is silent", SDL_GetError());
		pthread_create(&thread, NULL, silent_clock_thread, NULL);
		pthread_detach(thread);
	}
}

#endif

/* ---------- completion */

static void packet_release(struct sdl_stream *stream, struct voice_packet *entry)
{
	/* (its bytes are the game's again: what was decoded from them is not
	the next packet's, even at the same address) */
	entry->source = NULL;
	stream->block_source = NULL;
}

/* completes the head packet; called with the lock held, which the game's
callback runs without */
static void stream_complete_head(struct sdl_stream *stream, DWORD status, DWORD completed_size)
{
	struct voice_packet *entry = &stream->packets[stream->packet_head];
	XMEDIAPACKET packet = entry->packet;

	/* (one not played to its end, flushed: the cursor was its place, and the
	mixer may take the next while the lock is let go below) */
	if (!entry->finished)
		stream->cursor = 0;
	packet_release(stream, entry);
	entry->finished = FALSE;
	stream->packet_head = (stream->packet_head + 1) % MAXIMUM_STREAM_PACKETS;
	stream->packet_count--;
	if (packet.pdwCompletedSize)
		*packet.pdwCompletedSize = completed_size;
	if (packet.pdwStatus)
		*packet.pdwStatus = status;
	if (stream->callback)
	{
		pthread_mutex_unlock(&mixer_lock);
		stream->callback(stream->context, packet.pContext, status);
		pthread_mutex_lock(&mixer_lock);
	}
	else if (packet.hCompletionEvent)
	{
		SetEvent(packet.hCompletionEvent);
	}
}

static void streams_complete_finished(void)
{
	struct sdl_stream *stream;

	pthread_mutex_lock(&mixer_lock);
	for (stream = streams; stream; stream = stream->next)
	{
		while (stream->packet_count && stream->packets[stream->packet_head].finished)
			stream_complete_head(stream, XMEDIAPACKET_STATUS_SUCCESS, stream->packets[stream->packet_head].packet.dwMaxSize);
	}
	pthread_mutex_unlock(&mixer_lock);
}

/* ---------- stream interface */

static struct sdl_stream *stream_from_interface(void *stream)
{
	return (struct sdl_stream *)stream;
}

static ULONG STDMETHODCALLTYPE stream_add_reference(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = ++stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	return count;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object);

static ULONG STDMETHODCALLTYPE stream_release(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct sdl_stream **link;
	ULONG count;

	pthread_mutex_lock(&mixer_lock);
	count = --stream->reference_count;
	pthread_mutex_unlock(&mixer_lock);
	if (count)
		return count;

	stream_flush(object);
	pthread_mutex_lock(&mixer_lock);
	for (link = &streams; *link; link = &(*link)->next)
	{
		if (*link == stream)
		{
			*link = stream->next;
			break;
		}
	}
	pthread_mutex_unlock(&mixer_lock);
	free(stream);
	return 0;
}

static HRESULT STDMETHODCALLTYPE stream_get_info(IDirectSoundStream *object, LPXMEDIAINFO information)
{
	struct sdl_stream *stream = stream_from_interface(object);

	memset(information, 0, sizeof(*information));
	information->dwFlags = XMO_STREAMF_FIXED_SAMPLE_SIZE | XMO_STREAMF_INPUT_ASYNC;
	information->dwInputSize = stream->adpcm ? XBOX_ADPCM_BLOCK_BYTES * stream->channels : 2 * stream->channels;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_get_status(IDirectSoundStream *object, LPDWORD status)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	*status = stream->packet_count < MAXIMUM_STREAM_PACKETS ? XMO_STATUSF_ACCEPT_INPUT_DATA : 0;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_process(IDirectSoundStream *object, LPCXMEDIAPACKET input, LPCXMEDIAPACKET output)
{
	struct sdl_stream *stream = stream_from_interface(object);
	struct voice_packet *entry;

	(void)output;
	if (!input)
		return E_INVALIDARG;
	pthread_mutex_lock(&mixer_lock);
	if (stream->packet_count == MAXIMUM_STREAM_PACKETS)
	{
		pthread_mutex_unlock(&mixer_lock);
		return E_OUTOFMEMORY;
	}
	entry = &stream->packets[(stream->packet_head + stream->packet_count) % MAXIMUM_STREAM_PACKETS];
	entry->packet = *input;
	entry->source = input->pvBuffer;
	entry->frames = input->pvBuffer ? packet_frames(stream->adpcm, input->dwMaxSize, stream->channels) : 0;
	entry->finished = FALSE;
	if (input->pdwStatus)
		*input->pdwStatus = XMEDIAPACKET_STATUS_PENDING;
	if (input->pdwCompletedSize)
		*input->pdwCompletedSize = 0;
	if (!stream->packet_count)
	{
		/* a stream that ran dry starts over */
		stream->cursor = 0;
		resampler_reset(stream);
		stream->gains_valid = FALSE;
	}
	stream->packet_count++;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_discontinuity(IDirectSoundStream *object)
{
	(void)object;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE stream_flush(IDirectSoundStream *object)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	while (stream->packet_count)
	{
		struct voice_packet *head = &stream->packets[stream->packet_head];

		/* Cancellation is synchronous. SUCCESS would let the game's
		completion callback refill the stream we are stopping. */
		stream_complete_head(stream, XMEDIAPACKET_STATUS_FLUSHED,
			head->finished ? head->packet.dwMaxSize : 0);
	}
	stream->cursor = 0;
	resampler_reset(stream);
	/* (the next sound on the channel says where it is) */
	stream->stereo_positioned = FALSE;
	pthread_mutex_unlock(&mixer_lock);
	return S_OK;
}

/* A stereo voice of a sound in the world (`positioned`) is panned towards
it, `pan` from -1 (left) to 1 (right), and muffled and reverberated as a 3D
voice is (its I3DL2 source, which the game sets as a 3D channel's), its room
send rolling off from `minimum_distance` with `distance`. The game fades its
volume with distance itself, by `distance_fade`. Not positioned, it plays as
the Xbox played every stereo sound, unpanned and dry. (sound_manager.c,
update_channels: sound_dsound_xbox.c calls this.) */
void dsound_sdl_stream_set_stereo_position(IDirectSoundStream *object, BOOL positioned, float pan,
	float distance, float minimum_distance, float distance_fade)
{
	struct sdl_stream *stream = stream_from_interface(object);

	pthread_mutex_lock(&mixer_lock);
	stream->stereo_positioned = positioned && stream->channels == 2;
	stream->stereo_pan = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
	stream->stereo_distance = distance > 0.0f ? distance : 0.0f;
	stream->stereo_distance_fade = distance_fade < 0.0f ? 0.0f : (distance_fade > 1.0f ? 1.0f : distance_fade);
	/* (as the game gives a 3D channel: no maximum) */
	stream->minimum_distance = minimum_distance > 0.0f ? minimum_distance : 0.0f;
	stream->maximum_distance = 3.4e38f;
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
}

static IDirectSoundStreamVtbl stream_vtable =
{
	stream_add_reference,
	stream_release,
	stream_get_info,
	stream_get_status,
	stream_process,
	stream_discontinuity,
	stream_flush,
};

/* ---------- the DirectSound object */

struct sdl_direct_sound
{
	ULONG reference_count;
};

static struct sdl_direct_sound direct_sound = { 0 };

HRESULT WINAPI DirectSoundCreate(LPGUID device_id, LPDIRECTSOUND *result, LPUNKNOWN outer)
{
	(void)device_id;
	(void)outer;
	audio_start();
	direct_sound.reference_count++;
	*result = (LPDIRECTSOUND)&direct_sound;
	return DS_OK;
}

ULONG WINAPI IDirectSound_Release(LPDIRECTSOUND sound)
{
	(void)sound;
	return direct_sound.reference_count ? --direct_sound.reference_count : 0;
}

VOID WINAPI DirectSoundDoWork(void)
{
	static unsigned long volume_read_at = (unsigned long)-1;

	/* (audio.volume and audio.reverb read again when the settings change: on
	the game's thread, not the mixer's, whose lock the config's file I/O
	would hold) */
	if (volume_read_at != config_changes())
	{
		volume_read_at = config_changes();
		master_volume = (float)config_real("audio.volume");
		reverb_enabled = config_boolean("audio.reverb");
		voice_parameters_serial++;
	}
	streams_complete_finished();
}

VOID WINAPI DirectSoundUseFullHRTF(void)
{
}

HRESULT WINAPI IDirectSound_GetCaps(LPDIRECTSOUND sound, LPDSCAPS caps)
{
	(void)sound;
	memset(caps, 0, sizeof(*caps));
	caps->dwFree2DBuffers = 64;
	caps->dwFree3DBuffers = 64;
	caps->dwFreeBufferSGEs = 2047;
	caps->dwMemoryAllocated = 0;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_GetSpeakerConfig(LPDIRECTSOUND sound, LPDWORD speaker_config)
{
	(void)sound;
	*speaker_config = DSSPEAKER_STEREO;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_DownloadEffectsImage(LPDIRECTSOUND sound, LPCVOID image, DWORD image_size,
	LPCDSEFFECTIMAGELOC image_location, LPDSEFFECTIMAGEDESC *image_description)
{
	(void)sound;
	(void)image;
	(void)image_size;
	(void)image_location;
	if (image_description)
		*image_description = NULL;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CommitDeferredSettings(LPDIRECTSOUND sound) { (void)sound; return DS_OK; }
HRESULT WINAPI IDirectSound_SetMixBinHeadroom(LPDIRECTSOUND sound, DWORD mix_bin_mask, DWORD headroom) { (void)sound; (void)mix_bin_mask; (void)headroom; return DS_OK; }
HRESULT WINAPI IDirectSound_SetI3DL2Listener(LPDIRECTSOUND sound, LPCDSI3DL2LISTENER listener_properties, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	environment = *listener_properties;
	environment_serial++;
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetDistanceFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.distance_factor = factor > 0.0f ? factor : 1.0f;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetRolloffFactor(LPDIRECTSOUND sound, FLOAT factor, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.rolloff_factor = factor >= 0.0f ? factor : 1.0f;
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetPosition(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.position[0] = x;
	listener.position[1] = y;
	listener.position[2] = z;
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_SetVelocity(LPDIRECTSOUND sound, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)sound; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }

static void normalize3(float *vector)
{
	float length = sqrtf(dot3(vector, vector));

	if (length > 1.0e-6f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

HRESULT WINAPI IDirectSound_SetOrientation(LPDIRECTSOUND sound, FLOAT x_front, FLOAT y_front, FLOAT z_front,
	FLOAT x_top, FLOAT y_top, FLOAT z_top, DWORD apply)
{
	(void)sound;
	(void)apply;
	pthread_mutex_lock(&mixer_lock);
	listener.front[0] = x_front;
	listener.front[1] = y_front;
	listener.front[2] = z_front;
	listener.top[0] = x_top;
	listener.top[1] = y_top;
	listener.top[2] = z_top;
	normalize3(listener.front);
	normalize3(listener.top);
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundStream(LPDIRECTSOUND sound, LPCDSSTREAMDESC description,
	LPDIRECTSOUNDSTREAM *result, LPUNKNOWN outer)
{
	struct sdl_stream *stream = calloc(1, sizeof(*stream));
	const WAVEFORMATEX *format = description->lpwfxFormat;

	(void)sound;
	(void)outer;
	if (!stream)
		return E_OUTOFMEMORY;
	stream->object.lpVtbl = &stream_vtable;
	stream->reference_count = 1;
	stream->callback = description->lpfnCallback;
	stream->context = description->lpvContext;
	stream->adpcm = format && format->wFormatTag == WAVE_FORMAT_XBOX_ADPCM;
	stream->channels = format && format->nChannels == 2 ? 2 : 1;
	stream->sample_rate = format ? format->nSamplesPerSec : 0;
	stream->frequency = stream->sample_rate;
	stream->volume = 1.0f;
	/* DirectSound's default mix bins: a mono voice to both fronts, a
	stereo voice's channels to the front left and right */
	stream->mix_left = 1.0f;
	stream->mix_right = 1.0f;
	stream->has_3d = (description->dwFlags & DSSTREAMCAPS_CTRL3D) != 0;
	stream->mode = DS3DMODE_NORMAL;
	stream->minimum_distance = DS3D_DEFAULTMINDISTANCE;
	stream->maximum_distance = DS3D_DEFAULTMAXDISTANCE;
	resampler_reset(stream);
	pthread_mutex_lock(&mixer_lock);
	stream->next = streams;
	streams = stream;
	pthread_mutex_unlock(&mixer_lock);
	*result = &stream->object;
	return DS_OK;
}

/* January-era DirectSound exports the game declares itself
(sound_dsound_xbox.c); the XDK 3911 headers no longer carry them */

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream)
{
	stream_flush(stream);
}

unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long active;

	pthread_mutex_lock(&mixer_lock);
	active = record->packet_count != 0;
	pthread_mutex_unlock(&mixer_lock);
	return active;
}

#define STREAM_SETTER(body) \
	struct sdl_stream *record = stream_from_interface(stream); \
	pthread_mutex_lock(&mixer_lock); \
	body; \
	voice_parameters_serial++; \
	pthread_mutex_unlock(&mixer_lock); \
	return DS_OK;

HRESULT WINAPI IDirectSoundStream_SetFrequency(LPDIRECTSOUNDSTREAM stream, DWORD frequency)
{
	STREAM_SETTER(record->frequency = frequency ? frequency : record->sample_rate)
}

HRESULT WINAPI IDirectSoundStream_SetVolume(LPDIRECTSOUNDSTREAM stream, LONG volume)
{
	STREAM_SETTER(record->volume = gain_from_millibels(volume))
}

HRESULT WINAPI IDirectSoundStream_SetMixBins(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask)
{
	STREAM_SETTER(
		record->mix_left = (mix_bin_mask & DSMIXBIN_FRONT_LEFT) ? 1.0f : 0.0f;
		record->mix_right = (mix_bin_mask & DSMIXBIN_FRONT_RIGHT) ? 1.0f : 0.0f)
}

/* volumes come in the order of the set bits of the mask */
HRESULT WINAPI IDirectSoundStream_SetMixBinVolumes(LPDIRECTSOUNDSTREAM stream, DWORD mix_bin_mask, const LONG *volumes)
{
	struct sdl_stream *record = stream_from_interface(stream);
	unsigned long bit, index = 0;

	pthread_mutex_lock(&mixer_lock);
	for (bit = 0; bit < 32; bit++)
	{
		if (!(mix_bin_mask & (1UL << bit)))
			continue;
		if ((1UL << bit) == DSMIXBIN_FRONT_LEFT)
			record->mix_left = gain_from_millibels(volumes[index]);
		else if ((1UL << bit) == DSMIXBIN_FRONT_RIGHT)
			record->mix_right = gain_from_millibels(volumes[index]);
		index++;
	}
	voice_parameters_serial++;
	pthread_mutex_unlock(&mixer_lock);
	return DS_OK;
}

HRESULT WINAPI IDirectSoundStream_SetMode(LPDIRECTSOUNDSTREAM stream, DWORD mode, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->mode = mode)
}

HRESULT WINAPI IDirectSoundStream_SetPosition(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->position[0] = x; record->position[1] = y; record->position[2] = z)
}

HRESULT WINAPI IDirectSoundStream_SetMinDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->minimum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetMaxDistance(LPDIRECTSOUNDSTREAM stream, FLOAT distance, DWORD apply)
{
	(void)apply;
	STREAM_SETTER(record->maximum_distance = distance)
}

HRESULT WINAPI IDirectSoundStream_SetI3DL2Source(LPDIRECTSOUNDSTREAM stream, LPCDSI3DL2BUFFER source, DWORD apply)
{
	LONG direct, direct_hf, room, room_hf;

	(void)apply;
	/* the direct path: at low frequencies obstruction and occlusion take
	their LF ratio of their high frequency levels, which (with the source's
	own) muffle it above the HF reference */
	direct = source->lDirect +
		(LONG)(source->Obstruction.lHFLevel * source->Obstruction.flLFRatio) +
		(LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	if (direct > 0)
		direct = 0;
	direct_hf = source->lDirect + source->lDirectHF + source->Obstruction.lHFLevel + source->Occlusion.lHFLevel;
	/* the room send: occlusion muffles it as it does the direct path, and
	obstruction does not reach it */
	room = source->lRoom + (LONG)(source->Occlusion.lHFLevel * source->Occlusion.flLFRatio);
	room_hf = source->lRoom + source->lRoomHF + source->Occlusion.lHFLevel;
	{
		STREAM_SETTER(
			record->direct = direct;
			record->direct_hf = direct_hf;
			record->room = room;
			record->room_hf = room_hf;
			record->room_rolloff_factor = source->flRoomRolloffFactor)
	}
}

HRESULT WINAPI IDirectSoundStream_Pause(LPDIRECTSOUNDSTREAM stream, DWORD pause)
{
	STREAM_SETTER(record->paused = pause == DSSTREAMPAUSE_PAUSE)
}

HRESULT WINAPI IDirectSoundStream_SetVelocity(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)stream; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeAngles(LPDIRECTSOUNDSTREAM stream, DWORD inside, DWORD outside, DWORD apply) { (void)stream; (void)inside; (void)outside; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOrientation(LPDIRECTSOUNDSTREAM stream, FLOAT x, FLOAT y, FLOAT z, DWORD apply) { (void)stream; (void)x; (void)y; (void)z; (void)apply; return DS_OK; }
HRESULT WINAPI IDirectSoundStream_SetConeOutsideVolume(LPDIRECTSOUNDSTREAM stream, LONG volume, DWORD apply) { (void)stream; (void)volume; (void)apply; return DS_OK; }

/* ---------- buffers

The game's only buffer is a silent looping one that keeps the voice
processor busy; it needs no mixing. */

struct null_buffer
{
	ULONG reference_count;
	LPVOID data;
	DWORD size;
	BOOL playing;
};

HRESULT WINAPI DirectSoundCreateBuffer(LPCDSBUFFERDESC description, LPDIRECTSOUNDBUFFER *result)
{
	struct null_buffer *buffer = calloc(1, sizeof(*buffer));

	(void)description;
	if (!buffer)
		return E_OUTOFMEMORY;
	buffer->reference_count = 1;
	*result = (LPDIRECTSOUNDBUFFER)buffer;
	return DS_OK;
}

HRESULT WINAPI IDirectSound_CreateSoundBuffer(LPDIRECTSOUND sound, LPCDSBUFFERDESC description,
	LPDIRECTSOUNDBUFFER *result, LPUNKNOWN outer)
{
	(void)sound;
	(void)outer;
	return DirectSoundCreateBuffer(description, result);
}

ULONG WINAPI IDirectSoundBuffer_Release(LPDIRECTSOUNDBUFFER buffer)
{
	struct null_buffer *record = (struct null_buffer *)buffer;
	ULONG count = --record->reference_count;

	if (!count)
		free(record);
	return count;
}

HRESULT WINAPI IDirectSoundBuffer_SetBufferData(LPDIRECTSOUNDBUFFER buffer, LPVOID data, DWORD size)
{
	struct null_buffer *record = (struct null_buffer *)buffer;

	record->data = data;
	record->size = size;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Play(LPDIRECTSOUNDBUFFER buffer, DWORD reserved1, DWORD reserved2, DWORD flags)
{
	(void)reserved1;
	(void)reserved2;
	(void)flags;
	((struct null_buffer *)buffer)->playing = TRUE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_Stop(LPDIRECTSOUNDBUFFER buffer)
{
	((struct null_buffer *)buffer)->playing = FALSE;
	return DS_OK;
}

HRESULT WINAPI IDirectSoundBuffer_SetCurrentPosition(LPDIRECTSOUNDBUFFER buffer, DWORD play_cursor) { (void)buffer; (void)play_cursor; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetLoopRegion(LPDIRECTSOUNDBUFFER buffer, DWORD loop_start, DWORD loop_length) { (void)buffer; (void)loop_start; (void)loop_length; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetPitch(LPDIRECTSOUNDBUFFER buffer, LONG pitch) { (void)buffer; (void)pitch; return DS_OK; }
HRESULT WINAPI IDirectSoundBuffer_SetVolume(LPDIRECTSOUNDBUFFER buffer, LONG volume) { (void)buffer; (void)volume; return DS_OK; }
