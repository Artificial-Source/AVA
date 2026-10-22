import { useId, useState } from 'react';
import { validIdentity } from '../chat/requestValidation';
import { ModelMark } from '../components/ModelMark';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { ModelChoice, ModelPickerView, ModelSelection } from './contracts';
import { FormField, ListNotice, SearchField } from './ScreenPrimitives';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

const boundModels = ['aster', 'aster-mini', 'orion'];
const groupKey = (choice: ModelChoice) => JSON.stringify([choice.providerId, choice.family?.id ?? null]);
const windowLabel = (choice: ModelChoice) => Number.isSafeInteger(choice.windowTokens) && choice.windowTokens! > 0 ? `${choice.windowTokens!.toLocaleString('en-US')} token window` : 'Window unknown';
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

export function ModelDialog({ view, query, onQueryChange, onSelect, onRetry, description = 'Choose a model for this review.', ...chrome }: DialogChrome & {
  view: ModelPickerView; query: string; onQueryChange: (query: string) => void; description?: string;
  onSelect: (intent: ModelSelection) => void; onRetry: () => void;
}) {
  const name = useId();
  const [draft, setDraft] = useState(view.scope === 'current-chat' ? view.currentId : view.defaultId);
  const [effort, setEffort] = useState<string | null>((view.scope === 'current-chat' ? view.currentReasoning : view.defaultReasoning) ?? null);
  const gate = useViewGuard(JSON.stringify(view));
  const valid = validModelCatalog(view);
  const selected = valid ? view.choices.find((choice) => choice.id === draft) : undefined;
  const reasoning = view.reasoningEditable !== false && selected?.reasoning.length ? (effort && selected.reasoning.includes(effort) ? effort : selected.reasoning[0]) : null;
  const filtered = valid ? view.choices.filter((choice) => `${choice.label} ${choice.providerLabel} ${choice.family?.label ?? ''}`.toLocaleLowerCase().includes(query.toLocaleLowerCase().trim())) : [];
  const groups = [...new Set(filtered.map(groupKey))];
  const enabled = selectableModel(view, selected);
  return <DialogFrame {...chrome} wide title="Choose a model" description={description} onBack={query ? () => { if (gate.valid()) onQueryChange(''); } : undefined}>
    <section className={styles.modelSummary} aria-label="Selected model">
      <div className={styles.modelSummaryHeading}>
        <div className={styles.modelIdentity}>
          {selected?.modelId && boundModels.includes(selected.modelId) ? <span className={styles.modelMark}><ModelMark model={selected.modelId} /></span> : null}
          <div><h2>{selected?.label ?? 'Choose a model'}</h2><p>{selected ? `${selected.providerLabel}${selected.family ? ` · ${selected.family.label} family` : ''}` : 'Model details unavailable'}</p></div>
        </div>
        <span className={styles.modelScope}>{view.scope === 'current-chat' ? 'This chat' : 'New chat default'}</span>
      </div>
      {selected ? <div className={styles.modelSummaryFacts}>
        <span className={styles.modelWindow}>{windowLabel(selected)}</span>
        <ul className={styles.modelCapabilities} aria-label="Model capabilities">
          {selected.capabilities.length ? selected.capabilities.map((capability, index) => <li key={index}>{capability}</li>) : <li>Capabilities unknown</li>}
          {selected.reasoning.length ? <li>Reasoning: {selected.reasoning.join(', ')}</li> : null}
        </ul>
      </div> : null}
    </section>
    <SearchField label="Search models" value={query} onChange={(value) => { if (gate.valid()) onQueryChange(value); }} />
    <ListNotice state={view.state} onRetry={() => { if (gate.claim()) onRetry(); }} />
    {!valid ? <p role="alert" className={styles.notice}>Unsupported or malformed model catalog. No choices can be applied.</p> : null}
    {valid && filtered.length === 0 && view.state === 'ready' ? <p role="status" className={styles.notice}>No matching models. Clear the search to see supplied choices.</p> : null}
    <form onSubmit={(event) => {
      event.preventDefault();
      if (!enabled || !selected?.modelId) return;
      const intent: ModelSelection = { id: view.id, version: view.version, modelId: selected.modelId, scope: view.scope, reasoning };
      if (validModelSelection(view, intent) && gate.claim()) onSelect(intent);
    }}>
      {groups.map((key) => {
        const choices = filtered.filter((choice) => groupKey(choice) === key);
        const { family, providerLabel } = choices[0];
        return <fieldset className={styles.modelGroup} key={key}>
          <legend>{family ? <>{family.label} family{' '}<span>{providerLabel}</span></> : providerLabel}</legend>
          <div className={styles.modelTiles}>{choices.map((choice) => <label key={choice.id} className={styles.modelTile} data-selected={draft === choice.id} data-disabled={!selectableModel(view, choice)}>
            <input type="radio" name={name} value={choice.id} checked={draft === choice.id} disabled={!selectableModel(view, choice)} onChange={() => {
              if (gate.valid() && selectableModel(view, choice)) { setDraft(choice.id); setEffort(null); }
            }} />
            <span className={styles.modelTileBody}>
              <span className={styles.modelTileTitle}>{choice.modelId && boundModels.includes(choice.modelId) ? <ModelMark model={choice.modelId} /> : null}<strong>{choice.label}</strong>{' '}
                {choice.id === view.currentId ? <span className={styles.tag}>CURRENT</span> : null}{' '}{choice.id === view.defaultId ? <span className={styles.tag}>DEFAULT</span> : null}
              </span>{' '}
              <span className={styles.modelWindow}>{windowLabel(choice)}</span>{' '}
              <span className={styles.modelTileCapabilities}>{choice.capabilities.length ? choice.capabilities.join(' · ') : 'Capabilities unknown'}{choice.reasoning.length ? ' · Reasoning' : ''}</span>
              {choice.disabledReason || !choice.modelId ? <span className={styles.modelUnavailable}>{choice.disabledReason || 'No bound model profile. Selection unavailable.'}</span> : null}
            </span>
          </label>)}</div>
        </fieldset>;
      })}
      {view.reasoningEditable !== false && selected?.reasoning.length ? <FormField label="Reasoning level" hint="Supplied example capability, not private reasoning content.">{(props) => <select {...props} className={styles.select} value={reasoning ?? ''} disabled={!enabled} onChange={(event) => {
        if (gate.valid() && enabled && selected.reasoning.includes(event.currentTarget.value)) setEffort(event.currentTarget.value);
      }}>{selected.reasoning.map((level) => <option key={level}>{level}</option>)}</select>}</FormField> : null}
      <div className={styles.modelActions}><Button onClick={chrome.onDismiss}>Cancel</Button><Button type="submit" tone="primary" disabled={!enabled}>Use {selected?.label ?? 'model'}</Button></div>
    </form>
  </DialogFrame>;
}
