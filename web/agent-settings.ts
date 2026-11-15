// 设置 → AI 助查. Ported from DR Reader's web/settings.ts (syncAgent, loadAgentModels,
// detectAgents and their helpers) and its SelectControl popovers. The CLIs are only detected and asked for
// their models while the feature is on and the tab is open.
import { $, call, escape, toast } from './bridge.ts';
import { state, type AgentChoice, type AgentId, type AgentModelInfo, type Settings } from './state.ts';
import { SelectControl } from './select-control.ts';

type Save = (patch: Partial<Settings>) => Promise<void>;
const agents: [AgentId, string][] = [
  ['codex', 'Codex'],
  ['claude', 'Claude Code'],
  ['opencode', 'opencode'],
];
// Mirror validate_agent_model and validate_agent_effort in engine/src/agent.cpp.
const modelPattern = /^[A-Za-z0-9._:/@+\-[\]]{0,128}$/;
const effortPattern = /^[A-Za-z0-9_-]{0,32}$/;
// Neither a valid model id nor an effort, so it can stand for "type another one" in the lists.
const CUSTOM = '*custom';
const effortNames: Record<string, string> = { none: '不思考', minimal: '最低', low: '低', medium: '中', high: '高', xhigh: '很高', max: '最高', ultra: '极限' };
const effortLabel = (effort: string) => (effortNames[effort] ? `${effortNames[effort]}（${effort}）` : effort);
// A display name that only restates the id ("GPT-6.1-Sol" for gpt-6.1-sol) is shown alone.
const plain = (text: string) => text.toLowerCase().replace(/[\s_-]/g, '');
const modelLabel = (m: AgentModelInfo) => (plain(m.name) === plain(m.id) ? m.name : `${m.name}（${m.id}）`);

const modelLists = new Map<AgentId, { models?: AgentModelInfo[]; error?: string; loading?: boolean }>();
const custom = { model: false, effort: false };
let save: Save = async () => {};
let detected = false;
const controls = new Map<string, SelectControl>();

const select = (id: string) => $<HTMLSelectElement>(id);
const field = (id: string) => $<HTMLInputElement>(id);

export function renderAgentSettings(): void {
  const s = state.settings;
  field('agent-enabled').checked = s.agentEnabled;
  field('agent-button').checked = s.agentButton;
  field('agent-button').disabled = !s.agentEnabled;
  const options = $('agent-options');
  options.classList.toggle('disabled', !s.agentEnabled);
  for (const control of options.querySelectorAll<HTMLInputElement | HTMLSelectElement | HTMLButtonElement>('select, input, button'))
    control.disabled = !s.agentEnabled;
  if (s.agentEnabled) syncAgent();
  else {
    $('agent-status').innerHTML = '<li><small>打开 AI 助查后检测本机工具</small></li>';
    $('agent-model-status').textContent = '';
    $('agent-effort-status').textContent = '';
  }
  for (const control of controls.values()) control.sync();
}

/** The tab was shown: look for the CLIs and ask the chosen one for its models. */
export function agentTabShown(): void {
  if (!state.settings.agentEnabled) return;
  detect(!detected);
  loadModels();
}

