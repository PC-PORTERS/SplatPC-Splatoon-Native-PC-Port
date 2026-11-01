"""Settings for the NEX lab/public server.

Defaults reproduce the lab server exactly: bound to 127.0.0.1, auth on 60000, secure on
60001, NAT check on the loopback aliases, identities read from online_identity.ini. A JSON
file (or command-line switches) switches to public mode, where the server stops rewriting
player addresses to 127.x and announces the address the packets really come from.

Every key is optional. `_`-prefixed keys are ignored, so an example file can carry notes:

    {
      "_help": "lab = same PC, public = real Internet addresses",
      "mode": "lab",                    # lab | public
      "listen": "127.0.0.1",            # bind address, 0.0.0.0 accepts every interface
      "advertise": "203.0.113.10",      # address written into station URLs (needed with 0.0.0.0)
      "auth_port": 60000,
      "secure_port": 60001,             # default: auth_port + 1
      "natcheck": true,                 # default: true in lab, false in public (step 4 completes it)
      "natcheck_addresses": ["127.0.0.1", "127.0.0.2"],
      "identities": ["path/to/online_identity.ini"],
      "identity_roots": ["path/to/dir"],
      "accounts_db": "nexlab-accounts.sqlite3",   # public mode: accounts.py store
      "registration": "open",           # public: open | token | closed (default: closed)
      "registration_port": 60002,       # default: auth_port + 2 (0 when closed)
      "registration_token": ""          # required when registration is "token"
    }
"""
import ipaddress
import json
import pathlib
import re

MODES = ("lab", "public")
WILDCARDS = ("", "0.0.0.0", "::")
REGISTRATIONS = ("open", "token", "closed")
FIELDS = ("mode", "listen", "advertise", "auth_port", "secure_port", "natcheck",
          "natcheck_addresses", "identities", "identity_roots",
          "accounts_db", "registration", "registration_port", "registration_token",
          "fill_timeout", "natcheck_single_ip", "natcheck_same_socket", "registration_proxy",
          "min_client_version", "random_session_key")


class ConfigError(ValueError):
    """A configuration value is missing, out of range, or inconsistent."""


def _ip(value, field):
    try:
        return str(ipaddress.ip_address(str(value).strip()))
    except ValueError:
        raise ConfigError(f"{field}: '{value}' is not an IPv4/IPv6 address") from None


def _host(value, field):
    text = str(value).strip()
    try:
        return str(ipaddress.ip_address(text))
    except ValueError:
        pass
    if not text or any(c.isspace() for c in text) or "/" in text or "://" in text:
        raise ConfigError(f"{field}: '{value}' is not a host name or address")
    return text.lower()


def _port(value, field):
    try:
        number = int(value)
    except (TypeError, ValueError):
        raise ConfigError(f"{field}: '{value}' is not a port number") from None
    if not 1 <= number <= 65535:
        raise ConfigError(f"{field}: {number} is outside 1..65535")
    return number


def _loopback(value):
    """True for 127.0.0.0/8 and localhost names: addresses that never leave this PC."""
    if not value:
        return False
    text = str(value).strip().lower()
    if text == "localhost" or text.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(text).is_loopback
    except ValueError:
        return False


