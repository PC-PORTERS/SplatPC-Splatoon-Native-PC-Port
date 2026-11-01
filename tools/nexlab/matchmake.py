"""Matchmaking for the NEX lab/public server.

What the game calls, in order (measured in build/diagnostics/online-20261027-162435-server.log,
format):
  109/16 GetPlayingSession(list<pid>)            -> no session in progress: empty list
  109/40 AutoMatchmakeWithParamPostpone(param)   -> join an open session with room, or create one
then, still to be seen in a real run: GetSessionURLs, Open/CloseParticipation, EndParticipation...

Sessions live in memory only: restarting the server forgets them.
Lab mode keeps the original behaviour (a session stays open until a client closes it).
Public mode adds the classic-match fill: at most 8 players, the session closes as soon as it is
full, or when its fill window (FILL_TIMEOUT seconds) ends, so a match starts with whoever came.
Private rooms (CreateMatchmakeSessionWithParam) are never closed by the server.
"""
import asyncio, contextlib, logging, secrets, time
from nintendo.nex import common, matchmaking, nattraversal, notification
try:
    from . import natcheck_server
except ImportError:
    import natcheck_server

log = logging.getLogger("nexserver")

STATIONS = {}          # pid -> station URLs registered on the secure server (SecureServer.answer)
CLIENTS = {}           # pid -> RMC connection of the secure server, to push notifications
PEER = {}              # pid -> (ip, port) of its peer-to-peer (PIA) socket, see report_nat_properties
NAT_PORT = {}          # public mode: pid -> PIA port as mapped by its router (NAT check), see report_nat_properties
BY_RVCID = {}          # NEX connection id (station URL RVCID) -> pid
FAKE_IPS = set()       # loopback IPs nexserver shows as public addresses (same-PC lab, nexserver.SecureServer)
MODE = "lab"           # "lab" (same-PC shortcuts) or "public" (real addresses), set by nexserver.serve
FILL_TIMEOUT = 20.0    # public only: after this long an open session starts with who is there
MAX_PLAYERS = 8        # Splatoon's classic match size, whatever the client asks for
MAX_SESSIONS_PER_PID = 3   # sessions one player may own at once (public mode): one account cannot fill the table
MAX_SESSIONS = 512     # hard cap on sessions kept in memory (S-05)
SESSION_TTL = 600.0    # a closed session is forgotten after this long (S-05)
MAX_JOIN_MESSAGE = 200 # join messages are rebroadcast to every member


RANDOM_SESSION_KEY = False   # public servers: a random 32-byte session key instead of zeros (config random_session_key)


def configure(mode="lab", fill_timeout=None, random_session_key=False):
    """Select the address and matchmaking policy for this server run (lab stays untouched)."""
    global MODE, FILL_TIMEOUT, RANDOM_SESSION_KEY
    if mode not in ("lab", "public"):
        raise ValueError("mode must be 'lab' or 'public', not %r" % (mode,))
    MODE = mode
    RANDOM_SESSION_KEY = bool(random_session_key)
    if fill_timeout is not None:
        if not float(fill_timeout) > 0:
            raise ValueError("fill_timeout must be positive")
        FILL_TIMEOUT = float(fill_timeout)


def is_public():
    return MODE == "public"


def lab_address(url):
    """Copy of a station URL whose lab public IP is put back to 127.0.0.1, where the
    game's sockets really are (the port binds them to 127.0.0.1). Public mode keeps the
    address as announced: it is the one the player's packets really come from."""
    url = url.copy()
    if MODE == "lab" and url["address"] in FAKE_IPS:
        url["address"] = "127.0.0.1"
    return url
PARTICIPATION_JOIN = 3001   # NotificationEvent type: category 3 (participation), subtype 1 (joined)
TASKS = set()


async def _notify(members, event):
    await asyncio.sleep(0.2)          # after the RMC response the game is waiting on
    for pid in members:
        client = CLIENTS.get(pid)
        if client is None:
            continue
        try:
            await notification.NotificationClient(client).process_notification_event(event)
            log.info("  notified %08x: type=%d gid=%08x joined=%08x", pid, event.type, event.param1, event.param2)
        except Exception as error:      # a closed connection must not stop the server
            log.warning("  notification to %08x failed: %s", pid, error)


