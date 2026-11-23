#! /usr/bin/env python3
# Black-box tests for hotcrp-comet.
#
# Usage: test/comet-test.py [--slow] PATH-TO-HOTCRP-COMET [TEST...]
#
# Runs a fake HotCRP back end (answering `api.php/trackerstatus` and
# `api.php/whoami`), starts hotcrp-comet with `--verify-port` pointing at it,
# and checks the server's observable behavior.

import http.client
import http.server
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

SITE = "http://conf.example/c1/"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Backend:
    """Fake HotCRP site. `mode` is "ok", "fail" (`{"ok":false}`), "raw"
    (answer with the `raw` string), or "close" (drop the connection without
    answering)."""

    def __init__(self):
        self.eventid = 1
        self.status = "s1"
        self.mode = "ok"
        self.raw = ""
        self.delay = 0              # seconds before answering
        self.requests = []
        self.active = 0             # requests being answered now
        self.max_active = 0
        self.lock = threading.Lock()
        backend = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                with backend.lock:
                    backend.requests.append((self.path, dict(self.headers)))
                    backend.active += 1
                    backend.max_active = max(backend.max_active, backend.active)
                    delay = backend.delay
                try:
                    if delay:
                        time.sleep(delay)
                    self.answer()
                except (BrokenPipeError, ConnectionResetError):
                    # comet gave up on this request
                    self.close_connection = True
                finally:
                    with backend.lock:
                        backend.active -= 1

            def answer(self):
                with backend.lock:
                    mode = backend.mode
                    eventid, status = backend.eventid, backend.status
                if mode == "close":
                    self.close_connection = True
                    return
                path = urllib.parse.urlsplit(self.path).path
                if mode == "raw":
                    body = backend.raw.encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if mode == "fail":
                    j = {"ok": False}
                elif path.endswith("/api.php/trackerstatus"):
                    j = {"ok": True, "tracker_eventid": eventid,
                         "tracker_status": status}
                elif path.endswith("/api.php/whoami"):
                    cookie = self.headers.get("Cookie", "")
                    j = {"ok": True, "email": "u-" + cookie.replace("=", "-")}
                else:
                    j = {"ok": False}
                body = json.dumps(j).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def set(self, **kw):
        with self.lock:
            for k, v in kw.items():
                setattr(self, k, v)

    def paths(self):
        with self.lock:
            return [p for p, _ in self.requests]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class Comet:
    def __init__(self, binary, backend, args=(), tmpdir=None):
        self.port = free_port()
        self.logname = os.path.join(tmpdir, "comet-%d.log" % self.port)
        self.log = open(self.logname, "w")
        argv = [binary, "--fg", "-p", str(self.port),
                "--verify-port", str(backend.port), "--log-level", "3"]
        argv.extend(args)
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                     stdout=self.log, stderr=self.log)
        deadline = time.time() + 5
        while True:
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                break
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise RuntimeError("comet did not start; see " + self.logname)
                time.sleep(0.02)

    def conn(self, timeout=10):
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)

    def get(self, path, conn=None, timeout=10):
        c = conn or self.conn(timeout)
        c.request("GET", path)
        r = c.getresponse()
        body = r.read()
        if conn is None:
            c.close()
        return r, json.loads(body)

    def poll(self, eventid, timeout_ms=None, site=SITE, conn=None, wait=10):
        q = {"conference": site, "poll": str(eventid)}
        if timeout_ms is not None:
            q["timeout"] = str(timeout_ms)
        return self.get("/poll?" + urllib.parse.urlencode(q), conn, wait)

    def update(self, eventid, status, site=SITE, token=None):
        q = {"conference": site, "tracker_eventid": str(eventid),
             "tracker_status": status}
        if token is not None:
            q["token"] = token
        return self.get("/update?" + urllib.parse.urlencode(q))

    def status(self):
        return self.get("/status")[1]

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()


def wait_until(pred, timeout=5, what="condition"):
    deadline = time.time() + timeout
    while not pred():
        if time.time() > deadline:
            raise AssertionError("timed out waiting for " + what)
        time.sleep(0.02)


def in_background(fn):
    result = {}

    def run():
        try:
            result["value"] = fn()
        except BaseException as e:
            result["error"] = e
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, result


# ---- tests --------------------------------------------------------------

