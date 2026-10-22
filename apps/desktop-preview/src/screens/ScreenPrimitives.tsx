import { useId, type ReactNode } from 'react';
import { CircleNotchIcon } from '@phosphor-icons/react/dist/csr/CircleNotch';
import { ClockCounterClockwiseIcon } from '@phosphor-icons/react/dist/csr/ClockCounterClockwise';
import { HourglassMediumIcon } from '@phosphor-icons/react/dist/csr/HourglassMedium';
import { LockSimpleIcon } from '@phosphor-icons/react/dist/csr/LockSimple';
import { MinusCircleIcon } from '@phosphor-icons/react/dist/csr/MinusCircle';
import { TrayIcon } from '@phosphor-icons/react/dist/csr/Tray';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { Button } from '../ui/Button';
import feedback from '../ui/feedback.module.css';
import type { ListState } from './contracts';
import styles from './screens.module.css';

// Error first in the description so it is announced before the standing hint.
export function FormField({ label, hint, error, children }: { label: string; hint?: string; error?: string; children: (props: { id: string; 'aria-describedby'?: string; 'aria-invalid'?: boolean }) => ReactNode }) {
  const id = useId();
  const errorId = error ? `${id}-error` : undefined;
  const hintId = hint ? `${id}-hint` : undefined;
  const describedBy = errorId && hintId ? `${errorId} ${hintId}` : errorId ?? hintId;
  return <div className={`${styles.field} ${feedback.formField}`}>
    <label htmlFor={id}>{label}</label>
    {children({ id, 'aria-describedby': describedBy, 'aria-invalid': error ? true : undefined })}
    {error ? <p id={errorId} className={feedback.fieldError} role="alert"><WarningCircleIcon size={14} aria-hidden="true" /><span>{error}</span></p> : null}
    {hint ? <p id={hintId} className={styles.hint}>{hint}</p> : null}
  </div>;
}

export function SearchField({ label, value, onChange }: { label: string; value: string; onChange: (value: string) => void }) {
  return <FormField label={label}>{(props) => <input {...props} className={styles.input} type="search" autoComplete="off" value={value} onChange={(event) => onChange(event.currentTarget.value)} />}</FormField>;
}

export function PreferenceRow({ title, description, children }: { title: string; description: string; children?: ReactNode }) {
  return <div className={styles.preference}><div><h3>{title}</h3><p>{description}</p></div>{children ? <div className={styles.preferenceControl}>{children}</div> : null}</div>;
}

const notices: Record<Exclude<ListState, 'ready'>, { icon: ReactNode; text: string }> = {
  loading: { icon: <CircleNotchIcon size={16} />, text: 'Loading…' },
  empty: { icon: <TrayIcon size={16} />, text: 'Nothing here yet.' },
  error: { icon: <WarningCircleIcon size={16} />, text: 'Couldn’t load this list.' },
  stale: { icon: <ClockCounterClockwiseIcon size={16} />, text: 'This list may be out of date.' },
  unavailable: { icon: <MinusCircleIcon size={16} />, text: 'This isn’t available.' },
  busy: { icon: <HourglassMediumIcon size={16} />, text: 'A run is in progress. Stop it or wait for it to finish to make changes.' },
  disabled: { icon: <LockSimpleIcon size={16} />, text: 'This action is turned off.' },
};
const unsupported = { icon: <WarningCircleIcon size={16} />, text: 'This list can’t be shown. Actions are turned off.' };

// Errors (and unknown states) interrupt as alerts; everything else is a polite status.
export function ListNotice({ state, onRetry, message, retryLabel = 'Retry' }: { state: ListState; onRetry?: () => void; message?: string; retryLabel?: string }) {
  if (state === 'ready') return null;
  const known = Object.hasOwn(notices, state);
  const notice = known ? notices[state] : unsupported;
  const alert = !known || state === 'error';
  return <div className={feedback.notice} data-state={known ? state : 'error'}>
    <span className={state === 'loading' ? `${feedback.noticeIcon} ${feedback.spin}` : feedback.noticeIcon} aria-hidden="true">{notice.icon}</span>
    <div className={feedback.noticeBody}>
      <p className={feedback.noticeMessage} role={alert ? 'alert' : 'status'}>{message || notice.text}</p>
      {state === 'error' && onRetry ? <Button onClick={onRetry}>{retryLabel}</Button> : null}
    </div>
  </div>;
}

export function Confirmation({ children, label, disabled, tone = 'primary', onConfirm, onCancel }: { children: ReactNode; label: string; disabled?: boolean; tone?: 'primary' | 'destructive'; onConfirm: () => void; onCancel: () => void }) {
  return <form onSubmit={(event) => { event.preventDefault(); if (!disabled) onConfirm(); }}>{children}<div className={styles.actions}><Button onClick={onCancel}>Cancel</Button><Button type="submit" tone={tone} disabled={disabled}>{label}</Button></div></form>;
}