def notify_join(session, pid, message):
    """Real servers tell every participant (the newcomer included) that someone joined;
    hypothesis: the game's NexMatchAutoMatchmakeJob::WaitNotification waits for it."""
    event = notification.NotificationEvent()
    event.pid, event.type = pid, PARTICIPATION_JOIN
    event.param1, event.param2 = session.id, pid
    event.text = (message or "")[:MAX_JOIN_MESSAGE]   # client text, rebroadcast to everyone
    event.param3 = len(SESSIONS.members[session.id])
    task = asyncio.get_running_loop().create_task(_notify(list(SESSIONS.members[session.id]), event))
    TASKS.add(task); task.add_done_callback(TASKS.discard)


class SplatoonSearchCriteria(common.Structure):
    """Splatoon writes MatchmakeSessionSearchCriteria struct version 3 with trailing bytes
    NintendoClients 5.0.0 does not read. Not a subclass: Structure.decode walks the hierarchy
    and would expect one header per class, while the game sends a single version-3 header.
    Same fields as the library (NEX 3.8.3 is below 4.0), then the rest is consumed."""
    def max_version(self, settings):
        return 3

    def check_required(self, settings, version):
        pass

    def load(self, stream, version):
        self.attribs = stream.list(stream.string)
        self.game_mode = stream.string()
        self.min_participants = stream.string()
        self.max_participants = stream.string()
        self.matchmake_system = stream.string()
        self.vacant_only = stream.bool()
        self.exclude_locked = stream.bool()
        self.exclude_non_host_pid = stream.bool()
        self.selection_method = stream.u32()
        self.vacant_participants = stream.u16() if stream.settings["nex.version"] >= 30500 else 1
        # With struct headers (RMC always enables them) decode gives us a length-delimited
        # substream, so the unknown trailing fields can be consumed safely. Without a header
        # there is no substream and the rest of the packet belongs to the caller.
        self.extra = stream.read(stream.available()) if stream.settings["nex.struct_header"] else b""


class SplatoonAutoMatchmakeParam(common.Structure):
    """AutoMatchmakeParam as Splatoon (NEX 3.8.3) writes it (serializer 02BF5478): it stops
    after target_gids, while NintendoClients 5.0.0 also reads a block_list. Not a subclass:
    Structure.decode runs the load of every class in the hierarchy."""
    def check_required(self, settings, version):
        pass

    def load(self, stream, version):
        self.session = stream.extract(matchmaking.MatchmakeSession)
        self.participants = stream.list(stream.pid)
        self.gid_for_participation_check = stream.u32()
        self.options = stream.u32()
        self.join_message = stream.string()
        self.num_participants = stream.u16()
        self.search_criteria = stream.list(SplatoonSearchCriteria)
        self.target_gids = stream.list(stream.u32)


class LabCreateMatchmakeSessionParam(common.Structure):
    """CreateMatchmakeSessionParam (Private Battle room, 109/38). Same fields as
    NintendoClients; fields after join_message are read only if the game sent them."""
    def check_required(self, settings, version):
        pass

    def load(self, stream, version):
        self.session = stream.extract(matchmaking.MatchmakeSession)
        self.additional_participants = stream.list(stream.pid)
        self.gid_for_participation_check = stream.u32()
        self.options = stream.u32()
        self.join_message = stream.string()
        self.num_participants = stream.u16() if stream.available() >= 2 else 1


class LabJoinMatchmakeSessionParam(common.Structure):
    """JoinMatchmakeSessionParam (joining a friend's room, 109/39). Like AutoMatchmakeParam,
    NintendoClients 5.0.0 reads newer trailing fields (block_list): read them only if present."""
    def check_required(self, settings, version):
        pass

    def load(self, stream, version):
        self.gid = stream.u32()
        self.participants = stream.list(stream.pid)
        self.gid_for_participation_check = stream.u32()
        self.options = stream.u32()
        self.behavior = stream.u8()
        self.user_password = stream.string()
        self.system_password = stream.string()
        self.join_message = stream.string()
        self.num_participants = stream.u16() if stream.available() >= 2 else 1
        self.extra_participants = stream.u16() if stream.available() >= 2 else 0
        self.rest = stream.read(stream.available())


