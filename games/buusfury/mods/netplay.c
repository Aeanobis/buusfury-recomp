// Online one-on-one battles (asked for 2026-10-19).
//
// Both machines run the same game and the same mod. The host's whole machine
// state is copied to the guest when a match starts, so both start identical,
// and from then on each side sends the other its buttons frame by frame and
// runs a frame only once it holds both sides' buttons for it (lockstep, with a
// few frames of delay to cover the round trip; the runtime's frame gate holds
// the game while a frame's buttons are still on their way). The host's fighter
// is the game's own player object; the guest's is a second player object built
// with the game's player constructor and added to the area's object list. The
// player code asks its object for its character record and its button record
// through two virtual getters (class table +0x80 and +0x84); those are hooked
// to answer with the guest's record and a second button record for the second
// object, so the same code drives both fighters independently. The match ends
// when a fighter's health reaches zero, and each machine then goes back to the
// snapshot it took before the match: nothing of it stays in anyone's game.
//
// The way in is a page of the game's Start menu, ONLINE, between Options and
// Journal (asked for 2026-10-20, in place of a line on the save pad's menu):
// RANDOM BATTLE, HOST A BATTLE (a room code for a friend), JOIN WITH CODE.
//
// The relay server (tools/netplay/netplay_server.py) pairs two players, by a
// four-digit room code (a battle with a friend) or with whoever is waiting (a
// random battle, asked for 2026-10-20; the relay tells the first to come that
// it hosts), and passes bytes between them. Its address goes in
// <save folder>/buusfury.netplay as "server host:port".
#include "bf.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#define NP_VERSION 1
#define FN_PLAYER_CTOR 0x080109AA   // (object or 0, sprite def, &int16 x,y, 1): the game's player object
#define FN_ALLOC 0x0803D5B8         // (bytes): the game's heap
#define FN_AREA_ADD 0x0801191A      // (the area's object list, object): the list every object in the area is updated and drawn from
#define FN_RECORD_GETTER 0x0804F840 // player class +0x80: the character record the object plays
#define FN_INPUT_GETTER 0x0804F8C8  // player class +0x84: the button record the object reads
#define ROM_ADDRESS_TABLE 0x080716A0 // the game's table of addresses; +0x14 the area's object list
#define RAM_FRAME_COUNTER 0x03000F94 // the game's frame counter (stamped on each press in a button record)
#define RAM_FIGHTING_ALLOWED (BF_RAM_ZONE + 0xC) // the world state's "this area allows fighting" word (towns have 0: A only talks there)
#define INPUT_RECORD_SIZE 0x188     // +0 history index, +2 held, +4 pressed, +6 released, +8 64 pressed words, +0x88 64 frame stamps
#define SCRATCH_SIZE 0x40
#define FN_PLAYER_EVENT 0x08010DDE  // player class +0x24: (kind, info) — kind 0 is an attack: overlap test, damage, knockback
#define DEFAULT_DELAY 3
#define MATCH_BUTTONS (BF_BTN_A | BF_BTN_RIGHT | BF_BTN_LEFT | BF_BTN_UP | BF_BTN_DOWN | BF_BTN_R) // what a fighter may press: B (the chosen skill: a teleport, a transformation) and L, Start, Select stay out
#define HASH_EVERY 20
#define RING 65536

enum { PK_HELLO = 1, PK_STATE_BEGIN, PK_STATE_DATA, PK_READY, PK_START, PK_INPUT, PK_HASH, PK_END, PK_PING, PK_PONG };
enum { ST_IDLE, ST_CODE, ST_CONNECTING, ST_OPENING, ST_WAIT_PEER, ST_HELLO, ST_PREP, ST_WAIT_READY, ST_RECV_STATE, ST_WAIT_START, ST_MATCH, ST_OVER };

struct NpHello { uint32_t version, stateSize; uint8_t who, pad[3]; uint8_t record[0x68]; char name[16]; };
struct NpStart { uint32_t p1, p2, p2in; uint8_t who1, who2, delay, shown2; };
struct NpInput { uint32_t frame; uint16_t keys, pad; };
struct NpHash { uint32_t frame, hash; };

static int st = ST_IDLE, host, sock = -1, cancelled, debug;
static int randomMatch; // paired with whoever is waiting at the relay (the relay says who hosts) rather than by room code
static unsigned stateSince, overUntil, noteUntil, holdFrames, lastGameFrame;
static char note[64], serverHost[128], serverPort[16];
static uint32_t roomCode;
static struct NpHello peer;
static int haveHello, sentHello;
static unsigned pingSent, pings, rttFrames; // the host's round-trip measurement (in frames of the 60 Hz loop)
// the match
static uint32_t p1, p2, p2in, scratch; // scratch: a copy of an attack handed to the other fighter (right after p2's button record)
static int localTest;                   // NETPLAY_LOCALTEST=1: a match on this machine alone, the second fighter driven by NETPLAY_P2INPUT (tests)
static int who1 = -1, who2 = -1, delay = DEFAULT_DELAY, shown1, shown2; // shown: the characters named on the bars (a mirror match plays the guest's copy from the spare record)
static uint32_t matchFrame;
static uint16_t localKeys[RING], remoteKeys[RING];
static uint8_t localHave[RING], remoteHave[RING];
static struct NpHash myHashes[64], theirHashes[64];
static int nMyHashes, nTheirHashes;
static void* returnState;
// the state going across
static uint8_t* stateBlob; static uint32_t stateRaw, stateComp, stateGot;
// the socket
static uint8_t rx[1 << 20], tx[1 << 20];
static size_t rxLen, txLen;
static char result[32];
// the Start menu's extra page, ONLINE, between Options and Journal
enum { NO_PAGE = -1, PAGE_STATUS, PAGE_INVENTORY, PAGE_EQUIP, PAGE_OPTIONS, PAGE_JOURNAL };
static int page = NO_PAGE;   // the page the game's Start menu is on, read off the picture each frame
static int onlinePage;       // the ONLINE page is up: drawn over the game's menu, which is given no buttons meanwhile
static int under = NO_PAGE;  // the game's page underneath it: Options (entered with R) or Journal (entered with L)
static int justOpened, menuLine, codeDigit;
static int codeDigits[4];
static uint32_t hidden, gateSeen, passKeys; // buttons kept from the game until let go; buttons as of the last frame; a press handed on to the game's menu
static int passing;                         // frames of passKeys still to give the game
static int closeTries, settled;             // the host shutting its menu before the arena is snapshotted
static unsigned closeAt;
static uint32_t prevX1, prevY1, prevX2, prevY2;

int bfNetplayActive(void) { return st != ST_IDLE; }
static void openPage(int from, uint32_t key);

static void say(const char* s) { snprintf(note, sizeof(note), "%s", s); noteUntil = modFrame() + 180; }

// ---- the socket ---------------------------------------------------------------------
static void closeSock(void) { if (sock >= 0) { close(sock); sock = -1; } rxLen = txLen = 0; }

static int openSock(void) {
	struct addrinfo hints, *res = NULL, *ai;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(serverHost, serverPort, &hints, &res) || !res) {
		return 0;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		int one = 1;
		sock = socket(ai->ai_family, SOCK_STREAM, 0);
		if (sock < 0) {
			continue;
		}
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0 || errno == EINPROGRESS) {
			break;
		}
		close(sock);
		sock = -1;
	}
	freeaddrinfo(res);
	return sock >= 0;
}