def test_status(env):
    c = env.comet()
    j = c.status()
    assert isinstance(j["at"], (int, float))
    assert isinstance(j["at_time"], str)
    assert j["nconnections"] >= 1
    assert j["sites"] == []
    # the /status request's own connection
    assert len(j["connections"]) == 1
    assert j["connections"][0]["state"] in ("request", "response")


def test_bad_request_keepalive(env):
    c = env.comet()
    conn = c.conn()
    r, j = c.get("/nonsense", conn)
    assert r.status == 200 and j == {"ok": False, "error": "bad request"}
    assert r.getheader("Access-Control-Allow-Origin") == "*"
    assert r.getheader("Content-Type") == "application/json"
    assert r.getheader("Connection") is None
    # same connection still works
    r, j = c.get("/poll?conference=not-a-url", conn)
    assert j == {"ok": False, "error": "bad request"}
    conn.close()


def test_poll_validates_new_site(env):
    env.backend.set(eventid=7, status="s7")
    c = env.comet()
    t0 = time.time()
    r, j = c.poll(3)
    assert time.time() - t0 < 2
    assert j["ok"] is True and j["tracker_eventid"] == 7
    assert r.getheader("Connection") == "close"
    paths = env.backend.paths()
    assert paths and paths[0] == "/c1/api.php/trackerstatus", paths
    hdrs = env.backend.requests[0][1]
    assert hdrs.get("Host") == "conf.example", hdrs


