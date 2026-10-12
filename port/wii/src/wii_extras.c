/*
WII_EXTRAS.C

The desktop builds' additions to the game that the Wii goes without, each
answering as its interface says it does when there is none:

- high-res text and HUD (port/linux/src/text_hires.c, hud_hires.c): no font
  or texture stands in, so the maps' own bitmaps are drawn;
- the menus read from XML (menu_files.c): none, so the maps' own menus are
  the game's;
- the mouse pointer in the menus and on the scoreboard (d3d8_gl.c): none;
- Halo Custom Edition's texel channels (xbox_textures.c): those maps do not
  run on the Wii (src/wii_memory.c);
- voice chat (voice_audio.c): no microphone, nothing to hear;
- internet play (p2p*.c): system link only;
- write tracking of guest memory (memory_watch.c): none, so a write needs no
  preparing.
*/

#include "platform.h"
#include "halo_ui_pointer.h"
#include "posix.h"

#include <string.h>

/* ---------- high-res text and HUD */

struct text_hires_glyph;

long text_hires_font(char const *tag_name, float cap_height, float oversample)
{
	(void)tag_name;
	(void)cap_height;
	(void)oversample;
	return -1;
}

int text_hires_covers(long font, unsigned long code)
{
	(void)font;
	(void)code;
	return 0;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	(void)font;
	(void)code;
	(void)glyph;
	return 0;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	(void)texture;
	(void)width;
	(void)height;
}

/* (no atlas, so it never starts over and the hook is never called) */
void text_hires_set_reset_hook(void (*hook)(void))
{
	(void)hook;
}

long hud_hires_asset_count(void)
{
	return 0;
}

char const *hud_hires_asset_tag(long asset)
{
	(void)asset;
	return "";
}

long hud_hires_asset_bitmap(long asset)
{
	(void)asset;
	return -1;
}

long hud_hires_asset_fits(long asset, long width, long height)
{
	(void)asset;
	(void)width;
	(void)height;
	return 0;
}

/* (the sprites drawn from a high-res texture: none here) */
unsigned long hud_hires_asset_sprites(long asset)
{
	(void)asset;
	return 0;
}

int hud_hires_sprites_drawable(long asset, unsigned long address, unsigned long level0_size)
{
	(void)asset;
	(void)address;
	(void)level0_size;
	return 0;
}

void hud_hires_register_placeholder(long asset, const unsigned long *texture)
{
	(void)asset;
	(void)texture;
}

/* ---------- the XML menus */

struct halo_menus;

struct halo_menus const *halo_menus_load(void)
{
	return NULL;
}

/* (text as UTF-16, its terminator included, "\n" as "\r\n": what the menus'
strings would be, kept for the callers that convert their own) */
long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity)
{
	long written = 0;
	const unsigned char *in = (const unsigned char *)utf8;

	while (*in && written < capacity - 1)
	{
		unsigned long code = *in++;

		if (code >= 0xC0 && (*in & 0xC0) == 0x80)
		{
			int extra = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : 1;

			code &= 0x3F >> extra;
			while (extra-- && (*in & 0xC0) == 0x80)
				code = (code << 6) | (*in++ & 0x3F);
		}
		if (code == '\n' && written < capacity - 2)
			out[written++] = '\r';
		out[written++] = code > 0xFFFF ? '?' : (unsigned short)code;
	}
	if (capacity > 0)
		out[written++] = 0;
	return written;
}

void halo_menus_log(char const *file, long line, char const *message, char const *detail)
{
	platform_log("menus: %s:%ld: %s %s", file, line, message, detail ? detail : "");
}

void halo_menus_art_register(void const *texture, char const *png)
{
	(void)texture;
	(void)png;
}

void halo_menus_art_forget(void)
{
}

/* ---------- the mouse pointer */

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	(void)menus_active;
	memset(pointer, 0, sizeof(*pointer));
	return 0;
}

/* -1: there is none (as on Android) */
int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	(void)offered;
	memset(pointer, 0, sizeof(*pointer));
	return -1;
}

/* ---------- Halo Custom Edition's texels */

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	(void)texels;
	(void)channel_order;
}

void halo_custom_edition_texels_forget(void)
{
}

/* ---------- voice chat */

int voice_audio_microphone(int open)
{
	(void)open;
	return 0;
}

int voice_audio_read_frame(float *frame)
{
	(void)frame;
	return 0;
}

float voice_audio_level(const float *frame)
{
	(void)frame;
	return 0.0f;
}

int voice_audio_encode(const float *frame, int bitrate, unsigned char *packet, int maximum)
{
	(void)frame;
	(void)bitrate;
	(void)packet;
	(void)maximum;
	return 0;
}

void voice_audio_play(int speaker, unsigned short sequence, const unsigned char *packet, int length, float gain,
	float pan)
{
	(void)speaker;
	(void)sequence;
	(void)packet;
	(void)length;
	(void)gain;
	(void)pan;
}

