# Accounts on maid-server

Design for the roadmap item "Accounts on maid-server". Micaiah's decisions (2026-09-30) are the frame and are not reopened here: accounts live in MAID's own server and never in llama.cpp; password login first with TOTP as the second factor, passkeys later; argon2id for password hashing; an encrypted store for anything sensitive; email verification on signup through a local relay, the one outbound step and only on the user's action; OAuth later through a generic OIDC hook; short-lived session tokens with refresh and revocation; rate limits on logins and codes; no secrets in URLs; recovery that does not weaken 2FA; the audit log naming the user; per-user session ownership; an admin list; today's per-device tokens folded into per-user ones; `maid join SERVER` on the client.

Two rules from [remote.md](../remote.md) and [harness.md](../harness.md) hold throughout: the server stays the only thing a client talks to (item 3 puts llama.cpp behind it, never beside it), and every remote request is `Origin::Remote`, asked about in every mode, with no route that can reset the tripwire. Nothing in this design touches either.

## Dependency

The vcpkg tree has both `libsodium` (1.0.20, ISC) and `argon2` (20190702, Apache-2.0 or CC0). Recommendation: **libsodium**, one library for the whole feature:

| Need | libsodium call |
| :--- | :--- |
| argon2id, PHC string with parameters and salt, upgrade check | `crypto_pwhash_str`, `crypto_pwhash_str_verify`, `crypto_pwhash_str_needs_rehash` |
| the encrypted store | `crypto_aead_xchacha20poly1305_ietf_*` (24-byte random nonce, associated data) |
| constant-time compare | `sodium_memcmp` |
| random tokens, codes, salts | `randombytes_buf` |
| the master key in memory | `sodium_mlock`, `sodium_memzero` |

The `argon2` port alone would give the hash and nothing else; the store and the compare would stay on OpenSSL (`EVP_aes_256_gcm`, `CRYPTO_memcmp`), two libraries for one feature and a hand-rolled envelope format. Its parallelism knob is the only thing libsodium lacks (libsodium fixes `p = 1`); the parameters below are chosen so that does not matter. OpenSSL stays for TLS, the certificate and SHA-256; it is not replaced. [cleanroom.md](../cleanroom.md) gains one row: libsodium, ISC, password hashing, the sealed store, random tokens.

## Threat model

| Attacker holds | Gets | Does not get | Because |
| :--- | :--- | :--- | :--- |
| A stolen refresh or access token (today: a device token) | that session's reach until it is revoked, expires, or its theft is detected | a password change, a TOTP change, new recovery codes, another user's sessions, a tripwire reset | those routes re-check the password (and the TOTP code where enrolled); refresh rotation detects a reused token and ends the whole session family; the owner sees every session with device and source and can revoke it; the reset route does not exist |
| The store files without the master key | usernames, emails, timestamps, device names, SHA-256 of refresh tokens | password hashes, TOTP secrets, recovery codes, OIDC subjects | those fields are sealed; a refresh token hash has no useful preimage |
| The store files and the master key | argon2id hashes (slow to attack), TOTP secrets (immediate), recovery code hashes | passwords without an offline attack at 64 MiB per guess | this is the "key and file" case: the answer is `maid server key rotate` plus re-enrolment of every TOTP, and the audit rows say when the files were last written |
| A shell on the workstation as the user | everything the user has, including the key, the files and the audit log | a tripwire reset; a silent reset of the lock from any route | accounts do not defend against this and the design does not pretend to; the root-owned tripwire does, which is why the state directory joins the harness's protected paths so an agent cannot read or write it |
| A phishing page | a password and a TOTP code relayed inside its 30 s window | a long-lived foothold that stays hidden | the resulting session is listed with its source and device; the client pins the certificate and `maid join` shows the fingerprint; TOTP is phishable by design, and passkeys are the fix when a stable hostname exists (open question 11) |
| Online brute force | nothing in useful time | account enumeration, a lockout of the real owner | argon2id cost on every attempt, a dummy hash for unknown names so timing and wording match, per-source limits, per-account backoff that slows rather than locks |
| A second account on the same server | what that account's reach allows on Micaiah's workstation | more than its grant | per-user workspaces and a mode cap (open question 1) |
| The relay (roadmap item 1) | connection times and sizes | any token, code or password | everything here rides inside the end-to-end tunnel |

## Data model and where it lives