static int sockConnected(void) {
	int err = 0; socklen_t len = sizeof(err);
	fd_set w; struct timeval tv = { 0, 0 };
	FD_ZERO(&w); FD_SET(sock, &w);
	if (select(sock + 1, NULL, &w, NULL, &tv) <= 0) {
		return 0;
	}
	getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
	return err == 0 ? 1 : -1;
}

static int sockDead;
static void pump(void) {
	if (sock < 0) {
		return;
	}
	while (txLen) {
		ssize_t n = send(sock, tx, txLen, MSG_NOSIGNAL);
		if (n > 0) {
			memmove(tx, tx + n, txLen - n);
			txLen -= n;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		} else {
			sockDead = 1;
			return;
		}
	}
	for (;;) {
		ssize_t n = recv(sock, rx + rxLen, sizeof(rx) - rxLen, 0);
		if (n > 0) {
			rxLen += n;
			if (rxLen == sizeof(rx)) {
				break;
			}
		} else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
			sockDead = 1;
			return;
		} else {
			break;
		}
	}
}

static void sendRaw(const void* data, size_t len) {
	if (txLen + len > sizeof(tx)) {
		sockDead = 1;
		return;
	}
	memcpy(tx + txLen, data, len);
	txLen += len;
}

static void sendPacket(int type, const void* payload, size_t len) {
	uint8_t head[4] = { (uint8_t) type, 0, (uint8_t) len, (uint8_t) (len >> 8) };
	sendRaw(head, 4);
	if (len) {
		sendRaw(payload, len);
	}
}

// ---- the game's button record, filled the way the game's own poll does ------------
static void pollInto(uint32_t rec, uint16_t keys) {
	uint16_t held = 0xFC00 | keys, prev = modRead16(rec + 2), changed = (uint16_t) (prev ^ held);
	uint16_t pressed = held & changed, released = prev & changed;
	modWrite16(rec + 4, pressed);
	modWrite16(rec + 6, released);
	modWrite16(rec + 2, held);
	if (pressed) {
		uint8_t i = modRead8(rec);
		modWrite16(rec + 8 + i * 2, pressed);
		modWrite32(rec + 0x88 + i * 4, modRead32(RAM_FRAME_COUNTER));
		i = (uint8_t) (i + 1);
		if (i >= 0x40) {
			i = 0;
		}
		modWrite8(rec, i);
	}
}

static int onRecordGetter(uint32_t* r) {
	if (st >= ST_WAIT_READY && p2 && r[0] == p2) {
		r[0] = BF_RAM_CHARS + (uint32_t) who2 * sizeof(struct BfChar);
		return MOD_RETURN;
	}
	return MOD_CONTINUE;
}

static int onInputGetter(uint32_t* r) {
	if (st >= ST_WAIT_READY && p2 && r[0] == p2) {
		static unsigned n;
		if (debug && n++ % 300 == 0) {
			modLog("netplay: p2 asked for its buttons (%u times so far) from %08X at match frame %u", n, r[14], matchFrame);
		}
		r[0] = p2in;
		return MOD_RETURN;
	}
	return MOD_CONTINUE;
}

// A player's punch is broadcast to every object in the area as an attack of
// "type 1", which the player's own handler ignores (so he never hits himself).
// In a match the other fighter is a player object too: he gets a copy of the
// attack typed as an enemy's (type 0), which the handler takes, and the game's
// own overlap test, damage, knockback and sound follow.
static int onPlayerEvent(uint32_t* r) {
	uint32_t info = r[2], attacker;
	if (st != ST_MATCH || r[1] != 0 || (r[0] != p1 && r[0] != p2)) {
		return MOD_CONTINUE;
	}
	attacker = modRead32(info + 0x20);
	if (debug) {
		uint32_t def = modRead32(r[0] + BF_ACTOR_SPRITE_DEF), box = def + 0x10 + modRead8(r[0] + 0x10) * 16;
		modLog("netplay: attack on %s: type %u box (%d,%d)-(%d,%d) dir %u from %s %08X (class %08X) (lr %08X); victim at (%d,%d) dir %u body box +(%d,%d)-(%d,%d) hittable %08X", r[0] == p1 ? "p1" : "p2", modRead8(info), (int) modRead32(info + 4), (int) modRead32(info + 8), (int) modRead32(info + 12), (int) modRead32(info + 16), modRead8(info + 0x14), attacker == p1 ? "p1" : attacker == p2 ? "p2" : "?", attacker, (attacker >> 24) == 2 || (attacker >> 24) == 3 ? modRead32(attacker) : 0, r[14],
		       (int) modRead32(r[0] + BF_ACTOR_WORLD_X) >> 8, (int) modRead32(r[0] + BF_ACTOR_WORLD_Y) >> 8, modRead8(r[0] + 0x10), (int) modRead32(box), (int) modRead32(box + 4), (int) modRead32(box + 8), (int) modRead32(box + 12), modRead32(r[0] + 0x1CC));
	}
	if (modRead8(info) != 1 || attacker == r[0] || (attacker != p1 && attacker != p2)) {
		return MOD_CONTINUE;
	}
	memcpy(modRam(scratch), modRam(info), SCRATCH_SIZE);
	modWrite8(scratch, 0);
	r[2] = scratch;
	return MOD_CONTINUE;
}

// Who in the area answers to this? (0x08011C70: the player asks before acting on A:
// a save pad, a person to talk to, a sign.) In a match A is a punch and nothing else.
// A in a match is a punch and nothing else. Three things in the game would make it
// something else, and each freezes both fighters with a box: the player's own ask
// of the area before he acts on A (0x08011C70, kind 3: a person to talk to, a
// sign), his check of the triggers under his feet (0x080127AE), and the world's
// own scan of those triggers every frame (0x080126D4: the save pad's prompt comes
// from there, on the game's global buttons, which are the host's). All three are
// answered "nothing" while a match is on.
#define QUERY_INTERACT 3
#define FN_TRIGGER_UNDERFOOT 0x080127AE // (the area's trigger list, player) -> the trigger acted on, or 0
#define FN_TRIGGER_SCAN 0x080126D4      // (the area's trigger list, player): the world's per-frame check
static int onQuery(uint32_t* r) {
	if (st == ST_MATCH && r[1] == QUERY_INTERACT) {
		r[0] = 0; // nobody answers
		return MOD_RETURN;
	}
	return MOD_CONTINUE;
}
static int onTrigger(uint32_t* r) {
	if (st == ST_MATCH) {
		r[0] = 0;
		return MOD_RETURN;
	}
	return MOD_CONTINUE;
}
static int onApplyHit(uint32_t* r) {
	if (st == ST_MATCH && debug) {
		modLog("netplay: %s takes a hit: damage %d (%08X), hp %d", r[0] == p1 ? "p1" : r[0] == p2 ? "p2" : "?", (int) modRead32(r[1] + 0x18) >> 10, modRead32(r[1] + 0x18), (int16_t) bfChar(r[0] == p2 ? who2 : who1)->hp);
	}
	return MOD_CONTINUE;
}

// ---- the match ---------------------------------------------------------------------
static void finish(const char* why) {
	if (st == ST_OVER) {
		return;
	}
	snprintf(result, sizeof(result), "%s", why);
	st = ST_OVER;
	overUntil = modFrame() + 240;
	if (!localTest) {
		sendPacket(PK_END, why, strlen(why) + 1);
		pump();
	}
	modLog("netplay: match over at frame %u: %s", matchFrame, why);
}

