import { useId, useLayoutEffect, useRef, useState } from 'react';
import { ArrowRightIcon } from '@phosphor-icons/react/dist/csr/ArrowRight';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { MagnifyingGlassIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlass';
import { GettingStarted, type GettingStartedState } from '../components/GettingStarted';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import { Presence } from '../ui/Presence';
import disclosure from '../ui/Disclosure.module.css';
import type { HelpTopicId, Preferences } from './contracts';
import { gettingStartedSearch, helpArticles, shortcutsSearch, type HelpArticleId, type HelpDestination } from './helpTopics';
import { bindingLabel, defaultShortcuts, effectiveBinding, navigationKeys, shortcutActions, shortcutGroups, type ShortcutPreferences } from './shortcuts';
import { safeBinding } from './shortcutBindings';
import css from './HelpDialog.module.css';

type KeyRow = Readonly<{ id: string; label: string; detail: string; keys: readonly string[] }>;
type KeyGroup = Readonly<{ id: string; label: string; fixed: boolean; rows: readonly KeyRow[] }>;

// Case-folded words, with typographic apostrophes matched by the plain one people type. Key
// names stay whole (“ctrl+k”, “⌘⇧9”); other punctuation separates words.
function words(text: string) {
  return text.toLocaleLowerCase().replace(/[‘’]/gu, '\'').split(/[^\p{L}\p{M}\p{N}\p{S}']+/u).filter(Boolean);
}
// Every query word must start a word of the text, so “sign in” never finds “Unassigned”.
function matches(text: string, query: readonly string[]) {
  const value = words(text);
  return query.length > 0 && query.every((word) => value.some((entry) => entry.startsWith(word)));
}
function isArticle(topic: HelpTopicId | undefined): topic is HelpArticleId { return helpArticles.some((article) => article.id === topic); }

// Lines a topic up with the top of Help's scrolling body, clear of the pinned search (its scroll
// margin). Only that scroller moves: scrollIntoView would also scroll the dialog itself, carrying
// its title and Close out of view.
function alignInBody(node: HTMLElement) {
  for (let scroller = node.parentElement; scroller && !(scroller instanceof HTMLDialogElement); scroller = scroller.parentElement) {
    const { overflowY } = getComputedStyle(scroller);
    if (overflowY !== 'auto' && overflowY !== 'scroll') continue;
    const margin = Number.parseFloat(getComputedStyle(node).scrollMarginTop) || 0;
    scroller.scrollTop += node.getBoundingClientRect().top - scroller.getBoundingClientRect().top - scroller.clientTop - margin;
    return;
  }
}

// The user's current keys, then the fixed navigation keys. A stored chord the app would
// refuse to run reads as Unassigned, as it does in the palette.
function keyGroups(preferences: ShortcutPreferences, enter: Preferences['enter'] | undefined): readonly KeyGroup[] {
  const app = shortcutGroups.map((group): KeyGroup => ({ id: group, label: group, fixed: false, rows: shortcutActions.filter((action) => action.group === group).map((action) => {
    const binding = effectiveBinding(preferences, action.id);
    return { id: action.id, label: action.label, detail: action.detail, keys: binding && safeBinding(binding) ? [bindingLabel(binding)] : [] };
  }) }));
  return [...app, { id: 'navigation', label: 'Navigation', fixed: true, rows: navigationKeys(enter) }];
}

// One searchable place for Getting started, short explanations and every keyboard shortcut.
// Top-anchored like Commands, so filtering never moves the search. `topic` opens that topic
// expanded and scrolled into view, with focus on its heading or disclosure; otherwise focus
// starts in the search. Escape clears the search first, then closes. Once every step is done,
// Getting started folds to one line unless it's the topic asked for or the search finds it.
export function HelpDialog({ topic, query, onQueryChange, shortcuts, enter, gettingStarted, onConnectProvider, onOpenProject, onGo, canGo, onSearchSettings, onOpenCommands, ...chrome }: DialogChrome & {
  topic?: HelpTopicId; query: string; onQueryChange: (value: string) => void; shortcuts?: ShortcutPreferences;
  // The user's Enter setting, so the Send row lists the keys that send for them. Without it the
  // row lists only Ctrl/Cmd+Enter, which sends whatever the setting.
  enter?: Preferences['enter'];
  gettingStarted: GettingStartedState; onConnectProvider: () => void; onOpenProject: () => void;
  // Opens a topic's real place. Without it Help offers no Go to or Change shortcuts actions;
  // `canGo` hides the ones this host can't open.
  onGo?: (to: HelpDestination) => void; canGo?: (to: HelpDestination) => boolean;
  // Next steps when nothing matches. Pass onSearchSettings only while Settings would find the
  // query; it receives the trimmed query.
  onSearchSettings?: (query: string) => void; onOpenCommands?: () => void;
}) {
  const id = useId();
  const search = useRef<HTMLInputElement>(null);
  const target = useRef<HTMLElement | null>(null);
  const landing = useRef<HTMLElement | null>(null);
  const article = isArticle(topic) ? topic : null;
  const [open, setOpen] = useState<ReadonlySet<HelpArticleId>>(() => new Set(article ? [article] : []));
  // While searching, every matching topic shows open; these are the ones folded during this search.
  const [folded, setFolded] = useState<ReadonlySet<HelpArticleId>>(() => new Set());
  const text = query.trim();
  const terms = words(query);
  const searching = text.length > 0;
  const value = shortcuts ?? defaultShortcuts;

  // DialogFrame has already moved focus to the topic; align the topic with the top of the body.
  useLayoutEffect(() => { if (landing.current) alignInBody(landing.current); }, []);

  const showStart = !searching || matches(gettingStartedSearch, terms);
  const articles = helpArticles.filter((entry) => !searching || matches(`${entry.title} ${entry.body.join(' ')} ${entry.keywords}`, terms));
  // A row without keys is searched by what it does, never by the word “Unassigned”.
  const groups = keyGroups(value, enter).map((group) => ({ ...group, rows: searching
    ? group.rows.filter((row) => matches(`${shortcutsSearch} ${group.label} ${row.label} ${row.detail} ${row.keys.join(' ')}`, terms))
    : group.rows })).filter((group) => group.rows.length > 0);
  const results = (showStart ? 1 : 0) + articles.length + groups.reduce((total, group) => total + group.rows.length, 0);
  const stuck = searching && !results;

  function flip(set: ReadonlySet<HelpArticleId>, entry: HelpArticleId) {
    const next = new Set(set);
    if (next.has(entry)) next.delete(entry); else next.add(entry);
    return next;
  }
  function toggle(entry: HelpArticleId) {
    if (searching) setFolded((set) => flip(set, entry)); else setOpen((set) => flip(set, entry));
  }
  function changeQuery(next: string) {
    if (folded.size) setFolded(new Set());
    onQueryChange(next);
  }
  function go(to: HelpDestination, label: string) {
    return onGo && (canGo?.(to) ?? true) ? <button type="button" className={css.go} onClick={() => onGo(to)}>{label}<ArrowRightIcon size={12} aria-hidden="true" /></button> : null;
  }

  return <DialogFrame {...chrome} anchor="top" title="Help" initialFocus={topic ? target : search}
    description="Search topics and shortcuts. Escape clears the search, then closes Help."
    onBack={query ? () => changeQuery('') : undefined}>
    <div className={css.help}>
      <div className={css.bar}>
        <MagnifyingGlassIcon size={16} aria-hidden="true" />
        <input ref={search} className={css.search} type="search" aria-label="Search help" placeholder="Search help" autoComplete="off" spellCheck={false}
          value={query} onChange={(event) => changeQuery(event.currentTarget.value)} />
      </div>
      <p className={stuck ? css.noMatch : 'srOnly'} role="status">
        {!searching ? '' : results ? `${results} ${results === 1 ? 'result' : 'results'}` : `No topics or shortcuts match “${text}”.`}
      </p>
      {stuck && (onSearchSettings || onOpenCommands) ? <div className={css.next}>
        {onSearchSettings ? <button type="button" className={css.go} onClick={() => onSearchSettings(text)}><span>Search Settings for “{text}”</span><ArrowRightIcon size={12} aria-hidden="true" /></button> : null}
        {onOpenCommands ? <button type="button" className={css.go} onClick={onOpenCommands}>Open Commands<ArrowRightIcon size={12} aria-hidden="true" /></button> : null}
      </div> : null}

      {showStart ? <div className={css.section} ref={(node) => {
        if (!node || topic !== 'getting-started') return;
        // GettingStarted owns its heading; opening Help on this topic makes it the focus target.
        const heading = node.querySelector<HTMLElement>('h3');
        if (heading) { heading.tabIndex = -1; target.current = heading; }
        landing.current = node;
      }}>
        <GettingStarted state={gettingStarted} onConnectProvider={onConnectProvider} onOpenProject={onOpenProject} headingLevel={3} collapsible={!searching && topic !== 'getting-started'} />
      </div> : null}

      {articles.length ? <section className={css.section} aria-labelledby={`${id}-articles`}>
        <h3 id={`${id}-articles`} className={css.sectionTitle}>How AVA works</h3>
        <ul className={css.topics}>{articles.map((entry) => {
          const shown = searching ? !folded.has(entry.id) : open.has(entry.id);
          const panel = `${id}-${entry.id}`;
          return <li key={entry.id} className={css.topic} ref={(node) => { if (node && entry.id === article) landing.current = node; }}>
            <h4 className={css.topicHeading}>
              <button ref={(node) => { if (node && entry.id === article) target.current = node; }} type="button" className={css.trigger}
                aria-expanded={shown} aria-controls={shown ? panel : undefined} onClick={() => toggle(entry.id)}>
                <span>{entry.title}</span><CaretDownIcon size={12} aria-hidden="true" />
              </button>
            </h4>
            <div className={disclosure.track} data-open={shown}>
              <Presence id={panel} open={shown} className={disclosure.content}>
                <div className={css.body}>
                  {entry.body.map((paragraph) => <p key={paragraph}>{paragraph}</p>)}
                  {entry.go ? go(entry.go.to, entry.go.label) : null}
                </div>
              </Presence>
            </div>
          </li>;
        })}</ul>
      </section> : null}

      {groups.length ? <section className={css.section} aria-labelledby={`${id}-shortcuts`} ref={(node) => { if (node && topic === 'shortcuts') landing.current = node; }}>
        <div className={css.sectionHead}>
          <h3 id={`${id}-shortcuts`} className={css.sectionTitle} tabIndex={-1} ref={(node) => { if (node && topic === 'shortcuts') target.current = node; }}>Keyboard shortcuts</h3>
          {go('shortcuts', 'Change shortcuts')}
        </div>
        {!value.enabled ? <p className={css.note}>App shortcuts are off. The navigation keys still work.</p> : null}
        <div className={css.keyGroups}>{groups.map((group) => <table key={group.id} className={css.keys} data-off={!group.fixed && !value.enabled || undefined}>
          <caption>{group.label}</caption>
          <thead className="srOnly"><tr><th scope="col">Action</th><th scope="col">Keys</th></tr></thead>
          <tbody>{group.rows.map((row) => <tr key={row.id}>
            <th scope="row">{row.label}</th>
            <td>{row.keys.length ? row.keys.map((key) => <kbd key={key} className={css.key}>{key}</kbd>) : <span className={css.unassigned}>Unassigned</span>}</td>
          </tr>)}</tbody>
        </table>)}</div>
      </section> : null}
    </div>
  </DialogFrame>;
}
