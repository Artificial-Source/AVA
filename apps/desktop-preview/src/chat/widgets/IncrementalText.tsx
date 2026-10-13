import { memo, useState } from 'react';

// Appending supplied text must not replace already selectable text nodes.
export const IncrementalText = memo(function IncrementalText({ text }: { text: string }) {
  const [snapshot, setSnapshot] = useState({ text, parts: [text] });
  if (snapshot.text !== text) setSnapshot({ text, parts: text.startsWith(snapshot.text) ? [...snapshot.parts, text.slice(snapshot.text.length)] : [text] });
  return <>{snapshot.parts.map((part, index) => <span key={index}>{part}</span>)}</>;
});