static void leave(const char* text) {
	int wasMatch = st == ST_MATCH || st == ST_OVER;
	modLog("netplay: %s: %s", wasMatch ? "back from the match" : "out of the lobby", text ? text : "(no message)");
	closeSock();
	if (returnState) {
		modStateLoad(returnState);
		free(returnState);
		returnState = NULL;
	}
	free(stateBlob);
	stateBlob = NULL;
	p1 = p2 = p2in = 0;
	who1 = who2 = -1;
	haveHello = sentHello = sockDead = 0;
	st = ST_IDLE;
	if (under != NO_PAGE && modRead32(BF_RAM_MENU_OPEN) == 1) {
		onlinePage = 1; // back on the ONLINE page: the game's menu is still up underneath (or is again, with the restore)
		page = under;
		hidden |= gateSeen;
	} else {
		onlinePage = 0;
		under = NO_PAGE;
	}
	(void) wasMatch;
	if (text) {
		say(text);
	}
}

static uint32_t fnv(uint32_t h, const void* data, size_t len) {
	const uint8_t* p = data;
	while (len--) {
		h = (h ^ *p++) * 16777619u;
	}
	return h;
}

static uint32_t stateHash(void) {
	uint32_t v[12] = { modRead32(p1 + BF_ACTOR_WORLD_X), modRead32(p1 + BF_ACTOR_WORLD_Y), modRead32(p2 + BF_ACTOR_WORLD_X), modRead32(p2 + BF_ACTOR_WORLD_Y),
	                   modRead32(p1 + 0x10), modRead32(p2 + 0x10), bfChar(who1)->hp, bfChar(who2)->hp, bfChar(who1)->ep, bfChar(who2)->ep, modRead32(p2in + 2), modRead32(RAM_FRAME_COUNTER) };
	return fnv(2166136261u, v, sizeof(v));
}

static void checkHashes(void) {
	int i, j;
	for (i = 0; i < nTheirHashes; ++i) {
		for (j = 0; j < nMyHashes; ++j) {
			if (myHashes[j].frame == theirHashes[i].frame) {
				if (myHashes[j].hash != theirHashes[i].hash) {
					modLog("netplay: DESYNC at frame %u (%08X here, %08X there)", myHashes[j].frame, myHashes[j].hash, theirHashes[i].hash);
					finish("DESYNC");
				}
				theirHashes[i] = theirHashes[--nTheirHashes];
				--i;
				break;
			}
		}
	}
}

// Neither fighter may leave the other's screen (the camera follows the host's
// fighter on both machines): whoever stepped too far is put back
static void keepTogether(void) {
	int x1 = (int) modRead32(p1 + BF_ACTOR_WORLD_X), y1 = (int) modRead32(p1 + BF_ACTOR_WORLD_Y);
	int x2 = (int) modRead32(p2 + BF_ACTOR_WORLD_X), y2 = (int) modRead32(p2 + BF_ACTOR_WORLD_Y);
	int far = abs(x1 - x2) > 100 * 256 || abs(y1 - y2) > 56 * 256;
	if (far) {
		int moved1 = (uint32_t) x1 != prevX1 || (uint32_t) y1 != prevY1, moved2 = (uint32_t) x2 != prevX2 || (uint32_t) y2 != prevY2;
		if (moved1 || !moved2) {
			modWrite32(p1 + BF_ACTOR_WORLD_X, prevX1);
			modWrite32(p1 + BF_ACTOR_WORLD_Y, prevY1);
		}
		if (moved2 || !moved1) {
			modWrite32(p2 + BF_ACTOR_WORLD_X, prevX2);
			modWrite32(p2 + BF_ACTOR_WORLD_Y, prevY2);
		}
	}
	prevX1 = modRead32(p1 + BF_ACTOR_WORLD_X); prevY1 = modRead32(p1 + BF_ACTOR_WORLD_Y);
	prevX2 = modRead32(p2 + BF_ACTOR_WORLD_X); prevY2 = modRead32(p2 + BF_ACTOR_WORLD_Y);
}

static void afterFrame(void) {
	keepTogether();
	if (matchFrame % HASH_EVERY == 0) {
		struct NpHash hsh = { matchFrame, stateHash() };
		if (debug) {
			modLog("netplay: f%u p1 (%d,%d) %08X hp %d | p2 (%d,%d) %08X hp %d | hash %08X", matchFrame, (int) modRead32(p1 + BF_ACTOR_WORLD_X) >> 8, (int) modRead32(p1 + BF_ACTOR_WORLD_Y) >> 8, modRead32(p1 + 0x10),
			       (int16_t) bfChar(who1)->hp, (int) modRead32(p2 + BF_ACTOR_WORLD_X) >> 8, (int) modRead32(p2 + BF_ACTOR_WORLD_Y) >> 8, modRead32(p2 + 0x10), (int16_t) bfChar(who2)->hp, hsh.hash);
		}
		if (!localTest) {
			if (nMyHashes == 64) {
				memmove(myHashes, myHashes + 1, sizeof(myHashes) - sizeof(myHashes[0]));
				--nMyHashes;
			}
			myHashes[nMyHashes++] = hsh;
			sendPacket(PK_HASH, &hsh, sizeof(hsh));
			checkHashes();
		}
	}
	if (st != ST_MATCH) {
		return;
	}
	{
		int hp1 = (int16_t) bfChar(who1)->hp, hp2 = (int16_t) bfChar(who2)->hp;
		if (hp1 <= 0 || hp2 <= 0) {
			int iWon = host ? hp2 <= 0 && hp1 > 0 : hp1 <= 0 && hp2 > 0;
			finish(hp1 <= 0 && hp2 <= 0 ? "DRAW" : iWon ? "YOU WIN" : "YOU LOSE");
		}
	}
}

// NETPLAY_P2INPUT, in the runtime's --input letters (A B E S R L U D, l r the shoulders, N none; "L:30,A:3,N:10")
static uint16_t scriptKeys(uint32_t frame) {
	const char* p = getenv("NETPLAY_P2INPUT");
	uint32_t at = 0;
	while (p && *p) {
		char c = *p++;
		int n = 0;
		uint16_t k = c == 'A' ? BF_BTN_A : c == 'B' ? BF_BTN_B : c == 'E' ? BF_BTN_SELECT : c == 'S' ? BF_BTN_START : c == 'R' ? BF_BTN_RIGHT : c == 'L' ? BF_BTN_LEFT : c == 'U' ? BF_BTN_UP : c == 'D' ? BF_BTN_DOWN : c == 'r' ? BF_BTN_R : c == 'l' ? BF_BTN_L : 0;
		if (*p == ':') {
			++p;
			n = atoi(p);
			while (*p && *p != ',') {
				++p;
			}
		}
		if (*p == ',') {
			++p;
		}
		if (frame >= at && frame < at + (uint32_t) n) {
			return k;
		}
		at += n;
	}
	return 0;
}

