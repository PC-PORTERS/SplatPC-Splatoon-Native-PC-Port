"""RMC wire-level tests, in-memory transport only (NintendoClients dependency)."""
import unittest
from tools.nexlab import nexserver as lab
from nintendo.nex import rmc, secure, settings, streams, common

class Transport:
    def __init__(self, pid=None, minor=0):
        self.sent = []
        self._pid = pid
        self._minor = minor
    def pid(self):
        return self._pid
    def minor_version(self):
        return self._minor
    async def send(self, data):
        self.sent.append(data)

class NexLabTests(unittest.IsolatedAsyncioTestCase):
    async def test_unknown_protocol_metadata_and_error(self):
        transport = Transport()
        config = settings.default()
        client = lab.LabRMCClient(config, transport)
        request = rmc.RMCMessage.request(config, 109, 7, 42, b"abc")
        with self.assertLogs("nexserver", level="INFO") as logs:
            await client._handle_request(request)
        self.assertIn("protocol=109 method=7 params=3", " ".join(logs.output))
        response = rmc.RMCMessage.parse(config, transport.sent[0])
        self.assertEqual(response.result, common.Result.error("Core::NotImplemented").code())

    async def test_send_report_success(self):
        transport = Transport()
        config = settings.default()
        client = lab.LabRMCClient(config, transport)
        client.register_server(lab.SecureServer())
        body = streams.StreamOut(config)
        body.u32(23); body.qbuffer(b"\x00\x01\xff")
        # Resolve the actual generated method dispatch, rather than assume its id.
        method = next(k for k, handler in client._servers[11].methods.items() if handler.__name__ == "handle_send_report")
        request = rmc.RMCMessage.request(config, 11, method, 43, body.get())
        with self.assertLogs("nexserver", level="INFO") as logs:
            await client._handle_request(request)
        self.assertIn("SendReport id=23 bytes=3 data=0001ff", " ".join(logs.output))
        response = rmc.RMCMessage.parse(config, transport.sent[0])
        self.assertEqual(response.result, -1)
        self.assertEqual(response.body, b"")

    async def test_matchmaking_splatoon_format(self):
        from tools.nexlab import matchmake
        from nintendo.nex import matchmaking
        matchmake.SESSIONS = matchmake.Sessions()
        config = settings.default()
        config.configure(lab.ACCESS_KEY, lab.NEX_VERSION)
        config["nex.struct_header"] = 1          # PRUDP minor version 4, as the game

        def call(pid, method, body):
            transport = Transport(pid, 4)
            client = lab.LabRMCClient(config, transport)
            for server in matchmake.servers():
                client.register_server(server)
            return client, transport, rmc.RMCMessage.request(config, 109, method, 1, body)

        body = streams.StreamOut(config)
        body.list([0x1234], body.pid)
        client, transport, request = call(0x1234, 16, body.get())
        await client._handle_request(request)
        response = rmc.RMCMessage.parse(config, transport.sent[0])
        self.assertEqual(response.result, -1)
        self.assertEqual(response.body, bytes(4))           # empty list

        # AutoMatchmakeParam as Splatoon writes it (02BF5478): no block_list at the end.
        def param():
            session = matchmaking.MatchmakeSession()
            session.game_mode, session.min_participants, session.max_participants = 1, 2, 8
            inner = streams.StreamOut(config)
            inner.add(session)
            inner.list([], inner.pid); inner.u32(0); inner.u32(0); inner.string("")
            inner.u16(1); inner.list([], inner.add); inner.list([], inner.u32)
            out = streams.StreamOut(config)
            out.u8(0); out.u32(len(inner.get())); out.write(inner.get())
            return out.get()
        gids = []
        for pid in (0x1234, 0x5678):
            client, transport, request = call(pid, 40, param())
            await client._handle_request(request)
            response = rmc.RMCMessage.parse(config, transport.sent[0])
            self.assertEqual(response.result, -1)
            gids.append(streams.StreamIn(response.body, config).extract(matchmaking.MatchmakeSession).id)
        self.assertEqual(gids[0], gids[1])
        self.assertEqual(matchmake.SESSIONS.members[gids[0]], [0x1234, 0x5678])

        # Private Battle: 109/38 creates a room, a friend joins it by gid with 109/39
        # (the join param without the newer trailing fields).
        def struct(fill):
            inner = streams.StreamOut(config); fill(inner)
            out = streams.StreamOut(config)
            out.u8(0); out.u32(len(inner.get())); out.write(inner.get())
            return out.get()
        def create(s):
            session = matchmaking.MatchmakeSession()
            session.game_mode, session.min_participants, session.max_participants = 3, 1, 8
            s.add(session); s.list([], s.pid); s.u32(0); s.u32(0); s.string(""); s.u16(1)
        client, transport, request = call(0xAAAA, 38, struct(create))
        await client._handle_request(request)
        response = rmc.RMCMessage.parse(config, transport.sent[0])
        self.assertEqual(response.result, -1)
        room = streams.StreamIn(response.body, config).extract(matchmaking.MatchmakeSession).id
        def join(s):
            s.u32(room); s.list([], s.pid); s.u32(0); s.u32(0); s.u8(0)
            s.string(""); s.string(""); s.string(""); s.u16(1)
        client, transport, request = call(0xBBBB, 39, struct(join))
        await client._handle_request(request)
        response = rmc.RMCMessage.parse(config, transport.sent[0])
        self.assertEqual(response.result, -1)
        self.assertEqual(matchmake.SESSIONS.members[room], [0xAAAA, 0xBBBB])

if __name__ == "__main__":
    unittest.main()


