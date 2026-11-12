// What the lobby does when a player goes away: a clean leave frees the seat at once, a dropped socket ends presence at once
// and frees the seat after LIMITS.graceMs, and invitations and sessions follow their room. The clock is the harness's.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LIMITS } from '../src/lobby.js';
import { makeLobby } from './harness.js';

const NAMES = ['Alpha', 'Bravo', 'Charlie', 'Delta'];
const HIDDEN = 0x14000000;
const PUBLIC = 0x04000000;

let nextIp = 60;
function player(env, name) {
  const client = env.connect(`198.51.100.${nextIp++}`);
  assert.equal(client.hello({ name, relay: true }).code, 'ok');
  client.name = name;
  return client;
}

function befriend(a, b) {
  const code = a.lobby.accountRow(a.account).code;
  assert.equal(b.call('friends.add', { code }).code, 'ok');
  assert.equal(a.call('friends.accept', { account: b.account }).code, 'ok');
}

function sessionData(room) {
  const bytes = Buffer.alloc(16);
  bytes.writeBigUInt64LE(BigInt(room), 0);
  bytes.writeUInt32LE(1, 8);
  bytes.writeUInt16LE(1, 12);
  bytes[14] = 4;
  return bytes.toString('hex');
}

// Alpha hosts a public four-seat squad with a session; Bravo, Charlie and Delta join; everyone is friends with Alpha.
function fullSquad(env) {
  const [alpha, bravo, charlie, delta] = NAMES.map((name) => player(env, name));
  for (const other of [bravo, charlie, delta]) befriend(alpha, other);
  const created = alpha.call('room.create', { world: 1, slots: 4, flags: HIDDEN });
  assert.equal(created.code, 'ok');
  const room = created.room.id;
  const made = alpha.call('session.create', { private: false, max: 4, data: sessionData(room) });
  assert.equal(made.code, 'ok');
  assert.equal(alpha.call('room.set_internal', { room, flag_mask: HIDDEN, flag_value: PUBLIC }).code, 'ok');
  const seats = [];
  for (const guest of [bravo, charlie, delta]) {
    const joined = guest.call('room.join', { room });
    assert.equal(joined.code, 'ok');
    seats.push(joined.me);
  }
  assert.deepEqual(seats, [0x20, 0x30, 0x40]);
  for (const client of [alpha, bravo, charlie, delta]) client.inbox = [];
  return { alpha, bravo, charlie, delta, room, session: made.session };
}

const members = (client, room) => client.call('room.members', { room }).members.map((m) => m.name);
const friendOf = (client, other) => client.call('friends.list').friends.find((f) => f.account === other.account);

test('a clean leave removes the member and frees the seat at once, and closing the socket afterwards changes nothing', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, bravo, charlie, delta, room } = fullSquad(env);
  assert.equal(delta.call('room.leave', { room }).code, 'ok');
  assert.deepEqual(alpha.events('room.member_left').map((e) => [e.member, e.why]), [[0x40, 'leave']]);
  assert.deepEqual(alpha.events('peer.gone').map((e) => e.member), [0x40]);
  assert.deepEqual(bravo.events('room.member_left').map((e) => e.member), [0x40]);
  assert.deepEqual(members(alpha, room), ['Alpha', 'Bravo', 'Charlie']);

  delta.drop();
  assert.deepEqual(alpha.events('friend.offline').map((e) => e.account), [delta.account]);
  env.advance(LIMITS.graceMs + 1);
  assert.equal(alpha.events('room.member_left').length, 0);

  const echo = player(env, 'Echo');
  assert.equal(echo.call('room.join', { room }).me, 0x40);
  assert.equal(charlie.call('room.leave', { room }).code, 'ok');
  const foxtrot = player(env, 'Foxtrot');
  assert.equal(foxtrot.call('room.join', { room }).me, 0x30);
});