// The runtime asks before each game frame: run it with these buttons, or hold?
static int gate(uint32_t* keys) {
	uint16_t mine, k1, k2;
	pump();
	if (st == ST_IDLE) {
		uint32_t pressed = *keys & ~gateSeen;
		gateSeen = *keys;
		hidden &= *keys;
		if (!onlinePage && !passing && !localTest) {
			// On Options, R is kept from the game and opens the ONLINE page instead; on Journal, L
			if (page == PAGE_OPTIONS && (pressed & BF_BTN_R)) {
				openPage(PAGE_OPTIONS, BF_BTN_R);
			} else if (page == PAGE_JOURNAL && (pressed & BF_BTN_L)) {
				openPage(PAGE_JOURNAL, BF_BTN_L);
			}
		}
		if (passing) {
			--passing;
			*keys = passKeys;
		} else if (onlinePage) {
			*keys = 0;
		} else {
			*keys &= ~hidden;
		}
		return 1;
	}
	if (st == ST_CODE || st == ST_CONNECTING || st == ST_OPENING || st == ST_WAIT_PEER || st == ST_HELLO || st == ST_PREP || st == ST_RECV_STATE) {
		// The lobby. With the Start menu up underneath, the game keeps running (the music plays on) and is given
		// no buttons; the host shuts its menu with B before the arena is snapshotted (prepare). Without a menu
		// (the test hooks) the game is held instead.
		if (modRead32(BF_RAM_MENU_OPEN) != 1 && st != ST_PREP) {
			return 0;
		}
		if (passing) {
			--passing;
			*keys = passKeys;
		} else {
			*keys = 0;
		}
		return 1;
	}
	if (st != ST_MATCH) {
		return 0;
	}
	if (localTest) {
		uint16_t k = (uint16_t) (scriptKeys(matchFrame) & MATCH_BUTTONS);
		*keys &= MATCH_BUTTONS;
		pollInto(p2in, k);
		if (debug && (k || matchFrame % 20 == 0)) {
			modLog("netplay: gate f%u: p2 keys %03X -> record %08X: %02X held %04X pressed %04X released %04X", matchFrame, k, p2in, modRead8(p2in), modRead16(p2in + 2), modRead16(p2in + 4), modRead16(p2in + 6));
		}
		return 1;
	}
	if (!remoteHave[matchFrame & (RING - 1)]) {
		holdFrames = 1;
		return 0;
	}
	mine = (uint16_t) (*keys & MATCH_BUTTONS);
	{
		uint32_t f = matchFrame + delay, i = f & (RING - 1);
		if (!localHave[i]) {
			struct NpInput in = { f, mine, 0 };
			localKeys[i] = mine;
			localHave[i] = 1;
			localHave[(f + RING / 2) & (RING - 1)] = 0;
			sendPacket(PK_INPUT, &in, sizeof(in));
		}
	}
	k1 = host ? localKeys[matchFrame & (RING - 1)] : remoteKeys[matchFrame & (RING - 1)];
	k2 = host ? remoteKeys[matchFrame & (RING - 1)] : localKeys[matchFrame & (RING - 1)];
	if (debug && (k1 || k2 || mine) && matchFrame < 400) {
		modLog("netplay: gate f%u: mine %03X (for f%u), p1 gets %03X, p2 gets %03X", matchFrame, mine, matchFrame + delay, k1, k2);
	}
	*keys = k1;
	pollInto(p2in, k2);
	if (debug && k2 && matchFrame < 400) {
		modLog("netplay: gate f%u: p2 record %08X: %02X held %04X pressed %04X | p2 state %08X", matchFrame, p2in, modRead8(p2in), modRead16(p2in + 2), modRead16(p2in + 4), modRead32(p2 + 0x10));
	}

	return 1;
}

static void startMatch(const struct NpStart* s) {
	uint32_t f;
	p1 = s->p1; p2 = s->p2; p2in = s->p2in; scratch = p2in + INPUT_RECORD_SIZE; who1 = s->who1; who2 = s->who2; delay = s->delay;
	shown1 = who1;
	shown2 = s->shown2;
	memset(localHave, 0, sizeof(localHave));
	memset(remoteHave, 0, sizeof(remoteHave));
	for (f = 0; f < (uint32_t) delay; ++f) {
		localKeys[f] = remoteKeys[f] = 0;
		localHave[f] = remoteHave[f] = 1;
	}
	matchFrame = 0;
	nMyHashes = nTheirHashes = 0;
	lastGameFrame = modGameFrame();
	prevX1 = modRead32(p1 + BF_ACTOR_WORLD_X); prevY1 = modRead32(p1 + BF_ACTOR_WORLD_Y);
	prevX2 = modRead32(p2 + BF_ACTOR_WORLD_X); prevY2 = modRead32(p2 + BF_ACTOR_WORLD_Y);
	st = ST_MATCH;
	modLog("netplay: match started: p1 %08X (%d) p2 %08X (%d) buttons at %08X, delay %d", p1, who1, p2, who2, p2in, delay);
}

// The second fighter: the game's own player object, built for the guest's
// character, placed to the right of the host's and facing him, with its own
// button record. Needs modCanCall().
static int spawnSecond(void) {
	int form = bfChar(who2)->form;
	uint32_t def = modRead32(BF_ROM_SPRITE_TABLE + modRead16(BF_ROM_FORMS + form * 24 + 14) * 4);
	int16_t* pos = (int16_t*) modRom(BF_ROM_SCRATCH + 0x240);
	uint8_t* current = modRam(BF_RAM_SAVE);
	pos[0] = (int16_t) (((int) modRead32(p1 + BF_ACTOR_WORLD_X) >> 8) + 48);
	pos[1] = (int16_t) ((int) modRead32(p1 + BF_ACTOR_WORLD_Y) >> 8);
	p2in = modCall(FN_ALLOC | 1, INPUT_RECORD_SIZE + SCRATCH_SIZE, 0, 0, 0);
	if (!p2in) {
		return 0;
	}
	scratch = p2in + INPUT_RECORD_SIZE;
	memset(modRam(p2in), 0, INPUT_RECORD_SIZE + SCRATCH_SIZE);
	modWrite16(p2in + 2, 0xFC00);
	*current = (uint8_t) who2; // the constructor picks the behaviour by the current character's form
	p2 = modCall(FN_PLAYER_CTOR | 1, 0, def, 0x08000000 + BF_ROM_SCRATCH + 0x240, 1);
	*current = (uint8_t) who1;
	modWrite32(BF_RAM_PLAYER_PTR, p1); // (the constructor made itself the player)
	if (!p2) {
		return 0;
	}
	modCall(FN_AREA_ADD | 1, modRead32(ROM_ADDRESS_TABLE + 0x14), p2, 0, 0);
	if (modRead8(modRead32(p1 + BF_ACTOR_SPRITE_DEF) + 3) == 4) { // facing each other (the actor's direction byte: 2 left, 3 right; only a four-way sprite has those)
		modWrite8(p1 + 0x10, 3);
	}
	if (modRead8(def + 3) == 4) {
		modWrite8(p2 + 0x10, 2);
	}
	modWrite32(RAM_FIGHTING_ALLOWED, 1); // punches land even in a town
	return 1;
}

