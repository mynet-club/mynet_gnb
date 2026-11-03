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


def main(binary_dir, dual=False, safe_index=False, reference_binary_dir=None):
    assert sys.platform.startswith("linux") and os.geteuid() == 0
    binary_dir = Path(binary_dir).resolve()
    reference_binary_dir = Path(reference_binary_dir).resolve() if reference_binary_dir else binary_dir
    root = Path(tempfile.mkdtemp(prefix="gnb-utc-topology-"))
    prefix = "gnbutc" + str(os.getpid())
    namespaces = []
    processes = []
    handles = []
    outcomes = {}
    participants = (1, 2, 3, 9000) if safe_index else (1, 2, 3)
    def run(*args, check=True):
        return subprocess.run([str(x) for x in args], check=check, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=15)
    def ns(name, *args, check=True):
        return run("ip", "netns", "exec", prefix + name, *args, check=check)
    def spawn(name, args, logfile):
        h = open(root / logfile, "w")
        handles.append(h)
        p = subprocess.Popen(["ip", "netns", "exec", prefix+name] + [str(x) for x in args], stdout=h, stderr=h)
        processes.append(p)
        return p
    def wait_for(predicate, timeout=15):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return
            time.sleep(0.2)
        raise AssertionError("timed out; evidence: " + str(root))
    def policy(node, gen, mode=1, ttl=600):
        peer, address, port = (2, "7f000001000000000000000000000000", 14001) if node == 1 else (1, "0afa0004000000000000000000000000", 14000)
        if dual and node == 2:
            address, port = "7f000001000000000000000000000000", 14002
        if not mode:
            address, port, ttl = "0" * 32, 0, 0
        expiry = int(time.time()) + ttl if mode else 0
        text = f"utc-v1 lab node{node} {node} {gen}\npeer {peer} 12 {gen} {mode} {expiry} {4 if mode else 0} {address} {port} 0 {'1'*64}\n"
        file = root / str(node) / "utc.conf"
        tmp = file.with_suffix(".new")
        tmp.write_text(text)
        tmp.chmod(0o600)
        tmp.replace(file)
    def applied(node, gen, state="APPLIED"):
        try:
            lines = (root / str(node) / "utc.status").read_text().splitlines()
            return int(lines[0].split()[5]) == gen and any(x.startswith("peer ") and state in x for x in lines)
        except (OSError, IndexError, ValueError):
            return False
    def ping(a, b, success=True):
        r = ns(str(a), "ping", "-c", "2", "-W", "1", f"10.42.0.{b}", check=False)
        assert (r.returncode == 0) == success, r.stdout
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
        this = Path(__file__).resolve()
        spawn("s", [sys.executable, this, "--capture", root / "udp.jsonl"], "capture.log")
        index_args = [binary_dir / "gnb", "-c", root / "9000"] if safe_index else [binary_dir / "gnb", "-P", "-l", "19000", "-4", "-b", root / "index.map", "--pid-file", root / "index.pid", "--console-log-level", "1"]
        spawn("5", index_args, "index.log")
        edge_mode = "dual-edge" if dual else "edge"
        edge = spawn("4", [sys.executable, this, "--bridge", edge_mode, str(root)], "edge.log")
        wait_for(lambda: (root / "edge.listening").exists())
        client = spawn("1", [sys.executable, this, "--bridge", "client", str(root)], "client.log")
        if dual:
            client_b = spawn("2", [sys.executable, this, "--bridge", "client-b", str(root)], "client-b.log")
        wait_for(lambda: (root / "edge.ready").exists() and (root / "client.ready").exists() and (not dual or (root / "client-b.ready").exists()))
        policy(1, 1)
        policy(2, 1)
        nodes = {}
        for n in range(1, 4):
            nodes[n] = spawn(str(n), [(reference_binary_dir if n == 3 else binary_dir) / "gnb", "-c", root / str(n)], f"gnb{n}.log")
        wait_for(lambda: applied(1, 1) and applied(2, 1))
        wait_for(lambda: ns("1", "ip", "link", "show", "gnb1", check=False).returncode == 0)
        ping(1, 2)
        ping(2, 1)
        ping(1, 3)
        ping(3, 1)
        outcomes["forced_tcp_bidirectional_tun_and_third_peer"] = "PASS"
        if reference_binary_dir != binary_dir:
            outcomes["unchanged_reference_peer_wire_interop"] = "PASS"
        # Snapshot the strict interval, exclude initial startup ARP/probe scheduling.
        start = time.time()
        ping(1, 2)
        ping(2, 1)
        time.sleep(2)
        packets = [json.loads(x) for x in (root / "udp.jsonl").read_text().splitlines()]
        interval = [p for p in packets if p[0] >= start]
        assert not any(p[1] == "10.250.0.1" and p[3] == "10.250.0.2" for p in interval), interval
        assert not any(p[1] == "10.250.0.2" and p[3] == "10.250.0.1" for p in interval), interval
        if not dual:
            assert any(p[1] == "10.250.0.4" and p[3] == "10.250.0.2" for p in interval)
            assert any(p[1] == "10.250.0.2" and p[3] == "10.250.0.4" for p in interval)
        outcomes["pair_zero_native_udp_with_native_candidates"] = "PASS"
        for node in (1, 2):
            host = f"10.250.0.{node}"
            assert any(p[1] == host and p[3] == "10.250.0.5" and p[5] in ("0801", "0803") for p in packets)
            assert any(p[1] == "10.250.0.5" and p[3] == host and p[5] in ("0802", "0804") for p in packets)
        outcomes["index_registration_query_response_preserved"] = "PASS"
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
        ping(1, 3)
        outcomes["missing_config_blocks_pair_only"] = "PASS"
        policy(1, 2)
        policy(2, 2)
        wait_for(lambda: applied(1, 2) and applied(2, 2))
        ping(1, 2)
        outcomes["new_generation_recovers_pair"] = "PASS"
        policy(2, 3, mode=0)
        wait_for(lambda: applied(2, 3))
        ping(2, 1, False)  # valid peer, wrong native source for A's strict pair
        ping(1, 3)
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
        ping(1, 3)
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
        ping(1, 3)
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
        wait_for(lambda: (root / "1" / "utc.status").read_text().splitlines()[0].split()[4] != old_boot, timeout=20)
        wait_for(lambda: applied(1, 7, "BLOCKED"))
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
            assert failed.returncode != 0 and "RECOVERY_REQUIRED" in failed.stdout, failed.stdout
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
        assert failed.returncode != 0 and "RECOVERY_REQUIRED" in failed.stdout, failed.stdout
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
        # Drop all disposable test secrets and gnb maps; metadata/logs remain.
        for f in root.glob("*.map*"):
            f.unlink()
        for n in participants:
            for subdir in ("security", "ed25519"):
                shutil.rmtree(root / str(n) / subdir, ignore_errors=True)
            for f in (root / str(n)).glob("*.map*"):
                f.unlink()
        print("evidence:", root, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir")
    parser.add_argument("--dual", action="store_true")
    parser.add_argument("--safe-index", action="store_true")
    parser.add_argument("--reference-binary-dir")
    parser.add_argument("--bridge", nargs=2)
    parser.add_argument("--capture")
    args = parser.parse_args()
    if args.bridge:
        bridge(*args.bridge)
    elif args.capture:
        capture(args.capture)
    else:
        assert args.binary_dir
        main(args.binary_dir, args.dual, args.safe_index, args.reference_binary_dir)
