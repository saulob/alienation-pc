// The NP sessions every Alienation room gets (POST /v1/sessions from the game's WebApi) and the invitations to them:
// how a joiner finds the room from a session, private squads, invitations, the friend list's Join and public room Join.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS } from '../src/lobby.js';
import { makeLobby } from './harness.js';

const EU = 'CUSA00062-1.00';
const EU_106 = 'CUSA00062-1.06';
const PUBLIC = 0x04000000;
const HIDDEN = 0x14000000;
const SESSION_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

let nextIp = 20;
function player(env, name, fields = {}) {
  const client = env.connect(`198.51.100.${nextIp++}`);
  assert.equal(client.hello({ name, relay: true, ...fields }).code, 'ok');
  client.name = name;
  return client;
}

function friends(a, b) {
  const code = a.call('friends.list') && a.lobby.accountRow(a.account).code;
  assert.equal(b.call('friends.add', { code }).code, 'ok');
  assert.equal(a.call('friends.accept', { account: b.account }).code, 'ok');
}

// The session data the game sends: room id u64, world u32, server u16, seats u8, one byte of anything.
function sessionData(room, seats = 4) {
  const bytes = Buffer.alloc(16);
  bytes.writeBigUInt64LE(BigInt(room), 0);
  bytes.writeUInt32LE(1, 8);
  bytes.writeUInt16LE(1, 12);
  bytes[14] = seats;
  bytes[15] = 0xcc;
  return bytes.toString('hex');
}

// A squad as the game makes it: the room (hidden), its session, then public unless private.
function squad(host, { priv = false, slots = 4 } = {}) {
  const created = host.call('room.create', { world: 1, slots, flags: HIDDEN });
  assert.equal(created.code, 'ok');
  const room = created.room.id;
  const made = host.call('session.create', { private: priv, max: slots, data: sessionData(room, slots) });
  assert.equal(made.code, 'ok');
  assert.match(made.session, SESSION_ID);
  if (!priv) assert.equal(host.call('room.set_internal', { room, flag_mask: HIDDEN, flag_value: PUBLIC }).code, 'ok');
  return { room, session: made.session };
}

test('a public squad: any player of the build reads the session data and joins the room it names', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const { room, session } = squad(host);
  const guest = player(env, 'Bravo');
  assert.equal(guest.call('session.join', { session }).code, 'ok');
  const data = guest.call('session.data', { session });
  assert.equal(data.code, 'ok');
  assert.equal(data.data, sessionData(room));
  assert.equal(BigInt.asUintN(64, Buffer.from(data.data, 'hex').readBigUInt64LE(0)).toString(), room);
  assert.equal(guest.call('room.join', { room }).code, 'ok');
  // Asking again for the same room gives the same session: one per room.
  assert.equal(host.call('session.create', { private: false, max: 4, data: sessionData(room) }).session, session);
});

test('only the room owner creates its session, with valid data', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  assert.equal(host.call('session.create', { private: false, max: 4, data: sessionData(1) }).code, 'not_found');
  const { room } = squad(host);
  const guest = player(env, 'Bravo');
  assert.equal(guest.call('room.join', { room }).code, 'ok');
  assert.equal(guest.call('session.create', { private: false, max: 4, data: sessionData(room) }).code, 'not_owner');
  for (const bad of [{ private: 'no', max: 4, data: sessionData(room) }, { private: false, max: 9, data: sessionData(room) },
    { private: false, max: 4, data: 'ab' }, { private: false, max: 4, data: 'zz'.repeat(16) }]) {
    assert.equal(host.call('session.create', bad).code, 'bad_request');
  }
  assert.equal(host.call('session.data', { session: 'not-a-session' }).code, 'bad_request');
  assert.equal(host.call('session.data', { session: '00000000-0000-0000-0000-000000000000' }).code, 'not_found');
});

test('another build cannot join through a session, a full room is refused, and a closed room is gone', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const { room, session } = squad(host, { slots: 2 });
  const updated = player(env, 'Newer', { build: EU_106 });
  const refused = updated.call('session.join', { session });
  assert.equal(refused.code, 'incompatible');
  assert.equal(refused.build, EU);
  assert.equal(updated.call('session.data', { session }).code, 'incompatible');
  assert.equal(player(env, 'Bravo').call('room.join', { room }).code, 'ok');
  assert.equal(player(env, 'Charlie').call('session.join', { session }).code, 'full');
  host.call('room.leave', { room });
  const last = env.lobby.memberRows(Number(room));
  assert.equal(last.length, 1);
  env.lobby.removeMember(Number(room), last[0].member, 'leave');
  assert.equal(player(env, 'Delta').call('session.join', { session }).code, 'not_found');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM room_sessions').n, 0);
});

