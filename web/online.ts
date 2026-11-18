// Online sources shown together with the dictionaries, in the main window and the popup:
// - Bilibili / NiuTrans: a sentence (or a missing headword) is translated between Chinese and English,
//   with a button that copies the plain translation;
// - Wiktionary: articles from the chosen editions, after the dictionaries' own.
import { call, escape } from './bridge.ts';
import { icon } from './icons.ts';
import { avatar, state } from './state.ts';

export type Translation = { text: string; from: string; to: string; translation: string };
export type WikiHit = { id: string; site: string; name: string; title: string };

const dictOrigin = 'https://dict.dr-dict.local';
const languageNames: Record<string, string> = { zh: '中文', en: '英文' };
const latinWord = /[A-Za-zÀ-ɏ]+(?:['’-][A-Za-zÀ-ɏ]+)*/g;
const hanCharacter = /\p{Script=Han}/gu;

/**
 * Whether the text reads as a sentence rather than a headword: five or more words, or three with
 * sentence punctuation; nine or more Chinese characters, or four with punctuation. Phrases such as
 * "a piece of cake" or "一石二鸟" stay headwords.
 */
export function isSentence(text: string): boolean {
  const t = text.trim();
  const words = t.match(latinWord)?.length ?? 0;
  const han = t.match(hanCharacter)?.length ?? 0;
  const punctuated = /[,;:!?，。！？；：、]|\.\s|[.!?。！？]$/.test(t);
  return words >= 5 || (words >= 3 && punctuated) || han >= 9 || (han >= 4 && punctuated);
}

/**
 * 'translate' for a sentence, or for a word the dictionaries do not have; 'setup' when a sentence
 * would use NiuTrans but no key is saved yet; null otherwise.
 */
export function translationMode(text: string, hasResults: boolean): 'translate' | 'setup' | null {
  if (!state.settings.translateSentences) return null;
  const t = text.trim();
  if (!t.match(latinWord) && !t.match(hanCharacter)) return null; // numbers, symbols
  const sentence = isSentence(t);
  if (!sentence && hasResults) return null;
  if (state.settings.translationProvider === 'niutrans' && !state.niutrans.configured) return sentence ? 'setup' : null;
  return 'translate';
}

/** The translation block; `setupHint` (HTML) is shown instead when no key is saved. */
export function translationHtml(mode: 'translate' | 'setup', setupHint: string): string {
  if (mode === 'setup') return `<section class="translation setup">${icon('globe')}<p>${setupHint}</p></section>`;
  return `<section class="translation loading" aria-live="polite" aria-busy="true">
      <header class="translation-header">
        <span class="translation-name">${state.settings.translationProvider === 'niutrans' ? '小牛翻译' : '哔哩哔哩翻译'}</span>
        <span class="translation-direction"></span>
        <button class="translation-copy" type="button" title="复制译文" disabled>
          <span class="translation-copy-icon">${icon('copy')}</span><span class="translation-copy-label">复制</span>
        </button>
      </header>
      <div class="translation-body"><div class="skeleton"><span></span><span></span></div></div>
    </section>`;
}

const translations = new Map<string, Translation>();
/** Fills a translation block; `stillShown` drops the answer once another word is on screen. */
export async function fillTranslation(section: HTMLElement, text: string, stillShown: () => boolean): Promise<void> {
  const { translationProvider, bilibiliModel } = state.settings;
  const key = `${translationProvider}\n${bilibiliModel}\n${text.trim()}`;
  const current = () => stillShown() && state.settings.translationProvider === translationProvider &&
    state.settings.bilibiliModel === bilibiliModel;
  const body = section.querySelector<HTMLElement>('.translation-body')!;
  let result = translations.get(key);
  try {
    if (!result) {
      result = await call<Translation>('translate', { text: text.trim(), provider: translationProvider, model: bilibiliModel });
      translations.set(key, result);
      if (translations.size > 100) translations.delete(translations.keys().next().value!);
    }
  } catch (error) {
    if (!current()) return;
    section.classList.remove('loading');
    section.classList.add('failed');
    section.removeAttribute('aria-busy');
    body.innerHTML = `<p class="translation-error">${escape((error as Error).message)}</p>`;
    return;
  }
  if (!current()) return;
  section.classList.remove('loading');
  section.removeAttribute('aria-busy');
  section.querySelector('.translation-direction')!.textContent =
    `${languageNames[result.from] ?? result.from} → ${languageNames[result.to] ?? result.to}`;
  const paragraph = document.createElement('p');
  paragraph.className = 'translation-text';
  paragraph.lang = result.to === 'zh' ? 'zh-CN' : 'en';
  paragraph.textContent = result.translation;
  body.replaceChildren(paragraph);
  section.dataset.translation = result.translation;
  section.querySelector<HTMLButtonElement>('.translation-copy')!.disabled = false;
}

/** The copy buttons of translation blocks inside `root`. */
export function initTranslation(root: HTMLElement): void {
  root.addEventListener('click', async (event) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('.translation-copy');
    const text = button?.closest<HTMLElement>('.translation')?.dataset.translation;
    if (!button || !text) return;
    const label = button.querySelector('.translation-copy-label')!;
    const glyph = button.querySelector('.translation-copy-icon')!;
    try {
      await navigator.clipboard.writeText(text);
      label.textContent = '已复制';
      glyph.innerHTML = icon('check');
    } catch {
      label.textContent = '复制失败';
    }
    button.classList.add('done');
    window.setTimeout(() => {
      label.textContent = '复制';
      glyph.innerHTML = icon('copy');
      button.classList.remove('done');
    }, 1600);
  });
}

/** Wiktionary is asked for headwords only: no sentences, nothing over 80 characters. */
export function wikiWanted(word: string): boolean {
  const t = word.trim();
  return state.settings.wiktionary.length > 0 && !!t && [...t].length <= 80 && !isSentence(t);
}

const wikiResults = new Map<string, WikiHit[]>();
/** The enabled editions that have a page for `word`, in settings order. */
export async function findWiki(word: string): Promise<WikiHit[]> {
  const sites = state.settings.wiktionary;
  const key = `${sites.join(',')}\n${word}`;
  const known = wikiResults.get(key);
  if (known) return known;
  // An edition that cannot be reached (offline, blocked) is left out without an error.
  const answers = await Promise.all(
    sites.map((site) =>
      call<{ found: boolean; id: string; site: string; name: string; title?: string }>('wiki.lookup', { word, site }).catch(
        () => null,
      ),
    ),
  );
  const hits = answers
    .filter((a) => a?.found && a.title)
    .map((a) => ({ id: a!.id, site: a!.site, name: a!.name, title: a!.title! }));
  if (answers.every((a) => a)) {
    wikiResults.set(key, hits);
    if (wikiResults.size > 100) wikiResults.delete(wikiResults.keys().next().value!);
  }
  return hits;
}

export function wikiSection(hit: WikiHit, theme: string, paper: boolean, collapsed: boolean): string {
  // The id ("@wiki-en") goes in as is: the native side routes on the literal "@wiki-" prefix.
  const src = `${dictOrigin}/${hit.id}/@article?word=${encodeURIComponent(hit.title)}&theme=${theme}`;
  return `<section class="entry wiki loading${collapsed ? ' collapsed' : ''}${paper ? ' paper' : ''}" data-id="${escape(hit.id)}">
      <header class="entry-header">
        <button class="entry-toggle" type="button" aria-expanded="${!collapsed}" title="${collapsed ? '展开' : '折叠'}">
          <span class="chevron" data-icon="chevron-down"></span>
          ${avatar({ id: hit.id, name: hit.name, hasIcon: false }, 18)}
          <span class="entry-name">${escape(hit.name)}</span>
        </button>
        <span class="entry-count">在线</span>
      </header>
      <div class="entry-body">
        <div class="skeleton"><span></span><span></span><span></span></div>
        <iframe title="${escape(hit.name)}" sandbox="allow-scripts allow-same-origin" referrerpolicy="no-referrer" src="${escape(src)}"></iframe>
      </div>
    </section>`;
}
