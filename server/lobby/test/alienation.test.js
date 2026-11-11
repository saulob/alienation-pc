// What this lobby adds for Alienation: build separation, the launcher's room list and the classic Matching2 reads.
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { BACKEND_VERSION, FLAG_HIDDEN, GAME, LIMITS } from '../src/lobby.js';
import { encodeRelayFrame, roomToken } from '../src/relay.js';
import { ROOM, makeLobby } from './harness.js';

const EU = 'CUSA00062-1.00';
const KR = 'CUSA04430-1.00';
const PUBLIC = 0x04000000;
const HIDDEN = 0x14000000;

let nextIp = 10;
function greeted(env, fields = {}) {
  const client = env.connect(`198.51.100.${nextIp++}`);
  assert.equal(client.hello({ relay: true, ...fields }).code, 'ok');
  return client;
}

function host(env, fields = {}, flags = PUBLIC) {
  const client = greeted(env, fields);
  const created = client.call('room.create', { ...ROOM, flags });
  assert.equal(created.code, 'ok');
  return { client, room: created.room };
}

test('the service names this game and its backend version', () => {
  const env = makeLobby({ log: () => {} });
  const reply = env.connect().hello();
  assert.equal(GAME, 'Alienation');
  assert.equal(reply.backend, BACKEND_VERSION);
  assert.deepEqual(env.lobby.metricsSummary().backend, BACKEND_VERSION);
});

test('rooms.list shows public rooms of the reader build only, with no address, key or code', () => {
  const env = makeLobby({ log: () => {} });
  const eu = host(env, { name: 'Host1', version: '0.2.0' });
  host(env, { name: 'Hidden' }, HIDDEN);
  host(env, { name: 'Korea', build: KR });
  const reader = greeted(env);
  const list = reader.call('rooms.list');
  assert.equal(list.code, 'ok');
  assert.deepEqual([list.total, list.incompatible, list.rooms.length], [1, 1, 1]);
  const [row] = list.rooms;
  assert.deepEqual(Object.keys(row).sort(), ['host', 'ints', 'joinable', 'minutes', 'players', 'slots', 'version']);
  assert.deepEqual([row.host, row.players, row.slots, row.joinable, row.version], ['Host1', 1, 4, true, '0.2.0']);
  const text = JSON.stringify(list);
  for (const secret of [eu.room.key, eu.room.code, eu.room.id, '198.51.100']) assert.ok(!text.includes(secret), secret);
  assert.deepEqual(reader.call('rooms.list', { rev: list.rev }).unchanged, true);
  const kr = greeted(env, { build: KR });
  assert.deepEqual(kr.call('rooms.list').rooms.map((r) => r.host), ['Korea']);
});

test('rooms.list counts a full room as not joinable and a launcher without a build sees none', () => {
  const env = makeLobby({ log: () => {} });
  const { room } = host(env, { name: 'Full' });
  for (let i = 0; i < 3; i++) assert.equal(greeted(env).call('room.join', { room: room.id }).code, 'ok');
  const list = greeted(env).call('rooms.list');
  assert.deepEqual(list.rooms.map((r) => [r.players, r.joinable]), [[4, false]]);
  const launcher = greeted(env, { build: undefined });
  assert.deepEqual([launcher.call('rooms.list').rooms.length, launcher.call('rooms.list').incompatible], [0, 1]);
});

test('rooms.list has its own budget and a bounded answer', () => {
  const env = makeLobby({ log: () => {} });
  for (let i = 0; i < LIMITS.roomList + 3; i++) host(env);
  const reader = greeted(env);
  const first = reader.call('rooms.list');
  assert.deepEqual([first.total, first.rooms.length], [LIMITS.roomList + 3, LIMITS.roomList]);
  let refused = null;
  for (let i = 0; i < 10 && !refused; i++) {
    const answer = reader.call('rooms.list');
    if (answer.code !== 'ok') refused = answer.code;
  }
  assert.equal(refused, 'limit');
  assert.equal(reader.call('room.search', { world: 1 }).code, 'ok');
});

test('room.external and room.members read a findable or own room, never a hidden or other-build one', () => {
  const env = makeLobby({ log: () => {} });
  const open = host(env, { name: 'Open' });
  const hidden = host(env, { name: 'Secret' }, HIDDEN);
  const kr = host(env, { name: 'Korea', build: KR });
  const reader = greeted(env, { name: 'Reader' });
  const ext = reader.call('room.external', { rooms: [open.room.id, hidden.room.id, kr.room.id] });
  assert.deepEqual(ext.rooms.map((r) => r.owner_name), ['Open']);
  assert.ok(!('key' in ext.rooms[0]) && !('members' in ext.rooms[0]));
  assert.deepEqual(reader.call('room.members', { room: open.room.id }).members, [{ id: 0x10, name: 'Open', owner: true }]);
  assert.equal(reader.call('room.members', { room: hidden.room.id }).code, 'not_found');
  assert.equal(reader.call('room.members', { room: kr.room.id }).code, 'not_found');
  assert.deepEqual(hidden.client.call('room.external', { rooms: [hidden.room.id] }).rooms.map((r) => r.owner_name), ['Secret']);
  assert.equal(reader.call('room.external', { rooms: [] }).code, 'bad_request');
});

