// Interface appearance: the colour scheme (built-in palettes live in styles.css as
// body[data-palette]), custom background and accent colours layered over it, and an optional
// background image. The native title bar is told the resulting colours.
import { call } from './bridge.ts';
import { activeTheme, darkThemes, effectiveTheme, luminance, state, type Theme } from './state.ts';

/** Schemes in the order the settings show them, with swatch colours (background, text, accent). */
export const themes: { id: Theme; name: string; bg: string; text: string; accent: string }[] = [
  { id: 'system', name: '跟随系统', bg: '#ffffff', text: '#37352f', accent: '#2b6f62' },
  { id: 'light', name: '浅色', bg: '#ffffff', text: '#37352f', accent: '#2b6f62' },
  { id: 'dark', name: '深色', bg: '#191919', text: '#e8e8e8', accent: '#5fbfab' },
  { id: 'paper', name: '纸张', bg: '#fbf7ef', text: '#4a4036', accent: '#8a5a3a' },
  { id: 'green', name: '豆绿', bg: '#eef3e8', text: '#33402f', accent: '#4c7a47' },
  { id: 'gray', name: '雾灰', bg: '#f3f3f1', text: '#37352f', accent: '#2f6f9f' },
  { id: 'night', name: '夜蓝', bg: '#1b2230', text: '#c9d2de', accent: '#8ab4e0' },
  { id: 'mocha', name: '暖褐', bg: '#211d1a', text: '#e3d9cc', accent: '#d4a373' },
];

export const isHex = (value: string) => /^#[0-9a-f]{6}$/i.test(value);

export function backgroundUrl(name: string): string {
  return `https://dict.dr-dict.local/@background/${encodeURIComponent(name)}`;
}

// Custom colours derive the surrounding greys from themselves, mixed toward the scheme's
// strongest text colour (darker on light backgrounds, lighter on dark ones).
const backgroundVars = (c: string, dark: boolean): Record<string, string> => ({
  '--bg': c,
  '--sidebar': `color-mix(in srgb, ${c} 95%, var(--strong))`,
  '--hover': `color-mix(in srgb, ${c} 92%, var(--strong))`,
  '--selected': `color-mix(in srgb, ${c} 88%, var(--strong))`,
  '--border': `color-mix(in srgb, ${c} 89%, var(--strong))`,
  '--control': dark ? `color-mix(in srgb, ${c} 92%, #ffffff)` : `color-mix(in srgb, ${c} 35%, #ffffff)`,
  '--control-border': `color-mix(in srgb, ${c} 80%, var(--strong))`,
});
const accentVars = (c: string): Record<string, string> => ({
  '--accent': c,
  '--accent-hover': `color-mix(in srgb, ${c} 82%, var(--strong))`,
  '--accent-soft': `color-mix(in srgb, ${c} 16%, var(--bg))`,
  '--on-accent': luminance(c) > 0.45 ? '#111111' : '#ffffff',
});
const customProperties = [
  ...Object.keys(backgroundVars('#000000', false)),
  ...Object.keys(accentVars('#000000')),
  '--bg-image',
  '--bg-image-opacity',
  '--bg-image-blur',
];

export function applyAppearance(): void {
  const body = document.body;
  const settings = state.settings;
  const mode = effectiveTheme();
  const theme = activeTheme();
  body.dataset.theme = mode;
  // A built-in palette applies while the page is in its mode; a custom background colour of the
  // other brightness falls back to the plain light or dark palette underneath it.
  const palette = theme !== 'light' && theme !== 'dark' && darkThemes.has(theme) === (mode === 'dark') ? theme : '';
  if (palette) body.dataset.palette = palette;
  else delete body.dataset.palette;
  for (const name of customProperties) body.style.removeProperty(name);
  const vars: Record<string, string> = {};
  if (isHex(settings.backgroundColor)) Object.assign(vars, backgroundVars(settings.backgroundColor, mode === 'dark'));
  if (isHex(settings.accentColor)) Object.assign(vars, accentVars(settings.accentColor));
  if (settings.backgroundImage) {
    vars['--bg-image'] = `url("${backgroundUrl(settings.backgroundImage)}")`;
    vars['--bg-image-opacity'] = String(settings.backgroundOpacity);
    vars['--bg-image-blur'] = `${settings.backgroundBlur}px`;
    body.dataset.bgImage = '';
  } else delete body.dataset.bgImage;
  for (const [name, value] of Object.entries(vars)) body.style.setProperty(name, value);
  syncWindowChrome(mode === 'dark');
}

// Computed colours come back as rgb(…) or, for color-mix(), as color(srgb r g b) in 0–1.
function resolve(variable: string): string {
  probe.style.color = `var(${variable})`;
  const value = getComputedStyle(probe).color;
  const numbers = value.match(/[\d.]+/g)?.map(Number) ?? [];
  const channels = value.startsWith('color(') ? numbers.slice(0, 3).map((n) => n * 255) : numbers.slice(0, 3);
  if (channels.length < 3) return '';
  return `#${channels.map((n) => Math.round(Math.min(255, Math.max(0, n))).toString(16).padStart(2, '0')).join('')}`;
}
const probe = document.createElement('span');
probe.hidden = true;
let lastChrome = '';
// The main window colours its title bar; the Ctrl+C+C popup colours the edge around its WebView.
let chromeMethod = 'window.chrome';
export function setChromeMethod(method: string): void {
  chromeMethod = method;
}
function syncWindowChrome(dark: boolean): void {
  if (!probe.isConnected) document.body.append(probe);
  const chrome = { dark, background: resolve('--bg'), text: resolve('--text'), hover: resolve('--hover') };
  const signature = JSON.stringify(chrome);
  if (signature === lastChrome || !chrome.background) return;
  lastChrome = signature;
  void call(chromeMethod, chrome).catch(() => {});
}
/** The native side resets its colours when Windows switches theme; send them again. */
export function resendWindowChrome(): void {
  lastChrome = '';
  syncWindowChrome(effectiveTheme() === 'dark');
}