class Sessions:
    def __init__(self):
        self.next_gid = 0x1000
        self.by_gid = {}             # gid -> MatchmakeSession
        self.members = {}            # gid -> list of pids, owner first
        self.deadlines = {}          # gid -> monotonic time when the public fill window ends
        self.timers = {}             # gid -> asyncio task that closes the session at the deadline
        self.closed_at = {}          # gid -> monotonic time the session stopped accepting players

    def capacity(self, session):
        """Splatoon's classic match holds 8 players, whatever the client advertises."""
        try:
            wanted = int(session.max_participants or MAX_PLAYERS)
        except (TypeError, ValueError):
            wanted = MAX_PLAYERS
        return max(1, min(wanted, MAX_PLAYERS))

    def is_full(self, session):
        return len(self.members[session.id]) >= self.capacity(session)

    def expired(self, session):
        deadline = self.deadlines.get(session.id)
        return deadline is not None and time.monotonic() >= deadline

    def purge(self, now=None):
        """Forget closed sessions after SESSION_TTL (S-05: memory must not grow forever)."""
        now = time.monotonic() if now is None else now
        stale = [gid for gid, at in self.closed_at.items() if now - at >= SESSION_TTL]
        for gid in stale:
            self.forget(gid)

    def forget(self, gid):
        timer = self.timers.pop(gid, None)
        if timer is not None:
            timer.cancel()
        self.deadlines.pop(gid, None)
        self.closed_at.pop(gid, None)
        self.by_gid.pop(gid, None)
        self.members.pop(gid, None)

    def find_open(self, wanted):
        self.purge()
        for gid, session in list(self.by_gid.items()):
            if not session.open_participation or session.game_mode != wanted.game_mode:
                continue
            if self.is_full(session):
                self.close(session, "full")
                continue
            if self.expired(session):
                self.close(session, "fill window over")
                continue
            return session
        return None

    def create(self, wanted, pid, fill_window=0.0):
        self.purge()
        if len(self.by_gid) >= MAX_SESSIONS:
            log.warning("  matchmaking refused: %d sessions already open (S-05 cap)",
                        len(self.by_gid))
            raise common.RMCError("RendezVous::SessionFull")
        if is_public():
            # Unpredictable gathering ids: a stranger cannot walk through 0x1001, 0x1002, ...
            if sum(1 for other in self.by_gid.values() if other.owner == pid) >= MAX_SESSIONS_PER_PID:
                log.warning("  matchmaking refused: pid %08x already owns %d sessions", pid, MAX_SESSIONS_PER_PID)
                raise common.RMCError("RendezVous::SessionFull")
            gid = 0x1000 + secrets.randbelow(0x7FFFFFFF - 0x1000)
            while gid in self.by_gid:
                gid = 0x1000 + secrets.randbelow(0x7FFFFFFF - 0x1000)
        else:
            self.next_gid += 1
            gid = self.next_gid
        session = wanted
        session.id = gid
        session.owner = session.host = pid
        session.started_time = common.DateTime.now()
        session.session_key = secrets.token_bytes(32) if RANDOM_SESSION_KEY else bytes(32)
        self.by_gid[session.id] = session
        self.members[session.id] = []
        if fill_window > 0:
            self.deadlines[session.id] = time.monotonic() + fill_window
        return session

    def close(self, session, why=""):
        """Stop accepting players: session full, fill window over, or a client CloseParticipation."""
        timer = self.timers.pop(session.id, None)
        if timer is not None:
            timer.cancel()
        if session.open_participation:
            session.open_participation = False
            self.closed_at[session.id] = time.monotonic()
            log.info("  matchmaking closed gid=%08x (%s) with %d player(s)", session.id, why,
                     len(self.members.get(session.id, [])))

    def arm_start_timer(self, session):
        """Public mode: close the session when the fill window ends, even below 8 players."""
        if not is_public() or session.id in self.timers:
            return
        deadline = self.deadlines.get(session.id)
        if deadline is None:
            return

        async def start_with_who_is_there():
            await asyncio.sleep(max(0.0, deadline - time.monotonic()))
            if session.open_participation:
                self.close(session, "fill window over, starting with %d player(s)"
                            % len(self.members.get(session.id, [])))

        task = asyncio.get_running_loop().create_task(start_with_who_is_there())
        self.timers[session.id] = task
        TASKS.add(task)
        task.add_done_callback(TASKS.discard)

    def check_joinable(self, session, pid):
        """Public mode: a stranger cannot join a session that is closed or full, nor read it by guessing its id."""
        if not is_public() or pid in self.members.get(session.id, []):
            return
        if not session.open_participation:
            log.warning("  join refused: gid=%08x is closed (caller %08x)", session.id, pid)
            raise common.RMCError("RendezVous::SessionClosed")
        if self.is_full(session):
            log.warning("  join refused: gid=%08x is full (caller %08x)", session.id, pid)
            raise common.RMCError("RendezVous::SessionFull")

    def join(self, session, pid, count):
        members = self.members[session.id]
        if pid not in members:
            members.append(pid)
        try:
            claimed = int(count or 0)
        except (TypeError, ValueError):
            claimed = 0
        # Never trust the count the client claims (S-12): it is shown to every member.
        session.num_participants = min(max(len(members), claimed), self.capacity(session))
        return session

    def leave(self, gid, pid):
        members = self.members.get(gid)
        if members is None:
            return False
        if pid in members:
            members.remove(pid)
        if not members:
            self.forget(gid)
        elif self.by_gid[gid].owner == pid:
            self.by_gid[gid].owner = self.by_gid[gid].host = members[0]
        return True

    def get(self, gid):
        session = self.by_gid.get(gid)
        if session is None:
            raise common.RMCError("RendezVous::SessionVoid")
        return session

    def is_member(self, session, pid):
        return pid in self.members.get(session.id, [])

    def allow_host(self, session, pid, action):
        """Only the host may change or close its session (S-03); refuse quietly otherwise."""
        if session.owner == pid:
            return True
        log.warning("  %s from %08x refused: %08x owns gid=%08x", action, pid, session.owner, session.id)
        return False


