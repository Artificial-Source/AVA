# App/TUI source boundary

Application-owned terminal presentation adapters live in `src/ava/app/tui/`.
They retain the `ava::app` namespace to keep this source-layout refactor free of
behavioral and persisted-configuration changes. Backend session, permission,
process, clipboard, external-editor, command-routing, and provider authority
remain in `src/ava/app/` (or their owning backend modules).

The catalog coordinator uses frontend-neutral command and file-reference records
from `app/command_catalog_data.h` in `ApplicationCatalogCache` and
`ApplicationCatalogDelivery`. Its bounded filesystem scan, session-tree authority, generation
accounting, and cache synchronization remain in `app/command_palette.cpp`.
Selector declarations and frontend-facing adapters live under `app/tui/`.
Selector DTO construction and its presentation-only helpers live in
`app/tui/command_palette_views.cpp`. The backend exposes only narrow neutral
helpers shared by catalog navigation and projection, such as session-tree
ordering and label formatting; these helpers do not construct TUI views.
Command and file-reference records are converted to TUI DTOs at that same integration seam.
Default keybindings and canonical action labels are projected into neutral
`CommandHotkey` records by `app/tui/command_hotkeys.*`.
The shared help formatter consumes those records rather than importing TUI keybinding types.

Terminal-text sanitization lives in `app/terminal_text.*`, owned by the
`AVA::app_common` target. Both app and TUI link this small core-dependent target;
the TUI does not link the application orchestration target, avoiding a target cycle.
The source dependency guard rejects direct TUI includes outside `app/tui/`
and restricts TUI access to app headers to the shared frontend and terminal-text contracts.

Canonical command policy and dispatch remain in `app/commands.cpp`.
TUI command family classification, keybinding edits, and display-setting
response builders live in `app/tui/configuration_commands.cpp`.
Generic raw JSON object parsing is shared through `app/settings_json.*`;
the consuming TUI modules retain their own schema validation and duplicate-field policies.
Validated UI configuration itself lives in
`app/tui/tui_display_settings.*`; applying it for `/reload` and projecting the
neutral ordered detail payload lives in `app/tui/display_reload.*`. Generic
reload target normalization, backend-domain orchestration, sanitization, and
report formatting remain in `app/command_reload.cpp`.

`interactive_internal.h` contains only shared submission state/results and the
canonical submission handler. TUI request segmentation, queue presentation,
selectors, snapshots, and runtime wiring are declared in
`app/tui/interactive_tui_internal.h`.
