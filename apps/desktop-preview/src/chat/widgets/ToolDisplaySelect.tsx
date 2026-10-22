import { useRef } from 'react';
import { canFocus } from '../../ui/DialogFrame';
import { isToolDisplay, type ToolDisplay } from '../toolDisplay';

// Shared native presentation control. Its caller owns preference authority.
// `custom` means the caller's tools no longer match the preset (an Expand/Collapse override), so the
// control says Custom rather than a preset that isn't showing; choosing any preset, even the
// current one, is then a real change the caller applies.
export function ToolDisplaySelect({ display, showCompletedOutput = true, custom = false, disabled = false, className, onChange }: {
  display: ToolDisplay; showCompletedOutput?: boolean; custom?: boolean; disabled?: boolean; className?: string; onChange: (display: ToolDisplay, control: HTMLSelectElement) => void;
}) {
  const ref = useRef<HTMLSelectElement>(null);
  const snapshot = { display, showCompletedOutput, custom, disabled, onChange };
  const latest = useRef(snapshot); latest.current = snapshot;
  const shownCustom = custom || !showCompletedOutput;
  return <select ref={ref} data-tool-display className={className} aria-label="Tool details" value={shownCustom ? 'custom' : display} disabled={disabled} onChange={(event) => {
    const control = event.currentTarget; const next = control.value;
    if (latest.current !== snapshot || control !== ref.current || disabled || !canFocus(control) || !isToolDisplay(next) || next === display && !shownCustom) return;
    onChange(next, control);
  }}>
    {shownCustom ? <option value="custom" disabled>{showCompletedOutput ? 'Custom' : 'Custom · output hidden'}</option> : null}
    <option value="compact">Compact</option><option value="auto">Auto</option><option value="expanded">Expanded</option>
  </select>;
}
