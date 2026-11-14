"""Load test: 100 fake players through the server, in waves (chantier 2).

Why waves: NintendoClients' PRUDP V1 packs source/dest port in 4 bits and allocates 16
local ports per stream, so a single listener accepts ~14 secure connections at once (proven
during this mission: a 100-way burst connects 14, the rest fail with "All ports are in use").
The load test drives 100 players from distinct loopback addresses through the server in waves
of 6 (a separate burst probe connected 30 players from 30 distinct loopback addresses
simultaneously, so the old 16-connection suspicion is disproven) and checks time, heap and
resource release.

Not part of the default suite. Run:
    $env:NEXLAB_LOAD_TEST = '1'; py -3.12 -m unittest tools.tests.test_load -v
"""
import asyncio
import contextlib
import gc
import os
import pathlib
import secrets
import time
import tracemalloc
import unittest

from tools.nexlab import matchmake, nexserver
from tools.tests.test_server_e2e import FakePlayer, auto_param, free_ports

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"
PLAYERS = 100
WAVE = 6


def password_for(pid):
    return (f"{pid:08x}" * 2)[:16]


@unittest.skipUnless(os.environ.get("NEXLAB_LOAD_TEST") == "1",
                     "set NEXLAB_LOAD_TEST=1 to run the 100-player load test")
class LoadTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        DIRECTORY.mkdir(parents=True, exist_ok=True)
        self.directory = DIRECTORY / f"load-{secrets.token_hex(4)}"
        self.directory.mkdir()
        self.saved = (matchmake.MODE, matchmake.SESSIONS)
        # Short idle detection so brutally cut wave members free their PRUDP port quickly.
        self.saved_idle = (nexserver.IDLE_CONNECTION_TIMEOUT, nexserver.IDLE_SCAN_SECONDS)
        nexserver.IDLE_CONNECTION_TIMEOUT = 3.0
        nexserver.IDLE_SCAN_SECONDS = 0.2

    async def asyncTearDown(self):
        matchmake.shutdown()
        matchmake.SESSIONS = self.saved[1]
        matchmake.configure(self.saved[0])
        nexserver.IDLE_CONNECTION_TIMEOUT, nexserver.IDLE_SCAN_SECONDS = self.saved_idle
        for path in self.directory.glob("*"):
            path.unlink(missing_ok=True)
        self.directory.rmdir()

    async def test_100_players(self):
        identities = []
        players = []
        for index in range(PLAYERS):
            pid = 0x2000 + index
            password = password_for(pid)
            path = self.directory / f"{pid:08x}.ini"
            path.write_text(f"pid = {pid:08x}\npassword = {password}\n", encoding="utf-8")
            identities.append(path)
            players.append((pid, password))
        port = free_ports(2)
        tracemalloc.start()
        started = time.perf_counter()
        connected = 0
        failures = []
        session_peak = [0]

        async with nexserver.serve("127.0.0.1", port, identities=identities,
                                   identity_roots=[], natcheck=False, trace=False):
            semaphore = asyncio.Semaphore(WAVE)

            async def run_player(index, brutal):
                # The whole connection lifetime stays in one task: anyio cancel scopes cannot
                # be exited from another task.
                pid, password = players[index]
                stack = contextlib.AsyncExitStack()
                try:
                    async with semaphore:
                        local_host = f"127.0.0.{2 + index % 250}"
                        fake = FakePlayer(pid, password, port, port + 1, local_host=local_host)
                        player = await fake.connect(stack)
                        await player.extension.auto_matchmake_with_param_postpone(auto_param())
                        session_peak[0] = max(session_peak[0], len(matchmake.SESSIONS.by_gid))
                    if brutal:
                        with contextlib.suppress(BaseException):
                            await player.client._client._socket.close()
                except BaseException as error:
                    failures.append((index, error))
                finally:
                    with contextlib.suppress(BaseException):
                        await stack.aclose()

            for start in range(0, PLAYERS, WAVE):
                wave = list(range(start, min(start + WAVE, PLAYERS)))
                await asyncio.gather(*(run_player(i, i % 2 == 1) for i in wave))
                connected += len(wave)
                await asyncio.sleep(0.8)          # server releases the wave's connections
            elapsed = time.perf_counter() - started
            await asyncio.sleep(0.5)
            gc.collect()
            heap = tracemalloc.get_traced_memory()
            tracemalloc.stop()
            print(f"[load] {connected}/{PLAYERS} players in waves of {WAVE}, "
                  f"{len(failures)} failures, {elapsed:.2f} s, "
                  f"{session_peak[0]} sessions at peak, heap peak {heap[1] / 1e6:.1f} MB, "
                  f"heap after close {heap[0] / 1e6:.1f} MB")
            if failures:
                print(f"[load] first failure: {failures[0][1]!r}")
            self.assertEqual(failures, [])
            # Everything must be released: no player, no session, no peer, no NEX id.
            self.assertEqual(matchmake.CLIENTS, {})
            self.assertEqual(matchmake.STATIONS, {})
            self.assertEqual(matchmake.PEER, {})
            self.assertEqual(matchmake.BY_RVCID, {})
            self.assertEqual(matchmake.LIVE, {})
            self.assertEqual(len(matchmake.SESSIONS.by_gid), 0)


if __name__ == "__main__":
    unittest.main()
