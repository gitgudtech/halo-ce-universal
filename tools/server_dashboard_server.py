#!/usr/bin/env python3
"""Serve the Ready Up dashboard and bridge allowlisted commands over SSH."""

import argparse
import json
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.parse import urlsplit

TOOLS_DIR = Path(__file__).resolve().parent
UPDATE_REPOSITORY = "gitgudtech/halo-ce-universal"
MAPS = {
    "beavercreek": "Beaver Creek",
    "sidewinder": "Sidewinder",
    "damnation": "Damnation",
    "ratrace": "Rat Race",
    "prisoner": "Prisoner",
    "hangemhigh": "Hang 'Em High",
    "chillout": "Chill Out",
    "carousel": "Carousel",
    "boardingaction": "Boarding Action",
    "bloodgulch": "Blood Gulch",
    "wizard": "Wizard",
    "putput": "Putput",
    "longest": "Longest",
}
GAME_TYPES = {
    "race": "Race",
    "team_race": "Team Race",
    "rally": "Rally",
    "slayer": "Slayer",
    "team_slayer": "Team Slayer",
    "elimination": "Elimination",
    "stalker": "Stalker",
    "team_oddball": "Team Oddball",
    "accumulation": "Accumulation",
    "oddball": "Oddball",
    "ctf": "CTF",
    "ironctf": "Iron CTF",
    "king": "King",
    "team_king": "Team King",
}


