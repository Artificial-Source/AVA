import { fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { ToolActivityGroup } from './ToolActivityGroup';
vi.mock('./ToolActivityDetails', () => { throw new Error('fixture details unavailable'); });
it('contains an optional detail-load failure without losing status or the native row control', async () => {
  const error = vi.spyOn(console, 'error').mockImplementation(() => {});
  try {
    render(<ToolActivityGroup individual records={[{ id: 'failed', action: 'Read', detail: 'example.ts', state: 'failed' }]} open={false} onOpenChange={() => {}} />);
    const row = screen.getByRole('button', { name: 'Read example.ts Failed' }); fireEvent.click(row);
    expect(await screen.findByText('Details couldn’t load. The status above is unchanged.')).toBeVisible(); expect(row).toHaveTextContent('Failed'); expect(row).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(row); expect(row).toHaveAttribute('aria-expanded', 'false'); expect(screen.queryByText('Details couldn’t load. The status above is unchanged.')).not.toBeInTheDocument();
    expect(error).toHaveBeenCalled();
  } finally { error.mockRestore(); }
});
