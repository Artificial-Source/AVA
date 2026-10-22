import { Button } from '../../ui/Button';
import { Status, type StatusValue } from '../../ui/Status';
import type { RunView } from '../contracts';
import styles from './widgets.module.css';

const errors: Readonly<Record<string, string>> = {
  unavailable: 'The service AVA uses is unavailable right now.',
  rate_limited: 'Too many requests right now. Try again in a moment.',
  invalid_request: 'The request couldn’t be accepted.',
};

function notice(view: RunView): Readonly<{ status: StatusValue; label?: string; message: string; cause?: string }> {
  if (view.state === 'error') {
    const known = Object.hasOwn(errors, view.code);
    const cause = view.message?.trim() || (known ? undefined : 'AVA didn’t report why.');
    return { status: 'failed', message: known ? errors[view.code] : 'The run couldn’t continue.', cause };
  }
  if (view.state === 'retrying') return { status: 'working', label: 'Retrying', message: `Attempt ${Number.isSafeInteger(view.attempt) && view.attempt > 0 ? view.attempt : 'number unknown'}`, cause: view.cause };
  if (view.state === 'running') return { status: 'working', message: 'AVA is working on this.' };
  if (view.state === 'cancel-requested') return { status: 'queued', label: 'Stopping', message: 'Waiting for the run to stop.' };
  if (view.state === 'canceled') return { status: 'stopped', message: 'The run was stopped before it finished.' };
  return { status: 'completed', message: 'The run finished.' };
}

// Status first, then one plain sentence. Retry appears only when the caller can actually retry;
// a failure the caller can't retry still gets a next step when it can ask AVA to try again.
export function RunNotice({ view, onRetry, onAskRetry }: { view: RunView; onRetry?: () => void; onAskRetry?: () => void }) {
  const { status, label, message, cause } = notice(view);
  const ask = view.state === 'error' && !onRetry && onAskRetry ? onAskRetry : undefined;
  return <section className={styles.runNotice} aria-label="Run status" data-state={view.state}>
    <p className={styles.runLine}><Status status={status} label={label} /><span>{message}</span></p>
    {cause || ask ? <p className={styles.runCause}>{cause}{cause && ask ? ' ' : null}{ask ? <button type="button" className={styles.askRetry} onClick={ask}>Ask AVA to retry</button> : null}</p> : null}
    {view.state === 'error' && onRetry ? <Button className={styles.retry} onClick={onRetry}>Retry</Button> : null}
  </section>;
}
