# Akonadi Microsoft Graph Resource — Implementierungsplan

Ziel: Eine Akonadi-Resource `akonadi_graph_resource`, die Exchange Online / Microsoft 365
über die **Microsoft Graph API** anbindet — als zukunftssicherer Ersatz für die EWS-Resource
(EWS-Abschaltung: Blocking ab 1. Okt. 2026, endgültig ~April 2027).

Verwendet mit **KMail / Kontact** wie jede andere Akonadi-Mail-Resource.

Diese Resource ist als eigenständige (out-of-tree) Resource strukturiert, orientiert sich aber
1:1 an `resources/ews`, damit ein späteres Upstreaming nach kdepim-runtime trivial ist.

---

## 1. Architektur (übernommen von der EWS-Resource)

```
graphresource            → Akonadi::ResourceBase-Subklasse (Empfang: Mail/Kalender/Kontakte)
graphmtaresource         → separater Sende-Agent (KMail verlangt getrennte Send-/Receive-Resources)
graphclient/             → Protokoll-Layer (REST/JSON statt SOAP/XML)
  graphclient            → zentrale Client-Klasse (hält QNetworkAccessManager + Auth)
  graphrequest           → Basis-KJob für einen Graph-Call (inkl. 429-Backoff, Paging)
  auth/graphoauth        → OAuth2 (Auth-Code + PKCE) gegen login.microsoftonline.com
mail/graphmailhandler    → Item-Handler: MIME <-> Graph-message-Mapping
jobs/                    → je eine KJob-Klasse pro High-Level-Operation
```

### Zentrale Erkenntnis für KMail
KMail arbeitet auf **MIME** (`KMime::Message`). Graph liefert die Roh-MIME direkt über
`GET /messages/{id}/$value`. Damit ist das Payload-Mapping trivial — genau wie EWS über
die `item:MimeContent`-Property. **Kein** Zusammenbauen aus JSON-Feldern nötig.

---

## 2. Graph-Endpoint → Akonadi-Methoden-Mapping (Mail, Phase 1)

| Akonadi `ResourceBase`-Methode         | Graph-Aufruf                                              | Akonadi-Rückmeldung                          |
|----------------------------------------|----------------------------------------------------------|----------------------------------------------|
| `retrieveCollections()`                | `GET /me/mailFolders/delta` (voll: `?$top=...`)          | `collectionsRetrieved[Incremental]()`        |
| `retrieveItems(collection)`            | `GET /me/mailFolders/{id}/messages/delta`                | `itemsRetrieved[Incremental]()`              |
| `retrieveItems(items, parts)`          | `GET /me/messages/{id}/$value` → RFC822                  | `setPayload<KMime::Message::Ptr>`, `itemsRetrieved()` |
| `itemAdded(item, col)`                 | `POST /me/mailFolders/{id}/messages` (MIME als Draft)    | `changeCommitted(item)`                       |
| `itemChanged()` (Seen-Flag)            | `PATCH /me/messages/{id}` `{ "isRead": true }`           | `changeCommitted(item)`                       |
| `itemsFlagsChanged()`                  | `PATCH /me/messages/{id}` (isRead / flag.flagStatus)     | `changeProcessed()`                           |
| `itemsMoved()`                         | `POST /me/messages/{id}/move` `{ destinationId }`        | `changeCommitted(items)`                      |
| `itemsRemoved()`                       | `DELETE /me/messages/{id}` (bzw. move → deleteditems)    | `changeProcessed()`                           |
| `collectionAdded()`                    | `POST /me/mailFolders` `{ displayName }`                 | `changeCommitted(collection)`                 |
| `collectionChanged()` (rename)         | `PATCH /me/mailFolders/{id}` `{ displayName }`           | `changeCommitted(collection)`                 |
| `collectionRemoved()`                  | `DELETE /me/mailFolders/{id}`                            | `changeProcessed()`                           |
| `sendItem()` (MTA-Resource)            | Draft aus MIME + `POST /me/messages/{id}/send`           | `itemSent(...)`                               |

### Delta-Sync (= EWS SyncState-Äquivalent)
- Voller Sync liefert am Ende einen `@odata.deltaLink`.
- Diesen Link **persistieren** (pro Collection als Attribut, analog `EwsSyncStateAttribute`).
- Nächster Sync ruft den `deltaLink` direkt auf → nur Änderungen (added/updated/`@removed`).
- Gelöschte Items kommen als Objekt mit `@removed`-Annotation → in `...Incremental()` als removed.

---

## 3. Wichtige Graph-Besonderheiten (unterscheiden sich von EWS)