def test_poll_blocks_until_update(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    _, j = c.poll(1)                      # validate the site
    assert j["tracker_eventid"] == 5
    t, res = in_background(lambda: c.poll(5, 20000))
    time.sleep(0.4)
    assert t.is_alive(), "poll returned early"
    _, uj = c.update(6, "s6")
    assert uj == {"ok": True}
    t.join(5)
    assert not t.is_alive()
    _, pj = res["value"]
    assert pj["ok"] is True and pj["tracker_eventid"] == 6


def test_poll_many_wake(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    polls = [in_background(lambda: c.poll(5, 20000)) for _ in range(40)]
    wait_until(lambda: sum(1 for x in c.status()["connections"]
                           if x.get("state") == "poll") == 40,
               what="40 pollers")
    c.update(9, "s9")
    for t, res in polls:
        t.join(5)
        assert not t.is_alive()
        assert res["value"][1]["tracker_eventid"] == 9


def test_poll_status_string(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    # matching status string blocks (here until the timeout)
    t0 = time.time()
    _, j = c.poll("s5", 400)
    assert time.time() - t0 >= 0.35 and j["tracker_eventid"] == 5
    # a different status string returns at once
    t0 = time.time()
    _, j = c.poll("old", 5000)
    assert time.time() - t0 < 1 and j["tracker_eventid"] == 5


def test_poll_timeout(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    t0 = time.time()
    _, j = c.poll(5, 500)
    dt = time.time() - t0
    assert 0.45 <= dt < 2, dt
    assert j["ok"] is True and j["tracker_eventid"] == 5


def test_poll_backend_failing(env):
    env.backend.set(mode="fail")
    c = env.comet()
    t0 = time.time()
    _, j = c.poll(1, 400)
    assert time.time() - t0 < 2
    assert j["ok"] is False
    assert c.status()["sites"][0]["nupdate_fail"] >= 1


def test_poll_backend_closes(env):
    env.backend.set(mode="close")
    c = env.comet()
    _, j = c.poll(1, 400)
    assert j["ok"] is False
    # back end recovers; an update makes the site valid again
    env.backend.set(mode="ok", eventid=4, status="s4")
    c.update(4, "s4")
    _, j = c.poll(1, 400)
    assert j["ok"] is True and j["tracker_eventid"] == 4


def test_backend_connection_reuse(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    for i in range(3):
        env.backend.set(eventid=5 + i)
        # stale status forces revalidation via the `site_min_validate_interval`
        # path only rarely, so poll new sites instead
        c.poll(0, 1000, site="http://conf.example/c%d/" % (i + 10))
    assert len(env.backend.paths()) >= 3


def test_site_urls(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    _, j = c.poll(0, site="http://u@conf.example:8080/x/y")
    assert j["tracker_eventid"] == 5, j
    path, hdrs = env.backend.requests[-1]
    assert path == "/x/y/api.php/trackerstatus", path
    assert hdrs.get("Host") == "conf.example:8080", hdrs
    _, j = c.poll(0, site="https://[::1]?q=1")
    assert env.backend.requests[-1][0] == "/api.php/trackerstatus"
    assert env.backend.requests[-1][1].get("Host") == "[::1]"
    for bad in ["conf.example/c1", "http:///x", "http://h:99999/",
                "http://h/a b", "/c1/", "http://[::1/"]:
        _, j = c.poll(0, site=bad)
        assert j == {"ok": False, "error": "bad request"}, (bad, j)


def test_update_token(env):
    env.backend.set(eventid=3, status="s3")
    c = env.comet(["--token", "sekrit"])
    _, j = c.update(3, "s3")
    assert j == {"ok": False, "error": "invalid status update"}
    _, j = c.update(3, "s3", token="wrong")
    assert j["ok"] is False
    _, j = c.update(3, "s3", token="sekrit")
    assert j == {"ok": True}
    _, j = c.poll(1, 400)
    assert j["ok"] is True and j["tracker_eventid"] == 3
    assert all("token=sekrit" in p for p in env.backend.paths())


def test_update_older_ignored(env):
    c = env.comet()
    c.update(10, "s10")
    _, j = c.update(8, "s8")
    assert j["ok"] is False
    site = c.status()["sites"][0]
    assert site["eventid"] == 10


# The Tamer version fails this test: it noticed only local closes, so a poll
# whose client went away lingered until its timeout.
def test_client_disconnect_during_poll(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    s = socket.create_connection(("127.0.0.1", c.port))
    q = urllib.parse.urlencode({"conference": SITE, "poll": "5", "timeout": "20000"})
    s.sendall(("GET /poll?%s HTTP/1.1\r\nHost: x\r\n\r\n" % q).encode())
    wait_until(lambda: any(x.get("state") == "poll"
                           for x in c.status()["connections"]), what="poller")
    s.close()
    wait_until(lambda: not any(x.get("state") == "poll"
                               for x in c.status()["connections"]),
               what="poller to go away")
    assert c.status()["sites"][0]["npollers"] == 0


def test_status_details(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    t, res = in_background(lambda: c.poll(5, 3000))
    wait_until(lambda: any(x.get("state") == "poll"
                           for x in c.status()["connections"]), what="poller")
    j = c.status()
    site = j["sites"][0]
    assert site["site"] == SITE and site["eventid"] == 5
    assert site["npoll"] == 2 and site["npollers"] == 1
    poller = [x for x in j["connections"] if x.get("state") == "poll"][0]
    assert poller["site"] == SITE and poller["eventid"] == 5
    c.update(6, "s6")
    t.join(5)


def test_status_port(env):
    sport = free_port()
    c = env.comet(["--status-port", str(sport)])
    _, j = c.get("/status")
    assert j == {"ok": True}
    conn = http.client.HTTPConnection("127.0.0.1", sport, timeout=5)
    conn.request("GET", "/status")
    j = json.loads(conn.getresponse().read())
    assert "connections" in j


def test_track_users(env):
    env.backend.set(eventid=5, status="s5")
    c = env.comet(["--track-users"])
    conn = c.conn()
    q = urllib.parse.urlencode({"conference": SITE, "poll": "1"})
    conn.request("GET", "/poll?" + q,
                 headers={"Cookie": "hotcrpsession=abc; hotlist-info-1=zzz"})
    r = conn.getresponse()
    r.read()
    wait_until(lambda: any(p.startswith("/c1/api.php/whoami")
                           for p in env.backend.paths()), what="whoami")
    who = [h for p, h in env.backend.requests if "/whoami" in p][0]
    assert who.get("Cookie") == "hotcrpsession=abc", who


def test_sigterm_pidfile(env):
    pidfile = os.path.join(env.tmpdir, "comet.pid")
    c = env.comet(["--pid-file", pidfile])
    with open(pidfile) as f:
        assert int(f.read()) == c.proc.pid
    c.proc.send_signal(signal.SIGTERM)
    assert c.proc.wait(5) == 1
    with open(pidfile) as f:
        assert f.read() == "0\n"


def test_connection_limit(env):
    c = env.comet(["--nfiles", "130"])
    socks = []
    for _ in range(115):
        socks.append(socket.create_connection(("127.0.0.1", c.port)))
    time.sleep(0.5)
    closed = []
    for i, s in enumerate(socks):
        s.setblocking(False)
        try:
            if s.recv(1) == b"":
                closed.append(i)
        except BlockingIOError:
            pass
        except ConnectionResetError:
            closed.append(i)
    assert closed, "no connection was evicted"
    assert closed[0] < 10, closed
    for s in socks:
        s.close()
    # the server still works
    assert "connections" in c.status()


def test_backend_pool_limit(env):
    # more concurrent validations than pooled back-end connections
    env.backend.set(eventid=5, status="s5", delay=0.3)
    c = env.comet()
    polls = [in_background(lambda i=i: c.poll(0, 5000, site="http://conf.example/p%d/" % i))
             for i in range(12)]
    for t, res in polls:
        t.join(10)
        assert not t.is_alive()
        assert res["value"][1]["tracker_eventid"] == 5, res
    assert 2 <= env.backend.max_active <= 5, env.backend.max_active


def test_backend_pool_cancel(env):
    # clients that go away while their `whoami` checks are pending must not
    # use up pooled back-end connections
    env.backend.set(eventid=5, status="s5")
    c = env.comet(["--track-users"])
    c.poll(1)                             # validate the site
    env.backend.set(delay=1.0)
    for i in range(8):
        s = socket.create_connection(("127.0.0.1", c.port))
        q = urllib.parse.urlencode({"conference": SITE, "poll": "1"})
        s.sendall(("GET /poll?%s HTTP/1.1\r\nHost: x\r\nCookie: s=%d\r\n\r\n"
                   % (q, i)).encode())
        s.recv(4096)
        s.close()
    time.sleep(1.5)
    env.backend.set(delay=0, eventid=6)
    polls = [in_background(lambda i=i: c.poll(0, 5000, site="http://conf.example/q%d/" % i))
             for i in range(8)]
    for t, res in polls:
        t.join(10)
        assert not t.is_alive(), "back-end pool stuck"
        assert res["value"][1]["tracker_eventid"] == 6


def test_daemon(env):
    # without --fg: the parent exits once the child is listening
    pidfile = os.path.join(env.tmpdir, "daemon.pid")
    logfile = os.path.join(env.tmpdir, "daemon.log")
    port = free_port()
    r = subprocess.run([env.binary, "-p", str(port), "--pid-file", pidfile,
                        "--log", logfile, "--log-level", "3",
                        "--verify-port", str(env.backend.port)],
                       stdin=subprocess.DEVNULL, timeout=10)
    assert r.returncode == 0
    with open(pidfile) as f:
        pid = int(f.read())
    try:
        env.backend.set(eventid=5, status="s5")
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        q = urllib.parse.urlencode({"conference": SITE, "poll": "1"})
        conn.request("GET", "/poll?" + q)
        j = json.loads(conn.getresponse().read())
        assert j["ok"] is True and j["tracker_eventid"] == 5, j
    finally:
        os.kill(pid, signal.SIGTERM)

    def gone():
        try:
            os.kill(pid, 0)
            return False
        except ProcessLookupError:
            return True
    wait_until(gone, what="daemon to exit")
    with open(pidfile) as f:
        assert f.read() == "0\n"
    with open(logfile) as f:
        log = f.read()
    assert "hotcrp-comet started, pid %d" % pid in log, log


# --update-directory (Linux only): HotCRP creates each update file in place
# (named by timestamp, with an `x<random>` suffix on collision), writes
# compact JSON, and closes it; comet applies and removes it.

def update_dir(env, args=()):
    d = tempfile.mkdtemp(prefix="updates", dir=env.tmpdir)
    return d, env.comet(["--update-directory", d] + list(args))


def write_update(d, name, j, raw=None):
    path = os.path.join(d, name)
    with open(path, "x") as f:
        f.write(raw if raw is not None else json.dumps(j, separators=(",", ":")))
    return path


def site_update(site, eventid, status=None):
    return {"ok": True, "conference": site, "tracker_eventid": eventid,
            "tracker_status": status or "s%d" % eventid}


def site_eventid(c, site):
    for s in c.status()["sites"]:
        if s["site"] == site:
            return s["eventid"]
    return None


def test_update_directory(env):
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    d, c = update_dir(env)
    path = write_update(d, "1795313035", site_update(SITE, 42))
    wait_until(lambda: not os.path.exists(path), what="update file removal")
    assert site_eventid(c, SITE) == 42
    # an older event ID is ignored, but its file is still removed
    path = write_update(d, "1795313036", site_update(SITE, 41))
    wait_until(lambda: not os.path.exists(path), what="stale file removal")
    assert site_eventid(c, SITE) == 42


def test_update_directory_wakes_poll(env):
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    env.backend.set(eventid=5, status="s5")
    d, c = update_dir(env)
    c.poll(1)                             # validate the site
    t, res = in_background(lambda: c.poll(5, 20000))
    wait_until(lambda: any(x.get("state") == "poll"
                           for x in c.status()["connections"]), what="poller")
    t0 = time.time()
    write_update(d, "1795313035", site_update(SITE, 6))
    t.join(5)
    assert not t.is_alive() and time.time() - t0 < 2
    assert res["value"][1]["tracker_eventid"] == 6


def test_update_directory_burst(env):
    # many files at once, so one inotify read returns several events
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    d, c = update_dir(env)
    sites = ["http://conf.example/d%d/" % i for i in range(30)]
    paths = [write_update(d, "1795313035" + ("x%d" % i if i else ""),
                          site_update(s, 100 + i))
             for i, s in enumerate(sites)]
    wait_until(lambda: not any(os.path.exists(p) for p in paths),
               what="all update files removed")
    for i, s in enumerate(sites):
        assert site_eventid(c, s) == 100 + i, s


def test_update_directory_bad_files(env):
    # files without a string `conference` are left alone
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    d, c = update_dir(env)
    bad = [write_update(d, "b1", None, raw="not json"),
           write_update(d, "b2", None, raw=""),
           write_update(d, "b3", {"ok": True, "tracker_eventid": 7}),
           write_update(d, "b4", {"ok": True, "conference": 12, "tracker_eventid": 7})]
    good = write_update(d, "g1", site_update(SITE, 9))
    wait_until(lambda: not os.path.exists(good), what="good file removal")
    assert all(os.path.exists(p) for p in bad)
    assert site_eventid(c, SITE) == 9
    assert [s["site"] for s in c.status()["sites"]] == [SITE]


def test_update_directory_token(env):
    # update files work with --token, though HotCRP doesn't put a token in
    # them; HTTP updates still need it
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    d, c = update_dir(env, ["--token", "sekrit"])
    path = write_update(d, "1795313035", site_update(SITE, 42))
    wait_until(lambda: not os.path.exists(path), what="update file removal")
    assert site_eventid(c, SITE) == 42
    _, j = c.update(43, "s43")
    assert j["ok"] is False
    assert site_eventid(c, SITE) == 42


def test_update_directory_long_name(env):
    # a maximum-length name fills the inotify read buffer
    if not sys.platform.startswith("linux"):
        return "skipped (Linux only)"
    d, c = update_dir(env)
    paths = [write_update(d, ch * 255, site_update("http://conf.example/%s/" % ch, 3))
             for ch in "abc"]
    wait_until(lambda: not any(os.path.exists(p) for p in paths),
               what="long-name file removal")
    for ch in "abc":
        assert site_eventid(c, "http://conf.example/%s/" % ch) == 3


def raw_get(port, path):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
               % path).encode())
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.split(b"\r\n\r\n", 1)[1]


def test_response_format(env):
    c = env.comet()
    q = urllib.parse.urlencode({"conference": SITE, "tracker_eventid": "4",
                                "tracker_status": "s4"})
    assert raw_get(c.port, "/update?" + q) == b'{"ok":true}\n'
    assert raw_get(c.port, "/x") == b'{"ok":false,"error":"bad request"}\n'
    env.backend.set(eventid=4, status="s4")
    q = urllib.parse.urlencode({"conference": SITE, "poll": "1"})
    body = raw_get(c.port, "/poll?" + q)
    j = json.loads(body)
    assert list(j) == ["ok", "at", "tracker_eventid"], j
    assert body.endswith(b"}\n") and b"\n" not in body[:-1]
    # `at` has at most two decimal places
    assert round(j["at"], 2) == j["at"]
    # /status: two levels indented, sites and connections one per line
    lines = raw_get(c.port, "/status").decode().split("\n")
    assert lines[0] == "{" and lines[-2] == "}" and lines[-1] == ""
    assert lines[1].startswith('  "at": ')
    sitelines = [l for l in lines if l.startswith('    {"site":')]
    assert len(sitelines) == 1, lines
    json.loads(sitelines[0].rstrip(","))


def test_hostile_strings(env):
    # invalid UTF-8 and odd values from clients don't break responses
    env.backend.set(eventid=5, status="s5")
    c = env.comet()
    c.poll(1)
    bad = "%FF%FE%22%5C"                  # invalid UTF-8, quote, backslash
    site = urllib.parse.quote(SITE, safe="")
    t, res = in_background(lambda: raw_get(
        c.port, "/poll?conference=%s&poll=s5&timeout=2000&uuid=%s" % (site, bad)))
    wait_until(lambda: any(x.get("state") == "poll"
                           for x in c.status()["connections"]), what="poller")
    j = c.status()
    poller = [x for x in j["connections"] if x.get("state") == "poll"][0]
    assert poller["eventid"] == "s5"
    assert isinstance(poller.get("uuid", ""), str), poller
    t.join(5)
    assert json.loads(res["value"])["tracker_eventid"] == 5
    # non-integer event IDs are rejected (the Tamer version, with its old
    # JSON library, accepted an out-of-range integer)
    for v in ["-1", "1e3", "[1]", '"7"', "99999999999999999999999", "x", ""]:
        q = urllib.parse.urlencode({"conference": SITE, "tracker_eventid": v,
                                    "tracker_status": "z"})
        _, j = c.get("/update?" + q)
        assert j["ok"] is False, (v, j)
    assert c.status()["sites"][0]["eventid"] == 5
    # a garbage timeout falls back to the default
    _, j = c.poll("old", "abc")
    assert j["tracker_eventid"] == 5


def test_backend_json(env):
    c = env.comet()
    cases = [
        ('{"ok":1,"tracker_eventid":9,"tracker_status":"s9"}', 9),
        ('{"ok":"yes","tracker_eventid":10,"tracker_status":"s10"}', 10),
        ('{"ok":true,"tracker_eventid":11}', None),     # no status: ok false
        ('not json', None),
        ('[1,2,3]', None),
        ('{"ok":0,"tracker_eventid":12,"tracker_status":"s12"}', None),
        ('{"ok":true,"tracker_eventid":-3,"tracker_status":"s"}', None),
        ('{"ok":true,"tracker_eventid":1.5,"tracker_status":"s"}', None),
    ]
    for i, (raw, want) in enumerate(cases):
        env.backend.set(mode="raw", raw=raw)
        _, j = c.poll(0, 300, site="http://conf.example/b%d/" % i)
        if want is None:
            assert j["ok"] is False, (raw, j)
        else:
            assert j["ok"] is True and j["tracker_eventid"] == want, (raw, j)


def test_keepalive_timeout(env):
    if not env.slow:
        return "skipped (--slow)"
    c = env.comet()
    s = socket.create_connection(("127.0.0.1", c.port))
    s.sendall(b"GET /nonsense HTTP/1.1\r\nHost: x\r\n\r\n")
    s.settimeout(10)
    data = b""
    while b"\r\n\r\n" not in data:
        data += s.recv(4096)
    t0 = time.time()
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
    dt = time.time() - t0
    assert 4 <= dt < 8, dt


TESTS = [v for k, v in list(globals().items()) if k.startswith("test_")]


class Env:
    def __init__(self, binary, tmpdir, slow):
        self.binary = binary
        self.tmpdir = tmpdir
        self.slow = slow
        self.backend = Backend()
        self.comets = []

    def comet(self, args=()):
        c = Comet(self.binary, self.backend, args, self.tmpdir)
        self.comets.append(c)
        return c

    def close(self):
        for c in self.comets:
            c.stop()
        self.backend.stop()


def main(argv):
    slow = "--slow" in argv
    argv = [a for a in argv if a != "--slow"]
    if len(argv) < 2:
        print(__doc__ or "usage: comet-test.py BINARY [TEST...]", file=sys.stderr)
        return 2
    binary = os.path.abspath(argv[1])
    names = argv[2:]
    failures = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        for fn in TESTS:
            name = fn.__name__[5:]
            if names and name not in names:
                continue
            env = Env(binary, tmpdir, slow)
            try:
                note = fn(env)
                print("%-32s %s" % (name, note or "ok"))
            except Exception as e:
                failures += 1
                print("%-32s FAIL %s: %s" % (name, type(e).__name__, e))
                for c in env.comets:
                    c.log.flush()
                    with open(c.logname) as f:
                        tail = f.read()[-1500:]
                    if tail:
                        print("    --- %s ---\n    %s" % (c.logname,
                              tail.replace("\n", "\n    ")))
            finally:
                env.close()
    print("%d failure%s" % (failures, "" if failures == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
