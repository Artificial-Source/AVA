import { Fragment, useEffect, useState } from 'react';
import { CodeIcon } from '@phosphor-icons/react/dist/csr/Code';
import { FileCodeIcon } from '@phosphor-icons/react/dist/csr/FileCode';
import type { ThemedToken } from 'shiki/types';
import styles from './CodeBlock.module.css';

// Fence language ids are untrusted response text: Map/Set lookups never reach Object.prototype.
const languageNames = new Map([
  ['ts', 'TypeScript'], ['typescript', 'TypeScript'], ['mts', 'TypeScript'], ['cts', 'TypeScript'], ['tsx', 'TSX'],
  ['js', 'JavaScript'], ['javascript', 'JavaScript'], ['mjs', 'JavaScript'], ['cjs', 'JavaScript'], ['jsx', 'JSX'],
  ['json', 'JSON'], ['css', 'CSS'], ['html', 'HTML'], ['md', 'Markdown'], ['markdown', 'Markdown'],
  ['sh', 'Shell'], ['shell', 'Shell'], ['bash', 'Bash'], ['zsh', 'Zsh'], ['console', 'Console'],
  ['py', 'Python'], ['python', 'Python'], ['rs', 'Rust'], ['rust', 'Rust'], ['go', 'Go'], ['sql', 'SQL'],
  ['yaml', 'YAML'], ['yml', 'YAML'], ['toml', 'TOML'], ['diff', 'Diff'], ['text', 'Plain text'], ['txt', 'Plain text'],
]);
// Only the TypeScript grammar ships; it also reads plain JavaScript. Other languages stay plain text.
const highlightedLanguages = new Set(['ts', 'typescript', 'mts', 'cts', 'js', 'javascript', 'mjs', 'cjs']);

export type CodeBlockProps = Readonly<{
  code: string;
  /** Supplied fixture examples carry a filename; response fences do not. */
  filename?: string;
  /** Fence language id; defaults to the filename extension. */
  language?: string;
  partial?: boolean;
}>;

export function CodeBlock({ code, filename, language, partial = false }: CodeBlockProps) {
  const id = (language ?? /\.([a-z0-9]+)$/i.exec(filename ?? '')?.[1] ?? '').trim().toLowerCase();
  const name = languageNames.get(id) ?? (id || 'Code');
  const highlightable = highlightedLanguages.has(id);
  const [highlighted, setHighlighted] = useState<{ code: string; lines: ThemedToken[][] } | null>(null);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    if (partial || !highlightable) return;
    let active = true;
    import('../highlight').then(({ highlightTypeScript }) => (active ? highlightTypeScript(code) : null)).then(
      (tokens) => {
        if (active && tokens) setHighlighted({ code, lines: tokens });
      },
      () => {
        if (active) setFailed(true);
      },
    );
    return () => { active = false; };
  }, [code, partial, highlightable]);

  const lines = !partial && highlighted?.code === code ? highlighted.lines : null;
  return (
    <figure className={styles.example}>
      <figcaption className={styles.caption}>
        {filename ? <FileCodeIcon size={13} aria-hidden="true" /> : <CodeIcon size={13} aria-hidden="true" />}
        <span className={styles.name}>{filename || name}</span>
        {filename ? <span className={styles.language}>{name}</span> : null}
      </figcaption>
      <pre aria-label={filename ? `${filename}, illustrative ${name}` : id ? `${name} code block` : 'Code block'}>
        <code>
          {lines
            ? lines.map((line, lineIndex) => (
                <Fragment key={lineIndex}>
                  {line.map((token, tokenIndex) => (
                    <span key={tokenIndex} style={{ color: token.color }}>{token.content}</span>
                  ))}
                  {lineIndex < lines.length - 1 ? '\n' : ''}
                </Fragment>
              ))
            : code}
        </code>
      </pre>
      {failed ? <p className={styles.fallback}>Syntax highlighting unavailable; showing plain text.</p> : null}
    </figure>
  );
}
