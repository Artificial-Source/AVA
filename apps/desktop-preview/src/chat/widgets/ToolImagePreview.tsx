import { useRef, useState, type SyntheticEvent } from 'react';
import { toolImagePreview } from '../toolImages';
import styles from './ToolActivityDetails.module.css';

function SuppliedImage({ image }: { image: NonNullable<ReturnType<typeof toolImagePreview>> }) {
  const current = useRef<HTMLImageElement>(null);
  const [state, setState] = useState<'loading' | 'loaded' | 'error'>('loading');
  function settle(event: SyntheticEvent<HTMLImageElement>, failed: boolean) {
    const node = event.currentTarget;
    // The current image/source owns delivery, not an immutable tool snapshot.
    if (current.current !== node || !node.isConnected || node.getAttribute('src') !== image.src) return;
    setState(failed || !node.naturalWidth || !node.naturalHeight ? 'error' : 'loaded');
  }
  return <figure className={styles.imagePreview}>
    <div className={styles.imageFrame}>
      {state === 'error' ? <p className={styles.imageStatus}>Image preview unavailable. Supplied image could not be loaded.</p> : <img ref={current} className={`${styles.image} ${state === 'loading' ? styles.imageLoading : ''}`} src={image.src} alt={image.alt} width={image.width} height={image.height} loading="lazy" decoding="async" onLoad={(event) => settle(event, false)} onError={(event) => settle(event, true)} />}
      {state === 'loading' ? <p className={styles.imageStatus}>Loading supplied image…</p> : null}
    </div>
    <figcaption>{image.label}</figcaption>
  </figure>;
}

export function ToolImagePreview({ previewId }: { previewId: unknown }) {
  const image = toolImagePreview(previewId);
  return image ? <SuppliedImage key={image.src} image={image} /> : <p>Image preview unavailable. No supplied image.</p>;
}
