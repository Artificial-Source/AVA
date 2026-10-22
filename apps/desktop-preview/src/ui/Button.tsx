import type { ComponentProps } from 'react';
import { ArrowLeftIcon } from '@phosphor-icons/react/dist/csr/ArrowLeft';
import styles from './primitives.module.css';

// destructive: neutral surface with --danger-text label and border; never a red fill.
type ButtonProps = ComponentProps<'button'> & { tone?: 'primary' | 'quiet' | 'destructive' };

export function Button({ type = 'button', tone = 'quiet', className = '', ...props }: ButtonProps) {
  return <button {...props} type={type} data-tone={tone} className={`${styles.button} ${className}`} />;
}

// quiet: borderless 30px hit area, the inspector-close look.
export function IconButton({ label, variant = 'bordered', className = '', ...props }: Omit<ButtonProps, 'aria-label'> & { label: string; variant?: 'bordered' | 'quiet' }) {
  return <Button {...props} aria-label={label} data-variant={variant} className={`${styles.icon} ${className}`} />;
}

// One Back treatment everywhere: a quiet control with a leading arrow and a "Back to …" label.
// BackLink is for leaving the page; it looks identical to BackButton.
export function BackButton({ type = 'button', className = '', children, ...props }: ComponentProps<'button'>) {
  return <button {...props} type={type} className={`${styles.back} ${className}`}><ArrowLeftIcon size={14} aria-hidden="true" />{children}</button>;
}

export function BackLink({ className = '', children, ...props }: ComponentProps<'a'>) {
  return <a {...props} className={`${styles.back} ${className}`}><ArrowLeftIcon size={14} aria-hidden="true" />{children}</a>;
}