function syncAgent(): void {
  const s = state.settings;
  const agent = s.agent || 'auto';
  const auto = agent === 'auto';
  select('agent-choice').value = agent;
  controls.get('choice')?.sync();
  const list = auto ? undefined : modelLists.get(agent);
  const models = list?.models;
  const savedModel = auto ? '' : (s.agentModels?.[agent] ?? '');
  const savedEffort = auto ? '' : (s.agentEfforts?.[agent] ?? '');
  const fallback = models?.find((m) => m.default);
  const model = savedModel ? models?.find((m) => m.id === savedModel) : fallback;
  const modelChoices = [
    { value: '', label: fallback ? `默认（${fallback.name}）` : '工具的默认模型' },
    ...(models ?? []).map((m) => ({ value: m.id, label: modelLabel(m) })),
  ];
  const effortChoices = [
    { value: '', label: model?.defaultEffort ? `默认（${effortNames[model.defaultEffort] ?? model.defaultEffort}）` : '模型的默认强度' },
    ...(model?.efforts ?? []).map((e) => ({ value: e, label: effortLabel(e) })),
  ];
  syncChoice('model', auto, modelChoices, savedModel, '其他模型…');
  syncChoice('effort', auto, effortChoices, savedEffort, '其他强度…');
  $<HTMLButtonElement>('agent-model-refresh').disabled = auto || Boolean(list?.loading);
  const status = $('agent-model-status');
  status.textContent = auto
    ? '自动选择时使用各工具的默认模型'
    : list?.loading
      ? '正在查询可用模型…'
      : list?.error
        ? `无法获取模型列表：${list.error}`
        : models
          ? `共 ${models.length} 个可用模型`
          : '打开此页时查询该工具的可用模型';
  status.classList.toggle('agent-error', Boolean(!auto && list?.error && !list.loading));
  $('agent-effort-status').textContent = auto
    ? '自动选择时使用各模型的默认强度'
    : !model
      ? '按模型的默认强度，也可手动指定'
      : model.efforts.length
        ? `${model.name} 支持 ${model.efforts.length} 档`
        : `${model.name} 不提供强度选项`;
}

/** One list-plus-other pair: the select, its 其他… row and that row's input. */
function syncChoice(kind: 'model' | 'effort', auto: boolean, choices: { value: string; label: string }[], saved: string, other: string): void {
  const list = select(`agent-${kind}`);
  const input = field(`agent-${kind}-custom`);
  if (auto) choices = [{ value: '', label: '先选择具体的工具' }];
  else {
    if (saved && !choices.some((c) => c.value === saved)) choices.push({ value: saved, label: `${saved}（自定义）` });
    choices.push({ value: CUSTOM, label: other });
  }
  controls.get(kind)!.setChoices(choices);
  list.value = custom[kind] && !auto ? CUSTOM : saved;
  list.disabled = auto;
  controls.get(kind)!.sync();
  $(`agent-${kind}-custom-row`).hidden = auto || !custom[kind];
  if (document.activeElement !== input) input.value = saved;
}

/** Asks the chosen CLI for its models once per session; the refresh button asks again. */
function loadModels(force = false): void {
  const agent = state.settings.agent || 'auto';
  if (agent === 'auto' || !state.settings.agentEnabled) return;
  const known = modelLists.get(agent);
  if (known?.loading || (known?.models && !force)) return;
  modelLists.set(agent, { ...known, loading: true });
  syncAgent();
  call<{ models: AgentModelInfo[] }>('agent.models', { agent })
    .then(
      (result) => modelLists.set(agent, { models: result.models }),
      (error) => modelLists.set(agent, { models: known?.models, error: (error as Error).message }),
    )
    .finally(() => renderAgentSettings());
}

function choose(kind: 'model' | 'effort'): void {
  const agent = state.settings.agent || 'auto';
  if (agent === 'auto') return;
  const value = select(`agent-${kind}`).value;
  if (value === CUSTOM) {
    custom[kind] = true;
    syncAgent();
    field(`agent-${kind}-custom`).focus();
    field(`agent-${kind}-custom`).select();
    return;
  }
  custom[kind] = false;
  void saveOption(agent, kind, value);
}

function saveCustom(kind: 'model' | 'effort'): void {
  const s = state.settings;
  const agent = s.agent || 'auto';
  if (agent === 'auto' || !custom[kind]) return;
  const input = field(`agent-${kind}-custom`);
  const value = input.value.trim();
  const [pattern, message] =
    kind === 'model' ? [modelPattern, '模型名称只能包含字母、数字和 . _ : / @ + - [ ]'] : [effortPattern, '思考强度只能包含字母、数字、_ 和 -'];
  if (!pattern.test(value)) {
    toast(message, 'error');
    input.value = (kind === 'model' ? s.agentModels : s.agentEfforts)?.[agent] ?? '';
    return;
  }
  custom[kind] = false;
  input.blur();
  void saveOption(agent, kind, value);
}

