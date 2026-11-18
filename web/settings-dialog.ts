import { $, call, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { publish, state, type Settings, type Theme } from './state.ts';
import { backgroundUrl, isHex, themes } from './appearance.ts';
import { ColorPicker } from './color-picker.ts';
import { agentTabShown, initAgentSettings, renderAgentSettings } from './agent-settings.ts';
import { speak } from './speech.ts';
import { SelectControl } from './select-control.ts';

export type Tab = 'library' | 'interface' | 'article' | 'online' | 'agent' | 'data';
const dialog = $<HTMLDialogElement>('settings-dialog');
const tabs = [...dialog.querySelectorAll<HTMLButtonElement>('.tab')];
let onTabChange: (tab: Tab) => void = () => {};
const onlineControls: SelectControl[] = [];
let testingTranslation = false;

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
  if (tab === 'agent') agentTabShown();
  onTabChange(tab);
}

function render(): void {
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[type=radio]'))
    input.checked = String(state.settings[input.name as keyof Settings]) === input.value;
  $<HTMLInputElement>('invert-articles').checked = state.settings.invertArticles;
  $<HTMLInputElement>('scan-popup').checked = state.settings.scanPopup;
  const percent = Math.round(state.settings.volume * 100);
  $<HTMLInputElement>('volume').value = String(percent);
  $<HTMLInputElement>('volume').style.setProperty('--fill', `${percent}%`);
  $('volume-value').textContent = `${percent}%`;
  // Variant choices only apply while simplified/traditional lookup is on, and need OpenCC's data.
  const conversion = $<HTMLInputElement>('chinese-conversion');
  conversion.checked = state.settings.chineseConversion;
  conversion.disabled = !state.chineseAvailable;
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[name=chineseVariant]'))
    input.disabled = !state.chineseAvailable || !state.settings.chineseConversion;
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[name=traditionalVariant]'))
    input.disabled = !state.chineseAvailable;
  if (!state.chineseAvailable)
    $('chinese-note').textContent = '未找到 OpenCC 数据（程序目录下的 opencc 文件夹），简繁查询不可用。';
  $('data-path').textContent = state.dataPath;
  renderAppearance();
  renderOnline();
  renderAgentSettings();
}

function renderOnline(): void {
  $<HTMLInputElement>('translate-sentences').checked = state.settings.translateSentences;
  const bilibili = state.settings.translationProvider === 'bilibili';
  $<HTMLSelectElement>('translation-provider').value = state.settings.translationProvider;
  $<HTMLSelectElement>('bilibili-model').value = state.settings.bilibiliModel;
  for (const control of onlineControls) control.sync();
  $('bilibili-options').hidden = !bilibili;
  $('niutrans-options').hidden = bilibili;
  for (const input of dialog.querySelectorAll<HTMLInputElement>('input[data-wiki]'))
    input.checked = state.settings.wiktionary.includes(input.dataset.wiki!);
  const { configured, appId } = state.niutrans;
  const appIdInput = $<HTMLInputElement>('niutrans-app-id');
  if (configured && !appIdInput.value) appIdInput.value = appId;
  $<HTMLInputElement>('niutrans-api-key').placeholder = configured ? '已保存，留空则不修改' : '';
  const status = $('niutrans-status');
  status.textContent = configured ? `已配置 · App ID ${appId}` : '尚未填写 App ID 和 API Key';
  status.dataset.state = configured ? 'ready' : 'missing';
  $<HTMLButtonElement>('niutrans-clear').disabled = !configured;
  $<HTMLButtonElement>('translation-test').disabled = testingTranslation || (!bilibili && !configured);
}

