#!/usr/bin/env node
// Makes test instances friends the way two players do: one sends a friend request by friend code, the other accepts it.
// Each instance signs in with its own online_identity.ini and presence off, so nobody appears online and a running game
// of that instance keeps its own connection. Nothing here is special to the lobby: it is the normal friend protocol.
//   node tools/online-friends.mjs --lobby <wss://...> --player Alpha=<identity.ini> --player Bravo=<identity.ini> [...]
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';

const GAME = 'Alienation';
const LOBBY_PROTOCOL = 1;
const ONLINE_PROTOCOL = 1;
const TIMEOUT_MS = 10000;

export function readIdentity(file) {
  const values = {};
  for (const line of readFileSync(file, 'utf8').split(/\r?\n/)) {
    const at = line.indexOf('=');
    if (at > 0 && !line.startsWith(';') && !line.startsWith('#')) values[line.slice(0, at).trim()] = line.slice(at + 1).trim();
  }
  if (!/^[1-9][0-9]{0,19}$/.test(values.account ?? '') || !/^[0-9a-f]{64}$/.test(values.secret ?? '')) {
    throw new Error(`${file} is not an online identity`);
  }
  return { account: values.account, secret: values.secret };
}

class Session {
  static async open(url, name, identity) {
    const s = new Session();
    s.name = name;
    s.account = identity.account;
    s.seq = 0;
    s.pending = new Map();
    s.ws = new WebSocket(url);
    s.ws.addEventListener('message', (e) => {
      if (typeof e.data !== 'string' || e.data === 'pong') return;
      const msg = JSON.parse(e.data);
      const waiter = msg.re !== undefined ? s.pending.get(msg.re) : undefined;
      if (waiter) s.pending.delete(msg.re), waiter(msg);
    });
    await new Promise((resolve, reject) => {
      s.ws.addEventListener('open', resolve, { once: true });
      s.ws.addEventListener('error', () => reject(new Error(`${name}: cannot reach ${url}`)), { once: true });
    });
    const hello = await s.call('hello', { proto: LOBBY_PROTOCOL, game: GAME, account: identity.account, secret: identity.secret,
      name, presence: false, online_protocol: ONLINE_PROTOCOL, version: 'online-friends', platform: 'test-tool' });
    s.code = hello.friend_code;
    return s;
  }

  call(op, body = {}) {
    const seq = ++this.seq;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`${this.name}: ${op} timed out`)), TIMEOUT_MS);
      this.pending.set(seq, (msg) => {
        clearTimeout(timer);
        if (msg.code === 'ok') resolve(msg);
        else reject(new Error(`${this.name}: ${op} refused (${msg.code}${msg.error ? `: ${msg.error}` : ''})`));
      });
      this.ws.send(JSON.stringify({ op, seq, ...body }));
    });
  }

  close() {
    const closed = new Promise((resolve) => this.ws.addEventListener('close', resolve, { once: true }));
    this.ws.close();
    return closed;
  }
}

// Every pair becomes friends: a request from the first, accepted by the second. Returns one line per pair.
export async function makeFriends({ lobby, players, log = console.log }) {
  if (players.length < 2) throw new Error('at least two players are needed');
  const sessions = [];
  try {
    for (const p of players) sessions.push(await Session.open(lobby, p.name, p.identity));
    for (const s of sessions) log(`Friend code of ${s.name}: ${s.code}`);
    const lines = [];
    for (let i = 0; i < sessions.length; ++i) {
      for (let j = i + 1; j < sessions.length; ++j) {
        const [a, b] = [sessions[i], sessions[j]];
        const sent = await a.call('friends.add', { code: b.code });
        if (sent.state !== 'friends') await b.call('friends.accept', { account: a.account });
        const line = sent.state === 'friends' ? `${a.name} and ${b.name} are friends` :
          `${a.name} and ${b.name} are friends (${a.name} sent the request, ${b.name} accepted it)`;
        log(line);
        lines.push(line);
      }
    }
    return lines;
  } finally {
    await Promise.all(sessions.map((s) => s.close()));
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({ options: { lobby: { type: 'string' }, player: { type: 'string', multiple: true, default: [] } } });
  try {
    if (!/^wss?:\/\/\S+$/.test(values.lobby ?? '')) throw new Error('--lobby needs a ws:// or wss:// address');
    const players = values.player.map((spec) => {
      const at = spec.indexOf('=');
      if (at < 1) throw new Error(`--player ${spec}: expected Name=identity.ini`);
      return { name: spec.slice(0, at), identity: readIdentity(spec.slice(at + 1)) };
    });
    await makeFriends({ lobby: values.lobby, players });
  } catch (error) {
    console.error(`Friends: ${error.message}`);
    process.exit(1);
  }
}
