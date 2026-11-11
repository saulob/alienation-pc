# Online play

Alienation's own multiplayer code runs unchanged: the port answers the console services it calls (NpManager, NetCtl,
classic NpMatching2 and the DGRAM_P2P socket of libSceNet). Players meet through the Alienation lobby (`server/lobby`);
game traffic goes directly between their PCs over UDP, and through the lobby only for a pair of players who cannot reach
each other. Nothing in the game executable is patched for online play, and with Online play off every import keeps its
offline behaviour (`src/runtime_services.c`).

The design follows Dragon's Crown Pro PC's production online stack (lobby, relay, peer transport, launcher page,
dashboard) as a separate service: its own Worker, namespaces, token, database and game key. Dragon's lobby is never
contacted.

Status: implemented and tested headlessly (backend protocol tests, the game's entry points against a loopback lobby).
Two real game instances entering one native session has not been verified yet; see "Testing".

## Playing online

1. In the launcher's **Online** page, turn **Online play** On. The rest of the page appears only then. No environment
   variable is needed: the game reads the same setting (`online_enabled` in `alienation.ini`).
2. Choose your **Online name** (up to 16 letters, digits, `-` or `_`). It is your in-game online id. Left at `Player`, the
   id becomes `Player-` and six hex digits of your account, so two profiles never share one.
3. Share **Your friend code** (`XXXX-XXXX`, **Copy**). On the **Friends** tab, enter a friend's code and **Send request**;
   requests appear at the top of the page with **Accept** and **Decline**; **Remove** ends a friendship. A friend shows
   Online, Offline, "Online, in a room" or "Online, in a joinable room" (public, a free seat, your game build), and
   **Incompatible** when their room is of another game build or online version.
4. **Connection test** > **Run test** (PLAY waits for one finished test while Online play is on). It reports:

   | Row | Ready | Limited | Not ready |
   |---|---|---|---|
   | Lobby | signed in, with the round trip | | service or identity unavailable |
   | Public | a STUN server saw this PC's public UDP endpoint | symmetric NAT, or STUN off | the UDP port cannot open, or no STUN answer |
   | Direct | other players can probably connect directly (or UPnP opened the port) | some players may not | no UDP port |
   | Router | UPnP mapped the port | UPnP off, unavailable or behind another router (not required) | the port is not open |
   | Relay | the service offers the relay and the port opened | | not offered, or no service |

   Every row comes from a measurement of this test (sign-in, two STUN servers from the game's UDP port, a UPnP probe,
   the service's relay capability); nothing is assumed from settings. Each real peer path is verified when joining.
5. The **Lobby** tab lists **Public rooms** of your game build (host, players, joinable, host version, age); join them from
   the game's own Online menu. **Show me in Lobby** additionally lists your Online name so others can send a friend
   request.
6. Press **Play**, then use the game's Online menu.

Only the same game build plays together: CUSA00062 1.00 (EUR), CUSA00062 1.06 (EUR, updated) and CUSA04430 1.00
(ASIA/KR) never meet each other, because the game's lockstep simulation needs the same executable on every PC. The DUPLEX
repack runs as CUSA00062 1.06 (same executable and game data) and meets official 1.06 players.

## Bug reports

The launcher's Bug Report page sends a report only when the player presses Send report (`incident.file` on a short lobby
session with `presence: false`, so Online play does not have to be on). The Worker stores it as received and answers a
`ticket` (`ALN-XXXXXX`) and a **receipt**: 256 random bits, a capability for that one report. The launcher keeps the
receipt in its own report history file, never in `alienation.ini` or a log; the Worker stores only a salted SHA-256 of it.

`incident.status` and `incident.read` take no `hello` and accept only a ticket with its receipt, so a lookup never involves
the account. My reports shows the status the developer set in `/admin` (New, Investigating, Fixed, Cannot reproduce,
Duplicate, Not a bug, Closed). An account may file 2 accepted reports per rolling hour and 10 per 24 hours; the Worker
alone decides.

## Settings

| Key (`alienation.ini`) | Variable | Default | Meaning |
|---|---|---|---|
| `online_enabled` | `ALN_ONLINE` | 0 | Online play on |
| `online_name` | `ALN_ONLINE_NAME` | `Player` | Online name (in-game id rules above) |
| `online_udp_port` | `ALN_UDP_PORT` | 0 | 0: the game's port 3658, or any free one when it is taken |
| `online_stun` | `ALN_STUN` | `stun.cloudflare.com:3478,stun.l.google.com:19302` | STUN servers; `off` disables |
| `online_connection_test_port` | - | 0 | UDP port of the last finished Connection test |
| `show_in_lobby` | - | 0 | list the Online name in the public Lobby |
| - | `ALN_LOBBY` | `wss://alienation-lobby.saulobenigno.workers.dev` | developer override of the production lobby |
| - | `ALN_UPNP` | 1 | `0` leaves the router alone |
| - | `ALN_JOIN_CODE` | - | the game's room search returns only the room with this code (diagnostics) |
| - | `ALN_GAME_BUILD` | - | pins a test instance to one prepared build |

Environment variables are debug and test overrides only. The lobby address is `aln::net::kDefaultLobbyUrl`
(`src/net/online_config.h`), the only place it is written.

## Profiles and saves

A profile is a user folder: settings (`alienation.ini`), saves, trophies and the online identity. The default profile is
`user\`; `--user <dir> --logs <dir>` selects another one and the launcher passes both to the game process.
`tools\online-instance.ps1 -Name a` uses `user\test-client-a` and `build\logs\test-client-a`, copying the settings and
saves of `user\` on first start; two instances never share a save, identity or log folder.

The game keeps one save, `savedata` with `savedata.dat` (20-byte header `AYE!`, checksum, sizes, version; a Stated-
serialized payload), under `<profile>/savedata/1/<title id>/`. Writable mounts are committed at unmount, as on the
console: the guest works on `savedata.aln-work`, the unmount flushes it, writes `savedata.aln-commit` and renames it over
`savedata`. At the next mount a work copy with the commit mark is swapped in, one without it (the game stopped before
unmounting) is dropped. `tests/savedata_tests.c` covers it.

Multiplayer and saves: every PC saves only its own player's progression, loot and mission completion, through its own
profile, exactly as on the console; no save data crosses the network. A disconnect, a drop-out or leaving a room changes
nothing in this path: a save interrupted by a crash or a lost connection is either the previous committed save or the
next, never a mix. The online layer never touches the save payload.

## Online identity

`online_identity.ini` in the profile folder holds a random 63-bit account id and a 32-byte secret (`src/net/identity.cpp`,
shared with the launcher), made on first use and never overwritten. The lobby stores only the secret's SHA-256 and
refuses a known account with another secret; the secret is never logged. No Sony account, e-mail or password exists.

The SceNpId the game signs in with (`src/runtime_np.c`) carries the online id and the account in its opt bytes. The
lobby refuses to seat two members with the same online id in one room (`name_taken`): the game finds its own player
among the members with `sceNpCmpOnlineId`.

## How the game goes online

1. **Network and sign-in** (`src/runtime_np.c`): NetCtl state 3 (IP obtained), NAT type 2 for every player (so any two
   can meet; the relay covers what NAT would stop), the NP state callback "signed in" delivered from
   `sceNpCheckCallback`, then the account checks (`CheckNpAvailability`, `GetParentalControlInfo`, `CheckPlus`) answered
   for the profile's own online id.
2. **"UNX Global Matrix"** is the game's name for its Matching2 service. The message "Cannot connect to UNX Global
   Matrix" is the Stated text `menu__network__error_general`; "joined unx global matrix" is shown when the native
   `network_matching_is_ready` (EU 0x27b7b0 -> 0x1506130) is true: the Matching2 context has started (context event
   0x6f02) and the login chain finished, a `GetServerId` step (0x2e0890) then a `GetWorldInfoList` step (0x2e09a0) whose
   answer must list at least one world with a non-zero id. Offline, every Matching2 import answered "signed out", so the
   chain failed at once. Now `ContextStart` opens the lobby link and 0x6f02 follows the lobby's hello answer;
   `GetWorldInfoList` asks the lobby (`worlds`) and answers world 1 with its room and member counts. With the lobby
   unreachable the context starts over (0x6f01, error 0x80550d02); the game retries three times and then shows the
   message, which is then true.
3. **Rooms** (classic Matching2 over the lobby, `src/runtime_matching2.cpp`): `CreateJoinRoom` (hidden at first, flags
   0x14000000), `SetRoomDataInternal` to make it public (0x04000000) or keep it hidden (invitations), `SetRoomDataExternal`
   for the 0x70-byte room description (attr 0x55), `SearchRoom` with the game's flag filter and int filters on attrs
   0x4d..0x53, `JoinRoom` checking the compatibility byte (internal attr 0x57), `LeaveRoom`,
   `GetRoomDataExternalList`, `GetRoomMemberDataExternalList`, `SignalingGetPingInfo`.
4. **Peers**: the game polls `SignalingGetConnectionStatus` for each member until it is active, then sends to the address
   and port it got; it attributes every received datagram by an exact match of that address and port.

Matching2 callbacks run only from `sceNpCheckCallback` (every frame, EU 0x1439ae0), never inside a Matching2 call and
never with a port lock held: the game marks a request pending only after the call returns and takes its own locks in the
callbacks.

### Classic Matching2 structures

Offsets in bytes, EU addresses of the guest code that writes or reads them.

| Call (event) | Request | Response or event data |
|---|---|---|
| CreateContext (0x2dff10) | {own SceNpId* @0, comm id @8, passphrase @0x10, service label @0x18} | ctx id 1 |
| ContextStart (0x2dff3a) | (ctx, timeout) | context callback 0x6f02 / 0x6f01 / 0x6f03 |
| GetServerId (0x1510684) | (ctx, u16*) synchronous | server 1 |
| GetWorldInfoList (0x1510a1b, 0x0002) | {server u16} | {World* @0, u64 count @8}; World 0x28: id @8, rooms @0x18, members @0x1c |
| CreateJoinRoom (0x150b34b, 0x0101) | max slots u16 @0, flags @8, world @0xc, internal bin attrs @0x60/@0x68, searchable ints @0x70/@0x78, external bin attrs @0x90/@0x98, member bin attrs @0xa0/@0xa8 | {RoomDataInternal* @0, members @8, count @0x10, me @0x18, owner @0x20} |
| JoinRoom (0x150da16, 0x0102) | room @0, member bin attrs @0x18/@0x20 | as CreateJoinRoom |
| LeaveRoom (0x150e26b, 0x0103) | room @0 | {room @0} |
| SearchRoom (0x150f334, 0x0106) | option u8 @0 (0x10 random, 0x08 NAT filter), world @4, start @0x10, max @0x14, flag filter @0x18, flag attr @0x1c, int filters @0x20/@0x28 (op u8 @0, attr u16 @8, value u32 @0xc) | {start @0, total @4, count @8, RoomDataExternal* @0x10} |
| SetRoomDataInternal (0x150c14e, 0x0109) | room @0, flag filter @8, flag attr @0xc, bin attrs @0x10/@0x18 | - |
| SetRoomDataExternal (0x150c295, 0x0004) | room @0, ints @8/@0x10, bin attrs @0x28/@0x30 | - (unchanged repeats answered locally) |
| GetRoomDataExternalList (0x150c614, 0x0005) | room ids @0/@8 | {RoomDataExternal* @0, count @8} |
| GetRoomMemberDataExternalList (0x150cae6, 0x0003) | room @0 | {list @0, count @8}; entry: next @0, SceNpId @0x10, role u8 @0x34 (2 owner) |
| SignalingGetPingInfo (0x150c9c3, 0x0e01) | room @0 | {server @0, world @4, room @8, rtt us @0x10} (worst peer link) |
| SignalingGetConnectionStatus (0x1501b62) | (ctx, room, member, status*, addr*, port*) | status 1 pending / 2 active; address and port in network order |

- RoomDataInternal (0x80): max slots u16 @8, server u16 @0xa, world @0xc, room @0x18, flags @0x40, bin attrs @0x48/@0x50
  (element 0x28: update date @0, member @8, id @0x10, data @0x18, size @0x20).
- RoomMemberDataInternal (0x58): next @0, join date @8, SceNpId @0x10, member id u16 @0x38, bin attrs @0x48/@0x50 (element
  0x20: update date @0, id @8, data @0x10, size @0x18).
- RoomDataExternal (0xa0): next @0, max slots @8, members u16 @0xa, server @0x10, world @0x14, room @0x20, owner SceNpId*
  @0x40, ints @0x58/@0x60 (attrs 0x4d..0x53 in order), bin attrs @0x78/@0x80.
- Room events: 0x1101 member joined and 0x1102 member left {RoomMemberDataInternal* @0}; 0x1103 kicked and 0x1104
  destroyed {cause u8 @0}. Signaling: 0x5102 established when the member's P2P link connects, 0x5101 dead when it leaves.
- Errors the game tells apart: 0x80550d13 no such room (also another build), 0x80550d15 room gone, 0x80550d19 full,
  0x80550d02 service unavailable, 0x80550d03 busy (rate limited); 0x80550cxx are the port's own.
- Member ids are opaque to the game: 0x10, 0x20, 0x30, 0x40. Rooms have at most 4 seats.

## Peer-to-peer socket

The game opens one `sceNetSocket("udpp2p_socket", AF_INET, SCE_NET_SOCK_DGRAM_P2P)` (EU 0x1504b3c), sets SO_NBIO and two
PS4 crypto options (accepted and ignored), binds port 3658 with its vport (1872), and on its network thread polls a new
epoll per pass, sends only while the socket is writable (`sendto` with the member's address and its own vport, up to
6144 bytes) and reads with `recvfrom` until EAGAIN (0x80410123). Shutdown is abort, shutdown, close.

`src/runtime_p2p.cpp` maps that socket onto one host UDP socket (`net::PeerSocket`):

- Every room member has one room-wide virtual endpoint, `127.65.76.78` with the member id as the port, the same on every
  PC of the room. `SignalingGetConnectionStatus` reports it, `sendto` routes it to that member's link and `recvfrom`
  reports it as the source, so the game's exact address match holds whatever the real path is (LAN, public, UPnP,
  relay, or a NAT that changes ports). A real address never reaches the game.
- Each datagram is one PeerSocket message, unchanged: the game's packets are never parsed or translated. Links are
  authenticated (HMAC of the room key), ordered and retransmitted; a datagram to a member whose link is not connected yet
  is dropped as UDP would drop it, and the game resends.
- The socket is always writable while bound (EPOLLOUT); readable when any member's link holds a datagram; hang-up after
  abort or close. Other socket types (debug consoles, HTTP) stay unreachable, as offline.
- A second instance on one PC gets a free UDP port when 3658 is taken; the lobby tells peers the real port.

## Signaling, NAT traversal and the relay

The game's P2P layer sits on Dragon's Crown Pro PC's proven peer transport (`src/net/peer_socket.cpp`, same revision):

- **Endpoint report** (`udp`): once the socket is bound, STUN (RFC 5389 from the game's own socket) and UPnP get up to
  5 s; then the client reports the endpoint peers should dial: the local port when the lobby is on the LAN, else the UPnP
  mapping when the router's WAN address is public, else the STUN port, else the local port. A client the lobby sees over
  IPv6 also reports the public IPv4 it found (`public_ip`); IPv6 is never turned into `0.0.0.0`.
- **Candidates**: the public endpoint, plus up to four LAN candidates (loopback and private IPv4, local port). The lobby
  validates them (private, loopback or link-local IPv4 only) and passes LAN candidates only to members behind the same
  observed address; endpoints go only to members of the same room. Duplicates and unchanged reports are dropped.
- **Linking**: a member's link starts once its endpoint is known and stable for 0.5 s and ours reached the lobby (or 8 s
  passed); both ends punch and send HELLOs to every candidate (100 ms, then 250 ms, for 15 s). The member becomes
  "active" for the game only when its link is connected.
- **UPnP**: SSDP discovery, AddPortMapping (UDP, 1 h lease renewed, next ports on conflict), removed when the socket
  closes; tried automatically, never required. `ALN_UPNP=0` disables it.
- **Relay**: direct is tried alone for 2.5 s, then the lobby's relay is offered beside it; the first authenticated path
  wins. A direct link that turns critical later moves to the relay without the game noticing. Relay frames carry the
  same datagrams byte for byte ("DR", version, type, room token, member, sequence, length; payload <= 1200 bytes); the
  lobby forwards only between live members of the same room and build that asked for it, writes the sender's member
  itself, and enforces per-connection and per-room budgets.

## Lobby protocol (version 1)

One WebSocket per process, JSON text frames (`{"op", "seq", ...}` answered `{"re", "code", ...}`, events `{"ev", ...}`),
binary frames for the relay, `ping`/`pong` every 20 s. The full Dragon protocol applies (`server/lobby/src/lobby.js`),
with these differences:

| op | Alienation |
|---|---|
| `hello` | `game` must be `Alienation` (else `wrong_game`); `build` (`CUSA00062-1.00`) required for rooms; the answer adds `backend` |
| `room.create` / `room.search` / `room.join` | need a build; other builds are left out (`incompatible` count) or refused (`incompatible` with `build`); a duplicate online id is refused (`name_taken`) |
| `room.external` | `rooms` (1-20 ids): RoomDataExternal of the own room or findable rooms |
| `room.members` | `room`: `members [{id, name, owner}]` |
| `rooms.list` | the launcher's public rooms of its build: `rooms [{host, players, slots, joinable, version, minutes}]`, `total`, `incompatible`, `rev` |
| `friends.list` | rows add `build` and `joinable` |
| `party.report` | not supported |

## Testing

Headless (no game, no window):

```
cd server\lobby; node --test "test/*.test.js"     # protocol, builds, rooms, friends, relay, limits, dashboard
tools\test.ps1                                     # also starts a loopback lobby and runs aln-online-hle-tests
```

`aln-online-hle-tests` calls the classic Matching2 and libSceNet entry points as the game does while a second player
joins through the lobby and connects over UDP: the unreachable-service path (0x6f01), context start, world list, room
create (hidden, then public), the member join event with its SceNpId and attribute 0x59, endpoint exchange, signaling
pending then active at the virtual endpoint, epoll writability, datagrams both ways (6000 bytes, exact source match,
EAGAIN when empty), member and room external reads, a KR room invisible and refused from the EU build, member leave
(0x1102, 0x5101), leave, search, socket abort/shutdown/close and context stop.

Two instances on one PC (each its own profile, saves, identity, logs and UDP port):

```
tools\online-instance.ps1 -Name a -OnlineName Alpha          # terminal 1
tools\online-instance.ps1 -Name b -OnlineName Bravo          # terminal 2
tools\online-instance.ps1 -Name a -Build CUSA00062-1.00      # pinned to one prepared build
tools\online-instance.ps1 -Name a -Lobby ws://127.0.0.1:8790/  # with tools\online-lobby.ps1 in another window
```

What to look for in each instance's log: `Online: on as ...`, `Matching2: signed in to the lobby`, `Matching2: created
room N` / `joined room N`, `Net: P2P socket ... bound`, `Matching2: endpoint reported`, `Matching2: member 0x20 linking`,
`P2P: link N ... connected via`, `Matching2: member 0x20 established`, and at the end the `P2P: member 0x20 ...` line with
datagram counts.

Two instances on one PC connect through their LAN candidates; NAT traversal needs two PCs on different Internet
connections (see the manual A/B test in the delivery notes).

## Deploying the lobby

See [server/lobby/README.md](../server/lobby/README.md): `wrangler secret put ALN_DASHBOARD_TOKEN`, then
`.\tools\worker.ps1 -DryRun` and `.\tools\worker.ps1` from the repository root.

## Known limits

- Friends-only rooms are public rooms whose description says so; the game checks friendship itself through WebApi, which
  is not provided, so such rooms may stay unjoinable. Invitations, PSN friends, presence and leaderboards use WebApi,
  NpScore and NpAuth, which still fail cleanly.
- The searchable int attributes 0x4d..0x53 and the room description (attr 0x55) are passed through unchanged; their
  meaning (mission, difficulty, world level) is not decoded, so the launcher's room list shows only host, players and
  joinability.
- Peer datagrams travel on a reliable, ordered link: a lost packet delays the ones behind it by about one round trip
  instead of being lost.
