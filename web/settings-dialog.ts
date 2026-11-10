import { $, call, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { publish, state, type Settings } from './state.ts';

export type Tab = 'library' | 'interface' | 'article' | 'data';
const dialog = $<HTMLDialogElement>('settings-dialog');
const tabs = [...dialog.querySelectorAll<HTMLButtonElement>('.tab')];
let onTabChange: (tab: Tab) => void = () => {};

export function settingsOpen(): boolean {
  return dialog.open;
}
export function currentTab(): Tab {
  return (tabs.find((t) => t.getAttribute('aria-selected') === 'true')?.dataset.tab ?? 'library') as Tab;
}

function select(tab: Tab): void {
  for (const button of tabs) {
    const selected = button.dataset.tab === tab;
    button.setAttribute('aria-selected', String(selected));
    button.tabIndex = selected ? 0 : -1;
  }
  for (const panel of dialog.querySelectorAll<HTMLElement>('.tab-panel')) panel.hidden = panel.dataset.panel !== tab;
  onTabChange(tab);
}

function render(): void {
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[type=radio]'))
    input.checked = String(state.settings[input.name as keyof Settings]) === input.value;
  $<HTMLInputElement>('invert-articles').checked = state.settings.invertArticles;
  // Variant choices only apply while simplified/traditional lookup is on, and need OpenCC's data.
  const conversion = $<HTMLInputElement>('chinese-conversion');
  conversion.checked = state.settings.chineseConversion;
  conversion.disabled = !state.chineseAvailable;
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[name=chineseVariant]'))
    input.disabled = !state.chineseAvailable || !state.settings.chineseConversion;
  if (!state.chineseAvailable)
    $('chinese-note').textContent = '未找到 OpenCC 数据（程序目录下的 opencc 文件夹），简繁查询不可用。';
  $('data-path').textContent = state.dataPath;
}

async function save(patch: Partial<Settings>): Promise<void> {
  // Apply optimistically so the controls respond at once; the native side validates.
  state.settings = { ...state.settings, ...patch };
  publish('settings');
  render();
  try {
    state.settings = await call<Settings>('settings.save', patch);
    publish('settings');
    publish('saved');
    render();
  } catch (error) {
    toast((error as Error).message, 'error');
  }
}

export function openSettings(tab?: Tab): void {
  render();
  select(tab ?? currentTab());
  if (dialog.open) return;
  dialog.showModal();
  // The native title bar darkens together with the page behind the dialog.
  void call('window.dim', { dimmed: true }).catch(() => {});
}

export function initSettings(handlers: { onTabChange: (tab: Tab) => void }): void {
  onTabChange = handlers.onTabChange;
  hydrateIcons(dialog);
  dialog.addEventListener('close', () => void call('window.dim', { dimmed: false }).catch(() => {}));
  dialog.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    if (target === dialog || target.closest('[data-close]')) dialog.close();
    const tab = target.closest<HTMLButtonElement>('.tab');
    if (tab) select(tab.dataset.tab as Tab);
  });
  // Arrow keys move between tabs, as in a native property sheet.
  dialog.querySelector('.tabs')!.addEventListener('keydown', (event) => {
    const key = (event as KeyboardEvent).key;
    if (key !== 'ArrowLeft' && key !== 'ArrowRight') return;
    event.preventDefault();
    const index = tabs.findIndex((t) => t.dataset.tab === currentTab());
    const next = tabs[(index + (key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length];
    select(next.dataset.tab as Tab);
    next.focus();
  });
  dialog.addEventListener('change', (event) => {
    const input = event.target as HTMLInputElement;
    if (input.type === 'radio' && input.checked) void save({ [input.name]: input.value } as Partial<Settings>);
    else if (input.id === 'invert-articles') void save({ invertArticles: input.checked });
    else if (input.id === 'chinese-conversion') void save({ chineseConversion: input.checked });
  });
  $('clear-history').addEventListener('click', async () => {
    try {
      state.history = await call('history.clear');
      publish('history');
      toast('查询历史已清除', 'success');
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  });
}
