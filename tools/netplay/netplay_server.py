#!/usr/bin/env python3
"""Relay server for Buu's Fury online battles.

Two players connect. Either the first asks to HOST and gets a four-digit room
code that the second JOINs with (a battle with a friend), or both ask for a
RANDOM battle and the server pairs them with whoever is waiting. From then on
the server only passes bytes between the two, in both directions, until one
of them goes away. It never looks inside the game's packets, so it needs
nothing but Python 3.

    python3 netplay_server.py [--port 7777] [--bind 0.0.0.0]

Wire format of the opening exchange (little-endian):
    client -> server   "BFNP" ver(u8) cmd(u8: 1 host, 2 join, 3 random)
                       code(u32; a host may ask for a code, 0 = any)
    server -> client   "BFNP" status(u8) code(u32)
        status 0  paired: the relay starts (a host or a queued random player
                  gets this as a second answer when the other player arrives)
        status 1  no such room          status 2  room full
        status 3  bad request           status 4  queued for a random battle:
                  wait for a status 0; you will be the host of the match
A waiting player that closes its connection is forgotten at once; one that
nobody comes to is dropped after ROOM_TTL seconds.
"""
import argparse
import asyncio
import random
import socket
import struct
import time

MAGIC = b"BFNP"
VERSION = 1
ROOM_TTL = 600  # seconds a room (or a random player) waits for its second player
ST_OK, ST_NO_ROOM, ST_FULL, ST_BAD, ST_QUEUED = 0, 1, 2, 3, 4

rooms = {}  # code -> Room (hosted, waiting for a join)
queue = []  # Rooms of players waiting for a random battle, oldest first


class Room:
    def __init__(self, code, reader, writer, who):
        self.code = code
        self.host = (reader, writer)
        self.host_name = who
        self.guest = None
        self.joined = asyncio.Event()  # a guest has registered
        self.done = asyncio.Event()  # the match is over (the guest's handler waits for this)

    def label(self):
        return "room %04d" % self.code if self.code else "random match"


def peer_name(writer):
    try:
        host, port = writer.get_extra_info("peername")[:2]
        return "%s:%d" % (host, port)
    except Exception:
        return "?"


def answer(writer, status, code):
    writer.write(struct.pack("<4sBI", MAGIC, status, code))


async def pump(src, dst):
    try:
        while True:
            data = await src.read(65536)
            if not data:
                break
            dst.write(data)
            await dst.drain()
    except (ConnectionError, asyncio.IncompleteReadError, asyncio.CancelledError):
        pass
    finally:
        try:
            dst.close()
        except Exception:
            pass


def forget(room):
    rooms.pop(room.code, None)
    if room in queue:
        queue.remove(room)


async def wait_then_relay(room):
    """The waiting side (a host, or a random player with nobody to pair with yet).

    Waits for a second player, noticing if its own player leaves first (the
    game sends nothing until it is told the other side came, so a byte or an
    EOF on the way in means it is gone). Then relays until one side closes.
    """
    reader, writer = room.host
    joined = asyncio.ensure_future(room.joined.wait())
    gone = asyncio.ensure_future(reader.read(1))
    done, _ = await asyncio.wait({joined, gone}, timeout=ROOM_TTL, return_when=asyncio.FIRST_COMPLETED)
    if gone in done or joined not in done:
        forget(room)
        joined.cancel()
        if gone not in done:
            gone.cancel()
        await asyncio.gather(joined, gone, return_exceptions=True)
        print("%s: %s" % (room.label(), "the waiting player left" if gone in done else "nobody came"), flush=True)
        writer.close()
        if room.guest is not None:  # arrived in the same instant: tell it
            answer(room.guest[1], ST_NO_ROOM, room.code)
            try:
                await room.guest[1].drain()
            except Exception:
                pass
            room.guest[1].close()
        room.done.set()
        return
    gone.cancel()
    await asyncio.gather(gone, return_exceptions=True)  # nothing was consumed: it was still waiting
    guest_reader, guest_writer = room.guest
    answer(writer, ST_OK, room.code)
    answer(guest_writer, ST_OK, room.code)
    try:
        await writer.drain()
        await guest_writer.drain()
    except Exception:
        writer.close()
        guest_writer.close()
        room.done.set()
        return
    print("%s: %s and %s paired, relaying" % (room.label(), room.host_name, peer_name(guest_writer)), flush=True)
    started = time.time()
    await asyncio.gather(pump(reader, guest_writer), pump(guest_reader, writer))
    print("%s: over after %d s" % (room.label(), time.time() - started), flush=True)
    room.done.set()


async def handle(reader, writer):
    who = peer_name(writer)
    sock = writer.get_extra_info("socket")
    if sock is not None:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        head = await asyncio.wait_for(reader.readexactly(10), 15)
    except Exception:
        writer.close()
        return
    magic, ver, cmd, code = struct.unpack("<4sBBI", head)
    if magic != MAGIC or ver != VERSION or cmd not in (1, 2, 3):
        answer(writer, ST_BAD, 0)
        await writer.drain()
        writer.close()
        print("%s: bad request" % who, flush=True)
        return
    if cmd == 1:
        if code and code not in rooms and 1000 <= code <= 9999:
            pass  # the host asked for this code (tests)
        else:
            code = random.randint(1000, 9999)
            while code in rooms:
                code = random.randint(1000, 9999)
        room = Room(code, reader, writer, who)
        rooms[code] = room
        answer(writer, ST_OK, code)
        await writer.drain()
        print("room %04d: hosted by %s" % (code, who), flush=True)
        await wait_then_relay(room)
        return
    if cmd == 3:
        if queue:
            room = queue.pop(0)
            print("random match: %s takes %s" % (who, room.host_name), flush=True)
        else:
            room = Room(0, reader, writer, who)
            queue.append(room)
            answer(writer, ST_QUEUED, 0)
            await writer.drain()
            print("random match: %s waiting (%d in the queue)" % (who, len(queue)), flush=True)
            await wait_then_relay(room)
            return
    else:
        room = rooms.get(code)
        if room is None:
            answer(writer, ST_NO_ROOM, code)
            await writer.drain()
            writer.close()
            print("%s: no room %04d" % (who, code), flush=True)
            return
        if room.guest is not None:
            answer(writer, ST_FULL, code)
            await writer.drain()
            writer.close()
            return
        del rooms[code]
    # the second player: the waiting side's handler answers both and relays
    room.guest = (reader, writer)
    room.joined.set()
    await room.done.wait()


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--bind", default=None, help="an address to listen on (default: every one, IPv4 and IPv6)")
    args = ap.parse_args()
    server = await asyncio.start_server(handle, args.bind, args.port)
    print("netplay relay listening on %s:%d" % (args.bind or "*", args.port), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
