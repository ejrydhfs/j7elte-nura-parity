# Session archives

Generated with the omp-archive plugin (`export_session_archive`).

| File | Size | Contents |
| --- | --- | --- |
| `omp-archive-full.zip` | 40 MB | Main transcript, 63 subagent sessions, 311 artifact/tool logs, `jobs.json`, `agents.json`, standalone `viewer.html`, manifest |
| `omp-archive-noartifacts.zip` | 27 MB | Main transcript, manifest, `viewer.html`; no artifact logs |

## View

```bash
unzip omp-archive-full.zip -d archive
xdg-open archive/viewer.html
```

Local usernames, home paths, host names, volume UUIDs and the author's email
addresses were redacted. The archive still contains command output, research
context and filenames from the session.
