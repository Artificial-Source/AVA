import { useId, useLayoutEffect, useRef, useState, type KeyboardEvent } from 'react';
import { ChatStatus } from '../components/ChatStatus';
import { validChoices, validIdentity } from '../chat/requestValidation';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { FinderIntent, FinderItem, FinderScope, FinderView, UICommand } from './contracts';
import { ListNotice } from './ScreenPrimitives';
import { defaultBindings } from './shortcutBindings';
import { useViewGuard } from './useViewGuard';
import styles from './FinderDialog.module.css';

const paletteCommands: readonly string[] = ['providers', 'workspace', 'rename', 'archive', 'branch', 'shortcuts', 'help'];
export function knownCommand(value: string): value is UICommand { return Object.hasOwn(defaultBindings, value) || paletteCommands.includes(value); }
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

const scopes: readonly Readonly<{ id: FinderScope; label: string }>[] = [{ id: 'all', label: 'All' }, { id: 'active', label: 'Active' }, { id: 'archived', label: 'Archived' }];
type Group = { key: string; label: string; items: FinderItem[] };
type Segment = Readonly<{ text: string; match: boolean }>;
type Searchable = Readonly<{ text: string; lower: string }>;

// Whitespace collapsed so a match can span a line break, plus its lower-case twin for matching.
function searchable(value: string): Searchable {
  const text = value.replace(/\s+/g, ' ').trim();
  const lower = text.toLocaleLowerCase();
  // Case folding that changes length (rare) would misalign the two; show the folded text then.
  return { text: lower.length === text.length ? text : lower, lower };
}
// A short window of a chat's messages, starting a few words before the earliest match.
// Word-aligned, with an ellipsis on each cut side; every term inside the window is marked.
const leadChars = 28;
const windowChars = 120;
function matchSnippet(source: Searchable, terms: readonly string[]): readonly Segment[] | null {
  const { text, lower } = source;
  const hits = terms.map((term) => lower.indexOf(term)).filter((index) => index >= 0);
  if (!hits.length) return null;
  const first = Math.min(...hits);
  let start = Math.max(0, first - leadChars);
  if (start > 0) {
    const space = lower.indexOf(' ', start);
    start = space >= 0 && space < first ? space + 1 : first;
  }
  let end = Math.min(text.length, start + windowChars);
  if (end < text.length) {
    const space = lower.lastIndexOf(' ', end);
    if (space > first) end = space;
  }
  const segments: Segment[] = [];
  if (start > 0) segments.push({ text: '…', match: false });
  for (let at = start; at < end;) {
    let next = -1;
    let length = 0;
    for (const term of terms) {
      const index = lower.indexOf(term, at);
      if (index >= 0 && index < end && (next < 0 || index < next || index === next && term.length > length)) { next = index; length = term.length; }
    }
    if (next < 0) { segments.push({ text: text.slice(at, end), match: false }); break; }
    if (next > at) segments.push({ text: text.slice(at, next), match: false });
    const stop = Math.min(end, next + length);
    segments.push({ text: text.slice(next, stop), match: true });
    at = stop;
  }
  if (end < text.length) segments.push({ text: '…', match: false });
  return segments;
}

// Chats group by project in the supplied project order; commands by their own group, first seen first.
function groupItems(view: FinderView, items: readonly FinderItem[]) {
  const groups = new Map<string, Group>(view.kind === 'sessions' ? view.projects.map((project) => [project.id, { key: project.id, label: project.label, items: [] }]) : []);
  for (const item of items) {
    const key = (view.kind === 'sessions' ? item.projectId : item.group) ?? '';
    const group = groups.get(key) ?? { key, label: view.kind === 'sessions' ? 'Unknown project' : key, items: [] };
    groups.set(key, group);
    group.items.push(item);
  }
  return [...groups.values()];
}

