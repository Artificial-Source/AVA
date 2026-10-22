import { screen, within } from '@testing-library/react';
import type userEvent from '@testing-library/user-event';
import { vi } from 'vitest';
import type { ModelId } from '../state/types';

export async function chooseMainModel(user: ReturnType<typeof userEvent.setup>, model: ModelId) {
  await user.click(screen.getByRole('button', { name: 'Model for this chat' }));
  const dialog = await screen.findByRole('dialog', { name: 'Choose a model' });
  const radio = within(dialog).getAllByRole<HTMLInputElement>('radio').find((input) => input.value === model)!;
  await user.click(radio);
  const label = { aster: 'Aster', 'aster-mini': 'Aster Mini', orion: 'Orion' }[model];
  await user.click(within(dialog).getByRole('button', { name: `Use ${label}` }));
}

// jsdom only. Production uses the browser's native top layer and focus algorithm.
export function installDialogTestSupport() {
  for (const method of ['showModal', 'close'] as const) {
    if (!HTMLDialogElement.prototype[method]) Object.defineProperty(HTMLDialogElement.prototype, method, { configurable: true, writable: true, value() {} });
  }
  vi.spyOn(HTMLDialogElement.prototype, 'showModal').mockImplementation(function (this: HTMLDialogElement) {
    this.setAttribute('open', '');
    this.querySelector<HTMLElement>('input:not(:disabled), button:not(:disabled), select:not(:disabled)')?.focus();
  });
  vi.spyOn(HTMLDialogElement.prototype, 'close').mockImplementation(function (this: HTMLDialogElement) { this.removeAttribute('open'); });
}
