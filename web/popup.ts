// The Ctrl+C+C lookup popup. The native side reads the clipboard and sends "popup-lookup"; this
// page looks the word up, renders the dictionaries' articles like the main window does, then asks
// to be shown beside the pointer. Links inside articles navigate within the popup.
import { $, call, escape, formatNumber, on, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { applyAppearance, setChromeMethod } from './appearance.ts';
import { hideContextMenu, initContextMenu, showContextMenu } from './context-menu.ts';
import { speak, stopSpeaking } from './speech.ts';
import { Tooltips } from './tooltip.ts';
import {
  avatar,
  effectiveTheme,
  setSystemDark,
  state,
  type Dictionary,
} from './state.ts';
import { initScriptSwitch, scriptQuery, scriptSwitch } from './script-switch.ts';
import {
  fillTranslation,
  findWiki,
  initTranslation,
  isSentence,
  translationHtml,
  translationMode,
  wikiSection,
  wikiWanted,
} from './online.ts';

type Hit = { id: string; name: string; count: number };
type LookupResult = { word: string; query: string; results: Hit[]; variants?: string[]; suggestions?: string[] };

const dictOrigin = 'https://dict.dr-dict.local';
const input = $<HTMLInputElement>('popup-input');
const content = $('popup-content');
const pinButton = $<HTMLButtonElement>('popup-pin');
const favoriteButton = $<HTMLButtonElement>('popup-favorite');
const jump = $('popup-jump');
const navigation = { stack: [] as string[], index: -1 };
let current: LookupResult | null = null;
let sequence = 0;
const collapsed = new Set<string>(JSON.parse(localStorage.getItem('collapsed') ?? '[]'));

setChromeMethod('popup.chrome');
// Self-drawn hover tips, as in the main window (the bar's titles become data-tooltip).
const tooltips = new Tooltips();

function frameTheme(): string {
  return effectiveTheme() === 'dark' && state.settings.invertArticles ? 'dark' : 'light';
}
// 原 / 简 / 繁 is chosen per dictionary and shared with the main window (script-switch.ts).
function articleUrl(id: string, word: string): string {
  return `${dictOrigin}/${encodeURIComponent(id)}/@article?word=${encodeURIComponent(word)}&theme=${frameTheme()}&vol=${state.settings.volume}${scriptQuery(id)}`;
}
/** The search box's dictionary selection, as the main window applies it. */
function scope(): string[] {
  const enabled = state.dictionaries.filter((d) => d.enabled && d.status !== 'removed').map((d) => d.id);
  const ids = enabled.filter((id) => (state.settings.scope ?? []).includes(id));
  return ids.length && ids.length < enabled.length ? ids : [];
}

function updateNavigation(): void {
  $<HTMLButtonElement>('popup-back').disabled = navigation.index <= 0;
  $<HTMLButtonElement>('popup-forward').disabled = navigation.index >= navigation.stack.length - 1;
}

async function lookup(word: string, options: { push?: boolean; show?: boolean } = {}): Promise<void> {
  const text = word.trim();
  if (!text) return;
  const ticket = ++sequence;
  let result: LookupResult;
  try {
    result = await call<LookupResult>('lookup', { word: text, dicts: scope() });
  } catch (error) {
    toast((error as Error).message, 'error');
    return;
  }
  if (ticket !== sequence) return;
  if (options.push !== false) {
    navigation.stack.splice(navigation.index + 1);
    if (navigation.stack[navigation.index] !== result.word) navigation.stack.push(result.word);
    navigation.index = navigation.stack.length - 1;
  }
  updateNavigation();
  input.value = result.word;
  render(result);
  if (options.show) await call('popup.show').catch(() => {});
}

// The star follows the shown word; favorites are shared with the main window.
function updateFavorite(): void {
  const word = current?.results.length ? current.word : '';
  const starred = !!word && state.favorites.some((f) => f.word === word);
  favoriteButton.disabled = !word;
  favoriteButton.setAttribute('aria-pressed', String(starred));
  favoriteButton.title = starred ? '取消收藏 (Ctrl D)' : '收藏 (Ctrl D)';
}
async function toggleFavorite(): Promise<void> {
  const word = current?.results.length ? current.word : '';
  if (!word) return;
  try {
    const starred = state.favorites.some((f) => f.word === word);
    state.favorites = await call('favorites.toggle', { word });
    updateFavorite();
    toast(starred ? `已取消收藏“${word}”` : `已收藏“${word}”`, 'success');
  } catch (error) {
    toast((error as Error).message, 'error');
  }
}

/** The contents rail at the right, as in the main window (articles.ts); none for one dictionary. */
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
             return `<button class="jump-item" type="button" data-id="${escape(hit.id)}">
               ${avatar(dict ?? { id: hit.id, name: hit.name, hasIcon: false }, 16)}
               <span class="toc-name">${escape(hit.name)}</span>
               ${hit.count > 1 ? `<span class="toc-count">${formatNumber(hit.count)}</span>` : ''}
             </button>`;
           })
           .join('')}
       </div>`;
  updateJump();
}
function updateJump(): void {
  const sections = [...content.querySelectorAll<HTMLElement>('.entry')];
  const top = content.getBoundingClientRect().top + 48;
  let active = sections[0]?.dataset.id;
  for (const s of sections) if (s.getBoundingClientRect().top <= top) active = s.dataset.id;
  for (const item of jump.querySelectorAll<HTMLElement>('[data-id]'))
    item.classList.toggle('active', item.dataset.id === active);
}
// After a jump the target stays in place for a moment while frames above it finish loading.
let jumpPin: { id: string; until: number } | null = null;
function scrollToSection(section: HTMLElement, behavior: ScrollBehavior): void {
  const top = section.getBoundingClientRect().top - content.getBoundingClientRect().top + content.scrollTop;
  const header = section.querySelector<HTMLElement>('.entry-header');
  const box = getComputedStyle(section);
  const stuck = header
    ? parseFloat(box.borderTopWidth) + parseFloat(box.paddingTop) - (parseFloat(getComputedStyle(header).top) || 0)
    : 0;
  content.scrollTo({ top: top + stuck, behavior });
}
function keepJumpPin(): void {
  if (!jumpPin) return;
  if (performance.now() > jumpPin.until) {
    jumpPin = null;
    return;
  }
  const section = content.querySelector<HTMLElement>(`.entry[data-id="${CSS.escape(jumpPin.id)}"]`);
  if (section) scrollToSection(section, 'auto');
}

function render(result: LookupResult): void {
  current = result;
  jumpPin = null;
  content.scrollTop = 0;
  updateFavorite();
  renderJump(result.results);
  const sentence = isSentence(result.query);
  const translation = translationMode(result.query, result.results.length > 0);
  const translationBlock = translation
    ? translationHtml(translation, '长句可以用小牛翻译直接译成中文或英文：在主窗口的“设置 → 在线”中填写 App ID 和 API Key。')
    : '';
  const paper = effectiveTheme() === 'dark' && frameTheme() === 'light';
  const shown = () => current === result;
  if (translation === 'translate')
    queueMicrotask(() => {
      const section = content.querySelector<HTMLElement>('.translation');
      if (section && shown()) void fillTranslation(section, result.query, shown);
    });
  // Wiktionary sections follow the dictionaries once they arrive (see online.ts).
  if (wikiWanted(result.word))
    void findWiki(result.word).then((wiki) => {
      if (!shown() || !wiki.length) return;
      if (!result.results.length) content.querySelector('.not-found')?.remove();
      content.insertAdjacentHTML(
        'beforeend',
        wiki.map((hit) => wikiSection(hit, frameTheme(), paper, collapsed.has(hit.id))).join(''),
      );
      hydrateIcons(content);
      renderJump([...result.results, ...wiki.map((w) => ({ id: w.id, name: w.name, count: 1 }))]);
    });
  if (!result.results.length) {
    const suggestions = result.suggestions ?? [];
    content.innerHTML =
      translationBlock +
      (sentence
        ? ''
        : `<div class="not-found">
        <p>没有找到“${escape(result.query)}”。${suggestions.length ? '试试这些词：' : ''}</p>
        ${suggestions.length ? `<div class="chips">${suggestions.map((s) => `<button class="chip" type="button" data-word="${escape(s)}">${escape(s)}</button>`).join('')}</div>` : ''}
      </div>`);
    hydrateIcons(content);
    return;
  }
  const redirected = result.word.toLowerCase() !== result.query.toLowerCase();
  content.innerHTML =
    translationBlock +
    `<div class="popup-meta">${result.results.length} 部词典${redirected ? ` · 由“${escape(result.query)}”推断的原形` : ''}${
      result.variants?.length ? ` · 同时查了 ${result.variants.map(escape).join('、')}` : ''
    }</div>` +
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
              src="${escape(articleUrl(hit.id, result.word))}"></iframe>
          </div>
        </section>`;
      })
      .join('');
  hydrateIcons(content);
}

