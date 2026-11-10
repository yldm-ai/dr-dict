import { $, call, escape, formatNumber, toast } from './bridge.ts';
import { brandMark, hydrateIcons } from './icons.ts';
import { avatar, effectiveTheme, publish, state } from './state.ts';

type Hit = { id: string; name: string; count: number };
type LookupResult = {
  word: string;
  query: string;
  results: Hit[];
  suggestions?: string[];
  history?: { word: string; at: number }[];
};

const dictOrigin = 'https://dict.dr-dict.local';
const content = $('content');
const results = $('results');
const home = $('home');
const jump = $('jump');
const navigation = { stack: [] as string[], index: -1 };
let current: LookupResult | null = null;
let sequence = 0;
const collapsed = new Set<string>(JSON.parse(localStorage.getItem('collapsed') ?? '[]'));

export let onWordShown: (word: string, fromHistory: boolean) => void = () => {};
export function setWordShownHandler(handler: (word: string, fromHistory: boolean) => void): void {
  onWordShown = handler;
}

export function currentWord(): string {
  return current?.word ?? '';
}

function updateNavButtons(): void {
  $<HTMLButtonElement>('back').disabled = navigation.index <= 0;
  $<HTMLButtonElement>('forward').disabled = navigation.index >= navigation.stack.length - 1;
}

export async function lookup(word: string, options: { push?: boolean } = {}): Promise<void> {
  const text = word.trim();
  if (!text) return;
  const ticket = ++sequence;
  let result: LookupResult;
  try {
    result = await call<LookupResult>('lookup', { word: text });
  } catch (error) {
    toast((error as Error).message, 'error');
    return;
  }
  if (ticket !== sequence) return;
  if (result.history) {
    state.history = result.history;
    publish('history');
  }
  if (options.push !== false) {
    navigation.stack.splice(navigation.index + 1);
    if (navigation.stack[navigation.index] !== result.word) navigation.stack.push(result.word);
    navigation.index = navigation.stack.length - 1;
  }
  updateNavButtons();
  show(result, options.push === false);
}

export function back(): void {
  if (navigation.index <= 0) return;
  navigation.index--;
  void lookup(navigation.stack[navigation.index], { push: false });
}
export function forward(): void {
  if (navigation.index >= navigation.stack.length - 1) return;
  navigation.index++;
  void lookup(navigation.stack[navigation.index], { push: false });
}

function frameTheme(): string {
  return effectiveTheme() === 'dark' && state.settings.invertArticles ? 'dark' : 'light';
}

function show(result: LookupResult, fromHistory = false): void {
  current = result;
  home.hidden = true;
  results.hidden = false;
  content.scrollTop = 0;
  void call('window.title', { word: result.word });
  onWordShown(result.word, fromHistory);
  const redirected = result.word.toLowerCase() !== result.query.toLowerCase();
  const hero = `<header class="hero">
      <h1 class="hero-word">${escape(result.word)}</h1>
      <div class="hero-meta">${
        result.results.length
          ? `在 ${result.results.length} 部词典中找到${redirected ? ` · 由“${escape(result.query)}”推断的原形` : ''}`
          : '没有找到这个词'
      }</div>
    </header>`;
  if (!result.results.length) {
    const suggestions = result.suggestions ?? [];
    results.innerHTML = `${hero}<div class="not-found">
        <p>${state.dictionaries.some((d) => d.enabled) ? '已启用的词典中都没有收录这个词。检查一下拼写，或试试下面这些词：' : '还没有启用任何词典。在“设置 → 词典库”中添加或启用词典。'}</p>
        ${suggestions.length ? `<div class="chips">${suggestions.map((s) => `<button class="chip" type="button" data-word="${escape(s)}">${escape(s)}</button>`).join('')}</div>` : ''}
      </div>`;
    jump.innerHTML = '';
    return;
  }
  const word = encodeURIComponent(result.word);
  const theme = frameTheme();
  const paper = effectiveTheme() === 'dark' && theme === 'light';
  results.innerHTML =
    hero +
    result.results
      .map((hit) => {
        const dict = state.dictionaries.find((d) => d.id === hit.id);
        const isCollapsed = collapsed.has(hit.id);
        return `<section class="entry loading${isCollapsed ? ' collapsed' : ''}${paper ? ' paper' : ''}" data-id="${escape(hit.id)}">
          <header class="entry-header">
            <button class="entry-toggle" type="button" aria-expanded="${!isCollapsed}" title="${isCollapsed ? '展开' : '折叠'}">
              <span class="chevron" data-icon="chevron-down"></span>
              ${avatar(dict ?? { id: hit.id, name: hit.name, hasIcon: false }, 18)}
              <span class="entry-name">${escape(hit.name)}</span>
            </button>
            ${hit.count > 1 ? `<span class="entry-count">${formatNumber(hit.count)} 条</span>` : ''}
          </header>
          <div class="entry-body">
            <div class="skeleton"><span></span><span></span><span></span></div>
            <iframe title="${escape(hit.name)}" sandbox="allow-scripts allow-same-origin" referrerpolicy="no-referrer"
              src="${dictOrigin}/${encodeURIComponent(hit.id)}/@article?word=${word}&theme=${theme}"></iframe>
          </div>
        </section>`;
      })
      .join('');
  hydrateIcons(results);
  // A single dictionary needs no jump list.
  jump.innerHTML = result.results.length < 2 ? '' : result.results
    .map((hit) => `<button class="jump-item" type="button" data-id="${escape(hit.id)}">${escape(hit.name)}</button>`)
    .join('');
  updateJump();
}

