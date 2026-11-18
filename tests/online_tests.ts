import assert from 'node:assert/strict';
import { test } from 'node:test';
import { registerHooks } from 'node:module';

// Vite imports the logo as an asset URL; the translation tests only need its module shape.
registerHooks({ load(url, context, nextLoad) {
  return url.includes('.svg?') ? { format: 'module', source: 'export default "logo.svg";', shortCircuit: true }
    : nextLoad(url, context);
} });

// Exercise the shared main-window / popup translation flow through a fake native bridge.
let receive: (event: { data: unknown }) => void;
const requests: { id: number; method: string; params: Record<string, string> }[] = [];
let deferred = false;
const reply = (request: typeof requests[number]) => receive({ data: {
  id: request.id, result: { text: request.params.text, from: 'en', to: 'zh', translation: `译文 ${request.params.provider} ${request.params.model}` },
} });
(globalThis as any).window = {
  setTimeout,
  chrome: { webview: {
    addEventListener: (_: string, listener: typeof receive) => { receive = listener; },
    postMessage: (request: typeof requests[number]) => {
      requests.push(request);
      if (!deferred) queueMicrotask(() => reply(request));
    },
  } },
};
(globalThis as any).document = { createElement: () => ({}) };
const { state } = await import('../web/state.ts');
const { fillTranslation, translationMode, translationHtml } = await import('../web/online.ts');

function section() {
  const nodes: Record<string, any> = {
    '.translation-body': { paragraph: undefined, replaceChildren(p: unknown) { this.paragraph = p; } },
    '.translation-direction': {}, '.translation-copy': { disabled: true },
  };
  return {
    nodes, dataset: {} as Record<string, string>,
    classList: { remove() {}, add() {} }, removeAttribute() {},
    querySelector: (name: string) => nodes[name],
  };
}

test('Bilibili is ready by default; NiuTrans alone requires credentials', () => {
  assert.equal(state.settings.translationProvider, 'bilibili');
  assert.equal(state.settings.bilibiliModel, 'Index-Translate-2B');
  state.niutrans.configured = false;
  assert.equal(translationMode('The weather is nice today.', true), 'translate');
  assert.equal(translationMode('weather', false), 'translate');
  assert.equal(translationMode('weather', true), null);
  assert.equal(translationMode('12345', false), null);
  assert.match(translationHtml('translate', ''), /哔哩哔哩翻译/);
  state.settings.translationProvider = 'niutrans';
  assert.equal(translationMode('The weather is nice today.', true), 'setup');
  assert.equal(translationMode('weather', false), null);
  state.niutrans.configured = true;
  assert.equal(translationMode('weather', false), 'translate');
  assert.match(translationHtml('translate', ''), /小牛翻译/);
  state.settings.translateSentences = false;
  assert.equal(translationMode('The weather is nice today.', false), null);
  state.settings.translateSentences = true;
  state.settings.translationProvider = 'bilibili';
});

test('shared translation cache distinguishes provider and model, and drops stale responses', async () => {
  const fill = async () => {
    const block = section();
    await fillTranslation(block as any, '  Hello world  ', () => true);
    return block;
  };
  const first = await fill();
  assert.equal(first.dataset.translation, '译文 bilibili Index-Translate-2B');
  assert.equal(first.nodes['.translation-copy'].disabled, false);
  assert.equal(requests.length, 1);
  assert.equal(requests[0].params.text, 'Hello world');
  await fill();
  assert.equal(requests.length, 1);
  state.settings.bilibiliModel = 'Index-Translate-9B';
  assert.equal((await fill()).dataset.translation, '译文 bilibili Index-Translate-9B');
  assert.equal(requests.length, 2);
  state.settings.translationProvider = 'niutrans';
  assert.equal((await fill()).dataset.translation, '译文 niutrans Index-Translate-9B');
  assert.equal(requests.length, 3);
  state.settings.translationProvider = 'bilibili';
  state.settings.bilibiliModel = 'Index-Translate-35B-A3B';
  deferred = true;
  const pendingBlock = section();
  const pending = fillTranslation(pendingBlock as any, 'Hello world', () => true);
  state.settings.bilibiliModel = 'Index-Translate-2B';
  reply(requests.at(-1)!);
  await pending;
  assert.equal(pendingBlock.dataset.translation, undefined);
  assert.equal(pendingBlock.nodes['.translation-copy'].disabled, true);
  assert.equal((await fill()).dataset.translation, '译文 bilibili Index-Translate-2B');
  assert.equal(requests.length, 4);
});
