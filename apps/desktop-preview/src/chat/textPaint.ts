// Presentation only: preserve exact stored text, including UTF-16 pairs split by delivery.
export function paintText(text: string): string {
  const bounded = text.slice(0, 20000).split('\n').slice(0, 200).join('\n');
  const last = bounded.charCodeAt(bounded.length - 1);
  return last >= 0xd800 && last <= 0xdbff ? bounded.slice(0, -1) : bounded;
}