/** A new model drops a saved effort it is known not to accept. */
async function saveOption(agent: AgentId, kind: 'model' | 'effort', value: string): Promise<void> {
  const s = state.settings;
  if (kind === 'effort') return save({ agentEfforts: { ...s.agentEfforts, [agent]: value } });
  const models = modelLists.get(agent)?.models;
  const model = value ? models?.find((m) => m.id === value) : models?.find((m) => m.default);
  const effort = s.agentEfforts?.[agent] ?? '';
  const patch: Partial<Settings> = { agentModels: { ...s.agentModels, [agent]: value } };
  if (effort && model && !model.efforts.includes(effort)) {
    patch.agentEfforts = { ...s.agentEfforts, [agent]: '' };
    custom.effort = false;
  }
  return save(patch);
}

/**
 * Shows which agent CLIs this machine has. A manual check also retries model lists that failed,
 * e.g. before a CLI was installed.
 */
function detect(manual = false): void {
  const list = $('agent-status');
  const button = $<HTMLButtonElement>('agent-detect');
  if (button.dataset.busy) return;
  if (manual || !list.childElementCount) list.innerHTML = '<li><small>正在检测…</small></li>';
  button.dataset.busy = '1';
  button.classList.add('spinning');
  if (manual) for (const [id, known] of modelLists) if (known.error && !known.loading) modelLists.delete(id);
  call<Record<AgentId, { path: string } | null>>('agent.detect')
    .then(
      (found) => {
        detected = true;
        list.replaceChildren(
          ...agents.map(([id, name]) => {
            const item = document.createElement('li');
            item.dataset.found = String(Boolean(found[id]));
            item.innerHTML = `<span>${name}</span><small>${found[id] ? `已找到 · ${escape(found[id]!.path)}` : '未找到'}</small>`;
            return item;
          }),
        );
      },
      (error) => {
        list.innerHTML = `<li><small>${escape((error as Error).message)}</small></li>`;
      },
    )
    .finally(() => {
      delete button.dataset.busy;
      button.classList.remove('spinning');
      if (manual) {
        renderAgentSettings();
        loadModels();
      }
    });
}

export function initAgentSettings(saveSettings: Save): void {
  save = saveSettings;
  controls.set('choice', new SelectControl(select('agent-choice')));
  controls.set('model', new SelectControl(select('agent-model'), {
    placeholder: '搜索模型', count: (n) => `${n} 个模型，输入名称缩小范围`, empty: '没有匹配的模型',
  }));
  controls.set('effort', new SelectControl(select('agent-effort')));
  $('settings-dialog').addEventListener('click', (event) => {
    if ((event.target as Element).closest('[data-tab]'))
      for (const control of controls.values()) control.close(false);
  });
  field('agent-enabled').addEventListener('change', async () => {
    const enabled = field('agent-enabled').checked;
    await save({ agentEnabled: enabled });
    if (enabled) agentTabShown();
  });
  field('agent-button').addEventListener('change', () => void save({ agentButton: field('agent-button').checked }));
  select('agent-choice').addEventListener('change', async () => {
    custom.model = custom.effort = false;
    await save({ agent: select('agent-choice').value as AgentChoice });
    loadModels();
  });
  $('agent-detect').addEventListener('click', () => detect(true));
  $('agent-model-refresh').addEventListener('click', () => loadModels(true));
  for (const kind of ['model', 'effort'] as const) {
    select(`agent-${kind}`).addEventListener('change', () => choose(kind));
    field(`agent-${kind}-custom`).addEventListener('change', () => saveCustom(kind));
    field(`agent-${kind}-custom`).addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        saveCustom(kind);
      }
    });
  }
}
