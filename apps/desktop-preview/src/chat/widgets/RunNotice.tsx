import { Button } from '../../ui/Button';
import { Surface } from '../../ui/Surface';
import type { RunView } from '../contracts';
import styles from './widgets.module.css';

const errors: Readonly<Record<string, string>> = {
  unavailable: 'The supplied service is unavailable.',
  rate_limited: 'The supplied request was rate limited.',
  invalid_request: 'The supplied request could not be accepted.',
};

export function RunNotice({ view, onRetry }: { view: RunView; onRetry?: () => void }) {
  const message = view.state === 'error'
    ? Object.hasOwn(errors, view.code) ? errors[view.code] : 'The run could not continue. No further error details are available.'
    : view.state === 'retrying' ? 'Retry in progress · supplied state'
      : view.state === 'cancel-requested' ? 'Cancellation requested · not yet confirmed'
        : view.state === 'canceled' ? 'Canceled · supplied terminal state' : 'Completed · supplied terminal state';
  return <Surface aria-label="Run notice">
    <p className={styles.title}>{message}</p>
    {view.state === 'retrying' ? <p className={styles.description}>Attempt {Number.isSafeInteger(view.attempt) && view.attempt > 0 ? view.attempt : 'unavailable'} · {view.cause}</p> : null}
    {view.state === 'error' && onRetry ? <div className={styles.actions}><Button onClick={onRetry}>Request retry</Button></div> : null}
  </Surface>;
}
