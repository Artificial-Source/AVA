import { PauseIcon } from '@phosphor-icons/react/dist/csr/Pause';
import { PlayIcon } from '@phosphor-icons/react/dist/csr/Play';
import { SkipForwardIcon } from '@phosphor-icons/react/dist/csr/SkipForward';
import type { PreviewExample } from '../chat/suppliedRunFixture';
import banner from './PreviewBanner.module.css';
export { suppliedSnapshots } from '../chat/suppliedRunFixture';
export { createRunDriver } from '../chat/suppliedRunDriver';
export type { PreviewExample } from '../chat/suppliedRunFixture';

export function PreviewControls({ example, running, paused, onExample, onPause, onStep }: {
  example: PreviewExample; running: boolean; paused: boolean;
  onExample: (example: PreviewExample) => void; onPause: () => void; onStep: () => void;
}) {
  return <>
    <select className={banner.select} aria-label="Preview example" title={running ? 'Stop the preview to choose another example' : 'Choose which example reply to play'} value={example} disabled={running} onChange={(event) => onExample(event.target.value as PreviewExample)}>
      <option value="completion">Completion</option><option value="failure">Failure</option><option value="burst">Burst delivery</option>
    </select>
    {running ? <>
      <button className={`${banner.control} ${banner.quiet}`} type="button" onClick={onPause} title={paused ? 'Resume the preview reply' : 'Pause the preview reply'}>
        {paused ? <PlayIcon size={13} aria-hidden="true" /> : <PauseIcon size={13} aria-hidden="true" />}{paused ? 'Resume preview' : 'Pause preview'}
      </button>
      {paused ? <button className={`${banner.control} ${banner.quiet}`} type="button" onClick={onStep} title="Play the next part of the reply">
        <SkipForwardIcon size={13} aria-hidden="true" />Next snapshot
      </button> : null}
    </> : null}
  </>;
}
