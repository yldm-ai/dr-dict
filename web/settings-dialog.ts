import { $, call, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { publish, state, type Settings } from './state.ts';

const dialog = $<HTMLDialogElement>('settings-dialog');

function render(): void {
  for (const group of dialog.querySelectorAll<HTMLElement>('.segmented')) {
    const value = String(state.settings[group.dataset.setting as keyof Settings]);
    for (const button of group.querySelectorAll<HTMLButtonElement>('button'))
      button.setAttribute('aria-pressed', String(button.dataset.value === value));
  }
  $<HTMLInputElement>('invert-articles').checked = state.settings.invertArticles;
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
    render();
  } catch (error) {
    toast((error as Error).message, 'error');
  }
}

export function openSettings(): void {
  render();
  if (!dialog.open) dialog.showModal();
}

export function initSettings(): void {
  hydrateIcons(dialog);
  dialog.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    if (target === dialog || target.closest('[data-close]')) {
      dialog.close();
      return;
    }
    const option = target.closest<HTMLButtonElement>('.segmented button');
    const group = option?.closest<HTMLElement>('.segmented');
    if (option && group) void save({ [group.dataset.setting!]: option.dataset.value } as Partial<Settings>);
  });
  $<HTMLInputElement>('invert-articles').addEventListener('change', (event) =>
    void save({ invertArticles: (event.target as HTMLInputElement).checked }),
  );
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
