"""Online sign-in and launch, for Linux (works on Windows too; there Play-Online.bat keeps the
PowerShell screen). Same steps as Splatoon-Login.ps1 + Launch-Online.ps1:

  1. pseudo (2-10 characters) and game language, saved in Game/settings.ini (other lines kept);
  2. server from scripts/server.txt, else scripts/server.public.txt (server=, nncs1=, nncs2=, register=);
  3. /health of the register= server: min_client_version, and latest_version when integrity_mode
     is "enforce" -> an outdated game is stopped here with a clear message;
  4. first PLAY on a server: creates Game/online_identity.ini (runtime/online_identity.h format)
     and signs it up once (POST register=), then claims the pseudo (POST /social/pseudo);
  5. starts the local lab server when no remote one is set, the SpotPass tools, the game (from the
     Game folder: the Linux port reads settings.ini from its working directory) and the friends relay.

    python3 scripts/splatoon_login.py                 window (tkinter) or, without one, terminal prompts
    python3 scripts/splatoon_login.py --cli --pseudo Ana [--yes] [--offline] [--diag]
Only the standard library is used (Python 3.10+).
"""
import argparse
import datetime
import ipaddress
import json
import locale
import os
import re
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")
PSEUDO_RE = re.compile(r"^[A-Za-z0-9 _.\-]{2,10}$")
SERVER_RE = re.compile(r"^[A-Za-z0-9.\-]{1,253}:\d{1,5}$")
REGISTER_RE = re.compile(r"^(https://[A-Za-z0-9.\-]+(:\d{1,5})?(/\S*)?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/\S*)?)$")
SOCIAL_RE = re.compile(r"^(https://[A-Za-z0-9.\-]+(:\d{1,5})?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?)$")
VERSION_FILES = (os.path.join("build", "installed-version.txt"), "VERSION")

# Wii U system language numbers (settings.ini language=), offered when the player's own dump has
# content/Message/CommonMsg_<region><lang>.szs.
LANGUAGES = {"ja": (0, "日本語"), "en": (1, "English"), "fr": (2, "Français"), "de": (3, "Deutsch"),
             "it": (4, "Italiano"), "es": (5, "Español"), "ko": (7, "한국어"), "nl": (8, "Nederlands"),
             "pt": (9, "Português"), "ru": (10, "Русский")}

FRENCH = {
    "PLAY": "JOUER",
    "Play offline": "Jouer hors ligne",
    "Pseudo": "Pseudo",
    "Language": "Langue",
    "Server: {0}": "Serveur : {0}",
    "2 to 10 characters: letters, digits, space, _ - .": "2 à 10 caractères : lettres, chiffres, espace, _ - .",
    "The server cannot be reached. Check your Internet connection.": "Serveur injoignable. Vérifiez votre connexion Internet.",
    "Too many sign-ups from your connection. Try again later.": "Trop d'inscriptions depuis votre connexion. Réessayez plus tard.",
    "This server needs version {0} or newer. Update the game, then try again.":
        "Ce serveur demande la version {0} ou plus récente. Mettez le jeu à jour, puis réessayez.",
    "The server file has an invalid value ({0}). Fix or delete scripts/server.txt.":
        "Le fichier du serveur a une valeur invalide ({0}). Corrigez ou supprimez scripts/server.txt.",
    "You are about to sign up on another server ({0}). Your player number and its secret will be sent to it. "
    "Only continue if you trust its owner.\n\nContinue?":
        "Vous allez vous inscrire sur un autre serveur ({0}). Votre numéro de joueur et son secret lui seront "
        "envoyés. Continuez seulement si vous faites confiance à son propriétaire.\n\nContinuer ?",
    "This account exists with another password. Keep your old online_identity.ini.":
        "Ce compte existe avec un autre mot de passe. Gardez votre ancien online_identity.ini.",
    "This account is banned from the server.": "Ce compte est banni du serveur.",
    "This pseudo is already taken by another player. Choose a different one.":
        "Ce pseudo est déjà pris par un autre joueur. Choisissez-en un autre.",
    "Sign-up refused by the server ({0}).": "Inscription refusée par le serveur ({0}).",
    "The server sign-up must use https.": "L'inscription au serveur doit utiliser https.",
    "Game/online_identity.ini is damaged.": "Game/online_identity.ini est endommagé.",
    "The game is not built yet: run setup.sh first.": "Le jeu n'est pas encore compilé : lancez d'abord setup.sh.",
}


