// The Worker's Durable Object under a model of the Hibernation runtime: what reaches the lobby, and what an open socket
// keeps when the object is rebuilt.
import assert from 'node:assert/strict';
import { mock, test } from 'node:test';

import { PROTECTION } from '../src/abuse.js';
import { RELAY, encodeRelayFrame } from '../src/relay.js';
import { DurableObjectState } from './support/hibernation.js';

mock.timers.enable({ apis: ['Date'], now: 1_700_000_000_000 });

const STRIKES = PROTECTION.refusedRequests;
let accounts = 0;
let seq = 0;

const hello = (fields = {}) => JSON.stringify({ op: 'hello', seq: ++seq, proto: 1, game: 'Alienation', build: 'CUSA00062-1.00',
  account: String(5000 + ++accounts * 7919), secret: 'cd'.repeat(32), name: 'Tester', relay: true, ...fields });
const call = (op, body = {}) => JSON.stringify({ op, seq: ++seq, ...body });
const answers = (ws) => ws.sent.filter((m) => typeof m === 'string' && m.startsWith('{')).map((m) => JSON.parse(m));

async function lobby() {
  const state = new DurableObjectState();
  const object = await state.wake();
  const handled = [];
  const message = object.lobby.message.bind(object.lobby);
  object.lobby.message = (conn, text) => {
    handled.push(text);
    message(conn, text);
  };
  return { state, object, handled };
}

test('the heartbeat is answered by the runtime and never reaches the lobby, before or after sign-in', async () => {
  const { state, object, handled } = await lobby();
  assert.equal(state.autoResponse.request, 'ping');
  assert.equal(state.autoResponse.response, 'pong');
  const ws = await state.connect(object);
  for (let i = 0; i < 100; i++) state.deliver(object, ws, 'ping');
  assert.equal(handled.length, 0);
  assert.equal(ws.sent.filter((m) => m === 'pong').length, 100);
  assert.equal(ws.closedWith, null);
  state.deliver(object, ws, hello());
  assert.equal(answers(ws).at(-1).code, 'ok');
  for (let i = 0; i < 100; i++) state.deliver(object, ws, 'ping');
  assert.equal(handled.length, 1);
  assert.equal(object.lobby.sockets()[0].conn.keepaliveAt(), Date.now());
});

test('only the exact heartbeat is auto-answered: anything else is an ordinary message', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, 'PING');
  state.deliver(object, ws, JSON.stringify({ op: 'ping', seq: 1 }));
  assert.equal(handled.length, 2);
  assert.deepEqual(answers(ws).map((a) => a.code), ['bad_request', 'bad_request']);
});

test('an upgrade gives the socket an attachment and refusing a frame flood before sign-in writes nothing', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  assert.match(ws.deserializeAttachment().cid, /^[0-9a-f]{16}$/);
  const before = state.changes();
  for (let i = 0; i < 1000 && !ws.closedWith; i++) state.deliver(object, ws, call('worlds'));
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
  assert.equal(handled.length, PROTECTION.preHelloFrames + 1);
  assert.equal(state.changes(), before);
});

test('a socket keeps its pre-sign-in count when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  for (let i = 0; i < PROTECTION.preHelloFrames - 1; i++) state.deliver(object, ws, 'not json');
  const woken = await state.wake();
  state.deliver(woken, ws, 'not json');
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, 'not json');
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
});

test('a socket keeps its message budget and strikes when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, hello());
  let sent = 0;
  while (answers(ws).filter((a) => a.code === 'limit').length < STRIKES.burst - 1) {
    state.deliver(object, ws, call('worlds'));
    sent += 1;
  }
  const woken = await state.wake();
  const before = state.changes();
  state.deliver(woken, ws, call('worlds'));
  assert.equal(answers(ws).at(-1).code, 'limit');
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, call('worlds'));
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'too many refused requests' });
  assert.ok(sent < 100);
  assert.equal(state.changes(), before);
});

test('a socket keeps its relay strikes when the object is rebuilt', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, hello());
  const junk = new Uint8Array([1, 2, 3]).buffer;
  for (let i = 0; i < RELAY.malformed.burst - 1; i++) state.deliver(object, ws, junk);
  const woken = await state.wake();
  state.deliver(woken, ws, junk);
  assert.equal(ws.closedWith, null);
  state.deliver(woken, ws, junk);
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'relay abuse' });
});

test('binary frames before sign-in share the pre-sign-in count with text', async () => {
  const { state, object } = await lobby();
  const ws = await state.connect(object);
  const frame = encodeRelayFrame(1, 0x20, 1, new Uint8Array(40));
  state.deliver(object, ws, 'not json');
  for (let i = 0; i < PROTECTION.preHelloFrames - 1; i++) state.deliver(object, ws, frame.buffer);
  assert.equal(ws.closedWith, null);
  state.deliver(object, ws, frame.buffer);
  assert.deepEqual(ws.closedWith, { code: 1008, reason: 'sign in first' });
});

test('a text frame over the size limit is closed by the Worker before the lobby sees it', async () => {
  const { state, object, handled } = await lobby();
  const ws = await state.connect(object);
  state.deliver(object, ws, 'x'.repeat(1024 * 1024));
  assert.equal(ws.closedWith?.code, 1009);
  assert.equal(handled.length, 0);
});
