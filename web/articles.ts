import { $, call, escape, formatNumber, toast } from './bridge.ts';
import { brandMark, hydrateIcons } from './icons.ts';
import {
  activeDictionaries,
  avatar,
  effectiveTheme,
  publish,
  state,
} from './state.ts';
import { initScriptSwitch, scriptQuery, scriptSwitch } from './script-switch.ts';
import { speak, stopSpeaking } from './speech.ts';
import { scopeIds } from './scope.ts';
import { hideContextMenu, showContextMenu } from './context-menu.ts';
import {
  fillTranslation,
  findWiki,
  initTranslation,
  isSentence,
  translationHtml,
  translationMode,
  wikiSection,
  wikiWanted,
  type WikiHit,
} from './online.ts';

type Hit = { id: string; name: string; count: number };
type LookupResult = {
  word: string;
  query: string;
  results: Hit[];
  // Simplified/traditional writings that were looked up as well and found.
  variants?: string[];
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
function articleUrl(id: string, word: string, theme: string): string {
  return `${dictOrigin}/${encodeURIComponent(id)}/@article?word=${encodeURIComponent(word)}&theme=${theme}&vol=${
    state.settings.volume
  }${scriptQuery(id)}`;
}

export let onWordShown: (word: string, fromHistory: boolean) => void = () => {};
export function setWordShownHandler(handler: (word: string, fromHistory: boolean) => void): void {
  onWordShown = handler;
}

export function currentWord(): string {
  return current?.word ?? '';
}
/** Names of the dictionaries that have the shown word (for AI 助查's context). */
export function currentDictionaryNames(): string[] {
  return current?.results.map((hit) => hit.name) ?? [];
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
    result = await call<LookupResult>('lookup', { word: text, dicts: scopeIds() });
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

function heroMeta(result: LookupResult, wiki: WikiHit[]): string {
  const redirected = result.word.toLowerCase() !== result.query.toLowerCase();
  const scoped = scopeIds().length ? ` · 仅查 ${scopeIds().length} 部词典` : '';
  const online = wiki.length ? ` · ${wiki.map((w) => escape(w.name)).join('、')}` : '';
  if (result.results.length)
    return `在 ${result.results.length} 部词典中找到${redirected ? ` · 由“${escape(result.query)}”推断的原形` : ''}${
      result.variants?.length ? ` · 同时查了 ${result.variants.map(escape).join('、')}` : ''
    }${scoped}${online}`;
  if (wiki.length) return `本地词典中没有这个词${scoped}${online}`;
  return isSentence(result.query) ? `整句${scoped}` : `没有找到这个词${scoped}`;
}

function show(result: LookupResult, fromHistory = false): void {
  current = result;
  pinned = null;
  home.hidden = true;
  results.hidden = false;
  content.scrollTop = 0;
  onWordShown(result.word, fromHistory);
  const sentence = isSentence(result.query);
  const translation = translationMode(result.query, result.results.length > 0);
  const hero = `<header class="hero${sentence ? ' sentence' : ''}">
      <h1 class="hero-word">${escape(sentence ? result.query : result.word)}</h1>
      <div class="hero-meta">${heroMeta(result, [])}</div>
    </header>${
      translation
        ? translationHtml(
            translation,
            '长句可以用小牛翻译直接译成中文或英文。<button class="link-button" type="button" data-action="online-settings">在设置中填写 API Key</button>',
          )
        : ''
    }`;
  const theme = frameTheme();
  const paper = effectiveTheme() === 'dark' && theme === 'light';
  const shown = () => current === result;
  if (translation === 'translate') queueMicrotask(() => {
    const section = results.querySelector<HTMLElement>('.translation');
    if (section && shown()) void fillTranslation(section, result.query, shown);
  });
  // Wiktionary answers after the dictionaries; its sections are appended when they arrive.
  if (wikiWanted(result.word))
    void findWiki(result.word).then((wiki) => {
      if (!shown() || !wiki.length) return;
      results.insertAdjacentHTML(
        'beforeend',
        wiki.map((hit) => wikiSection(hit, theme, paper, collapsed.has(hit.id))).join(''),
      );
      hydrateIcons(results);
      results.querySelector('.hero-meta')!.innerHTML = heroMeta(result, wiki);
      renderJump([...result.results, ...wiki.map((w) => ({ id: w.id, name: w.name, count: 1 }))]);
    });
  if (!result.results.length) {
    const suggestions = result.suggestions ?? [];
    // A sentence is not misspelt: no "check the spelling" and no similar headwords.
    results.innerHTML = sentence
      ? hero
      : `${hero}<div class="not-found">
        <p>${state.dictionaries.some((d) => d.enabled) ? '已启用的词典中都没有收录这个词。检查一下拼写，或试试下面这些词：' : '还没有启用任何词典。在“设置 → 词典库”中添加或启用词典。'}</p>
        ${suggestions.length ? `<div class="chips">${suggestions.map((s) => `<button class="chip" type="button" data-word="${escape(s)}">${escape(s)}</button>`).join('')}</div>` : ''}
      </div>`;
    hydrateIcons(results);
    jump.innerHTML = '';
    jump.hidden = true;
    return;
  }
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
            ${scriptSwitch(hit.id)}
          </header>
          <div class="entry-body">
            <div class="skeleton"><span></span><span></span><span></span></div>
            <iframe title="${escape(hit.name)}" sandbox="allow-scripts allow-same-origin" referrerpolicy="no-referrer"
              src="${escape(articleUrl(hit.id, result.word, theme))}"></iframe>
          </div>
        </section>`;
      })
      .join('');
  hydrateIcons(results);
  renderJump(result.results);
}

// The contents: one short line per dictionary at the right edge; hovering unfolds the names.
// A single dictionary needs none.
function renderJump(hits: Hit[]): void {
  jump.hidden = hits.length < 2;
  jump.innerHTML = jump.hidden
    ? ''
    : `<div class="toc-rail" aria-hidden="true">${hits
        .map((hit) => `<span class="toc-line" data-id="${escape(hit.id)}"></span>`)
        .join('')}</div>
       <div class="toc-panel">
         <div class="toc-title">词典</div>
         ${hits
           .map((hit) => {
             const dict = state.dictionaries.find((d) => d.id === hit.id);
             return `<button class="jump-item" type="button" data-id="${escape(hit.id)}" title="${escape(hit.name)}">
               ${avatar(dict ?? { id: hit.id, name: hit.name, hasIcon: false }, 16)}
               <span class="toc-name">${escape(hit.name)}</span>
               ${hit.count > 1 ? `<span class="toc-count">${formatNumber(hit.count)}</span>` : ''}
             </button>`;
           })
           .join('')}
       </div>`;
  updateJump();
}

export function showHome(): void {
  current = null;
  results.hidden = true;
  home.hidden = false;
  jump.innerHTML = '';
  jump.hidden = true;
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
        activeDictionaries().length
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
/** Sends the pronunciation volume to the article frames already shown. */
export function applyVolume(): void {
  for (const frame of results.querySelectorAll('iframe'))
    frame.contentWindow?.postMessage({ source: 'dr-app', type: 'volume', volume: state.settings.volume }, dictOrigin);
}
/** Looks the shown word up again, for settings that change what a lookup finds. */
export function requery(): void {
  if (current) void lookup(current.query, { push: false });
}

function sectionFor(source: MessageEventSource | null): HTMLElement | null {
  for (const frame of results.querySelectorAll('iframe'))
    if (frame.contentWindow === source) return frame.closest('.entry');
  return null;
}

// After a jump the target stays pinned for a moment: frames above it that are still loading
// change height and would otherwise push it away from the top.
let pinned: { id: string; until: number } | null = null;
function scrollToSection(section: HTMLElement, behavior: ScrollBehavior): void {
  // Measured against the scroll container: offsetTop counts from the page and would include the
  // top bar, overshooting so the sticky dictionary header covers the start of the article.
  const top = section.getBoundingClientRect().top - content.getBoundingClientRect().top + content.scrollTop;
  // Land where the header sticks (its blank top hidden, see .entry-header in styles.css).
  const header = section.querySelector<HTMLElement>('.entry-header');
  const box = getComputedStyle(section);
  const stuck = header
    ? parseFloat(box.borderTopWidth) + parseFloat(box.paddingTop) - (parseFloat(getComputedStyle(header).top) || 0)
    : 0;
  content.scrollTo({ top: top + stuck, behavior });
}
function keepPinned(): void {
  if (!pinned) return;
  if (performance.now() > pinned.until) {
    pinned = null;
    return;
  }
  const section = results.querySelector<HTMLElement>(`.entry[data-id="${CSS.escape(pinned.id)}"]`);
  if (section) scrollToSection(section, 'auto');
}

function updateJump(): void {
  const sections = [...results.querySelectorAll<HTMLElement>('.entry')];
  const top = content.getBoundingClientRect().top + 72;
  let active = sections[0]?.dataset.id;
  for (const s of sections) if (s.getBoundingClientRect().top <= top) active = s.dataset.id;
  for (const item of jump.querySelectorAll<HTMLElement>('[data-id]'))
    item.classList.toggle('active', item.dataset.id === active);
}

export function initArticles(handlers: {
  focusSearch: (text?: string, append?: string) => void;
  openOnlineSettings: () => void;
}): void {
  initTranslation(results);
  content.addEventListener('scroll', updateJump, { passive: true });
  jump.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>('.jump-item');
    const section = item && results.querySelector<HTMLElement>(`.entry[data-id="${CSS.escape(item.dataset.id!)}"]`);
    if (!section) return;
    section.classList.remove('collapsed');
    // Let the contents fold again once the pointer leaves (focus would keep it open).
    item.blur();
    pinned = { id: item.dataset.id!, until: performance.now() + 2000 };
    scrollToSection(section, 'smooth');
  });
  // Any scrolling by the user ends the pin.
  for (const type of ['wheel', 'pointerdown', 'keydown'])
    content.addEventListener(type, () => (pinned = null), { passive: true });
  results.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    if (target.closest('[data-action="online-settings"]')) {
      handlers.openOnlineSettings();
      return;
    }
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
  // 原 / 简 / 繁: reload only that dictionary's frame. It keeps its height until the converted
  // page reports its own, so nothing jumps.
  initScriptSwitch(results, content, (section) => {
    const frame = section.querySelector('iframe');
    if (!frame || !current) return;
    const url = articleUrl(section.dataset.id!, current.word, frameTheme());
    if (frame.src !== url) frame.src = url;
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
        keepPinned();
        break;
      }
      case 'lookup':
        if (typeof data.word === 'string' && data.word.trim()) {
          handlers.focusSearch(data.word.trim());
          void lookup(data.word);
        }
        break;
      case 'speak':
        if (typeof data.text === 'string' && !speak(data.text, typeof data.lang === 'string' ? data.lang : 'en-US'))
          toast('没有可用的本机朗读语音', 'error');
        break;
      case 'menu': {
        const frame = section?.querySelector('iframe');
        if (!frame || typeof data.text !== 'string' || typeof data.x !== 'number' || typeof data.y !== 'number') return;
        const rect = frame.getBoundingClientRect();
        const target = frame.contentWindow;
        showContextMenu(rect.left + data.x, rect.top + data.y, data.text, () =>
          target?.postMessage({ source: 'dr-app', type: 'copy' }, dictOrigin),
        );
        break;
      }
      case 'speak-stop':
        stopSpeaking();
        break;
      case 'open':
        if (typeof data.url === 'string') call('open.external', { url: data.url }).catch((e) => toast(e.message, 'error'));
        break;
      case 'scroll':
        if (section && typeof data.top === 'number') {
          const frame = section.querySelector('iframe')!;
          const top = frame.getBoundingClientRect().top - content.getBoundingClientRect().top + content.scrollTop;
          // Keep the anchor clear of the dictionary's sticky header.
          const header = section.querySelector<HTMLElement>('.entry-header')?.offsetHeight ?? 0;
          content.scrollTo({ top: top + data.top - header - 8, behavior: 'smooth' });
        }
        break;
      case 'key':
        if (data.key === 'escape') hideContextMenu();
        else if (data.key === 'back') back();
        else if (data.key === 'forward') forward();
        else if (data.key === 'search') handlers.focusSearch();
        else if (typeof data.key === 'string' && data.key.startsWith('zoom-'))
          void call('window.zoom', { step: data.key.slice(5) }).catch(() => {});
        else if (typeof data.key === 'string' && data.key.length === 1) handlers.focusSearch(undefined, data.key);
        break;
    }
  });
}

export function homeVisible(): boolean {
  return !home.hidden;
}
