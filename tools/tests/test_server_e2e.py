"""End-to-end tests for the lab and public server, without the game.

A real server answers on UDP loopback and NintendoClients plays the client side (PRUDP V1
handshake, Kerberos ticket, secure connection, matchmaking, notification 3001). Two fake
players log in, one creates a session, the other joins it, and the answers are checked.

The public-mode tests bind the fake players to 127.0.0.2 and 127.0.0.3, so the address the
server sees is not 127.0.0.1: in public mode the station URLs must carry the real packet
source, while lab mode keeps its 127.0.0.10+ shortcuts.

Nothing is read from Game/: the identities are invented here (pid + 16-character password)
and nexserver's identity lookup is redirected to those files only.

Run from the repository root:
    py -3.12 -m unittest tools.tests.test_server_e2e -v

Note: `-I` cannot be used, it hides the user site-packages where NintendoClients is
installed (`py -3.12 -I -c "import nintendo"` fails).
"""
import asyncio
import contextlib
import json
import pathlib
import secrets
import socket
import unittest
import urllib.request

import anyio
import anynet.udp
import anynet.util
from nintendo.nex import authentication, common, kerberos, matchmaking, notification, prudp, rmc, secure, settings
from tools.nexlab import (accounts as account_store, matchmake, nexserver as lab, register_server,
                          server_config)

HOST = "127.0.0.1"
PID_A = 0x1001
PID_B = 0x1002
NOTIFICATION_JOINED = 3001
PIA_PORT = 40000            # fake peer-to-peer port, nothing listens on it
# Identity files go into an existing, git-ignored directory: the sandbox lets the process
# write files there, but not inside a directory its own run has just created.
IDENTITY_DIR = pathlib.Path(__file__).resolve().parents[2] / "build" / "tmp"


def free_ports(count=2):
    """A port p such that p .. p+count-1 are all free on UDP 127.0.0.1."""
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind((HOST, 0))
            port = probe.getsockname()[1]
        if port > 65535 - count:
            continue
        held = []
        try:
            for candidate in range(port, port + count):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                held.append(sock)
                sock.bind((HOST, candidate))
        except OSError:
            continue
        finally:
            for sock in held:
                sock.close()
        return port
    raise RuntimeError("no free UDP port range on loopback")


@contextlib.asynccontextmanager
async def rmc_connect_from(nex_settings, host, port, local_host, credentials=None, servers=()):
    """`rmc.connect` from a chosen local address (anyio's local_host).

    The library's `rmc.connect` cannot bind the client socket, so this rebuilds the same
    stack (PRUDP transport + RMC loop) around a socket bound to `local_host`."""
    async with await anyio.create_connected_udp_socket(host, port, local_host=local_host) as udp_socket:
        udp_client = anynet.udp.UDPClient(udp_socket)
        # anynet's task-group helper cancels its scope before leaving; a plain
        # anyio.create_task_group() can hang here because the PRUDP reader stays blocked.
        async with anynet.util.create_task_group() as transport_group:
            transport = prudp.PRUDPClientTransport(nex_settings, udp_client, transport_group)
            transport.start()
            async with transport.connect(1, 10, credentials=credentials) as pru:
                client = rmc.RMCClient(nex_settings, pru)
                async with client:
                    async with anynet.util.create_task_group() as rmc_group:
                        rmc_group.start_soon(client.start, list(servers))
                        yield client


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


def password_for(pid):
    """A valid lab password: 16 alphanumeric characters, as find_password requires."""
    return f"{pid:08x}" * 2


def write_identity(directory, pid, password):
    """online_identity.ini as the port writes it, passed through nexserver's `identities`."""
    path = pathlib.Path(directory) / f"nexlab-e2e-{pid:08x}-{secrets.token_hex(4)}.ini"
    path.write_text(
        "# fake identity, tests only\n"
        f"pid = {pid:08x}\n"
        f"password = {password}\n",
        encoding="utf-8",
    )
    return path


class NotificationSink(notification.NotificationServer):
    """Server side of protocol 14: records what the lab server pushes to this player."""
    def __init__(self):
        super().__init__()
        self.events = []

    async def process_notification_event(self, client, event):
        self.events.append(event)

    async def wait_for(self, count, timeout=5.0):
        async with asyncio.timeout(timeout):
            while len(self.events) < count:
                await asyncio.sleep(0.02)
        return self.events[count - 1]