class Paths:
    def __init__(self, root=ROOT, game=None):
        self.root = root
        self.scripts = os.path.join(root, "scripts")
        self.game = game or os.path.join(root, "Game")
        self.settings = os.path.join(self.game, "settings.ini")
        self.identity = os.path.join(self.game, "online_identity.ini")
        self.marker = os.path.join(self.game, "online_registered.txt")
        self.logs = os.path.join(root, "build", "diagnostics")


def find_executable(game_dir):
    """setup.sh installs Game/Splatoon, Setup.ps1 Game\\Splatoon.exe, older installs Splatoon-v288.exe:
    the newest existing one, or None."""
    names = ("Splatoon.exe", "Splatoon-v288.exe") if os.name == "nt" else ("Splatoon", "Splatoon-v288")
    found = [os.path.join(game_dir, n) for n in names if os.path.isfile(os.path.join(game_dir, n))]
    return max(found, key=os.path.getmtime) if found else None


# --- settings.ini ------------------------------------------------------------------------------
def read_lines(path):
    try:
        with open(path, encoding="utf-8-sig") as handle:
            return handle.read().splitlines()
    except OSError:
        return []


def get_setting(path, key):
    for line in read_lines(path):
        match = re.match(rf"^\s*{re.escape(key)}\s*=\s*(.*)$", line)
        if match:
            return match.group(1).strip()
    return ""


def write_atomic(path, text):
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    os.replace(temporary, path)


def save_setting(path, key, value):
    lines = [l for l in read_lines(path) if not re.match(rf"^\s*{re.escape(key)}\s*=", l)]
    lines.append(f"{key}={value}")
    write_atomic(path, "\n".join(lines) + "\n")


def valid_pseudo(name):
    return bool(PSEUDO_RE.match(name))


def installed_languages(game_dir):
    found = {}
    try:
        names = os.listdir(os.path.join(game_dir, "content", "Message"))
    except OSError:
        names = []
    for name in names:
        match = re.match(r"^CommonMsg_[A-Z]{2}([a-z]{2})\.szs$", name)
        if match and match.group(1) in LANGUAGES:
            code, label = LANGUAGES[match.group(1)]
            found[code] = (match.group(1), code, label)
    return [found[c] for c in sorted(found)]


def start_language(languages, saved):
    """Saved choice, else the system language, else English, else the first one installed."""
    if not languages:
        return None
    for entry in languages:
        if str(entry[1]) == saved:
            return entry
    system = (os.environ.get("LANG") or (locale.getlocale()[0] or ""))[:2].lower()
    for tag in (system, "en"):
        for entry in languages:
            if entry[0] == tag:
                return entry
    return languages[0]


# --- server file -------------------------------------------------------------------------------
def valid_ipv4(text):
    if not re.match(r"^\d{1,3}(\.\d{1,3}){3}$", text):
        return False
    return all(int(part) <= 255 for part in text.split("."))


def read_server_file(scripts_dir):
    """scripts/server.txt (local, never committed) wins over scripts/server.public.txt. Returns
    (config, custom, bad keys): a bad file is refused, not half-used."""
    cfg, custom = {}, False
    for name in ("server.txt", "server.public.txt"):
        path = os.path.join(scripts_dir, name)
        if not os.path.isfile(path):
            continue
        for line in read_lines(path):
            match = re.match(r"^\s*(\w+)\s*=\s*(\S+)\s*$", line)
            if match:
                cfg[match.group(1).lower()] = match.group(2)
        custom = name == "server.txt"
        break
    bad = []
    if cfg.get("server") and not SERVER_RE.match(cfg["server"]):
        bad.append("server")
    bad += [k for k in ("nncs1", "nncs2") if cfg.get(k) and not valid_ipv4(cfg[k])]
    if cfg.get("register") and not REGISTER_RE.match(cfg["register"]):
        bad.append("register")
    return cfg, custom, bad


def official_register(scripts_dir):
    for line in read_lines(os.path.join(scripts_dir, "server.public.txt")):
        match = re.match(r"^\s*register\s*=\s*(\S+)\s*$", line)
        if match:
            return match.group(1)
    return ""


def needs_foreign_confirmation(cfg, paths):
    """True when register= is neither the shipped public server nor one already accepted."""
    url = cfg.get("register")
    if not url:
        return False
    if read_lines(paths.marker)[:1] == [url]:
        return False
    official = official_register(paths.scripts)
    return not (official and url == official)