SESSIONS = Sessions()


class MatchmakeExtensionServer(matchmaking.MatchmakeExtensionServer):
    async def get_playing_session(self, client, pids):
        log.info("GetPlayingSession pids=%s -> none", [f"{p:08x}" for p in pids])
        return []

    async def handle_auto_matchmake_with_param_postpone(self, client, input, output):
        param = input.extract(SplatoonAutoMatchmakeParam)
        CLIENTS[client.pid()] = client
        wanted = param.session
        log.info("AutoMatchmakeWithParamPostpone pid=%08x mode=%d attribs=%s players=%d..%d "
                 "count=%d criteria=%d options=%d app_data=%d bytes", client.pid(), wanted.game_mode,
                 wanted.attribs, wanted.min_participants, wanted.max_participants,
                 param.num_participants, len(param.search_criteria), param.options,
                 len(wanted.application_data))
        session = SESSIONS.find_open(wanted)
        if session is None:
            session = SESSIONS.create(wanted, client.pid(),
                                      fill_window=FILL_TIMEOUT if is_public() else 0.0)
        SESSIONS.join(session, client.pid(), param.num_participants)
        log.info("  -> gid=%08x owner=%08x members=%s", session.id, session.owner,
                 [f"{p:08x}" for p in SESSIONS.members[session.id]])
        if is_public():
            if SESSIONS.is_full(session):
                SESSIONS.close(session, "full")
            else:
                SESSIONS.arm_start_timer(session)
        output.add(session)
        notify_join(session, client.pid(), param.join_message)

    async def handle_create_matchmake_session_with_param(self, client, input, output):
        """Private Battle: the owner creates a room that friends join by its gid."""
        param = input.extract(LabCreateMatchmakeSessionParam)
        CLIENTS[client.pid()] = client
        wanted = param.session
        log.info("CreateMatchmakeSessionWithParam pid=%08x mode=%d attribs=%s players=%d..%d count=%d options=%d",
                 client.pid(), wanted.game_mode, wanted.attribs, wanted.min_participants,
                 wanted.max_participants, param.num_participants, param.options)
        session = SESSIONS.create(wanted, client.pid())
        SESSIONS.join(session, client.pid(), param.num_participants)
        log.info("  -> gid=%08x open=%s players=%d..%d policy=%d flags=%d state=%d key=%d bytes",
                 session.id, session.open_participation, session.min_participants,
                 session.max_participants, session.participation_policy, session.flags,
                 session.state, len(session.session_key))
        output.add(session)
        notify_join(session, client.pid(), param.join_message)

    async def handle_join_matchmake_session_with_param(self, client, input, output):
        param = input.extract(LabJoinMatchmakeSessionParam)
        CLIENTS[client.pid()] = client
        log.info("JoinMatchmakeSessionWithParam pid=%08x gid=%08x count=%d options=%d behavior=%d extra=%d bytes",
                 client.pid(), param.gid, param.num_participants, param.options, param.behavior, len(param.rest))
        session = SESSIONS.get(param.gid)
        SESSIONS.check_joinable(session, client.pid())
        if not session.open_participation:
            log.warning("  join accepted on a closed session (game expects a refusal?) gid=%08x",
                        session.id)
        if SESSIONS.is_full(session):
            log.warning("  join accepted on a full session gid=%08x", session.id)
        SESSIONS.join(session, client.pid(), 0)
        log.info("  -> members=%s open=%s participants=%d/%d host=%08x state=%d",
                 [f"{p:08x}" for p in SESSIONS.members[session.id]], session.open_participation,
                 session.num_participants, session.max_participants, session.host, session.state)
        output.add(session)
        notify_join(session, client.pid(), param.join_message)

    async def join_matchmake_session_ex(self, client, gid, message, ignore_block_list, num_participants):
        log.info("JoinMatchmakeSessionEx pid=%08x gid=%08x count=%d", client.pid(), gid, num_participants)
        CLIENTS[client.pid()] = client
        SESSIONS.check_joinable(SESSIONS.get(gid), client.pid())
        session = SESSIONS.join(SESSIONS.get(gid), client.pid(), 0)
        notify_join(session, client.pid(), message)
        return session.session_key

    async def find_matchmake_session_by_gathering_id_detail(self, client, gid):
        log.info("FindMatchmakeSessionByGatheringIdDetail gid=%08x", gid)
        session = SESSIONS.get(gid)
        # Public mode: only a member, or somebody who could still join it, may read a session (host, attributes).
        if is_public() and not SESSIONS.is_member(session, client.pid()):
            SESSIONS.check_joinable(session, client.pid())
        return session

    async def open_participation(self, client, gid):
        log.info("OpenParticipation gid=%08x", gid)
        session = SESSIONS.get(gid)
        if SESSIONS.allow_host(session, client.pid(), "OpenParticipation"):
            session.open_participation = True

    async def close_participation(self, client, gid):
        log.info("CloseParticipation gid=%08x", gid)
        session = SESSIONS.get(gid)
        if SESSIONS.allow_host(session, client.pid(), "CloseParticipation"):
            SESSIONS.close(session, "CloseParticipation from %08x" % client.pid())

    async def modify_current_game_attribute(self, client, gid, attrib, value):
        log.info("ModifyCurrentGameAttribute gid=%08x attrib=%d value=%d", gid, attrib, value)
        session = SESSIONS.get(gid)
        if not SESSIONS.allow_host(session, client.pid(), "ModifyCurrentGameAttribute"):
            return
        attribs = session.attribs
        if 0 <= attrib < len(attribs):
            attribs[attrib] = value

    async def update_progress_score(self, client, gid, score):
        log.info("UpdateProgressScore gid=%08x score=%d", gid, score)
        session = SESSIONS.get(gid)
        if not SESSIONS.is_member(session, client.pid()):
            log.warning("  UpdateProgressScore from %08x refused: not a member of gid=%08x",
                        client.pid(), gid)
            return
        session.progress_score = score


