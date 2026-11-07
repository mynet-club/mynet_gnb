#!/usr/bin/env python3
"""Linux root-only isolated network namespaces, real signed/encrypted GNB TUNs.
The TCP bridge is a test fixture, NOT a production UTC/mTLS implementation.
No production routes, keys or namespaces are used. Run with --binary-dir.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def bridge(mode, directory):
    directory = Path(directory)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if mode == "dual-edge":
        listeners = []
        for port in (14600, 14601):
            listener = socket.socket()
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("10.250.0.4", port))
            listener.listen(1)
            listeners.append(listener)
        (directory / "edge.listening").touch()
        a, _ = listeners[0].accept()
        b, _ = listeners[1].accept()
        (directory / "edge.ready").touch()
        def pump(source, target):
            while True:
                data = source.recv(65535)
                if not data:
                    os._exit(0)
                target.sendall(data)
        threading.Thread(target=pump, args=(a, b), daemon=True).start()
        pump(b, a)
        return
    if mode == "edge":
        udp.bind(("10.250.0.4", 14000))
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("10.250.0.4", 14600))
        listener.listen(1)
        (directory / "edge.listening").touch()
        tcp, _ = listener.accept()
        target = ("10.250.0.2", 9002)
    else:
        peer = 2 if mode == "client-b" else 1
        udp.bind(("127.0.0.1", 14000 + peer))
        tcp = socket.create_connection(("10.250.0.4", 14600 + peer - 1), timeout=10)
        tcp.settimeout(None)
        target = ("127.0.0.1", 9000 + peer)
    def receive_exact(count):
        data = b""
        while len(data) < count:
            part = tcp.recv(count - len(data))
            if not part:
                os._exit(0)
            data += part
        return data
    last_frame = {"data": None}
    def from_tcp():
        while True:
            size = struct.unpack("!H", receive_exact(2))[0]
            data = receive_exact(size)
            if len(data) >= 4 and data[2] == 4:
                last_frame["data"] = data  # encrypted VPN datagram, memory only
            udp.sendto(data, target)
    def inject_control():
        control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        file = directory / (mode + "-control.sock")
        file.unlink(missing_ok=True)
        control.bind(str(file))
        file.chmod(0o600)
        control.listen(1)
        while True:
            connection, _ = control.accept()
            with connection:
                command = connection.recv(32)
                if command != b"inject" or last_frame["data"] is None:
                    connection.sendall(b"not-ready")
                    continue
                wrong_source = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                wrong_source.bind(("127.0.0.1", 0))
                wrong_source.sendto(last_frame["data"], target)
                wrong_source.close()
                connection.sendall(b"injected")
    if mode != "edge":
        threading.Thread(target=inject_control, daemon=True).start()
    threading.Thread(target=from_tcp, daemon=True).start()
    (directory / ("edge.ready" if mode == "edge" else mode + ".ready")).touch()
    while True:
        data, source = udp.recvfrom(65535)
        if source != target:
            continue
        tcp.sendall(struct.pack("!H", len(data)) + data)


def capture(output):
    # Metadata-only capture: no VPN payload stored. Ethernet + IPv4 + UDP headers.
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0003))
    with open(output, "w", buffering=1) as f:
        while True:
            b = s.recv(65535)
            if len(b) < 42 or b[12:14] != b"\x08\x00" or b[23] != 17:
                continue
            ihl = (b[14] & 15) * 4
            if len(b) < 14 + ihl + 8:
                continue
            src = socket.inet_ntoa(b[26:30])
            dst = socket.inet_ntoa(b[30:34])
            sp, dp = struct.unpack("!HH", b[14+ihl:18+ihl])
            # Public GNB type/subtype only; never capture bytes after these.
            type_sub = b[14+ihl+10:14+ihl+12].hex()
            f.write(json.dumps([time.time(), src, sp, dst, dp, type_sub]) + "\n")


def formal_materials(root, utc_guard, utc_tests, dual=False, manifest=None, lease_ttl=None):
    """Install the formal static-mTLS trust, permits and durable floors.

    Uses the same issuer/trust generator as the utcd R1 tests. Keys are created
    in this disposable topology only; no production trust root is touched.
    """
    import sys
    sys.path.insert(0, str(utc_tests))
    from test_tls_channel import generate_pki
    from test_utc_formal import Harness
    pki_root = root / "pki"
    pki_root.mkdir()
    generate_pki(pki_root, shutil.which("openssl"))
    plan = {"scope": "lab", "pair_id": 12, "pair_gen": 1,
            "manifest": manifest or ("1" * 64),
            "peer_a": 2, "peer_b": 1, "lease_target": 2, "ttl": lease_ttl or 600}
    # The trust contract requires both directions to share one target peer id.
    plan_kw = dict(scope=plan["scope"], pair_gen=plan["pair_gen"], pair_id=plan["pair_id"],
                   manifest=plan["manifest"], target_peer_id_a=plan["lease_target"],
                   target_peer_id_b=plan["lease_target"])
    harness = Harness(pki_root, None, None, str(utc_guard))
    harness.permits(ttl=plan["ttl"], **plan_kw)
    materials = {"plan": plan, "harness": harness, "issuer_hex": harness.public_hex}
    if dual:
        from test_utc_dual import issue_client_b
        issue_client_b(pki_root, shutil.which("openssl"))
        trust = harness.trust("relay", {harness.pin("client"): "client-A", harness.pin("client-b"): "client-B"},
                              "edge-E", "client-A", dual_peer="client-B", **plan_kw)
        materials["relay"] = (trust, pki_root)
        installed = harness.guard_run("install-floor", trust, revision=2)
        assert installed.returncode == 0, installed.stderr
        for role, local, side in (("client", "client-A", None), ("client-b", "client-B", "b")):
            trust = harness.trust(role, harness.pin("server"), local, "edge-E", side=side, **plan_kw)
            installed = harness.guard_run("install-floor", trust, revision=2)
            assert installed.returncode == 0, installed.stderr
            materials[role] = (trust, pki_root)
        return materials
    for role, local, peer in (("relay", "edge-E", "client-A"), ("client", "client-A", "edge-E")):
        trust = harness.trust(role, harness.pin("client" if role == "relay" else "server"), local, peer, **plan_kw)
        installed = harness.guard_run("install-floor", trust, revision=2)
        assert installed.returncode == 0, installed.stderr
        materials[role] = (trust, pki_root)
    return materials


def main(binary_dir, dual=False, safe_index=False, reference_binary_dir=None, utcd=None, agent=None,
         security="token-lab", utc_guard=None, utc_tests=None, block_udp=False,
         relay_host=None, relay_public_ip=None, client_public_ip=None, relay_ssh_key=None):
    if agent and not utcd:
        raise ValueError("agent smoke requires real utcd")
    if dual and security != "static-mtls":
        raise ValueError("A-D requires --security static-mtls")
    if agent and relay_host and not dual:
        raise ValueError("agent publish/verify does not support relay-host A-S: B runs remotely")
    if security == "static-mtls":
        assert utcd and utc_guard and utc_tests, "static-mtls needs --utcd --utc-guard --utc-tests"
    if relay_host:
        assert relay_public_ip and client_public_ip, "public relay needs --relay-public-ip and --client-public-ip"
    assert sys.platform.startswith("linux") and os.geteuid() == 0
    binary_dir = Path(binary_dir).resolve()
    reference_binary_dir = Path(reference_binary_dir).resolve() if reference_binary_dir else binary_dir
    root = Path(tempfile.mkdtemp(prefix="gnb-utc-topology-"))
    prefix = "gnbutc" + str(os.getpid())
    ip_forward_original = None
    stage = None
    plan_state = {"manifest": "1" * 64, "pair": 12, "gen": 1, "valid_until": None, "scope": "lab"}
    def write_run_manifest(dual, security, relay_host, outcomes):
        import hashlib
        def sha(path):
            return hashlib.sha256(Path(path).read_bytes()).hexdigest()
        manifest = {"argv": [str(x) for x in sys.argv], "topology": "A-D" if dual else "A-S",
                    "security": security, "relay_host": relay_host, "checks": outcomes,
                    "finished": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        base = Path(__file__).resolve().parents[2]
        for name in ("mynet_gnb", "mynet_utc", "mynet_agent"):
            repo = Path(__file__).resolve().parents[1] if name == "mynet_gnb" else base / name
            head = run("git", "-C", str(repo), "rev-parse", "HEAD", check=False)
            status = run("git", "-C", str(repo), "status", "--porcelain", check=False)
            # The worktree fingerprint binds the actual inputs: staged, unstaged
            # and untracked content, not just a diffstat summary.
            worktree = []
            for entry in status.stdout.splitlines():
                path = repo / entry[3:].strip()
                if path.is_file() and path.stat().st_size <= 8 * 1024 * 1024:
                    worktree.append({"file": entry, "sha256": sha(path)})
                else:
                    worktree.append({"file": entry,
                                     "size": path.stat().st_size if path.exists() else None})
            diff = run("git", "-C", str(repo), "diff", "HEAD", check=False)
            manifest[name] = {"head": head.stdout.strip(), "worktree": worktree,
                              "diff_sha256": hashlib.sha256(diff.stdout.encode()).hexdigest()}
        # Hash every executable this run actually invokes — chosen by the
        # branches that ran and by the exact paths passed on the command line.
        # A missing hash is missing evidence and must fail the manifest.
        binaries = {}
        ran = [Path(binary_dir) / "gnb", Path(binary_dir) / "gnb_crypto",
               Path(reference_binary_dir) / "gnb"]
        if utcd:
            utcd_path = Path(utcd)
            ran += [utcd_path, utcd_path.with_name("utc")]
        else:
            # The GNB-only flow exercises the native hot-restart via gnb_ctl.
            ran.append(Path(binary_dir) / "gnb_ctl")
        if security == "static-mtls":
            # Only the formal branch consumes --utc-guard; token-lab may
            # legitimately omit it, and Path(None) must never crash the
            # manifest of a diagnostic run.
            ran.append(Path(utc_guard))
        if agent:
            ran.append(Path(agent))
        if utcd and (dual or relay_host):
            ran.append(Path("/tmp/r6/utc.test"))
        for path in ran:
            if not path.is_file():
                raise AssertionError("run manifest missing binary that ran: " + str(path))
            binaries[str(path.resolve())] = sha(path)
        if relay_host:
            remote = run("ssh", *rkey, relay_host,
                         "sha256sum /tmp/r6/bin/utcd /tmp/r6/bin/utc "
                         "/tmp/r6/bin/utc-guard /tmp/r6/bin/gnb /tmp/r6/bin/mynet 2>/dev/null",
                         check=False, timeout=30)
            if remote.returncode != 0 or len(remote.stdout.splitlines()) != 5:
                raise AssertionError("run manifest missing remote binary hashes")
            manifest["relay_binaries"] = remote.stdout.strip().splitlines()
        manifest["binaries"] = binaries
        try:
            (root / "run-manifest.json").write_text(json.dumps(manifest, indent=2))
        except OSError as error:
            raise AssertionError("run manifest write failed: " + str(error))
    relay_known_hosts = os.environ.get("RELAY_KNOWN_HOSTS", "/root/.ssh/known_hosts")
    # One multiplexed connection carries every ssh/scp call: the 0.2s polling
    # loops would otherwise open hundreds of TCP connections per run and trip
    # the relay host's fail2ban on the shared egress address.
    rkey = (["-i", relay_ssh_key] if relay_ssh_key else []) + [
        "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
        "-o", f"UserKnownHostsFile={relay_known_hosts}",
        "-o", "ControlMaster=auto", "-o", "ControlPersist=600s",
        "-o", f"ControlPath=/tmp/gbutc-ssh-{os.getpid()}-%r@%h:%p"]
    def remote_relay_status():
        result = run("ssh", *rkey, relay_host,
                     f"sudo -n /tmp/r6/bin/utc --socket {root}/relay.sock status --json",
                     check=False, timeout=15)
        assert result.returncode == 0, "relay status query failed: " + result.stdout[-300:]
        info = json.loads(result.stdout)
        assert info.get("status") == "running", info
        return info
    def strict_relay_status_for_expiry():
        result = run("ssh", *rkey, relay_host,
                     f"sudo -n /tmp/r6/bin/utc --socket {root}/relay.sock status --json",
                     check=False, timeout=15)
        assert result.returncode == 0, result.stdout[-300:]
        info = json.loads(result.stdout)
        assert info.get("status") == "running", info
        return info
    def relay_ready():
        try:
            remote_relay_status()
            return True
        except AssertionError:
            return False
    namespaces = []
    processes = []
    handles = []
    outcomes = {}
    boots = {}
    probe_log = []
    participants = (1, 2, 3, 9000) if safe_index else (1, 2, 3)
    def run(*args, check=True, timeout=60):
        return subprocess.run([str(x) for x in args], check=check, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, timeout=timeout)
    def ns(name, *args, check=True):
        return run("ip", "netns", "exec", prefix + name, *args, check=check)
    def spawn(name, args, logfile):
        h = open(root / logfile, "w")
        handles.append(h)
        p = subprocess.Popen(["ip", "netns", "exec", prefix+name] + [str(x) for x in args], stdout=h, stderr=h)
        processes.append(p)
        return p
    def wait_for(predicate, timeout=180):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return
            time.sleep(0.2)
        raise AssertionError("timed out; evidence: " + str(root))
    def policy(node, gen, mode=1, ttl=600):
        peer, address, port = (2, "7f000001000000000000000000000000", 14001) if node == 1 else (1, "0afa0004000000000000000000000000", 14000)
        if relay_host and node == 2 and not dual:
            address, port = "7f000001000000000000000000000000", 14000
        if dual and node == 2:
            address, port = "7f000001000000000000000000000000", 14002
        if not mode:
            address, port, ttl = "0" * 32, 0, 0
        if not mode:
            expiry = 0
        elif plan_state.get("valid_until"):
            expiry = plan_state["valid_until"]
        else:
            expiry = int(time.time()) + ttl
        if agent:
            projection = {"scope": "lab", "instance": f"node{node}", "local_id": node, "config_gen": gen,
                "peers": [{"peer_id": peer, "pair_id": 12, "pair_gen": gen, "mode": mode,
                    "expires_at": expiry, "export": f"{socket.inet_ntoa(bytes.fromhex(address)[:4])}:{port}" if mode else "",
                    "scope_id": 0, "manifest": "1"*64}]}
            file = root / f"projection{node}.json"
            file.write_text(json.dumps(projection))
            result = ns(str(node), "env", "MYNET_HOME="+str(root / "agent-home"), agent, "utc", "publish", "-c", root / str(node), "--binary", binary_dir / "gnb", "--file", file)
            assert json.loads(result.stdout)["state"] == "PUBLISHED"
            return
        text = f"utc-v1 lab node{node} {node} {gen}\npeer {peer} 12 {gen} {mode} {expiry} {4 if mode else 0} {address} {port} 0 {plan_state['manifest']}\n"
        file = root / str(node) / "utc.conf"
        tmp = file.with_suffix(".new")
        tmp.write_text(text)
        tmp.chmod(0o600)
        tmp.replace(file)
        if relay_host and node == 2 and not dual:
            run("scp", *rkey, str(file), f"{relay_host}:{stage}/utc.conf.new")
            run("ssh", *rkey, relay_host,
                f"sudo -n install -m 600 -o root -g root {stage}/utc.conf.new {root}/{node}/utc.conf.new && "
                f"sudo -n mv {root}/{node}/utc.conf.new {root}/{node}/utc.conf")
    def applied(node, gen, state="APPLIED", allow_new_boot=False):
        try:
            if relay_host and node == 2 and not dual:
                result = run("ssh", *rkey, relay_host, f"sudo -n cat {root}/{node}/utc.status",
                             check=False, timeout=15)
                if result.returncode != 0:
                    return False
                lines = [x for x in result.stdout.splitlines()
                         if x.startswith(("status-v1 ", "metrics ", "policy ", "peer "))]
            else:
                lines = (root / str(node) / "utc.status").read_text().splitlines()
            header = lines[0].split() if lines else []
            if len(header) < 9 or header[0] != "status-v1" or header[5] != str(gen):
                return False
            if header[1] != plan_state["scope"] or header[2] != "node" + str(node) or \
               header[3] != str(node):
                return False
            try:
                if abs(int(header[6]) - time.time()) > 5:
                    return False
            except ValueError:
                return False
            expect_peer = 2 if node == 1 else 1
            manifest = plan_state["manifest"]
            policy_seen = False
            for row in lines[1:]:
                fields = row.split()
                if fields and fields[0] == "policy" and len(fields) >= 11:
                    if fields[1] != str(expect_peer) or fields[2] != str(plan_state["pair"]) or \
                       fields[3] != str(gen) or fields[10] != manifest:
                        return False
                    policy_seen = True
                if len(fields) >= 8 and fields[0] == "peer" and fields[3] == str(gen) and fields[4] == state:
                    if not policy_seen:
                        return False
                    if fields[1] != str(expect_peer) or fields[2] != str(plan_state["pair"]) or \
                       fields[6] != manifest:
                        return False
                    if node in nodes and nodes[node] is not None and header[8] != str(nodes[node].pid):
                        return False
                    if relay_host and node == 2 and not dual:
                        live = run("ssh", *rkey, relay_host,
                                   f"sudo -n pgrep -f '[g]nb -c {root}/2'", check=False, timeout=15)
                        if live.returncode != 0 or header[8] not in live.stdout.split():
                            return False
                    boot = header[4]
                    if not boot:
                        return False
                    prior = boots.get(node)
                    # Same PID with a changed boot is a writer handoff: stale
                    # status is already excluded by freshness and PID binding,
                    # and only an explicitly orchestrated handoff may rebind.
                    if prior and prior[0] == header[8] and prior[1] != boot and not allow_new_boot:
                        return False
                    boots[node] = (header[8], boot)
                    return True
            return False
        except (OSError, IndexError, ValueError, subprocess.SubprocessError):
            return False
    def parse_last_refusal(info):
        fields = str(info.get("last_refusal", "none")).split()
        try:
            assert len(fields) == 4, info
            return fields[0], int(fields[1]), int(fields[2]), int(fields[3])
        except (ValueError, AssertionError):
            raise AssertionError("malformed last_refusal: " + repr(info.get("last_refusal")))
    def ping(a, b, success=True):
        at = time.time()
        if relay_host and a == 2 and not dual:
            r = run("ssh", *rkey, relay_host, f"ping -c 2 -W 1 10.42.0.{b}", check=False, timeout=20)
        else:
            r = ns(str(a), "ping", "-c", "2", "-W", "1", f"10.42.0.{b}", check=False)
        probe_log.append({"at": at, "from": a, "to": b, "expect_success": success, "rc": r.returncode})
        assert (r.returncode == 0) == success and (success or r.returncode == 1), r.stdout
    try:
        for n in ("s", "1", "2", "3", "4", "5"):
            run("ip", "netns", "add", prefix+n)
            namespaces.append(prefix+n)
            ns(n, "ip", "link", "set", "lo", "up")
        ns("s", "ip", "link", "add", "br0", "type", "bridge")
        ns("s", "ip", "link", "set", "br0", "up")
        for n in range(1, 6):
            left, right = "u"+str(os.getpid())+str(n), "v"+str(os.getpid())+str(n)
            run("ip", "link", "add", left, "type", "veth", "peer", "name", right)
            run("ip", "link", "set", left, "netns", prefix+str(n))
            run("ip", "link", "set", right, "netns", prefix+"s")
            ns(str(n), "ip", "link", "set", left, "name", "eth0")
            ns(str(n), "ip", "addr", "add", f"10.250.0.{n}/24", "dev", "eth0")
            ns(str(n), "ip", "link", "set", "eth0", "up")
            ns("s", "ip", "link", "set", right, "master", "br0")
            ns("s", "ip", "link", "set", right, "up")
        if relay_host:
            # NAT'd egress so the client namespace can dial the public relay.
            run("ip", "link", "add", "wanA" + str(os.getpid()), "type", "veth",
                "peer", "name", "wanB" + str(os.getpid()))
            run("ip", "link", "set", "wanB" + str(os.getpid()), "netns", prefix + "s")
            run("ip", "addr", "add", "10.250.200.1/30", "dev", "wanA" + str(os.getpid()))
            run("ip", "link", "set", "wanA" + str(os.getpid()), "up")
            ns("s", "ip", "link", "set", "wanB" + str(os.getpid()), "name", "wan0")
            ns("s", "ip", "addr", "add", "10.250.200.2/30", "dev", "wan0")
            ns("s", "ip", "link", "set", "wan0", "up")
            ns("s", "ip", "addr", "add", "10.250.0.254/24", "dev", "br0")
            ip_forward_original = Path("/proc/sys/net/ipv4/ip_forward").read_text().strip()
            run("sysctl", "-w", "net.ipv4.ip_forward=1")
            ns("s", "sysctl", "-w", "net.ipv4.ip_forward=1")
            run("iptables", "-t", "nat", "-A", "POSTROUTING", "-s", "10.250.0.0/24",
                "-m", "comment", "--comment", f"gbutc-{os.getpid()}", "-j", "MASQUERADE")
            run("ip", "route", "add", "10.250.0.0/24", "via", "10.250.200.2")
            ns("s", "ip", "route", "add", "default", "via", "10.250.200.1")
            for egress_node in (1, 2):
                ns(str(egress_node), "ip", "route", "add", "default", "via", "10.250.0.254")
        for n in participants:
            directory = root / str(n)
            (directory / "security").mkdir(parents=True)
            (directory / "ed25519").mkdir()
            run(binary_dir / "gnb_crypto", "-c", "-p", directory / "security" / f"{n}.private", "-k", directory / "security" / f"{n}.public")
            (directory / "node.conf").write_text(f"nodeid {n}\nsafe-index {'on' if safe_index else 'off'}\nlisten {19000 if n == 9000 else 9000+n}\nmulti-socket on\nifname gnb{n}\nctl-block {directory}/gnb.map\npid-file {directory}/gnb.pid\nipv4-only on\nunified-forwarding auto\nconsole-log-level 3\npf-log-level 3\nnode-log-level 3\nfile-log-level 0\nmtu 1280\n")
            if n == 9000:
                with (directory / "node.conf").open("a") as f:
                    f.write("set-tun off\nnode-worker off\nindex-worker off\nnode-detect-worker off\n")
            (directory / "route.conf").write_text("".join(f"{i}|10.42.0.{9 if i == 9000 else i}|255.255.255.0\n" for i in participants))
            (directory / "address.conf").write_text("i|9000|10.250.0.5|19000\n" + "".join(f"n|{i}|10.250.0.{i}|900{i}\n" for i in range(1,4) if i != n))
        for n in participants:
            for i in participants:
                if i != n:
                    shutil.copy(root / str(i) / "security" / f"{i}.public", root / str(n) / "ed25519")
        if block_udp:
            # All external UDP is blocked. Index stays configured but unreachable;
            # only the authorized TCP/TLS path may carry the selected pair.
            for n in participants:
                # Keep loopback and the authorized relay/client ports: blocking
                # every UDP would also kill the GNB<->utcd export itself.
                ns(str(n), "iptables", "-A", "OUTPUT", "-p", "udp", "-d", "127.0.0.0/8", "-j", "ACCEPT")
                for port in (9002, 14000, 14001, 14002):
                    ns(str(n), "iptables", "-A", "OUTPUT", "-p", "udp", "--dport", str(port), "-j", "ACCEPT")
                ns(str(n), "iptables", "-A", "OUTPUT", "-p", "udp", "-j", "DROP")
        this = Path(__file__).resolve()
        spawn("s", [sys.executable, this, "--capture", root / "udp.jsonl"], "capture.log")
        index_args = [binary_dir / "gnb", "-c", root / "9000"] if safe_index else [binary_dir / "gnb", "-P", "-l", "19000", "-4", "-b", root / "index.map", "--pid-file", root / "index.pid", "--console-log-level", "1"]
        spawn("5", index_args, "index.log")
        if utcd:
            utcd = Path(utcd).resolve()
            shared_manifest = None
            lease_ttl = int(os.environ.get("TOPO_LEASE_TTL", "0")) or None
            plan_file = root / "coordinator-plan.json"
            if security == "static-mtls":
                if dual or relay_host:
                    emit_env = [f"TOPO_ROOT={root}", f"TOPO_PLAN_FILE={plan_file}",
                                "TOPO_PLAN_EMIT=1", "TOPO_PLAN_TTL=600"]
                    if relay_host and not dual:
                        # A-S: side B's tunnel endpoint is the relay utcd on the
                        # relay host; the plan export must say so.
                        emit_env.append("TOPO_PLAN_EXPORT_B=127.0.0.1:14000")
                    emitted = run("env", *emit_env, "/tmp/r6/utc.test",
                                  "-test.run", "TestTopologyCoordinatorE2E", timeout=60)
                    shared = json.loads(next(x for x in emitted.stdout.splitlines()
                                             if x.startswith("COORDINATOR_PLAN ")).split(" ", 1)[1])
                    shared_manifest = shared["manifest"]
                materials = formal_materials(root, utc_guard, utc_tests, dual,
                                             manifest=shared_manifest, lease_ttl=lease_ttl)
                plan_state["manifest"] = materials["plan"]["manifest"]
                plan_state["pair"] = materials["plan"]["pair_id"]
                plan_state["gen"] = materials["plan"]["pair_gen"]
                plan_state["scope"] = materials["plan"]["scope"]
                plan_state["valid_until"] = shared.get("valid_until") \
                    if (dual or relay_host) else None
            for role, address, udp_port, target, cert in (
                ("relay", "listen = 10.250.0.4:14600", 14000, "10.250.0.2:9002", "server"),
                ("client", "connect = 10.250.0.4:14600", 14001, "127.0.0.1:9001", "client"),
            ):
                if relay_host and role == "client":
                    address = f"connect = {relay_public_ip}:8443"
                config = root / (role + ".ini")
                if security == "static-mtls":
                    trust, pki_root = materials[role]
                    dual_lines = "pairing = dual\n" if dual and role == "relay" else ""
                    target_line = "" if dual and role == "relay" else f"udp_target = {target}\n"
                    port_line = "" if dual and role == "relay" else f"udp_port = {udp_port}\n"
                    config.write_text(f'[dataplane "lab-{role}"]\nprotocol = gnb\ntransport = tcp\nsecurity = static-mtls\nrole = {role}\n{address}\n'
                        f'tls_ca = {pki_root}/trusted.pem\ntls_certificate = {pki_root}/{cert}.pem\n'
                        f'tls_private_key = {pki_root}/{cert}.key\nutc_trust = {trust}\n'
                        f'udp_bind = {"10.250.0.4" if role == "relay" else "127.0.0.1"}\n{port_line}'
                        f'{target_line}{dual_lines}max_sessions = 4\nquota_block_bytes = 65536\n'
                        f'[management]\ncontrol_socket = {root}/{role}.sock\n')
                else:
                    config.write_text(f'[dataplane "lab-{role}"]\nprotocol = gnb\ntransport = tcp\nrole = {role}\n{address}\nudp_bind = {"10.250.0.4" if role == "relay" else "127.0.0.1"}\nudp_port = {udp_port}\nudp_target = {target}\nauth_token = 12345\nmax_sessions = 1\n[management]\ncontrol_socket = {root}/{role}.sock\n')
                config.chmod(0o600)
            if relay_host:
                # relay and B run on the public VPS; A and the third peer stay in
                # namespaces on this host. The trust tree is mirrored at the SAME
                # absolute path so installed state directories stay valid.
                remote_ini = root / "relay-public.ini"
                remote_ini.write_text((root / "relay.ini").read_text()
                    .replace("listen = 10.250.0.4:14600", "listen = 0.0.0.0:8443")
                    .replace("udp_bind = 10.250.0.4", "udp_bind = 0.0.0.0")
                    .replace("udp_target = 10.250.0.2:9002", "udp_target = 127.0.0.1:9002"))
                stage = f"{root}.stage"
                run("ssh", *rkey, relay_host, f"rm -rf {stage} && mkdir -m 700 {stage}")
                run("scp", *rkey, "-r", str(root), f"{relay_host}:{stage}/", timeout=300)
                run("ssh", *rkey, relay_host,
                    f"sudo -n install -d -m 700 -o root -g root {root} && "
                    f"sudo -n cp -r {stage}/{root.name}/. {root}/ && "
                    f"sudo -n chown -R root:root {root}")
                run("scp", *rkey, str(remote_ini), f"{relay_host}:{stage}/relay.ini")
                run("ssh", *rkey, relay_host,
                    f"sudo -n install -m 600 -o root -g root {stage}/relay.ini {root}/relay.ini")
                run("ssh", *rkey, relay_host,
                    f"sudo -n /tmp/r6/bin/utc-guard install-floor --trust {root}/pki/relay.trust --revision 2")
                run("ssh", *rkey, relay_host,
                    f"sudo -n bash -c 'setsid nohup {utcd} -c {root}/relay.ini > {root}/relay.log 2>&1 < /dev/null &'")
                wait_for(lambda: remote_relay_status().get("status") == "running", timeout=30)
            else:
                edge = spawn("4", [utcd, "-c", root / "relay.ini"], "utcd-edge.log")
                wait_for(lambda: (root / "relay.sock").exists())
            client = spawn("1", [utcd, "-c", root / "client.ini"], "utcd-client.log")
            if dual:
                trust_b, pki_root_b = materials["client-b"]
                config_b = root / "client-b.ini"
                config_b.write_text(f'[dataplane "lab-client-b"]\nprotocol = gnb\ntransport = tcp\nsecurity = static-mtls\n'
                    f'role = client\nconnect = {relay_public_ip + ":8443" if relay_host else "10.250.0.4:14600"}\nside = b\n'
                    f'tls_ca = {pki_root_b}/trusted.pem\ntls_certificate = {pki_root_b}/client-b.pem\n'
                    f'tls_private_key = {pki_root_b}/client-b.key\nutc_trust = {trust_b}\n'
                    f'udp_bind = 127.0.0.1\nudp_port = 14002\nudp_target = 127.0.0.1:9002\n'
                    f'max_sessions = 1\nquota_block_bytes = 65536\n'
                    f'[management]\ncontrol_socket = {root}/client-b.sock\n')
                config_b.chmod(0o600)
                client_b = spawn("2", [utcd, "-c", config_b], "utcd-client-b.log")
                wait_for(lambda: (root / "client-b.sock").exists())
            wait_for(lambda: (root / "client.sock").exists())
        else:
            edge_mode = "dual-edge" if dual else "edge"
            edge = spawn("4", [sys.executable, this, "--bridge", edge_mode, str(root)], "edge.log")
            wait_for(lambda: (root / "edge.listening").exists())
            client = spawn("1", [sys.executable, this, "--bridge", "client", str(root)], "client.log")
            if dual:
                client_b = spawn("2", [sys.executable, this, "--bridge", "client-b", str(root)], "client-b.log")
            wait_for(lambda: (root / "edge.ready").exists() and (root / "client.ready").exists() and (not dual or (root / "client-b.ready").exists()))
        if not agent:
            policy(1, 1)
            policy(2, 1)
        nodes = {}
        for n in range(1, 4):
            if relay_host and n == 2 and not dual:
                # B's GNB listens on the public VPS next to the relay.
                run("scp", *rkey, "-r", str(root / "2"), f"{relay_host}:{stage}/", timeout=300)
                run("ssh", *rkey, relay_host,
                    f"sudo -n cp -r {stage}/2/. {root}/2/ && sudo -n chown -R root:root {root}/2",
                    timeout=120)
                run("ssh", *rkey, relay_host,
                    f"sudo -n bash -c 'setsid nohup {binary_dir}/gnb -c {root}/2 > {root}/gnb2.log 2>&1 < /dev/null &'")
                nodes[n] = None
                continue
            nodes[n] = spawn(str(n), [(reference_binary_dir if n == 3 else binary_dir) / "gnb", "-c", root / str(n)], f"gnb{n}.log")
        if agent:
            wait_for(lambda: all(ns(str(n), "ip", "link", "show", f"gnb{n}", check=False).returncode == 0 for n in (1, 2)))
            policy(1, 1)
            policy(2, 1)
        wait_for(lambda: applied(1, 1) and applied(2, 1))
        if agent:
            for node in (1, 2):
                header = (root / str(node) / "utc.status").read_text().splitlines()[0].split()
                assert int(header[8]) == nodes[node].pid
                verified = ns(str(node), "env", "MYNET_HOME="+str(root / "agent-home"), agent, "utc", "verify", "-c", root / str(node), "--file", root / f"projection{node}.json", "--boot", header[4], "--pid", nodes[node].pid)
                assert json.loads(verified.stdout)["state"] == "APPLIED"
            outcomes["agent_publish_and_current_process_applied_verification"] = "PASS"

        wait_for(lambda: ns("1", "ip", "link", "show", "gnb1", check=False).returncode == 0)
        ping(1, 2)
        ping(2, 1)
        ping(1, 3, not block_udp)
        ping(3, 1, not block_udp)
        outcomes["forced_tcp_bidirectional_tun_and_third_peer"] = "PASS"
        if utcd and security == "static-mtls" and "plan" in materials:
            plan = materials["plan"]
            peer_of = {1: plan["peer_a"], 2: plan["peer_b"]}
            def status_lines(node):
                if relay_host and node == 2 and not dual:
                    out = run("ssh", *rkey, relay_host,
                              f"sudo -n cat {root}/{node}/utc.status", check=False).stdout
                else:
                    out = (root / str(node) / "utc.status").read_text()
                return [x for x in out.splitlines()
                        if x.startswith(("status-v1 ", "policy ", "peer "))]
            for node in (1, 2):
                rows = status_lines(node)
                header = rows[0].split()
                assert header[1] == plan["scope"], header
                policy = next(x for x in rows if x.startswith("policy ")).split()
                assert policy[1] == str(peer_of[node]), policy
                assert policy[2] == str(plan["pair_id"]) and policy[3] == str(plan["pair_gen"]), policy
                assert policy[10] == plan["manifest"], policy
                if plan_state.get("valid_until"):
                    assert policy[5] == str(plan_state["valid_until"]), policy
            for lease_name in ("lease-a", "lease-b"):
                lease_doc = json.loads((root / "pki" / f"{lease_name}.json").read_text())
                assert lease_doc["scope"] == plan["scope"], lease_doc
                assert lease_doc["pair_id"] == plan["pair_id"] and lease_doc["pair_gen"] == plan["pair_gen"], lease_doc
                assert lease_doc["manifest"] == plan["manifest"], lease_doc
                assert lease_doc["target_peer_id"] == plan["lease_target"], lease_doc
            outcomes["same_plan_binds_gnb_policy_and_utcd_lease"] = "PASS"
            if lease_ttl:
                # Natural expiry: with live sessions established, the signed
                # lease reaching expires_at must block the pair without any
                # restart. The proof is the authorization layer refusing with
                # the EXPIRED wire reason for this pair's lease — instance
                # counters alone are not attributable — plus business stop in
                # both directions and the client's permanent refusal exit.
                lease_a = json.loads((root / "pki" / "lease-a.json").read_text())
                lease_b = json.loads((root / "pki" / "lease-b.json").read_text())
                expires_at = int(lease_a["expires_at"])
                assert int(lease_b["expires_at"]) == expires_at
                ping(1, 2)   # positive control with a still-valid lease
                ping(2, 1)
                cli = Path(utcd).with_name("utc")
                def relay_counters():
                    # The relay is the authorization authority and stays alive
                    # across the refusal; the client exits (permanent refusal)
                    # so its counters cannot be read after the fact.
                    if relay_host:
                        result = run("ssh", *rkey, relay_host,
                                     f"sudo -n /tmp/r6/bin/utc --socket {root}/relay.sock "
                                     "status --json", check=False, timeout=15)
                    else:
                        result = run(str(cli), "--socket", str(root / "relay.sock"),
                                     "status", "--json", check=False, timeout=15)
                    if result.returncode != 0:
                        return None
                    info = json.loads(result.stdout)
                    assert info.get("status") == "running", info
                    assert "permit_expired" in info and "lease_expires_at" in info, info
                    assert "refused_expired" in info and "last_refusal" in info, info
                    return info
                relay_pre = relay_counters()
                assert relay_pre is not None, "relay status unreadable before expiry"
                assert int(relay_pre["lease_expires_at"]) == expires_at, relay_pre
                while time.time() < expires_at + 3:
                    time.sleep(1)
                def expiry_observed():
                    info = relay_counters()
                    return info is not None and info["permit_expired"] > relay_pre["permit_expired"]
                wait_for(expiry_observed, timeout=45)
                ping(1, 2, False)
                ping(2, 1, False)
                # An expired permit is a permanent refusal, not a retry loop:
                # the client must have exited nonzero after receiving EXPIRED.
                wait_for(lambda: client.poll() is not None, timeout=30)
                client_rc = client.poll()
                assert client_rc != 0, "expired permit must end in a nonzero client exit"
                if dual:
                    wait_for(lambda: client_b.poll() is not None, timeout=30)
                    assert client_b.poll() != 0, "expired permit must end in a nonzero client exit"
                confirmed = {}
                def expiry_confirmed():
                    info = relay_counters()
                    if info is None:
                        return False
                    kind, pair, gen, at = parse_last_refusal(info)
                    # The refusal must belong to THIS event: recorded for this
                    # pair/generation, at or after this lease's signed expiry,
                    # with the refusal counter moved relative to the pre-expiry
                    # snapshot. A stale refusal cannot carry a future timestamp.
                    ok = (kind == "expired" and pair == plan_state["pair"] and
                          gen == plan_state["gen"] and at >= expires_at and
                          int(info["refused_expired"]) > int(relay_pre["refused_expired"]))
                    if ok:
                        confirmed["relay"] = info
                    return ok
                wait_for(expiry_confirmed, timeout=30)
                # The evidence records the exact state that satisfied the
                # assertion; the counters are re-checked on that same read.
                relay_post = confirmed["relay"]
                assert relay_post["permit_expired"] > relay_pre["permit_expired"], relay_post
                (root / "lease-expiry-evidence.json").write_text(json.dumps(
                    {"lease_expires_at": expires_at,
                     "relay_pre": relay_pre, "relay_post": relay_post,
                     "client_exit_code": client_rc,
                     "probes": probe_log}, indent=2))
                outcomes["public_lease_expiry_blocks_pair"] = "PASS"
                print(json.dumps({"topology": "A-S real utcd " + security + " smoke",
                                  "evidence": str(root), "checks": outcomes}, indent=2), flush=True)
                return
        if block_udp:
            outcomes["third_peer_unreachable_under_udp_block"] = "PASS"
        if reference_binary_dir != binary_dir and not block_udp:
            outcomes["unchanged_reference_peer_wire_interop"] = "PASS"
        # Snapshot the strict interval, exclude initial startup ARP/probe scheduling.
        if relay_host and not dual:
            run("ssh", *rkey, relay_host, f"sudo -n rm -f {root}/remote-b.txt")
            run("ssh", *rkey, relay_host,
                f"sudo -n bash -c 'setsid nohup timeout 150 tcpdump -l -tt -ni any "
                f"\"udp and (port 9002 or port 14000)\" > {root}/remote-b.txt 2>&1 < /dev/null &'")
            time.sleep(2)  # let the filter install before the strict interval
        start = time.time()
        ping(1, 2)
        ping(2, 1)
        time.sleep(2)
        packets = [json.loads(x) for x in (root / "udp.jsonl").read_text().splitlines()]
        interval = [p for p in packets if p[0] >= start]
        assert not any(p[1] == "10.250.0.1" and p[3] == "10.250.0.2" for p in interval), interval
        assert not any(p[1] == "10.250.0.2" and p[3] == "10.250.0.1" for p in interval), interval
        if not dual and not relay_host:
            assert any(p[1] == "10.250.0.4" and p[3] == "10.250.0.2" for p in interval)
            assert any(p[1] == "10.250.0.2" and p[3] == "10.250.0.4" for p in interval)
        if relay_host and not dual:
            time.sleep(9)  # capture window must finish and flush
            b_capture = run("ssh", *rkey, relay_host, f"sudo -n cat {root}/remote-b.txt", check=False).stdout
            # Tunnel traffic must involve the test node on loopback; production
            # GNB traffic from 9016 to public peers:9002 is out of scope here.
            b_loop = [line for line in b_capture.splitlines() if "127.0.0.1.9002" in line]
            probe = run("ssh", *rkey, relay_host, "sudo -n ss -ulpn | grep 9002", check=False).stdout
            assert b_loop, ("no test-node UDP observed on the relay host: " +
                            b_capture[-300:] + " listen=" + probe)
            b_src = [line for line in b_capture.splitlines() if ".9002 >" in line and "IP" in line]
            assert all("> 127.0.0.1.14000:" in line for line in b_src), \
                [line for line in b_src if "> 127.0.0.1.14000:" not in line]
        elif relay_host and dual:
            # A-D has no B-side UDP on the VPS: the relay pairs two TCP sessions.
            info = remote_relay_status()
            assert info.get("established", 0) >= 2, info
        outcomes["pair_zero_native_udp_with_native_candidates"] = "PASS"
        for node in ((1,) if relay_host and not dual else (1, 2)):
            host = f"10.250.0.{node}"
            if not block_udp:
                assert any(p[1] == host and p[3] == "10.250.0.5" and p[5] in ("0801", "0803") for p in packets)
                assert any(p[1] == "10.250.0.5" and p[3] == host and p[5] in ("0802", "0804") for p in packets)
        if not block_udp:
            outcomes["index_registration_query_response_preserved"] = "PASS"
        else:
            outcomes["index_config_retained_but_unreachable"] = "PASS"
        if utcd:
            timeline = {}
            def remote_utcd_pids():
                r = run("ssh", *rkey, relay_host,
                        f"sudo -n pgrep -f '[u]tcd -c {root}/relay.ini'", check=False, timeout=15)
                if r.returncode not in (0, 1):
                    raise AssertionError("pgrep failed on the relay host: " + r.stdout[-200:])
                return set(r.stdout.split()) if r.returncode == 0 else set()
            def edge_alive():
                if relay_host:
                    return bool(remote_utcd_pids())
                return edge.poll() is None
            def edge_stop():
                if relay_host:
                    timeline["stop"] = time.time()
                    stopped = remote_utcd_pids()
                    assert stopped, "relay reported alive but no live process"
                    timeline["stopped"] = stopped
                    run("ssh", *rkey, relay_host,
                        f"sudo -n pkill -f '[u]tcd -c {root}/relay.ini'", check=False)
                    wait_for(lambda: not remote_utcd_pids(), timeout=15)
                    closed = run("ssh", *rkey, relay_host,
                                 "ss -tln 'sport = :8443'", check=False, timeout=15)
                    assert closed.returncode == 0 and "8443" not in closed.stdout, closed.stdout
                else:
                    edge.terminate()
                    edge.wait(timeout=5)
            def edge_start():
                stopped = timeline.get("stopped", set())
                if relay_host:
                    timeline["start"] = time.time()
                    run("ssh", *rkey, relay_host,
                        f"sudo -n bash -c 'setsid nohup {utcd} -c {root}/relay.ini > {root}/relay.log 2>&1 < /dev/null &'")
                    wait_for(relay_ready, timeout=30)
                    after = remote_utcd_pids()
                    assert after and after != stopped, (stopped, after)
                    listening = run("ssh", *rkey, relay_host,
                                    "ss -tln 'sport = :8443'", check=False, timeout=15)
                    assert listening.returncode == 0 and "8443" in listening.stdout, listening.stdout
                    return None
                return spawn("4", [utcd, "-c", root / "relay.ini"], "utcd-edge-restarted.log")
            assert edge_alive() and client.poll() is None
            outcomes["real_utcd_processes_carry_gnb_datagrams"] = "PASS"
            status_cli = Path(utcd).with_name("utc")
            def client_status(sock_name="client.sock"):
                return json.loads(run(str(status_cli), "--socket", str(root / sock_name),
                                      "status", "--json").stdout)
            epoch_at_stop = client_status().get("connection_epoch", 0)
            client_b_pid = client_b.pid if dual else None
            edge_stop()
            ping(1, 2, False)
            ping(1, 3, not block_udp)
            outcomes["real_utcd_failure_blocks_pair_preserves_native_peer"] = "PASS"
            assert client.poll() is None
            client_pid = client.pid
            restart_at = time.time()
            edge = edge_start()
            wait_for(lambda: ns("1", "ping", "-c", "1", "-W", "1", "10.42.0.2", check=False).returncode == 0, timeout=45)
            assert client.poll() is None and client.pid == client_pid
            reconnected = client_status()
            assert reconnected.get("connection_epoch", 0) > epoch_at_stop, (epoch_at_stop, reconnected)
            # `established` is a formal-mode field; token-lab sessions prove the
            # same fact with the live session count.
            live = reconnected.get("established") if security == "static-mtls" \
                else reconnected.get("sessions")
            assert (live or 0) >= 1, reconnected
            assert ":14001" in ns("1", "ss", "-uln", check=False).stdout
            if dual:
                assert client_b.poll() is None and client_b.pid == client_b_pid, \
                    "B client must survive the outage"
                assert ":14002" in ns("2", "ss", "-uln", check=False).stdout
            if relay_host:
                assert remote_relay_status().get("established", 0) >= (2 if dual else 1)
            assert applied(2, 1)
            ping(1, 2)
            ping(2, 1)
            timeline["recovered"] = time.time()
            outcomes["real_utcd_automatic_reconnect_fixed_export_ports"] = "PASS"
            packets = [json.loads(x) for x in (root / "udp.jsonl").read_text().splitlines()]
            interval = [p for p in packets if p[0] >= restart_at]
            assert not any(p[1] == "10.250.0.1" and p[3] == "10.250.0.2" for p in interval)
            assert not any(p[1] == "10.250.0.2" and p[3] == "10.250.0.1" for p in interval)
            outcomes["real_utcd_recovery_has_no_native_pair_bypass"] = "PASS"
            if relay_host and not dual and "stop" in timeline and "start" in timeline:
                stop_at, recovered_at = timeline["stop"], timeline["recovered"]
                # B's own index/discovery probes (to the third peer and the
                # index) are unmanaged paths; the PAIR must only leave through
                # the tunnel, and never toward A's native addresses.
                def capture_rows():
                    out = run("ssh", *rkey, relay_host,
                              f"sudo -n cat {root}/remote-b.txt", check=False, timeout=15).stdout
                    rows = []
                    for line in out.splitlines():
                        if ".9002 >" not in line or "IP" not in line:
                            continue
                        try:
                            rows.append((float(line.split()[0]), line))
                        except ValueError:
                            continue
                    return rows
                # The GNB export cadence is bursty (probe-driven, ~10-37s), so
                # wait for exports to land strictly after the confirmed
                # recovery point instead of relaxing the window into the
                # outage: packets sent during the stop must never satisfy the
                # recovery phase.
                def post_recovery_exports():
                    return len([1 for ts, line in capture_rows()
                                if "> 127.0.0.1.14000:" in line and ts >= recovered_at]) >= 2
                wait_for(post_recovery_exports, timeout=120)
                stamped = capture_rows()
                # Failing to preserve the capture must fail this evidence item.
                # The remote file is root-owned, so fetch it via sudo cat; a
                # plain scp silently failed for the run user all along.
                preserved = run("ssh", *rkey, relay_host, f"sudo -n cat {root}/remote-b.txt",
                                timeout=30).stdout
                (root / "remote-b.txt").write_text(preserved)
                assert (root / "remote-b.txt").stat().st_size > 0
                pair_lines = [(ts, line) for ts, line in stamped
                              if "> 127.0.0.1.14000:" in line]
                assert pair_lines, "no pair tunnel egress from B: " + str(stamped[:3])
                def destination(line):
                    try:
                        return line.split(" > ", 1)[1].rsplit(":", 1)[0]
                    except IndexError:
                        return ""
                foreign = [line for _, line in stamped
                           if not destination(line).startswith(("127.", "10.250."))]
                assert not foreign, foreign
                bypass = [line for _, line in stamped
                          if "10.250.0.1." in line or "10.42.0.1." in line]
                assert not bypass, bypass
                before = [line for ts, line in pair_lines if ts <= stop_at]
                after = [line for ts, line in pair_lines if ts >= recovered_at]
                assert len(before) >= 2, "pair export traffic missing before the cutoff"
                assert len(after) >= 2, "pair export traffic missing after the recovery point"
                outcomes["remote_capture_covers_cut_stop_recover"] = "PASS"
            if dual or relay_host:
                # The real coordinator drives both GNB sides and both utcd
                # sessions while traffic crosses the relay. In A-S the B side
                # lives on the relay host and is driven over SSH through its
                # operator-installed CLI.
                status_cli = Path(utcd).with_name("utc")
                def read_epoch(sock):
                    result = run(str(status_cli), "--socket", str(root / sock), "status", "--json",
                                 check=False, timeout=15)
                    assert result.returncode == 0, result.stderr[-300:]
                    info = json.loads(result.stdout)
                    assert "connection_epoch" in info, info
                    return int(info["connection_epoch"])
                def read_epoch_remote():
                    result = run("ssh", *rkey, relay_host,
                                 f"sudo -n /tmp/r6/bin/utc --socket {root}/relay.sock status --json",
                                 check=False, timeout=15)
                    assert result.returncode == 0, result.stderr[-300:]
                    info = json.loads(result.stdout)
                    assert "connection_epoch" in info, info
                    return int(info["connection_epoch"])
                if dual:
                    epochs_before = (read_epoch("client.sock"), read_epoch("client-b.sock"))
                else:
                    epochs_before = (read_epoch("client.sock"), read_epoch_remote())
                traffic = [spawn("1", ["ping", "-i", "0.2", "-W", "1", "10.42.0.2"], "coord-ping-a.log")]
                if dual:
                    traffic.append(spawn("2", ["ping", "-i", "0.2", "-W", "1", "10.42.0.1"],
                                         "coord-ping-b.log"))
                traffic_at = time.time()
                try:
                    forward = [f"TOPO_DEBUG_APPLY={os.environ['TOPO_DEBUG_APPLY']}"] \
                        if "TOPO_DEBUG_APPLY" in os.environ else []
                    remote_env = []
                    socks = f"TOPO_SOCK_B={root}/client-b.sock"
                    if not dual:
                        socks = f"TOPO_SOCK_B={root}/relay.sock"
                        remote_env = [f"TOPO_REMOTE_B={relay_host}",
                                      f"TOPO_REMOTE_KEY={relay_ssh_key}",
                                      f"TOPO_REMOTE_CONF={root}/2",
                                      f"TOPO_REMOTE_CLI=/tmp/r6/bin/mynet",
                                      f"TOPO_REMOTE_GNB=/tmp/r6/bin/gnb",
                                      f"TOPO_REMOTE_UTCD=/tmp/r6/bin/utc",
                                      f"TOPO_REMOTE_SOCK={root}/relay.sock"]
                    result = run("env", *forward, *remote_env, f"TOPO_ROOT={root}",
                                 f"TOPO_GNB_BIN={binary_dir}/gnb",
                                 f"TOPO_SOCK_A={root}/client.sock", socks,
                                 f"TOPO_PLAN_FILE={root}/coordinator-plan.json",
                                 f"TOPO_ISSUER_KEY_HEX={materials['issuer_hex']}",
                                 f"MYNET_HOME={root}/coord-home",
                                 "/tmp/r6/utc.test", "-test.run", "TestTopologyCoordinatorE2E",
                                 "-test.v", timeout=180, check=False)
                    if result.returncode != 0:
                        raise AssertionError("coordinator e2e failed: " +
                                             result.stdout[-2000:] + result.stderr[-1000:])
                    record_line = next(x for x in result.stdout.splitlines()
                                       if x.startswith("COORDINATOR_RECORD "))
                    record = json.loads(record_line.split(" ", 1)[1])
                    assert record.get("phase") == "EFFECTIVE", record
                    plan = materials["plan"]
                    record_plan = record["plan"]
                    assert record_plan["manifest"] == plan["manifest"], record_plan
                    assert record_plan["pair_id"] == plan["pair_id"] and \
                        record_plan["generation"] == plan["pair_gen"], record_plan
                    timing = json.loads(next(x for x in result.stdout.splitlines()
                                             if x.startswith("COORDINATOR_TIMING ")).split(" ", 1)[1])
                    started_at = timing["started_at"] / 1e9
                    finished_at = timing["finished_at"] / 1e9
                    assert started_at - 2 <= traffic_at <= finished_at, \
                        "probe traffic must start inside the operation window"
                    epochs_after = (read_epoch("client.sock"),
                                    read_epoch("client-b.sock") if dual else read_epoch_remote())
                    assert epochs_after == epochs_before, \
                        "counters must belong to one live session on both sides"
                    for index in (0, 1):
                        assert record["applied"][index]["state"] == "APPLIED", record
                        assert record["verified"][index]["state"] == "VERIFIED", record
                        assert record["verified"][index]["sent"] > 0, record
                        assert record["verified"][index]["received"] > 0, record
                        assert record["verified"][index]["session_epoch"] > 0, record
                        assert record["verified"][index]["session_sent"] > 0, record
                        assert record["verified"][index]["session_received"] > 0, record
                    (root / "coordinator-record.json").write_text(json.dumps(
                        {"record": record, "timing": timing}, indent=2))
                    outcomes["coordinator_applied_verified_effective"] = "PASS"
                finally:
                    for process in traffic:
                        if process.poll() is None:
                            process.terminate()
            if relay_host:
                from test_utc_formal import REVOCATION_DOMAIN, revocations
                plan = materials["plan"]
                harness = materials["harness"]
                harness.now = int(time.time())
                harness.expires = harness.now + 300
                harness.snapshot = harness.signed("revocations-revoked-a.json", REVOCATION_DOMAIN,
                                                  revocations(3, harness.now - 5, harness.expires,
                                                              ["lease-A"], scope=plan["scope"]))
                relay_map = {harness.pin("client"): "client-A"}
                if dual:
                    relay_map[harness.pin("client-b")] = "client-B"
                rtrust = harness.trust("relay", relay_map,
                                       "edge-E", "client-A",
                                       dual_peer="client-B" if dual else None,
                                       scope=plan["scope"], pair_gen=plan["pair_gen"],
                                       pair_id=plan["pair_id"], manifest=plan["manifest"],
                                       target_peer_id_a=plan["lease_target"],
                                       target_peer_id_b=plan["lease_target"])
                run("scp", *rkey, str(harness.snapshot), f"{relay_host}:{root}.stage/revocations-revoked-a.json")
                run("ssh", *rkey, relay_host,
                    f"sudo -n install -m 600 -o root -g root {root}.stage/revocations-revoked-a.json "
                    f"{root}/pki/revocations-revoked-a.json")
                run("scp", *rkey, str(rtrust), f"{relay_host}:{root}.stage/relay.trust")
                run("ssh", *rkey, relay_host,
                    f"sudo -n install -m 600 -o root -g root {root}.stage/relay.trust {root}/pki/relay.trust")
                run("ssh", *rkey, relay_host,
                    f"sudo -n /tmp/r6/bin/utc-guard install-floor --trust {root}/pki/relay.trust --revision 3")
                def strict_relay_status():
                    result = run("ssh", *rkey, relay_host,
                                 f"sudo -n /tmp/r6/bin/utc --socket {root}/relay.sock status --json",
                                 check=False, timeout=15)
                    assert result.returncode == 0, result.stdout[-300:]
                    info = json.loads(result.stdout)
                    assert info.get("status") == "running", info
                    return info
                ping(1, 2)   # positive control: same pair, lease still valid
                ping(2, 1)
                revoke_at = time.time()
                edge_stop()
                edge = edge_start()
                # The proof of an authorization-layer refusal is the relay
                # recording the REVOKED wire reason for this pair's lease.
                # `accepted` counts bare TCP arrivals before TLS and proves
                # nothing on its own; a missing session is not attributable.
                def revoked_refusal():
                    current = strict_relay_status()
                    kind, pair, gen, at = parse_last_refusal(current)
                    return kind == "revoked" and pair == plan_state["pair"] and \
                        gen == plan_state["gen"] and at >= revoke_at - 5
                wait_for(revoked_refusal, timeout=30)
                info = strict_relay_status()
                assert int(info.get("refused_revoked", 0)) >= 1, info
                assert info["rejected"] == 0, info
                assert info["established"] == 0, info
                ping(1, 2, False)
                wait_for(lambda: client.poll() is not None, timeout=30)
                client_rc = client.poll()
                assert client_rc != 0, "a revoked lease is a permanent refusal"
                client_b_rc = None
                if dual:
                    wait_for(lambda: client_b.poll() is not None, timeout=30)
                    client_b_rc = client_b.poll()
                    assert client_b_rc != 0, "a revoked lease is a permanent refusal"
                (root / "revocation-evidence.json").write_text(json.dumps(
                    {"revoked_at": revoke_at, "relay": info,
                     "client_exit_code": client_rc, "client_b_exit_code": client_b_rc,
                     "probes": probe_log}, indent=2))
                outcomes["revoked_lease_refused_on_public_relay"] = "PASS"
            print(json.dumps({"topology": ("A-D" if dual else "A-S") + " real utcd " + security + " smoke", "evidence": str(root), "checks": outcomes}, indent=2), flush=True)
            return
        # Replay one genuinely encrypted GNB datagram from an unauthorized UDP
        # source. Assert the driver's reject counter and actual TUN RX, not ping
        # failure alone (which could also mean the sender never sent).
        def rx_packets():
            link = json.loads(ns("1", "ip", "-s", "-j", "link", "show", "gnb1").stdout)[0]
            return link.get("stats64", link.get("stats"))["rx"]["packets"]
        def rejected():
            return int(next(x for x in (root / "1" / "utc.status").read_text().splitlines() if x.startswith("metrics ")).split()[4])
        time.sleep(1)
        before_rx, before_reject = rx_packets(), rejected()
        control = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        control.connect(str(root / "client-control.sock"))
        control.sendall(b"inject")
        assert control.recv(32) == b"injected"
        control.close()
        wait_for(lambda: rejected() > before_reject)
        assert rx_packets() == before_rx
        outcomes["valid_encrypted_wrong_source_rejected_before_tun"] = "PASS"


        # Delete A's config: only the selected pair blocks; C remains native.
        (root / "1" / "utc.conf").unlink()
        wait_for(lambda: applied(1, 1, "BLOCKED"))
        ping(1, 2, False)
        ping(1, 3, not block_udp)
        outcomes["missing_config_blocks_pair_only"] = "PASS"
        policy(1, 2)
        policy(2, 2)
        wait_for(lambda: applied(1, 2) and applied(2, 2))
        ping(1, 2)
        outcomes["new_generation_recovers_pair"] = "PASS"
        policy(2, 3, mode=0)
        wait_for(lambda: applied(2, 3))
        ping(2, 1, False)  # valid peer, wrong native source for A's strict pair
        ping(1, 3, not block_udp)
        outcomes["authenticated_peer_native_source_rejected"] = "PASS"
        policy(1, 4)
        policy(2, 4)
        wait_for(lambda: applied(1, 4) and applied(2, 4))
        ping(1, 2)
        # Scope/manifest cannot be replayed as a lower generation release.
        policy(1, 1, mode=0)
        time.sleep(2)
        assert applied(1, 4)
        ping(1, 2)
        outcomes["older_direct_release_rejected"] = "PASS"
        edge.terminate()
        edge.wait(timeout=5)
        client.wait(timeout=5)  # bridge disconnect exits the test client
        if dual:
            client_b.wait(timeout=5)
        ping(1, 2, False)
        ping(1, 3, not block_udp)
        outcomes["tcp_disconnect_no_native_fallback"] = "PASS"
        (root / "edge.ready").unlink()
        (root / "client.ready").unlink()
        (root / "edge.listening").unlink()
        edge = spawn("4", [sys.executable, this, "--bridge", edge_mode, str(root)], "edge-reconnect.log")
        wait_for(lambda: (root / "edge.listening").exists())
        client = spawn("1", [sys.executable, this, "--bridge", "client", str(root)], "client-reconnect.log")
        if dual:
            (root / "client-b.ready").unlink()
            client_b = spawn("2", [sys.executable, this, "--bridge", "client-b", str(root)], "client-b-reconnect.log")
        wait_for(lambda: (root / "edge.ready").exists() and (root / "client.ready").exists())
        ping(1, 2)
        outcomes["tcp_reconnect_same_udp_endpoints"] = "PASS"

        # Explicit release plus disabling native forced forwarding must restore direct.
        policy(1, 5, mode=0)
        policy(2, 5, mode=0)
        wait_for(lambda: applied(1, 5) and applied(2, 5))
        # Native liveness is 37s PING + 10s sync; release must revalidate,
        # not inherit a proxy PONG as proof of direct reachability.
        wait_for(lambda: ns("1", "ping", "-c", "1", "-W", "1", "10.42.0.2", check=False).returncode == 0, timeout=55)
        ping(1, 2)
        ping(2, 1)
        outcomes["explicit_release_restores_native_connectivity"] = "PASS"
        policy(1, 6, ttl=4)
        policy(2, 6, ttl=4)
        wait_for(lambda: applied(1, 6) and applied(2, 6))
        wait_for(lambda: applied(1, 6, "BLOCKED") and applied(2, 6, "BLOCKED"), timeout=8)
        ping(1, 2, False)
        ping(1, 3, not block_udp)
        outcomes["idle_expiry_blocks_pair_preserves_third_peer"] = "PASS"
        old_boot = (root / "1" / "utc.status").read_text().splitlines()[0].split()[4]
        nodes[1].terminate()
        nodes[1].wait(timeout=5)
        nodes[1] = spawn("1", [binary_dir / "gnb", "-c", root / "1"], "gnb1-restart.log")
        wait_for(lambda: (root / "1" / "utc.status").read_text().splitlines()[0].split()[4] != old_boot)
        assert applied(1, 6, "BLOCKED")
        ping(1, 2, False)
        policy(1, 7)
        policy(2, 7)
        wait_for(lambda: applied(1, 7) and applied(2, 7))
        ping(1, 2)
        ping(2, 1)
        outcomes["restart_block_then_fresh_generation_recover"] = "PASS"
        old_boot = (root / "1" / "utc.status").read_text().splitlines()[0].split()[4]
        ns("1", binary_dir / "gnb_ctl", "-b", root / "1" / "gnb.map", "-w", "core.flag=2")
        wait_for(lambda: (root / "1" / "utc.status").read_text().splitlines()[0].split()[4] != old_boot, timeout=120)
        wait_for(lambda: applied(1, 7, "BLOCKED", allow_new_boot=True))
        policy(1, 8)
        policy(2, 8)
        wait_for(lambda: applied(1, 8) and applied(2, 8))
        ping(1, 2)
        ping(2, 1)
        outcomes["native_hot_restart_fresh_boot_writer_handoff"] = "PASS"
        nodes[1].terminate()
        nodes[1].wait(timeout=5)
        for option in ("--ur0", "--ur1"):
            failed = ns("1", binary_dir / "gnb", "-c", root / "1", option, "on", check=False)
            assert failed.returncode != 0 and "RECOVERY_REQUIRED" in failed.stdout + failed.stderr, \
                failed.stdout + failed.stderr
        outcomes["opaque_ur_server_conflict_rejected"] = "PASS"
        nodes[1] = spawn("1", [binary_dir / "gnb", "-c", root / "1"], "gnb1-after-ur-rejection.log")
        wait_for(lambda: applied(1, 8, "BLOCKED"))
        policy(1, 9)
        policy(2, 9)
        wait_for(lambda: applied(1, 9) and applied(2, 9))
        ping(1, 2)



        nodes[1].terminate()
        nodes[1].wait(timeout=5)
        backup = root / "managed-backup"
        backup.mkdir()
        artifacts = ("utc.conf", "utc.managed", "utc.state.0", "utc.state.1", "utc.state.2")
        for artifact in artifacts:
            (root / "1" / artifact).rename(backup / artifact)
        failed = ns("1", binary_dir / "gnb", "-c", root / "1", "--utc-managed", check=False)
        assert failed.returncode != 0 and "RECOVERY_REQUIRED" in failed.stdout + failed.stderr, \
            failed.stdout + failed.stderr
        outcomes["launcher_guard_rejects_total_managed_state_loss"] = "PASS"
        for artifact in artifacts:
            (backup / artifact).rename(root / "1" / artifact)
        backup.rmdir()
        nodes[1] = spawn("1", [binary_dir / "gnb", "-c", root / "1", "--utc-managed"], "gnb1-guarded-restore.log")
        wait_for(lambda: applied(1, 9, "BLOCKED"))
        policy(1, 10)
        policy(2, 10)
        wait_for(lambda: applied(1, 10) and applied(2, 10))
        ping(1, 2)
        ping(2, 1)
        outcomes["guarded_trusted_restore_and_fresh_generation"] = "PASS"
        print(json.dumps({"topology": "A-D driver fixture" if dual else "A-S driver fixture", "evidence": str(root), "checks": outcomes}, indent=2), flush=True)
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
        for p in processes:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        for h in handles:
            h.close()
        for n in reversed(namespaces):
            run("ip", "netns", "del", n, check=False)
        if relay_host:
            try:
                snapshot = run("ssh", *rkey, relay_host,
                               f"sudo -n stat -c '%U %G %a %n' {root} {root}/2 {root}/pki && "
                               f"sudo -n sha256sum {root}/2/utc.conf {root}/2/utc.status",
                               check=False, timeout=30)
                (root / "remote-state.txt").write_text(snapshot.stdout)
            except (subprocess.SubprocessError, OSError):
                pass
            if not os.environ.get("RELAY_KEEP_EVIDENCE"):
                for command in (f"sudo -n pkill -f '[u]tcd -c {root}/relay.ini'; "
                                f"sudo -n pkill -f '[g]nb -c {root}/2'; true",
                                f"sudo -n rm -rf {root} {root}.stage"):
                    try:
                        run("ssh", *rkey, relay_host, command, check=False, timeout=120)
                    except (subprocess.SubprocessError, OSError):
                        pass
        # The VM network is reclaimed on every run regardless of evidence
        # retention; each NAT rule carries this run's unique tag.
        if relay_host:
            if ip_forward_original is not None:
                run("sysctl", "-w", f"net.ipv4.ip_forward={ip_forward_original}", check=False)
            run("ip", "link", "del", "wanA" + str(os.getpid()), check=False)
            run("ip", "route", "del", "10.250.0.0/24", "via", "10.250.200.2", check=False)
            run("iptables", "-t", "nat", "-D", "POSTROUTING", "-s", "10.250.0.0/24",
                "-m", "comment", "--comment", f"gbutc-{os.getpid()}", "-j", "MASQUERADE", check=False)
            leftover = run("iptables", "-t", "nat", "-S", check=False).stdout.count(f"gbutc-{os.getpid()}")
            assert leftover == 0, f"NAT rules not reclaimed: {leftover}"
        had_failure = sys.exc_info()[0] is not None
        try:
            write_run_manifest(dual, security, relay_host, outcomes)
        except Exception as manifest_error:
            # Required evidence: a missing manifest or missing hashes of the
            # binaries that ran must fail the run — never exit 0 without it.
            print("MANIFEST_WRITE_FAILED: " + repr(manifest_error), flush=True)
            if not had_failure:
                raise SystemExit("run manifest is required evidence")
        print("evidence:", root, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir")
    parser.add_argument("--dual", action="store_true")
    parser.add_argument("--safe-index", action="store_true")
    parser.add_argument("--reference-binary-dir")
    parser.add_argument("--agent", help="real mynet executor for local A2 publish/verify smoke")
    parser.add_argument("--utcd", help="real utcd binary for A-S smoke")
    parser.add_argument("--security", default="token-lab", choices=["token-lab", "static-mtls"])
    parser.add_argument("--block-udp", action="store_true", help="drop all external UDP in the namespaces")
    parser.add_argument("--relay-host", help="ssh target of the public relay (E)")
    parser.add_argument("--relay-public-ip", help="public IP the client dials for TLS")
    parser.add_argument("--client-public-ip", help="public IP the relay uses as B's UDP target")
    parser.add_argument("--relay-ssh-key", help="private key file used for the relay ssh/scp")
    parser.add_argument("--utc-guard", help="utc-guard binary for static-mtls issuance/floors")
    parser.add_argument("--utc-tests", help="directory containing mynet_utc/tests helpers")
    parser.add_argument("--bridge", nargs=2)
    parser.add_argument("--capture")
    args = parser.parse_args()
    if args.bridge:
        bridge(*args.bridge)
    elif args.capture:
        capture(args.capture)
    else:
        assert args.binary_dir
        main(args.binary_dir, args.dual, args.safe_index, args.reference_binary_dir, args.utcd, args.agent,
             args.security, args.utc_guard, args.utc_tests, args.block_udp,
             args.relay_host, args.relay_public_ip, args.client_public_ip, args.relay_ssh_key)