class ServerConfig:
    def __init__(self, **values):
        unknown = sorted(set(values) - set(FIELDS) - {k for k in values if k.startswith("_")})
        if unknown:
            raise ConfigError(f"unknown setting(s): {', '.join(unknown)}")
        self._provided = {key for key in values if key in FIELDS}
        self.mode = values.get("mode", "lab")
        # A public server must be reachable, so an unspecified listen means every interface;
        # the lab keeps its loopback default. An explicit loopback in public mode is still
        # refused by validate() (B-03), and advertise becomes mandatory with 0.0.0.0.
        self.listen = values.get("listen", "127.0.0.1"
                                 if str(self.mode).strip().lower() != "public" else "0.0.0.0")
        self.advertise = values.get("advertise")
        self.auth_port = values.get("auth_port", 60000)
        self.secure_port = values.get("secure_port")
        self.natcheck = values.get("natcheck")
        self.natcheck_addresses = values.get("natcheck_addresses")
        # Explicit opt-in: answer the NAT check from ONE public IP (the "type 2" probe, which
        # needs a second IP, stays unanswered). Off by default: Pretendo's server uses two.
        self.natcheck_single_ip = bool(values.get("natcheck_single_ip", False))
        # Explicit opt-in: answer every NAT check probe from the socket that received it (see
        # natcheck_server.NatCheck). Needed when players sit behind routers that drop replies
        # coming from another port or IP.
        self.natcheck_same_socket = bool(values.get("natcheck_same_socket", False))
        # Registration behind a local HTTPS proxy (Caddy): bind 127.0.0.1 and take the player's
        # address from the proxy's X-Forwarded-For, so the per-address limits still apply.
        self.registration_proxy = bool(values.get("registration_proxy", False))
        # Oldest game version this server accepts ("0.2.0"); published in /health, and the player's
        # launcher refuses to start an online game below it. Empty: every version is accepted.
        # Public servers: hand out a random 32-byte session key (zeros otherwise). Off until a duo test shows the game takes it.
        self.random_session_key = bool(values.get("random_session_key", False))
        self.min_client_version = values.get("min_client_version") or None
        if self.min_client_version is not None and not (
                isinstance(self.min_client_version, str)
                and re.fullmatch(r"\d+\.\d+(\.\d+)?", self.min_client_version.strip())):
            raise ConfigError(f"min_client_version: '{self.min_client_version}' is not a version like 0.2.0")
        self.identities = list(values.get("identities") or [])
        roots = values.get("identity_roots")
        self.identity_roots = None if roots is None else [str(r) for r in roots]
        self.accounts_db = values.get("accounts_db")
        self.registration = values.get("registration")
        self.registration_port = values.get("registration_port")
        self.registration_token = values.get("registration_token")
        self.fill_timeout = values.get("fill_timeout", 20.0)
        self._normalize()
        self.validate()

    # -- construction ------------------------------------------------------
    @classmethod
    def load(cls, path):
        path = pathlib.Path(path)
        try:
            text = path.read_text(encoding="utf-8-sig")
        except OSError as error:
            raise ConfigError(f"cannot read config file {path}: {error.strerror or error}") from None
        try:
            data = json.loads(text)
        except json.JSONDecodeError as error:
            raise ConfigError(f"{path}: invalid JSON at line {error.lineno}: {error.msg}") from None
        if not isinstance(data, dict):
            raise ConfigError(f"{path}: the config file must contain a JSON object")
        return cls(**data)

    def updated(self, **changes):
        """Copy with some values replaced (None means 'keep' here, as for the CLI overrides).

        When the mode changes, settings that only had a mode-dependent default (NAT check,
        registration) and were never set explicitly fall back to the new mode's default:
        `--public` must not drag the lab's NAT check into a public server.
        """
        clean = {key: value for key, value in changes.items() if value is not None}
        values = {field: getattr(self, field) for field in FIELDS}
        provided = set(self._provided)
        mode_changed = "mode" in clean and str(clean["mode"]).strip().lower() != self.mode
        if mode_changed:
            for dependent in ("natcheck", "registration"):
                if dependent not in clean and dependent not in provided:
                    values[dependent] = None
                    provided.discard(dependent)
            # The default bind address is mode-dependent too: public listens everywhere,
            # the lab stays on loopback. An explicit listen (file or CLI) is kept as is.
            if "listen" not in clean and "listen" not in provided:
                values["listen"] = ("127.0.0.1"
                                    if str(clean["mode"]).strip().lower() != "public" else "0.0.0.0")
                provided.discard("listen")
        values.update(clean)
        provided |= set(clean)
        config = ServerConfig(**values)
        config._provided = provided
        return config

    # -- validation --------------------------------------------------------
    def _normalize(self):
        self.mode = str(self.mode).strip().lower()
        if self.natcheck is None:
            self.natcheck = self.mode == "lab"
        if self.advertise is not None:
            self.advertise = str(self.advertise).strip()
        if self.registration is None:
            # S-07: a public server starts with registration closed; the operator opens it
            # deliberately ("open", or "token" with a shared secret).
            self.registration = "closed"
        if self.accounts_db is None:
            self.accounts_db = "nexlab-accounts.sqlite3"
        if self.registration_token is not None:
            self.registration_token = str(self.registration_token)
        try:
            self.fill_timeout = float(self.fill_timeout)
        except (TypeError, ValueError):
            raise ConfigError(f"fill_timeout: '{self.fill_timeout}' is not a number of seconds") from None

    def validate(self):
        if self.mode not in MODES:
            raise ConfigError(f"mode: '{self.mode}' is not one of {', '.join(MODES)}")
        if self.listen not in WILDCARDS:
            self.listen = _ip(self.listen, "listen")
        if self.advertise:
            self.advertise = _host(self.advertise, "advertise")
        elif self.listen in WILDCARDS:
            raise ConfigError("advertise: required when listen is 0.0.0.0 (players must be told "
                              "which address to connect back to)")
        self.auth_port = _port(self.auth_port, "auth_port")
        self.secure_port = _port(self.auth_port + 1 if self.secure_port is None else self.secure_port,
                                 "secure_port")
        if self.auth_port == self.secure_port:
            raise ConfigError("secure_port must differ from auth_port")
        if not self.fill_timeout > 0:
            raise ConfigError("fill_timeout: must be a positive number of seconds")
        if self.natcheck_addresses is not None:
            self.natcheck_addresses = [_ip(address, "natcheck_addresses")
                                       for address in self.natcheck_addresses]
            if not self.natcheck_addresses:
                raise ConfigError("natcheck_addresses: give at least one address, or remove the key")
        if self.natcheck and self.mode == "public":
            # Pretendo's NAT check server states it plainly: nncs1 and nncs2 must be two
            # different public IPs, each on UDP 10025 and 10125. The lab uses 127.0.0.1 and
            # 127.0.0.2; a public server needs two real addresses (two IPs or two hosts).
            single = self.natcheck_single_ip and self.natcheck_addresses and len(self.natcheck_addresses) == 1
            if not single:
                if not self.natcheck_addresses or len(self.natcheck_addresses) != 2:
                    raise ConfigError("natcheck_addresses: public mode needs exactly two different IPs "
                                      "for nncs1/nncs2 (each on UDP 10025 and 10125), or natcheck=false "
                                      "(or one IP with natcheck_single_ip=true)")
                if self.natcheck_addresses[0] == self.natcheck_addresses[1]:
                    raise ConfigError("natcheck_addresses: nncs1 and nncs2 cannot share one IP")
        if self.mode == "public":
            # A loopback address in public mode can never work: players would be told to
            # connect to their own PC, or the NAT check would answer locally (B-03).
            for field, value in (("listen", self.listen), ("advertise", self.advertise)):
                if _loopback(value):
                    raise ConfigError(f"{field}: a public server cannot use the loopback "
                                      f"address '{value}' (players could not reach it)")
            for address in self.natcheck_addresses or []:
                if _loopback(address):
                    raise ConfigError("natcheck_addresses: public mode cannot use the loopback "
                                      f"address '{address}' (the NAT check must be reachable)")
        self.identities = [str(path) for path in self.identities]
        self.accounts_db = str(self.accounts_db)
        if self.registration not in REGISTRATIONS:
            raise ConfigError(f"registration: '{self.registration}' is not one of {', '.join(REGISTRATIONS)}")
        if self.registration == "token" and not self.registration_token:
            raise ConfigError("registration_token: required when registration is 'token'")
        if self.registration == "closed":
            self.registration_port = 0
        else:
            if not self.registration_port:      # None, or the 0 left by a closed config
                self.registration_port = self.auth_port + 2
            self.registration_port = _port(self.registration_port, "registration_port")
        return self

    # -- helpers -----------------------------------------------------------
    def is_public(self):
        return self.mode == "public"

    def station_host(self):
        """Address written into station URLs: the advertised one, else the bound one."""
        return self.advertise or ("" if self.listen in WILDCARDS else self.listen)

    def describe(self):
        return (f"mode={self.mode} listen={self.listen} advertise={self.station_host()} "
                f"auth={self.auth_port} secure={self.secure_port} natcheck={self.natcheck} "
                f"registration={self.registration}"
                + (f":{self.registration_port}" if self.registration != "closed" else ""))

    def __repr__(self):
        return f"ServerConfig({self.describe()})"