class MatchMakingServer(matchmaking.MatchMakingServer):
    async def get_session_urls(self, client, gid):
        session = SESSIONS.get(gid)
        if not SESSIONS.is_member(session, client.pid()):
            # Only participants may learn a host's address: port (S-03).
            log.warning("  GetSessionURLs from %08x refused: not a member of gid=%08x",
                        client.pid(), gid)
            raise common.RMCError("RendezVous::SessionVoid")
        urls = [url.copy() for url in STATIONS.get(session.host, [])][:2]   # PIA accepts 1 or 2 (101B5E68)
        # The registered URLs carry the NEX socket; the game's PIA socket is another one
        # (measured 27/10: the joiner sent its PIA packets to the host's NEX port). Once the
        # game calls ReplaceURL, the first URL already carries the real PIA endpoint (type 3,
        # measured in the duo logs): never override it. PEER is only the lab fallback for a
        # host whose PIA socket was never announced. Public mode never uses PEER (S-02).
        replaced = bool(urls) and urls[0]["type"] == 3
        if not is_public() and not replaced and session.host in PEER:
            for url in urls:
                url["address"], url["port"] = PEER[session.host]
        log.info("GetSessionURLs gid=%08x caller=%08x host=%08x -> %s", gid, client.pid(),
                 session.host, urls)
        return urls

    async def unregister_gathering(self, client, gid):
        log.info("UnregisterGathering gid=%08x pid=%08x", gid, client.pid())
        return SESSIONS.leave(gid, client.pid())