# --- HTTP --------------------------------------------------------------------------------------
def http_json(method, url, body=None, timeout=8):
    """(status, decoded JSON or {}); status 0 when the server cannot be reached."""
    data = json.dumps(body, separators=(",", ":")).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, method=method,
                                     headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as reply:
            status, raw = reply.status, reply.read()
    except urllib.error.HTTPError as error:
        status, raw = error.code, error.read()
    except (urllib.error.URLError, OSError, ValueError):
        return 0, {}
    try:
        return status, json.loads(raw.decode("utf-8") or "{}")
    except ValueError:
        return status, {}


def base_url(url):
    parts = urllib.parse.urlsplit(url)
    return f"{parts.scheme}://{parts.netloc}"


def parse_version(text):
    """"0.2" and "0.2.0" are the same version; None when it is not one."""
    match = re.match(r"^v?(\d+)\.(\d+)(?:\.(\d+))?$", str(text or "").strip())
    return (int(match.group(1)), int(match.group(2)), int(match.group(3) or 0)) if match else None


def installed_version(root):
    for rel in VERSION_FILES:
        version = parse_version(" ".join(read_lines(os.path.join(root, rel))))
        if version:
            return version
    return None


def check_server_version(cfg, installed, http=http_json):
    """'' when this game is recent enough or the server cannot be asked (sign-up reports that)."""
    if not cfg.get("register"):
        return ""
    status, reply = http("GET", base_url(cfg["register"]) + "/health", timeout=4)
    if status != 200 or not isinstance(reply, dict):
        return ""
    minimum = parse_version(reply.get("min_client_version"))
    if reply.get("integrity_mode") == "enforce":
        latest = parse_version(reply.get("latest_version"))
        if latest and (not minimum or latest > minimum):
            minimum = latest
    if minimum and installed and installed < minimum:
        return ("This server needs version {0} or newer. Update the game, then try again.", ".".join(map(str, minimum)))
    return ""


def read_identity(path):
    fields = {}
    for line in read_lines(path):
        match = re.match(r"^\s*(pid|password)\s*=\s*(\S+)\s*$", line)
        if match:
            fields[match.group(1)] = match.group(2)
    return fields


