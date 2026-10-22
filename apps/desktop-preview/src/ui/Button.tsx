import type { ComponentProps } from 'react';
import styles from './primitives.module.css';

type ButtonProps = ComponentProps<'button'> & { tone?: 'primary' | 'quiet' };

export function Button({ type = 'button', tone = 'quiet', className = '', ...props }: ButtonProps) {
  return <button {...props} type={type} data-tone={tone} className={`${styles.button} ${className}`} />;
}

export function IconButton({ label, className = '', ...props }: Omit<ButtonProps, 'aria-label'> & { label: string }) {
  return <Button {...props} aria-label={label} className={`${styles.icon} ${className}`} />;
}
