import { providerIdentity } from './providerIdentity';
import styles from './ProviderMark.module.css';

// Decorative only: the adjacent label owns the accessible identity. Providers
// without a local mark get a letter tile, never an empty outline that reads as a radio.
export function ProviderMark({ id, label }: { id: string; label?: string }) {
  const identity = providerIdentity(id);
  if (identity) return <span aria-hidden="true" data-provider-mark={id} className={styles.mark}
    style={{ maskImage: `url("/provider-marks/${identity.file}")`, WebkitMaskImage: `url("/provider-marks/${identity.file}")` }} />;
  return <span aria-hidden="true" data-provider-mark="neutral" className={styles.monogram}>{String(label ?? id).match(/[\p{L}\p{N}]/u)?.[0].toLocaleUpperCase()}</span>;
}
