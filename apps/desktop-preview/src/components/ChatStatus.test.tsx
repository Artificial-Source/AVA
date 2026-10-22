import { readFileSync } from 'node:fs';
import { act, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { ChatStatus } from './ChatStatus';
import styles from './ChatStatus.module.css';

const css = readFileSync('src/components/ChatStatus.module.css', 'utf8');

const cases = [
  ['working', 'Working'],
  ['completed', 'Completed'],
  ['approval', 'Attention'],
  ['stopped', 'Stopped'],
  ['failed', 'Failed'],
  ['unavailable', 'Unavailable'],
  ['queued', 'Queued'],
] as const;

describe('shared status', () => {
  it.each(cases)('renders %s as a labeled status with a decorative glyph and no mock tooltip', (status, label) => {
    render(<button><ChatStatus status={status} />{' '}Example chat</button>);
    const badge = screen.getByText(label);
    const glyph = badge.firstElementChild!;
    expect(badge).toHaveAttribute('data-status', status);
    expect(badge).not.toHaveAttribute('title');
    expect(badge).toHaveTextContent(new RegExp(`^${label}$`));
    expect(screen.getByRole('button')).toHaveAccessibleName(`${label} Example chat`);
    expect(glyph).toHaveClass(styles.glyph);
    expect(glyph).toHaveAttribute('aria-hidden', 'true');
    expect(glyph.querySelectorAll('svg')).toHaveLength(1);
    expect(glyph.querySelector('svg')).toHaveAttribute('width', status === 'stopped' ? '11' : '12');
    // This is the icon-only CSS cycle hook, not proof of rendered motion.
    expect(glyph.classList.contains(styles.cycling)).toBe(status === 'working');
    expect(badge.matches('[data-status="working"]')).toBe(status === 'working');
    expect(badge).not.toHaveClass(styles.cycling);
  });

  it('keeps every non-idle glyph distinct without depending on color', () => {
    const { container } = render(<>{cases.map(([status]) => <ChatStatus key={status} status={status} />)}</>);
    const glyphs = container.querySelectorAll('[data-status] svg');
    expect(glyphs).toHaveLength(cases.length);
    const paths = Array.from(glyphs, (glyph) => Array.from(glyph.querySelectorAll('path'), (path) => path.getAttribute('d')).join(' '));
    expect(paths.every(Boolean)).toBe(true);
    expect(new Set(paths).size).toBe(cases.length);
  });

  it('renders Queued as a static neutral clock, distinct from the Working cycle', () => {
    render(<ChatStatus status="queued" />);
    const badge = screen.getByText('Queued');
    expect(badge).toHaveAttribute('data-status', 'queued');
    expect(badge.firstElementChild).not.toHaveClass(styles.cycling);
    expect(css).toMatch(/\.status\[data-status='queued'\] \{ color: var\(--status-queued, var\(--muted\)\); \}/);
    expect(css).not.toMatch(/data-status='queued'\][^{]*\{[^}]*animation/);
  });

  it('overrides only the visible word, keeping the status look and glyph', () => {
    const { container } = render(<><ChatStatus status="unavailable" /><ChatStatus status="unavailable" label="Not run" /></>);
    const [plain, renamed] = Array.from(container.querySelectorAll('[data-status]'));
    expect(renamed).toHaveTextContent(/^Not run$/);
    expect(renamed).toHaveAttribute('data-status', 'unavailable');
    expect(renamed.querySelector('svg')!.innerHTML).toBe(plain.querySelector('svg')!.innerHTML);
    expect(screen.queryAllByText('Unavailable')).toHaveLength(1);
  });

  it('renders nothing for idle, with no empty label or motion wrapper', () => {
    const { container } = render(<ChatStatus status="idle" />);
    expect(container).toBeEmptyDOMElement();
  });

  it('splits into a hidden glyph part that keeps the cycle hook and a glyph-free label part', () => {
    const { container } = render(<button><ChatStatus status="working" part="glyph" />Example chat{' '}<ChatStatus status="working" part="label" /></button>);
    const [glyphPart, labelPart] = Array.from(container.querySelectorAll('[data-status="working"]'));
    expect(glyphPart).toHaveAttribute('aria-hidden', 'true');
    expect(glyphPart).toHaveTextContent(/^$/);
    expect(glyphPart.firstElementChild).toHaveClass(styles.cycling);
    expect(labelPart).not.toHaveAttribute('aria-hidden');
    expect(labelPart.querySelector('svg')).toBeNull();
    expect(screen.getByRole('button')).toHaveAccessibleName('Example chat Working');
  });

  it('removes the icon-only cycle hook on every supplied nonworking status, including Stop and idle', () => {
    const { container, rerender } = render(<ChatStatus status="working" />);
    for (const [status, label] of cases.slice(1)) {
      rerender(<ChatStatus status="working" />);
      expect(screen.getByText('Working')).toHaveAttribute('data-status', 'working');
      expect(screen.getByText('Working').firstElementChild).toHaveClass(styles.cycling);
      rerender(<ChatStatus status={status} />);
      expect(screen.queryByText('Working')).not.toBeInTheDocument();
      expect(screen.getByText(label)).toHaveAttribute('data-status', status);
      expect(container.querySelector('[data-status="working"]')).not.toBeInTheDocument();
      expect(container.querySelector(`.${styles.cycling}`)).not.toBeInTheDocument();
    }
    rerender(<ChatStatus status="working" />);
    expect(screen.getByText('Working')).toHaveAttribute('data-status', 'working');
    expect(screen.getByText('Working').firstElementChild).toHaveClass(styles.cycling);
    rerender(<ChatStatus status="idle" />);
    expect(container).toBeEmptyDOMElement();
  });

  it('declares only a 2.4s linear icon rotation with no glow or halo styles', () => {
    // Source contract only; real motion and preference rendering need browser validation.
    expect(css).toMatch(/\.cycling\s*\{ animation: workingCycle 2\.4s linear infinite;/);
    expect(css.match(/animation:/g)).toHaveLength(1);
    expect(css).not.toMatch(/text-shadow|::before|radial-gradient|workingGlow|workingHalo/);
    expect(css).toContain('transform: rotate(360deg)');
  });

  it('schedules no timer and never settles a Working example on elapsed time', () => {
    vi.useFakeTimers();
    try {
      render(<ChatStatus status="working" />);
      expect(vi.getTimerCount()).toBe(0);
      act(() => vi.advanceTimersByTime(60_000));
      expect(screen.getByText('Working')).toHaveAttribute('data-status', 'working');
      expect(screen.queryByText('Completed')).not.toBeInTheDocument();
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });
});
