<!--
SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
SPDX-License-Identifier: LGPL-2.0-or-later
-->

# Azure App Registration for the Microsoft 365 (Graph) Akonadi resource

To let the resource authenticate against Microsoft 365 / Exchange Online (Microsoft
Graph) you need your own **Azure App Registration**. Microsoft allows this for free for
both personal and work/school accounts. The whole process takes about 5 minutes.

You end up with an **Application (client) ID** and a **tenant** value that you enter in
the account configuration dialog (*Settings → Accounts → Microsoft 365 (Graph) →
Configure*).

## 1. Register

1. Go to <https://entra.microsoft.com/> → **Applications** → **App registrations** →
   **New registration**.
2. **Name**: `Akonadi Microsoft 365` (anything works).
3. **Supported account types**: pick what fits you. For both personal and work use:
   - *Accounts in any organizational directory (multitenant) and personal Microsoft accounts*
4. **Redirect URI**: platform **Mobile and desktop applications**, URI exactly:
   `http://localhost:53682/callback`
   (The resource listens on port 53682 and falls back to a random free port if it is
   taken; Azure ignores the port for `http://localhost` desktop redirect URIs, so this
   one entry is enough. To pin a different port, change `kCallbackPort` in
   `graphclient/auth/graphoauth.cpp`.)
5. Click **Register**.

## 2. Enable public-client mode

1. Left sidebar → **Authentication**.
2. Scroll to **Advanced settings** → **Allow public client flows** → **Yes**.
3. Save.

> Do **not** create a client secret. The resource is a public client and uses PKCE.

## 3. Configure API permissions

1. Left sidebar → **API permissions** → **Add a permission** → **Microsoft Graph** →
   **Delegated permissions**.
2. Search and enable:
   - `offline_access`
   - `User.Read`
   - `Mail.ReadWrite`
   - `Mail.Send`
   - `Calendars.ReadWrite`
   - `Contacts.ReadWrite`
3. Click **Add permissions**.
4. Personal Microsoft accounts do not need admin consent. Work/school tenants may
   require an administrator to *Grant admin consent* for the delegated scopes above.

## 4. Copy the values

From the app registration **Overview**:

- **Application (client) ID** — a UUID, e.g. `12345678-aaaa-bbbb-cccc-1234567890ab`.
- **Directory (tenant) ID** — only needed for single-tenant apps; multi-tenant uses
  `common`.

## 5. Enter them in the resource

After installing (see the top-level [README](../README.md)):

1. `akonadictl restart`
2. In KMail/Kontact: *Settings → Accounts → Add → "Microsoft 365 (Graph)"*.
3. Open the account's *Configure* dialog and fill in **Application (client) ID** and
   **Azure tenant**.
4. The resource opens your browser for sign-in; Microsoft asks for consent and the
   refresh token is stored in KWallet.

## Tenant values at a glance

| Value | Meaning |
|---|---|
| `common` | Personal **and** work/school accounts |
| `organizations` | Work/school only (any tenant) |
| `consumers` | Personal Microsoft accounts only |
| `<tenant-uuid>` | Exactly your own work/school tenant |

## Token storage

The refresh token lives in **KWallet/QtKeychain** under:
- Service: `akonadi_graph_resource`
- Key: the resource instance id (e.g. `akonadi_graph_resource_0`)

Removing the account in KMail/Kontact, or deleting the entry via the KWallet manager,
signs you out.
