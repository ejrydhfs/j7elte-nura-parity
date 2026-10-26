# Session archives

Generated with the omp-archive plugin (`export_session_archive`).

| File | Size | Contents |
| --- | --- | --- |
| `omp-archive-full.zip` | 40 MB | Main transcript, 63 subagent sessions, 311 artifact/tool logs, `jobs.json`, `agents.json`, standalone `viewer.html`, manifest |
| `omp-archive-noartifacts.zip` | 27 MB | Main transcript, manifest, `viewer.html`; no artifact logs |

Both are also attached to the `session-archive-2026-10-25` GitHub release.

## View

```bash
unzip omp-archive-full.zip -d archive
xdg-open archive/viewer.html
```

The archive is unredacted. It includes local paths, command output, research
context and personal filenames from the session.
