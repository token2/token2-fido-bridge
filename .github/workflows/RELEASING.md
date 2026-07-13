# Releasing

## Quick release (manual changelog)
```sh
sudo apt install devscripts        # provides `dch`
dch -v 0.2.0-1 "Describe the changes"
git commit -am "Release 0.2.0"
git tag v0.2.0
git push origin main v0.2.0
```
The tag push triggers .github/workflows/release.yml, which builds the .deb
and attaches it to a GitHub Release.

## Fully automatic version-from-tag (optional)
To skip manual `dch`, add this step in release.yml BEFORE "Build .deb",
so the changelog version is derived from the git tag:

```yaml
      - name: Sync changelog version to tag
        if: startsWith(github.ref, 'refs/tags/v')
        run: |
          VER="${GITHUB_REF_NAME#v}"        # v0.2.0 -> 0.2.0
          sudo apt-get install -y devscripts
          dch --newversion "${VER}-1" --distribution unstable \
              "Automated release ${VER}" || \
          dch --create --package token2-fido-bridge \
              --newversion "${VER}-1" --distribution unstable \
              "Automated release ${VER}"
```

## Required one-time repo setting
Settings -> Actions -> General -> Workflow permissions ->
"Read and write permissions". Without this the release upload fails.

## Adding arm64 builds
Cross-building .debs for arm64 needs either QEMU emulation or a native
arm64 runner. Simplest path: add a job using `uraimo/run-on-arch-action`
or build in an arm64 container. For most smartcard desktop users amd64 is
sufficient to start.
