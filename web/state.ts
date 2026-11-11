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
export type Settings = {
  theme: 'light' | 'dark' | 'system';
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
};
export type HistoryItem = { word: string; at: number };

export const state = {
  dictionaries: [] as Dictionary[],
  settings: {
    theme: 'system',
    shell: 'mica',
    invertArticles: true,
    articleWidth: 'comfortable',
    chineseConversion: false,
    chineseVariant: 't',
    traditionalVariant: 't',
    scope: [],
    volume: 0.22,
  } as Settings,
  history: [] as HistoryItem[],
  favorites: [] as HistoryItem[],
  dataPath: '',
  // Whether OpenCC's data shipped with the app, so simplified/traditional lookup can work.
  chineseAvailable: false,
  importing: false,
};

// 'settings' fires for every change, including optimistic ones; 'saved' only once the native
// side has stored them, which is when lookups see the new values.
type Topic = 'dictionaries' | 'settings' | 'saved' | 'history' | 'favorites';
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
export function effectiveTheme(): 'light' | 'dark' {
  if (state.settings.theme === 'system') return systemDark ? 'dark' : 'light';
  return state.settings.theme;
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