void voice_audio_forget(int speaker)
{
	(void)speaker;
}

void voice_audio_forget_all(void)
{
}

void voice_audio_set_volume(float volume)
{
	(void)volume;
}

/* ---------- internet play: none (system link only) */

struct p2p_listing;

void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = '\0';
	if (name_size > 0)
		name[0] = '\0';
}

void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	(void)name;
	if (size > 0)
	{
		strncpy(destination, source ? source : "", (size_t)size - 1);
		destination[size - 1] = '\0';
	}
}

void p2p_hardware_id(char *hex, int size)
{
	if (size > 0)
		hex[0] = '\0';
}

void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{
	p2p_discord_sanitize(destination, size, source, 0);
}

int p2p_invite_link(char *link, int size)
{
	if (size > 0)
		link[0] = '\0';
	return 0;
}

int p2p_join_invite(const char *text)
{
	(void)text;
	return 0;
}

int p2p_listing_unlock(struct p2p_listing *listing, const char *password)
{
	(void)listing;
	(void)password;
	return 0;
}

void p2p_lobby_browse(int on)
{
	(void)on;
}

int p2p_lobby_games(struct p2p_listing *games, int maximum_count)
{
	(void)games;
	(void)maximum_count;
	return 0;
}

void p2p_lobby_mark_failed(const unsigned char *identifier)
{
	(void)identifier;
}

void p2p_lobby_refresh(void)
{
}

int p2p_peer_address(const unsigned char *identifier, unsigned long *address)
{
	(void)identifier;
	(void)address;
	return 0;
}

unsigned long p2p_peer_endpoint_address(unsigned long virtual_address)
{
	return virtual_address;
}

void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams)
{
	(void)name;
	(void)map;
	(void)gametype;
	(void)engine_type;
	(void)open;
	(void)in_progress;
	(void)has_teams;
}

void p2p_set_game_player_counts(int count, int maximum)
{
	(void)count;
	(void)maximum;
}

void p2p_set_hosting_allowed(int allowed)
{
	(void)allowed;
}

void p2p_set_hosting_password(const char *password)
{
	(void)password;
}

void p2p_set_hosting_public(int public)
{
	(void)public;
}

/* (map sharing's, port/linux/game/map_share.c: no internet player to reach
or answer; its hashing, p2p_blake2b_256, is reached only for a Custom Edition
map, which the Wii does not run, and is left to the link's stubs) */
int p2p_peer_key_hash(unsigned long virtual_address, unsigned char *identifier, unsigned char *key_hash)
{
	(void)virtual_address;
	(void)identifier;
	(void)key_hash;
	return 0;
}

void p2p_set_answering_players(int on)
{
	(void)on;
}

int p2p_reach_player(const unsigned char *key_hash)
{
	(void)key_hash;
	return 0;
}

int p2p_peer_reached(const unsigned char *identifier, unsigned long *address)
{
	(void)identifier;
	(void)address;
	return 0;
}

/* ---------- MSVC's compiler barrier, an intrinsic clang has only for x86
(source/scenario/scenario.c) */

void _ReadWriteBarrier(void)
{
	__asm__ volatile ("" : : : "memory");
}

/* the Winsock layer's internet-play hooks (port/linux/src/xnet.c): no
address is ever a peer's */
void p2p_initialize(unsigned long local_address)
{
	(void)local_address;
}

/* this machine's identifier in its XNADDR (6 bytes), made once a run */
const unsigned char *p2p_identifier(void)
{
	static unsigned char identifier[6];
	static int made;

	if (!made)
	{
		posix_random_bytes(identifier, sizeof(identifier));
		made = 1;
	}
	return identifier;
}

int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port)
{
	(void)stream;
	(void)socket;
	(void)address;
	(void)port;
	return 0;
}

int p2p_incoming(int stream, unsigned long *address, unsigned short *port)
{
	(void)stream;
	(void)address;
	(void)port;
	return 0;
}

int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count)
{
	(void)port;
	(void)addresses;
	(void)ports;
	(void)maximum_count;
	return 0;
}

int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data,
	int size)
{
	(void)source_port;
	(void)address;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size)
{
	(void)source_port;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

void p2p_socket_port(int socket, int stream, int listening, unsigned short port)
{
	(void)socket;
	(void)stream;
	(void)listening;
	(void)port;
}

void p2p_port_taken(int stream, unsigned short port)
{
	(void)stream;
	(void)port;
}

void p2p_socket_closed(int socket, unsigned short datagram_port)
{
	(void)socket;
	(void)datagram_port;
}

/* (the mixer, on the audio thread) no voices to add */
void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

/* ---------- guest memory write tracking (memory_watch.c) */

void memory_watch_prepare_write(void *address, unsigned long size)
{
	(void)address;
	(void)size;
}