// The host builds the second fighter, snapshots the machine and sends it over
static void prepare(void) {
	uint32_t player = modRead32(BF_RAM_PLAYER_PTR);
	size_t size = modStateSize();
	uLongf comp;
	uint8_t* raw;
	if (!returnState) { // taken with the Start menu still up: that is where both sides come back to
		returnState = malloc(size);
		if (!returnState || !modStateSave(returnState)) {
			leave("COULD NOT SNAPSHOT THE GAME");
			return;
		}
	}
	if (modRead32(BF_RAM_MENU_OPEN) == 1) {
		// the Start menu is up (the page lives on it): shut it with B, again if one press is not enough
		if (!passing && modFrame() - closeAt > 40) {
			if (++closeTries > 6) {
				leave("COULD NOT CLOSE THE MENU");
				return;
			}
			passKeys = BF_BTN_B;
			passing = 3;
			closeAt = modFrame();
			onlinePage = 0;
			modLog("netplay: shutting the Start menu (press %d)", closeTries);
		}
		settled = 0;
		return;
	}
	if ((player >> 24) != 2 || !modCanCall() || ++settled < (getenv("NETPLAY_SETTLE") ? atoi(getenv("NETPLAY_SETTLE")) : 30)) {
		return; // the world settles for a moment after the menu (the game puts the area's picture back)
	}
	who1 = bfCurrentChar();
	who2 = peer.who;
	if (who2 == who1 || who2 >= BF_CHARACTERS) {
		who2 = BF_NINTH; // a mirror match: the guest's copy lives in the spare record
	}
	memcpy(bfChar(who2), peer.record, sizeof(struct BfChar));
	bfChar(who1)->hp = bfChar(who1)->maxHp;
	bfChar(who1)->ep = bfChar(who1)->maxEp;
	bfChar(who2)->hp = bfChar(who2)->maxHp;
	bfChar(who2)->ep = bfChar(who2)->maxEp;
	p1 = player;
	if (!spawnSecond()) {
		leave("THE GAME IS OUT OF MEMORY");
		return;
	}
	st = ST_WAIT_READY; // (the getters answer for p2 from here on)
	raw = malloc(size);
	comp = compressBound(size);
	stateBlob = malloc(comp);
	if (!raw || !stateBlob || !modStateSave(raw) || compress2(stateBlob, &comp, raw, size, 6) != Z_OK) {
		free(raw);
		leave("COULD NOT SNAPSHOT THE GAME");
		return;
	}
	free(raw);
	{
		uint32_t head[2] = { (uint32_t) size, (uint32_t) comp }, off = 0;
		sendPacket(PK_STATE_BEGIN, head, sizeof(head));
		while (off < comp) {
			uint32_t n = comp - off > 32768 ? 32768 : (uint32_t) (comp - off);
			sendPacket(PK_STATE_DATA, stateBlob + off, n);
			off += n;
		}
	}
	free(stateBlob);
	stateBlob = NULL;
	modLog("netplay: host prepared: p1 %08X (%d) at (%d,%d) p2 %08X (%d), state %u bytes (%lu packed), game frame %u", p1, who1, (int) modRead32(p1 + BF_ACTOR_WORLD_X) >> 8, (int) modRead32(p1 + BF_ACTOR_WORLD_Y) >> 8, p2, who2, (unsigned) size, (unsigned long) comp, modGameFrame());
}

static void sendHello(void) {
	struct NpHello h;
	memset(&h, 0, sizeof(h));
	h.version = NP_VERSION;
	h.stateSize = (uint32_t) modStateSize();
	h.who = (uint8_t) bfCurrentChar();
	memcpy(h.record, bfChar(h.who), sizeof(h.record));
	snprintf(h.name, sizeof(h.name), "%s", getenv("NETPLAY_NAME") ? getenv("NETPLAY_NAME") : "");
	sendPacket(PK_HELLO, &h, sizeof(h));
	sentHello = 1;
}

static void onPacket(int type, const uint8_t* data, size_t len) {
	switch (type) {
	case PK_HELLO:
		if (len != sizeof(peer)) {
			leave("THE OTHER SIDE RUNS A DIFFERENT VERSION");
			return;
		}
		memcpy(&peer, data, sizeof(peer));
		if (peer.version != NP_VERSION || peer.stateSize != modStateSize()) {
			leave("THE OTHER SIDE RUNS A DIFFERENT VERSION");
			return;
		}
		haveHello = 1;
		if (host) {
			uint32_t now = modFrame();
			pings = 0;
			rttFrames = 0;
			pingSent = now;
			sendPacket(PK_PING, &now, sizeof(now)); // three trips measured before the arena goes over
		} else {
			st = ST_RECV_STATE;
		}
		break;
	case PK_PING:
		if (len == 4) {
			sendPacket(PK_PONG, data, 4);
		}
		break;
	case PK_PONG:
		if (host && len == 4 && st == ST_HELLO) {
			uint32_t sent;
			memcpy(&sent, data, 4);
			rttFrames += modFrame() - sent;
			if (++pings < 3) {
				uint32_t now = modFrame();
				sendPacket(PK_PING, &now, sizeof(now));
			} else {
				unsigned rtt = rttFrames / pings;
				delay = (int) (rtt / 2 + 2); // half the trip, plus a frame of slack and one for the send itself
				if (delay < DEFAULT_DELAY) {
					delay = DEFAULT_DELAY;
				}
				if (delay > 10) {
					delay = 10;
				}
				modLog("netplay: round trip %u frames: input delay %d frames", rtt, delay);
				st = ST_PREP;
			}
		}
		break;
	case PK_STATE_BEGIN:
		if (host || len != 8) {
			return;
		}
		stateRaw = ((const uint32_t*) data)[0];
		stateComp = ((const uint32_t*) data)[1];
		stateGot = 0;
		if (stateRaw != modStateSize() || stateComp > (16u << 20)) {
			leave("THE OTHER SIDE RUNS A DIFFERENT VERSION");
			return;
		}
		free(stateBlob);
		stateBlob = malloc(stateComp);
		if (!returnState) {
			returnState = malloc(stateRaw);
			if (!returnState || !modStateSave(returnState)) {
				leave("COULD NOT SNAPSHOT THE GAME");
				return;
			}
		}
		break;
	case PK_STATE_DATA:
		if (host || !stateBlob || stateGot + len > stateComp) {
			return;
		}
		memcpy(stateBlob + stateGot, data, len);
		stateGot += len;
		if (stateGot == stateComp) {
			uint8_t* raw = malloc(stateRaw);
			uLongf rawLen = stateRaw;
			int ok = raw && uncompress(raw, &rawLen, stateBlob, stateComp) == Z_OK && rawLen == stateRaw && modStateLoad(raw);
			free(raw);
			free(stateBlob);
			stateBlob = NULL;
			if (!ok) {
				leave("COULD NOT TAKE THE HOST'S GAME");
				return;
			}
			st = ST_WAIT_START;
			sendPacket(PK_READY, NULL, 0);
		}
		break;
	case PK_READY:
		if (host && st == ST_WAIT_READY) {
			struct NpStart s = { p1, p2, p2in, (uint8_t) who1, (uint8_t) who2, (uint8_t) delay, peer.who };
			sendPacket(PK_START, &s, sizeof(s));
			startMatch(&s);
		}
		break;
	case PK_START:
		if (!host && st == ST_WAIT_START && len == sizeof(struct NpStart)) {
			struct NpStart s;
			memcpy(&s, data, sizeof(s));
			startMatch(&s);
		}
		break;
	case PK_INPUT:
		if (len == sizeof(struct NpInput)) {
			struct NpInput in;
			memcpy(&in, data, sizeof(in));
			remoteKeys[in.frame & (RING - 1)] = in.keys;
			remoteHave[in.frame & (RING - 1)] = 1;
			remoteHave[(in.frame + RING / 2) & (RING - 1)] = 0;
		}
		break;
	case PK_HASH:
		if (len == sizeof(struct NpHash) && nTheirHashes < 64) {
			memcpy(&theirHashes[nTheirHashes++], data, sizeof(struct NpHash));
			checkHashes();
		}
		break;
	case PK_END:
		if (st == ST_MATCH) {
			char why[32];
			snprintf(why, sizeof(why), "%.*s", (int) (len < 31 ? len : 31), (const char*) data);
			finish(!strcmp(why, "YOU WIN") ? "YOU LOSE" : !strcmp(why, "YOU LOSE") ? "YOU WIN" : why);
		} else if (st != ST_OVER && st != ST_IDLE) {
			leave("THE OTHER SIDE LEFT");
		}
		break;
	}
}

