# Alienation lobby server

Players meet here: accounts, friend codes, friends, presence, rooms and the exchange of UDP endpoints. Game traffic goes
directly between players; only a pair that cannot reach each other uses the lobby's relay (`src/relay.js`, binary
WebSocket messages). The protocol and the game side are in [docs/ONLINE.md](../../docs/ONLINE.md).

This is a separate service from Dragon's Crown Pro PC's lobby: its own Worker (`alienation-lobby`), Durable Object
namespace, rate-limit namespace, dashboard token, database and game key. It refuses any client whose `hello` names
another game (`wrong_game`).

`src/lobby.js` is the whole lobby. It runs in two hosts:

- `src/worker.js`: a Cloudflare Worker with one SQLite-backed Durable Object (WebSocket Hibernation, `ping`/`pong`
  answered without waking the object, the seat grace as a Durable Object alarm).
- `local/server.js`: the development server on Node.js 22.13 or later (`node:http` + `node:sqlite`), no account needed.

No dependencies are installed for either the tests or the local server.

## What is specific to Alienation

- **Builds never mix.** A client's `hello` carries `build` (`CUSA00062-1.00`, `CUSA00062-1.06`, `CUSA04430-1.00`). The game runs a
  deterministic lockstep simulation, so peers must run the same executable: rooms remember their creator's build, searches
  leave other builds out (counted in `incompatible`), a join is refused with `incompatible` and the room's `build`, and the
  relay never routes between two builds. `room.create`, `room.search`, `room.join`, `room.external` and `room.members` need
  a build; a launcher session without an installed game has none.
- **Online ids are unique in a room.** The game finds its own player among the members by online id, so a join whose name
  matches a member (any case) is refused with `name_taken`.
- **Reads the classic Matching2 API needs:** `room.external` (GetRoomDataExternalList) and `room.members`
  (GetRoomMemberDataExternalList), for the client's own room or a room a search could find; never a key, code or address.
- **`rooms.list`**: the launcher's Public Lobby, the public rooms of the reader's build (host, players, slots, joinable,
  host version, minutes open), at most 50, `rev` for an unchanged list.
- **No party statistics** (Dragon's character classes): `party.report` is an unknown request here.

## Production

Players, the launcher and the admin use the deployed Worker, whose address is `aln::net::kDefaultLobbyUrl`
(`src/net/online_config.h`): `wss://alienation-lobby.saulobenigno.workers.dev`. The launcher's Bug Report page files
reports on the same service (`incident.file`); `/admin` shows them in the Bugs tab.

## Developer and testing only

```
node local/server.js                      # ws://127.0.0.1:8790/, data in memory
node local/server.js --db lobby.sqlite    # keeps accounts, friend codes and friends (rooms are removed at start)
node --test "test/*.test.js"              # protocol, rooms, builds, friends, relay, limits, dashboard, bug reports
```

`tools\online-lobby.ps1` wraps the `--db` form with `user\online-test-lobby\lobby.sqlite` (`-Reset`, `-SeedTestFriends`,
`-Memory`, `-Lan`). `tools\test.ps1` runs the tests above and the game-side HLE test against a loopback lobby it starts
and stops itself.

## Cloudflare deployment (manual)

Nothing in the repository logs in, stores a token or deploys by itself.

1. Install Wrangler (`npm install -g wrangler`) and run `wrangler login` with your Cloudflare account.
2. In this folder, check `wrangler.toml`: name `alienation-lobby`, the `LOBBY` Durable Object with its SQLite migration,
   and the `WS_UPGRADE_LIMITER` rate limit (`namespace_id` 2701, which must be unique in the account).
3. For `/admin`: `wrangler secret put ALN_DASHBOARD_TOKEN` (a long random value, at least 16 characters; without it the
   admin API answers 503).
4. From the repository root: `.\tools\worker.ps1 -DryRun` bundles without uploading; `.\tools\worker.ps1` deploys. It uses
   the Wrangler already installed (`npx --no-install` never downloads one).
5. Check it: `curl https://alienation-lobby.<your subdomain>.workers.dev/` answers
   `{"service":"alienation-lobby","proto":1,"backend":"1.0.0"}`, and the launcher's Connection test says Lobby: Ready.
6. If the subdomain differs from the one in `kDefaultLobbyUrl`, change that constant (players must not need `ALN_LOBBY`).

The Worker uses `CF-Connecting-IP` as each player's address and `request.cf.country` for the country statistics.

## Abuse and quota protection

Every protection is invisible to a client that behaves; limits live in memory (`src/abuse.js`) and cost no write.

- **Edge** (`src/edge.js`): 30 WebSocket upgrades per address group per 60 s (IPv6 per /64) before the Durable Object.
- **Connections**: 16 sockets and 20 new accounts per day per address group; frames of 32 KB (160 KB for a bug report).
- **Sign-in**: per account 12 then 1 per 15 s, counted after the secret matched (nobody can lock another account out);
  an unknown lobby protocol (`proto`) or game (`wrong_game`) is refused and counted once per address per 10 minutes.
- **Requests**: 60 burst then 20/s per socket; room operations 30 then 1/s; `room.create` 6 then 1 per 20 s per account
  (20 then 1 per 6 s per address); `room.join` 20 then 1 per 5 s per account; `rooms.list` 6 then 1 per 5 s per account
  (60 then 3/s per address).
- **Writes**: unchanged `udp`, `room.set_internal` and `room.set_external` write nothing. A change spends a budget of the
  socket, the account and the address (room data and endpoints 40 then 1/s; friend changes 20 then 1 per 3 s).
- **Public endpoints**: each new advertised IPv4 of an IPv6 client costs a token of its address (8 then 1 per 30 s).
- **Signaling hygiene**: LAN candidates reach only members behind the same observed address; only private, loopback and
  link-local IPv4 candidates are accepted; a client on IPv4 cannot advertise another address; endpoints go only to
  members of the same room.
- **Relay**: per connection 400 frames/s and 128 KB/s, per room twice that, 1200-byte payloads, a 16-byte header whose
  room token only members know; the lobby writes the sender's member itself, so a sender cannot impersonate another; 200
  invalid frames then 5/s close the connection (1008).
- **Refusals**: more than 30 refused or malformed requests on one socket, refilled 1 per 2 s, close it (1008).

## Usage statistics and the dashboard

`src/metrics.js` keeps anonymous aggregate counts in the Durable Object: sessions (per platform, version and country),
rooms created, joins, peaks, closed rooms with their largest occupancy and lifetime, rooms that had a guest and rooms the
relay carried. Counted in memory and written with the 15-minute diagnostics (never one write per event): refused joins by
reason (`join_failed`), relay frames and payload bytes (`relay`), room leaves, friend requests and new friendships
(`activity`), and the abuse diagnostics (`abuse`). No row names a player, an account, an address or a room.

`/admin` (Bearer `ALN_DASHBOARD_TOKEN`) has the Dashboard (Lobby health, Connection health, Room lifecycle, Relay usage,
Friends, Abuse protection, history with period comparisons) and Bugs tabs. The live summary also shows the backend
version, the lobby protocol, and the connected players per game build and online protocol.

```
curl -H "Authorization: Bearer <token>" https://alienation-lobby.<subdomain>.workers.dev/admin/api/summary
curl -H "Authorization: Bearer <token>" "https://alienation-lobby.<subdomain>.workers.dev/admin/api/history?days=90"
```