function initOnline(): void {
  for (const id of ['translation-provider', 'bilibili-model'])
    onlineControls.push(new SelectControl($<HTMLSelectElement>(id)));
  $('translation-provider').addEventListener('change', (event) =>
    void save({ translationProvider: (event.target as HTMLSelectElement).value as Settings['translationProvider'] }));
  $('bilibili-model').addEventListener('change', (event) =>
    void save({ bilibiliModel: (event.target as HTMLSelectElement).value as Settings['bilibiliModel'] }));
  const appId = $<HTMLInputElement>('niutrans-app-id');
  const apikey = $<HTMLInputElement>('niutrans-api-key');
  const saveKey = async (clear: boolean) => {
    const id = clear ? '' : appId.value.trim();
    const key = clear ? '' : apikey.value.trim();
    if (!clear && !key && state.niutrans.configured && id === state.niutrans.appId) {
      toast('密钥没有变化', 'info');
      return;
    }
    if (!clear && (!id || !key)) {
      toast('请填写 App ID 和 API Key', 'error');
      return;
    }
    try {
      state.niutrans = await call('niutrans.save', { appId: id, apikey: key });
      apikey.value = '';
      if (clear) appId.value = '';
      renderOnline();
      publish('niutrans');
      toast(clear ? '已清除小牛翻译密钥' : '已加密保存小牛翻译密钥', 'success');
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  };
  $('niutrans-save').addEventListener('click', () => void saveKey(false));
  $('niutrans-clear').addEventListener('click', () => void saveKey(true));
  apikey.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') void saveKey(false);
  });
  const test = $<HTMLButtonElement>('translation-test');
  test.addEventListener('click', async () => {
    testingTranslation = true;
    test.disabled = true;
    test.textContent = '测试中…';
    try {
      const result = await call<{ translation: string }>('translation.test');
      toast(`测试成功：${result.translation}`, 'success');
    } catch (error) {
      toast(`测试失败：${(error as Error).message}`, 'error');
    } finally {
      testingTranslation = false;
      test.textContent = '测试翻译';
      renderOnline();
    }
  });
  $('niutrans-console').addEventListener('click', () =>
    call('open.external', { url: 'https://niutrans.com/cloud/api/list' }).catch((e) => toast(e.message, 'error')),
  );
}

// Colour picker presets: light and dark backgrounds; Notion-like accents.
const backgroundPresets = ['#ffffff', '#fbf7ef', '#eef3e8', '#f3f3f1', '#eaf1f8', '#f7eef0', '#191919', '#1b2230', '#211d1a', '#1c2420'];
const accentPresets = ['#2b6f62', '#2f6f9f', '#4c7a47', '#8a5a3a', '#9065b0', '#c14c8a', '#d44c47', '#d9730d', '#cb912f', '#787774'];
const picker = new ColorPicker(dialog);

function renderAppearance(): void {
  const s = state.settings;
  $('theme-grid').innerHTML = themes
    .map(
      (t) => `<button class="theme-card" type="button" data-theme-id="${t.id}" aria-pressed="${s.theme === t.id}">
        <span class="theme-sample" style="--sample-bg:${t.bg};--sample-text:${t.text};--sample-accent:${t.accent}">Aa</span>${t.name}
      </button>`,
    )
    .join('');
  const colorButton = (id: string, value: string, fallback: string) => {
    const button = $(id);
    const chip = button.querySelector<HTMLElement>('.color-chip')!;
    const custom = isHex(value);
    chip.classList.toggle('none', !custom);
    chip.style.setProperty('--chip', custom ? value : 'transparent');
    button.querySelector('.color-text')!.textContent = custom ? value.toUpperCase() : fallback;
    $<HTMLButtonElement>(`${id}-reset`).disabled = !custom;
  };
  colorButton('bg-color', s.backgroundColor, '跟随配色');
  colorButton('accent-color', s.accentColor, '跟随配色');
  const preview = $('bg-image-preview');
  preview.style.backgroundImage = s.backgroundImage ? `url("${backgroundUrl(s.backgroundImage)}")` : '';
  preview.textContent = s.backgroundImage ? '' : '无';
  $<HTMLButtonElement>('bg-image-clear').disabled = !s.backgroundImage;
  const opacity = Math.round(s.backgroundOpacity * 100);
  const opacityInput = $<HTMLInputElement>('bg-opacity');
  opacityInput.value = String(opacity);
  opacityInput.style.setProperty('--fill', `${((opacity - 5) / 95) * 100}%`);
  opacityInput.disabled = !s.backgroundImage;
  $('bg-opacity-value').textContent = `${opacity}%`;
  const blurInput = $<HTMLInputElement>('bg-blur');
  blurInput.value = String(s.backgroundBlur);
  blurInput.style.setProperty('--fill', `${(s.backgroundBlur / 40) * 100}%`);
  blurInput.disabled = !s.backgroundImage;
  $('bg-blur-value').textContent = `${s.backgroundBlur}px`;
}