1. **Kein Push für Desktop.** EWS hat Streaming-Subscriptions; Graph nutzt Webhooks, die einen
   öffentlich erreichbaren HTTPS-Endpoint bräuchten → für einen Desktop-Client unbrauchbar.
   → Stattdessen **Polling per Delta-Query** (Intervall wie IMAP-Resources). `graphsubscription`
     bleibt daher vorerst leer/optional.
2. **Rate-Limiting.** Graph antwortet aggressiv mit `429 Too Many Requests` + `Retry-After`.
   → Zentral in `GraphRequest` behandeln: bei 429 nach `Retry-After` Sekunden erneut.
3. **Paging.** Listen liefern `@odata.nextLink`. `GraphRequest` muss transparent nachladen.
4. **MIME senden:** `POST /me/sendMail` akzeptiert **nur JSON**. Für Roh-MIME:
   `POST /me/messages` mit `Content-Type: text/plain` und Base64-MIME erzeugt eine Nachricht,
   danach `POST /me/messages/{id}/send`.
5. **Feature-Lücken ggü. EWS:** öffentliche Ordner, bestimmte Shared-Mailbox-Fälle, GAL
   (Graph nutzt `/users` bzw. People-API). Für Phase 1 (persönliches Postfach) irrelevant.

---

## 4. OAuth2 / Azure

- Endpoint v2.0: `https://login.microsoftonline.com/{tenant}/oauth2/v2.0/authorize` + `/token`
  (`{tenant}` = `common` für Multi-Tenant, oder konkrete Tenant-ID).
- Flow: **Authorization Code + PKCE** (Desktop, kein Client-Secret).
- Scopes (Phase 1): `offline_access https://graph.microsoft.com/Mail.ReadWrite
  https://graph.microsoft.com/Mail.Send https://graph.microsoft.com/User.Read`
  (später `Calendars.ReadWrite`, `Contacts.ReadWrite`).
- **Azure-App-Registrierung** nötig (eigene Client-ID). Redirect-URI: `http://localhost` (loopback)
  oder `https://login.microsoftonline.com/common/oauth2/nativeclient`.
- Token-Speicher: QtKeychain/KWallet — die vorhandene EWS-`ewsoauth`-Mechanik ist fast 1:1 nutzbar.

---

## 5. Phasenplan

Stand 2026-07-07: Phase 0–3 implementiert; Client-Schicht + Sync-Jobs live gegen den
Tenant verifiziert (`tools/graphsmoketest.cpp`). Offen: Config-Dialog, `$batch`,
Phase 4/5 und der erste Test in einer laufenden Akonadi-Instanz (interaktiver Login).

**Phase 0 — Prototyp (ohne Akonadi)** ✅ erledigt
  Endpoints (Folder-Delta, Message-Delta, `/$value`-MIME) gegen den echten Tenant
  validiert; Smoke-Test-Binary ersetzt das Python-Probe-Tool.

**Phase 1 — Mail lesen** ✅ implementiert
  `retrieveCollections` (Folder-Delta) → `retrieveItems` (Message-Delta) → Payload (`$value`).
  Ergebnis: Postfach erscheint in KMail, Mails lesbar. Read-only.

**Phase 2 — Mail schreibend** ✅ implementiert
  Seen-Flag (`PATCH isRead`), Move, Delete, Ordner anlegen/umbenennen/löschen.

**Phase 3 — Senden** ✅ implementiert (D-Bus-Delegation wie EWS-MTA)
  `graphmtaresource`: Draft aus MIME + `/send`.

**Phase 4 — Kalender & Kontakte** ✅ implementiert & live verifiziert
  `calendar/grapheventhandler` (`/me/calendars/{id}/events` ↔ KCalendarCore::Event),
  `contact/graphcontacthandler` (`/me/contacts` ↔ KContacts::Addressee),
  `jobs/graphfetchpimitemsjob` (Vollsync mit Payload). Dispatch per Collection-MimeType.
  Read+Write gegen echten Tenant getestet (`tools/phase4check`).

**Phase 5 — Upstreaming**
  Nach kdepim-runtime einreichen, mit Pflege-Zusage.

---

## 6. Build & Test (out-of-tree)

Siehe `README.md`. Kurz:
```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
# Resource registrieren:
akonadictl restart
# In KMail: Einstellungen → Konten → Hinzufügen → "Microsoft 365 (Graph)"
```

Test gegen echten Tenant: eigene Azure-App-Registrierung eintragen (Client-ID in Konto-Dialog).
