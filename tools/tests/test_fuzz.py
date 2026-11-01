"""Fuzzing and crash-resistance tests for the NEX lab server (bug hunt / chantier 2).

In-process only: random RMC bodies dispatched to the real service servers, and random or
truncated PRUDP datagrams sent to a real lab server on loopback. The point is not to find
protocol answers but to prove that malformed input never escapes as a BaseException, never
kills the server, and never grows memory without bound (10,000 cases each, fixed seed).
Run from the repository root:
    py -3.12 -m unittest tools.tests.test_fuzz -v
"""
import asyncio
import gc
import pathlib
import random
import secrets
import socket
import tracemalloc
import unittest

from nintendo.nex import authentication, common, prudp, rmc, settings
from tools.nexlab import matchmake, nexserver

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"
SEED = 0x5EED_2026
CASES = 10_000


def free_udp_pair():
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        if port > 65533:
            continue
        held = []
        try:
            for candidate in (port, port + 1):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                held.append(sock)
                sock.bind(("127.0.0.1", candidate))
        except OSError:
            continue
        finally:
            for sock in held:
                sock.close()
        return port
    raise RuntimeError("no free UDP port pair")


class FakeClient:
    def __init__(self, pid=0x1234):
        self._pid = pid

    def pid(self):
        return self._pid

    def remote_address(self):
        return ("127.0.0.1", 5000)

    async def close(self):
        pass


class HandlerFuzzTests(unittest.IsolatedAsyncioTestCase):
    """Random protocol/method/body against every service server, in process."""

    async def asyncSetUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.saved = (matchmake.MODE, matchmake.SESSIONS)
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()
        config = settings.default()
        config.configure(nexserver.ACCESS_KEY, nexserver.NEX_VERSION)
        self.config = config
        self.servers = {server.PROTOCOL_ID: server for server in matchmake.servers()}
        station = "prudps:/address=127.0.0.1;port=60001;CID=1;PID=2;sid=1;stream=10;type=2"
        self.servers[authentication.AuthenticationServer.PROTOCOL_ID] = nexserver.AuthServer(
            config, b"k" * 16, station, "lab", None)
        self.servers[nexserver.SecureServer.PROTOCOL_ID] = nexserver.SecureServer("lab")

    async def asyncTearDown(self):
        matchmake.SESSIONS = matchmake.Sessions()
        try:
            matchmake.shutdown()
        finally:
            matchmake.SESSIONS = self.saved[1]
            matchmake.configure(self.saved[0])

    async def test_random_rmc_requests_do_not_escape(self):
        rng = random.Random(SEED)
        protocols = list(self.servers)
        tracemalloc.start()
        before = tracemalloc.get_traced_memory()[0]
        escaped = 0
        for _ in range(CASES):
            server = self.servers[rng.choice(protocols)]
            body = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 48)))
            from nintendo.nex import streams
            input = streams.StreamIn(body, self.config)
            output = streams.StreamOut(self.config)
            try:
                await server.handle(FakeClient(rng.randrange(1, 0x7FFFFFFF)),
                                    rng.randrange(0, 300), input, output)
            except Exception:
                pass                    # RMCError / stream errors: the library turns them into a result
            except BaseException:
                escaped += 1            # KeyboardInterrupt/SystemExit: a real crash path
        gc.collect()
        after = tracemalloc.get_traced_memory()[0]
        tracemalloc.stop()
        self.assertEqual(escaped, 0, "a BaseException escaped a handler")
        self.assertLess(after - before, 8 * 1024 * 1024, "handler memory grew without bound")

    async def test_random_prudp_packets_do_not_escape(self):
        rng = random.Random(SEED + 1)
        decoder = prudp.PRUDPMessageSelector(self.config).select(self.config["prudp.version"])
        tracemalloc.start()
        before = tracemalloc.get_traced_memory()[0]
        escaped = 0
        for _ in range(CASES):
            size = rng.randrange(0, 200)
            data = bytes(rng.getrandbits(8) for _ in range(size))
            if size >= 4 and rng.random() < 0.5:
                data = b"\x32\xab\x98\x64" + data[4:]      # plausible PRUDP magic
            try:
                decoder.decode(data)
            except Exception:
                pass
            except BaseException:
                escaped += 1
        gc.collect()
        after = tracemalloc.get_traced_memory()[0]
        tracemalloc.stop()
        self.assertEqual(escaped, 0, "a BaseException escaped the PRUDP decoder")
        self.assertLess(after - before, 8 * 1024 * 1024, "decoder memory grew without bound")


class WireFuzzTests(unittest.IsolatedAsyncioTestCase):
    """Random UDP datagrams at a real lab server, then a valid login must still work."""

    async def asyncSetUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.identity = DIRECTORY / f"fuzz-id-{secrets.token_hex(4)}.ini"
        self.identity.write_text("pid = 00001001\npassword = 0000100100001001\n", encoding="utf-8")
        self.addCleanup(self.identity.unlink, missing_ok=True)
        self.saved = (matchmake.MODE, matchmake.SESSIONS)
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()

    async def asyncTearDown(self):
        matchmake.shutdown()
        matchmake.SESSIONS = self.saved[1]
        matchmake.configure(self.saved[0])

    async def test_random_datagrams_do_not_kill_the_server(self):
        config = settings.default()
        config.configure(nexserver.ACCESS_KEY, nexserver.NEX_VERSION)
        port = free_udp_pair()
        rng = random.Random(SEED + 2)
        async with nexserver.serve("127.0.0.1", port, identities=[self.identity],
                                   identity_roots=[], natcheck=False, trace=False):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setblocking(False)
            tracemalloc.start()
            before = tracemalloc.get_traced_memory()[0]
            dropped = 0
            for _ in range(CASES):
                size = rng.randrange(0, 400)
                data = bytes(rng.getrandbits(8) for _ in range(size))
                if size >= 8 and rng.random() < 0.5:
                    data = b"\x32\xab\x98\x64" + data[4:]
                try:
                    sock.sendto(data, ("127.0.0.1", rng.choice((port, port + 1))))
                except OSError:
                    dropped += 1
            sock.close()
            await asyncio.sleep(0.5)          # let the server process what it can
            # A well-formed client must still complete a login.
            async with rmc.connect(config, "127.0.0.1", port) as client:
                response = await authentication.AuthenticationClient(client).login("4097")
                self.assertEqual(response.pid, 4097)
            gc.collect()
            after = tracemalloc.get_traced_memory()[0]
            tracemalloc.stop()
            self.assertLess(after - before, 24 * 1024 * 1024,
                            "server memory grew without bound under fuzzing")
            self.assertLess(dropped, CASES // 10)


if __name__ == "__main__":
    unittest.main()