def ensure_identity(path):
    """runtime/online_identity.h format: pid 0x10000000-0x7FFFFFFE, 16 letters or digits. Written
    aside then moved: two launches at once cannot leave two different identities."""
    if os.path.isfile(path):
        return
    alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    principal = 0x10000000 + secrets.randbelow(0x6FFFFFFF)
    password = "".join(secrets.choice(alphabet) for _ in range(16))
    text = (f"# Splatoon PC online identity. Keep this file: it identifies you to friends and rooms.\n"
            f"pid={principal:08x}\npassword={password}\n")
    temporary = f"{path}.{os.getpid()}.tmp"
    with open(temporary, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    try:
        if os.path.exists(path):
            raise FileExistsError(path)
        os.replace(temporary, path)
    except OSError:
        os.remove(temporary)


def register_online(cfg, paths, http=http_json):
    url = cfg.get("register")
    if not url:
        return ""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" and parts.hostname not in ("127.0.0.1", "localhost"):
        return "The server sign-up must use https."
    if os.path.isfile(paths.identity) and read_lines(paths.marker)[:1] == [url]:
        return ""
    ensure_identity(paths.identity)
    fields = read_identity(paths.identity)
    if not fields.get("pid") or not fields.get("password"):
        return "Game/online_identity.ini is damaged."
    status, _ = http("POST", url, {"pid": "0x" + fields["pid"], "password": fields["password"]}, timeout=10)
    if status == 0:
        return "The server cannot be reached. Check your Internet connection."
    if status >= 400:
        return {409: "This account exists with another password. Keep your old online_identity.ini.",
                429: "Too many sign-ups from your connection. Try again later.",
                403: "This account is banned from the server."}.get(status, ("Sign-up refused by the server ({0}).", status))
    write_atomic(paths.marker, url + "\n")
    return ""


def claim_pseudo(cfg, paths, pseudo, http=http_json):
    """Only "taken" stops the player; any other problem lets the game start without friends."""
    if not cfg.get("register"):
        return ""
    fields = read_identity(paths.identity)
    if not fields.get("pid") or not fields.get("password"):
        return ""
    status, _ = http("POST", base_url(cfg["register"]) + "/social/pseudo",
                     {"pid": "0x" + fields["pid"], "password": fields["password"], "pseudo": pseudo})
    return "This pseudo is already taken by another player. Choose a different one." if status == 409 else ""


def prepare_online(cfg, paths, pseudo, http=http_json):
    """Steps 3-4 after the server file was accepted: '' or a message (text or (text, arg))."""
    return (check_server_version(cfg, installed_version(paths.root), http)
            or register_online(cfg, paths, http)
            or claim_pseudo(cfg, paths, pseudo, http))


# --- launch ------------------------------------------------------------------------------------
def udp_port_free(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def nexserver_running():
    """Linux: a nexserver.py already in the process list (its command line in /proc)."""
    try:
        entries = os.listdir("/proc")
    except OSError:
        return False
    for entry in entries:
        if entry.isdigit():
            try:
                with open(f"/proc/{entry}/cmdline", "rb") as handle:
                    if b"nexserver.py" in handle.read():
                        return True
            except OSError:
                continue
    return False


def game_environment(cfg, paths, online, diag, run, port=60000):
    env = dict(os.environ)
    env["RBREW_STUB_REPORT"] = os.path.join(paths.logs, f"online-{run}-stubs.txt")
    if os.name == "nt":
        env["SDL_DIRECTINPUT_ENABLED"] = "0"
    if not online:
        return env
    env["RBREW_ONLINE_SERVER"] = cfg.get("server") or f"127.0.0.1:{port}"
    env["RBREW_FRIENDS_DIR"] = os.path.join(paths.game, "friends")
    env["RBREW_DNS_OVERRIDE"] = (f"nncs1.app.nintendowifi.net={cfg.get('nncs1') or '127.0.0.1'},"
                                 f"nncs2.app.nintendowifi.net={cfg.get('nncs2') or '127.0.0.2'}")
    if not get_setting(paths.settings, "pseudo"):
        env["RBREW_PSEUDO"] = "Player"      # never the system user name, which others would see
    if diag:
        for key, suffix in (("RBREW_INPUT_RECORD", "input"), ("RBREW_SCAN_TRACE", "scan"),
                            ("RBREW_NET_TRACE", "net"), ("RBREW_IMPORT_TRACE", "imports")):
            env[key] = os.path.join(paths.logs, f"online-{run}-{suffix}.txt")
        env["RBREW_NET_PROBE"] = "0209674C"
    return env


def launch(cfg, paths, online, diag=False, port=60000):
    exe = find_executable(paths.game)
    if not exe:
        raise RuntimeError("The game is not built yet: run setup.sh first.")
    os.makedirs(paths.logs, exist_ok=True)
    run = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = os.path.join(paths.logs, f"online-{run}-launcher.log")

    def note(text):
        with open(log_path, "a", encoding="utf-8") as handle:
            handle.write(f"{datetime.datetime.now():%H:%M:%S} {text}\n")
    cutoff = time.time() - 7 * 86400          # old run logs never fill the disk
    for name in os.listdir(paths.logs):
        full = os.path.join(paths.logs, name)
        if name.startswith("online-") and os.path.isfile(full) and os.path.getmtime(full) < cutoff:
            try:
                os.remove(full)
            except OSError:
                pass
    children = []
    try:
        if online and not cfg.get("server") and not nexserver_running():
            if not (udp_port_free(port) and udp_port_free(port + 1)):
                raise RuntimeError(f"UDP port {port} or {port + 1} is already in use; close the program using it.")
            server_log = os.path.join(paths.logs, f"online-{run}-server.log")
            server = subprocess.Popen([sys.executable, os.path.join(paths.root, "tools", "nexlab", "nexserver.py"),
                                       "--port", str(port), "--log", server_log], cwd=paths.root,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            children.append(server)
            time.sleep(2)
            if server.poll() is not None:
                raise RuntimeError(f"The NEX lab server did not start; see {server_log}")
        if online:
            os.makedirs(os.path.join(paths.game, "friends"), exist_ok=True)
            with open(os.path.join(paths.logs, f"online-{run}-schedule.log"), "a") as schedule:
                for tool in ("make_vssetting.py", "make_festival.py"):
                    code = subprocess.call([sys.executable, os.path.join(paths.root, "tools", "boss", tool)],
                                           cwd=paths.root, stdout=schedule, stderr=subprocess.STDOUT)
                    if code:
                        note(f"{tool} failed (exit {code})")
        with open(os.path.join(paths.logs, f"online-{run}-stdout.log"), "wb") as out, \
             open(os.path.join(paths.logs, f"online-{run}-stderr.log"), "wb") as err:
            # cwd = Game: the Linux port reads settings.ini (and next to it online_identity.ini) there.
            game = subprocess.Popen([exe], cwd=paths.game, env=game_environment(cfg, paths, online, diag, run, port),
                                    stdout=out, stderr=err)
            note(f"Game started ({'server ' + (cfg.get('server') or 'local') if online else 'offline'})")
            social = base_url(cfg["register"]) if cfg.get("register") else ""
            if online and SOCIAL_RE.match(social):
                children.append(subprocess.Popen(
                    [sys.executable, os.path.join(paths.scripts, "friends_relay.py"), "--game-dir", paths.game,
                     "--server", social, "--watch-pid", str(game.pid)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            code = game.wait()
        write_atomic(os.path.join(paths.logs, f"online-{run}-exit.txt"), f"{code}\n")
        note(f"Game exited with code {code}")
        return code
    except Exception as error:
        note(f"ERROR: {error}")
        raise
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()


# --- screens -----------------------------------------------------------------------------------
class Texts:
    def __init__(self):
        self.french = False

    def __call__(self, message):
        if isinstance(message, tuple):
            text, *args = message
            return (FRENCH.get(text, text) if self.french else text).format(*args)
        return FRENCH.get(message, message) if self.french else message


def play_checks(paths, pseudo, ask):
    """Everything PLAY does before the launch: '' or a message. ask(question) -> bool."""
    cfg, _, bad = read_server_file(paths.scripts)
    if bad:
        return cfg, ("The server file has an invalid value ({0}). Fix or delete scripts/server.txt.", ", ".join(bad))
    if needs_foreign_confirmation(cfg, paths):
        question = ("You are about to sign up on another server ({0}). Your player number and its secret will be "
                    "sent to it. Only continue if you trust its owner.\n\nContinue?",
                    urllib.parse.urlsplit(cfg["register"]).netloc)
        if not ask(question):
            return cfg, None
    return cfg, prepare_online(cfg, paths, pseudo)


def run_cli(paths, args, t):
    languages = installed_languages(paths.game)
    language = start_language(languages, get_setting(paths.settings, "language"))
    pseudo = (args.pseudo or "").strip()
    while not valid_pseudo(pseudo):
        if args.pseudo is not None or not sys.stdin.isatty():
            print(t("2 to 10 characters: letters, digits, space, _ - ."), file=sys.stderr)
            return 2
        saved = get_setting(paths.settings, "pseudo")
        pseudo = (input(f"{t('Pseudo')} [{saved}]: ").strip() or saved)
    save_setting(paths.settings, "pseudo", pseudo)
    if language:
        save_setting(paths.settings, "language", str(language[1]))
    if args.offline:
        return launch({}, paths, online=False, diag=args.diag)

    def ask(question):
        if args.yes:
            return True
        if not sys.stdin.isatty():
            return False
        return input(t(question) + " [y/N] ").strip().lower() in ("y", "yes", "o", "oui")
    cfg, message = play_checks(paths, pseudo, ask)
    if message is None:
        return 1
    if message:
        print(t(message), file=sys.stderr)
        return 1
    return launch(cfg, paths, online=True, diag=args.diag)


def run_window(paths, args, t, tk):
    from tkinter import messagebox, ttk
    languages = installed_languages(paths.game)
    current = start_language(languages, get_setting(paths.settings, "language"))
    t.french = bool(current and current[0] == "fr")
    root = tk.Tk()
    root.title("Splatoon")
    root.resizable(False, False)
    frame = ttk.Frame(root, padding=16)
    frame.grid()
    pseudo_label = ttk.Label(frame)
    pseudo_label.grid(row=0, column=0, sticky="w")
    box = ttk.Entry(frame, width=24)
    box.insert(0, get_setting(paths.settings, "pseudo"))
    box.grid(row=1, column=0, columnspan=2, sticky="we", pady=(2, 8))
    language_label = ttk.Label(frame)
    language_label.grid(row=2, column=0, sticky="w")
    names = [entry[2] for entry in languages]
    chooser = ttk.Combobox(frame, values=names, state="readonly", width=21)
    if current:
        chooser.set(current[2])
    chooser.grid(row=3, column=0, columnspan=2, sticky="we", pady=(2, 8))
    problem = ttk.Label(frame, foreground="#d9534f", wraplength=260)
    problem.grid(row=4, column=0, columnspan=2, sticky="w")
    play = ttk.Button(frame)
    play.grid(row=5, column=0, columnspan=2, sticky="we", pady=6)
    offline = ttk.Button(frame)
    offline.grid(row=6, column=0, columnspan=2, sticky="we")
    cfg, custom, _ = read_server_file(paths.scripts)
    server = ttk.Label(frame, foreground="#ffc857" if custom else "#888888")
    server.grid(row=7, column=0, sticky="w", pady=(8, 0))
    version = installed_version(paths.root)
    ttk.Label(frame, text="v" + ".".join(map(str, version)) if version else "", foreground="#888888").grid(row=8, column=0, sticky="w")
    choice = {}

    def set_texts():
        pseudo_label.config(text=t("Pseudo"))
        language_label.config(text=t("Language"))
        play.config(text=t("PLAY"))
        offline.config(text=t("Play offline"))
        server.config(text=t(("Server: {0}", (cfg.get("server") or "local").split(":")[0])))

    def selected_language():
        return next((entry for entry in languages if entry[2] == chooser.get()), None)

    def on_language(_event=None):
        entry = selected_language()
        t.french = bool(entry and entry[0] == "fr")
        set_texts()

    def confirm_pseudo():
        name = box.get().strip()
        if not valid_pseudo(name):
            problem.config(text=t("2 to 10 characters: letters, digits, space, _ - ."))
            return None
        save_setting(paths.settings, "pseudo", name)
        entry = selected_language()
        if entry:
            save_setting(paths.settings, "language", str(entry[1]))
        return name

    def on_play():
        name = confirm_pseudo()
        if not name:
            return
        root.config(cursor="watch")
        root.update()
        result_cfg, message = play_checks(paths, name, lambda q: messagebox.askyesno("Splatoon", t(q), icon="warning"))
        root.config(cursor="")
        if message is None:
            return
        if message:
            problem.config(text=t(message))
            return
        choice.update(mode="online", cfg=result_cfg)
        root.destroy()

    def on_offline():
        if confirm_pseudo():
            choice.update(mode="offline", cfg={})
            root.destroy()
    chooser.bind("<<ComboboxSelected>>", on_language)
    box.bind("<KeyRelease>", lambda _e: problem.config(text=""))
    box.bind("<Return>", lambda _e: on_play())
    play.config(command=on_play)
    offline.config(command=on_offline)
    set_texts()
    box.focus_set()
    root.mainloop()
    if not choice:
        return 0
    try:
        return launch(choice["cfg"], paths, online=choice["mode"] == "online", diag=args.diag)
    except Exception as error:
        tk.Tk().withdraw()
        messagebox.showerror("Splatoon", f"The online launch failed:\n\n{t(str(error))}\n\nLog: {paths.logs}")
        return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="Splatoon online sign-in and launch (see the top of this file)")
    parser.add_argument("--cli", action="store_true", help="terminal prompts instead of a window")
    parser.add_argument("--pseudo", help="pseudo to use (no prompt)")
    parser.add_argument("--offline", action="store_true", help="start the game offline")
    parser.add_argument("--yes", action="store_true", help="accept signing up on a non-public server")
    parser.add_argument("--diag", action="store_true", help="network/import traces in build/diagnostics")
    parser.add_argument("--game", help="the game folder (default: Game next to scripts/)")
    parser.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    paths = Paths(os.path.abspath(args.root), os.path.abspath(args.game) if args.game else None)
    t = Texts()
    language = start_language(installed_languages(paths.game), get_setting(paths.settings, "language"))
    t.french = bool(language and language[0] == "fr")
    if not find_executable(paths.game):
        print(t("The game is not built yet: run setup.sh first."), file=sys.stderr)
        return 1
    tk = None
    if not (args.cli or args.pseudo is not None) and (os.name == "nt" or os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        try:
            import tkinter as tk
        except ImportError:
            tk = None
    try:
        return run_window(paths, args, t, tk) if tk else run_cli(paths, args, t)
    except RuntimeError as error:
        print(t(str(error)), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
