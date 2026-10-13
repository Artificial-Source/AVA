import { providerIdentity } from './providerIdentity';
import styles from './ProviderMark.module.css';

// Decorative only: the adjacent supplied label owns the accessible identity.
export function ProviderMark({ id }: { id: string }) {
  const identity = providerIdentity(id);
  return <span aria-hidden="true" data-provider-mark={identity ? id : 'neutral'} className={identity ? styles.mark : styles.fallback}
    style={identity ? { maskImage: `url("/provider-marks/${identity.file}")`, WebkitMaskImage: `url("/provider-marks/${identity.file}")` } : undefined} />;
}