test('a dropped socket ends presence at once and the seat after the grace period, then the seat is reusable', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, bravo, charlie, delta, room } = fullSquad(env);
  charlie.drop();

  assert.deepEqual(alpha.events('friend.offline').map((e) => e.account), [charlie.account]);
  assert.deepEqual([friendOf(alpha, charlie).online, friendOf(alpha, charlie).room], [false, undefined]);
  assert.deepEqual(members(alpha, room), ['Alpha', 'Bravo', 'Charlie', 'Delta']);
  assert.equal(alpha.events('room.member_left').length, 0);
  const echo = player(env, 'Echo');
  assert.equal(echo.call('room.join', { room }).code, 'full');

  env.advance(LIMITS.graceMs - 1);
  assert.equal(alpha.events('room.member_left').length, 0);
  assert.equal(members(alpha, room).length, 4);

  env.advance(1);
  assert.deepEqual(alpha.events('room.member_left').map((e) => [e.member, e.why]), [[0x30, 'disconnect']]);
  assert.deepEqual(bravo.events('peer.gone').map((e) => e.member), [0x30]);
  assert.deepEqual(delta.events('room.member_left').map((e) => e.member), [0x30]);
  assert.deepEqual(members(alpha, room), ['Alpha', 'Bravo', 'Delta']);
  assert.equal(echo.call('room.join', { room }).me, 0x30);
});

test('a dropped member who comes back inside the grace period keeps the seat and is not announced as gone', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, bravo, room } = fullSquad(env);
  bravo.drop();
  env.advance(LIMITS.graceMs - 1000);
  const back = env.connect('198.51.100.200');
  back.account = bravo.account;
  back.secret = bravo.secret;
  assert.equal(back.hello({ name: 'Bravo', relay: true }).code, 'ok');
  assert.equal(back.call('room.resume', { room, member: 0x20 }).me, 0x20);
  env.advance(LIMITS.graceMs);
  assert.equal(alpha.events('room.member_left').length, 0);
  assert.equal(members(alpha, room).length, 4);
});

test('when every socket of a room drops, the room and its session are gone after the grace period, not before', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, bravo, charlie, delta, room, session } = fullSquad(env);
  const observer = player(env, 'Echo');
  const rooms = () => env.host.sql.get('SELECT COUNT(*) AS n FROM rooms').n;
  assert.equal(rooms(), 1);
  for (const client of [alpha, bravo, charlie, delta]) client.drop();
  env.advance(LIMITS.graceMs - 1);
  assert.equal(rooms(), 1);
  env.advance(1);
  assert.equal(rooms(), 0);
  assert.equal(observer.call('session.join', { session }).code, 'not_found');
  assert.equal(observer.call('room.join', { room }).code, 'not_found');
});

test('an invitation outlives its invitee dropping, but not its room or its own lifetime', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, bravo, charlie, delta, room, session } = fullSquad(env);
  const echo = player(env, 'Echo');
  const foxtrot = player(env, 'Foxtrot');
  befriend(alpha, echo);
  befriend(alpha, foxtrot);
  assert.equal(alpha.call('invite.send', { session, to: [echo.account, foxtrot.account] }).code, 'ok');
  echo.drop();
  assert.equal(foxtrot.call('invite.list').invitations.length, 1);

  env.advance(LIMITS.inviteMs - 1000);
  assert.equal(foxtrot.call('invite.list').invitations.length, 1);
  env.advance(2000);
  assert.equal(foxtrot.call('invite.list').invitations.length, 0);

  assert.equal(alpha.call('invite.send', { session, to: [foxtrot.account] }).code, 'ok');
  assert.equal(foxtrot.call('invite.list').invitations.length, 1);
  for (const member of [alpha, bravo, charlie, delta]) assert.equal(member.call('room.leave', { room }).code, 'ok');
  assert.equal(env.host.sql.get('SELECT COUNT(*) AS n FROM invitations').n, 0);
  assert.equal(foxtrot.call('invite.list').invitations.length, 0);
});

test('the lobby has no idle timer of its own for a seat: only a closed socket starts its grace period', () => {
  const env = makeLobby({ log: () => {} });
  const { alpha, room } = fullSquad(env);
  env.advance(LIMITS.graceMs * 10);
  assert.equal(alpha.events('room.member_left').length, 0);
  assert.deepEqual(members(alpha, room), NAMES);
});