class ConsoleBridge:
    def __init__(self, ssh_target, remote_port, host_address=None):
        self.host_address = host_address or ssh_target.rsplit("@", 1)[-1].split(":", 1)[0]
        ssh = shutil.which("ssh")
        if not ssh:
            raise RuntimeError("OpenSSH client 'ssh' was not found on PATH")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(("127.0.0.1", 0))
            self.local_port = probe.getsockname()[1]

        forward = f"127.0.0.1:{self.local_port}:127.0.0.1:{remote_port}"
        self.ssh = subprocess.Popen(
            [
                ssh,
                "-N",
                "-o", "BatchMode=yes",
                "-o", "ExitOnForwardFailure=yes",
                "-o", "ServerAliveInterval=20",
                "-L", forward,
                ssh_target,
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.lock = threading.Lock()
        self.console = None
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.ssh.poll() is not None:
                raise RuntimeError("SSH tunnel exited; check SSH access and remote Telnet")
            try:
                self.console = socket.create_connection(("127.0.0.1", self.local_port), timeout=0.2)
                self.console.setblocking(False)
                return
            except OSError:
                time.sleep(0.1)
        self.close()
        raise RuntimeError("SSH tunnel did not become ready")

    def close(self):
        if self.console:
            try:
                self.console.close()
            finally:
                self.console = None
        if self.ssh.poll() is None:
            self.ssh.terminate()
            try:
                self.ssh.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.ssh.kill()
                self.ssh.wait(timeout=3)

    @staticmethod
    def _read_command(stream, command, timeout=6.0):
        stream.sendall((command + "\r\n").encode("ascii"))
        received = bytearray()
        deadline = time.monotonic() + timeout
        last_data = None
        got_reply = False
        reply_marker = re.compile(rb"(?m)^(?:OK|ERR)(?:\s|$)")

        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            readable, _, _ = select.select([stream], [], [], min(0.25, remaining))
            if readable:
                chunk = stream.recv(4096)
                if not chunk:
                    break
                received.extend(chunk)
                if reply_marker.search(received):
                    got_reply = True
                    last_data = time.monotonic()
            elif got_reply and last_data is not None and time.monotonic() - last_data >= 0.25:
                break

        text = received.decode("ascii", errors="replace")
        if not got_reply:
            raise TimeoutError(f"No admin reply to {command!r}")
        return text

    def commands(self, commands):
        with self.lock:
            if self.ssh.poll() is not None:
                raise ConnectionError("SSH tunnel is no longer running")
            if self.console is None:
                self.console = socket.create_connection(("127.0.0.1", self.local_port), timeout=4)
                self.console.setblocking(False)
            try:
                return [self._read_command(self.console, command) for command in commands]
            except (OSError, TimeoutError):
                self.console.close()
                self.console = None
                raise

    def command(self, command):
        if command in {"status", "currentmap", "players", "stats", "invite", "mapqueue", "listmaps", "start", "end", "restart", "nextmap"}:
            return self.commands([command])[0]
        if re.fullmatch(r"gametype [a-z_]+", command):
            if command.split(" ", 1)[1] not in GAME_TYPES:
                raise ValueError("Unknown game type")
            return self.commands([command])[0]
        match = re.fullmatch(r"(map|queuemap) ([a-z0-9_]+)(?: ([a-z_]+))?", command)
        if match:
            if match.group(2) not in MAPS:
                raise ValueError("Unknown map alias")
            if match.group(3) and match.group(3) not in GAME_TYPES:
                raise ValueError("Unknown game type")
            return self.commands([command])[0]
        match = re.fullmatch(r"kick (-?\d+)", command)
        if match and int(match.group(1)) != -1:
            return self.commands([command])[0]
        raise ValueError("Command is not allowed by the dashboard bridge")

    def snapshot(self):
        status_text, players_text, queue_text, version_text = self.commands(
            ["status", "players", "mapqueue", "version"]
        )
        status_match = re.search(
            r"OK status=(\S+) map=(\S+) gametype=(\S+) players=(\d+) machines=(\d+)",
            status_text,
        )
        if not status_match:
            raise RuntimeError("Could not parse server status")
        phase, map_path, game_type, player_count, machine_count = status_match.groups()
        version_match = re.search(r"(?m)^OK version=(\d+)$", version_text.replace("\r", ""))
        map_alias = map_path.rsplit("\\", 1)[-1]

        players = []
        for line in players_text.splitlines():
            match = re.search(
                r"player id=(-?\d+) name=(.*?) machine=(-?\d+) controller=(-?\d+) team=(-?\d+) biped=(yes|no)",
                line,
            )
            if match:
                datum, name, machine, controller, team, biped = match.groups()
                machine_index = int(machine)
                team_index = int(team)
                if machine_index == 0 and int(controller) == 0 and name == "<unnamed>":
                    name = "Ready Up"
                players.append({
                    "id": int(datum),
                    "name": name,
                    "machine": machine_index,
                    "controller": int(controller),
                    "team": f"Team {team_index + 1}",
                    "host": machine_index == 0,
                    "biped": biped == "yes",
                })

        queue = []
        for line in queue_text.splitlines():
            match = re.search(r"OK mapqueue\[\d+\]=([^\s]+) gametype=([^\s]+)", line)
            if match:
                alias, mode = match.groups()
                queue.append({
                    "map": MAPS.get(alias, alias),
                    "alias": alias,
                    "gameType": GAME_TYPES.get(mode, mode),
                    "gameTypeAlias": mode,
                })

        return {
            "serverRunning": phase != "offline",
            "hostAddress": self.host_address,
            "phase": {"lobby": "Lobby", "in-game": "In Match", "postgame": "Postgame"}.get(phase, phase.title()),
            "map": MAPS.get(map_alias, map_alias),
            "mapAlias": map_alias,
            "gameType": GAME_TYPES.get(game_type, game_type),
            "gameTypeAlias": game_type,
            "playerCount": int(player_count),
            "machineCount": int(machine_count),
            "buildNumber": int(version_match.group(1)) if version_match else None,
            "players": players,
            "queue": queue,
            "updatedAt": time.strftime("%H:%M:%S"),
        }

    def stats_snapshot(self):
        stats_text = self.commands(["stats"])[0].replace("\r", "")
        if not re.search(r"(?m)^OK stats(?:\s|$)", stats_text):
            raise RuntimeError("Could not read server stats")

        def value_or_none(value):
            return None if value == "na" else int(value)

        teams = []
        for match in re.finditer(r"(?m)^team id=(\d+) score=(-?\d+)$", stats_text):
            team_index, score = match.groups()
            teams.append({"id": int(team_index), "team": f"Team {int(team_index) + 1}", "score": int(score)})

        players = []
        pattern = re.compile(
            r"(?m)^stat id=(-?\d+) score=(-?\d+|na) kills=(-?\d+|na) "
            r"assists=(-?\d+|na) deaths=(-?\d+|na)$"
        )
        for match in pattern.finditer(stats_text):
            player_id, score, kills, assists, deaths = match.groups()
            players.append({
                "id": int(player_id),
                "score": value_or_none(score),
                "kills": value_or_none(kills),
                "assists": value_or_none(assists),
                "deaths": value_or_none(deaths),
            })

        return {"players": players, "teams": teams, "updatedAt": time.strftime("%H:%M:%S")}

    def invite_url(self):
        response = self.command("invite").replace("\r", "")
        match = re.search(r"(?m)^OK invite=(halo://join/[0-9a-f]{64})$", response)
        if not match:
            raise RuntimeError("Server did not return an active invite link")
        return {"url": match.group(1)}

    def update_check(self):
        version_text = self.commands(["version"])[0].replace("\r", "")
        version_match = re.search(r"(?m)^OK version=(\d+)$", version_text)
        if not version_match:
            raise RuntimeError("Could not read the server build number")

        request = Request(
            f"https://api.github.com/repos/{UPDATE_REPOSITORY}/releases/latest",
            headers={
                "Accept": "application/vnd.github+json",
                "User-Agent": "Ready-Up-Halo-Server",
            },
        )
        with urlopen(request, timeout=10) as response:
            release = json.load(response)

        tag = release.get("tag_name", "")
        latest_match = re.fullmatch(r"build-(\d+)", tag)
        if not latest_match:
            raise RuntimeError("The latest fork release is not a build-N release")
        current_build = int(version_match.group(1))
        latest_build = int(latest_match.group(1))
        return {
            "currentBuild": current_build,
            "latestBuild": latest_build,
            "updateAvailable": latest_build > current_build,
            "releaseUrl": release.get("html_url"),
            "checkedAt": time.strftime("%H:%M:%S"),
        }


class DashboardHandler(SimpleHTTPRequestHandler):
    bridge = None
    port = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(TOOLS_DIR), **kwargs)

    def _local_request(self):
        host = self.headers.get("Host", "")
        allowed_hosts = {f"127.0.0.1:{self.port}", f"localhost:{self.port}"}
        if self.client_address[0] not in {"127.0.0.1", "::1"} or host not in allowed_hosts:
            self.send_error(403, "Local access only")
            return False
        origin = self.headers.get("Origin")
        if origin and origin not in {f"http://127.0.0.1:{self.port}", f"http://localhost:{self.port}"}:
            self.send_error(403, "Cross-origin requests are not allowed")
            return False
        return True

    def _json(self, code, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self._local_request():
            return
        if self.path == "/api/snapshot":
            try:
                self._json(200, self.bridge.snapshot())
            except Exception as error:
                self._json(503, {"error": str(error)})
            return
        if self.path == "/api/stats":
            try:
                self._json(200, self.bridge.stats_snapshot())
            except Exception as error:
                self._json(503, {"error": str(error)})
            return
        if self.path == "/api/invite":
            try:
                self._json(200, self.bridge.invite_url())
            except Exception as error:
                self._json(503, {"error": str(error)})
            return
        if self.path == "/api/update-check":
            try:
                self._json(200, self.bridge.update_check())
            except Exception as error:
                self._json(503, {"error": str(error)})
            return
        if self.path == "/":
            self.path = "/server-dashboard.html"
        super().do_GET()

    def do_POST(self):
        if not self._local_request():
            return
        if self.path != "/api/command":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 1 or length > 2048:
                raise ValueError("Invalid request size")
            payload = json.loads(self.rfile.read(length))
            command = payload.get("command")
            if not isinstance(command, str):
                raise ValueError("Command must be text")
            response = self.bridge.command(command)
            is_error = re.search(r"(?m)^ERR(?:\s|$)", response) is not None
            self._json(400 if is_error else 200, {"ok": not is_error, "response": response})
        except ValueError as error:
            self._json(400, {"ok": False, "error": str(error)})
        except Exception as error:
            self._json(503, {"ok": False, "error": str(error)})

    def log_message(self, format_string, *args):
        print("dashboard: " + format_string % args)


def main():
    parser = argparse.ArgumentParser(description="Run the Ready Up live server dashboard locally")
    parser.add_argument("--ssh-target", required=True, help="SSH target that can reach the Ubuntu host")
    parser.add_argument("--remote-telnet-port", type=int, default=2323)
    parser.add_argument("--host-address", help="Address to display in the dashboard (defaults to the SSH host)")
    parser.add_argument("--http-port", type=int, default=8765)
    args = parser.parse_args()

    bridge = ConsoleBridge(args.ssh_target, args.remote_telnet_port, args.host_address)
    DashboardHandler.bridge = bridge
    DashboardHandler.port = args.http_port
    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), DashboardHandler)
    server.daemon_threads = True
    print(f"Dashboard: http://127.0.0.1:{args.http_port}/", flush=True)
    print("Admin traffic is carried through SSH to the host's loopback-only Telnet port.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        bridge.close()


if __name__ == "__main__":
    main()
