"""Authentication for the Cerid orchestrator, in the standard library only (ADR-0135 §12, security review).

- Bearer tokens (job, agent session, registration): 256 random bits, only their SHA-256 is stored, compared in
  constant time, each bound to one job or agent and expiring.
- JWT verification for the three signed identities the coordinator accepts: a cloud VM's Google identity token
  (RS256, Google's JWKS), Cloudflare Access (RS256, the team's certs) and Google IAP (ES256, IAP's JWKS) in front of
  the web UI. RS256 is RSASSA-PKCS1-v1_5 with SHA-256 (RFC 8017 §8.2.2); ES256 is ECDSA on P-256 with SHA-256
  (FIPS 186-5, RFC 7518 §3.4). Only verification is implemented (public keys and public data: no secret-dependent
  timing). `alg` must match the key type, `none` and HMAC algorithms are refused, `exp`, `nbf`, `iss` and `aud`
  are checked with a 60 s leeway.
"""
from __future__ import annotations

from pathlib import Path
import base64
import hashlib
import hmac
import ipaddress
import json
import secrets
import socket
import subprocess
import threading
import time
import urllib.parse

LEEWAY = 60


class AuthError(PermissionError):
    pass


# --------------------------------------------------------------------------------------------- bearer tokens

def new_token(prefix):
    """A fresh bearer token, `<prefix>_` plus 43 URL-safe characters (256 bits)."""
    return '%s_%s' % (prefix, secrets.token_urlsafe(32))


def token_hash(token):
    return hashlib.sha256(token.encode('utf-8')).hexdigest()


def same(a, b):
    return hmac.compare_digest(str(a or '').encode('utf-8'), str(b or '').encode('utf-8'))


def bearer(headers):
    value = headers.get('Authorization') or headers.get('authorization') or ''
    scheme, _, token = value.partition(' ')
    return token.strip() if scheme.lower() == 'bearer' and token.strip() else None


# --------------------------------------------------------------------------------------------- JWT

def b64url(text):
    if isinstance(text, str):
        text = text.encode('ascii')
    return base64.urlsafe_b64decode(text + b'=' * (-len(text) % 4))


def b64url_encode(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def as_int(data):
    return int.from_bytes(data, 'big')


SHA256_PREFIX = bytes.fromhex('3031300d060960864801650304020105000420')


def rsa_verify(n, e, message, signature):
    """RSASSA-PKCS1-v1_5 verification with SHA-256: EM = 00 01 FF..FF 00 DigestInfo(SHA-256(message))."""
    k = (n.bit_length() + 7) // 8
    if len(signature) != k or k < 128:
        return False
    s = as_int(signature)
    if s >= n:
        return False
    em = pow(s, e, n).to_bytes(k, 'big')
    digest = SHA256_PREFIX + hashlib.sha256(message).digest()
    expected = b'\x00\x01' + b'\xff' * (k - len(digest) - 3) + b'\x00' + digest
    return hmac.compare_digest(em, expected)


# P-256 (secp256r1) domain parameters, FIPS 186-5 / SEC 2.
P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
A = P - 3
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)


def ec_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        slope = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (slope * slope - x1 - x2) % P
    return x3, (slope * (x1 - x3) - y1) % P


def ec_mul(k, point):
    result = None
    addend = point
    while k:
        if k & 1:
            result = ec_add(result, addend)
        addend = ec_add(addend, addend)
        k >>= 1
    return result


def on_curve(point):
    x, y = point
    return 0 <= x < P and 0 <= y < P and (y * y - x * x * x - A * x - B) % P == 0


def ecdsa_verify(x, y, message, signature):
    """ECDSA P-256 / SHA-256 verification of a JWS signature (r || s, 32 bytes each)."""
    if len(signature) != 64 or not on_curve((x, y)):
        return False
    r, s = as_int(signature[:32]), as_int(signature[32:])
    if not (0 < r < N and 0 < s < N):
        return False
    z = as_int(hashlib.sha256(message).digest())
    w = pow(s, -1, N)
    point = ec_add(ec_mul(z * w % N, G), ec_mul(r * w % N, (x, y)))
    return point is not None and point[0] % N == r


def verify_signature(alg, jwk, message, signature):
    if alg == 'RS256' and jwk.get('kty') == 'RSA':
        return rsa_verify(as_int(b64url(jwk['n'])), as_int(b64url(jwk['e'])), message, signature)
    if alg == 'ES256' and jwk.get('kty') == 'EC' and jwk.get('crv') == 'P-256':
        return ecdsa_verify(as_int(b64url(jwk['x'])), as_int(b64url(jwk['y'])), message, signature)
    return False