// A keyboard picker: focus stays in the search; arrows move a highlight through the
// listbox (aria-activedescendant); Enter opens it; Escape clears the search, then closes.
// Chats also match their message text; a chat found only by its messages shows where.
// `initialScope` opens chats on one filter (All when omitted); the filter buttons still change it.
// `emptyAction` is the next step offered while there are no chats at all (New chat, or Open a
// project before any is open); without it the empty list only says so.
export function FinderDialog({ view, query, initialScope, emptyAction, onQueryChange, onSelect, onRetry, ...chrome }: DialogChrome & {
  view: FinderView; query: string; initialScope?: FinderScope; emptyAction?: Readonly<{ label: string; run: () => void }>;
  onQueryChange: (value: string) => void; onSelect: (intent: FinderIntent) => void; onRetry: () => void;
}) {
  const id = useId();
  const search = useRef<HTMLInputElement>(null);
  const reveal = useRef(false);
  const [scope, setScope] = useState<FinderScope>(() => scopes.find((entry) => entry.id === initialScope)?.id ?? 'all');
  const [highlight, setHighlight] = useState<string | null>(null);
  const gate = useViewGuard(JSON.stringify(view));
  const valid = validFinder(view);
  const chats = view.kind === 'sessions';
  const terms = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
  const snippets = new Map<string, readonly Segment[]>();
  const inScope = valid ? view.items.filter((item) => !chats || scope === 'all' || (scope === 'archived') === Boolean(item.archived)) : [];
  const groups = groupItems(view, inScope).map((group) => ({ ...group, items: group.items.filter((item) => {
    const named = `${item.label} ${item.detail} ${group.label}`.toLocaleLowerCase();
    const rest = terms.filter((term) => !named.includes(term));
    if (!rest.length) return true;
    // Only terms the title, detail and project don't cover reach the message text.
    const body = chats && typeof item.searchText === 'string' ? searchable(item.searchText) : null;
    if (!body || !rest.every((term) => body.lower.includes(term))) return false;
    const snippet = matchSnippet(body, rest);
    if (snippet) snippets.set(item.id, snippet);
    return true;
  }) })).filter((group) => group.items.length);
  const ordered = groups.flatMap((group) => group.items);
  const position = new Map(ordered.map((item, index) => [item.id, index]));
  const usable = (item: FinderItem) => selectableItem(view, item);
  const active = ordered.find((item) => item.id === highlight) ?? ordered.find(usable) ?? ordered[0];
  const optionId = (item: FinderItem) => `${id}-option-${position.get(item.id)}`;
  const activeId = active ? optionId(active) : undefined;
  // Every row blocked (or a non-ready list): one notice instead of a reason on every row.
  const blocked = ordered.length > 0 && !ordered.some(usable);
  const reasons = new Set(ordered.map((item) => item.disabledReason));
  const listId = `${id}-list`;
  const noun = chats ? 'chats' : 'commands';

  useLayoutEffect(() => {
    if (!reveal.current || !activeId) return;
    reveal.current = false;
    document.getElementById(activeId)?.scrollIntoView?.({ block: 'nearest' });
  }, [activeId]);

  function choose(item: FinderItem) {
    const intent = { id: view.id, version: view.version, itemId: item.id };
    if (validFinderIntent(view, intent) && gate.claim()) onSelect(intent);
  }
  function move(event: KeyboardEvent<HTMLInputElement>) {
    if (event.nativeEvent.isComposing || event.keyCode === 229 || event.altKey || event.ctrlKey || event.metaKey) return;
    if (event.key === 'Enter') {
      event.preventDefault();
      if (active) choose(active);
      return;
    }
    const index = active ? position.get(active.id)! : -1;
    const last = ordered.length - 1;
    const next = event.key === 'ArrowDown' ? (index >= last ? 0 : index + 1) : event.key === 'ArrowUp' ? (index <= 0 ? last : index - 1)
      : event.key === 'Home' && !event.shiftKey ? 0 : event.key === 'End' && !event.shiftKey ? last : null;
    if (next === null || !ordered.length) return;
    event.preventDefault();
    if (!gate.valid()) return;
    reveal.current = true;
    setHighlight(ordered[next].id);
  }

  return <DialogFrame {...chrome} anchor="top" initialFocus={search} title={chats ? 'Find a chat' : 'Commands'}
    description={chats ? 'Type to search chat titles and messages. Arrow keys move, Enter opens the chat.' : 'Type to filter commands. Arrow keys move, Enter runs the command.'}
    onBack={query ? () => { if (gate.valid()) { setHighlight(null); onQueryChange(''); } } : undefined}>
    <div className={styles.bar}>
      <input ref={search} className={styles.search} type="text" role="combobox" aria-label={chats ? 'Search chats' : 'Search commands'} placeholder={chats ? 'Search chats and messages' : 'Type a command'}
        aria-autocomplete="list" aria-expanded={ordered.length > 0} aria-controls={ordered.length ? listId : undefined} aria-activedescendant={activeId}
        autoComplete="off" spellCheck={false} value={query} onKeyDown={move}
        onChange={(event) => { if (gate.valid()) { setHighlight(null); onQueryChange(event.currentTarget.value); } }} />
      {chats && valid ? <div className={styles.scope} role="group" aria-label="Show">
        {scopes.map((entry) => <button key={entry.id} type="button" aria-pressed={scope === entry.id} onClick={() => { if (gate.valid()) { setScope(entry.id); setHighlight(null); } }}>{entry.label}</button>)}
      </div> : null}
    </div>
    {!valid ? <ListNotice state="error" message={`These ${noun} can’t be shown, so nothing can be opened from this list.`} /> : <ListNotice state={view.state} onRetry={() => { if (gate.claim()) onRetry(); }} />}
    {valid && view.state === 'ready' && blocked ? <ListNotice state="disabled" message={reasons.size === 1 && [...reasons][0] || `None of these ${noun} can be used right now.`} /> : null}
    {valid && view.state === 'ready' && !ordered.length ? <ListNotice state="empty" message={
      terms.length ? `No ${noun} match “${query.trim()}”.` : !view.items.length ? chats ? 'No chats yet.' : 'No commands available.' : scope === 'archived' ? 'No archived chats.' : 'No active chats.'} /> : null}
    {chats && valid && view.state === 'ready' && !view.items.length && emptyAction ? <div className={styles.next}><Button onClick={() => { if (gate.valid()) emptyAction.run(); }}>{emptyAction.label}</Button></div> : null}
    {ordered.length ? <div id={listId} role="listbox" aria-label={chats ? 'Chats' : 'Commands'}>
      {groups.map((group, groupIndex) => <div key={group.key} className={styles.group} role="group" aria-labelledby={group.label ? `${id}-group-${groupIndex}` : undefined}>
        {group.label ? <div id={`${id}-group-${groupIndex}`} className={styles.groupLabel} role="presentation">{group.label}</div> : null}
        {group.items.map((item) => {
          const option = optionId(item);
          const enabled = usable(item);
          const snippet = snippets.get(item.id);
          // A shortcut stays listed while its command is unavailable, so it can still be learned here.
          return <div key={item.id} id={option} className={styles.option} role="option" aria-selected={item === active} aria-disabled={!enabled || undefined}
            aria-labelledby={`${option}-label`} aria-describedby={`${option}-detail ${option}-meta${snippet ? ` ${option}-snippet` : ''}`} data-active={item === active || undefined} data-snippet={snippet ? true : undefined}
            onMouseDown={(event) => event.preventDefault()} onMouseMove={() => { if (highlight !== item.id && gate.valid()) setHighlight(item.id); }} onClick={() => choose(item)}>
            {chats ? <span className={styles.glyph}>{item.status ? <ChatStatus status={item.status} part="glyph" /> : null}</span> : null}
            <span id={`${option}-label`} className={styles.label}>{item.label}</span>
            <span id={`${option}-detail`} className={styles.detail}>{item.status ? <ChatStatus status={item.status} part="label" /> : null}{item.detail ? <span>{item.detail}</span> : null}</span>
            <span id={`${option}-meta`} className={styles.meta}>
              {item.current ? <span className={styles.tag}>Current</span> : null}
              {item.archived ? <span className={styles.tag}>Archived</span> : null}
              {!enabled && !blocked ? <span className={styles.reason}>{item.disabledReason || 'Unavailable'}</span> : null}
              {item.shortcut ? <kbd className={styles.key}>{item.shortcut}</kbd> : null}
            </span>
            {snippet ? <span id={`${option}-snippet`} className={styles.snippet}>{snippet.map((segment, index) => segment.match ? <mark key={index}>{segment.text}</mark> : segment.text)}</span> : null}
          </div>;
        })}
      </div>)}
    </div> : null}
  </DialogFrame>;
}
