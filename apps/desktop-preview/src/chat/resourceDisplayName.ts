import type { ToolResource } from './contracts';

// Lexical presentation only. The original path remains available in tool details;
// no resolution, file access or inference about why an image failed is performed.
export function resourceDisplayName(resource: ToolResource): string {
  const name = resource.path.split(/[\\/]/).at(-1) || resource.path;
  if (resource.kind === 'image') {
    const generated = /^(?:pi-clipboard|pasted-image)[-_]([\w-]+)\.(?:png|jpe?g|gif|webp|bmp)$/i.exec(name);
    if (generated && (/[0-9]/.test(generated[1]) || /(?:^|[\\/])(?:tmp|temp|temporaryitems)[\\/]/i.test(resource.path))) return 'Pasted image';
    if (/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\.(?:png|jpe?g|gif|webp|bmp)$/i.test(name)) return 'Attached image';
  }
  return name || (resource.kind === 'image' ? 'Attached image' : 'Attached file');
}