class Jwks:
    """A JSON Web Key Set fetched from `url` and cached for `ttl` seconds; an unknown `kid` refetches once (key
    rotation) but at most every 60 s."""

    def __init__(self, url, fetch, ttl=3600, clock=time.time):
        self.url = url
        self.fetch = fetch
        self.ttl = ttl
        self.clock = clock
        self.keys = {}
        self.loaded = 0.0
        self.lock = threading.Lock()

    def _load(self):
        document = self.fetch(self.url)
        self.keys = {k.get('kid'): k for k in document.get('keys', []) if k.get('kid')}
        self.loaded = self.clock()

    def key(self, kid):
        with self.lock:
            if not self.keys or self.clock() - self.loaded > self.ttl:
                self._load()
            if kid not in self.keys and self.clock() - self.loaded > 60:
                self._load()
            return self.keys.get(kid)


def verify_jwt(token, jwks, audience, issuers, algorithms=('RS256', 'ES256'), now=None):
    """The verified claims of a compact JWS, or AuthError."""
    try:
        head_b64, body_b64, sig_b64 = token.split('.')
        header = json.loads(b64url(head_b64))
        claims = json.loads(b64url(body_b64))
        signature = b64url(sig_b64)
    except (ValueError, TypeError) as error:
        raise AuthError('malformed token: %s' % error) from None
    alg = header.get('alg')
    if alg not in algorithms:
        raise AuthError('algorithm %r refused' % alg)
    key = jwks.key(header.get('kid'))
    if key is None:
        raise AuthError('unknown signing key')
    if not verify_signature(alg, key, ('%s.%s' % (head_b64, body_b64)).encode('ascii'), signature):
        raise AuthError('bad signature')
    now = time.time() if now is None else now
    if not isinstance(claims.get('exp'), (int, float)) or claims['exp'] < now - LEEWAY:
        raise AuthError('token expired')
    if isinstance(claims.get('nbf'), (int, float)) and claims['nbf'] > now + LEEWAY:
        raise AuthError('token not yet valid')
    if claims.get('iss') not in issuers:
        raise AuthError('issuer %r refused' % claims.get('iss'))
    aud = claims.get('aud')
    if not any(same(a, audience) for a in (aud if isinstance(aud, list) else [aud])):
        raise AuthError('audience refused')
    return claims


# --------------------------------------------------------------------------------------------- identities

GOOGLE_CERTS = 'https://www.googleapis.com/oauth2/v3/certs'
GOOGLE_ISSUERS = ('https://accounts.google.com', 'accounts.google.com')
IAP_CERTS = 'https://www.gstatic.com/iap/verify/public_key-jwk'
IAP_ISSUERS = ('https://cloud.google.com/iap',)


def google_instance(claims, service_account, project):
    """(instance name, zone) of a verified Compute Engine identity token (`format=full`) issued to the runner service
    account of the CI project, or AuthError."""
    if not same(claims.get('email'), service_account) or not claims.get('email_verified'):
        raise AuthError('identity is not the runner service account')
    engine = (claims.get('google') or {}).get('compute_engine') or {}
    if not same(engine.get('project_id'), project) or not engine.get('instance_name'):
        raise AuthError('identity is not a Compute Engine instance of the CI project')
    return engine['instance_name'], engine.get('zone', '')


IAP_TUNNEL_RANGE = '35.235.240.0/20'
TAILNET_RANGES = ('100.64.0.0/10', 'fd7a:115c:a1e0::/48')


def in_networks(address, networks):
    try:
        ip = ipaddress.ip_address((address or '').split('%', 1)[0])
    except ValueError:
        return False
    if getattr(ip, 'ipv4_mapped', None):
        ip = ip.ipv4_mapped
    return any(ip in ipaddress.ip_network(n, strict=False) for n in networks)


class TailscaleWhois:
    """Who a tailnet peer is, from the local tailscaled (its LocalAPI over the Unix socket on Linux, else the
    `tailscale whois --json` command): the identity WireGuard authenticated, not a header a client could forge.
    Answers are cached for a minute per peer address."""

    SOCKET = '/var/run/tailscale/tailscaled.sock'

    def __init__(self, run=subprocess.run, socket_path=SOCKET, clock=time.time, ttl=60.0):
        self.run = run
        self.socket_path = socket_path
        self.clock = clock
        self.ttl = ttl
        self.cache = {}
        self.lock = threading.Lock()

    def _localapi(self, address):
        import http.client

        class Connection(http.client.HTTPConnection):
            def connect(conn):
                conn.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                conn.sock.settimeout(5)
                conn.sock.connect(self.socket_path)
        connection = Connection('local-tailscaled.sock', timeout=5)
        try:
            connection.request('GET', '/localapi/v0/whois?addr=%s' % urllib.parse.quote(address, safe=''))
            response = connection.getresponse()
            body = response.read()
            if response.status != 200:
                raise AuthError('tailscaled does not know peer %s' % address)
            return json.loads(body.decode('utf-8'))
        finally:
            connection.close()

    def _cli(self, address):
        completed = self.run(['tailscale', 'whois', '--json', address], capture_output=True, text=True, timeout=10,
                             check=False)
        if completed.returncode != 0:
            raise AuthError('tailscale whois %s failed: %s' % (address, (completed.stderr or '').strip()[:200]))
        return json.loads(completed.stdout)

    def login(self, host, port):
        address = '[%s]:%s' % (host, port) if ':' in host else '%s:%s' % (host, port)
        with self.lock:
            hit = self.cache.get(address)
            if hit and self.clock() - hit[0] < self.ttl:
                return hit[1]
        if hasattr(socket, 'AF_UNIX') and Path(self.socket_path).exists():
            answer = self._localapi(address)
        else:
            answer = self._cli(address)
        login = ((answer.get('UserProfile') or {}).get('LoginName') or '').strip()
        node = answer.get('Node') or {}
        if not login or node.get('Tags'):
            raise AuthError('peer %s is a tagged device or has no user login' % address)
        with self.lock:
            self.cache[address] = (self.clock(), login)
        return login