test('a private squad (friends only or invite only) is open to friends of the owner and to invited players', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const friend = player(env, 'Bravo');
  const stranger = player(env, 'Charlie');
  friends(host, friend);
  const { session } = squad(host, { priv: true });
  assert.equal(stranger.call('session.join', { session }).code, 'forbidden');
  assert.equal(stranger.call('session.data', { session }).code, 'forbidden');
  assert.equal(friend.call('session.join', { session }).code, 'ok');
  // The room stays hidden from searches and from the public room list.
  assert.equal(stranger.call('rooms.list').rooms.length, 0);
  // The friend list shows the friend's private squad as joinable for a friend, with its session.
  const row = friend.call('friends.list').friends.find((f) => f.name === 'Alpha');
  assert.equal(row.session, session);
  assert.equal(row.joinable, true);
});

test('privacy changes with the squad settings, and the owner ending the session closes it to joiners', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const { room, session } = squad(host);
  const stranger = player(env, 'Charlie');
  assert.equal(host.call('session.update', { session, private: true }).code, 'ok');
  assert.equal(stranger.call('session.join', { session }).code, 'forbidden');
  assert.equal(host.call('session.update', { session, private: false }).code, 'ok');
  assert.equal(stranger.call('session.join', { session }).code, 'ok');
  assert.equal(stranger.call('session.update', { session, private: true }).code, 'not_found');
  assert.equal(stranger.call('session.leave', { session }).code, 'ok');
  assert.equal(stranger.call('session.join', { session }).code, 'ok');
  assert.equal(host.call('session.leave', { session }).code, 'ok');
  assert.equal(stranger.call('session.join', { session }).code, 'not_found');
  assert.equal(host.call('room.leave', { room }).code, 'ok');
});

test('invitations: the host invites a friend, who hears it, lists it and accepts; the host hears the answer', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const friend = player(env, 'Bravo');
  const stranger = player(env, 'Charlie');
  friends(host, friend);
  const { room, session } = squad(host, { priv: true });
  assert.equal(host.call('invite.send', { session, to: [stranger.account] }).code, 'not_friends');
  const sent = host.call('invite.send', { session, to: [friend.account], message: 'Join my squad' });
  assert.equal(sent.code, 'ok');
  assert.equal(sent.sent, 1);
  const [heard] = friend.events('invite.received');
  assert.equal(heard.session, session);
  assert.equal(heard.name, 'Alpha');
  assert.equal(heard.message, 'Join my squad');
  assert.equal(heard.build, EU);
  assert.match(heard.invitation, /^[0-9a-f]{32}$/);
  assert.ok(!JSON.stringify(heard).includes(room) && !JSON.stringify(heard).includes('198.51.100'));
  const listed = friend.call('invite.list').invitations;
  assert.equal(listed.length, 1);
  assert.deepEqual([listed[0].invitation, listed[0].joinable, listed[0].players, listed[0].slots], [heard.invitation, true, 1, 4]);
  // The invitation alone opens a private session to a player who is not the owner's friend.
  assert.equal(host.call('friends.remove', { account: friend.account }).code, 'ok');
  assert.equal(friend.call('session.join', { session }).code, 'ok');
  const accepted = friend.call('invite.answer', { invitation: heard.invitation, accept: true });
  assert.equal(accepted.code, 'ok');
  assert.equal(accepted.session, session);
  assert.equal(accepted.joinable, true);
  const [answer] = host.events('invite.answered');
  assert.deepEqual([answer.invitation, answer.accept, answer.name], [heard.invitation, true, 'Bravo']);
  assert.equal(friend.call('invite.list').invitations.length, 0);
  assert.equal(friend.call('session.data', { session }).data, sessionData(room));
  assert.equal(friend.call('room.join', { room }).code, 'ok');
  assert.equal(friend.call('invite.used', { invitation: heard.invitation }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT state FROM invitations WHERE id = ?', heard.invitation).state, 'used');
});

test('a declined invitation closes the private session to that player and tells the host', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const friend = player(env, 'Bravo');
  friends(host, friend);
  const { session } = squad(host, { priv: true });
  host.call('invite.send', { session, to: [friend.account] });
  const [heard] = friend.events('invite.received');
  host.call('friends.remove', { account: friend.account });
  assert.equal(friend.call('invite.answer', { invitation: heard.invitation, accept: false }).code, 'ok');
  assert.equal(host.events('invite.answered')[0].accept, false);
  assert.equal(friend.call('session.join', { session }).code, 'forbidden');
  assert.equal(friend.call('invite.answer', { invitation: heard.invitation, accept: true }).code, 'ok');
  assert.equal(host.call('invite.answer', { invitation: heard.invitation, accept: true }).code, 'not_found');
  assert.equal(friend.call('invite.answer', { invitation: 'x', accept: true }).code, 'bad_request');
});

