import { useRef, useState, type SyntheticEvent } from 'react';
import { MagnifyingGlassMinusIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlassMinus';
import { MagnifyingGlassPlusIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlassPlus';
import { toolImagePreview } from '../toolImages';
import styles from './ToolActivityDetails.module.css';

type InputImageSource = Readonly<{ src: string; alt: string; width: number; height: number }>;

function InputImage({ image }: { image: InputImageSource }) {
  const current = useRef<HTMLImageElement>(null);
  const [state, setState] = useState<'loading' | 'loaded' | 'error'>('loading');
  const [enlarged, setEnlarged] = useState(false);
  function settle(event: SyntheticEvent<HTMLImageElement>, failed: boolean) {
    const node = event.currentTarget;
    // The current image/source owns delivery, not an immutable tool snapshot.
    if (current.current !== node || !node.isConnected || node.getAttribute('src') !== image.src) return;
    setState(failed || !node.naturalWidth || !node.naturalHeight ? 'error' : 'loaded');
  }
  // A failed load is one line: no empty frame and no caption for pixels that never arrived.
  if (state === 'error') return <p>Couldn’t load the input image.</p>;
  return <figure className={styles.imagePreview} data-enlarged={enlarged}>
    <button type="button" className={styles.imageButton} aria-pressed={enlarged} onClick={() => setEnlarged((value) => !value)}>
      <span className="srOnly">Enlarge image: </span>
      <span className={styles.imageFrame}>
        <img ref={current} className={`${styles.image} ${state === 'loading' ? styles.imageLoading : ''}`} src={image.src} alt={image.alt} width={image.width} height={image.height} loading="lazy" decoding="async" onLoad={(event) => settle(event, false)} onError={(event) => settle(event, true)} />
        {state === 'loading' ? <span className={styles.imageStatus}>Loading image…</span> : null}
      </span>
      <span className={styles.zoom} aria-hidden="true">{enlarged ? <MagnifyingGlassMinusIcon size={12} /> : <MagnifyingGlassPlusIcon size={12} />}</span>
    </button>
    <figcaption>Input image</figcaption>
  </figure>;
}

export function ToolImagePreview({ previewId }: { previewId: unknown }) {
  const image = toolImagePreview(previewId);
  return image ? <InputImage key={image.src} image={image} /> : <p>No preview available for this image.</p>;
}
