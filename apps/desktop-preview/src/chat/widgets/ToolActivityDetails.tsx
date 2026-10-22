import { useContext } from 'react';
import { XCircleIcon } from '@phosphor-icons/react/dist/csr/XCircle';
import { AskRetryContext } from '../askRetry';
import type { ToolView } from '../contracts';
import { paintText } from '../textPaint';
import { useBottomFollow } from '../useBottomFollow';
import { ToolDiffView } from './ToolDiffView';
import { ToolImagePreview } from './ToolImagePreview';
import { IncrementalText } from './IncrementalText';
import styles from './ToolActivityDetails.module.css';

function ToolOutput({ record }: { record: ToolView }) {
  const text = record.output!.text;
  const follow = useBottomFollow<HTMLPreElement>(text);
  return <><pre ref={follow.ref} className={styles.output} aria-label={`Output: ${record.action}`} tabIndex={0} onScroll={follow.onScroll} onPointerDown={follow.onPointerDown} onKeyDown={follow.onKeyDown} onBlur={follow.onBlur}><code><IncrementalText text={paintText(text)} /></code></pre>
    {follow.away ? <button ref={follow.latestRef} type="button" className={styles.latest} onClick={follow.latest}>Latest output</button> : null}
    {record.output!.truncated ? <p>Output was cut off before the end.</p> : null}
    {text.length > 20000 || text.split('\n').length > 200 ? <p>Showing the first 200 lines or 20,000 characters of output.</p> : null}</>;
}

// The cause leads a failed row so a passing-looking image or output never reads as success.
function failureCause(record: ToolView) {
  if (record.error?.trim()) return record.error;
  return record.output ? 'This step failed. Its output is below.' : 'AVA didn’t report why this failed.';
}

// Loaded only for explicitly expanded output. No file, process or provider authority.
export default function ToolActivityDetails({ record, showCompletedOutput = true }: { record: ToolView; showCompletedOutput?: boolean }) {
  const askRetry = useContext(AskRetryContext);
  const outputHidden = record.state === 'completed' && !showCompletedOutput && Boolean(record.output);
  const other = record.state === 'failed' || Boolean(record.resource) || record.changes !== undefined;
  return <div className={styles.details}>
    {/* The next step sits on the cause line itself, where the failure is read. */}
    {record.state === 'failed' ? <p className={styles.failure}><XCircleIcon size={14} aria-hidden="true" /><span>{failureCause(record)}{askRetry ? <>{' '}<button type="button" className={styles.askRetry} onClick={askRetry}>Ask AVA to retry</button></> : null}</span></p> : null}
    {record.resource ? <><p>{record.detail}</p><code className={styles.path}>{record.resource.path}</code></> : null}
    {record.resource?.kind === 'image' ? <ToolImagePreview key={record.id} previewId={record.resource.previewId} /> : null}
    {outputHidden ? <p>Output hidden by your Completed-tool output setting.</p> : record.output ? <ToolOutput record={record} /> : other ? null : <p>{record.state === 'queued' || record.state === 'running' ? 'No output yet.' : 'No output.'}</p>}
    {record.changes !== undefined ? <ToolDiffView changes={record.changes} /> : null}
  </div>;
}
