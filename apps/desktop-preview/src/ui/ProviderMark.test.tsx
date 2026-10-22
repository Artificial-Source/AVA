import { createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { render } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { ProviderMark } from './ProviderMark';
import { providerIdentities, providerIdentity } from './providerIdentity';

const hashes = {
  'openai.svg': 'a595df6b423920c67a7f8f73c063e4bfb72d415948097b6cac063a2366bb5186',
  'anthropic.svg': 'e833fdfa7e7187a86a05b870925067556a506dcd4239089aed73e1e58c9366a3',
  'deepseek.svg': '8f9443e351b6dacce71871790201b799d92e8bf96a45ef79aeb5e9bf4db423e2',
  'gemini.svg': '87d5b3c4be75a66f54c1936482a263df68185545b741129badd1b7c2449c18d3',
  'kimi.svg': 'ed08131b46acf70e6e2144eceb775e74098b58ea65fa21eaee04be7b05cfe63b',
  'moonshot.svg': '435f41b74e6a87639b6bf860e9628f20cbe7d6229bece412d548f4cea3081852',
  'openrouter.svg': 'ae671cc83de9bd45db97a47ed5e38a4e1bd9f93a12be495ba4b4f8b284026bca',
  'zai.svg': 'e748cb5108ce37b116d7a5ba97d37e0ae97eadf6849b0de11afb248e244a01e1',
  'xai.svg': '89eb7de9f0d02a41cfecd9109e253d7fd3529e27467dee4254faa67f3ac21451',
  'groq.svg': '078d5cf6e7a54905040c15ddacf5ea33c13042fe7739e820cedc2096e4406651',
  'cerebras.svg': '05af9593eca3fefdb30c5ad042040f008beea2b27e0a6b7315c319492f7a44ff',
  'together.svg': 'b3ec218e7e0b0432a2ce07f5ec98a1dcd24f808c74d5fee624ab31e4947feef3',
  'fireworks.svg': '9991ab2a8331096d3f408e07a5f2e1cb54c369143dd9fc7a97f6458478b33fe3',
  'mistral.svg': 'a06cfa54e7deff7f7544175b006b7f8a03fbc5624c44f7d553a44d07ea96e629',
};
const sha256 = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');

describe('pinned local provider identities', () => {
  it('maps exactly 15 public IDs; Z.ai shares only the asset, xAI is not Grok', () => {
    expect(Object.keys(providerIdentities)).toEqual(['openai', 'anthropic', 'deepseek', 'gemini', 'kimi', 'moonshot', 'openrouter', 'zai', 'zai-coding-cn', 'xai', 'groq', 'cerebras', 'together', 'fireworks', 'mistral']);
    expect(providerIdentity('zai-coding-cn')).not.toBe(providerIdentity('zai'));
    expect(providerIdentity('zai-coding-cn')?.file).toBe(providerIdentity('zai')?.file);
    expect(providerIdentity('xai')?.file).toBe('xai.svg');
    for (const id of Object.keys(providerIdentities)) expect(providerIdentity(id)?.file).toBe(`${id === 'zai-coding-cn' ? 'zai' : id}.svg`);
  });
  it.each(['unknown', 'custom', 'openai-compatible', 'OpenAI', ' openai', 'aster', 'aster-lab', 'orion', 'orion-lab', 'vercel', 'grok', '__proto__', 'constructor', 'toString', 'hasOwnProperty', 'https://example.invalid/icon.svg', '../openai.svg'])('uses only a neutral decorative monogram for %s', (id) => {
    expect(providerIdentity(id)).toBeNull();
    const { container } = render(<ProviderMark id={id} />);
    const mark = container.firstElementChild!;
    expect(mark).toHaveAttribute('data-provider-mark', 'neutral'); expect(mark).toHaveAttribute('aria-hidden', 'true');
    expect(mark).not.toHaveAttribute('style'); expect(container.querySelector('img, a, script, svg')).toBeNull();
    expect(mark.textContent).toMatch(/^[A-Z]$/);
  });
  it('prefers the supplied label for the monogram letter', () => {
    const { container } = render(<ProviderMark id="cedar-example" label="Harbor" />);
    expect(container.firstElementChild).toHaveTextContent(/^H$/);
  });
  it('references every known mark through a local mask, never raw SVG/HTML or arbitrary URLs', () => {
    const { container } = render(<>{Object.keys(providerIdentities).map((id) => <ProviderMark key={id} id={id} />)}</>);
    expect(container.querySelectorAll('[data-provider-mark]')).toHaveLength(15);
    for (const mark of container.children) {
      const identity = providerIdentity(mark.getAttribute('data-provider-mark')!)!;
      expect(mark).toHaveAttribute('aria-hidden', 'true');
      expect(mark.getAttribute('style')).toContain(`/provider-marks/${identity.file}`);
      expect(mark.innerHTML).toBe('');
    }
    const css = readFileSync('src/ui/ProviderMark.module.css', 'utf8');
    expect(css).toContain('background: currentColor');
    expect(css).not.toMatch(/https?:|@import/);
  });
  it('preserves all 14 original assets (14,177 bytes), safe SVG content, per-ID provenance and exact MIT notices', () => {
    const directory = 'public/provider-marks/';
    expect(readdirSync(directory).sort()).toEqual([...Object.keys(hashes), 'LICENSE.upstream.txt', 'manifest.json'].sort());
    const manifest = JSON.parse(readFileSync(`${directory}manifest.json`, 'utf8'));
    expect(manifest).toMatchObject({ source: '@lobehub/icons-static-svg@1.95.1', gitHead: '49a2130df7bfa5eb1b088261bff20a37e2967789', tarballSha256: 'fc2a063f9c5db18f6b3f27a62670c95a9e767cc45ba7bbe5d949f9f9af090fc3', verifiedIntegrity: 'sha512-Hw7EPPgVnC4NZLXBfTNJG6hyQgqECfUPC11VVXodPSr1aebKcFxDZlSpxhWwYNdCc6bhxps/x5TtXoPmfKH2ag==', uniqueSvgFiles: 14, uniqueSvgBytes: 14177 });
    expect(manifest.runtimeProviders.map((entry: { providerId: string }) => entry.providerId)).toEqual(Object.keys(providerIdentities));
    for (const row of manifest.runtimeProviders) {
      expect(providerIdentity(row.providerId)?.file).toBe(row.file);
      expect(row.archivePath).toBe(`package/icons/${row.file}`);
    }
    let bytes = 0;
    for (const [file, hash] of Object.entries(hashes)) {
      const data = readFileSync(`${directory}${file}`); bytes += data.length;
      expect(sha256(data)).toBe(hash);
      const document = new DOMParser().parseFromString(data.toString(), 'image/svg+xml');
      expect(document.documentElement.getAttribute('viewBox')).toBe('0 0 24 24');
      for (const element of document.querySelectorAll('*')) {
        expect(['svg', 'title', 'path']).toContain(element.tagName);
        for (const attribute of element.getAttributeNames()) expect(attribute).not.toMatch(/^on|href/i);
        if (element.hasAttribute('style')) { expect(element.tagName).toBe('svg'); expect(element.getAttribute('style')).toBe('flex:none;line-height:1'); }
      }
      expect(data.toString()).not.toMatch(/<script|<image|<foreignObject|url\(/i);
      for (const entry of manifest.runtimeProviders.filter((entry: { file: string }) => entry.file === file)) expect(entry).toMatchObject({ bytes: data.length, sha256: hash, scope: 'runtime-provider' });
    }
    expect(bytes).toBe(14177);
    const license = readFileSync(`${directory}LICENSE.upstream.txt`);
    expect(sha256(license)).toBe('add9d7531d1b21646317a8958e38fc727506fa39d24bdecb44154d943c82753a');
    expect(manifest.licenseSha256).toBe(sha256(license));
    expect(readFileSync('public/third-party-runtime-licenses.txt', 'utf8')).toContain(license.toString());
    expect(readFileSync('../../THIRD_PARTY_NOTICES.md', 'utf8')).toContain('Browser preview provider marks (not in the Linux C++ artifact)');
    expect(readFileSync('package.json', 'utf8')).not.toMatch(/lobehub|ant-design|@ant-design/);
  });
});
