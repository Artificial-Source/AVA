import { readFileSync } from 'node:fs';
import { describe, expect, expectTypeOf, it, vi } from 'vitest';
import type { ToolResource } from './contracts';
import { isToolImagePreviewId, toolImagePreview } from './toolImages';

describe('closed supplied tool image descriptor', () => {
  it('maps only the explicit opaque ID to a stable, immutable local raster descriptor', () => {
    const image = toolImagePreview('search-results-example');
    expect(isToolImagePreviewId('search-results-example')).toBe(true);
    expect(image).toEqual({ src: '/tool-previews/search-results.png', alt: 'Search results page with a search field, a Recent filter and three result rows.', width: 640, height: 400 });
    expect(Object.isFrozen(image)).toBe(true); expect(toolImagePreview('search-results-example')).toBe(image);
    expectTypeOf<{ kind: 'image'; path: string; previewId: 'search-results-example' }>().toExtend<ToolResource>();
    expectTypeOf<{ kind: 'file'; path: string; previewId: 'search-results-example' }>().not.toExtend<ToolResource>();
    expectTypeOf<{ kind: 'image'; path: string; previewId: 'unknown' }>().not.toExtend<ToolResource>();
  });
  it.each([
    undefined, null, '', 'unknown', 'SEARCH-RESULTS-EXAMPLE', ' search-results-example', 'search-results-example ', '__proto__', 'constructor', 'prototype', 'toString', 'hasOwnProperty',
    '/tool-previews/search-results.png', '/tmp/example/pi-clipboard-12345678.png', '../search-results-example', 'https://example.invalid/image.png', '//example.invalid/image.png', 'file:///tmp/image.png', 'blob:https://example.invalid/id', 'data:image/png;base64,abc', 'data:image/svg+xml,<svg/>', 'search-results.svg', 'search-results.html', '<img src=x>', '<svg/>',
    1, false, [], ['search-results-example'], {}, { previewId: 'search-results-example' }, new String('search-results-example'),
  ])('rejects non-ID input without deriving an image: %j', (value) => {
    expect(isToolImagePreviewId(value)).toBe(false); expect(toolImagePreview(value)).toBeNull();
  });
  it('never coerces or looks up properties on exotic values', () => {
    const read = vi.fn(() => { throw new Error('Must not inspect input'); });
    const proxy = Proxy.revocable({}, {}); proxy.revoke();
    for (const value of [Object.create(null), Object.create({ previewId: 'search-results-example' }), { get src() { return read(); }, toString: read, [Symbol.toPrimitive]: read }, proxy.proxy, Symbol('search-results-example')]) expect(toolImagePreview(value)).toBeNull();
    expect(read).not.toHaveBeenCalled();
  });
  it('ships only a bounded metadata-free RGB PNG matching the declared natural size', () => {
    const png = readFileSync('public/tool-previews/search-results.png');
    expect(png.length).toBeLessThanOrEqual(100000); expect(png.subarray(0, 8).toString('hex')).toBe('89504e470d0a1a0a');
    expect(png.readUInt32BE(16)).toBe(640); expect(png.readUInt32BE(20)).toBe(400);
    expect(png[24]).toBe(8); expect(png[25]).toBe(2); // 8-bit RGB, no alpha or palette.
    const chunks = []; let offset = 8;
    while (offset < png.length) { chunks.push(png.toString('ascii', offset + 4, offset + 8)); offset += png.readUInt32BE(offset) + 12; }
    expect(offset).toBe(png.length); expect(chunks[0]).toBe('IHDR'); expect(chunks.at(-1)).toBe('IEND'); expect([...new Set(chunks)]).toEqual(['IHDR', 'IDAT', 'IEND']);
  });
});