class FakePlayer:
    """One NEX client: Login, Kerberos ticket, secure Register, then RMC calls."""
    def __init__(self, pid, password, auth_port, secure_port, local_host=None):
        self.pid = pid
        self.password = password
        self.auth_port = auth_port
        self.secure_port = secure_port
        self.local_host = local_host
        self.sink = NotificationSink()
        self.login_response = None
        self.ticket = None
        self.register_response = None
        self.client = None
        self.station = None
        self.pia_station = None
        self.secure = None
        self.extension = None
        self.sessions = None

    def neap_settings(self):
        s = settings.default()
        s.configure(lab.ACCESS_KEY, lab.NEX_VERSION)
        return s

    def connect_to(self, s, port, **kwargs):
        """rmc.connect, or the same stack bound to `local_host` when the test asks for one."""
        if self.local_host:
            return rmc_connect_from(s, HOST, port, self.local_host, **kwargs)
        return rmc.connect(s, HOST, port, **kwargs)

    async def connect(self, stack):
        s = self.neap_settings()

        # 1. Anonymous connection to the auth server, Login (protocol 10 method 1).
        async with self.connect_to(s, self.auth_port) as client:
            auth = authentication.AuthenticationClient(client)
            self.login_response = await auth.login(str(self.pid))

        # 2. Decrypt the ticket the way the game does, then open the secure connection.
        user_key = kerberos.KeyDerivationOld().derive_key(self.password.encode("ascii"), self.pid)
        self.ticket = kerberos.ClientTicket.decrypt(self.login_response.ticket, user_key, s)
        credentials = kerberos.Credentials(self.ticket, self.pid, cid=1)
        self.client = await stack.enter_async_context(
            self.connect_to(s, self.secure_port, credentials=credentials, servers=[self.sink]))
        self.secure = secure.SecureConnectionClient(self.client)
        self.extension = matchmaking.MatchmakeExtensionClient(self.client)
        self.sessions = matchmaking.MatchMakingClient(self.client)

        # 3. Register the way the game does: its own NEX socket, type 2 (the PIA socket is
        #    announced later with ReplaceURL, type 3).
        local_port = self.client.local_address()[1]
        self.station = common.StationURL("prudps", address=self.local_host or HOST, port=local_port,
                                         CID=1, PID=self.pid, sid=1, stream=10, type=2)
        self.register_response = await self.secure.register([self.station])
        return self

    async def announce_pia_socket(self, port=PIA_PORT):
        """ReplaceURL: the game swaps its registered NEX URL for the PIA one, which carries
        the connection id (RVCID) that the secure server handed out at Register."""
        self.pia_station = common.StationURL("prudps", address=self.local_host or HOST, port=port,
                                             CID=1, PID=self.pid, sid=1, stream=10, type=3,
                                             RVCID=self.register_response.connection_id)
        await self.secure.replace_url(self.station, self.pia_station)
        return self.pia_station


def auto_param(max_participants=8):
    """AutoMatchmakeParam as NintendoClients (and the game) send it."""
    param = matchmaking.AutoMatchmakeParam()
    param.session.game_mode = 1
    param.session.min_participants = 2
    param.session.max_participants = max_participants
    param.participants = []
    param.gid_for_participation_check = 0
    param.options = 0
    param.join_message = ""
    param.num_participants = 1
    param.search_criteria = []
    param.target_gids = []
    return param


