import { useState } from 'react';
import { ToolActivityGroup } from '../chat/widgets/ToolActivityGroup';
import type { ToolRecord } from '../state/types';

export function ToolActivity({ records, open, onOpenChange }: { records: readonly ToolRecord[]; open?: boolean; onOpenChange?: (open: boolean) => void }) {
  const [localOpen, setLocalOpen] = useState(false);
  return <ToolActivityGroup
    records={records}
    open={open ?? localOpen}
    onOpenChange={onOpenChange ?? setLocalOpen}
  />;
}
