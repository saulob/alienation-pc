# Online play

Alienation's own multiplayer code runs unchanged: the port answers the console services it calls (NpManager, NetCtl,
classic NpMatching2, the NP sessions and invitations of NpWebApi, the Invitation dialog, the session invitation system
event and the DGRAM_P2P socket of libSceNet). Players meet through the Alienation lobby (`server/lobby`); game traffic
goes directly between their PCs over UDP, and through the lobby only for a pair of players who cannot reach each other.
Nothing in the game executable is patched for online play, and with Online play off every import keeps its offline
behaviour (`src/runtime_services.c`).

The design follows Dragon's Crown Pro PC's production online stack (lobby, relay, peer transport, launcher page,
dashboard) as a separate service: its own Worker, namespaces, token, database and game key. Dragon's lobby is never
contacted.

Status: implemented and tested headlessly (backend protocol tests, the game's entry points against a loopback lobby for
rooms of two to four players, NP sessions, invitations and the relay fallback). Real game instances entering one native
squad have not been verified yet; see "Testing".

## What has been played

Confirmed in the real game (manual tests, 2026-11-12):

- Online co-op, 2 players: friends, the launcher receiving an invitation before the game runs and Accept and play
  starting the game into the squad, invitations in game, the session resolving to the right Matching2 room, the join,
  P2P over LAN (thousands of datagrams, no resend), mission gameplay together, leaving and joining again.
- Local co-op on CUSA00062 1.06, 2 players with two physical controllers: user 2 logs in and its pad opens, both play a
  mission, and each player's save is mounted and written at once (`/savedata0` for user 1, `/savedata1` for user 2).

Headless only (`tools\test.ps1`: 3 and 4 player rooms and invitations on modelled peers, local co-op with 1 to 4 virtual
gamepads), not yet played: online squads of 3 and 4 and local co-op with 3 or 4 controllers; local co-op on CUSA00062
1.00 and CUSA04430 1.00 is not available there (see Local co-op).

## Playing online

1. In the launcher's **Online** page, turn **Online play** On. The rest of the page appears only then. No environment
   variable is needed: the game reads the same setting (`online_enabled` in `alienation.ini`).
2. Choose your **Online name** (up to 16 letters, digits, `-` or `_`). It is your in-game online id. Left at `Player`, the
   id becomes `Player-` and six hex digits of your account, so two profiles never share one.
3. Share **Your friend code** (`XXXX-XXXX`, **Copy**). On the **Friends** tab, enter a friend's code and **Send request**;
   requests appear at the top of the page with **Accept** and **Decline**; **Remove** ends a friendship. A friend shows
   Online, Offline, "Online, in a room" or "Online, in a joinable room" (public, a free seat, your game build), and
   **Incompatible** when their room is of another game build or online version.
