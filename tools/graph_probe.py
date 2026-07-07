#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
# SPDX-License-Identifier: LGPL-2.0-or-later
"""
Phase-0 probe for the Akonadi Graph resource.

Validates the two things the resource depends on, WITHOUT building any KDE code:
  1. OAuth2 device-code flow against Azure AD v2.0 (Graph scopes).
  2. The delta queries the resource maps to retrieveCollections()/retrieveItems():
        GET /me/mailFolders/delta
        GET /me/mailFolders/{inbox}/messages/delta
        GET /me/messages/{id}/$value   (raw MIME — what KMail consumes)

Usage:
    python3 graph_probe.py --client-id <AZURE_APP_ID> [--tenant common]

Requires a public-client Azure app registration with device-code flow enabled and
delegated permissions: offline_access, User.Read, Mail.ReadWrite.
Only depends on the Python standard library.
"""
import argparse
import json
import sys
import time
import urllib.parse
import urllib.request

SCOPES = "offline_access User.Read Mail.ReadWrite"
GRAPH = "https://graph.microsoft.com/v1.0"


def _post(url, data):
    body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(url, data=body, method="POST")
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def _get(url, token, raw=False):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req) as r:
        return r.read() if raw else json.load(r)


def device_code_login(tenant, client_id):
    base = f"https://login.microsoftonline.com/{tenant}/oauth2/v2.0"
    dc = _post(f"{base}/devicecode", {"client_id": client_id, "scope": SCOPES})
    print("\n== Sign in ==")
    print(dc["message"], "\n")  # "go to https://microsoft.com/devicelogin and enter CODE"

    interval = int(dc.get("interval", 5))
    while True:
        time.sleep(interval)
        try:
            tok = _post(f"{base}/token", {
                "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                "client_id": client_id,
                "device_code": dc["device_code"],
            })
            return tok["access_token"]
        except urllib.error.HTTPError as e:
            err = json.load(e).get("error")
            if err == "authorization_pending":
                continue
            if err == "slow_down":
                interval += 5
                continue
            raise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--client-id", required=True)
    ap.add_argument("--tenant", default="common")
    args = ap.parse_args()

    token = device_code_login(args.tenant, args.client_id)
    print("[ok] got access token\n")

    # 1) folder delta  -> retrieveCollections()
    folders = _get(f"{GRAPH}/me/mailFolders/delta?$select=id,displayName,parentFolderId", token)
    names = [f.get("displayName") for f in folders.get("value", []) if "displayName" in f]
    print(f"[folders] {len(names)} folders: {', '.join(names[:12])}"
          + (" ..." if len(names) > 12 else ""))
    if "@odata.deltaLink" in folders:
        print("[folders] got @odata.deltaLink (this becomes the persisted sync state)")

    inbox = _get(f"{GRAPH}/me/mailFolders/inbox?$select=id,displayName", token)
    inbox_id = inbox["id"]

    # 2) message delta -> retrieveItems(collection)
    msgs = _get(f"{GRAPH}/me/mailFolders/{inbox_id}/messages/delta"
                f"?$select=id,isRead,subject&$top=10", token)
    items = msgs.get("value", [])
    print(f"\n[inbox] pulled {len(items)} message stubs (delta):")
    for m in items[:5]:
        flag = " " if m.get("isRead") else "*"
        print(f"  [{flag}] {m.get('subject','(no subject)')[:70]}")

    # 3) raw MIME -> retrieveItems(items, parts)  (what KMail actually renders)
    if items:
        mid = items[0]["id"]
        mime = _get(f"{GRAPH}/me/messages/{mid}/$value", token, raw=True)
        head = mime[:200].decode("utf-8", "replace").replace("\r\n", " | ")
        print(f"\n[mime] /$value returned {len(mime)} bytes of RFC822. Head: {head} ...")
        print("\n[done] auth + folder-delta + message-delta + MIME all work.")
        print("       The Akonadi jobs in ../jobs/ wrap exactly these three calls.")


if __name__ == "__main__":
    try:
        main()
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')}", file=sys.stderr)
        sys.exit(1)
