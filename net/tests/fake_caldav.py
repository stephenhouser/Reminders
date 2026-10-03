#!/usr/bin/env python3
"""A small in-memory CalDAV server for the network tests (standard library only).

Prints the port it listens on, then serves until GET /_quit or 120 s idle.
User "alice", password "secret" (HTTP Basic). Layout:

    /.well-known/caldav        -> 301 /dav/
    /dav/                      current-user-principal: /dav/principals/alice/
    /dav/principals/alice/     calendar-home-set: /dav/calendars/alice/
    /dav/calendars/alice/X/    calendars, holding X/<name>.ics objects
"""

import base64
import itertools
import re
import sys
import threading
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DAV, CAL, CS, APPLE = "DAV:", "urn:ietf:params:xml:ns:caldav", "http://calendarserver.org/ns/", "http://apple.com/ns/ical/"
HOME = "/dav/calendars/alice/"
PRINCIPAL = "/dav/principals/alice/"
AUTH = "Basic " + base64.b64encode(b"alice:secret").decode()

calendars = {}  # href -> {"name", "color", "ctag", "objects": {href: (etag, data)}}
counter = itertools.count(1)
lock = threading.Lock()
idle = threading.Event()


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def multistatus(responses):
    body = ['<?xml version="1.0" encoding="utf-8"?>',
            f'<d:multistatus xmlns:d="{DAV}" xmlns:c="{CAL}" xmlns:cs="{CS}" xmlns:a="{APPLE}">']
    for href, props in responses:
        body.append(f"<d:response><d:href>{esc(href)}</d:href><d:propstat><d:prop>{''.join(props)}</d:prop>"
                    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>")
    body.append("</d:multistatus>")
    return "".join(body).encode()


def bump(cal):
    cal["ctag"] = str(next(counter))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def authorised(self):
        if self.headers.get("Authorization") == AUTH:
            return True
        self.body()
        self.send(401, b"", {"WWW-Authenticate": 'Basic realm="test"'})
        return False

    def calendar_of(self, path):
        return calendars.get(path[: path.rfind("/") + 1])

    def handle_one(self):
        idle.set()
        path = self.path
        if path == "/_quit":
            self.send(200)
            threading.Thread(target=self.server.shutdown).start()
            return
        if path == "/.well-known/caldav":
            self.body()
            self.send(301, b"", {"Location": "/dav/"})
            return
        if not self.authorised():
            return
        with lock:
            getattr(self, "do_" + self.command.lower() + "_")(path)

    def do_propfind_(self, path):
        self.body()
        if path == "/dav/":
            props = [f"<d:current-user-principal><d:href>{PRINCIPAL}</d:href></d:current-user-principal>"]
            return self.send(207, multistatus([(path, props)]))
        if path == PRINCIPAL:
            props = [f"<c:calendar-home-set><d:href>http://localhost{HOME}</d:href></c:calendar-home-set>"]
            return self.send(207, multistatus([(path, props)]))
        if path == HOME:
            out = [(HOME, ["<d:resourcetype><d:collection/></d:resourcetype>"])]
            if self.headers.get("Depth") == "1":
                for href, c in calendars.items():
                    comps = "".join(f'<c:comp name="{n}"/>' for n in c["comps"])
                    out.append((href, [
                        "<d:resourcetype><d:collection/><c:calendar/></d:resourcetype>",
                        f"<d:displayname>{esc(c['name'])}</d:displayname>",
                        f"<cs:getctag>{c['ctag']}</cs:getctag>",
                        f"<c:supported-calendar-component-set>{comps}</c:supported-calendar-component-set>",
                    ] + ([f"<a:calendar-color>{c['color']}FF</a:calendar-color>"] if c["color"] else [])))
            return self.send(207, multistatus(out))
        self.send(404)

    def do_report_(self, path):
        cal = calendars.get(path)
        root = ET.fromstring(self.body())
        if cal is None:
            return self.send(404)
        if root.tag == f"{{{CAL}}}calendar-query":
            out = [(h, [f"<d:getetag>{esc(e)}</d:getetag>"]) for h, (e, d) in cal["objects"].items() if "BEGIN:VTODO" in d]
            return self.send(207, multistatus(out))
        out = []
        for h in root.iter(f"{{{DAV}}}href"):
            if h.text in cal["objects"]:
                e, d = cal["objects"][h.text]
                out.append((h.text, [f"<d:getetag>{esc(e)}</d:getetag>", f"<c:calendar-data>{esc(d)}</c:calendar-data>"]))
        self.send(207, multistatus(out))

    def do_put_(self, path):
        data = self.body().decode()
        cal = self.calendar_of(path)
        if cal is None:
            return self.send(409)
        existing = cal["objects"].get(path)
        if self.headers.get("If-None-Match") == "*" and existing:
            return self.send(412)
        if self.headers.get("If-Match") and (not existing or existing[0] != self.headers["If-Match"]):
            return self.send(412)
        etag = f'"{next(counter)}"'
        cal["objects"][path] = (etag, data)
        bump(cal)
        self.send(204 if existing else 201, b"", {"ETag": etag})

    def do_delete_(self, path):
        self.body()
        if path in calendars:
            del calendars[path]
            return self.send(204)
        cal = self.calendar_of(path)
        if cal is None or path not in cal["objects"]:
            return self.send(404)
        if self.headers.get("If-Match") and cal["objects"][path][0] != self.headers["If-Match"]:
            return self.send(412)
        del cal["objects"][path]
        bump(cal)
        self.send(204)

    def props_from(self, body):
        root = ET.fromstring(body)
        name = root.find(f".//{{{DAV}}}displayname")
        color = root.find(f".//{{{APPLE}}}calendar-color")
        comps = [c.get("name") for c in root.iter(f"{{{CAL}}}comp")]
        return (name.text if name is not None else None,
                color.text if color is not None else None, comps)

    def do_mkcalendar_(self, path):
        name, color, comps = self.props_from(self.body())
        if path in calendars or not path.startswith(HOME):
            return self.send(405)
        calendars[path] = {"name": name or "", "color": color or "", "ctag": "0", "objects": {},
                           "comps": comps or ["VEVENT", "VTODO"]}
        bump(calendars[path])
        self.send(201)

    def do_proppatch_(self, path):
        name, color, _ = self.props_from(self.body())
        cal = calendars.get(path)
        if cal is None:
            return self.send(404)
        if name is not None:
            cal["name"] = name
        if color is not None:
            cal["color"] = color
        bump(cal)
        self.send(207, multistatus([(path, [])]))

    do_GET = do_PROPFIND = do_REPORT = do_PUT = do_DELETE = do_MKCALENDAR = do_PROPPATCH = handle_one


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(server.server_address[1], flush=True)

    def watchdog():
        while idle.wait(120):
            idle.clear()
        server.shutdown()

    threading.Thread(target=watchdog, daemon=True).start()
    server.serve_forever()


if __name__ == "__main__":
    main()
