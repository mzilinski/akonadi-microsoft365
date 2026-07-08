<!--
SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
SPDX-License-Identifier: LGPL-2.0-or-later
-->

# Akonadi Microsoft 365 (Graph) Resource

[![License: LGPL v2.0+](https://img.shields.io/badge/License-LGPLv2%2B-blue.svg)](LICENSES/LGPL-2.0-or-later.txt)
[![REUSE compliant](https://img.shields.io/badge/REUSE-compliant-green.svg)](https://reuse.software/)

An [Akonadi](https://community.kde.org/KDE_PIM/Akonadi) resource that connects
**KMail / KOrganizer / KAddressBook / Kontact** to **Microsoft 365 / Exchange Online**
through the **Microsoft Graph API** — mail, calendar and contacts, read and write.

It is a future-proof replacement for the EWS resource: Microsoft is switching EWS off
(blocked from 1 Oct 2026, removed ~April 2027). The code deliberately mirrors
`kdepim-runtime`'s `resources/ews` so it can be upstreamed there, while also building
stand-alone against installed KDE PIM packages.

## Features

- **Mail** — folder tree, message list and bodies (fetched on demand), read/unread and
  flag changes, move, delete, create/rename/delete folders, server-backed draft
  editing (a saved draft replaces its server copy instead of duplicating it).
- **Sending** — a separate transport agent (`akonadi_graphmta_resource`) hands outgoing
  MIME to the master resource over D-Bus; the sent copy is filed server-side (Graph does
  it on `/send`) with no local duplicate.
- **Special folders** — Inbox / Sent / Drafts / Trash / Junk / Outbox are tagged as
  Akonadi special collections, so KMail routes correctly and the unified-mailbox agent
  can include them.
- **Calendar** — events as KCalendarCore incidences (read + create/edit/delete);
  read-only calendars (Birthdays, Holidays, …) are marked read-only.
- **Contacts** — as KContacts addressees (read + create/edit/delete), including photos
  and categories.
- **Tasks** — Microsoft To Do lists as todo collections (read + create/edit/delete),
  with reminders, priorities and recurrence.
- **Incremental sync** — Graph delta queries for mail, calendar and contacts; only
  changes and tombstones are transferred after the first sync.
- **OAuth2** — authorization code + PKCE (public client, no secret), refresh token in
  KWallet/QtKeychain, silent refresh with interactive fallback and proactive renewal.
- **Robust REST layer** — 429/503 back-off honouring `Retry-After`, transparent
  `@odata.nextLink` paging, `$batch` for bulk payload fetches.

## Requirements

KDE PIM / Akonadi development packages. On Arch/CachyOS:

```
sudo pacman -S --needed akonadi akonadi-mime akonadi-calendar akonadi-contacts \
  kmime kcalendarcore kcontacts ki18n kconfig kcoreaddons \
  extra-cmake-modules qt6-networkauth qtkeychain-qt6
```

To view calendars/contacts you also want `korganizer` / `kaddressbook` (or `kontact`).

## Build & install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build
akonadictl restart
```

## Set up an account

You need your own free **Azure app registration** (client id + tenant) — the resource
ships with *no* embedded credentials. Follow **[docs/azure-setup.md](docs/azure-setup.md)**
(about 5 minutes), then:

1. In KMail/Kontact: *Settings → Accounts → Add → "Microsoft 365 (Graph)"*.
2. Open the account's *Configure* dialog, enter your **client id** and **tenant**.
3. Sign in when the browser opens; the refresh token is stored in KWallet.

## Architecture

The resource is an **Akonadi agent** — a plugin in the Akonadi sense: a standalone
executable registered via a `.desktop` descriptor and loaded/scheduled by the Akonadi
server. Internally it is organised as clear layers rather than a dynamic plugin registry:

```text
graphresource.{h,cpp}        Akonadi::ResourceBase — retrieve*/change-replay dispatch
graphmtaresource.{h,cpp}     transport agent, forwards MIME to the master over D-Bus
graphconfig.cpp              account configuration plugin (runs in the client process)
graphsyncstateattribute.*    per-collection @odata.deltaLink storage
graphclient/
  graphclient.{h,cpp}        QNetworkAccessManager + bearer token holder
  graphrequest.{h,cpp}       one REST call: auth + 429 back-off + paging + $batch bodies
  auth/graphoauth.{h,cpp}    OAuth2 (auth-code + PKCE), keychain persistence, refresh
mail/graphmailhandler        message  <-> MIME / flags
calendar/grapheventhandler   event    <-> KCalendarCore::Event
contact/graphcontacthandler  contact  <-> KContacts::Addressee
jobs/                        one KJob per high-level operation (fetch folders / items /
                             payloads / pim-items, batch change-replay)
tools/                       standalone live test binaries (not installed)
```

`retrieveItems()` and the change-replay handlers dispatch by the collection/item content
MIME type (mail vs. `application/x-vnd.akonadi.calendar.event` vs. contacts), so adding a
new content type means adding a handler + a fetch job and one dispatch branch. See
[IMPLEMENTATION.md](IMPLEMENTATION.md) for the full endpoint↔method mapping.

## Testing

The `tools/` binaries (built only in a stand-alone build, never installed) exercise the
code against a real tenant or a running Akonadi:

| Tool | What it checks |
| --- | --- |
| `graphsmoketest` | client layer + mail sync jobs (needs `GRAPH_ACCESS_TOKEN`) |
| `phase4check` | calendar/contacts read, write round-trip and delta incrementality |
| `writetest` | event/contact create/modify/delete through a running resource |
| `specialcheck` | special-collection tagging on a running resource |
| `akonadifetchtest` | on-demand mail payload retrieval through Akonadi |

## Known limitations

Deliberate gaps, roughly in the order users are likely to notice them:

- **Global Address List (GAL)** — only the personal contacts folder syncs; looking up
  other people in the organisation (`/users`, `/me/people`) is not implemented (it
  would need an additional OAuth scope and a read-only collection).
- **Shared and delegated mailboxes** — the resource only accesses the signed-in
  user's own mailbox (`/me/…`).
- **Task checklist items** — subtasks inside a Microsoft To Do task (`checklistItems`)
  are not mapped; iCalendar has no direct equivalent. They are preserved on the server
  when a task is edited from KDE.
- **"Nth weekday" recurrences** — rules like *every first Monday*
  (`relativeMonthly`/`relativeYearly`) are read as an approximation (monthly/yearly by
  date) and never written; deliberately left out rather than written wrongly. Note
  also that Microsoft To Do normalises counted repetitions (*10 times*) to
  never-ending server-side; calendar events keep their count.
- **Journal entries** (VJOURNAL) — no Graph equivalent, not synced.
- **Contact birthdays are read-only** — writing them would make Exchange create
  birthday calendar events on its own.
- **Messages copied into the account arrive flagged as draft** — Graph's MIME
  ingestion always creates drafts; the flag currently stays on the copy.
- **No account wizard integration yet** — accounts are added via KMail's
  *Add Custom Account…* plus the configure dialog (see above).

## License

LGPL-2.0-or-later. The project is [REUSE](https://reuse.software/) compliant; every file
carries SPDX headers and license texts live in `LICENSES/`.
