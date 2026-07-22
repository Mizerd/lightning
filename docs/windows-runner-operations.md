# Lightning Windows cross-package runner operations

The dedicated runner is a containerized Linux/MinGW cross-package runner on
`10.195.35.2`. It is neither a native Windows runner nor installed on the
runner-only VM (`10.195.35.6`). The deployed stack is independent of GitLab and
the existing Linux runners.

## Inventory and security model

| Item | Value |
| --- | --- |
| Host | `10.195.35.2` |
| Stack path | `/srv/gitlab-runners/lightning-windows` |
| Container | `lightning-windows-cross-runner` |
| Runner | `lightning-windows-cross-main-gitlab` |
| Runner image | `gitlab/gitlab-runner:v19.2.0` |
| Executor | Docker, non-privileged jobs |
| Tags | `windows-cross`, `windows-package` |
| Scope | project 7 only, locked, protected, tagged jobs only |
| Concurrency | 1 job; 2 polling requests |
| Job limits | 4 CPU, 8 GiB memory (10 GiB including swap), 2-hour maximum |
| Builder | `lightning-windows-builder:fedora44-qt6.11.1-rust1.95.0-v1` |

The runner manager mounts `/var/run/docker.sock`, which is root-equivalent host
access. It is constrained by project scope, protected-ref access, unique tags,
`run_untagged=false`, the restrictive CI rule, and concurrency one. Job
containers do **not** receive the socket, host paths, GitLab volumes, SSH keys,
or secrets. Do not make the tags available to another project and do not relax
the rule for merge requests or arbitrary branches.

Project source fetches use normal certificate verification against
`https://gitlab.smetonis.net`. No private CA is needed in the current topology;
never set `GIT_SSL_NO_VERIFY`, use `curl -k`, or disable TLS checks. Project 7's
short-lived `CI_JOB_TOKEN` and the existing project-6 inbound allowlist are the
only source credentials. The runner authentication token is stored only in the
root-owned `config/config.toml` on the host.

The public hostname is behind a proxy with an approximately 100 MiB request
limit, while the combined MSI, setup EXE, portable ZIP, deployment tree, and
reports form a larger artifact archive. The manager therefore polls the
coordinator and uploads job artifacts through GitLab's established
host-internal `http://10.195.35.2` endpoint. This traffic remains on the trusted
GitLab host network; job containers are not attached to GitLab's private Docker
network. Source clones remain HTTPS, and no certificate-verification bypass is
set. Treat access to that internal network as a credential-bearing trust
boundary.

## Routine commands

Run on `10.195.35.2` from the stack directory:

```bash
sudo docker compose ps
sudo docker compose logs --tail=100 runner
sudo docker compose stop
sudo docker compose start
sudo docker compose restart runner
sudo docker exec lightning-windows-cross-runner gitlab-runner verify
```

Rebuild the builder from a reviewed project-7 default-branch checkout, then
record the resulting image ID and size:

```bash
sudo docker build \
  --label net.smetonis.lightning.task=windows-packaging \
  -t lightning-windows-builder:fedora44-qt6.11.1-rust1.95.0-v1 \
  -f packaging/windows/Dockerfile .
sudo docker image inspect \
  lightning-windows-builder:fedora44-qt6.11.1-rust1.95.0-v1
```

Do not use a floating builder image. The official Qt multimedia source checksum
is verified during the image build. `docker system df` and `df -h /` are the
safe first disk checks. The runner cache is the dedicated stack `cache/`
directory; inspect it with `sudo du -sh cache`. Stop the runner before removing
only that directory's contents, and do so only when a cold rebuild is intended.
Never use a broad Docker prune on the GitLab VM.

Trigger the test using the variables documented in the README. Artifacts are
under the successful `windows-package-test` job and expire after seven days.
The expected build is CPU-heavy for roughly ten minutes on a cold Rust cache;
the measured Rust LTO peak approached 5.8 GiB, so the job has an 8 GiB cap
while concurrency stays one. A missing local builder image,
an unprotected ref, a non-SHA source, or any publication/release input causes
the job to be absent or fail closed.

An HTTP 413 during artifact upload means the runner is using the public proxy
instead of the internal coordinator endpoint. Check only the non-secret `url`
field in root-owned `config/config.toml`, correct it to `http://10.195.35.2`,
then restart and verify this dedicated runner. Do not print the token or weaken
project source TLS.

## Token rotation and unregister

Pause the runner in GitLab before token work. Create a replacement project-
scoped runner authentication token through GitLab's current runner-creation
workflow, update only the root-owned `config/config.toml`, set it to mode 600,
restart the dedicated container, and run `gitlab-runner verify`. Never put the
token in Compose, a command log, project Git, an image, or documentation.

To unregister permanently, pause it, remove only its project-7 assignment in
GitLab, stop the dedicated Compose stack, and remove only the dedicated
container after confirming no job is active. Preserve the config briefly for
investigation; remove the dedicated cache/config and builder image only after
explicit confirmation.

## Rollback

1. Pause `lightning-windows-cross-main-gitlab` in GitLab.
2. Confirm no `windows-package-test` job is running.
3. Stop `/srv/gitlab-runners/lightning-windows/compose.yml`.
4. Revert the project-7 Windows commits with new revert commits and push
   normally; do not rewrite history.
5. Remove only `lightning-windows-cross-runner`, its project assignment, and
   task-specific image/cache if removal is intended.
6. Leave GitLab, the Linux runner stacks, and `10.195.35.6` untouched.

For MSI failures inspect `reports/msi-*.idt` and `wixl.log`; for DLL/QML
failures inspect `runtime-dependencies.json`, `pe-imports.txt`, and the required
plugin checks; for Wine-only failures inspect `wine-*.log` and remember that a
Wine pass or failure is not native Windows acceptance.