static void parsePackets(void) {
	while (rxLen >= 4) {
		size_t len = rx[2] | (rx[3] << 8);
		if (rxLen < 4 + len) {
			break;
		}
		onPacket(rx[0], rx + 4, len);
		memmove(rx, rx + 4 + len, rxLen - 4 - len);
		rxLen -= 4 + len;
		if (st == ST_IDLE) {
			break;
		}
	}
}

// ---- the lobby ------------------------------------------------------------------------
static void loadConfig(void) {
	char path[1200], key[32], value[120];
	FILE* f;
	const char* env = getenv("NETPLAY_SERVER");
	serverHost[0] = 0;
	snprintf(serverPort, sizeof(serverPort), "7777");
	snprintf(path, sizeof(path), "%s/buusfury.netplay", modSaveDir());
	f = fopen(path, "r");
	while (f && fscanf(f, "%31s %119s", key, value) == 2) {
		if (!strcmp(key, "server")) {
			snprintf(serverHost, sizeof(serverHost), "%s", value);
		}
	}
	if (f) {
		fclose(f);
	}
	if (env) {
		snprintf(serverHost, sizeof(serverHost), "%s", env);
	}
	{
		char* colon = strrchr(serverHost, ':');
		if (colon) {
			*colon = 0;
			snprintf(serverPort, sizeof(serverPort), "%s", colon + 1);
		}
	}
}

// mode: 0 join the room `code`, 1 host a room, 2 random battle (paired by the relay with anyone waiting)
static void begin(int mode, uint32_t code) {
	loadConfig();
	if (!serverHost[0]) {
		say("NO SERVER SET (buusfury.netplay)");
		return;
	}
	host = mode == 1;
	randomMatch = mode == 2;
	roomCode = code;
	cancelled = 0;
	closeTries = settled = 0;
	if (!openSock()) {
		say("CANNOT REACH THE SERVER");
		return;
	}
	st = ST_CONNECTING;
	stateSince = modFrame();
	modLog("netplay: %s via %s:%s", mode == 2 ? "looking for a random battle" : mode == 1 ? "hosting" : "joining", serverHost, serverPort);
}

static void opening(void) {
	// the relay's opening exchange: "BFNP" ver cmd code -> "BFNP" status code
	if (st == ST_CONNECTING) {
		int c = sockConnected();
		if (c < 0 || modFrame() - stateSince > 600) {
			leave("CANNOT REACH THE SERVER");
		} else if (c > 0) {
			uint8_t req[10] = { 'B', 'F', 'N', 'P', NP_VERSION, (uint8_t) (randomMatch ? 3 : host ? 1 : 2) };
			memcpy(req + 6, &roomCode, 4);
			sendRaw(req, 10);
			st = ST_OPENING;
			stateSince = modFrame();
		}
		return;
	}
	if (rxLen >= 9 && (st == ST_OPENING || st == ST_WAIT_PEER)) {
		uint8_t status = rx[4];
		if (memcmp(rx, "BFNP", 4)) {
			leave("THAT IS NOT THE RELAY SERVER");
			return;
		}
		memcpy(&roomCode, rx + 5, 4);
		memmove(rx, rx + 9, rxLen - 9);
		rxLen -= 9;
		if (status == 1) {
			leave("NO ROOM WITH THAT CODE");
		} else if (status == 2) {
			leave("THAT ROOM IS FULL");
		} else if (status == 4) {
			host = 1; // queued for a random battle: the next answer means an opponent came, and this side hosts
			st = ST_WAIT_PEER;
		} else if (status != 0) {
			leave("THE SERVER REFUSED");
		} else if (st == ST_OPENING && host) {
			st = ST_WAIT_PEER; // the code is ours; the next reply means someone joined
		} else {
			st = ST_HELLO;
			sendHello();
		}
	} else if (st == ST_OPENING && modFrame() - stateSince > 600) {
		leave("THE SERVER DID NOT ANSWER");
	}
}

// ---- the ONLINE page of the Start menu ------------------------------------------------------
// The game's Start menu knows nothing about it. Which page the menu is on is read
// off the picture (the page's name in yellow across the top). On Options a press of
// R is kept from the game and this page is drawn over the menu instead; R again
// hands the press on, so the game moves to Journal. From Journal, L works the same
// way back. The game is not held while the page is up: it runs its own menu
// underneath, given no buttons, so the music plays on.
static int menuPage(void) {
	static const uint32_t names[5] = { 0x818DA662, 0x24EA8B7A, 0xA52C16C8, 0x4FD9D5B7, 0xCAB8FE43 }; // Status, Inventory, Equip, Options, Journal
	const uint32_t* screen = modScreen();
	uint32_t h = 2166136261u;
	int x, y, i;
	if (modRead32(BF_RAM_MENU_OPEN) != 1) {
		return NO_PAGE;
	}
	for (y = 0; y < 24; ++y) {
		for (x = 0; x < 240; ++x) {
			if ((screen[y * 240 + x] & 0xFFFFFF) == 0x00FFFF) {
				h = (h ^ (uint32_t) x) * 16777619u;
				h = (h ^ (uint32_t) y) * 16777619u;
			}
		}
	}
	for (i = 0; i < 5; ++i) {
		if (h == names[i]) {
			return i;
		}
	}
	return NO_PAGE;
}
static void openPage(int from, uint32_t key) {
	under = from;
	hidden |= key;
	onlinePage = 1;
	justOpened = 1;
	menuLine = 0;
}
static void leavePage(uint32_t key, int pass) {
	if (pass) {
		passKeys = key;
		passing = 3;
	}
	hidden |= gateSeen | key; // whatever is held now was meant for the page, not the game
	onlinePage = 0;
	under = NO_PAGE;
	modLog("netplay: ONLINE page left with %s", key == BF_BTN_B ? "B" : key == BF_BTN_R ? "R" : "L");
}

