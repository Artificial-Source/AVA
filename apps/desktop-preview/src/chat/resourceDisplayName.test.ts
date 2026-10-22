import { describe, expect, it } from 'vitest';
import { resourceDisplayName } from './resourceDisplayName';

describe('lexical resource labels', () => {
  it.each([
    ['/tmp/invented/pi-clipboard-11223344-5566-7788-9900-aabbccddeeff.png', 'Pasted image'],
    ['C:\\Temp\\pi-clipboard-invented123.JPG', 'Pasted image'],
    ['/foreign/tmp/pasted-image-invented_123.webp', 'Pasted image'],
    ['/tmp/11223344-5566-7788-9900-aabbccddeeff.png', 'Attached image'],
    ['result-layout.png', 'result-layout.png'],
    ['/project/images/result-layout.png', 'result-layout.png'],
    ['<img onerror=example>.png', '<img onerror=example>.png'],
    ['pi-clipboard-guide.png', 'pi-clipboard-guide.png'],
    ['', 'Attached image'],
  ])('presents %s as %s without resolving a path', (path, label) => {
    const resource = Object.freeze({ kind: 'image' as const, path });
    expect(resourceDisplayName(resource)).toBe(label);
    expect(resource.path).toBe(path);
  });
  it('leaves useful filenames, non-image resources and unrelated text alone', () => {
    expect(resourceDisplayName({ kind: 'file', path: 'src/SearchPage.tsx' })).toBe('SearchPage.tsx');
    expect(resourceDisplayName({ kind: 'file', path: 'pi-clipboard-example.png' })).toBe('pi-clipboard-example.png');
    expect(resourceDisplayName({ kind: 'image', path: 'notes about pi-clipboard.png' })).toBe('notes about pi-clipboard.png');
  });
});