class MatchMakingServerExt(matchmaking.MatchMakingServerExt):
    async def end_participation(self, client, gid, message):
        log.info("EndParticipation gid=%08x pid=%08x message=%r", gid, client.pid(), message)
        return SESSIONS.leave(gid, client.pid())


class NATTraversalServer(nattraversal.NATTraversalServer):
    """Protocol 3. After its NAT check the game reports the result (method 5); probes
    between two players (methods 3, 4) are only logged for now."""
    async def report_nat_properties(self, client, natm, natf, rtt):
        origin = client.remote_address()[0] if client is not None else None
        observed = natcheck_server.consume(origin)
        log.info("ReportNATProperties pid=%08x natm=%d natf=%d rtt=%d origin=%s peer=%s",
                 client.pid(), natm, natf, rtt, origin, observed)
        if is_public():
            # Public mode: the address stays the one the player's NEX packets come from (S-02),
            # but the port comes from the NAT check, which runs from the PIA socket: it is the
            # port the player's router really maps that socket to. Without it peers guessed the
            # local port and hole punching failed behind port-changing NATs (VPN test, 30/10).
            # The observation is keyed by this same source address, so it is the player's own.
            if observed:
                NAT_PORT[client.pid()] = observed[1]
                for url in STATIONS.get(client.pid(), []):
                    if url["type"] == 3:
                        url["port"] = observed[1]
            return
        # Lab only: the NAT check runs from the PIA socket, and its observation is keyed by the
        # source address of that same host, so two players checking at once cannot mix up.
        if observed:
            PEER[client.pid()] = observed

    async def report_nat_traversal_result(self, client, cid, result):
        log.info("ReportNATTraversalResult pid=%08x cid=%d result=%s", client.pid(), cid, result)

    async def request_probe_initiation_ext(self, client, target_urls, station_to_probe):
        """A joiner asks each target (the host) to send NAT probes to it: the server
        forwards InitiateProbe (3/2) to every target's connection."""
        log.info("RequestProbeInitiationExt pid=%08x targets=%s station=%s", client.pid(), target_urls, station_to_probe)
        probe = lab_address(station_to_probe)
        if is_public() and not _probe_station_allowed(client, station_to_probe):
            log.warning("  probe request refused: station %s is not an address of %08x", station_to_probe, client.pid())
            return
        for url in target_urls:
            pid = BY_RVCID.get(url["RVCID"]) or url["PID"]
            if is_public() and not _share_a_session(client.pid(), pid):
                log.warning("  probe request refused: %08x and %08x share no session", client.pid(), pid)
                continue
            target = CLIENTS.get(pid)
            if target is None:
                log.warning("  no connection for target RVCID=%d PID=%08x", url["RVCID"], url["PID"])
                continue
            task = asyncio.get_running_loop().create_task(_initiate_probe(target, pid, probe))
            TASKS.add(task); task.add_done_callback(TASKS.discard)


def _share_a_session(a, b):
    """True when both players are members of one session (only then may one probe the other)."""
    return any(a in members and b in members for members in SESSIONS.members.values())


def _probe_station_allowed(client, station):
    """The address a player asks others to probe must be its own: where its packets come from,
    or an address it registered itself. Otherwise the game of a victim could be made to send
    probes to a third party."""
    try:
        address = station["address"]
    except (KeyError, TypeError):
        return True
    if not address:
        return True
    own = {client.remote_address()[0]}
    own.update(url["address"] for url in STATIONS.get(client.pid(), []))
    return address in own


