import { $, call, escape, on, toast } from './bridge.ts';
import {
  applyVolume,
  back,
  currentWord,
  forward,
  initArticles,
  lookup,
  refresh,
  renderHome,
  requery,
  setWordShownHandler,
  showHome,
} from './articles.ts';
import { brandMark, hydrateIcons } from './icons.ts';
import { initContextMenu } from './context-menu.ts';
import { addDictionaries, initLibrary, resetLibrary } from './library-panel.ts';
import { initSettings, openSettings } from './settings-dialog.ts';
import { Tooltips } from './tooltip.ts';
import { initScope, scopeIds } from './scope.ts';
import { activeDictionaries, effectiveTheme, onSystemThemeChange, publish, setSystemDark, state, subscribe } from './state.ts';

// Self-drawn hover tips everywhere, as in DR Reader (native title tooltips never appear).
const tooltips = new Tooltips();
const input = $<HTMLInputElement>('search');
const listElement = $('word-list');
const listEmpty = $('list-empty');
let items: string[] = [];
let active = -1;
let suggestTicket = 0;
let suggestTimer = 0;
// 'history' shows the list picked by the tabs (recent queries or favorites); 'suggest' the matches.
let mode: 'history' | 'suggest' = 'history';
let listKind: 'history' | 'favorites' = 'history';

function applyTheme(): void {
  document.body.dataset.theme = effectiveTheme();
  document.body.dataset.articleWidth = state.settings.articleWidth;
}

function renderList(): void {
  $('list-title').hidden = mode !== 'suggest';
  $('list-tabs').hidden = mode === 'suggest';
  for (const tab of document.querySelectorAll<HTMLElement>('#list-tabs [data-list]'))
    tab.setAttribute('aria-selected', String(tab.dataset.list === listKind));
  const word = currentWord().toLowerCase();
  // Recent queries and favorites get a remove button on hover; suggestions do not.
  const label = listKind === 'favorites' ? '取消收藏' : '从最近查询中移除';
  const remove =
    mode === 'history'
      ? `<button class="word-remove" type="button" title="${label}" aria-label="${label}"><span data-icon="x"></span></button>`
      : '';
  listElement.innerHTML = items
    .map(
      (w, i) =>
        `<li role="option" class="word${i === active ? ' active' : ''}${w.toLowerCase() === word ? ' current' : ''}" data-index="${i}" aria-selected="${i === active}"><span class="word-text">${escape(w)}</span>${remove}</li>`,
    )
    .join('');
  if (remove) hydrateIcons(listElement);
  let empty = '';
  if (!items.length) {
    if (!activeDictionaries().length) empty = '添加词典后即可开始查询';
    else if (mode === 'suggest') empty = `没有以“${escape(input.value.trim())}”开头的词条`;
    else if (listKind === 'favorites') empty = '点击右上角的星标收藏单词';
    else empty = '查过的词会出现在这里';
  }
  listEmpty.innerHTML = empty;
  listEmpty.hidden = !empty;
  listElement.hidden = !items.length;
}

function showHistory(): void {
  mode = 'history';
  items = (listKind === 'favorites' ? state.favorites : state.history).map((h) => h.word).slice(0, 200);
  active = -1;
  renderList();
}

// The star in the top bar follows the shown word.
function updateFavoriteButton(): void {
  const button = $<HTMLButtonElement>('favorite');
  const word = currentWord();
  const starred = !!word && state.favorites.some((f) => f.word === word);
  button.disabled = !word;
  button.setAttribute('aria-pressed', String(starred));
  button.title = starred ? '取消收藏 (Ctrl D)' : '收藏 (Ctrl D)';
}
async function toggleFavorite(word = currentWord()): Promise<void> {
  if (!word) return;
  try {
    const starred = state.favorites.some((f) => f.word === word);
    state.favorites = await call('favorites.toggle', { word });
    publish('favorites');
    if (word === currentWord()) toast(starred ? `已取消收藏“${word}”` : `已收藏“${word}”`, 'success');
  } catch (error) {
    toast((error as Error).message, 'error');
  }
}

async function suggest(): Promise<void> {
  const query = input.value.trim();
  const ticket = ++suggestTicket;
  if (!query) {
    showHistory();
    return;
  }
  try {
    const words = await call<string[]>('suggest', { query, limit: 80, dicts: scopeIds() });
    if (ticket !== suggestTicket) return;
    mode = 'suggest';
    items = words;
    active = items.length ? 0 : -1;
    renderList();
  } catch (error) {
    toast((error as Error).message, 'error');
  }
}

function scheduleSuggest(): void {
  $('search-clear').hidden = !input.value;
  clearTimeout(suggestTimer);
  suggestTimer = window.setTimeout(() => void suggest(), 60);
}

