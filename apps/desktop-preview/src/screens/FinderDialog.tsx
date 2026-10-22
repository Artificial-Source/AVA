import { useState } from 'react';
import { validChoices, validIdentity } from '../chat/requestValidation';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { FinderIntent, FinderItem, FinderView, UICommand } from './contracts';
import { FormField, ListNotice, SearchField } from './ScreenPrimitives';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

export function knownCommand(value: string): value is UICommand { return ['settings', 'models', 'providers', 'sessions', 'workspace'].includes(value); }
export function validFinder(view: FinderView) {
  return validIdentity(view) && ['sessions', 'commands'].includes(view.kind) && validChoices(view.items) && validChoices(view.projects);
}
export function selectableItem(view: FinderView, item: FinderItem | undefined) {
  return validFinder(view) && view.state === 'ready' && Boolean(item && !item.disabledReason && (view.kind === 'sessions'
    ? view.projects.some((project) => project.id === item.projectId)
    : item.command && knownCommand(item.command) && item.id === item.command));
}
export function validFinderIntent(view: FinderView, intent: FinderIntent) {
  return validFinder(view) && intent.id === view.id && intent.version === view.version && selectableItem(view, view.items.find((item) => item.id === intent.itemId));
}
export function FinderDialog({ view, query, onQueryChange, onSelect, onRetry, ...chrome }: DialogChrome & {
  view: FinderView; query: string; onQueryChange: (value: string) => void; onSelect: (intent: FinderIntent) => void; onRetry: () => void;
}) {
  const [project, setProject] = useState('all');
  const gate = useViewGuard(JSON.stringify(view));
  const valid = validFinder(view);
  const filtered = valid ? view.items.filter((item) => (project === 'all' || item.projectId === project) && `${item.label} ${item.detail}`.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase())) : [];
  const title = view.kind === 'sessions' ? 'Find a session' : 'Commands';
  return <DialogFrame {...chrome} title={title} description={view.kind === 'sessions' ? 'Search supplied project-safe example chats. No session files or paths are accessed.' : 'Known UI navigation only. Text is never interpreted as slash, shell or process commands.'} onBack={query ? () => onQueryChange('') : undefined}>
    <SearchField label={view.kind === 'sessions' ? 'Search sessions' : 'Search commands'} value={query} onChange={(value) => { if (gate.valid()) onQueryChange(value); }} />
    {view.kind === 'sessions' && valid ? <FormField label="Project filter">{(props) => <select {...props} className={styles.select} value={project} onChange={(event) => { const value = event.currentTarget.value; if (gate.valid() && (value === 'all' || view.projects.some((entry) => entry.id === value))) setProject(value); }}><option value="all">All example projects</option>{view.projects.map((entry) => <option key={entry.id} value={entry.id}>{entry.label}</option>)}</select>}</FormField> : null}
    <ListNotice state={view.state} onRetry={() => { if (gate.claim()) onRetry(); }} />
    {!valid ? <p className={styles.notice} role="alert">Unsupported or duplicate choices. No action is available.</p> : null}
    {valid && filtered.length === 0 && view.state === 'ready' ? <p className={styles.notice} role="status">No matching {view.kind}. Search a different term or clear the filters.</p> : null}
    {valid ? <ul className={styles.list}>{filtered.map((item) => <li key={item.id}><button className={styles.result} disabled={!selectableItem(view, item)} onClick={() => {
      const intent = { id: view.id, version: view.version, itemId: item.id };
      if (validFinderIntent(view, intent) && gate.claim()) onSelect(intent);
    }}><span><strong>{item.label}</strong><small>{item.detail}</small>{!selectableItem(view, item) ? <small>{item.disabledReason || (view.state === 'ready' ? 'No known UI action or project binding. Selection unavailable.' : 'Unavailable in the supplied view state.')}</small> : null}</span><span aria-hidden="true">→</span></button></li>)}</ul> : null}
    <div className={styles.actions}><Button onClick={chrome.onDismiss}>Cancel</Button></div>
  </DialogFrame>;
}