4. **Connection status** (Settings tab, and one line on the other tabs) shows Offline, Connecting, Connected, Limited or
   Not ready. Its Lobby row is the launcher's own link as it is now; Public, Direct, Router and Relay come from the last
   finished **Connection test** (kept in `alienation.ini` with when it ran, never assumed: until a test has run they
   read "Not tested"). Ready with Direct limited and the relay available still plays: players who cannot connect
   directly play through the relay. **Connection test** > **Run test** (PLAY waits for one finished test while Online
   play is on) reports:

   | Row | Ready | Limited | Not ready |
   |---|---|---|---|
   | Lobby | signed in, with the round trip | | service or identity unavailable |
   | Public | a STUN server saw this PC's public UDP endpoint | symmetric NAT, or STUN off | the UDP port cannot open, or no STUN answer |
   | Direct | other players can probably connect directly (or UPnP opened the port) | some players may not | no UDP port |
   | Router | UPnP mapped the port | UPnP off, unavailable or behind another router (not required) | the port is not open |
   | Relay | the service offers the relay and the port opened | | not offered, or no service |

   Every row comes from a measurement of this test (sign-in, two STUN servers from the game's UDP port, a UPnP probe,
   the service's relay capability); nothing is assumed from settings. Each real peer path is verified when joining.
5. The **Lobby** tab lists the **Public rooms** of your game build (host, players and seats, Normal or Hardcore, world
   level, host version, age) with **Join**. **Show me in Lobby** additionally lists your Online name so others can send a
   friend request. Friends and rooms refresh by themselves (friend events at once, the lists every 15 to 30 s while
   shown).
6. Press **Play**, then use the game's own squad menus: Create Squad, Join Squad, Friends only, Invite only, and
   **Invite friends** (the game's Invitation dialog shows your online friends of the same game build in a list).
7. **Invitations**: a friend's invitation appears in game as a prompt (**Join squad** / **Decline**; closing it keeps it
   for later), and in the launcher's Friends tab with **Accept and play** and **Decline**. **Join** on a friend in a
   joinable squad, or on a public room, starts the game and enters that squad. Either way the game joins through its
   own path: the launcher leaves `online_join.ini` in the profile, the game reads it at start (ignored after ten
   minutes) and receives it as a session invitation event, exactly as a PS4 game started from an invitation.

Only the same game build plays together: CUSA00062 1.00 (EUR), CUSA00062 1.06 (EUR, updated) and CUSA04430 1.00
(ASIA/KR) never meet each other, because the game's lockstep simulation needs the same executable on every PC. A complete 1.06
package runs as CUSA00062 1.06 (same executable and game data).

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
| `online_connection_test_result` | - | - | what that test measured and when, for the Connection status (no addresses) |
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
`tools\online-instance.ps1 -Name alpha` uses `user\test-client-alpha` and `build\logs\test-client-alpha`, copying the
settings and saves of `user\` on first start; two instances never share a save, identity or log folder.

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
2. **The network status box** (bottom right of the menus) shows Alienation PC's online state. In the original game it
   showed the login to "UNX Global Matrix", Housemarque's own server (`prod.alienation.online.scee.com:5555`) for UNX
   assignments, online and global missions, bounties, leagues and global statistics. That server is gone and squads
   never needed it: the first real Alpha/Bravo test formed a squad, joined it over P2P and reached a mission while that
   login never got past "connecting". The box belongs to the status machine `AN.Menu.Network`; the `server_*` natives
   are called only there, and only after its NP authorization code. That code is never issued now (`sceNpAuthPollAsync`
   stays "running"), so the machine waits in WAITING_PSN for good: no server connection, no Server_Connection threads,
   no traffic. The machine paints its text only from some of its states (the LB state's login checks, the CONNECTED
   entry) and clears or hides it on several paths (the LB entry, the CONNECTED fade, the helper when the menu hud is
   missing, `layout_box_set_visible(false)` on a server error or disconnect), while its dot is updated on every tick in
   every state. The script patch (`launcher/online_status_script.c`, sites found by their code in every build) makes
   that tick own the text. While the dot (`Network_Icon`) exists, the text is the current state's: repainted through
   the machine's own helper whenever the text box (`Menu_Main/Network_Status`) or its tag is gone or the state changed
   (asked from `server_request_auth_code` with magic arguments, hooked in `src/online_status.c`), and never faded.
   Without the dot (the Options screen, for one) the text is hidden and its tag cleared, so the box is kept and the
   dot's return paints the same box again: no duplicate box, nothing left floating. The helper paints the current
   state's tag whatever its caller passed, shows the text only while the dot exists, and a paint made without the dot
   is marked so that the dot's arrival paints again; it no longer hides the text after `Network_Manager`'s error
   dialog, and the error and disconnect paths no longer hide the box. The game colours the text only green or red (by
   a flag per call) and the dot from two `online` flags, so the patch also redirects both choices: the dot takes the
   colour of the state the text names (the current state when there is no text) and the text the dot's own colour,
   applied through the dot's colour code (green 0.5/1/0.5, amber 1/1/0.5, red 1.1/0.3/0.3). `vec4` and other math/list
   natives are `ret` stubs outside the block's code range (the game makes them intrinsics only inside it), so the
   appended code never calls them:

   | Shown | Machine state | Text and dot | When |
   |---|---|---|---|
   | Online Connecting | WAITING_PSN / LOGIN | amber | Online Play on, the stack not ready yet (lobby link, NP sign-in, Matching2 context) |
   | Online Connected | CONNECTED | green | lobby signed in, NP signed in, the game's Matching2 context started |
   | Online Unavailable | GENERIC_ERROR | red | the lobby went down past its reconnect grace, or did not answer within 45 s; back to Connected when it returns |
   | Online Off | NETWORK_OFFLINE / USER_OFFLINE | amber | Online Play off (the user's choice, not a failure: never red, and not "offline" or "disconnected"; the box has no neutral colour) |

   A peer is not needed for Connected. The texts are the box's own localization keys (`menu__network__no_matching`,
   `login_wait_prompt`, `connected`, `error_general`, `platform_offline`, `user_offline`), answered by a hook on the two
   lookups of the game's localize helper in all 18 languages (`src/online_status_text.inc`; every character occurs in
   that language's own texts, so its font has it). No localization block is rewritten: the CJK and Russian blocks do
   not fit their units once recompressed. The UNX features themselves stay unavailable. Log lines:
   `Online status: connecting|connected|unavailable|off (why)` on each change. The Matching2 context: `ContextStart` opens the lobby link and 0x6f02 follows the lobby's hello answer;
   `GetWorldInfoList` answers world 1; with the lobby unreachable the context starts over (0x6f01, 0x80550d02).
3. **Rooms** (classic Matching2 over the lobby, `src/runtime_matching2.cpp`): `CreateJoinRoom` (hidden at first, flags
   0x14000000), `SetRoomDataInternal` to make it public (0x04000000) or keep it hidden (invitations), `SetRoomDataExternal`
   for the 0x70-byte room description (attr 0x55), `SearchRoom` with the game's flag filter and int filters on attrs
   0x4d..0x53, `JoinRoom` checking the compatibility byte (internal attr 0x57), `LeaveRoom`,
   `GetRoomDataExternalList`, `GetRoomMemberDataExternalList`, `SignalingGetPingInfo`.
4. **Peers**: the game polls `SignalingGetConnectionStatus` for each member until it is active, then sends to the address
   and port it got; it attributes every received datagram by an exact match of that address and port.

Matching2 callbacks run only from `sceNpCheckCallback` (every frame, EU 0x1439ae0), never inside a Matching2 call and
never with a port lock held: the game marks a request pending only after the call returns and takes its own locks in the
callbacks. With Online play on, the lobby link opens when the game starts (friends see the player online and invitations
arrive) and stays open; a link that drops is opened again every 30 s.

## Sessions and invitations

Every squad the game creates gets an NP session, and every join goes through one (identical in 1.00, 1.06 and the KR
build): creating runs CreateJoinRoom, then `POST /v1/sessions`, then SetRoomDataInternal and SetRoomDataExternal with
the session id at +0x40 of the 0x70-byte description (attr 0x55); joining (by invitation, by friend, by room list or by
quick match) runs `POST /v1/sessions/{id}/members`, then `GET /v1/sessions/{id}/sessionData`, whose first 8 bytes are
the Matching2 room id JoinRoom takes. A failed step ends the game's request queue, so without these answers no squad
forms. `src/runtime_webapi.cpp` answers them through the lobby, on the game's WebApi thread (the link's answers are
read there while no pump runs; game callbacks still run only from `sceNpCheckCallback`):

| Game request (apiGroup) | Lobby op | Answer |
|---|---|---|
| `POST /v1/sessions` (sessionInvitation), multipart: JSON request, image, 16 bytes of session data | `session.create` {private, max, data} | 200 `{"sessionId"}` (36 characters) |
| `POST /v1/sessions/{id}/members` | `session.join` | 204, or 403 / 404 (other build, full, private and not invited or a friend, gone) |
| `GET /v1/sessions/{id}/sessionData` | `session.data` | 200, the 16 bytes: room id u64, world u32, server u16, seats u8, one byte |
| `PUT /v1/sessions/{id}` `{"sessionPrivacy"}` | `session.update` | 204 (a name or status update: 204, kept locally) |
| `DELETE /v1/sessions/{id}`, `.../members/{onlineId}` | `session.leave` | 204 (the owner's ends the session) |
| `PUT /v1/users/{id}/invitations/{id}` `{"usedFlag":true}` | `invite.used` | 204 |
| `GET /v1/users/{id}/friendList?...` (userProfile) | `friends.list` | `totalResults`, `friendList[]` {`npId` (hex SceNpId, read back by `sceNpWebApiUtilityParseNpId`), `presence.onlineStatus`} |
| `GET .../presence`, `.../profile`, `.../blockList`; `PUT .../presence/gameStatus` | - | online status from the friends, an empty profile and block list, 204 |

The session data is opaque to the lobby; the room the session belongs to is the caller's own room. Friends-only and
invite-only squads are hidden rooms with a private session: the lobby lets the owner's friends and the invited players
join it, and the game's host still applies its own checks (friends only: the joiner is in its friend list, now the
lobby's friends; invite only: the joiner came through an accepted invitation). Push events are registered and never
called: the game only counts pushed invitations and its scripts never read that count.

**Invitation dialog** (`sceInvitationDialogOpen`, param 0x90: mode @0x34, data @0x48 {message @0, session id @8, max
@0x18}): send mode shows the player's online friends of the same build in the overlay list ("Bravo · online", "Bravo ·
in a squad"; the title adds the counts) and sends `invite.send` for the one picked; with nobody to list its one row
says why: no friends added, a friend request still waiting, friends online on another game version, or no friend
online. Friends are only players who accepted a friend request; sharing a squad or the public lobby makes nobody a
friend. The game's own friend list (userProfile `friendList`) is the lobby's `friends.list`, all friends with their
presence; `Friend: list for the game: ...` logs it when it changes. Receive mode lists the pending invitations. GetResult writes the error @8, cancelled @0xc and the number
sent through the pointer @0x10.

**Accepting**: the lobby's `invite.received` event becomes an in-game prompt (never over the menu or another list); Join
squad answers `invite.answer` and queues `SCE_SYSTEM_SERVICE_EVENT_SESSION_INVITATION` (0x10000002: session id @4,
invitation id @0x34, flag 1 @0x74, user id of local user 0 @0x90). A Join from the launcher is the same event with no
invitation id and flag 0, which the game treats as "joined from the system menu". `sceSystemServiceGetStatus` reports
the event only once the game's user manager has polled UserService, since the game drops an invitation for a user it
does not know yet; the game then shows its own Join_Invite screen and joins through the session.

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

## Leaving a squad and the timers

Every value is read from the code (`server/lobby/src/lobby.js` `LIMITS`, `src/net/lobby_link.h`, `src/net/peer_socket.h`).

| What | Behaviour |
|---|---|
| Clean exit (Exit Game, window closed, guest exit) | `runtime_online_leave` sends `room.leave` and closes the lobby link, waiting at most 1 s for the answer: the member is removed at once (`room.member_left`, why `leave`, and `peer.gone`), its seat is free and friends see it offline. Without it a closing process only closes its socket and the seat waits out the grace period. |
| Friend presence | Ends when the account's last present socket closes: `friend.offline` at once, for a clean close or a killed process (the OS closes the socket). There is no presence TTL. The lobby has no idle timer of its own: a socket that goes silent without closing is ended by the player's own link (a keepalive `ping` every 20 s, dead after 45 s without any frame, then 25 s of reconnects) or by the hosting platform. |
| Room seat after a closed socket | Held for `LIMITS.graceMs` = 30 s (the player may `room.resume`); then removed with why `disconnect`, with `peer.gone`, `room.member_left` and owner migration. A held seat still counts: the room stays full for 30 s. A room whose members all dropped is deleted 30 s later. |
| NP session | One per room, no member list of its own; it is deleted when the room's last member goes, or when its owner ends it. |
| Invitations | Expire after `LIMITS.inviteMs` = 30 min (checked when invitations are used) and are deleted with their room. |
| Public Lobby listing | An entry with no keepalive for `LIMITS.lobbyStaleMs` = 75 s is left out of the list. |
| Peer link (P2P) | A direct or relay path with no traffic for 10 s (`PeerSocket::Options::idle_timeout_ms`) is logged as timed out and dialled again; a link that does not connect in 15 s fails. A dead peer link alone never removes a member: only the lobby does, after the times above. |

`tests/online_hle_tests.cpp` (clean exit) and `server/lobby/test/cleanup.test.js` (fake clock) cover these.

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
| `friends.list` | rows add `build`, `joinable`, and `session` when the viewer may join that squad from the friend list |
| `session.create` / `.join` / `.data` / `.update` / `.leave` | NP sessions (table above); one per room, gone with its room |
| `invite.send` / `.list` / `.answer` / `.used` | invitations to a session, to friends only, 30 minutes; events `invite.received`, `invite.answered` |
| `rooms.list` | a joinable public room adds its `session` (what the game's own room list joins through) |
| `party.report` | not supported |

## Local co-op

CUSA00062 1.06 adds local co-op; 1.00 and the KR build have none (`tests/patch_site_tests.py` checks each
build's menu bytecode). Up to four players: each controller is a local user, and the game's user manager opens a pad for
every user it hears of (the initial user at start, then each LOGIN event). Another player joins by pressing OPTIONS on
their controller ("Join Local Co-op" on the squad screen, "Switch Player" on the player screen); each local player has
their own class, gear, save (per user) and trophies. The port gives the first gamepad (or the keyboard) to player 1 and
each further gamepad, in the order they connect, to players 2 to 4 (user ids 1-4, "Player N", the PS4 colours); a
disconnected controller keeps its user, and its slot takes the next controller that connects; the light bar follows
the game's player colours on gamepads that have one.

Local and online do not combine: the game creates its online context for local user 0 only and switches to offline when
a local player joins ("You have switched to a local co-op game and are now offline"). Local guests never need an online
identity.

How 1.06 holds local players: they are members of `Network_Manager`'s room (`room_member_list`,
`network_room_get_local_player_index`), so the room's four seats are the limit for local players as for online ones; the
network status machine runs per local player (its `player` variable) and only player 0 paints the status box. Each
player saves through their own user: the game mounts the same directory name, `savedata`, once per user, and the port
keeps each user's copy apart (`<profile>/savedata/<user id>/<title>/savedata`, mount points `/savedata0`, `/savedata1`,
and so on in mount order; `tests/savedata_tests.c` mounts users 1 to 4 at once).

Why 1.00 and CUSA04430 lack it: besides the menus, their executables do not register 27 (KR: 22) engine natives that the
1.06 scripts call, among them `online_user_set_active`, `persistence_is_save_queued`, `controller_reset_light_bar`,
`wwise_sound_new_controller` and `wwise_trigger_event_for_controller` (per-controller audio). Moving the 1.06 scripts onto
those builds would need these engine features rebuilt in the port, so CUSA00062 owners get local co-op from the 1.06
update (Install Update), and the KR build has no known update that adds it.

Where a later telemetry report can count players: local users from `libSceUserService` logins (`runtime_users.c`), room
members and their names from Matching2 member events (`runtime_matching2.cpp`), the party size as the room's member
count; per-member binary attribute 0x59 is passed through and not decoded, so a player's class is not known to the port
yet.

## Testing

Headless (no game, no window):

```
cd server\lobby; node --test "test/*.test.js"     # protocol, builds, rooms, friends, relay, limits, dashboard
tools\test.ps1                                     # everything headless, with a loopback lobby for aln-online-hle-tests
```

`aln-online-hle-tests` calls the classic Matching2, NpWebApi, Invitation dialog and libSceNet entry points as the game
does while other players use the lobby protocol and connect over UDP: the unreachable-service path (0x6f01), context
start, world list, room create (hidden, then public), the member join event with its SceNpId and attribute 0x59,
endpoint exchange, signaling pending then active at the virtual endpoint, epoll writability, datagrams both ways (6000
bytes, exact source match, EAGAIN when empty), member and room external reads, a KR room invisible and refused from the
EU build, member leave (0x1102, 0x5101), leave, search; squads of four (the game as the fourth member, a fifth refused,
a member leaving and taking the same seat again), two and three seats, a full room (0x80550d19) and a vanished one
(0x80550d13); a guest no direct path reaches connected through the relay; the session of a hosted squad (the game's
multipart POST) joined by another player; privacy changes; the friend list and its npId; the Invitation dialog sending to
a friend; an invitation received, prompted, accepted and delivered as SESSION_INVITATION, then the join through POST
members, GET sessionData and JoinRoom; a declined invitation; the launcher's Join file, fresh and too old; the launcher's
Accept and play (the launcher's own online client receives the invitation while no game runs, accepts it and writes the
Join file with the invitation, which is delivered as an accepted invitation, then the session's room joined);
the game inviting three friends at once (independent invitations, one declined, the others accepted out of order, a new
invitation to the one who declined, the room full at four, a guest leaving, another player taking the seat and the
first one refused until it is free again); rooms of two, three and four players hosted by the game, each with its
session, a search that finds it and hides it when full, the seats, the member events and the external member list, and
the full mesh: every pair of players (1, 3 and 6 pairs) linked through its own peer socket, a datagram each way over
every pair, then a guest leaving, its seat taken by another player and the first one rejoining with the mesh restored;
socket abort/shutdown/close and context stop. The game asks Matching2 for a full mesh (`network_topology_full_mesh`), so
every member links every other one: six links for four players, each instance making its own three.

`aln-local-pad-tests` drives SDL3 virtual gamepads (physical backends off) through libScePad and libSceUserService:
enumeration at start, two, three and four local controllers with their LOGIN events and pad handles, OPTIONS on the
second player's pad, a fifth controller ignored, a disconnect and reconnect keeping the user, names, colours and the
light bar. `aln-online-view-tests` checks the Online page model: the connection status from the live link and the
stored test, friends (Join only for a compatible, joinable squad), invitations, public rooms and the Join file.

Instances on one PC (each its own profile, saves, identity, logs and UDP port; the Online name is the callsign):

```
tools\online-instance.ps1 -Name alpha                       # terminal 1, online as Alpha
tools\online-instance.ps1 -Name bravo                       # terminal 2, online as Bravo
tools\online-instance.ps1 -Players 4                        # Alpha, Bravo, Charlie and Delta at once
tools\online-instance.ps1 -Name alpha -Build CUSA00062-1.06 # pinned to one prepared build
tools\online-instance.ps1 -Players 2 -Lobby ws://127.0.0.1:8790/  # with tools\online-lobby.ps1 in another window
tools\online-instance.ps1 -Players 2 -Friends               # then Alpha sends Bravo a friend request, Bravo accepts
```

Three or four full games on one PC can exhaust Windows commit and video memory (a real 4-instance run reached 105862 of
106257 MiB commit, then DirectSound and a 1920x1080 Vulkan image failed in the fourth process). With `-Players 3` or
`4` (or `-Resources Light`) every instance but Alpha runs a light profile written into its own `alienation.ini`:
windowed 1280x720, render resolution 720p (`-LightResolution 480p` is the lowest the launcher offers), SMAA, FSR 1 and
RCAS off, texture filtering from the game, and `ALN_AUDIO=none` (silent timer sink) for its process. Windows stay
visible and playable. `-Players 2`, a single `-Name` and `-Resources Normal` keep the settings as they are, and an
instance that was light gets its earlier values back (a value it no longer has at the light value is kept). The
guest's own memory (5056 MiB direct pool and the mapped ranges) does not depend on these settings; what shrinks is the
video memory that scales with render resolution (720p is 44 % of the 1080p pixels, 480p 20 %) and the audio device.

Two instances are not friends until one accepts the other's request. `-Friends` does that through the lobby with the
normal protocol (`tools\online-friends.mjs`, each instance signed in with its own identity and presence off); against
the development lobby `tools\online-lobby.ps1 -SeedTestFriends` seeds the same friendship. By hand: Online page, Friends
tab, enter the other instance's friend code, Send request, and Accept on the other one.

What to look for in each instance's log: `Online: on as ...`, `Matching2: signed in to the lobby`, `Online status:
connected`, `Friend: Bravo is now your friend`, `Friend: Invite dialog: ...`, `Matching2: created
room N` / `joined room N`, `Session: created ...` (host) or `Session: joining ...` and `Session: ... leads to room N`
(joiner), `Net: P2P socket ... bound`, `Matching2: endpoint reported`, `Matching2: member 0x20 linking`, `P2P: link N ...
connected via LAN / public / the relay`, `Matching2: member 0x20 established`, `Friend: invite sent to ...`, `Friend: ...
invited you`, `Friend: invite accepted`, `Runtime: system event 0x10000002 delivered`, and at the end the `P2P: member
0x20 ...` line with datagram counts. A `WebApi: ... is not provided` line names a request the port does not answer.

Two instances on one PC connect through their LAN candidates; NAT traversal needs two PCs on different Internet
connections (see the manual A/B test in the delivery notes).

## Deploying the lobby

See [server/lobby/README.md](../server/lobby/README.md): `wrangler secret put ALN_DASHBOARD_TOKEN`, then
`.\tools\worker.ps1 -DryRun` and `.\tools\worker.ps1` from the repository root.

## Known limits

- Friends-only and invite-only squads are private: Join from the friend list is offered for a friend's private squad,
  and the game's host refuses a player who was not invited to an invite-only one (the lobby cannot tell the two apart).
- The block list is always empty; leaderboards, weekly missions and leagues (NpScore, TSS) still fail cleanly, and the
  UNX Global Matrix features (UNX assignments, online and global missions, bounties, leagues, global stats) are
  unavailable: their server is never contacted.
- The in-game prompt for an invitation pauses the game's input like any overlay list while it is open.
- The searchable int attributes and the room description (attr 0x55) are passed through unchanged. The launcher shows
  only what the game's own filter names prove: 0x4d game_mode (bit 0 Hardcore) and 0x4f world_level; mission and
  difficulty are packed in the description and not shown.
- Peer datagrams travel on a reliable, ordered link: a lost packet delays the ones behind it by about one round trip
  instead of being lost.