test('invitations expire, die with their room and are bounded', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const friend = player(env, 'Bravo');
  friends(host, friend);
  const { room, session } = squad(host);
  assert.equal(host.call('invite.send', { session, to: [] }).code, 'bad_request');
  assert.equal(host.call('invite.send', { session, to: Array(LIMITS.invitesPerSend + 1).fill(friend.account) }).code,
    'bad_request');
  host.call('invite.send', { session, to: [friend.account] });
  host.call('invite.send', { session, to: [friend.account] });
  assert.equal(friend.call('invite.list').invitations.length, 1);
  env.advance(LIMITS.inviteMs + 1000);
  assert.equal(friend.call('invite.list').invitations.length, 0);
  host.call('invite.send', { session, to: [friend.account] });
  assert.equal(friend.call('invite.list').invitations.length, 1);
  host.call('room.leave', { room });
  assert.equal(friend.call('invite.list').invitations.length, 0);
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM invitations').n, 0);
  const outsider = player(env, 'Charlie');
  assert.equal(outsider.call('invite.send', { session, to: [friend.account] }).code, 'not_found');
});

test('a public room on the launcher list carries its session; a private or full one does not', () => {
  const env = makeLobby({ log: () => {} });
  const open = squad(player(env, 'Alpha'));
  squad(player(env, 'Hidden'), { priv: true });
  const full = squad(player(env, 'Pair'), { slots: 2 });
  assert.equal(player(env, 'Joined').call('room.join', { room: full.room }).code, 'ok');
  const reader = player(env, 'Reader');
  const rows = reader.call('rooms.list').rooms;
  assert.deepEqual(rows.map((r) => [r.host, r.joinable, r.session ?? null]).sort(),
    [['Alpha', true, open.session], ['Pair', false, null]].sort());
  const other = player(env, 'Newer', { build: EU_106 });
  assert.equal(other.call('rooms.list').rooms.length, 0);
});

test('the admin summary counts joinable rooms and rooms by players and build', () => {
  const env = makeLobby({ log: () => {} });
  squad(player(env, 'Alpha'));
  const full = squad(player(env, 'Pair'), { slots: 2 });
  player(env, 'Joined').call('room.join', { room: full.room });
  squad(player(env, 'Newer', { build: EU_106 }), { priv: true });
  const live = env.lobby.metricsSummary().live;
  assert.equal(live.rooms, 3);
  assert.equal(live.joinable_rooms, 1);
  assert.deepEqual(live.room_players.map((r) => [r.value, r.count]).sort(), [['1/4', 2], ['2/2', 1]]);
  assert.deepEqual(live.room_build.map((r) => [r.value, r.count]).sort(), [[EU, 2], [EU_106, 1]]);
});

test('repeating a session or an answer that changes nothing writes nothing, tells nobody and keeps the answer', () => {
  const env = makeLobby({ log: () => {} });
  const host = player(env, 'Alpha');
  const friend = player(env, 'Bravo');
  friends(host, friend);
  const { room, session } = squad(host, { priv: true });
  const changes = () => env.host.sql.get('SELECT total_changes() AS n').n;

  let before = changes();
  for (let i = 0; i < 50; i++) {
    env.advance(100);
    assert.deepEqual(host.call('session.create', { private: true, max: 4, data: sessionData(room) }).session, session);
  }
  assert.equal(changes(), before);
  assert.equal(host.call('session.create', { private: false, max: 4, data: sessionData(room) }).session, session);
  assert.equal(changes(), before + 1);

  const invitation = host.call('invite.send', { session, to: [friend.account] }) && friend.events('invite.received')[0].invitation;
  const first = friend.call('invite.answer', { invitation, accept: true });
  assert.equal(host.events('invite.answered').length, 1);
  before = changes();
  for (let i = 0; i < 50; i++) {
    env.advance(100);
    const again = friend.call('invite.answer', { invitation, accept: true });
    assert.deepEqual({ ...again, re: 0 }, { ...first, re: 0 });
  }
  assert.equal(changes(), before);
  assert.equal(host.events('invite.answered').length, 0);

  assert.equal(friend.call('invite.used', { invitation }).code, 'ok');
  before = changes();
  for (let i = 0; i < 50; i++) {
    env.advance(100);
    assert.equal(friend.call('invite.used', { invitation }).code, 'ok');
  }
  assert.equal(changes(), before);
  assert.equal(env.host.sql.get('SELECT state FROM invitations WHERE id = ?', invitation).state, 'used');
});