// Drawn in the menu's own colours: its striped backdrop, yellow heading, green chosen line
static void stripes(int x, int y, int w, int h) {
	int i;
	for (i = y; i < y + h; ++i) {
		modRect(x, i, w, 1, (i & 1) ? 0x007300 : 0x000000);
	}
}
static void centred(int y, const char* text, uint32_t color, int scale) {
	modTextScale(scale);
	modText(120 - (int) strlen(text) * 2 * scale, y, color, "%s", text);
	modTextScale(1);
}
static void arrow(int x, int y) {
	static const uint32_t rows[8][9] = {
		{ 0xFF7300, 0, 0, 0, 0, 0, 0, 0, 0 }, { 0xFF7300, 0xFF7300, 0xFF7300, 0, 0, 0, 0, 0, 0 }, { 0xFF7300, 0xFFAD00, 0xFFAD00, 0xFF7300, 0xFF7300, 0, 0, 0, 0 },
		{ 0xFF7300, 0xFFAD00, 0xFFD600, 0xFFAD00, 0xFFAD00, 0xFF7300, 0xFF7300, 0, 0 }, { 0xFF7300, 0xFFAD00, 0xFFD600, 0xFFAD00, 0xFFAD00, 0xFF7300, 0xFF7300, 0, 0 },
		{ 0xFF7300, 0xFFAD00, 0xFFAD00, 0xFF7300, 0xFF7300, 0, 0, 0, 0 }, { 0xFF7300, 0xFF7300, 0xFF7300, 0, 0, 0, 0, 0, 0 }, { 0xFF7300, 0, 0, 0, 0, 0, 0, 0, 0 } };
	int r, c;
	for (r = 0; r < 8; ++r) {
		for (c = 0; c < 9; ++c) {
			if (rows[r][c]) {
				modRect(x + c, y + r, 1, 1, rows[r][c]);
			}
		}
	}
}
static const char* statusLine(void) {
	return st == ST_CONNECTING || st == ST_OPENING ? "CONNECTING..." : st == ST_WAIT_PEER ? (randomMatch ? "LOOKING FOR AN OPPONENT" : "WAITING FOR A CHALLENGER") : st == ST_HELLO ? (randomMatch ? "OPPONENT FOUND" : "CHALLENGER FOUND") : st == ST_PREP || st == ST_WAIT_READY ? "SENDING THE ARENA" : st == ST_RECV_STATE ? "RECEIVING THE ARENA" : "STARTING";
}
// The page over the game's menu: the heading with the pages either side, the box emptied and filled again
static void drawPage(void) {
	static const char* const lines[3] = { "RANDOM BATTLE", "HOST A BATTLE", "JOIN WITH CODE" };
	static const char* const about[3] = { "FIGHT WHOEVER IS WAITING AT THE SERVER", "GET A ROOM CODE TO GIVE A FRIEND", "TYPE THE CODE A FRIEND GAVE YOU" };
	char text[16];
	int i;
	stripes(30, 0, 175, 24);  // the heading (the L and R buttons either end stay)
	stripes(0, 27, 8, 122);   // the arrow the game draws beside its box
	stripes(11, 27, 218, 122); // the box's inside
	modTextPlain(1);
	modText(40, 14, 0xFFFFFF, "OPTIONS");
	centred(6, "ONLINE", 0x00FFFF, 2);
	modText(166, 14, 0xFFFFFF, "JOURNAL");
	if (st == ST_IDLE) {
		for (i = 0; i < 3; ++i) {
			int y = 34 + i * 26;
			modTextScale(2);
			modText(24, y, i == menuLine ? 0x00FF00 : 0xFFFFFF, "%s", lines[i]);
			modTextScale(1);
			modText(26, y + 13, 0xC0C0C0, "%s", about[i]);
			if (i == menuLine) {
				arrow(2, y + 2);
			}
		}
		if (serverHost[0]) {
			modText(16, 116, 0x808080, "SERVER: %s:%s", serverHost, serverPort);
		} else {
			const char* dir = modSaveDir();
			modText(16, 116, 0x4040FF, "NO SERVER SET. PUT  server host:port  IN");
			modText(16, 124, 0x4040FF, "%s%s/buusfury.netplay", strlen(dir) > 34 ? "..." : "", strlen(dir) > 34 ? dir + strlen(dir) - 34 : dir);
		}
		if (modFrame() < noteUntil) {
			centred(134, note, 0x00FF00, 2);
		}
	} else if (st == ST_CODE) {
		centred(36, "ROOM CODE", 0x00FFFF, 2);
		for (i = 0; i < 4; ++i) {
			modTextScale(2);
			modText(92 + i * 16, 66, i == codeDigit ? 0x00FF00 : 0xFFFFFF, "%d", codeDigits[i]);
			modTextScale(1);
			if (i == codeDigit) {
				modRect(92 + i * 16, 80, 8, 2, 0x00FF00);
			}
		}
		modText(16, 110, 0xC0C0C0, "UP/DOWN: DIGIT   LEFT/RIGHT: NEXT DIGIT");
		modText(16, 120, 0xC0C0C0, "A: JOIN   B: BACK");
	} else {
		centred(40, statusLine(), 0xFFFFFF, 2);
		if (!randomMatch && (st == ST_WAIT_PEER || (st == ST_HELLO && host))) {
			snprintf(text, sizeof(text), "%04u", roomCode);
			centred(66, "ROOM CODE", 0xC0C0C0, 1);
			centred(76, text, 0x00FFFF, 2);
			centred(96, "TELL YOUR FRIEND THIS CODE", 0xC0C0C0, 1);
		}
		if (st == ST_CONNECTING || st == ST_OPENING || st == ST_WAIT_PEER) {
			centred(124, "B: CANCEL", 0xC0C0C0, 1);
		}
	}
	modTextPlain(0);
}
// The same while there is no menu underneath (the moment between the host's menu shutting and the match)
static void drawStatusBox(void) {
	static const uint32_t top[6] = { 0xAD4A00, 0xFFFF4A, 0xFFFFFF, 0xFFD600, 0x004A00, 0x007300 };
	static const uint32_t bottom[6] = { 0x007300, 0x004A00, 0xFFD600, 0xFFFFFF, 0xFFFF4A, 0xAD4A00 };
	int i;
	stripes(48, 100, 144, 54);
	for (i = 0; i < 6; ++i) {
		modRect(48, 94 + i, 144, 1, top[i]);
		modRect(48, 154 + i, 144, 1, bottom[i]);
	}
	modTextPlain(1);
	centred(104, "ONLINE BATTLE", 0x00FFFF, 2);
	centred(124, statusLine(), 0xFFFFFF, 1);
	modTextPlain(0);
}

static void drawBar(int x, int y, int w, int hp, int maxHp, uint32_t color) {
	int fill = maxHp > 0 ? (w - 2) * (hp < 0 ? 0 : hp) / maxHp : 0;
	modRect(x, y, w, 7, 0x000000);
	modRect(x + 1, y + 1, fill, 5, color);
}

static const char* const charNames[BF_CHARACTERS] = { "GOKU", "GOHAN", "GOTEN", "TRUNKS", "VEGETA", "GOTENKS", "VEGITO", "GOGETA", "GUEST" };

static void draw(void) {
	unsigned f = modFrame();
	if (st == ST_MATCH || st == ST_OVER) {
		int hp1 = (int16_t) bfChar(who1)->hp, hp2 = (int16_t) bfChar(who2)->hp;
		const char* n1 = charNames[shown1 >= 0 && shown1 < BF_CHARACTERS ? shown1 : 8];
		const char* n2 = charNames[shown2 >= 0 && shown2 < BF_CHARACTERS ? shown2 : 8];
		modText(4, 20, host ? 0x80FF80 : 0xFFFFFF, "%s%s", n1, host ? " (YOU)" : "");
		drawBar(4, 27, 90, hp1, bfChar(who1)->maxHp, 0x40E040);
		modText(146, 20, host ? 0xFFFFFF : 0x80FF80, "%s%s", n2, host ? "" : " (YOU)");
		drawBar(146, 27, 90, hp2, bfChar(who2)->maxHp, 0x40E040);
		if (st == ST_OVER) {
			modTextScale(2);
			modText(120 - (int) strlen(result) * 8, 70, 0xFFE040, "%s", result);
			modTextScale(1);
		} else if (holdFrames > 30) {
			modText(70, 80, 0xFFFFFF, "WAITING FOR THE OTHER SIDE");
		} else if (matchFrame < 90) {
			modTextScale(2);
			modText(84, 70, 0xFFE040, "FIGHT!");
			modTextScale(1);
		}
		return;
	}
	if (st == ST_IDLE && !onlinePage) {
		if (f < noteUntil) {
			modTextScale(2);
			modText(8, 30, 0x40FF40, "%s", note);
			modTextScale(1);
		}
		return;
	}
	if (modRead32(BF_RAM_MENU_OPEN) == 1) {
		drawPage();
	} else if (st != ST_IDLE) {
		drawStatusBox();
	}
}

