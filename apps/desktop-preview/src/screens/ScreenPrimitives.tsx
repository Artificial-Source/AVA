import { useId, type ReactNode } from 'react';
import { Button } from '../ui/Button';
import type { ListState } from './contracts';
import styles from './screens.module.css';

export function FormField({ label, hint, error, children }: { label: string; hint?: string; error?: string; children: (props: { id: string; 'aria-describedby'?: string; 'aria-invalid'?: boolean }) => ReactNode }) {
  const id = useId();
  return <div className={styles.field}><label htmlFor={id}>{label}</label>{children({ id, 'aria-describedby': hint || error ? `${id}-help` : undefined, 'aria-invalid': Boolean(error) })}{hint || error ? <p id={`${id}-help`} className={styles.hint} role={error ? 'alert' : undefined}>{error || hint}</p> : null}</div>;
}

export function SearchField({ label, value, onChange }: { label: string; value: string; onChange: (value: string) => void }) {
  return <FormField label={label}>{(props) => <input {...props} className={styles.input} type="search" autoComplete="off" value={value} onChange={(event) => onChange(event.currentTarget.value)} />}</FormField>;
}

export function PreferenceRow({ title, description, children }: { title: string; description: string; children?: ReactNode }) {
  return <div className={styles.preference}><div><h3>{title}</h3><p>{description}</p></div>{children ? <div className={styles.preferenceControl}>{children}</div> : null}</div>;
}

const stateText: Record<Exclude<ListState, 'ready'>, string> = {
  loading: 'Loading example · no background request is running.',
  empty: 'No entries supplied in this example.',
  error: 'The example could not be loaded. A revised view is required.',
  stale: 'Last supplied example · stale. Selection is disabled until a current view is supplied.',
  unavailable: 'Unavailable in this example. No connection or configuration will be changed.',
  busy: 'A run is active in this example. Changes are unavailable until a revised view is supplied.',
  disabled: 'This action is unavailable in the browser review.',
};
export function ListNotice({ state, onRetry }: { state: ListState; onRetry?: () => void }) {
  if (state === 'ready') return null;
  return <div className={styles.notice}><p role="status">{stateText[state] ?? 'Unsupported view state. Actions are disabled.'}</p>{state === 'error' && onRetry ? <Button onClick={onRetry}>Retry example</Button> : null}</div>;
}

export function Confirmation({ children, label, disabled, onConfirm, onCancel }: { children: ReactNode; label: string; disabled?: boolean; onConfirm: () => void; onCancel: () => void }) {
  return <form onSubmit={(event) => { event.preventDefault(); if (!disabled) onConfirm(); }}>{children}<div className={styles.actions}><Button onClick={onCancel}>Cancel</Button><Button type="submit" tone="primary" disabled={disabled}>{label}</Button></div></form>;
}