MAID has no SQL dependency and this feature does not justify one: a handful of users, a few dozen sessions, lookups by name or by token hash over a few kilobytes. The existing pattern fits: JSON written to a temporary file and renamed into place (today's `tokens.json`), an append-only text log, everything readable with a text tool and nothing a migration tool has to understand. SQLite would add a dependency, a schema version story and a binary file the audit habit cannot grep. Revisit only on a measured problem.

Everything stays in `~/.local/state/maid/server/` (0700), which already holds `tokens.json`, `audit.log` and the TLS pair:

| File | Contents | Written |
| :--- | :--- | :--- |
| `master.json` | the sealing keys, 0600 | first start, `key rotate` |
| `accounts.json` | users, with sealed fields | on each account change (rare) |
| `sessions.json` | refresh sessions, hashes only | on login, refresh, revoke |
| `audit.log` | one line per request and per auth event | append |

Short-lived material (access tokens, TOTP challenges, verification and setup codes, rate tables) is held in memory only: a restart drops it, clients refresh, a code is asked for again. Nothing a restart loses is worth a file, and a restart needs the workstation.

Record shapes, one line each in prose form:

```
user     {id, name, email, created, state, admin, must_enrol,
          password: sealed(PHC string), totp: {secret: sealed, confirmed, last_step} | null,
          recovery: [sealed(sha256)], oidc: [{issuer, subject: sealed}],
          workspaces: [...] | null, max_mode: "plan" | ... | null}
session  {id, user, device, created, last_seen, source, kind: "user" | "legacy",
          refresh_sha256, prev_sha256, prev_until, expires}
sealed   {k: key id, n: base64 nonce, c: base64 ciphertext}
```

`state` is `pending_verify`, `pending_approval`, `active` or `disabled`. `id` is 16 random bytes base64url, never reused, so renames do not disturb sessions or audit rows. In memory: access tokens as `sha256 -> {session, expires}`, challenges as `sha256 -> {user, device, source, attempts, expires}`, codes as `user -> {sha256, kind, attempts, expires}`.

## Encryption at rest

**Master key.** 32 random bytes made on first start, kept in `master.json` as `{keys: [{id, key, created}], active}`, 0600 in the 0700 directory, locked in memory with `sodium_mlock` and zeroed on exit. It never leaves the workstation and is never derived from anything a client sends. The file is in the same directory as the data it protects, so the key protects against the leaked-file case (a backup, a copied directory, a sync tool) and not against a shell as the user; that honesty is in the threat model, and a passphrase-wrapped mode is open question 2.

**What is sealed, what is hashed, what is plain.** Sealed: password hashes, TOTP secrets, recovery code hashes, OIDC subjects, and any provider token the OIDC hook ever keeps. Hashed with SHA-256 and not sealed: refresh tokens, access tokens, challenges and codes, all of which are random values with no preimage worth protecting. Plain: user ids, names, emails, states, flags, timestamps, device names, source addresses. The audit log names users, so names are not treated as secret.

**Sealing.** `crypto_aead_xchacha20poly1305_ietf` with a fresh random 24-byte nonce per write and associated data `"maid:" + user id + ":" + field name`. The associated data means a ciphertext cut from one user's record and pasted into another's, or from the `totp` field into the `password` field, fails to open. Each sealed value carries the id of the key that sealed it.

**Rotation.** `maid server key rotate` adds a key, marks it active, re-seals every field under it in one atomic rewrite of `accounts.json`, then removes the old key from `master.json`. Reads accept any key id still in the file, so a crash between the two writes leaves everything openable. Rotation is also the response to a suspected key and file leak, together with forced TOTP re-enrolment for every user.

**Argon2id parameters.** 64 MiB memory, 3 passes, parallelism 1, a 16-byte salt and a 32-byte tag, which is libsodium's `OPSLIMIT 3, MEMLIMIT 64 MiB` with `ALG_ARGON2ID13`. Reasoning: RFC 9106 gives 64 MiB and 3 passes as its second recommended setting (its first, 2 GiB, is for servers with nothing else to do); the workstation runs two llama servers and ComfyUI, and 64 MiB per attempt at roughly 100 to 200 ms on one core is a cost a human never notices and an attacker pays on every guess. Parallelism is 1 because libsodium offers nothing else and a workstation login should not take four cores. Concurrent hashes are capped at four with a counting semaphore, so a login flood costs at most 256 MiB and queues rather than swaps. The PHC string stores every parameter and the salt (`$argon2id$v=19$m=65536,t=3,p=1$...`); on each successful login `crypto_pwhash_str_needs_rehash` compares against the current settings and rehashes with the password in hand, so raising the cost later is a one-line change that upgrades accounts as they sign in.

## Password policy

Length over composition: at least 12 characters, at most 256 (bounding the hash input), any Unicode, taken as the UTF-8 bytes given with no trimming or normalisation, so what the user typed is what verifies. No character class rules, no forced rotation. Rejected: a password equal to the username or the email's local part, or containing the server's hostname. A breached-list check runs only against a local file (`server.password_blocklist`, a path to a newline-separated list the user downloaded themselves, one entry per line, compared case-insensitively); with no file configured there is no check, and there is never a lookup against a web service, because that is an outbound request and the No Internet rule applies to the server. The web client shows a length counter and nothing else.

## TOTP

RFC 6238 over HMAC-SHA1, 6 digits, 30 s step, which is what every authenticator app implements. The secret is 20 random bytes (160 bits), the size RFC 4226 recommends, shown once as base32 inside an `otpauth://totp/MAID:<name>?secret=...&issuer=MAID&digits=6&period=30` URI, and sealed in the store. Enrolment is two steps: `enrol` creates an unconfirmed secret and returns the URI; `confirm` takes one valid code and only then marks the enrolment confirmed, so a user whose app never scanned is not locked out. Verification accepts the current step and one step either side (drift of 30 s each way) and records the highest step accepted as `last_step`; a code for a step at or below `last_step` is refused, which is the replay protection, and it also stops the same code being used for login and then for a sensitive action within the same window.

Recovery codes: 8 codes of 10 characters from a 32-letter alphabet without `0 O 1 I` (50 bits each), shown once as `xxxxx-xxxxx`, stored as sealed SHA-256 hashes. A code is one-time because its entry is deleted when it verifies; the user sees how many remain in `GET /api/auth/me`. Regenerating replaces the whole set and needs the password and a TOTP code. A login that used a recovery code gets `notice: "recovery code used, N left"` and, at zero, `must_enrol` is set so the next login lands in enrolment.

The server setting `server.require_2fa = true` enforces TOTP for everyone: an account without a confirmed enrolment logs in to a session whose only reachable routes are `me`, `totp/enrol`, `totp/confirm`, `logout`, until it confirms. Optional per account otherwise.

## Login flow

1. `POST /api/auth/login {name, password, device}`. The server finds the user by name (case-insensitive). If there is none, it runs `crypto_pwhash_str_verify` against a dummy PHC string made at start, so the attempt costs the same as a real one. Unknown name, wrong password and a disabled account all answer `401 {"error": "wrong username or password", "code": "wrong_credentials"}`, same wording, same work. A correct password on a `pending_verify` or `pending_approval` account answers `403 verify_first` or `403 pending_approval`: the account's existence is told only to someone holding its password.
2. Password correct and no confirmed TOTP: step 4.
3. Password correct and TOTP confirmed: `200 {"second_factor": "totp", "challenge": "mac_..."}`. The challenge is 32 random bytes, kept hashed in memory with the user, device, source, an attempt counter and a 5 minute expiry. The client sends `POST /api/auth/totp {challenge, code}` or `{challenge, recovery_code}`. A wrong code is `401 wrong_code`; the fifth wrong code deletes the challenge and answers `401 challenge_expired` as an expired one does, so the user starts over at step 1 and pays the password cost again.
4. Success: a session row is written, and the answer is `200 {access_token, expires_in, refresh_token, session: {id, device}, user: {id, name, admin, totp, recovery_left, must_enrol}}`. Tokens travel in the body and in the `Authorization` header only, never in a query string or a path, and every auth route is `POST`.

The web client shows the one 401 message for every failure in step 1 and never says which part was wrong; the terminal client prints the same sentence.

## Session tokens

Both tokens are opaque random values, 32 bytes base64url, with a prefix that names the kind: `mat_` access, `mar_` refresh, `mac_` challenge. The prefix lets the audit log, the fuzzer and a person reading a leaked string tell what they have, and lets the parser refuse the wrong kind at the wrong route before any lookup. No JWT: one server holds its own state, so there is no signature to verify, no algorithm field to confuse, and revocation is deleting a row.

| | Access | Refresh |
| :--- | :--- | :--- |
| Lives in | memory, `sha256 -> session` | `sessions.json`, SHA-256 only |
| Lifetime | 15 minutes | 30 days sliding from last use, 90 days absolute from login |
| On use | looked up, session `last_seen` updated at most once a minute | rotated: a new refresh token is issued, the old hash moves to `prev_sha256` with `prev_until` 60 s away |
| Revoked by | deleting the session row (the access token dies with it) | the same |

Rotation on refresh is the theft detector: a refresh token presented after its 60 s grace, because it was already rotated, revokes the whole session (both hashes) and writes `refresh_reuse` to the audit log. The grace exists for a client that lost the response to its own refresh and retries. A session is bound to the `device` name given at login and records the source of its last refresh; `GET /api/auth/sessions` lists the user's own sessions with both, `DELETE /api/auth/sessions/{id}` revokes one, and `logout` revokes the current one. There is no separate revocation list because every token that exists is in a row; what is not in a row is invalid.

Sensitive actions (changing the password, enrolling or removing TOTP, generating recovery codes, revoking other sessions, linking OIDC) take `password` in the body and, where TOTP is enrolled, `code`, checked again against the account. A stolen access token is a 15 minute window into the harness, which is already asked about every action, and nothing more.

## Signup and email verification

`server.signup` is `closed` (default), `email` or `approve`.

* **closed**: only an admin creates accounts, `maid server user invite NAME EMAIL` or `POST /api/admin/users`. The invite prints a setup code; the person completes `POST /api/auth/setup {code, password}` and the account is `active`. If mail is configured the code can be mailed instead of printed.
* **email**: `POST /api/auth/signup {name, email, password}` creates a `pending_verify` user and mails a code; `POST /api/auth/verify {name, code}` activates. `409 name_taken` for a taken name or email; enumeration through signup is accepted on a server whose users are known to its admin, and the rate limit covers scraping.
* **approve**: signup creates `pending_approval`, no mail is sent, the answer is `202 pending_approval`, and an admin approves by API or `maid server user approve NAME`. This is what the server does when `signup = email` is set but no mail command exists: it degrades to approve at start and prints why, rather than failing signups one by one.

Codes are 8 digits (a phone keyboard, no letter confusion), kept hashed in memory with a 15 minute expiry and 5 attempts; the fifth wrong attempt voids it and `POST /api/auth/verify/resend` makes a new one, at most 3 sends per account per hour. Setup codes (invites and recovery) are the same mechanism with a 24 hour expiry, since they wait for a person.

**The mail step.** `server.mail = { command = "/usr/sbin/sendmail -t -i", from = "maid@hostname" }`. The server writes the message to the command's stdin and makes no network connection itself; the local relay (postfix, msmtp, a container on the LAN) is the user's, configured by the user, and is what reaches the internet. This keeps the roadmap's rule literally: MAID's server has no SMTP client, no DNS lookup of a mail host and no connection to make, and the only time the command runs is on signup, resend, or recovery, each a user's action. A built-in SMTP client is open question 4. The message body is a template with the code and the server's name, nothing else; no link, so there is no URL to carry a secret.

## Rate limiting

Three layers, so that no single one can be turned against the owner:

| Layer | Rule | Answer |
| :--- | :--- | :--- |
| Per source address | 10 failed auth attempts (login, totp, verify, setup, refresh) in a minute | `429 rate_limited` with `Retry-After` until the minute ends; a success clears it. Today's rule is 5 for device tokens and stays for them |
| Per account | after 3 failures in 15 minutes, a minimum spacing between attempts of 2^n seconds, capped at 64 s, where n counts the failures past the third | `429 rate_limited`, `Retry-After` the remaining spacing; a success resets n |
| Hash cost | at most 4 argon2id computations at once | later attempts queue |

The per-account layer slows and never locks: an attacker who knows a name can make its owner wait a minute between tries, not lock her out, and he pays the per-source limit meanwhile. Codes have their own counters above. Every limit hit writes `rate_limited` to the audit log with the layer and the account or source.

## Recovery

* **Lost password, 2FA intact.** `POST /api/auth/recover {name_or_email}` always answers `202` with one wording. If the account exists and mail works, a setup code is mailed; otherwise nothing happens and an admin runs `maid server user reset NAME`, which prints a code. `POST /api/auth/recover/complete {code, password, totp_code}` sets the new password and, when TOTP is enrolled, requires a valid code: a mailbox alone does not get past the second factor. Every session of the account is revoked.
* **Lost 2FA device, recovery codes in hand.** A recovery code at the TOTP step logs in and the client is told to re-enrol; `DELETE /api/auth/totp` then `enrol` and `confirm`, with the password re-checked.
* **Lost 2FA device and no codes.** Only an admin: `maid server user reset-2fa NAME` (or the admin route) removes the enrolment, sets `must_enrol`, revokes every session and writes `admin_reset_totp` naming the admin and the user. The next login reaches only the enrolment routes until a new TOTP is confirmed. The admin never sees a secret, a code or a password; the most an admin ever holds is a one-time setup code.

## The admin list

The first account created on an empty store is an admin, because that is the workstation's owner creating her own account at her own terminal. `maid server user promote NAME` and `POST /api/admin/users/{id}/admin {admin: true}` add admins; demotion refuses to remove the last one.

Admins can: list users and their states, invite, approve, reset a password or a TOTP enrolment to a setup code, promote and demote, revoke any session, read the audit log through the API, and see every agent session's metadata (id, workspace, owner, mode, running). Admins cannot, through the API: read another user's transcript, send a message into another user's agent session, or answer another user's approval. Reasoning: an admin at the workstation can read any file there already, so the question is not secrecy from the owner but the size of the API surface; an admin's stolen token should not turn into every conversation on the machine, and an approval is the harness asking the person whose session it is. Admins can interrupt and end any session, and anyone can trip, since tripping only adds restriction.

Per-user session ownership: every agent `Session` gains `owner`. `GET /api/sessions` lists the caller's own; the message, events, approval, interrupt and mode routes answer `403 not_owner` to anyone else, admins included. `POST /api/sessions {resume}` resumes only a session file whose `start` record names the same owner, so a transcript made at the terminal by Micaiah is hers to resume from the phone and no one else's.

## The OIDC hook

Not built now; the hook is shaped so that adding it changes no existing record. Settings: `server.oidc = { name, issuer, client_id, client_secret_file, authorization_endpoint, token_endpoint, jwks_uri, scopes = {"openid", "email"} }`, one provider per server, named generically. The endpoints are configured rather than discovered so the server makes no background fetch; the only outbound requests are the code exchange and the key fetch, each inside a user's login, and this is a second class of outbound request beside mail that Micaiah accepts when she enables the hook.

Flow: `POST /api/auth/oidc/start {device}` returns the authorization URL with `state` (32 random bytes, kept hashed in memory for 5 minutes with the device and source), `nonce` (the same, bound to the state) and a PKCE `code_challenge`. The provider redirects to `GET /api/auth/oidc/callback?code=...&state=...`; the code in that query is the one URL-carried value the protocol imposes, single-use, bound by PKCE, exchanged by `POST` within seconds and never stored. The ID token's `nonce` must match, its `iss`, `aud` and `exp` are checked, and `sub` is looked up among the sealed `oidc` links. A link is made only by a logged-in user through `POST /api/auth/oidc/link` with the password re-checked, so an account is never taken over by a provider identity with a matching email. Signup from a provider follows `server.signup` exactly like a password signup, with the provider's verified email standing in for the mailed code. After a provider login the TOTP step still runs when the account has it enrolled; the provider replaces the password, not the second factor (open question 7). No provider access or refresh token is kept unless a later feature needs one, and then it is sealed like a TOTP secret.

## `maid join SERVER`

```
maid join https://workstation:7373 [--device NAME] [--user NAME]
maid join --status        maid leave [SERVER]
```

1. `GET /api/auth/capabilities` without a token: `{version, login: ["password"], second_factor: ["totp"], signup, oidc: {name} | null, fingerprint, tls}`. The client prints the certificate's fingerprint and asks for a yes the first time, then pins it in the join record; a changed fingerprint later refuses to connect until `maid join --repin` is run on purpose.
2. Username, password without echo, then the TOTP code when challenged (`r` switches to a recovery code). The device name defaults to the hostname. The single `wrong username or password` sentence is printed for step 1 failures.
3. The refresh token goes to the OS keyring when `secret-tool` (libsecret) is on the PATH, called as a program the way bubblewrap is, under the attribute `maid-remote <server url>`; otherwise to `~/.local/state/maid/remotes/<host>.json`, 0600, with the server URL, user, device, session id and pinned fingerprint. That directory joins the harness's secret paths beside `.local/share/keyrings`, so no tool reads it. The access token lives only in the process that refreshed it.
4. `remote` in settings stays what it is, the URL of the server `maid open` prefers; `join` writes it when it is empty and otherwise leaves settings alone, and the credential never goes into a settings file. `remote_up` becomes the capabilities probe, and `maid open` sends the access token where it sends none today. `maid leave` revokes the session on the server and deletes the record.

A client that finds its refresh token revoked or reused prints one line saying so and asks for a fresh `maid join`.

## Migration of the per-device tokens

`tokens.json` is read once at the first start with accounts present. Each entry becomes a session row of `kind: "legacy"` owned by the first admin, `device` the token's name, no refresh, no expiry, accepted by the same SHA-256 lookup as today. Then `tokens.json` is renamed `tokens.json.migrated`. The phone keeps working until it logs in; `maid server session list` shows legacy rows marked, and `maid server session revoke --legacy` removes them all. `maid server token new|list|revoke` go away in the same change, replaced by `maid server user ...` and `maid server session ...`; the legacy acceptance branch is removed in a follow-up once Micaiah's devices have joined, so the folding is complete and no second credential kind remains.

## API surface

JSON in and out, errors `{"error": "...", "code": "..."}` as today with a machine `code` added. `Authorization: Bearer mat_...` on everything not marked public; the existing routes in [remote.md](../remote.md) keep their shapes and gain owner filtering.

| Method and path | Auth | Request | Response and errors |
| :--- | :--- | :--- | :--- |
| `GET /api/auth/capabilities` | public | | `version, login, second_factor, signup, oidc, fingerprint, tls` |
| `POST /api/auth/login` | public | `name, password, device` | `200` tokens, or `200 {second_factor, challenge}`; `401 wrong_credentials`; `403 verify_first`, `pending_approval`; `429` |
| `POST /api/auth/totp` | public | `challenge, code` or `recovery_code` | `200` tokens; `401 wrong_code`, `challenge_expired`; `429` |
| `POST /api/auth/refresh` | public | `refresh_token` | `200 {access_token, expires_in, refresh_token}`; `401 invalid_token`, `session_revoked` |
| `POST /api/auth/logout` | user | | `204` |
| `GET /api/auth/me` | user | | `id, name, email, admin, totp, recovery_left, must_enrol, session {id, device}` |
| `POST /api/auth/password` | user | `password, new_password, code?` | `204`; `401 wrong_credentials`; `400 weak_password` |
| `GET /api/auth/sessions` | user | | `{sessions: [{id, device, created, last_seen, source, current}]}` |
| `DELETE /api/auth/sessions/{id}` | user | `password` | `204`; `404` |
| `POST /api/auth/totp/enrol` | user | `password` | `200 {uri, secret}`; `409 already_enrolled` |
| `POST /api/auth/totp/confirm` | user | `code` | `200 {recovery_codes: [...]}`; `401 wrong_code` |
| `DELETE /api/auth/totp` | user | `password, code` | `204`; `403 required` when the server enforces 2FA |
| `POST /api/auth/recovery-codes` | user | `password, code` | `200 {recovery_codes}` |
| `POST /api/auth/signup` | public | `name, email, password` | `201 pending_verify` or `202 pending_approval`; `409 name_taken`; `403 signup_closed`; `400 weak_password` |
| `POST /api/auth/verify` | public | `name, code` | `204`; `401 wrong_code`, `code_expired` |
| `POST /api/auth/verify/resend` | public | `name` | `202`; `429` |
| `POST /api/auth/setup` | public | `code, password` | `204`; `401 wrong_code` |
| `POST /api/auth/recover` | public | `name_or_email` | `202` always |
| `POST /api/auth/recover/complete` | public | `code, password, totp_code?` | `204`; `401 wrong_code`, `second_factor_required` |
| `POST /api/auth/oidc/start`, `GET .../callback`, `POST .../link` | public, public, user | later | later |
| `GET /api/admin/users` | admin | | `{users: [{id, name, email, state, admin, totp, created, sessions}]}` |
| `POST /api/admin/users` | admin | `name, email` | `201 {id, setup_code?}` (the code only when no mail is configured) |
| `POST /api/admin/users/{id}/approve` | admin | | `204` |
| `POST /api/admin/users/{id}/reset` | admin | `what: "password" or "totp"` | `200 {setup_code?}`; sessions revoked |
| `POST /api/admin/users/{id}/admin` | admin | `admin` | `204`; `409 last_admin` |
| `POST /api/admin/users/{id}/disable` | admin | `disabled` | `204` |
| `DELETE /api/admin/users/{id}/sessions` | admin | | `204` |
| `GET /api/admin/audit?after=N&limit=M` | admin | | `{rows: [...]}` |
| `GET /api/sessions` and the session routes | user | as today | own sessions only; `403 not_owner` otherwise; the admin's metadata view of a session omits its `transcript` path |
| `POST /api/trip` | user | as today | as today; the audit row names the user |

Common errors: `400` bad body, `401 unauthorized` with `WWW-Authenticate: Bearer` for a missing or expired access token, `403 admin_only`, `403 enrol_first` for a must-enrol session touching anything but the enrolment routes, `429 rate_limited` with `Retry-After`.

## The audit log

The line keeps today's prefix so existing readers still parse it, and gains an event: `time source user method path status event k=v ...`. `user` is the account name for an authenticated request, `-` for none, and for a failed login `?` followed by `name_sha=<first 8 hex of SHA-256 of the name typed>`, so attempts against one unknown name correlate without the typed string being logged (people type passwords into the name box). No password, code, token, challenge or secret is ever written; a token appears only as `token=mat_<first 6>...` when a row is about one.

Events: `login_ok`, `login_fail`, `totp_ok`, `totp_fail`, `recovery_code_used`, `challenge_expired`, `refresh`, `refresh_reuse`, `logout`, `session_revoked`, `signup`, `verify_ok`, `verify_fail`, `code_sent`, `setup_ok`, `recover_requested`, `recover_ok`, `password_changed`, `totp_enrolled`, `totp_removed`, `recovery_codes_made`, `invite`, `approve`, `disable`, `promote`, `demote`, `admin_reset_password`, `admin_reset_totp`, `key_rotated`, `legacy_migrated`, `rate_limited`, `not_owner`, `trip`. Admin events carry `target=<user>`; `rate_limited` carries `layer=source|account|code`; `refresh_reuse` carries `session=<id> device=<name>`. The file is 0600 and append-only by habit, not by enforcement: the threat model says what a shell as the user can do to it.

## What the web client changes

The one-file client stays one file and loads nothing from anywhere else; it gains a `Content-Security-Policy` meta tag of `default-src 'self'`. The token screen becomes a login screen: name, password, then a code field when the server answers with a challenge, a "use a recovery code" link, and the single failure sentence. The access token lives in memory and the refresh token in `localStorage` under `maid.refresh`, the same risk class as today's token (open question 5); a `401` on any call triggers one refresh and a retry, and a failed refresh returns to the login screen with the server's reason. New cards under the sessions screen: "You" (name, admin, 2FA state, recovery codes left, change password, log out), "Your devices" (sessions with device, source and last seen, revoke each), "Two-factor" (enrol: show the secret and URI for manual entry first, with a local QR encoder as a follow-up since it must not come from a CDN; confirm; recovery codes shown once with a copy button). A `must_enrol` session opens on the two-factor card and nowhere else. Signup and verification screens appear only when capabilities say signup is not closed. An admin sees a "Users" card: list, approve, invite, reset, promote. The device-token box stays until the migration PR removes it.

## Testing

Unit tests, in `core` or a new `server/tests/auth_test.cpp` against the primitives alone:

* argon2id: a fixed PHC string produced once at the chosen parameters verifies its password and refuses one byte changed; `needs_rehash` is true for a string at lower parameters and false at the current ones; the semaphore admits four and queues the fifth.
* TOTP: the RFC 6238 Appendix B vectors for SHA-1 (seed `12345678901234567890`: time 59 gives 94287082, 1111111109 gives 07081804, 1234567890 gives 89005924, 2000000000 gives 69279037, 20000000000 gives 65353130, truncated to 6 digits for the 6-digit mode); the drift window accepts one step each side and refuses two; a replayed step is refused; base32 round trips and refuses bad input.
* Sealing: round trip; one flipped ciphertext byte fails; a value re-labelled to another user or field fails on the associated data; after rotation every field opens under the new key and the old id is gone.
* Tokens: prefixes route to the right table, wrong prefix at a route is refused before lookup, length and alphabet checks, every hash compare goes through one helper over `sodium_memcmp`, and a test checks that two hashes differing only in the last byte are refused.
* Rate limit: the per-source window, the per-account spacing sequence 2, 4, 8 ... 64, reset on success, and that an account under backoff still admits its owner after the spacing.

The integration test extends `server_test.cpp` against the existing `FakeServer`: first user is admin; invite, setup, login; enrol and confirm TOTP, logout, login with the challenge, a wrong code then a right one, replay refused; refresh rotation, a retry inside the grace succeeds, a reuse after it revokes the session and writes `refresh_reuse`; recovery code login and the must-enrol fence; recover with the password and TOTP still required; admin reset of TOTP revoking sessions and leaving its row; two users where the second gets `403 not_owner` on the first's session and `GET /api/sessions` shows only its own; `signup = email` with no mail command degrading to approve; the unknown-name and wrong-password answers byte-identical and both taking at least the hash's time; the per-source 429 and the per-account `Retry-After`; a legacy token row accepted and listed as legacy; every event above present in `audit.log` with no secret in any line (the test greps the log for the plaintext password, codes and tokens it used).

Fuzzing joins roadmap item 10's sanitizer run: libFuzzer targets for the `Authorization` header and token parser, the PHC string parser, the base32 decoder and the sealed-value JSON reader, each a few lines over the primitive, run under ASan and UBSan.

## Build order

Each step is one PR a reviewer can read in a sitting, builds on the one before, and leaves the server working.

1. libsodium in `vcpkg.json` and `server/CMakeLists.txt`; `server/src/crypto.{hpp,cpp}` with sealing, argon2id, random tokens, base32 and the constant-time compare; unit tests with the known-answer vectors; the cleanroom row.
2. `master.json` and `accounts.json`: the store, key rotation, `maid server user new|list|promote|demote|disable`, first user is admin. No routes yet.
3. Login and sessions: `sessions.json`, access and refresh tokens, `login|refresh|logout|me|sessions`, the pre-routing handler accepting access tokens beside device tokens, `owner` on agent sessions with the owner filter, audit events. The dummy hash and identical wording land here because they are the shape of the login handler, not an add-on.
4. TOTP: enrol, confirm, remove, the challenge step, recovery codes, `require_2fa` and the must-enrol fence.
5. Rate limiting per account and the hash semaphore; the per-source rule extended to the auth routes.
6. Signup, codes, the mail command, `signup` modes with the degrade, setup codes, recovery routes, `maid server user invite|approve|reset|reset-2fa`.
7. Admin routes and the audit read route.
8. Web client: login, refresh, the three cards, enrolment, signup when open, admin card.
9. `maid join`, `maid leave`, keyring or file storage, the pin, `remote_up` as the capabilities probe, `maid open` with the access token.
10. Migration of `tokens.json`, removal of `maid server token`, `maid server session list|revoke`; docs: [remote.md](../remote.md), [settings.md](../settings.md), [harness.md](../harness.md) protected paths.
11. Fuzz targets under the sanitizer preset.
12. The legacy acceptance branch removed once her devices have joined.
13. OIDC, when she has a provider to point at.

## Open questions for Micaiah

1. **What may a second account do on your workstation?** Every remote action is asked about, but asked of that account's owner, on your machine. Recommendation: non-admin accounts get `max_mode = plan` and an empty `workspaces` list until an admin grants per user (the two nullable fields in the user record); admins get the server's `workspaces` as today.
2. **Master key protection.** A 0600 file in the state directory, or an optional passphrase typed at `maid server start` that wraps it (argon2id as the KDF)? Recommendation: the file now, since a passphrase means the server cannot start unattended and the leaked-file case is what the key is for; add the passphrase mode if the server ever runs on a laptop that leaves the house.
3. **Default `signup`.** Recommendation: `closed`; a personal server has an admin who invites.
4. **Mail transport.** The `sendmail`-compatible command, or a built-in SMTP client with STARTTLS? Recommendation: the command; it keeps the server free of SMTP code and the only network step inside the relay you configure. Add the client only if a relay you want cannot offer a sendmail binary.
5. **Refresh token in the browser.** `localStorage`, the same class as today's token, or an `HttpOnly; SameSite=Strict` cookie scoped to `/api/auth/refresh`? Recommendation: `localStorage` with the CSP for this round, so the web, the terminal and a native client share one body-only contract; revisit when item 3's larger web app lands.
6. **Lifetimes.** 15 minutes access, 30 days sliding refresh, 90 days absolute. Recommendation: these; the phone refreshes without noticing and a forgotten device dies inside a season.
7. **Does a provider's own MFA count as the second factor under OIDC?** Recommendation: no; MAID's TOTP stays required when enrolled, since the server cannot verify what the provider did.
8. **Legacy device tokens.** Remove the acceptance branch after your devices join (step 12), or keep a non-expiring "device" session kind for headless clients? Recommendation: remove; a headless client can hold a refresh token in a 0600 file like `maid join` does.
9. **Admins and other users' transcripts.** Designed as not readable through the API. Recommendation: keep it; the files are yours at the terminal anyway and the API surface stays small.
10. **Username rules.** Recommendation: 3 to 32 characters, lowercase letters, digits, `-`, `_` and `.`, matched case-insensitively, display name free-form later if wanted.
11. **Passkeys.** WebAuthn needs a stable relying-party id, which a self-signed certificate for a LAN address does not give. Recommendation: defer until the server has a name (a LAN hostname with a certificate from a LAN CA, or the relay's name) and design the credential record then; nothing here blocks it.
12. **Where the web client's QR code comes from.** A small local encoder in the one file (a few hundred lines) or manual entry of the secret only? Recommendation: manual entry first (step 8), the encoder as its own small PR, never a CDN.