async def _initiate_probe(target, pid, station):
    try:
        await nattraversal.NATTraversalClient(target).initiate_probe(station)
        log.info("  InitiateProbe -> %08x station=%s", pid, station)
    except Exception as error:
        log.warning("  InitiateProbe to %08x failed: %s", pid, error)


def servers():
    return [MatchmakeExtensionServer(), MatchMakingServer(), MatchMakingServerExt(), NATTraversalServer()]


# --- live connections and cleanup -----------------------------------------------
# A player opens several NEX connections (secure server, matchmaking services). State is
# dropped only when the last one closes, so a service socket closing mid-session does not
# erase the player (bug hunt B-11 / ghost sessions).
LIVE = {}              # pid -> number of open RMC connections


def connection_opened(pid):
    if pid:
        LIVE[pid] = LIVE.get(pid, 0) + 1


def connection_closed(pid):
    """One connection closed; drop every trace of the player when the last one is gone."""
    if not pid:
        return
    count = LIVE.get(pid, 1) - 1
    if count > 0:
        LIVE[pid] = count
        return
    LIVE.pop(pid, None)
    forget_player(pid)


def registered(pid):
    """A new secure Register: drop the previous connection's state (reconnect after a cut).

    Session membership is kept (several service connections may register the same player);
    only the per-connection tables and the peer cache are replaced. A stale live count left
    by a brutally cut socket is reset to this connection.
    """
    NAT_PORT.pop(pid, None)
    if not pid:
        return
    CLIENTS.pop(pid, None)
    STATIONS.pop(pid, None)
    PEER.pop(pid, None)
    for gid, owner in list(BY_RVCID.items()):
        if owner == pid:
            BY_RVCID.pop(gid, None)
    if LIVE.get(pid, 0) > 1:
        LIVE[pid] = 1


def forget_player(pid):
    """Remove a player from the live tables, its sessions and the peer cache."""
    CLIENTS.pop(pid, None)
    STATIONS.pop(pid, None)
    PEER.pop(pid, None)
    for gid, owner in list(BY_RVCID.items()):
        if owner == pid:
            BY_RVCID.pop(gid, None)
    for gid in list(SESSIONS.by_gid):
        session = SESSIONS.by_gid.get(gid)
        members = SESSIONS.members.get(gid) if session is not None else None
        if not members or pid not in members:
            continue
        members.remove(pid)
        if session.host == pid:
            SESSIONS.close(session, "host disconnected")
            SESSIONS.forget(gid)
        elif not members:
            SESSIONS.close(session, "empty")
            SESSIONS.forget(gid)
        else:
            SESSIONS.by_gid[gid].num_participants = len(members)


def active_players():
    return sorted(CLIENTS)


def stats():
    """Numbers for /metrics: players with a live connection, open sessions, connections."""
    return {"players": len(CLIENTS), "sessions": len(active_sessions()),
            "connections": sum(LIVE.values())}


KICKS = set()


def request_kick(pid):
    """Queue a kick; the server's janitor closes the connection on its event loop."""
    KICKS.add(int(pid))


async def apply_kicks():
    while KICKS:
        pid = KICKS.pop()
        client = CLIENTS.get(pid)
        LIVE.pop(pid, None)
        forget_player(pid)
        if client is not None:
            with contextlib.suppress(Exception):
                await client.close()
        log.warning("player %08x kicked by operator", pid)


def active_sessions():
    """Sessions still accepting players or in game (closed ones wait for SESSION_TTL)."""
    return [session for gid, session in SESSIONS.by_gid.items() if gid not in SESSIONS.closed_at]


def shutdown():
    """Close every session and cancel timers (clean server shutdown)."""
    global SESSIONS
    for gid in list(SESSIONS.by_gid):
        session = SESSIONS.by_gid.get(gid)
        if session is not None:
            SESSIONS.close(session, "server shutdown")
        SESSIONS.forget(gid)
    for task in list(TASKS):
        task.cancel()
    TASKS.clear()
    SESSIONS = Sessions()
    CLIENTS.clear()
    STATIONS.clear()
    PEER.clear()
    BY_RVCID.clear()
    LIVE.clear()
    KICKS.clear()