function setActive(index: number): void {
  if (!items.length) return;
  active = Math.max(0, Math.min(items.length - 1, index));
  for (const li of listElement.querySelectorAll<HTMLElement>('.word')) {
    const selected = Number(li.dataset.index) === active;
    li.classList.toggle('active', selected);
    li.setAttribute('aria-selected', String(selected));
    if (selected) li.scrollIntoView({ block: 'nearest' });
  }
}

function focusSearch(text?: string, append?: string): void {
  if (text !== undefined) {
    input.value = text;
    scheduleSuggest();
  }
  if (append !== undefined) {
    input.value += append;
    scheduleSuggest();
    input.focus();
    return;
  }
  input.focus();
  if (append === undefined && text === undefined) input.select();
}

function submit(): void {
  const word = active >= 0 && mode === 'suggest' ? items[active] : input.value.trim();
  if (word) void lookup(word);
}

function initSearch(): void {
  input.addEventListener('input', scheduleSuggest);
  input.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowDown') {
      event.preventDefault();
      setActive(active + 1);
    } else if (event.key === 'ArrowUp') {
      event.preventDefault();
      setActive(active - 1);
    } else if (event.key === 'PageDown') {
      event.preventDefault();
      setActive(active + 10);
    } else if (event.key === 'PageUp') {
      event.preventDefault();
      setActive(active - 10);
    } else if (event.key === 'Enter') {
      event.preventDefault();
      if (mode === 'history' && active >= 0) void lookup(items[active]);
      else submit();
    } else if (event.key === 'Escape' && input.value) {
      event.preventDefault();
      input.value = '';
      scheduleSuggest();
    }
  });
  $('search-clear').addEventListener('click', () => {
    input.value = '';
    scheduleSuggest();
    input.focus();
  });
  listElement.addEventListener('mousedown', (event) => {
    // Keep focus in the search box while a word is picked.
    if ((event.target as HTMLElement).closest('.word')) event.preventDefault();
  });
  listElement.addEventListener('click', async (event) => {
    const li = (event.target as HTMLElement).closest<HTMLElement>('.word');
    if (!li) return;
    if ((event.target as HTMLElement).closest('.word-remove')) {
      const word = items[Number(li.dataset.index)];
      if (listKind === 'favorites') {
        await toggleFavorite(word);
        return;
      }
      try {
        state.history = await call('history.remove', { word });
        publish('history');
      } catch (error) {
        toast((error as Error).message, 'error');
      }
      return;
    }
    setActive(Number(li.dataset.index));
    void lookup(items[active]);
  });
  $('list-tabs').addEventListener('click', (event) => {
    const tab = (event.target as HTMLElement).closest<HTMLElement>('[data-list]');
    if (!tab) return;
    listKind = tab.dataset.list as 'history' | 'favorites';
    showHistory();
  });
  $('favorite').addEventListener('click', () => void toggleFavorite());
  setWordShownHandler((word, fromHistory) => {
    updateFavoriteButton();
    if (fromHistory) {
      input.value = word;
      scheduleSuggest();
    }
    for (const li of listElement.querySelectorAll<HTMLElement>('.word'))
      li.classList.toggle('current', items[Number(li.dataset.index)]?.toLowerCase() === currentWord().toLowerCase());
  });
}

// Keys that zoom with Ctrl, as in browsers (with and without Shift, main keys and numpad).
const zoomSteps: Record<string, 'in' | 'out' | 'reset'> = { '=': 'in', '+': 'in', '-': 'out', _: 'out', '0': 'reset' };

// Ctrl+wheel zooms one step per notch; touchpad pinches send many small deltas, so they add up
// to a notch (100) before each step.
let wheelZoom = 0;
function initWheelZoom(): void {
  window.addEventListener(
    'wheel',
    (event) => {
      if (!event.ctrlKey) return;
      event.preventDefault();
      wheelZoom += event.deltaY;
      if (Math.abs(wheelZoom) < 100) return;
      const step = wheelZoom < 0 ? 'in' : 'out';
      wheelZoom = 0;
      void call('window.zoom', { step });
    },
    { passive: false },
  );
}

function isEditable(target: EventTarget | null): boolean {
  const element = target as HTMLElement | null;
  return !!element && (element.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(element.tagName));
}