static void frame(void) {
	static uint32_t lastKeys;
	uint32_t keys = modKeys(), pressed = keys & ~lastKeys;
	const char* env;
	lastKeys = keys;
	if (st == ST_IDLE) {
		static int envDone;
		env = getenv("NETPLAY");
		if (getenv("NETPLAY_LOCALTEST") && !envDone && modFrame() > 100 && modCanCall() && (modRead32(BF_RAM_PLAYER_PTR) >> 24) == 2) {
			struct NpStart s;
			size_t size = modStateSize();
			envDone = 1;
			returnState = malloc(size);
			modStateSave(returnState);
			who1 = bfCurrentChar();
			who2 = atoi(getenv("NETPLAY_LOCALTEST")); // the other fighter's character
			if (who2 == who1 || who2 >= BF_CHARACTERS) {
				who2 = BF_NINTH;
				memcpy(bfChar(who2), bfChar(who1), sizeof(struct BfChar));
			}
			bfChar(who1)->hp = bfChar(who1)->maxHp;
			bfChar(who2)->hp = getenv("NETPLAY_TESTHP") ? (uint16_t) atoi(getenv("NETPLAY_TESTHP")) : bfChar(who2)->maxHp;
			p1 = modRead32(BF_RAM_PLAYER_PTR);
			host = 1;
			localTest = 1;
			if (spawnSecond()) {
				s.p1 = p1; s.p2 = p2; s.p2in = p2in; s.who1 = (uint8_t) who1; s.who2 = (uint8_t) who2; s.delay = 0; s.shown2 = (uint8_t) atoi(getenv("NETPLAY_LOCALTEST"));
				startMatch(&s);
				modLog("netplay: local test: p1 stats %04X %04X %04X %04X | p2 stats %04X %04X %04X %04X", modRead16(p1 + 0x1B0), modRead16(p1 + 0x1B2), modRead16(p1 + 0x1B4), modRead16(p1 + 0x1B6), modRead16(p2 + 0x1B0), modRead16(p2 + 0x1B2), modRead16(p2 + 0x1B4), modRead16(p2 + 0x1B6));
			}
		}
		if (env && !envDone && modFrame() > 30) {
			envDone = 1;
			begin(!strcmp(env, "host") ? 1 : !strcmp(env, "random") ? 2 : 0, (uint32_t) (getenv("NETPLAY_CODE") ? atoi(getenv("NETPLAY_CODE")) : 0));
		}
		page = menuPage();
		if (justOpened) {
			justOpened = 0;
			pressed = 0; // the press that opened the page
			loadConfig();
			modLog("netplay: ONLINE page open (from %s)", under == PAGE_OPTIONS ? "Options" : "Journal");
		}
		if (onlinePage) {
			if (page == NO_PAGE && !passing) {
				onlinePage = 0; // the menu went away underneath
				under = NO_PAGE;
			} else if (pressed & BF_BTN_DOWN) {
				menuLine = (menuLine + 1) % 3;
			} else if (pressed & BF_BTN_UP) {
				menuLine = (menuLine + 2) % 3;
			} else if (pressed & (BF_BTN_B | BF_BTN_START)) {
				leavePage(BF_BTN_B, 1); // out of the Start menu altogether
			} else if (pressed & BF_BTN_R) {
				leavePage(BF_BTN_R, under == PAGE_OPTIONS); // on to Journal (the game's menu is already there if it was entered from Journal)
			} else if (pressed & BF_BTN_L) {
				leavePage(BF_BTN_L, under == PAGE_JOURNAL);
			} else if (pressed & BF_BTN_A) {
				if (menuLine == 0) {
					begin(2, 0);
				} else if (menuLine == 1) {
					begin(1, 0);
				} else {
					st = ST_CODE;
					codeDigit = 0;
				}
			}
		}
		draw();
		return;
	}
	if (st == ST_CODE) {
		if (pressed & BF_BTN_UP) {
			codeDigits[codeDigit] = (codeDigits[codeDigit] + 1) % 10;
		} else if (pressed & BF_BTN_DOWN) {
			codeDigits[codeDigit] = (codeDigits[codeDigit] + 9) % 10;
		} else if (pressed & BF_BTN_RIGHT) {
			codeDigit = (codeDigit + 1) % 4;
		} else if (pressed & BF_BTN_LEFT) {
			codeDigit = (codeDigit + 3) % 4;
		} else if (pressed & BF_BTN_B) {
			st = ST_IDLE;
		} else if (pressed & BF_BTN_A) {
			begin(0, (uint32_t) (codeDigits[0] * 1000 + codeDigits[1] * 100 + codeDigits[2] * 10 + codeDigits[3]));
			if (st == ST_CODE) {
				st = ST_IDLE; // (could not start: the note says why)
			}
		}
		draw();
		return;
	}
	pump();
	if (localTest && st == ST_OVER && modFrame() >= overUntil) {
		localTest = 0;
		leave("BACK WHERE YOU WERE");
		draw();
		return;
	}
	if (sockDead && st != ST_OVER) {
		if (st == ST_MATCH) {
			finish("CONNECTION LOST");
		} else {
			leave("CONNECTION LOST");
			draw();
			return;
		}
	}
	if (st == ST_CONNECTING || st == ST_OPENING || st == ST_WAIT_PEER) {
		opening();
		if (st == ST_OPENING || st == ST_WAIT_PEER || st == ST_CONNECTING) {
			if (pressed & BF_BTN_B) {
				leave("CANCELLED");
			}
			draw();
			return;
		}
	}
	parsePackets();
	if (st == ST_HELLO && host && !sentHello) {
		sendHello();
	}
	if (st == ST_PREP) {
		prepare();
	}
	if (st == ST_MATCH) {
		if (modGameFrame() != lastGameFrame) {
			lastGameFrame = modGameFrame();
			++matchFrame;
			holdFrames = 0;
			afterFrame();
		} else if (holdFrames) {
			if (++holdFrames > 900) {
				finish("CONNECTION LOST");
			}
		}
	} else if (st == ST_HELLO || st == ST_RECV_STATE || st == ST_WAIT_START || st == ST_WAIT_READY) {
		if (modFrame() - stateSince > 60 * 120) {
			leave("THE OTHER SIDE TOOK TOO LONG");
		}
	}
	if (st == ST_OVER && modFrame() >= overUntil) {
		leave("BACK WHERE YOU WERE");
	}
	pump();
	draw();
}

MOD_INIT(netplay) {
	debug = getenv("NETPLAY_DEBUG") != NULL;
	modHook(FN_RECORD_GETTER, onRecordGetter);
	modHook(FN_INPUT_GETTER, onInputGetter);
	modHook(FN_PLAYER_EVENT, onPlayerEvent);
	modHook(0x08010BF2, onApplyHit); // player class +0x5C: the hit's damage comes off the record
	modHook(0x08011C70, onQuery); // a person or a sign answering A would freeze the match
	modHook(FN_TRIGGER_UNDERFOOT, onTrigger); // so would a save pad's menu
	modHook(FN_TRIGGER_SCAN, onTrigger);
	modSetFrameGate(gate);
	modOnFrame(frame);
}