class UiAuth:
    """Who is calling the web UI. There is no anonymous mode. Modes:

    - `iap-tunnel` (the default): the coordinator listens only on loopback and the launcher's internal address,
      the firewall admits TCP 8780 only from Google's IAP TCP-forwarding range, and only principals holding the IAP
      tunnel role on the launcher can open that tunnel (`ci/ops.py ui`). The coordinator accepts UI requests only
      from loopback and that range; a runner VM inside the network is refused.
    - `tailscale` (optional add-on): the peer must be a tailnet address and its user login, asked of the local
      tailscaled (whois), must be on the allow-list.
    - `cloudflare-access` (RS256 assertion) and `iap` (ES256 assertion behind an HTTPS load balancer): alternatives.
    - `local`: only on a loopback listener (tests and a workstation).
    """

    def __init__(self, mode, audience='', team='', fetch=None, allowed=(), loopback=False, clock=time.time,
                 networks=None, whois=None):
        self.mode = mode
        self.audience = audience
        self.allowed = {a.lower() for a in allowed}
        self.clock = clock
        if mode == 'cloudflare-access':
            if not team or not audience:
                raise ValueError('cloudflare-access needs the team domain and the application AUD tag')
            self.header = 'Cf-Access-Jwt-Assertion'
            self.issuers = ('https://%s' % team,)
            self.jwks = Jwks('https://%s/cdn-cgi/access/certs' % team, fetch)
            self.algorithms = ('RS256',)
        elif mode == 'iap':
            if not audience:
                raise ValueError('iap needs the backend audience (/projects/N/global/backendServices/ID)')
            self.header = 'x-goog-iap-jwt-assertion'
            self.issuers = IAP_ISSUERS
            self.jwks = Jwks(IAP_CERTS, fetch)
            self.algorithms = ('ES256',)
        elif mode == 'iap-tunnel':
            self.networks = tuple(networks or ('127.0.0.0/8', '::1/128', IAP_TUNNEL_RANGE))
        elif mode == 'tailscale':
            if not self.allowed:
                raise ValueError('tailscale mode needs ui.allowed_logins: the tailnet users who may open the UI')
            self.whois = whois or TailscaleWhois(clock=clock)
        elif mode == 'local':
            if not loopback:
                raise ValueError('local UI authentication is allowed only on a loopback listener')
        else:
            raise ValueError('unknown UI authentication mode %r' % mode)

    def user(self, headers, peer=None):
        """The viewer's name, or AuthError. `peer` is the connection's (host, port)."""
        if self.mode == 'local':
            return 'local-operator'
        if self.mode == 'iap-tunnel':
            host = (peer or ('', 0))[0]
            if not in_networks(host, self.networks):
                raise AuthError('the UI is reachable only through the IAP tunnel (python ci/ops.py ui); %s is not '
                                'an allowed source' % host)
            return 'operator (IAP tunnel)' if not in_networks(host, ('127.0.0.0/8', '::1/128')) else 'operator (local)'
        if self.mode == 'tailscale':
            host, port = peer or ('', 0)
            if not in_networks(host, TAILNET_RANGES):
                raise AuthError('the UI answers only tailnet peers; %s is not one' % host)
            login = self.whois.login(host, port)
            if login.lower() not in self.allowed:
                raise AuthError('%s is not on ui.allowed_logins' % login)
            return login
        token = headers.get(self.header) or headers.get(self.header.lower())
        if not token:
            raise AuthError('no %s header: the UI is reachable only through the access proxy' % self.header)
        claims = verify_jwt(token, self.jwks, self.audience, self.issuers, self.algorithms, now=self.clock())
        email = str(claims.get('email') or claims.get('sub') or '')
        if self.allowed and email.lower() not in self.allowed:
            raise AuthError('%s is not on the allow-list' % email)
        return email
