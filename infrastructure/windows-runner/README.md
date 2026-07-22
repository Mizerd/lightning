# Dedicated Windows cross-package runner stack

This sanitized stack is deployed only at
`/srv/gitlab-runners/lightning-windows` on `10.195.35.2`. It is a Linux Docker
executor for the local cross-builder image, not a Windows host. The manager is
project-7-only; jobs are non-privileged, limited to the pinned builder image,
and never receive the host Docker socket.

Copy `config.example.toml` to `config/config.toml` on the host, replace the
placeholder there with a modern project-runner authentication token, set the
file to root-only mode 600, and never copy that populated file back into Git.
The runner object must also be locked, protected, tagged only
`windows-cross,windows-package`, and configured with `run_untagged=false`.

The manager's Docker socket mount is root-equivalent and must not be added to
the job `volumes` list. Keep `concurrent=1`, the image allowlist, and the
project CI rule intact. See `docs/windows-runner-operations.md` for status,
rotation, cache maintenance, and rollback commands.
