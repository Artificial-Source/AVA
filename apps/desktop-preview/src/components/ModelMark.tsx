import type { ModelId } from '../state/types';

// First-party geometric marks for the invented preview catalog, not provider logos.
export function ModelMark({ model }: { model: ModelId }) {
  return (
    <svg data-model-mark={model} width="14" height="14" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" aria-hidden="true" focusable="false">
      {model === 'aster' ? <><path d="m10 1 9 9-9 9-9-9Z" /><path d="M10 1v18M1 10h18" /><path d="m10 6 4 4-4 4-4-4Z" fill="currentColor" stroke="none" /></> : null}
      {model === 'aster-mini' ? <><path d="m10 3 7 7-7 7-7-7Z" /><path d="m10 7 3 3-3 3-3-3Z" fill="currentColor" stroke="none" /></> : null}
      {model === 'orion' ? <><path d="M3 4h14v12H3zM3 10h14" /><path d="M7 4v6m6 0v6" /></> : null}
    </svg>
  );
}
