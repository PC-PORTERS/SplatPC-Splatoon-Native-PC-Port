"""First LAN two-copy test (28/10 20h16): regressions for the server-side fixes.

- Splatoon writes MatchmakeSessionSearchCriteria struct version 3 with 15 trailing bytes:
  the server now consumes them instead of flooding the log.
- In public mode the registered PIA URL carries the game's loopback address (127.0.0.1);
  peers must get the packet source instead, and ReplaceURL must still find the entry.
"""
import pathlib
import unittest

from nintendo.nex import matchmaking, settings, streams
from tools.nexlab import matchmake, nexserver

DIRECTORY = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"


def neap_settings():
    config = settings.default()
    config.configure(nexserver.ACCESS_KEY, nexserver.NEX_VERSION)
    return config


class SearchCriteriaTests(unittest.TestCase):
    def test_splatoon_version_3_consumes_its_extra_bytes(self):
        config = neap_settings()
        config["nex.struct_header"] = 1   # rmc.py enables it on every RMC connection
        import struct
        payload = b"".join([
            struct.pack("<I", 0),          # attribs
            struct.pack("<H", 0),          # game_mode ""
            struct.pack("<H", 0),          # min_participants ""
            struct.pack("<H", 0),          # max_participants ""
            struct.pack("<H", 0),          # matchmake_system ""
            b"\x01\x01\x00",               # vacant_only, exclude_locked, exclude_non_host_pid
            struct.pack("<I", 0),          # selection_method
            struct.pack("<H", 1),          # vacant_participants (NEX >= 3.5)
            bytes(range(15)),              # fields NintendoClients 5.0.0 does not know
        ])
        blob = (struct.pack("<I", 1)       # one criterion
                + b"\x03"                  # struct version Splatoon writes
                + struct.pack("<I", len(payload)) + payload)
        stream = streams.StreamIn(blob, config)
        criteria = stream.list(matchmake.SplatoonSearchCriteria)
        self.assertEqual(len(criteria), 1)
        self.assertEqual(bytes(criteria[0].extra), bytes(range(15)))
        self.assertEqual(stream.available(), 0, "the whole criteria blob must be consumed")


class FakeClient:
    def __init__(self, pid=0x5EEA2491, address=("192.168.1.8", 5000)):
        self._pid, self._address = pid, address

    def pid(self):
        return self._pid

    def remote_address(self):
        return self._address


class PublicStationAddressTests(unittest.TestCase):
    def setUp(self):
        matchmake.configure("lab")
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.STATIONS.clear()

    def tearDown(self):
        matchmake.STATIONS.clear()
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.configure("lab")

    def test_loopback_pia_url_is_rewritten_to_the_packet_source(self):
        server = nexserver.SecureServer("public")
        url = matchmake.common.StationURL("prudp", address="127.0.0.1", port=57285, type=2)
        answer = server.answer(FakeClient(), [url])
        self.assertEqual(answer.public_station["address"], "192.168.1.8")
        stored = matchmake.STATIONS[0x5EEA2491][0]
        self.assertEqual((stored["address"], stored["port"]), ("192.168.1.8", 57285))

        # The game's ReplaceURL still carries 127.0.0.1: it must match by port and type and
        # keep the public address for peers.
        import asyncio
        replacement = matchmake.common.StationURL("prudp", address="127.0.0.1", port=51534,
                                                  type=3, natf=1, natm=1)
        asyncio.run(server.replace_url(FakeClient(), url, replacement))
        stored = matchmake.STATIONS[0x5EEA2491][0]
        self.assertEqual((stored["address"], stored["port"], stored["type"]),
                         ("192.168.1.8", 51534, 3))

    def test_lab_mode_keeps_the_registered_address(self):
        server = nexserver.SecureServer("lab")
        url = matchmake.common.StationURL("prudp", address="127.0.0.1", port=64278, type=2)
        server.answer(FakeClient(address=("127.0.0.1", 5000)), [url])
        stored = matchmake.STATIONS[0x5EEA2491][0]
        self.assertEqual(stored["address"], "127.0.0.1")


if __name__ == "__main__":
    unittest.main()
