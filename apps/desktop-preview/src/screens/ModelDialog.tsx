import { useEffect, useId, useRef, useState } from 'react';
import { MagnifyingGlassIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlass';
import { MinusCircleIcon } from '@phosphor-icons/react/dist/csr/MinusCircle';
import { PlugsIcon } from '@phosphor-icons/react/dist/csr/Plugs';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { validIdentity } from '../chat/requestValidation';
import { previewModelProfile } from '../components/contextPreview';
import { ModelMark } from '../components/ModelMark';
import type { ModelId } from '../state/types';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import feedback from '../ui/feedback.module.css';
import type { ListState, ModelChoice, ModelPickerView, ModelSelection, Provider } from './contracts';
import { useProviderList } from './providerState';
import { FormField, ListNotice } from './ScreenPrimitives';
import { useViewGuard } from './useViewGuard';
import screens from './screens.module.css';
import styles from './ModelDialog.module.css';

const boundModels: readonly string[] = ['aster', 'aster-mini', 'orion'];
const isBound = (id: string | undefined): id is ModelId => id !== undefined && boundModels.includes(id);
const groupKey = (choice: ModelChoice) => JSON.stringify([choice.providerId, choice.family?.id ?? null]);
const windowLabel = (choice: ModelChoice) => Number.isSafeInteger(choice.windowTokens) && choice.windowTokens! > 0 ? `${choice.windowTokens!.toLocaleString('en-US')} token window` : 'Window unknown';
const notices: Partial<Record<ListState, string>> = {
  loading: 'Loading models…',
  error: 'Couldn’t load models.',
  empty: 'No models yet. Connect a provider to add some.',
  unavailable: 'Models aren’t available right now.',
};

export function validModelCatalog(view: ModelPickerView) {
  const providers = new Map<string, string>();
  const families = new Map<string, string>();
  return validIdentity(view) && ['current-chat', 'new-chat-default'].includes(view.scope) && Array.isArray(view.choices) &&
    new Set(view.choices.map((choice) => choice?.id)).size === view.choices.length && view.choices.every((choice) => {
      if (!choice || ![choice.id, choice.label, choice.providerId, choice.providerLabel].every((text) => typeof text === 'string' && text.trim().length > 0) ||
        !Array.isArray(choice.capabilities) || !choice.capabilities.every((text: unknown) => typeof text === 'string') ||
        !Array.isArray(choice.reasoning) || !choice.reasoning.every((text: unknown) => typeof text === 'string' && text.trim()) || new Set(choice.reasoning).size !== choice.reasoning.length) return false;
      const family = choice.family;
      if (family !== undefined && (!family || typeof family !== 'object' || Array.isArray(family) || ![family.id, family.label].every((text) => typeof text === 'string' && text.trim().length > 0))) return false;
      const key = groupKey(choice);
      if (providers.has(choice.providerId) && providers.get(choice.providerId) !== choice.providerLabel || family && families.has(key) && families.get(key) !== family.label) return false;
      providers.set(choice.providerId, choice.providerLabel);
      if (family) families.set(key, family.label);
      return true;
    });
}
export function selectableModel(view: ModelPickerView, choice: ModelChoice | undefined) {
  return validModelCatalog(view) && view.state === 'ready' && Boolean(choice && !choice.disabledReason && choice.modelId === choice.id && boundModels.includes(choice.id));
}
export function validModelSelection(view: ModelPickerView, intent: ModelSelection) {
  if (!validModelCatalog(view)) return false;
  const choice = view.choices.find((entry) => entry.id === intent.modelId);
  return intent.id === view.id && intent.version === view.version && intent.scope === view.scope && selectableModel(view, choice) &&
    (view.reasoningEditable === false ? intent.reasoning === null : choice!.reasoning.length ? intent.reasoning !== null && choice!.reasoning.includes(intent.reasoning) : intent.reasoning === null);
}

// This visit's picks, newest first. Memory only: nothing is saved and a reload starts empty.
let recentPicks: readonly string[] = [];
export function forgetRecentModels() { recentPicks = []; }

// A model is usable only through a connected provider. Providers the list doesn't
// know keep whatever the catalog says; a catalog reason always wins.
function withConnections(view: ModelPickerView, providers: readonly Provider[]): ModelPickerView {
  const offline = new Set((Array.isArray(providers) ? providers : []).filter((provider) => provider && provider.state !== 'connected-example' && provider.state !== 'external').map((provider) => provider.id));
  if (!offline.size || !Array.isArray(view.choices)) return view;
  return { ...view, choices: view.choices.map((choice) => choice && offline.has(choice.providerId) && !choice.disabledReason ? { ...choice, disabledReason: `${choice.providerLabel} isn’t connected.` } : choice) };
}

// Recent is a browsing shortcut; while searching, results stay with their family.
function arrange(choices: readonly ModelChoice[], query: string) {
  const needle = query.toLocaleLowerCase().trim();
  const filtered = choices.filter((choice) => `${choice.label} ${choice.providerLabel} ${choice.family?.label ?? ''}`.toLocaleLowerCase().includes(needle));
  const recent = needle ? [] : recentPicks.flatMap((id) => filtered.filter((choice) => choice.id === id));
  const rest = filtered.filter((choice) => !recent.includes(choice));
  const groups = [...new Set(rest.map(groupKey))].map((key) => rest.filter((choice) => groupKey(choice) === key));
  return { recent, groups, ordered: [...recent, ...groups.flat()] };
}

// `providers` defaults to this visit's connections; a review host can pass its own list.
export function ModelDialog({ view: catalog, providers, query, onQueryChange, onSelect, onRetry, onConnectProvider, chatTitle, description, ...chrome }: DialogChrome & {
  view: ModelPickerView; query: string; onQueryChange: (query: string) => void;
  onSelect: (intent: ModelSelection) => void; onRetry: () => void;
  onConnectProvider?: (providerId?: string) => void;
  providers?: readonly Provider[];
  chatTitle?: string; description?: string;
}) {
  const name = useId();
  const hintId = useId();
  const reasoningId = useId();
  const formId = useId();
  const search = useRef<HTMLInputElement>(null);
  const tiles = useRef(new Map<string, HTMLElement>());
  const composing = useRef(false);
  const connections = useProviderList();
  const view = withConnections(catalog, providers ?? connections);
  const gate = useViewGuard(JSON.stringify(view));
  const valid = validModelCatalog(view);
  const choices = valid ? view.choices : [];
  const scopeId = view.scope === 'new-chat-default' ? view.defaultId : view.currentId;
  // A picker opened with a query starts on its first match, like typing it.
  const [draft, setDraft] = useState(() => query.trim() ? arrange(choices, query).ordered.find((choice) => selectableModel(view, choice))?.id ?? scopeId : scopeId);
  const [effort, setEffort] = useState<string | null>((view.scope === 'current-chat' ? view.currentReasoning : view.defaultReasoning) ?? null);
  const { recent, groups, ordered } = arrange(choices, query);
  // Only a visible highlight can be used: a filtered-out model never stays the primary action.
  const highlighted = ordered.find((choice) => choice.id === draft);
  const enabled = selectableModel(view, highlighted);
  const levels = view.reasoningEditable !== false && enabled ? highlighted!.reasoning : [];
  const reasoning = levels.length ? (effort && levels.includes(effort) ? effort : levels[0]) : null;
  const summary = highlighted ? { mark: isBound(highlighted.modelId) ? highlighted.modelId : undefined, label: highlighted.label, detail: [highlighted.providerLabel, windowLabel(highlighted)] }
    : view.state === 'loading' && isBound(scopeId) ? { mark: scopeId, label: previewModelProfile(scopeId).modelLabel, detail: [previewModelProfile(scopeId).providerLabel, 'In use'] } : null;

  useEffect(() => { tiles.current.get(draft)?.scrollIntoView?.({ block: 'nearest' }); }, [draft]);

  function commit() {
    if (!enabled || !highlighted?.modelId) return;
    const intent: ModelSelection = { id: view.id, version: view.version, modelId: highlighted.modelId, scope: view.scope, reasoning };
    if (!validModelSelection(view, intent) || !gate.claim()) return;
    recentPicks = [highlighted.id, ...recentPicks.filter((id) => id !== highlighted.id)].slice(0, 4);
    onSelect(intent);
  }
  function move(step: 1 | -1) {
    const options = ordered.filter((choice) => selectableModel(view, choice));
    if (!gate.valid() || !options.length) return;
    const index = options.findIndex((choice) => choice.id === draft);
    setDraft((index < 0 ? options[step > 0 ? 0 : options.length - 1] : options[(index + step + options.length) % options.length]).id);
  }
  function changeQuery(value: string) {
    if (!gate.valid()) return;
    onQueryChange(value);
    const first = value.trim() ? arrange(choices, value).ordered.find((choice) => selectableModel(view, choice)) : undefined;
    if (first) setDraft(first.id);
  }

  function tile(choice: ModelChoice) {
    const selectable = selectableModel(view, choice);
    const bound = choice.modelId === choice.id && isBound(choice.id);
    const reason = choice.disabledReason || (bound ? '' : 'No model profile, so this can’t be selected.');
    return <div key={choice.id} ref={(node) => { if (node) tiles.current.set(choice.id, node); else tiles.current.delete(choice.id); }} className={styles.tile} data-selected={draft === choice.id} data-disabled={!selectable}>
      <label className={styles.choice}>
        <input type="radio" name={name} value={choice.id} checked={draft === choice.id} disabled={!selectable} onChange={() => { if (gate.valid() && selectableModel(view, choice)) setDraft(choice.id); }} />
        <span className={styles.tileBody}>
          <span className={styles.tileTitle}>{isBound(choice.modelId) ? <ModelMark model={choice.modelId} /> : null}<strong>{choice.label}</strong>{' '}
            {choice.id === view.currentId ? <span className={styles.tag}>This chat</span> : null}{' '}{choice.id === view.defaultId ? <span className={styles.tag}>New chat default</span> : null}
          </span>{' '}
          <span className={styles.facts}>{windowLabel(choice)} · {choice.capabilities.length ? choice.capabilities.join(' · ') : 'Capabilities unknown'}{choice.reasoning.length ? ' · Reasoning' : ''}</span>
          {reason ? <span className={styles.reason}><MinusCircleIcon size={14} aria-hidden="true" />{reason}</span> : null}
        </span>
      </label>
      {/* A model blocked only by its provider offers the way out next to the reason. */}
      {reason && bound && onConnectProvider ? <button type="button" className={`${styles.link} ${styles.tileLink}`} onClick={() => { if (gate.valid()) onConnectProvider(choice.providerId); }}><PlugsIcon size={14} aria-hidden="true" />Connect {choice.providerLabel}</button> : null}
    </div>;
  }

  const title = view.scope === 'new-chat-default' ? 'Default model for new chats' : chatTitle?.trim() ? `Model for “${chatTitle.trim()}”` : 'Model for this chat';
  const footer = <div className={styles.bar}>
    <p className={styles.summary} data-model-summary aria-live="polite" aria-atomic="true">{summary ? <>{summary.mark ? <ModelMark model={summary.mark} /> : null}<strong>{summary.label}</strong>{summary.detail.map((part) => <span key={part}> · {part}</span>)}</> : 'Nothing selected'}</p>
    <div className={styles.barRow}>
      {onConnectProvider ? <button type="button" className={styles.link} onClick={() => { if (gate.valid()) onConnectProvider(); }}><PlusIcon size={14} aria-hidden="true" />Connect a provider…</button> : null}
      <div className={styles.barEnd}>
        {levels.length ? <span className={styles.reasoning}><label htmlFor={reasoningId}>Reasoning level</label><select id={reasoningId} className={screens.select} value={reasoning ?? ''} onChange={(event) => {
          if (gate.valid() && levels.includes(event.currentTarget.value)) setEffort(event.currentTarget.value);
        }}>{levels.map((level) => <option key={level}>{level}</option>)}</select></span> : null}
        <Button onClick={chrome.onDismiss}>Cancel</Button>
        <Button type="submit" form={formId} tone="primary" disabled={!enabled}>{view.scope === 'new-chat-default' ? 'Set as default' : 'Use for this chat'}</Button>
      </div>
    </div>
  </div>;
  return <DialogFrame {...chrome} wide title={title} subtitle={view.scope === 'new-chat-default' ? 'Existing chats keep their model.' : 'Only this chat changes.'}
    description={description} initialFocus={search} footer={footer} onBack={query ? () => { if (gate.valid()) onQueryChange(''); } : undefined}>
    <FormField label="Search models">{(props) => <input {...props} ref={search} aria-describedby={hintId} className={screens.input} type="search" autoComplete="off" spellCheck={false} value={query}
      onChange={(event) => changeQuery(event.currentTarget.value)}
      onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}
      onKeyDown={(event) => {
        if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); move(event.key === 'ArrowDown' ? 1 : -1); }
        else if (event.key === 'Enter') { event.preventDefault(); commit(); }
      }} />}</FormField>
    <p id={hintId} className="srOnly">Up and down arrows move between models. Enter uses the selected one.</p>
    {!valid ? <ListNotice state="error" message="This model list can’t be shown, so nothing can be selected." />
      : view.state !== 'ready' ? <ListNotice state={view.state} message={view.state === 'disabled' ? description : notices[view.state]} onRetry={() => { if (gate.claim()) onRetry(); }} />
      : !view.choices.length ? <ListNotice state="empty" message={notices.empty} />
      : !ordered.length ? <div className={feedback.notice} data-state="empty">
        <span className={feedback.noticeIcon} aria-hidden="true"><MagnifyingGlassIcon size={16} /></span>
        <div className={feedback.noticeBody}><p className={feedback.noticeMessage} role="status">No models match “{query.trim()}”.</p>
          <Button onClick={() => { if (gate.valid()) { onQueryChange(''); search.current?.focus(); } }}>Clear search</Button></div>
      </div> : null}
    <form id={formId} onSubmit={(event) => { event.preventDefault(); commit(); }}>
      {recent.length ? <fieldset className={styles.group}><legend>Recent</legend><div className={styles.tiles}>{recent.map(tile)}</div></fieldset> : null}
      {groups.map((members) => {
        const { family, providerLabel } = members[0];
        return <fieldset className={styles.group} key={groupKey(members[0])}>
          <legend>{family ? <>{family.label} family{' '}<span>{providerLabel}</span></> : providerLabel}</legend>
          <div className={styles.tiles}>{members.map(tile)}</div>
        </fieldset>;
      })}
    </form>
  </DialogFrame>;
}