function back(): void {
  if (navigation.index <= 0) return;
  navigation.index--;
  void lookup(navigation.stack[navigation.index], { push: false });
}
function forward(): void {
  if (navigation.index >= navigation.stack.length - 1) return;
  navigation.index++;
  void lookup(navigation.stack[navigation.index], { push: false });
}
async function setPinned(pinned: boolean): Promise<void> {
  const result = await call<boolean>('popup.pin', { pinned }).catch(() => pinned);
  pinButton.setAttribute('aria-pressed', String(result));
  pinButton.title = result ? '取消固定' : '固定';
}
const hide = () => void call('popup.hide').catch(() => {});

function sectionFor(source: MessageEventSource | null): HTMLElement | null {
  for (const frame of content.querySelectorAll('iframe')) if (frame.contentWindow === source) return frame.closest('.entry');
  return null;
}

// A new Ctrl+C+C: settings and dictionaries come along, so the popup matches the main window.
on('popup-lookup', (data) => {
  state.settings = { ...state.settings, ...data.settings };
  state.dictionaries = (data.dictionaries ?? []) as Dictionary[];
  state.favorites = Array.isArray(data.favorites) ? data.favorites : [];
  state.chineseAvailable = Boolean(data.chineseAvailable);
  if (data.niutrans) state.niutrans = data.niutrans;
  setSystemDark(Boolean(data.systemDark));
  applyAppearance();
  if (pinButton.getAttribute('aria-pressed') !== 'true') {
    navigation.stack = [];
    navigation.index = -1;
  }
  void lookup(String(data.word ?? ''), { show: true });
});
on('popup-hidden', () => {
  stopSpeaking();
  hideContextMenu();
  tooltips.hide();
  // Closing a pinned popup unpins it; the next Ctrl+C+C starts a fresh, unpinned one.
  if (pinButton.getAttribute('aria-pressed') === 'true') void setPinned(false);
});

