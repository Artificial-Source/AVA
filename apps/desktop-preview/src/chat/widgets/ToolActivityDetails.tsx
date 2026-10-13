import type { ToolView } from '../contracts';
import { paintText } from '../textPaint';
import { useBottomFollow } from '../useBottomFollow';
import { ToolDiffView } from './ToolDiffView';
import { IncrementalText } from './IncrementalText';
import styles from './ToolActivityDetails.module.css';

function ToolOutput({ record }: { record: ToolView }) {
  const text = record.output!.text;
  const follow = useBottomFollow<HTMLPreElement>(text);
  return <><pre ref={follow.ref} className={styles.output} aria-label={`Output: ${record.action}`} tabIndex={0} onScroll={follow.onScroll} onPointerDown={follow.onPointerDown} onBlur={follow.onBlur}><code><IncrementalText text={paintText(text)} /></code></pre>
    {follow.away ? <button ref={follow.latestRef} type="button" className={styles.latest} onClick={follow.latest}>Latest output</button> : null}
    {record.output!.truncated ? <p>Output truncated by the supplied view.</p> : null}
    {text.length > 20000 || text.split('\n').length > 200 ? <p>Display limited to 200 lines or 20,000 characters. Remaining supplied output text omitted.</p> : null}</>;
}
// Loaded only for explicitly expanded output. No file, process or provider authority.
export default function ToolActivityDetails({ record, showCompletedOutput = true }: { record: ToolView; showCompletedOutput?: boolean }) {
  return <div className={styles.details}>
    {record.resource ? <><p>{record.detail}</p><code className={styles.path}>{record.resource.path}</code></> : null}
    {record.state === 'completed' && !showCompletedOutput && record.output ? <p>Completed output hidden by your conversation preference.</p> : record.output ? <ToolOutput record={record} /> : <p>No output supplied.</p>}
    {record.changes !== undefined ? <ToolDiffView changes={record.changes} /> : null}
  </div>;
}