function initKeyboard(): void {
  document.addEventListener('keydown', (event) => {
    const dialogOpen = [...document.querySelectorAll('dialog')].some((d) => d.open);
    if ((event.ctrlKey && ['k', 'l', 'f'].includes(event.key.toLowerCase())) || (event.key === '/' && !isEditable(event.target))) {
      if (dialogOpen) return;
      event.preventDefault();
      focusSearch();
    } else if (event.altKey && event.key === 'ArrowLeft') {
      event.preventDefault();
      back();
    } else if (event.altKey && event.key === 'ArrowRight') {
      event.preventDefault();
      forward();
    } else if (event.ctrlKey && event.key === ',') {
      event.preventDefault();
      openSettings();
    } else if (event.ctrlKey && zoomSteps[event.key]) {
      // Ctrl +/−/0 zoom the page like a browser (Ctrl+wheel is handled by WebView2 itself).
      event.preventDefault();
      void call('window.zoom', { step: zoomSteps[event.key] });
    } else if (event.ctrlKey && event.key.toLowerCase() === 'd' && !dialogOpen) {
      event.preventDefault();
      void toggleFavorite();
    } else if (
      !dialogOpen &&
      !isEditable(event.target) &&
      event.key.length === 1 &&
      !event.ctrlKey &&
      !event.altKey &&
      !event.metaKey &&
      event.key.trim()
    ) {
      // Typing anywhere starts a new search.
      input.value = '';
      input.focus();
    }
  });
  window.addEventListener('mouseup', (event) => {
    if (event.button === 3) back();
    if (event.button === 4) forward();
  });
}

async function bootstrap(): Promise<void> {
  const brand = document.querySelector('.brand-mark')!;
  brand.innerHTML = brandMark;
  hydrateIcons();
  initSearch();
  initKeyboard();
  initWheelZoom();
  initArticles({ focusSearch });
  initContextMenu({
    lookup: (word) => {
      focusSearch(word);
      void lookup(word);
    },
  });
  initLibrary(on);
  initSettings({ onTabChange: (tab) => tab === 'library' && resetLibrary() });
  $('back').addEventListener('click', back);
  $('forward').addEventListener('click', forward);
  $('open-settings').addEventListener('click', () => openSettings());
  $('home').addEventListener('click', (event) => {
    const action = (event.target as HTMLElement).closest<HTMLElement>('[data-action]')?.dataset.action;
    if (action === 'add-files') void addDictionaries('files');
    if (action === 'add-folder') void addDictionaries('folder');
  });
  document.querySelector('.brand')!.addEventListener('click', () => {
    showHome();
    updateFavoriteButton();
    focusSearch('');
  });
  subscribe('dictionaries', () => {
    renderHome();
    if (!input.value.trim()) showHistory();
    else scheduleSuggest();
  });
  subscribe('history', () => {
    renderHome();
    if (mode === 'history') showHistory();
  });
  subscribe('favorites', () => {
    updateFavoriteButton();
    if (mode === 'history') showHistory();
  });
  let previous = '';
  subscribe('settings', () => {
    applyTheme();
    applyVolume();
    const signature = `${effectiveTheme()}|${state.settings.invertArticles}|${state.settings.articleWidth}|${state.settings.traditionalVariant}`;
    if (signature !== previous) refresh();
    previous = signature;
  });
  let previousChinese = '';
  subscribe('saved', () => {
    // Simplified/traditional lookup changes what a lookup finds, so ask again.
    const signature = `${state.settings.chineseConversion}|${state.settings.chineseVariant}`;
    if (signature !== previousChinese) requery();
    previousChinese = signature;
  });
  onSystemThemeChange(() => {
    applyTheme();
    refresh();
  });
  on('system-theme', ({ dark }) => setSystemDark(Boolean(dark)));
  on('zoom', ({ factor }) => toast(`缩放 ${Math.round(Number(factor) * 100)}%（Ctrl 0 还原）`, 'success'));
  on('caption-tooltip', ({ text, left, width }) => {
    // A Direct2D caption button's tip: the button ends at the top edge of the web content.
    if (text) tooltips.show({ left, width, top: -1, height: 1 }, text);
    else tooltips.hide();
  });

  try {
    const data = await call('bootstrap');
    state.dictionaries = data.dictionaries;
    state.settings = { ...state.settings, ...data.settings };
    state.history = data.history;
    state.favorites = data.favorites ?? [];
    state.dataPath = data.dataPath;
    state.chineseAvailable = Boolean(data.chineseAvailable);
    setSystemDark(Boolean(data.systemDark));
  } catch (error) {
    toast((error as Error).message, 'error');
  }
  applyTheme();
  previous = `${effectiveTheme()}|${state.settings.invertArticles}|${state.settings.articleWidth}|${state.settings.traditionalVariant}`;
  previousChinese = `${state.settings.chineseConversion}|${state.settings.chineseVariant}`;
  // A new search scope changes both the shown result and the suggestions.
  initScope({
    onChange: () => {
      requery();
      if (input.value.trim()) scheduleSuggest();
    },
  });
  showHome();
  updateFavoriteButton();
  showHistory();
  document.body.classList.add('ready');
  input.focus();
}

void bootstrap();
