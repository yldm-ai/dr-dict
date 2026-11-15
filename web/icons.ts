// Line icons drawn for DR Dict on a 24px grid; they inherit the current text color.
const paths: Record<string, string> = {
  search: '<circle cx="11" cy="11" r="6.5"/><path d="M16 16l4.5 4.5"/>',
  x: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  // Filled through CSS (.favorite-button[aria-pressed=true] path { fill: currentColor }).
  star: '<path d="M12 3.8l2.5 5.2 5.7.8-4.1 4 1 5.7-5.1-2.7-5.1 2.7 1-5.7-4.1-4 5.7-.8z"/>',
  plus: '<path d="M12 5.5v13M5.5 12h13"/>',
  folder: '<path d="M3.5 7.5a2 2 0 0 1 2-2h3.8l2 2h7.2a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"/>',
  book: '<path d="M5.5 5.5a2 2 0 0 1 2-2h10v14h-10a2 2 0 0 0-2 2z"/><path d="M5.5 19.5a2 2 0 0 0 2 2h10v-4"/>',
  library: '<path d="M5 4.5v15M9.5 4.5v15"/><path d="M13.6 5.4l3.7-1 3.9 14.4-3.7 1z"/>',
  layout: '<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M3.5 9h17M9 9v10.5"/>',
  'chevron-down': '<path d="M7 10l5 5 5-5"/>',
  'chevron-right': '<path d="M10 7l5 5-5 5"/>',
  'arrow-left': '<path d="M19 12H5.5M11 6l-6 6 6 6"/>',
  'arrow-right': '<path d="M5 12h13.5M13 6l6 6-6 6"/>',
  'arrow-up': '<path d="M12 19V5.5M6 11l6-6 6 6"/>',
  'arrow-down': '<path d="M12 5v13.5M6 13l6 6 6-6"/>',
  trash: '<path d="M4.5 7h15M9.5 7V4.5h5V7M6.5 7l.9 12.5h9.2l.9-12.5"/>',
  refresh: '<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.5 4.5v4h-4"/>',
  info: '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5.5M12 7.75v.01"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  pin: '<path d="M9.5 4.5h5l-.8 5.2 3.3 3.3H7l3.3-3.3z"/><path d="M12 13v6.5"/>',
  'open-main': '<path d="M13.5 4.5h6v6M19.5 4.5l-8 8"/><path d="M17.5 13.5v4a2 2 0 0 1-2 2h-9a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2h4"/>',
  copy: '<rect x="8.5" y="8.5" width="11" height="11" rx="2"/><path d="M15.5 8.5v-2a2 2 0 0 0-2-2h-7a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2h2"/>',
  alert: '<path d="M12 4.5l8.5 15h-17z"/><path d="M12 10v4.5M12 17.25v.01"/>',
  clock: '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
  sound: '<path d="M4.5 9.5h3l4.5-4v13l-4.5-4h-3z"/><path d="M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11"/>',
  // A four-point spark for AI 助查.
  sparkle: '<path d="M12 3.5c.6 4.3 3.2 6.9 7.5 7.5-4.3.6-6.9 3.2-7.5 7.5-.6-4.3-3.2-6.9-7.5-7.5 4.3-.6 6.9-3.2 7.5-7.5z"/><path d="M18.5 15.5v4M16.5 17.5h4"/>',
  stop: '<rect x="7" y="7" width="10" height="10" rx="1.5"/>',
  'pin-off': '<path d="M9.5 4.5h5l-.8 5.2 3.3 3.3H7l3.3-3.3z"/><path d="M12 13v6.5M4.5 4.5l15 15"/>',
  globe: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.3 2.4 3.4 5.2 3.4 8.5s-1.1 6.1-3.4 8.5c-2.3-2.4-3.4-5.2-3.4-8.5s1.1-6.1 3.4-8.5z"/>',
};

export function icon(name: string): string {
  // DR Reader keeps the Fluent gear (U+E713, also present in Segoe MDL2 Assets) for settings.
  if (name === 'settings') return '<span class="icon fluent-icon" aria-hidden="true">&#xE713;</span>';
  const body = paths[name] ?? paths.info;
  return `<svg class="icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;
}

export function hydrateIcons(root: ParentNode = document): void {
  for (const element of root.querySelectorAll<HTMLElement>('[data-icon]')) {
    if (element.dataset.iconReady) continue;
    element.insertAdjacentHTML('afterbegin', icon(element.dataset.icon ?? ''));
    element.dataset.iconReady = '1';
  }
}

// The app mark: an open book with a ribbon, matching resources/dr-dict.svg.
export const brandMark = `<svg viewBox="0 0 256 256" aria-hidden="true"><rect x="8" y="8" width="240" height="240" rx="56" fill="currentColor"/><path d="M128 86C106 72 82 72 58 82V178C82 168 106 168 128 182C150 168 174 168 198 178V82C174 72 150 72 128 86ZM128 86V182" fill="none" stroke="#fff" stroke-width="11" stroke-linecap="round" stroke-linejoin="round"/><path d="M158 77V122L170 112L182 122V74" fill="#fff"/></svg>`;
