import { useId, useState } from 'react';
import { CheckIcon } from '@phosphor-icons/react/dist/csr/Check';
import { Button } from '../../ui/Button';
import { Status, type StatusValue } from '../../ui/Status';
import { Surface } from '../../ui/Surface';
import type { PermissionDecision, PermissionIntent, PermissionView } from '../contracts';
import { validPermission } from '../requestValidation';
import { useRequestIntent } from '../useRequestIntent';
import styles from './widgets.module.css';

const labels: Record<PermissionDecision, string> = { allow: 'Allow once', allow_session: 'Allow for this session', deny: 'Deny' };
const scopes: Record<PermissionDecision, string> = {
  allow: 'Only this request, this one time.',
  allow_session: 'This kind of action in this chat until AVA restarts.',
  deny: 'AVA continues without doing this.',
};

function header(valid: boolean, request: PermissionView, chosen: PermissionDecision | undefined): Readonly<{ status: StatusValue; label: string }> {
  if (!valid) return { status: 'unavailable', label: 'Can’t be answered' };
  if (chosen) return { status: 'completed', label: 'Decided' };
  if (request.state === 'ready') return { status: 'approval', label: 'Needs your decision' };
  if (request.state === 'pending' || request.state === 'submitted') return { status: 'queued', label: 'Waiting for AVA' };
  if (request.state === 'error') return { status: 'failed', label: 'Couldn’t complete' };
  return { status: 'unavailable', label: request.state === 'stale' ? 'Replaced' : 'Closed' };
}
function outcome(valid: boolean, request: PermissionView, chosen: PermissionDecision | undefined) {
  if (!valid) return 'This request can’t be answered here. It’s incomplete or unsupported.';
  if (chosen) return `You chose ${labels[chosen]}.`;
  if (request.state === 'pending') return 'Not ready for a decision yet.';
  if (request.state === 'submitted') return 'Your decision was sent.';
  if (request.state === 'error') return request.error || 'This request couldn’t be completed. Nothing was allowed.';
  if (request.state === 'stale') return 'Replaced by a newer request.';
  if (request.state === 'closed') return 'Closed by AVA · nothing was allowed.';
  return '';
}

// No default choice: every advertised decision has equal weight. Deny comes first and sits apart,
// so the first choice keyboard focus reaches (inline or in the dialog) is the one that allows
// nothing. The result is a receipt naming the choice: `decided` is the host's stored decision for
// this request, so the receipt survives a remount; the mounted gate sends at most one per version.
export function PermissionCard({ request, decided, onDecide, titled = true }: { request: PermissionView; decided?: PermissionDecision; onDecide?: (intent: PermissionIntent) => void; titled?: boolean }) {
  const id = useId();
  const gate = useRequestIntent(request);
  const key = JSON.stringify([request.id, request.version]);
  const [receipts, setReceipts] = useState<ReadonlyMap<string, PermissionDecision>>(() => new Map());
  const valid = validPermission(request);
  const stored = valid && decided !== undefined && request.decisions.includes(decided) ? decided : undefined;
  const chosen = valid ? stored ?? receipts.get(key) : undefined;
  const enabled = valid && request.state === 'ready' && !chosen && !gate.submitted && Boolean(onDecide);
  const status = header(valid, request, chosen);
  const allows = valid ? request.decisions.filter((decision) => decision !== 'deny') : [];
  const deny = valid && request.decisions.includes('deny');
  function decide(decision: PermissionDecision) {
    if (!enabled || !onDecide || !request.decisions.includes(decision) || !gate.claim()) return;
    setReceipts((value) => new Map(value).set(key, decision));
    onDecide({ id: request.id, version: request.version, decision });
  }
  function choice(decision: PermissionDecision) {
    const picked = chosen === decision;
    return <div key={decision} className={styles.choice}>
      <Button className={styles.choiceButton} data-autofocus={decision === 'deny' || undefined} aria-describedby={`${id}-${decision}`} aria-pressed={chosen ? picked : undefined} aria-disabled={picked || undefined} disabled={!enabled && !picked} onClick={() => decide(decision)}>
        {picked ? <CheckIcon size={14} aria-hidden="true" /> : null}{labels[decision]}
      </Button>
      <span id={`${id}-${decision}`} className={styles.scopeText}>{request.scopes?.[decision]?.trim() || scopes[decision]}</span>
    </div>;
  }
  return (
    <Surface className={styles.request} aria-labelledby={titled ? `${id}-title` : undefined} aria-label={titled ? undefined : request.title}>
      <p className={styles.requestStatus}><Status status={status.status} label={status.label} />{request.requester?.trim() ? <span className={styles.requester}>Requested by {request.requester}</span> : null}</p>
      {titled ? <h3 id={`${id}-title`} className={styles.title}>{request.title}</h3> : null}
      <p className={styles.description}>{request.summary}</p>
      {request.action || request.target ? <dl className={styles.subject}>
        {request.action ? <div><dt>Action</dt><dd><code>{request.action}</code></dd></div> : null}
        {request.target ? <div><dt>Target</dt><dd><code>{request.target}</code></dd></div> : null}
      </dl> : null}
      {request.reason ? <p className={styles.reason}>{request.reason}</p> : null}
      {valid ? <div className={styles.choices} role="group" aria-label="Your decision">
        {deny ? <>{choice('deny')}{allows.length ? <div className={styles.choiceDivider} role="presentation" /> : null}</> : null}
        {allows.map(choice)}
      </div> : null}
      <p className={styles.outcome} role="status">{chosen ? <CheckIcon size={14} aria-hidden="true" /> : null}{outcome(valid, request, chosen)}</p>
    </Surface>
  );
}
