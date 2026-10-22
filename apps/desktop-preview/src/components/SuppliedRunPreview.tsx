import type { PreviewExample } from '../chat/suppliedRunFixture';
export { suppliedSnapshots } from '../chat/suppliedRunFixture';
export { createRunDriver } from '../chat/suppliedRunDriver';
export type { PreviewExample } from '../chat/suppliedRunFixture';

export function PreviewControls({ example, running, paused, onExample, onPause, onStep }: {
  example: PreviewExample; running: boolean; paused: boolean;
  onExample: (example: PreviewExample) => void; onPause: () => void; onStep: () => void;
}) {
  return <>
    <select aria-label="Preview example" value={example} disabled={running} onChange={(event) => onExample(event.target.value as PreviewExample)}>
      <option value="completion">Completion</option><option value="failure">Failure</option><option value="burst">Burst delivery</option>
    </select>
    {running ? <><button type="button" onClick={onPause} title="Pause supplied snapshot delivery only">{paused ? 'Resume preview' : 'Pause preview'}</button>{paused ? <button type="button" onClick={onStep}>Next snapshot</button> : null}</> : null}
  </>;
}
