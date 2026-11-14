# Downloading ProS3 firmware and verifying its provenance

There are two ways to get a ProS3 **application image** for the
[Puffin Updater](../ios/PuffinUpdater/README.md):

| Source | Workflow | Lifetime | Access |
| --- | --- | --- | --- |
| CI artifact | `Firmware build` (`.github/workflows/firmware.yml`), every push and pull request | Expires after 30 days | Signed-in GitHub account |
| Release assets | `Firmware release` (`.github/workflows/release.yml`), when a GitHub release is published | Permanent, versioned | Direct public download |

Both contain only the application image, the file PlatformIO writes to
`.pio/build/ProS3/firmware.bin`. They never contain a merged whole-flash image, the
bootloader, or the partition table. Do not install those over Bluetooth, and do not
erase NVS. The board must already run updater-capable firmware installed by USB.

## CI artifact from a pull request or push

1. Open the commit's **Firmware build** run on the repository's Actions tab (or a
   pull request's **Checks** tab → *Firmware build* → *Summary*).
2. Under **Artifacts**, download `puffin-ProS3-<commit SHA>`. GitHub delivers a
   ZIP file. For pull request runs, the SHA is the temporary merge commit that
   GitHub built, not the branch head.
3. Extract it. It contains `firmware.bin` and `firmware.bin.sha256`.
4. Optional, on a computer: `sha256sum --check firmware.bin.sha256` (macOS:
   `shasum -a 256 --check firmware.bin.sha256`) must print `firmware.bin: OK`.
5. Save `firmware.bin` to the iOS Files app (for example, AirDrop it or extract the
   ZIP in Files).
6. In Puffin Updater, tap the **⋯** options button → **Install local firmware…** and
   choose `firmware.bin`. Do not select the `.sha256` file.

CI artifacts are development builds of whatever was pushed or proposed, including
pull requests from forks. They carry no provenance attestation; install them only
when you trust the change that produced them.

## Release assets

Publishing a GitHub release runs the release workflow, which builds the tagged
commit and attaches:

- `puffin-ProS3-<version>.bin`: the application image to install.
- `puffin-ProS3-<version>.sha256`: SHA-256 entries for the image and manifest.
- `puffin-ProS3-manifest.json`: an update manifest in the Puffin Updater's
  `FirmwareRelease` schema (`version`, `notes`, `target`, `url`, `size`, `sha256`)
  plus `file`, `tag`, and `commit`. The app ignores the extra keys.

`<version>` is the tag without a leading `v`. Tags must be 1–40 characters of
`0-9 A-Z a-z . _ -` after that optional `v`, starting with a letter or digit;
otherwise the workflow fails. Manifest `notes` are the first 12 bullet lines
(`- ` or `* `) of the release description, each cut to 300 characters.

Only the `.bin` file is firmware. The checksum and manifest are not installable.
To install a release by hand, download the `.bin` and use **Install local
firmware…** as above.

The manifest has the same name in every release, so
`https://github.com/figitaki/puffin-firmware/releases/latest/download/puffin-ProS3-manifest.json`
always serves the latest non-prerelease release's manifest. That URL is compatible
with the app's `PuffinReleaseManifestURL` setting, but this repository does not
configure it. Configuring it makes GitHub releases the app's trusted publisher
over HTTPS; see the trust model below.

### Verifying a release download

```sh
sha256sum --ignore-missing --check puffin-ProS3-<version>.sha256
gh attestation verify puffin-ProS3-<version>.bin --repo figitaki/puffin-firmware
```

The attestation verification succeeds only for files built by a GitHub Actions
workflow in `figitaki/puffin-firmware`. The output names the workflow file, ref,
and commit that produced it. Add
`--signer-workflow figitaki/puffin-firmware/.github/workflows/release.yml` to
require the release workflow specifically. The checksum and manifest files can be
verified the same way.

### Before publishing a release

GitHub runs the `release.yml` that exists **in the commit the release tag points
at**. While this workflow lives only on a feature branch, releases tagged on
other commits do nothing. Merge it first, then create the tag on a commit that
contains the workflow (usually the merged default branch), and publish the release.

The workflow checks out that exact commit, fails if the tag has moved since
publication, builds `ProS3` with the toolchain pinned in this repository without
a dependency cache, and uploads the assets with the job's token. It never creates,
edits, or publishes a release itself.

Reruns never replace published firmware. Each asset that already exists on the
release must match the rebuilt file byte for byte, and only missing assets are
uploaded. Separate GitHub Actions runs of the same source tree have produced
identical images, so a rerun is normally a no-op that only fills in missing
assets. (Local builds in a different checkout path produce different bytes, so
compare local builds with CI by testing on the board, not by hash.) If anything
differs, the job fails without uploading. To republish on purpose, delete the old
Puffin assets from the release, then rerun the workflow.

Manifest notes are a snapshot of the release description **at publication**.
GitHub reruns reuse the original release event, so editing the description later
changes neither a rerun's manifest nor a republished one. To ship different notes,
publish a new release.

## Trust model

- **SHA-256 is integrity, not authenticity.** It detects corruption, but anyone
  who can replace the image can replace the checksum too.
- **Attestations are provenance, not device-enforced signatures.** The release
  workflow signs a SLSA build-provenance statement for each asset with a
  short-lived GitHub OIDC (Sigstore) certificate, stored by GitHub. They let you
  verify off-device which repository, workflow, and commit built a file.
- **Neither Puffin firmware nor the iOS updater checks attestations.** The board
  validates the transferred image's SHA-256 and ESP image structure, and accepts
  any valid application image. There is no firmware signing, secure boot, or
  rollback protection.
- Provenance covers the workflow and source commit. PlatformIO downloads the
  pinned platform and libraries from their registries during the build, and
  those downloads are not separately attested.

Device-enforced signature checking would be a separate design: signing-key
custody, provisioning trusted keys on boards, rotation and revocation, recovery,
and verification in the updater or bootloader.
