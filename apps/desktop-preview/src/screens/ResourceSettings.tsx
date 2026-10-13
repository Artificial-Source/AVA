import type { InspectionView, McpInspection } from './contracts';
import { ListNotice, PreferenceRow } from './ScreenPrimitives';
import styles from './screens.module.css';

export function ResourceSettings({ view }: { view: InspectionView }) {
  return <><ListNotice state={view.state} />{['ready', 'stale'].includes(view.state) ? view.rows.map((row) => <PreferenceRow key={row.title} title={row.title} description={row.detail}><span className={styles.muted}>{row.status}</span></PreferenceRow>) : null}</>;
}

export function McpSettings({ view }: { view: McpInspection }) {
  return <><p className={styles.muted}>Supplied stdio configuration · read-only inspection</p><ListNotice state={view.state} />
    {['ready', 'stale'].includes(view.state) ? <div className={styles.resourceCards}>{view.servers.map((server) => <article key={server.id} className={styles.resourceCard} aria-label={server.name}>
      <div className={styles.resourceHeading}><h3>{server.name}</h3><span className={styles.tag}>Example</span></div>
      <dl className={styles.facts}>
        <dt>Transport</dt><dd>{server.transport}</dd>
        <dt>Configuration</dt><dd>{server.configEnabled === null ? 'Unknown' : server.configEnabled ? 'Enabled' : 'Disabled'}</dd>
        <dt>Connection</dt><dd>{server.connection === 'not-connected' ? 'Not connected' : 'Unknown'}</dd>
        <dt>Process lifetime</dt><dd>{server.lifecycle === 'one-shot' ? 'One-shot · fresh process on next use' : 'Unknown'}</dd>
        <dt>Source</dt><dd>{server.source === 'global' ? 'Global' : 'Project'}</dd>
        <dt>Project trust</dt><dd>{server.trust === 'not-required' ? 'Not required for global scope' : server.trust === 'required' ? 'Required · not evaluated' : 'Unknown'}</dd>
      </dl>
    </article>)}</div> : null}
  </>;
}
