import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { HelpContext } from './helpContext';
import { LearnMoreLink } from './LearnMoreLink';

describe('contextual help link', () => {
  it('renders nothing when no Help opener is provided', () => {
    const { container } = render(<LearnMoreLink topic="changes">About changes</LearnMoreLink>);
    expect(container).toBeEmptyDOMElement();
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
  });

  it('opens Help at its own topic by pointer and keyboard, and keeps focus for the dialog to return to', async () => {
    const user = userEvent.setup(); const openHelp = vi.fn();
    render(<HelpContext.Provider value={openHelp}><LearnMoreLink topic="permissions">About permissions</LearnMoreLink></HelpContext.Provider>);
    const link = screen.getByRole('button', { name: 'About permissions' });
    expect(link).toHaveAttribute('type', 'button'); expect(link).toHaveAttribute('aria-haspopup', 'dialog');
    expect(link.querySelector('svg')).toHaveAttribute('aria-hidden', 'true');
    await user.click(link);
    expect(openHelp).toHaveBeenCalledExactlyOnceWith('permissions');
    link.focus(); await user.keyboard('{Enter}'); await user.keyboard(' ');
    expect(openHelp).toHaveBeenCalledTimes(3); expect(openHelp).toHaveBeenLastCalledWith('permissions');
    expect(link).toHaveFocus();
  });
});