test('a hidden room becomes findable once its owner makes it public, as the game does after creating it', () => {
  const env = makeLobby({ log: () => {} });
  const { client, room } = host(env, {}, HIDDEN);
  const reader = greeted(env);
  assert.equal(reader.call('room.search', { world: 1, flag_mask: 0x60000000 }).total, 0);
  assert.equal(client.call('room.set_internal', { room: room.id, flag_mask: 0x10000000 | PUBLIC, flag_value: PUBLIC }).code, 'ok');
  assert.equal(reader.call('room.search', { world: 1, flag_mask: 0x60000000 }).total, 1);
  assert.equal(client.call('room.set_internal', { room: room.id, flag_mask: 0x10000000 | PUBLIC, flag_value: HIDDEN }).code, 'ok');
  assert.equal(reader.call('room.search', { world: 1, flag_mask: 0x60000000 }).total, 0);
  assert.ok(env.lobby.roomRow(Number(room.id)).flags & FLAG_HIDDEN);
});

test('the relay never joins two builds, even inside one room', () => {
  const env = makeLobby({ log: () => {} });
  const { client: eu, room } = host(env);
  const guest = greeted(env);
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  const frame = encodeRelayFrame(roomToken(room.key), 0x20, 1, Uint8Array.of(1, 2, 3));
  eu.relay(frame);
  assert.equal(guest.binary.length, 1);
  const seat = env.host.sockets().find((c) => c.attachment().account === guest.account);
  seat.setAttachment({ ...seat.attachment(), build: KR });
  env.lobby.relays.forgetConnection(eu.conn.attachment().cid);
  eu.relay(frame);
  assert.equal(guest.binary.length, 1);
});

test('a friend in a room shows the build of that room', () => {
  const env = makeLobby({ log: () => {} });
  const a = greeted(env, { name: 'A' });
  const b = greeted(env, { name: 'B' });
  const added = b.call('friends.add', { code: env.lobby.accountRow(a.account).code });
  assert.equal(added.state, 'pending');
  assert.equal(a.call('friends.accept', { account: b.account }).code, 'ok');
  assert.equal(a.call('room.create', { ...ROOM, flags: PUBLIC }).code, 'ok');
  const [friend] = b.call('friends.list').friends;
  assert.deepEqual([friend.name, friend.online, friend.build, Boolean(friend.room), friend.joinable], ['A', true, EU, true, true]);
  const c = greeted(env, { name: 'C', build: KR });
  assert.equal(c.call('friends.add', { code: env.lobby.accountRow(a.account).code }).state, 'pending');
  assert.equal(a.call('friends.accept', { account: c.account }).code, 'ok');
  assert.equal(c.call('friends.list').friends[0].joinable, false);
});

test('a joiner whose online id is already a member of the room is refused', () => {
  const env = makeLobby({ log: () => {} });
  const { room } = host(env, { name: 'Saulo' });
  assert.equal(greeted(env, { name: 'saulo' }).call('room.join', { room: room.id }).code, 'name_taken');
  assert.equal(greeted(env, { name: 'Other' }).call('room.join', { room: room.id }).code, 'ok');
});

test('refused joins and relayed traffic are counted in memory, then written with the diagnostics', () => {
  const env = makeLobby({ log: () => {} });
  const { client: eu, room } = host(env);
  const kr = greeted(env, { build: KR });
  assert.equal(kr.call('room.join', { room: room.id }).code, 'incompatible');
  assert.equal(kr.call('room.join', { room: '999999' }).code, 'not_found');
  const guest = greeted(env);
  assert.equal(guest.call('room.join', { room: room.id }).code, 'ok');
  eu.relay(encodeRelayFrame(roomToken(room.key), 0x20, 1, new Uint8Array(100)));
  assert.equal(guest.call('room.leave', { room: room.id }).code, 'ok');
  const rows = () => env.host.sql.all(`SELECT dimension, value, count FROM metrics_daily_dimensions
    WHERE dimension IN ('join_failed', 'relay', 'activity') ORDER BY dimension, value`);
  assert.deepEqual(rows(), []);
  env.advance(16 * 60 * 1000);
  assert.deepEqual(rows(), [
    { dimension: 'activity', value: 'room_leaves', count: 1 },
    { dimension: 'join_failed', value: 'incompatible', count: 1 },
    { dimension: 'join_failed', value: 'not_found', count: 1 },
    { dimension: 'relay', value: 'bytes', count: 100 },
    { dimension: 'relay', value: 'frames', count: 1 },
  ]);
});
