import { useRef } from 'react';
import { canFocus } from '../../ui/DialogFrame';
import { isToolDisplay, type ToolDisplay } from '../toolDisplay';

// Shared native presentation control. Its caller owns preference authority.
export function ToolDisplaySelect({ display, showCompletedOutput = true, disabled = false, className, onChange }: {
  display: ToolDisplay; showCompletedOutput?: boolean; disabled?: boolean; className?: string; onChange: (display: ToolDisplay, control: HTMLSelectElement) => void;
}) {
  const ref = useRef<HTMLSelectElement>(null);
  const snapshot = { display, showCompletedOutput, disabled, onChange };
  const latest = useRef(snapshot); latest.current = snapshot;
  return <select ref={ref} data-tool-display className={className} aria-label="Chat display" value={showCompletedOutput ? display : 'custom'} disabled={disabled} onChange={(event) => {
    const control = event.currentTarget; const next = control.value;
    if (latest.current !== snapshot || control !== ref.current || disabled || !canFocus(control) || !isToolDisplay(next) || next === display && showCompletedOutput) return;
    onChange(next, control);
  }}>
    {!showCompletedOutput ? <option value="custom" disabled>Custom · output hidden</option> : null}
    <option value="compact">Compact</option><option value="auto">Auto</option><option value="expanded">Expanded</option>
  </select>;
}
