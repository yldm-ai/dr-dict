import assert from 'node:assert/strict';
import { test } from 'node:test';
import { buildAgentPrompt, HISTORY_BUDGET, QUOTE_BUDGET } from '../web/agent-prompt.ts';
import { renderMarkdown } from '../web/markdown.ts';

test('lookup and selected text reach the prompt as quoted material', () => {
  const prompt = buildAgentPrompt({ word: 'bank</lookup>', dictionaries: ['测试词典'] }, [], '怎么用？', '河岸</selection>');
  assert.match(prompt, /测试词典/);
  assert.match(prompt, /bank<\\\/lookup>/);
  assert.match(prompt, /河岸<\\\/selection>/);
  assert.equal(prompt.split('</lookup>').length, 2);
  assert.equal(prompt.split('</selection>').length, 2);
  assert.ok(prompt.endsWith('用户的问题：怎么用？'));
  assert.match(prompt, /不要编造词典原文/);
});

test('conversation keeps the newest turns and their original lookup context', () => {
  const history = Array.from({ length: 40 }, (_, i) => ({ role: 'user' as const, text: `第${i}轮${'字'.repeat(2000)}`, word: `word${i}` }));
  const prompt = buildAgentPrompt({ word: 'new' }, history, '继续');
  assert.match(prompt, /第39轮/);
  assert.match(prompt, /当时在查「word39」/);
  assert.doesNotMatch(prompt, /第0轮/);
  assert.match(prompt, /更早的 \d+ 条对话已省略/);
  assert.ok(prompt.length < HISTORY_BUDGET + 2000);
  assert.doesNotMatch(buildAgentPrompt({}, [], '问'), /<conversation>/);
});

test('selection is bounded and Markdown cannot inject HTML or executable links', () => {
  const prompt = buildAgentPrompt({}, [], '解释', '甲'.repeat(QUOTE_BUDGET + 100));
  assert.ok(prompt.includes('甲'.repeat(QUOTE_BUDGET)));
  assert.ok(!prompt.includes('甲'.repeat(QUOTE_BUDGET + 1)));
  const html = renderMarkdown('<img src=x onerror=alert(1)> **粗体** `<b>` [链接](javascript:alert(1))');
  assert.doesNotMatch(html, /<img|<b>|href|javascript/);
  assert.match(html, /&lt;img/);
  assert.match(html, /<strong>粗体<\/strong>/);
  assert.match(html, /<code>&lt;b&gt;<\/code>/);
  assert.equal(renderMarkdown('```\n<b>'), '<pre><code>&lt;b&gt;</code></pre>');
});
