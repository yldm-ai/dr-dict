// AI 助查: a panel at the right of the main window where a local agent CLI (Codex, Claude Code or
// opencode) answers questions about the word or sentence being looked up, or about text selected
// in an article. Adapted from DR Reader's web/agent-chat.ts: the same request flow (agent.ask,
// streamed `agent` events, agent.cancel) and Markdown rendering; conversations stay in memory
// instead of being saved per book.
//
// The panel opens from the top bar button (right of the star), the article menu's 问 AI and
// Ctrl+J. Unpinned it lies over the content and hides itself on a click elsewhere (an article
// included) or Esc; pinned it takes its own column and stays.
import { $, call, escape, on, toast } from './bridge.ts';
import { icon } from './icons.ts';
import { renderMarkdown } from './markdown.ts';
import { buildAgentPrompt, QUOTE_BUDGET, type ChatTurn, type LookupContext } from './agent-prompt.ts';
import { publish, state, type AgentId, type Settings } from './state.ts';

type Message = { role: 'user' | 'assistant'; text: string; quote?: string; word?: string; label?: string; stopped?: boolean };
type AgentEvent = { request?: string; type?: 'text' | 'status' | 'done' | 'error'; text?: string };
type Active = {
  request: string;
  question: string;
  quote: string;
  word: string;
  answer: string;
  label: string;
  turn: HTMLElement;
  body: HTMLElement;
  meta: HTMLElement;
  frame: number;
  stopped: boolean;
};

export interface AgentPanelHost {
  context(): LookupContext;
}

export class AgentPanel {
  private readonly root = $('ask-panel');
  private readonly main = document.querySelector<HTMLElement>('.main')!;
  private readonly button = $<HTMLButtonElement>('ask-button');
  private thread!: HTMLElement;
  private scroller!: HTMLElement;
  private input!: HTMLTextAreaElement;
  private send!: HTMLButtonElement;
  private quoteBox!: HTMLElement;
  private contextLine!: HTMLElement;
  private pin!: HTMLButtonElement;
  private messages: Message[] = [];
  private quote = '';
  private active: Active | null = null;