$('popup-form').addEventListener('submit', (event) => {
  event.preventDefault();
  void lookup(input.value);
});
input.addEventListener('focus', () => input.select());
$('popup-back').addEventListener('click', back);
$('popup-forward').addEventListener('click', forward);
$('popup-close').addEventListener('click', hide);
pinButton.addEventListener('click', () => void setPinned(pinButton.getAttribute('aria-pressed') !== 'true'));
$('popup-main').addEventListener('click', () => void call('popup.openMain', { word: current?.word ?? input.value.trim() }));
favoriteButton.addEventListener('click', () => void toggleFavorite());
initScriptSwitch(content, content, (section) => {
  const frame = section.querySelector('iframe');
  if (!frame || !current) return;
  const url = articleUrl(section.dataset.id!, current.word);
  if (frame.src !== url) frame.src = url;
});
content.addEventListener('scroll', updateJump, { passive: true });
for (const type of ['wheel', 'pointerdown', 'keydown']) content.addEventListener(type, () => (jumpPin = null), { passive: true });
jump.addEventListener('click', (event) => {
  const item = (event.target as HTMLElement).closest<HTMLElement>('.jump-item');
  const section = item && content.querySelector<HTMLElement>(`.entry[data-id="${CSS.escape(item.dataset.id!)}"]`);
  if (!section) return;
  section.classList.remove('collapsed');
  section.querySelector('.entry-toggle')?.setAttribute('aria-expanded', 'true');
  item.blur(); // let the contents fold again once the pointer leaves
  jumpPin = { id: item.dataset.id!, until: performance.now() + 2000 };
  scrollToSection(section, 'smooth');
});
content.addEventListener('click', (event) => {
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
  if (isCollapsed) collapsed.add(section.dataset.id!);
  else collapsed.delete(section.dataset.id!);
  localStorage.setItem('collapsed', JSON.stringify([...collapsed]));
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') hide();
  else if (event.altKey && event.key === 'ArrowLeft') back();
  else if (event.altKey && event.key === 'ArrowRight') forward();
  else if (event.ctrlKey && ['k', 'l', 'f'].includes(event.key.toLowerCase())) input.focus();
  else if (event.ctrlKey && event.key.toLowerCase() === 'd') void toggleFavorite();
  else return;
  event.preventDefault();
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
      keepJumpPin();
      break;
    }
    case 'lookup':
      if (typeof data.word === 'string' && data.word.trim()) void lookup(data.word);
      break;
    case 'speak':
      if (typeof data.text === 'string' && !speak(data.text, typeof data.lang === 'string' ? data.lang : 'en-US'))
        toast('没有可用的本机朗读语音', 'error');
      break;
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
        const header = section.querySelector<HTMLElement>('.entry-header')?.offsetHeight ?? 0;
        content.scrollTo({ top: top + data.top - header - 8, behavior: 'smooth' });
      }
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
    case 'key':
      if (data.key === 'escape') {
        hideContextMenu();
        hide();
      } else if (data.key === 'back') back();
      else if (data.key === 'forward') forward();
      else if (data.key === 'search') input.focus();
      else if (typeof data.key === 'string' && data.key.length === 1) {
        input.focus();
        input.value = data.key;
      }
      break;
  }
});

initContextMenu({ lookup: (word) => void lookup(word) });
initTranslation(content);
hydrateIcons();
applyAppearance();
document.body.classList.add('ready');
