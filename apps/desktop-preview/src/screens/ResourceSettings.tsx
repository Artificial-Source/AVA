import type { InspectionView, ListState, McpInspection, ResourceInspections } from './contracts';
import { anchor } from './settingsIndex';
import { ListNotice, PreferenceRow } from './ScreenPrimitives';
import styles from './screens.module.css';

export type InspectionArea = 'rules' | keyof ResourceInspections;
type Notice = Readonly<{ state: Exclude<ListState, 'ready' | 'empty'>; message?: string }>;

// Page-specific copy for every non-ready state; `busy` keeps the shared run wording.
function notice(state: ListState, noun: string, label: string): Notice | null {
  if (state === 'ready' || state === 'empty') return null;
  if (state === 'loading') return { state, message: `Loading ${noun}…` };
  if (state === 'error') return { state, message: `Couldn’t load ${noun}.` };
  if (state === 'stale') return { state, message: `${label} may be out of date.` };
  if (state === 'unavailable') return { state, message: `${label} aren’t available.` };
  if (state === 'disabled') return { state, message: `You don’t have access to ${noun}.` };
  return { state: state === 'busy' ? 'busy' : 'error', message: state === 'busy' ? undefined : `Couldn’t show ${noun}.` };
}

export function RulesList({ view, onRetry }: { view: InspectionView; onRetry?: () => void }) {
  const status = notice(view.state, 'these rules', 'These rules');
  const rows = view.state === 'ready' || view.state === 'stale' ? view.rows : [];
  return <div {...anchor('permissions.rules')}>
    {status ? <ListNotice state={status.state} message={status.message} onRetry={onRetry} /> : null}
    {rows.map((row) => <PreferenceRow key={row.title} title={row.title} description={row.detail}><span className={styles.muted}>{row.status}</span></PreferenceRow>)}
    {!status && !rows.length ? <ListNotice state="empty" message="No saved rules or session grants." /> : null}
  </div>;
}

// The project of the chat Settings was opened from. Trust is decided per project, with `/trust` in one of its chats.
export type ProjectTrust = Readonly<{ name: string; trusted: boolean }>;
const trustScope = 'skills, prompts, subagents, plugins, MCP servers and language servers';

// Read-only like the rules: names the project and the one real route to change its trust.
export function ProjectTrustRow({ project }: { project?: ProjectTrust }) {
  const command = (text: string) => <code className={styles.command}>{text}</code>;
  return <div className={styles.preference} {...anchor('permissions.trust')}>
    <div><h3>Project trust</h3><p>
      {!project ? <>Trust is set per project. Until you trust a project, AVA skips that project’s own {trustScope}. To trust one, send {command('/trust project')} in one of its chats.</>
        : project.trusted ? <>{project.name} is this chat’s project. AVA uses its own {trustScope}. To stop using them, send {command('/trust deny')} in any {project.name} chat.</>
          : <>{project.name} is this chat’s project. Until you trust it, AVA skips its own {trustScope}. To trust it, send {command('/trust project')} in any {project.name} chat.</>}
    </p></div>
    {project ? <div className={styles.preferenceControl}><span className={styles.muted}>{project.trusted ? 'Trusted' : 'Not trusted'}</span></div> : null}
  </div>;
}

const areas = [
  { id: 'skills', label: 'Skills', noun: 'skills', type: 'Skill' },
  { id: 'prompts', label: 'Prompts', noun: 'prompts', type: 'Prompt' },
  { id: 'plugins', label: 'Plugins', noun: 'plugins', type: 'Plugin' },
  { id: 'mcp', label: 'MCP servers', noun: 'MCP servers', type: 'MCP server' },
  { id: 'lsp', label: 'Language servers', noun: 'language servers', type: 'Language server' },
] as const;
type Area = (typeof areas)[number];
type Entry = Readonly<{ key: string; area: Area; name: string; detail: string; scope: string; status: string; needsTrust: boolean }>;

function mcpStatus(server: McpInspection['servers'][number]) {
  const parts = [server.configEnabled === null ? 'Status unknown' : server.configEnabled ? 'On' : 'Off'];
  if (server.connection === 'not-connected') parts.push('not connected');
  return parts.join(' · ');
}
// A trusted project already meets a server's trust requirement, so only an untrusted one asks for it.
function entries(area: Area, inspections: ResourceInspections, projectTrusted: boolean): readonly Entry[] {
  const view = inspections[area.id];
  if (view.state !== 'ready' && view.state !== 'stale') return [];
  if (area.id === 'mcp') {
    return inspections.mcp.servers.map((server) => ({ key: `mcp:${server.id}`, area, name: server.name, detail: server.lifecycle === 'one-shot' ? `${server.transport} · starts when used` : server.transport, scope: server.source === 'global' ? 'Global' : 'Project', status: mcpStatus(server), needsTrust: server.trust === 'required' && !projectTrusted }));
  }
  return inspections[area.id].rows.map((row) => ({ key: `${area.id}:${row.title}`, area, name: row.title, detail: row.detail, scope: '—', status: row.status, needsTrust: false }));
}
function sentenceList(words: readonly string[]) {
  return words.length < 2 ? words.join('') : `${words.slice(0, -1).join(', ')} or ${words.at(-1)}`;
}

// One table for every extension kind. Only rows with data are listed; empty kinds share one sentence.
// “needs project trust” leads to Permissions & trust, where the route to trust the project is stated.
export function ExtensionsTable({ inspections, onRetry, projectTrusted = false, onOpenTrust }: { inspections: ResourceInspections; onRetry?: (area: keyof ResourceInspections) => void; projectTrusted?: boolean; onOpenTrust?: () => void }) {
  const listed = areas.map((area) => ({ area, rows: entries(area, inspections, projectTrusted), status: notice(inspections[area.id].state, area.noun, area.label) }));
  const rows = listed.flatMap((entry) => entry.rows);
  const notices = listed.filter((entry) => entry.status);
  const empty = listed.filter((entry) => !entry.status && !entry.rows.length).map((entry) => entry.area.noun);
  // Search lands on an area's notice when it has one, otherwise on its first row.
  const anchored = new Set(listed.filter((entry) => !entry.status && entry.rows.length).map((entry) => entry.rows[0].key));
  return <div className={styles.extensions}>
    {notices.map(({ area, status }) => <div key={area.id} {...anchor(`extensions.${area.id}`)}><ListNotice state={status!.state} message={status!.message} onRetry={onRetry ? () => onRetry(area.id) : undefined} /></div>)}
    {rows.length ? <div className={styles.tableScroll}><table className={styles.extensionTable}>
      <caption className="srOnly">Extensions</caption>
      <thead><tr><th scope="col">Name</th><th scope="col">Type</th><th scope="col">Scope</th><th scope="col">Status</th></tr></thead>
      <tbody>{rows.map((row) => <tr key={row.key} {...(anchored.has(row.key) ? anchor(`extensions.${row.area.id}`) : {})}>
        <th scope="row"><span>{row.name}</span>{row.detail ? <small>{row.detail}</small> : null}</th>
        <td>{row.area.type}</td><td>{row.scope}</td><td>{row.status}{row.needsTrust ? <> · {onOpenTrust ? <button type="button" className={styles.inlineLink} onClick={onOpenTrust}>needs project trust</button> : 'needs project trust'}</> : null}</td>
      </tr>)}</tbody>
    </table></div> : null}
    {empty.length === areas.length ? <ListNotice state="empty" message="No skills, prompts, plugins, MCP servers or language servers found." />
      : empty.length ? <p className={styles.hint}>No {sentenceList(empty)} found.</p> : null}
  </div>;
}
