import { useId } from 'react';
import { Button } from '../../ui/Button';
import { Surface } from '../../ui/Surface';
import type { PermissionDecision, PermissionIntent, PermissionView } from '../contracts';
import { validPermission } from '../requestValidation';
import { useRequestIntent } from '../useRequestIntent';
import styles from './widgets.module.css';

const labels: Record<PermissionDecision, string> = { allow: 'Allow once', allow_session: 'Allow for session', deny: 'Deny' };

export function PermissionCard({ request, onDecide }: { request: PermissionView; onDecide?: (intent: PermissionIntent) => void }) {
  const titleId = useId();
  const gate = useRequestIntent(request);
  const valid = validPermission(request);
  const enabled = valid && request.state === 'ready' && !gate.submitted && Boolean(onDecide);
  return (
    <Surface aria-labelledby={titleId}>
      <p className={styles.eyebrow}>Permission request</p>
      <h3 id={titleId} className={styles.title}>{request.title}</h3>
      <p className={styles.description}>{request.summary}</p>
      {request.target ? <code className={styles.target}>{request.target}</code> : null}
      <p className={styles.scope}>Choices emit UI intent only. The host owns effective scope and policy.</p>
      <p className={styles.state} role="status">{!valid ? 'Unsupported or invalid request.' : gate.submitted ? 'Intent submitted · awaiting host state' : request.state}</p>
      {request.state === 'error' ? <p className={styles.description}>{request.error || 'Request unavailable. A revised request is required.'}</p> : null}
      <div className={styles.actions}>
        {valid ? request.decisions.map((decision) => <Button key={decision} disabled={!enabled} onClick={() => {
          if (!enabled || !onDecide || !request.decisions.includes(decision) || !gate.claim()) return;
          onDecide({ id: request.id, version: request.version, decision });
        }}>{labels[decision]}</Button>) : null}
      </div>
    </Surface>
  );
}
