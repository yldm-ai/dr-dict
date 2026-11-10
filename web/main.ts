import { $, call, escape, on, toast } from './bridge.ts';
import { back, currentWord, forward, initArticles, lookup, refresh, renderHome, setWordShownHandler, showHome } from './articles.ts';
import { brandMark, hydrateIcons } from './icons.ts';
import { addDictionaries, initLibrary, resetLibrary } from './library-panel.ts';
import { initSettings, openSettings } from './settings-dialog.ts';
import { Tooltips } from './tooltip.ts';
import { effectiveTheme, onSystemThemeChange, state, subscribe } from './state.ts';

// Self-drawn hover tips everywhere, as in DR Reader (native title tooltips never appear).
const tooltips = new Tooltips();
const input = $<HTMLInputElement>('search');
const listElement = $('word-list');
const listLabel = $('list-label');
const listEmpty = $('list-empty');
let items: string[] = [];
let active = -1;
let suggestTicket = 0;
let suggestTimer = 0;
let mode: 'history' | 'suggest' = 'history';

function applyTheme(): void {
  document.body.dataset.theme = effectiveTheme();
  document.body.dataset.articleWidth = state.settings.articleWidth;
}

function renderList(): void {
  listLabel.textContent = mode === 'history' ? '最近查询' : '联想';
  const word = currentWord().toLowerCase();
  listElement.innerHTML = items
    .map(
      (w, i) =>
        `<li role="option" class="word${i === active ? ' active' : ''}${w.toLowerCase() === word ? ' current' : ''}" data-index="${i}" aria-selected="${i === active}">${escape(w)}</li>`,
    )
    .join('');
  let empty = '';
  if (!items.length) {
    if (!state.dictionaries.length) empty = '添加词典后即可开始查询';
    else if (mode === 'suggest') empty = `没有以“${escape(input.value.trim())}”开头的词条`;
    else empty = '查过的词会出现在这里';
  }
  listEmpty.innerHTML = empty;
  listEmpty.hidden = !empty;
  listElement.hidden = !items.length;
}

function showHistory(): void {
  mode = 'history';
  items = state.history.map((h) => h.word).slice(0, 100);
  active = -1;
  renderList();
}

async function suggest(): Promise<void> {
  const query = input.value.trim();
  const ticket = ++suggestTicket;
  if (!query) {
    showHistory();
    return;
  }
  try {
    const words = await call<string[]>('suggest', { query, limit: 80 });
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
  listElement.addEventListener('click', (event) => {
    const li = (event.target as HTMLElement).closest<HTMLElement>('.word');
    if (!li) return;
    setActive(Number(li.dataset.index));
    void lookup(items[active]);
  });
  setWordShownHandler((word, fromHistory) => {
    if (fromHistory) {
      input.value = word;
      scheduleSuggest();
    }
    for (const li of listElement.querySelectorAll<HTMLElement>('.word'))
      li.classList.toggle('current', items[Number(li.dataset.index)]?.toLowerCase() === currentWord().toLowerCase());
  });
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
  initArticles({ focusSearch });
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
  let previous = '';
  subscribe('settings', () => {
    applyTheme();
    const signature = `${effectiveTheme()}|${state.settings.invertArticles}|${state.settings.articleWidth}`;
    if (signature !== previous) refresh();
    previous = signature;
  });
  onSystemThemeChange(() => {
    applyTheme();
    refresh();
  });
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
    state.dataPath = data.dataPath;
  } catch (error) {
    toast((error as Error).message, 'error');
  }
  applyTheme();
  previous = `${effectiveTheme()}|${state.settings.invertArticles}|${state.settings.articleWidth}`;
  showHome();
  showHistory();
  document.body.classList.add('ready');
  input.focus();
}

void bootstrap();
