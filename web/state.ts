export type Dictionary = {
  id: string;
  name: string;
  format: 'mdict' | 'stardict';
  path: string;
  enabled: boolean;
  entries: number;
  resources: number;
  hasIcon: boolean;
  status: 'ready' | 'missing' | 'stale' | 'removed';
  // Removed dictionaries stay listed in a collapsed group, disabled and without an index.
  removed?: boolean;
  fileMissing?: boolean;
};
/** Dictionaries in the library proper (not the removed group). */
export function activeDictionaries(): Dictionary[] {
  return state.dictionaries.filter((d) => !d.removed);
}
export type TraditionalVariant = 't' | 'tw' | 'twp' | 'hk';
export const traditionalNames: Record<TraditionalVariant, string> = {
  t: '繁体（OpenCC 标准）',
  tw: '台湾正体',
  twp: '台湾正体及台湾用词',
  hk: '香港繁体',
};
// Colour schemes: following Windows, plain light and dark, and built-in palettes (styles.css).
export type Theme = 'system' | 'light' | 'dark' | 'paper' | 'green' | 'gray' | 'night' | 'mocha';
export const darkThemes = new Set<Theme>(['dark', 'night', 'mocha']);
export type Settings = {
  theme: Theme;
  // "" uses the scheme's own colour; otherwise "#rrggbb".
  backgroundColor: string;
  accentColor: string;
  // File name under <data>/backgrounds, served at https://dict.dr-dict.local/@background/<name>.
  backgroundImage: string;
  backgroundOpacity: number; // 0–1
  backgroundBlur: number; // CSS px
  // Ctrl+C+C anywhere opens the lookup popup.
  scanPopup: boolean;
  shell: 'mica' | 'direct2d';
  invertArticles: boolean;
  articleWidth: 'narrow' | 'comfortable' | 'wide';
  chineseConversion: boolean;
  chineseVariant: 't' | 'tw' | 'twp' | 'hk' | 'all';
  // What the 繁 button in an article header converts to by default.
  traditionalVariant: TraditionalVariant;
  // Dictionary ids searched from the search box; empty means all enabled dictionaries.
  scope: string[];
  // Pronunciation volume, 0–1.
  volume: number;
  // 小牛翻译 translates sentences, and words no dictionary has (zh ↔ en).
  translateSentences: boolean;
  // Wiktionary editions shown after the dictionaries ("en", "zh").
  wiktionary: string[];
  // AI 助查 through a local agent CLI: off by default. The top bar button can be hidden; the
  // right-hand panel hides itself on a click elsewhere unless pinned.
  agentEnabled: boolean;
  agentButton: boolean;
  agentPinned: boolean;
  agent: AgentChoice;
  agentModels: Partial<Record<AgentId, string>>;
  agentEfforts: Partial<Record<AgentId, string>>;
};
/** Local agent CLIs that can answer questions (engine/include/dr/agent.h). */
export type AgentId = 'codex' | 'claude' | 'opencode';
export type AgentChoice = 'auto' | AgentId;
/** A model an agent CLI reports (`agent.models`); `id` is what its `--model` takes. */
export type AgentModelInfo = {
  id: string;
  name: string;
  description: string;
  default: boolean;
  efforts: string[];
  defaultEffort: string;
};
/** 小牛翻译 credentials as the native side reports them; the key itself never reaches the page. */
export type NiutransStatus = { configured: boolean; appId: string };
export type HistoryItem = { word: string; at: number };

export const state = {
  dictionaries: [] as Dictionary[],
  settings: {
    theme: 'system',
    backgroundColor: '',
    accentColor: '',
    backgroundImage: '',
    backgroundOpacity: 0.35,
    backgroundBlur: 0,
    scanPopup: true,
    shell: 'mica',
    invertArticles: true,
    articleWidth: 'comfortable',
    chineseConversion: false,
    chineseVariant: 't',
    traditionalVariant: 't',
    scope: [],
    volume: 0.22,
    translateSentences: true,
    wiktionary: ['en'],
    agentEnabled: false,
    agentButton: true,
    agentPinned: false,
    agent: 'auto',
    agentModels: {},
    agentEfforts: {},
  } as Settings,
  niutrans: { configured: false, appId: '' } as NiutransStatus,
  history: [] as HistoryItem[],
  favorites: [] as HistoryItem[],
  dataPath: '',
  // Whether OpenCC's data shipped with the app, so simplified/traditional lookup can work.
  chineseAvailable: false,
  importing: false,
};

// 'settings' fires for every change, including optimistic ones; 'saved' only once the native
// side has stored them, which is when lookups see the new values.
type Topic = 'dictionaries' | 'settings' | 'saved' | 'history' | 'favorites' | 'niutrans';
const subscribers = new Map<Topic, Set<() => void>>();
export function subscribe(topic: Topic, handler: () => void): void {
  if (!subscribers.has(topic)) subscribers.set(topic, new Set());
  subscribers.get(topic)!.add(handler);
}
export function publish(topic: Topic): void {
  for (const handler of subscribers.get(topic) ?? []) handler();
}

// The WebView is pinned to prefers-color-scheme: light so dictionaries never apply their own dark
// styles under our inversion; the Windows theme comes from the native side instead.
let systemDark = false;
const systemThemeHandlers = new Set<() => void>();
export function setSystemDark(dark: boolean): void {
  if (dark === systemDark) return;
  systemDark = dark;
  for (const handler of systemThemeHandlers) handler();
}
/** Relative luminance (WCAG) of "#rrggbb". */
export function luminance(hex: string): number {
  const [r, g, b] = [1, 3, 5].map((i) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
/** The scheme in use, with "system" resolved from the Windows theme. */
export function activeTheme(): Exclude<Theme, 'system'> {
  const theme = state.settings.theme;
  if (theme === 'system') return systemDark ? 'dark' : 'light';
  return theme;
}
/**
 * Light or dark for text, articles and the window: a custom background colour decides by its
 * own brightness (light text on a dark colour), otherwise the scheme does.
 */
export function effectiveTheme(): 'light' | 'dark' {
  const custom = state.settings.backgroundColor;
  if (/^#[0-9a-f]{6}$/i.test(custom)) return luminance(custom) < 0.18 ? 'dark' : 'light';
  return darkThemes.has(activeTheme()) ? 'dark' : 'light';
}
export function onSystemThemeChange(handler: () => void): void {
  systemThemeHandlers.add(handler);
}

export const formatNames: Record<string, string> = { mdict: 'MDict', stardict: 'StarDict' };

const avatarHues = [212, 168, 28, 280, 350, 98, 190, 248];
// Dictionaries without an icon get a lettered tile tinted from their name.
export function avatar(dict: Pick<Dictionary, 'id' | 'name' | 'hasIcon'>, size = 20): string {
  if (dict.hasIcon)
    return `<img class="avatar" width="${size}" height="${size}" alt="" src="https://dict.dr-dict.local/${dict.id}/@icon">`;
  let hash = 0;
  for (const c of dict.name) hash = (hash * 31 + c.codePointAt(0)!) >>> 0;
  const letter = [...dict.name.trim()][0]?.toUpperCase() ?? '?';
  const hue = avatarHues[hash % avatarHues.length];
  return `<span class="avatar letter" style="--hue:${hue};width:${size}px;height:${size}px">${letter.replace(/[<>&"]/g, '')}</span>`;
}
