import { Fragment, useEffect, useState } from 'react';
import { FileCodeIcon } from '@phosphor-icons/react/dist/csr/FileCode';
import type { ThemedToken } from 'shiki/types';
import type { CodeExample } from '../state/types';
import styles from './CodeBlock.module.css';

export function CodeBlock({ filename, code, partial = false }: CodeExample & { partial?: boolean }) {
  const [highlighted, setHighlighted] = useState<{ code: string; lines: ThemedToken[][] } | null>(null);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    let active = true;
    if (partial) return;
    import('../highlight').then(({ highlightTypeScript }) => highlightTypeScript(code)).then(
      (lines) => {
        if (active) setHighlighted({ code, lines });
      },
      () => {
        if (active) setFailed(true);
      },
    );
    return () => { active = false; };
  }, [code, partial]);

  return (
    <figure className={styles.example}>
      <figcaption className={styles.caption}>
        <FileCodeIcon size={13} aria-hidden="true" />
        <span>{filename}</span>
        <span className={styles.language}>TypeScript</span>
      </figcaption>
      <pre aria-label={`${filename}, illustrative TypeScript`}>
        <code>
          {!partial && highlighted?.code === code
            ? highlighted.lines.map((line, lineIndex) => (
                <Fragment key={lineIndex}>
                  {line.map((token, tokenIndex) => (
                    <span key={tokenIndex} style={{ color: token.color }}>{token.content}</span>
                  ))}
                  {lineIndex < highlighted.lines.length - 1 ? '\n' : ''}
                </Fragment>
              ))
            : code}
        </code>
      </pre>
      {failed ? <p className={styles.fallback}>Syntax highlighting unavailable; showing plain text.</p> : null}
    </figure>
  );
}