  constructor(private readonly host: AgentPanelHost) {
    this.root.innerHTML = `<header class="ask-head">
        <span class="ask-title">${icon('sparkle')}<span>问 AI</span></span>
        <button type="button" class="icon-button small ask-reset" title="新对话">${icon('plus')}</button>
        <button type="button" class="icon-button small ask-pin" aria-pressed="false">${icon('pin')}</button>
        <button type="button" class="icon-button small ask-close" title="关闭 (Esc)">${icon('x')}</button>
      </header>
      <div class="ask-body"><div class="ask-thread" aria-live="polite"></div></div>
      <form class="ask-form">
        <div class="ask-context"></div>
        <div class="ask-quote" hidden><span class="ask-quote-text"></span><button type="button" class="icon-button small ask-quote-remove" title="移除引用">${icon('x')}</button></div>
        <textarea class="ask-input" rows="3" maxlength="4000" placeholder="问问这个词…" aria-label="向 AI 提问"></textarea>
        <div class="ask-actions"><span class="ask-hint">Enter 发送 · Shift+Enter 换行</span><button type="submit" class="ask-send" title="发送" aria-label="发送">${icon('arrow-up')}</button></div>
      </form>`;
    this.scroller = this.root.querySelector('.ask-body')!;
    this.thread = this.root.querySelector('.ask-thread')!;
    this.input = this.root.querySelector('.ask-input')!;
    this.send = this.root.querySelector('.ask-send')!;
    this.quoteBox = this.root.querySelector('.ask-quote')!;
    this.contextLine = this.root.querySelector('.ask-context')!;
    this.pin = this.root.querySelector('.ask-pin')!;

    this.root.querySelector<HTMLFormElement>('.ask-form')!.addEventListener('submit', (event) => {
      event.preventDefault();
      if (this.active) this.stop();
      else void this.ask(this.input.value);
    });
    this.input.addEventListener('keydown', (event) => {
      if (event.key !== 'Enter' || event.shiftKey || event.isComposing) return;
      event.preventDefault();
      if (!this.active) void this.ask(this.input.value);
    });
    this.root.querySelector('.ask-quote-remove')!.addEventListener('click', () => this.attach(''));
    this.root.querySelector('.ask-reset')!.addEventListener('click', () => {
      this.reset();
      this.input.focus();
    });
    this.root.querySelector('.ask-close')!.addEventListener('click', () => this.close());
    this.pin.addEventListener('click', () => void this.setPinned(!state.settings.agentPinned));
    this.thread.addEventListener('click', (event) => {
      const target = event.target as HTMLElement;
      const prompt = target.closest<HTMLElement>('[data-prompt]');
      if (prompt) void this.ask(prompt.dataset.prompt!);
    });
    this.button.addEventListener('click', () => this.toggle());
    on('agent', (message) => this.receive(message as AgentEvent));

    // Auto-hide: a click outside the panel, or into an article frame (the window loses focus to
    // the frame), closes an unpinned panel. Switching to another program does not.
    document.addEventListener(
      'pointerdown',
      (event) => {
        const target = event.target as HTMLElement;
        if (this.isOpen() && !state.settings.agentPinned && !target.closest('#ask-panel, #ask-button, .context-menu, dialog'))
          this.close();
      },
      true,
    );
    window.addEventListener('blur', () => {
      setTimeout(() => {
        if (this.isOpen() && !state.settings.agentPinned && document.activeElement instanceof HTMLIFrameElement) this.close();
      });
    });
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && this.isOpen() && !state.settings.agentPinned && !document.querySelector('dialog[open]')) {
        event.preventDefault();
        event.stopPropagation();
        this.close();
      }
    });
    this.renderEmpty();
    this.apply();
  }

  /** Follows the settings: the feature switch, the top bar button and the pin. */
  apply(): void {
    const s = state.settings;
    this.button.hidden = !s.agentEnabled || !s.agentButton;
    if (!s.agentEnabled) {
      if (this.active) this.stop();
      this.close();
    }
    this.pin.setAttribute('aria-pressed', String(s.agentPinned));
    this.pin.title = s.agentPinned ? '取消固定（点击别处时自动隐藏）' : '固定在右侧';
    this.pin.innerHTML = icon(s.agentPinned ? 'pin-off' : 'pin');
    this.main.classList.toggle('ask-docked', this.isOpen() && s.agentPinned);
  }

  isOpen(): boolean {
    return !this.root.hidden;
  }

  open(): void {
    if (!state.settings.agentEnabled) {
      toast('AI 助查没有开启，可在“设置 → AI 助查”中打开');
      return;
    }
    this.root.hidden = false;
    this.root.inert = false;
    this.button.setAttribute('aria-pressed', 'true');
    this.refreshContext();
    this.apply();
    this.input.focus({ preventScroll: true });
  }

  close(): void {
    if (this.root.hidden) return;
    this.root.hidden = true;
    this.root.inert = true;
    this.button.setAttribute('aria-pressed', 'false');
    this.apply();
  }

  toggle(): void {
    if (this.isOpen()) this.close();
    else this.open();
  }

  /** The article menu's 问 AI: opens the panel with the selection attached to the next question. */
  askAbout(text: string): void {
    this.open();
    if (!this.isOpen()) return;
    this.attach(text);
  }

  /** The shown word changed: the next question is about it. */
  refreshContext(): void {
    const word = this.host.context().word?.trim() ?? '';
    const short = word.length > 40 ? `${word.slice(0, 40)}…` : word;
    this.contextLine.textContent = word ? `正在查：${short}` : '';
    this.contextLine.hidden = !word;
    this.input.placeholder = word ? `问问“${short}”…` : '问点什么…';
    if (!this.messages.length && !this.active) this.renderEmpty();
  }

  private async setPinned(pinned: boolean): Promise<void> {
    state.settings = { ...state.settings, agentPinned: pinned };
    this.apply();
    try {
      state.settings = await call<Settings>('settings.save', { agentPinned: pinned });
      publish('settings');
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  }

  private attach(quote: string): void {
    this.quote = quote.trim().slice(0, QUOTE_BUDGET);
    this.quoteBox.hidden = !this.quote;
    this.quoteBox.querySelector('.ask-quote-text')!.textContent = this.quote;
  }

  /** Starts a new conversation. */
  reset(): void {
    if (this.active) this.stop();
    this.messages = [];
    this.attach('');
    this.renderEmpty();
  }

  private renderEmpty(): void {
    const word = this.host.context().word?.trim() ?? '';
    const sentence = word.length > 24 || /\s\S+\s/.test(word);
    const prompts = !word
      ? []
      : sentence
        ? ['分析这句话的语法结构', '逐个解释这句话里的难点词语', '给出两种更地道的译法']
        : [`解释“${word}”的意思和常见用法`, `给出几个“${word}”的例句并翻译`, `“${word}”和它的近义词有什么区别？`];
    this.thread.innerHTML = `<p class="ask-empty">回答由本机的 Codex、Claude Code 或 opencode 生成。可以直接提问，也可以在词条中选中文字后右键“问 AI”。</p>${
      prompts.length
        ? `<div class="ask-prompts">${prompts.map((p) => `<button type="button" class="ask-prompt" data-prompt="${escape(p)}">${escape(p)}</button>`).join('')}</div>`
        : ''
    }`;
  }

  private agent(): { agent: string; model: string; effort: string } {
    const s = state.settings;
    if (s.agent === 'auto') return { agent: 'auto', model: '', effort: '' };
    return { agent: s.agent, model: s.agentModels?.[s.agent as AgentId] ?? '', effort: s.agentEfforts?.[s.agent as AgentId] ?? '' };
  }

  private turns(): ChatTurn[] {
    return this.messages.map((m): ChatTurn =>
      m.role === 'user'
        ? { role: 'user', text: m.text, quote: m.quote || undefined, word: m.word || undefined }
        : { role: 'assistant', text: m.stopped ? `${m.text}\n（回答被用户中途停止）` : m.text },
    );
  }

  async ask(question: string): Promise<void> {
    question = question.trim();
    if (!state.settings.agentEnabled || !question || this.active) return;
    const context = this.host.context();
    const quote = this.quote;
    const prompt = buildAgentPrompt(context, this.turns(), question, quote);
    if (!this.messages.length) this.thread.replaceChildren();
    this.thread.append(this.userTurn(question, quote));
    const turn = document.createElement('div');
    turn.className = 'ask-turn ask-answer';
    turn.innerHTML = '<div class="ask-answer-body markdown"></div><div class="ask-meta">正在思考…</div>';
    this.thread.append(turn);
    const request = crypto.randomUUID();
    this.active = {
      request,
      question,
      quote,
      word: context.word ?? '',
      answer: '',
      label: '',
      turn,
      frame: 0,
      stopped: false,
      body: turn.querySelector('.ask-answer-body')!,
      meta: turn.querySelector('.ask-meta')!,
    };
    this.input.value = '';
    this.attach('');
    this.setBusy(true);
    this.reveal(turn);
    try {
      const started = await call<{ agent: AgentId; label: string }>('agent.ask', { request, prompt, ...this.agent() });
      if (this.active?.request === request) {
        this.active.label = started.label;
        if (!this.active.answer) this.active.meta.textContent = `${started.label} 正在思考…`;
      }
    } catch (error) {
      if (this.active?.request === request) this.fail((error as Error).message);
    }
  }

  stop(): void {
    const active = this.active;
    if (!active) return;
    active.stopped = true;
    call('agent.cancel', { request: active.request }).catch(() => {});
    this.complete(active, true);
  }

  private receive(message: AgentEvent): void {
    const active = this.active;
    if (!active || message.request !== active.request) return;
    const text = String(message.text ?? '');
    if (message.type === 'text') {
      active.answer += text;
      // Re-render once per frame however fast the deltas arrive.
      active.frame ||= requestAnimationFrame(() => {
        active.frame = 0;
        active.body.innerHTML = renderMarkdown(active.answer);
        active.meta.textContent = `${active.label || 'AI'} 正在回答…`;
        this.reveal(active.turn, true);
      });
    } else if (message.type === 'status') active.meta.textContent = text;
    else if (message.type === 'done') this.complete(active, false);
    else if (message.type === 'error') this.fail(text);
  }

  private complete(active: Active, stopped: boolean): void {
    cancelAnimationFrame(active.frame);
    this.active = null;
    this.setBusy(false);
    if (!active.answer.trim()) {
      // Nothing to keep: a stop before the first word leaves no turn behind.
      active.turn.previousElementSibling?.remove();
      active.turn.remove();
      if (!this.messages.length) this.renderEmpty();
      return;
    }
    const answer: Message = { role: 'assistant', text: active.answer, label: active.label, stopped };
    this.messages.push({ role: 'user', text: active.question, quote: active.quote || undefined, word: active.word }, answer);
    active.turn.replaceWith(this.answerTurn(answer));
  }

  private fail(message: string): void {
    const active = this.active;
    if (!active) return;
    cancelAnimationFrame(active.frame);
    this.active = null;
    this.setBusy(false);
    if (active.stopped) return;
    active.body.innerHTML = active.answer ? renderMarkdown(active.answer) : '';
    active.turn.classList.add('ask-failed');
    active.meta.innerHTML = `<span class="ask-error">${escape(message || '没有得到回答')}</span>`;
    const retry = document.createElement('button');
    retry.type = 'button';
    retry.className = 'ask-link';
    retry.textContent = '重试';
    retry.onclick = () => {
      // The failed exchange was never kept; ask it again in its place.
      active.turn.previousElementSibling?.remove();
      active.turn.remove();
      if (!this.messages.length) this.renderEmpty();
      this.attach(active.quote);
      void this.ask(active.question);
    };
    active.meta.append(retry);
  }

  private userTurn(question: string, quote: string): HTMLElement {
    const turn = document.createElement('div');
    turn.className = 'ask-turn ask-user';
    if (quote) {
      const passage = document.createElement('blockquote');
      passage.className = 'ask-turn-quote';
      passage.textContent = quote;
      turn.append(passage);
    }
    const text = document.createElement('p');
    text.textContent = question;
    turn.append(text);
    return turn;
  }

  /** A finished answer: its Markdown, which CLI wrote it, and a copy button. */
  private answerTurn(message: Message): HTMLElement {
    const turn = document.createElement('div');
    turn.className = 'ask-turn ask-answer';
    const body = document.createElement('div');
    body.className = 'ask-answer-body markdown';
    body.innerHTML = renderMarkdown(message.text);
    const meta = document.createElement('div');
    meta.className = 'ask-meta';
    const label = document.createElement('span');
    label.textContent = `${message.label || 'AI'}${message.stopped ? ' · 已停止' : ''}`;
    const copy = document.createElement('button');
    copy.type = 'button';
    copy.className = 'ask-link';
    copy.textContent = '复制';
    copy.onclick = () =>
      navigator.clipboard.writeText(message.text).then(
        () => toast('已复制回答', 'success'),
        () => toast('复制失败', 'error'),
      );
    meta.append(label, copy);
    turn.append(body, meta);
    return turn;
  }

  private setBusy(busy: boolean): void {
    this.send.innerHTML = icon(busy ? 'stop' : 'arrow-up');
    this.send.setAttribute('aria-label', busy ? '停止回答' : '发送');
    this.send.title = busy ? '停止回答' : '发送';
    this.root.dataset.busy = String(busy);
  }

  /** Keeps the newest answer in view while it streams, unless the user scrolled up. */
  private reveal(turn: HTMLElement, streaming = false): void {
    const s = this.scroller;
    const nearBottom = s.scrollHeight - s.scrollTop - s.clientHeight < 120;
    if (!streaming || nearBottom) turn.scrollIntoView({ block: 'nearest' });
  }
}
