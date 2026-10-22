// Exact public provider IDs, never inferred from a model name or a compatible API.
// Asset provenance and original hashes: public/provider-marks/manifest.json.
export const providerIdentities = {
  openai: { label: 'OpenAI', file: 'openai.svg' },
  anthropic: { label: 'Anthropic', file: 'anthropic.svg' },
  deepseek: { label: 'DeepSeek', file: 'deepseek.svg' },
  gemini: { label: 'Gemini', file: 'gemini.svg' },
  kimi: { label: 'Kimi', file: 'kimi.svg' },
  moonshot: { label: 'Moonshot', file: 'moonshot.svg' },
  openrouter: { label: 'OpenRouter', file: 'openrouter.svg' },
  zai: { label: 'Z.ai', file: 'zai.svg' },
  'zai-coding-cn': { label: 'Z.ai Coding CN', file: 'zai.svg' },
  xai: { label: 'xAI', file: 'xai.svg' },
  groq: { label: 'Groq', file: 'groq.svg' },
  cerebras: { label: 'Cerebras', file: 'cerebras.svg' },
  together: { label: 'Together AI', file: 'together.svg' },
  fireworks: { label: 'Fireworks AI', file: 'fireworks.svg' },
  mistral: { label: 'Mistral', file: 'mistral.svg' },
} as const;

export function providerIdentity(id: string) {
  return Object.hasOwn(providerIdentities, id) ? providerIdentities[id as keyof typeof providerIdentities] : null;
}