function initAppearance(): void {
  dialog.addEventListener('close', () => picker.close(false));
  $('theme-grid').addEventListener('click', (event) => {
    const card = (event.target as HTMLElement).closest<HTMLElement>('[data-theme-id]');
    if (card) void save({ theme: card.dataset.themeId as Theme, backgroundColor: '', accentColor: '' });
  });
  const colorSetting = (id: string, key: 'backgroundColor' | 'accentColor', title: string, presets: string[]) => {
    const button = $(id);
    button.addEventListener('click', () => {
      if (picker.isOpen) {
        picker.close();
        return;
      }
      const current = state.settings[key] || getComputedStyle(document.body).getPropertyValue(key === 'backgroundColor' ? '--bg' : '--accent').trim();
      picker.open(button, { title, color: current, presets, onPick: (color) => void save({ [key]: color }) });
    });
    $(`${id}-reset`).addEventListener('click', () => void save({ [key]: '' }));
  };
  colorSetting('bg-color', 'backgroundColor', '背景颜色', backgroundPresets);
  colorSetting('accent-color', 'accentColor', '主题色', accentPresets);
  $('bg-image-choose').addEventListener('click', async () => {
    try {
      const result = await call<Settings & { cancelled?: boolean }>('background.choose');
      if (result.cancelled) return;
      state.settings = result;
      publish('settings');
      render();
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  });
  $('bg-image-clear').addEventListener('click', async () => {
    try {
      state.settings = await call<Settings>('background.clear');
      publish('settings');
      render();
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  });
  // Sliders preview while dragging and save on release, like the volume.
  const slider = (id: string, apply: (value: number) => Partial<Settings>) => {
    const input = $<HTMLInputElement>(id);
    input.addEventListener('input', () => {
      state.settings = { ...state.settings, ...apply(Number(input.value)) };
      renderAppearance();
      publish('settings');
    });
    input.addEventListener('change', () => void save(apply(Number(input.value))));
  };
  slider('bg-opacity', (value) => ({ backgroundOpacity: value / 100 }));
  slider('bg-blur', (value) => ({ backgroundBlur: value }));
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
  initAppearance();
  initOnline();
  initAgentSettings(save);
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
    else if (input.id === 'scan-popup') void save({ scanPopup: input.checked });
    else if (input.id === 'chinese-conversion') void save({ chineseConversion: input.checked });
    else if (input.id === 'translate-sentences') void save({ translateSentences: input.checked });
    else if (input.dataset.wiki)
      void save({
        wiktionary: [...dialog.querySelectorAll<HTMLInputElement>('input[data-wiki]')]
          .filter((i) => i.checked)
          .map((i) => i.dataset.wiki!),
      });
    else if (input.id === 'volume') void save({ volume: Number(input.value) / 100 });
  });
  // While dragging, the label, the slider fill and open articles follow at once; the value is
  // saved when the slider is released (the change event above).
  $<HTMLInputElement>('volume').addEventListener('input', (event) => {
    const input = event.target as HTMLInputElement;
    state.settings = { ...state.settings, volume: Number(input.value) / 100 };
    input.style.setProperty('--fill', `${input.value}%`);
    $('volume-value').textContent = `${input.value}%`;
    publish('settings');
  });
  $('volume-test').addEventListener('click', () => {
    speak('This is how loud pronunciations will be.', 'en-US');
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