class ServerEndToEndTests(unittest.IsolatedAsyncioTestCase):
    @contextlib.asynccontextmanager
    async def running_server(self, *, mode="lab", config=None, port=None, accounts=None,
                             registration=False):
        """Server + fake players, entered and left inside the test method's own task.

        anyio task groups (prudp/rmc serve) are bound to the task that entered them, so this
        cannot be split between asyncSetUp and asyncTearDown. The identity lookup is limited
        to the files made here, so Game/online_identity.ini is never opened.

        `config` runs the server from a ServerConfig (as the command line does). In public
        mode an account store is created (and the two test players registered) unless one is
        passed; `registration=True` also starts the HTTP registration endpoint.
        """
        IDENTITY_DIR.mkdir(parents=True, exist_ok=True)
        self.identity_files = [write_identity(IDENTITY_DIR, pid, password_for(pid))
                               for pid in (PID_A, PID_B)]
        self.port = port or free_ports(2)
        if config is None:
            public = mode == "public"
            config = server_config.ServerConfig(
                mode=mode, listen="0.0.0.0" if public else HOST, auth_port=self.port,
                secure_port=self.port + 1, natcheck=False,
                advertise="203.0.113.10" if public else None)
        self.config = config
        self.players = []
        self.accounts = accounts
        self.accounts_db = None
        self.registration = None
        if config.mode == "public" and self.accounts is None:
            self.accounts_db = IDENTITY_DIR / f"accounts-{secrets.token_hex(4)}.sqlite3"
            self.accounts = account_store.Accounts(self.accounts_db)
            for pid in (PID_A, PID_B):
                self.accounts.register(pid, password_for(pid))
        if registration and self.accounts is not None:
            self.registration = register_server.RegistrationServer(
                self.accounts, HOST, free_tcp_port())

        # Fresh matchmaking state, as if the server had just restarted.
        matchmake.SESSIONS = matchmake.Sessions()
        matchmake.STATIONS.clear()
        matchmake.CLIENTS.clear()
        matchmake.PEER.clear()
        matchmake.BY_RVCID.clear()
        matchmake.FAKE_IPS.clear()
        matchmake.TASKS.clear()
        lab.natcheck_server.OBSERVATIONS.clear()

        async with contextlib.AsyncExitStack() as stack:
            self.stack = stack
            try:
                await stack.enter_async_context(lab.serve(
                    config.listen, config.auth_port, advertise=config.advertise,
                    secure_port=config.secure_port, mode=config.mode, natcheck=config.natcheck,
                    natcheck_addresses=config.natcheck_addresses, accounts=self.accounts,
                    registration=self.registration,
                    identities=self.identity_files, identity_roots=[], trace=False))
                yield
            finally:
                self.players.clear()
                for path in self.identity_files:
                    path.unlink(missing_ok=True)
                if self.accounts_db is not None:
                    self.accounts.close()
                    for suffix in ("", "-wal", "-shm"):
                        pathlib.Path(str(self.accounts_db) + suffix).unlink(missing_ok=True)

    async def add_player(self, pid, local_host=None):
        player = FakePlayer(pid, password_for(pid), self.port, self.config.secure_port,
                            local_host=local_host)
        await player.connect(self.stack)
        self.players.append(player)
        return player

    async def test_login_ticket_and_register(self):
        async with self.running_server():
            a = await self.add_player(PID_A)
            login = a.login_response

            self.assertEqual(login.pid, PID_A)
            self.assertEqual(login.server_name, lab.SERVER_NAME)
            # The game connects back to the secure station announced by the login answer.
            station = login.connection_data.main_station
            self.assertEqual(station["address"], HOST)
            self.assertEqual(station["port"], self.port + 1)
            self.assertEqual(station["PID"], lab.SECURE_PID)
            # The ticket really belongs to this player and to the secure server.
            self.assertEqual(a.ticket.target, lab.SECURE_PID)

            register = a.register_response
            self.assertEqual(register.public_station["RVCID"], register.connection_id)
            self.assertEqual(matchmake.BY_RVCID[register.connection_id], PID_A)
            self.assertEqual(matchmake.CLIENTS[PID_A].pid(), PID_A)
            # Registered list = the player's NEX URLs plus the announced public one (2 URLs: PIA
            # refuses 1 or 3), and the lab shows a loopback address as a fake public IP.
            self.assertEqual(len(matchmake.STATIONS[PID_A]), 2)
            public = register.public_station
            self.assertIn(public["address"], matchmake.FAKE_IPS)
            self.assertEqual(public["port"], a.client.local_address()[1])
            self.assertEqual(public["type"], 3)
            # lab_address puts the fake public IP back to 127.0.0.1 for peers on the same PC.
            self.assertEqual(matchmake.lab_address(public)["address"], HOST)

            # ReplaceURL (protocol 11 method 7) swaps the NEX URL for the PIA one.
            await a.announce_pia_socket()
            self.assertEqual(matchmake.STATIONS[PID_A][0]["port"], PIA_PORT)
            self.assertEqual(matchmake.STATIONS[PID_A][0]["type"], 3)

            # SendReport (11/8) is accepted and journalised, the game relies on the success.
            await a.secure.send_report(23, b"\x00\x01\xff")

    async def test_two_players_matchmake_notification_and_session_urls(self):
        async with self.running_server():
            a = await self.add_player(PID_A)
            b = await self.add_player(PID_B)

            self.assertEqual(await a.extension.get_playing_session([a.pid]), [])
            session_a = await a.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertNotEqual(session_a.id, 0)
            self.assertEqual(session_a.owner, PID_A)
            event = await a.sink.wait_for(1)
            self.assertEqual(event.type, NOTIFICATION_JOINED)
            self.assertEqual((event.param1, event.param2, event.param3), (session_a.id, PID_A, 1))

            session_b = await b.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertEqual(session_b.id, session_a.id)          # joined, not created
            self.assertEqual(matchmake.SESSIONS.members[session_a.id], [PID_A, PID_B])

            event_a = await a.sink.wait_for(2)
            event_b = await b.sink.wait_for(1)
            for event in (event_a, event_b):
                self.assertEqual(event.type, NOTIFICATION_JOINED)
                self.assertEqual((event.param1, event.param2, event.param3),
                                 (session_a.id, PID_B, 2))

            # GetSessionURLs: the host's registered URLs, PIA socket first.
            await a.announce_pia_socket()
            urls = await b.sessions.get_session_urls(session_a.id)
            self.assertEqual(len(urls), 2)
            self.assertEqual((urls[0]["address"], urls[0]["port"]), (HOST, PIA_PORT))
            self.assertEqual(urls[0]["RVCID"], a.register_response.connection_id)

            # ReplaceURL already gave the real PIA endpoint: PEER must not override it.
            matchmake.PEER[PID_A] = (HOST, PIA_PORT + 7)
            urls = await b.sessions.get_session_urls(session_a.id)
            self.assertEqual((urls[0]["address"], urls[0]["port"]), (HOST, PIA_PORT))
            # Without an announced PIA URL, PEER is the lab fallback (old behaviour).
            matchmake.STATIONS[PID_A] = [common.StationURL("prudps", address=HOST, port=1234, type=2)]
            urls = await b.sessions.get_session_urls(session_a.id)
            self.assertEqual((urls[0]["address"], urls[0]["port"]), (HOST, PIA_PORT + 7))

            # Participation flags (109/1 and 109/2) are what the countdown waits for.
            await a.extension.close_participation(session_a.id)
            self.assertFalse(matchmake.SESSIONS.get(session_a.id).open_participation)
            await a.extension.open_participation(session_a.id)
            self.assertTrue(matchmake.SESSIONS.get(session_a.id).open_participation)

            # EndParticipation (109/22) removes the player, the session survives with the host.
            end = matchmaking.MatchMakingClientExt(b.client)
            self.assertTrue(await end.end_participation(session_a.id, "bye"))
            self.assertEqual(matchmake.SESSIONS.members[session_a.id], [PID_A])

    async def test_private_room_create_and_join(self):
        async with self.running_server():
            a = await self.add_player(PID_A)
            b = await self.add_player(PID_B)

            create = matchmaking.CreateMatchmakeSessionParam()
            create.session.game_mode = 3
            create.session.min_participants = 1
            create.session.max_participants = 8
            create.session.participation_policy = 98
            create.additional_participants = []
            create.gid_for_participation_check = 0
            create.options = 0
            create.join_message = ""
            create.num_participants = 1
            room = await a.extension.create_matchmake_session_with_param(create)
            self.assertEqual(room.owner, PID_A)

            join = matchmaking.JoinMatchmakeSessionParam()
            join.gid = room.id
            join.participants = []
            join.gid_for_participation_check = 0
            join.options = 0
            join.behavior = 0
            join.user_password = ""
            join.system_password = ""
            join.join_message = ""
            join.num_participants = 1
            join.extra_participants = 0
            joined = await b.extension.join_matchmake_session_with_param(join)
            self.assertEqual(joined.id, room.id)
            self.assertEqual(matchmake.SESSIONS.members[room.id], [PID_A, PID_B])
            self.assertEqual((await a.sink.wait_for(2)).param2, PID_B)   # the owner saw both joins
            self.assertEqual((await b.sink.wait_for(1)).param2, PID_B)

            # A private room does not accept auto matchmaking.
            other = await b.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertNotEqual(other.id, room.id)

    async def test_lab_mode_still_rewrites_loopback_sources(self):
        """Lab mode keeps its shortcut even when the packets come from another 127.x address."""
        async with self.running_server():          # lab by default
            a = await self.add_player(PID_A, local_host="127.0.0.2")
            public = a.register_response.public_station
            self.assertIn(public["address"], matchmake.FAKE_IPS)
            self.assertNotEqual(public["address"], "127.0.0.2")
            self.assertEqual(matchmake.lab_address(public)["address"], HOST)

    async def test_public_register_uses_the_packet_source_address(self):
        """Public mode: a player is where its packets come from, never a 127.x rewrite.

        The players bind their sockets to 127.0.0.2 and 127.0.0.3, so the server sees two
        different sources; lab mode would invent 127.0.0.10/127.0.0.11 instead."""
        async with self.running_server(mode="public"):
            self.assertTrue(matchmake.is_public())
            a = await self.add_player(PID_A, local_host="127.0.0.2")
            b = await self.add_player(PID_B, local_host="127.0.0.3")

            for player, address in ((a, "127.0.0.2"), (b, "127.0.0.3")):
                public = player.register_response.public_station
                self.assertEqual(public["address"], address)
                self.assertEqual(public["port"], player.client.local_address()[1])
                self.assertEqual(matchmake.STATIONS[player.pid][-1]["address"], address)
            self.assertEqual(matchmake.FAKE_IPS, set())        # no fake loopback was invented

            # A peer receives the host's real address, not 127.0.0.1 and not 127.0.0.10.
            session = await a.extension.auto_matchmake_with_param_postpone(auto_param())
            joined = await b.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertEqual(joined.id, session.id)
            urls = await b.sessions.get_session_urls(session.id)
            self.assertEqual(len(urls), 2)
            self.assertEqual(urls[0]["address"], "127.0.0.2")
            self.assertEqual(urls[-1]["address"], "127.0.0.2")

            # lab_address keeps announced addresses as they are in public mode.
            announced = common.StationURL("prudps", address="127.0.0.10", port=9, type=3)
            self.assertEqual(matchmake.lab_address(announced)["address"], "127.0.0.10")

    async def test_public_config_file_and_secure_port(self):
        """A JSON config drives the server, including a secure port that is not auth + 1."""
        port = free_ports(3)                     # auth, gap, secure
        path = IDENTITY_DIR / f"server-{secrets.token_hex(4)}.json"
        path.write_text(json.dumps({
            "_note": "test config",
            "mode": "public",
            "listen": "0.0.0.0",
            "advertise": "203.0.113.10",
            "auth_port": port,
            "secure_port": port + 2,
            "natcheck": False,
        }), encoding="utf-8")
        self.addCleanup(path.unlink, missing_ok=True)
        config = server_config.ServerConfig.load(path)
        async with self.running_server(config=config, port=port):
            a = await self.add_player(PID_A, local_host="127.0.0.2")
            station = a.login_response.connection_data.main_station
            self.assertEqual(station["port"], port + 2)
            self.assertEqual(a.register_response.public_station["address"], "127.0.0.2")
            await a.secure.send_report(7, b"config")

    async def test_public_login_uses_the_account_store(self):
        """Public mode authenticates from SQLite, not from online_identity.ini."""
        async with self.running_server(mode="public"):
            self.assertEqual(self.accounts.count(), 2)
            newcomer = 0x1003
            self.accounts.register(newcomer, password_for(newcomer))
            player = await self.add_player(newcomer, local_host="127.0.0.4")
            self.assertEqual(player.login_response.pid, newcomer)
            self.assertEqual(player.register_response.public_station["address"], "127.0.0.4")

    async def test_public_unknown_pid_is_refused_until_registered(self):
        async with self.running_server(mode="public"):
            config = settings.default()
            config.configure(lab.ACCESS_KEY, lab.NEX_VERSION)
            async with rmc.connect(config, HOST, self.port) as client:
                auth = authentication.AuthenticationClient(client)
                with self.assertRaises(common.RMCError) as caught:
                    await auth.login(str(0x4242))          # no account for this pid
            self.assertEqual(caught.exception.name(), "RendezVous::InvalidUsername")

            self.accounts.register(0x4242, password_for(0x4242))
            player = await self.add_player(0x4242)
            self.assertEqual(player.login_response.pid, 0x4242)

    async def test_public_registration_endpoint_feeds_login(self):
        """The HTTP endpoint registers a stranger, whose next login then succeeds."""
        async with self.running_server(mode="public", registration=True):
            newcomer = 0x10001005
            request = urllib.request.Request(
                f"http://127.0.0.1:{self.registration.port}/register",
                data=json.dumps({"pid": f"0x{newcomer:08x}",
                                 "password": password_for(newcomer)}).encode(),
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=5) as response:
                self.assertEqual(json.loads(response.read())["status"], "created")
            player = await self.add_player(newcomer, local_host="127.0.0.5")
            self.assertEqual(player.login_response.pid, newcomer)

    async def test_public_eight_players_fill_one_session(self):
        """Public mode: strangers fill one classic match up to 8, then it is closed."""
        async with self.running_server(mode="public"):
            pids = [0x2001 + index for index in range(8)]
            for pid in pids:
                self.accounts.register(pid, password_for(pid))
            players = [await self.add_player(pid) for pid in pids]
            gid = None
            for player in players:
                session = await player.extension.auto_matchmake_with_param_postpone(auto_param())
                gid = session.id
            self.assertEqual(matchmake.SESSIONS.members[gid], pids)
            session = matchmake.SESSIONS.get(gid)
            self.assertEqual(session.num_participants, 8)
            self.assertFalse(session.open_participation)          # full: no more joins
            # A ninth player opens a new match instead of an over-full one.
            newcomer = 0x2009
            self.accounts.register(newcomer, password_for(newcomer))
            ninth = await self.add_player(newcomer)
            other = await ninth.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertNotEqual(other.id, gid)
            self.assertEqual(matchmake.SESSIONS.members[other.id], [newcomer])

    async def test_public_fill_window_starts_with_fewer_players(self):
        """With fewer than 8 after the fill window, the session closes and the match starts."""
        async with self.running_server(mode="public"):
            matchmake.FILL_TIMEOUT = 0.3
            a = await self.add_player(PID_A)
            b = await self.add_player(PID_B)
            session = await a.extension.auto_matchmake_with_param_postpone(auto_param())
            await b.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertTrue(matchmake.SESSIONS.get(session.id).open_participation)
            await asyncio.sleep(0.7)
            self.assertFalse(matchmake.SESSIONS.get(session.id).open_participation)
            self.assertEqual(matchmake.SESSIONS.members[session.id], [PID_A, PID_B])
            # A newcomer after the window gets a fresh session.
            newcomer = 0x200A
            self.accounts.register(newcomer, password_for(newcomer))
            third = await self.add_player(newcomer)
            other = await third.extension.auto_matchmake_with_param_postpone(auto_param())
            self.assertNotEqual(other.id, session.id)

    async def test_lab_auto_matchmake_never_closes_the_session(self):
        """Lab mode keeps the old behaviour: only the game closes a session."""
        async with self.running_server():
            a = await self.add_player(PID_A)
            b = await self.add_player(PID_B)
            session = await a.extension.auto_matchmake_with_param_postpone(auto_param())
            await b.extension.auto_matchmake_with_param_postpone(auto_param())
            await asyncio.sleep(0.5)
            self.assertTrue(matchmake.SESSIONS.get(session.id).open_participation)

    async def test_unknown_player_is_refused(self):
        async with self.running_server():
            config = settings.default()
            config.configure(lab.ACCESS_KEY, lab.NEX_VERSION)
            async with rmc.connect(config, HOST, self.port) as client:
                auth = authentication.AuthenticationClient(client)
                with self.assertRaises(common.RMCError) as caught:
                    await auth.login(str(0x4242))       # no identity file for this pid
            self.assertEqual(caught.exception.name(), "RendezVous::InvalidUsername")


if __name__ == "__main__":
    unittest.main()