export function showHome(): void {
  current = null;
  results.hidden = true;
  home.hidden = false;
  jump.innerHTML = '';
  void call('window.title', { word: '' });
  renderHome();
}

export function renderHome(): void {
  if (home.hidden) return;
  const enabled = state.dictionaries.filter((d) => d.enabled);
  const entries = enabled.reduce((sum, d) => sum + (d.entries || 0), 0);
  const recent = state.history.slice(0, 18);
  home.innerHTML = `<div class="home-inner">
      <div class="home-mark">${brandMark}</div>
      <h1 class="home-title">DR Dict</h1>
      <p class="home-lead">输入一个词，在你所有的词典里查找。</p>
      ${
        state.dictionaries.length
          ? `<div class="home-stats">${enabled.length} 部词典已启用 · 共 ${formatNumber(entries)} 个词条</div>
             ${
               recent.length
                 ? `<div class="home-section"><div class="section-label">最近查询</div><div class="chips">${recent
                     .map((h) => `<button class="chip" type="button" data-word="${escape(h.word)}">${escape(h.word)}</button>`)
                     .join('')}</div></div>`
                 : ''
             }
             <div class="home-tips">
               <span><kbd>Ctrl K</kbd> 搜索</span><span><kbd>Alt ←</kbd> 后退</span><span>双击正文中的词继续查询</span>
             </div>`
          : `<div class="onboard">
               <h2>先添加一部词典</h2>
               <p>支持 MDict（.mdx，附带 .mdd 资源）和 StarDict（.ifo）。选择文件夹时，会自动找到其中的全部词典。词典文件保留在原位置，不会被复制。</p>
               <div class="onboard-actions">
                 <button class="button primary" type="button" data-action="add-files"><span data-icon="plus"></span>添加词典</button>
                 <button class="button" type="button" data-action="add-folder"><span data-icon="folder"></span>添加文件夹</button>
               </div>
             </div>`
      }
    </div>`;
  hydrateIcons(home);
}

// Re-render frames after the theme or the article settings change.
export function refresh(): void {
  if (current) show(current);
  else renderHome();
}

function sectionFor(source: MessageEventSource | null): HTMLElement | null {
  for (const frame of results.querySelectorAll('iframe'))
    if (frame.contentWindow === source) return frame.closest('.entry');
  return null;
}

function updateJump(): void {
  const sections = [...results.querySelectorAll<HTMLElement>('.entry')];
  const top = content.getBoundingClientRect().top + 72;
  let active = sections[0]?.dataset.id;
  for (const s of sections) if (s.getBoundingClientRect().top <= top) active = s.dataset.id;
  for (const item of jump.querySelectorAll<HTMLElement>('.jump-item'))
    item.classList.toggle('active', item.dataset.id === active);
}

export function initArticles(handlers: { focusSearch: (text?: string, append?: string) => void }): void {
  content.addEventListener('scroll', updateJump, { passive: true });
  jump.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>('.jump-item');
    const section = item && results.querySelector<HTMLElement>(`.entry[data-id="${CSS.escape(item.dataset.id!)}"]`);
    if (!section) return;
    section.classList.remove('collapsed');
    content.scrollTo({ top: section.offsetTop - 8, behavior: 'smooth' });
  });
  results.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    const chip = target.closest<HTMLElement>('[data-word]');
    if (chip) {
      void lookup(chip.dataset.word!);
      return;
    }
    const toggle = target.closest('.entry-toggle');
    const section = toggle?.closest<HTMLElement>('.entry');
    if (!section) return;
    const isCollapsed = section.classList.toggle('collapsed');
    toggle!.setAttribute('aria-expanded', String(!isCollapsed));
    toggle!.setAttribute('title', isCollapsed ? '展开' : '折叠');
    if (isCollapsed) collapsed.add(section.dataset.id!);
    else collapsed.delete(section.dataset.id!);
    localStorage.setItem('collapsed', JSON.stringify([...collapsed]));
  });
  home.addEventListener('click', (event) => {
    const chip = (event.target as HTMLElement).closest<HTMLElement>('[data-word]');
    if (chip) void lookup(chip.dataset.word!);
  });

  window.addEventListener('message', (event) => {
    if (event.origin !== dictOrigin || !event.data || event.data.source !== 'dr-frame') return;
    const data = event.data;
    const section = sectionFor(event.source);
    switch (data.type) {
      case 'height': {
        const frame = section?.querySelector('iframe');
        if (!frame || typeof data.height !== 'number') return;
        frame.style.height = `${Math.max(24, Math.ceil(data.height))}px`;
        section!.classList.remove('loading');
        break;
      }
      case 'lookup':
        if (typeof data.word === 'string' && data.word.trim()) {
          handlers.focusSearch(data.word.trim());
          void lookup(data.word);
        }
        break;
      case 'open':
        if (typeof data.url === 'string') call('open.external', { url: data.url }).catch((e) => toast(e.message, 'error'));
        break;
      case 'scroll':
        if (section && typeof data.top === 'number') {
          const frame = section.querySelector('iframe')!;
          const top = frame.getBoundingClientRect().top - content.getBoundingClientRect().top + content.scrollTop;
          content.scrollTo({ top: top + data.top - 16, behavior: 'smooth' });
        }
        break;
      case 'key':
        if (data.key === 'back') back();
        else if (data.key === 'forward') forward();
        else if (data.key === 'search') handlers.focusSearch();
        else if (typeof data.key === 'string' && data.key.length === 1) handlers.focusSearch(undefined, data.key);
        break;
    }
  });
}

export function homeVisible(): boolean {
  return !home.hidden;
}
