import type { ComponentProps } from 'react';
import styles from './primitives.module.css';

export function Surface({ className = '', ...props }: ComponentProps<'section'>) {
  return <section {...props} className={`${styles.surface} ${className}`} />;
}
