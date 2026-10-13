# AVA Development Internals

These current maintainer references explain narrow runtime ownership seams. They are descriptive implementation guides, not public compatibility contracts.

- [Application/TUI source boundary](app-tui-boundary.md): application presentation adapters, backend authority, and temporary frontend DTO dependencies.
- [Run observer](run-observer.md): observer lifecycle, persistence, evaluation, and callback boundaries.
- [Session run controller](session-run-controller.md): ordered session mutation, authority, and active-run ownership.
