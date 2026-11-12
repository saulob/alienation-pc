// tools/online-friends.mjs, the A/B test tool: two test instances become friends through the normal friend request and
// accept, the second run changes nothing, and nobody is shown online by it.
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, before, test } from 'node:test';

import { makeFriends, readIdentity } from '../../../tools/online-friends.mjs';
import { startLobbyServer } from '../local/server.js';

let server;
let url;
let dir;

before(async () => {
  server = await startLobbyServer({ port: 0 });
  url = `ws://127.0.0.1:${server.port}/`;
  dir = mkdtempSync(join(tmpdir(), 'aln-online-friends-'));
});

after(async () => {
  await server.close();
  rmSync(dir, { recursive: true, force: true });
});

function identity(name, account, byte) {
  const file = join(dir, `${name}.ini`);
  writeFileSync(file, `; Online identity\naccount=${account}\nsecret=${byte.repeat(32)}\n`);
  return { name, identity: readIdentity(file) };
}

async function signIn(player, presence = true) {
  const ws = new WebSocket(url);
  const messages = [];
  ws.addEventListener('message', (e) => messages.push(JSON.parse(e.data)));
  await new Promise((resolve) => ws.addEventListener('open', resolve, { once: true }));
  let seq = 0;
  const call = async (op, body = {}) => {
    const id = ++seq;
    ws.send(JSON.stringify({ op, seq: id, ...body }));
    for (let i = 0; i < 100; ++i) {
      const reply = messages.find((m) => m.re === id);
      if (reply) return reply;
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error(`${op} timed out`);
  };
  const hello = await call('hello', { proto: 1, game: 'Alienation', build: 'CUSA04430-1.00', account: player.identity.account,
    secret: player.identity.secret, name: player.name, presence });
  const close = () => {
    const closed = new Promise((resolve) => ws.addEventListener('close', resolve, { once: true }));
    ws.close();
    return closed;
  };
  return { call, code: hello.friend_code, close };
}

test('Alpha and Bravo become friends by request and accept, once', async () => {
  const alpha = identity('Alpha', '1001', 'a1');
  const bravo = identity('Bravo', '1002', 'b2');
  const lines = [];
  const made = await makeFriends({ lobby: url, players: [alpha, bravo], log: (line) => lines.push(line) });
  assert.deepEqual(made, ['Alpha and Bravo are friends (Alpha sent the request, Bravo accepted it)']);
  assert.ok(lines.some((l) => /^Friend code of Alpha: \w{4}-\w{4}$/.test(l)));

  const game = await signIn(alpha);
  const friends = await game.call('friends.list');
  assert.deepEqual(friends.friends.map((f) => [f.name, f.online]), [['Bravo', false]]);
  const requests = await game.call('friends.requests');
  assert.deepEqual([requests.incoming.length, requests.outgoing.length], [0, 0]);
  await game.close();

  const again = await makeFriends({ lobby: url, players: [alpha, bravo], log: () => {} });
  assert.deepEqual(again, ['Alpha and Bravo are friends']);
});

test('a request already sent the other way is completed, and four players all become friends', async () => {
  const players = [identity('Charlie', '2001', 'c3'), identity('Delta', '2002', 'd4'), identity('Echo', '2003', 'e5'),
    identity('Foxtrot', '2004', 'f6')];
  const charlie = await signIn(players[0], false);
  const delta = await signIn(players[1], false);
  assert.equal((await delta.call('friends.add', { code: charlie.code })).state, 'pending');
  await charlie.close();
  await delta.close();
  const made = await makeFriends({ lobby: url, players, log: () => {} });
  assert.equal(made.length, 6);
  assert.equal(made[0], 'Charlie and Delta are friends');
  const check = await signIn(players[3]);
  assert.deepEqual((await check.call('friends.list')).friends.map((f) => f.name).sort(), ['Charlie', 'Delta', 'Echo']);
  await check.close();
});

test('an identity file that is not one is refused before anything is sent', () => {
  const file = join(dir, 'broken.ini');
  writeFileSync(file, 'account=0\nsecret=xyz\n');
  assert.throws(() => readIdentity(file), /not an online identity/);
});
