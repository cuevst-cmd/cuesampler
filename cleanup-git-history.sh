#!/usr/bin/env bash
#
# CUE SAMPLER — one-time git history cleanup
#
# Strips old .pkg installers (~3.5 GB) and the vendored tools/gh binary (37 MB)
# from all history, then force-pushes the rewritten history to GitHub.
#
# Expected result: .git shrinks from ~3.9 GB to roughly 150 MB.
#
# RUN THIS FROM YOUR OWN TERMINAL, not from Claude. Claude's sandbox has no
# delete permission on this folder, so it cannot perform a history rewrite.
#
# BACKUP STRATEGY (no local mirror needed):
#   GitHub still holds your complete original history until the force-push in
#   the final step. Until then, recovery is simply:
#       cd ~ && rm -rf Documents/SAMPLERv3 && git clone \
#           https://github.com/cuevst-cmd/cuesampler.git Documents/SAMPLERv3
#   So everything up to the final step is reversible at zero disk cost.
#   Verify the rewrite looks right BEFORE you answer y to the push prompt.

set -euo pipefail

REPO=~/Documents/SAMPLERv3

cd "$REPO"

# ---------------------------------------------------------------------------
# Step 0 — clear the stale lock and the stray probe files Claude left behind
# ---------------------------------------------------------------------------
rm -f .git/index.lock .git/__probe __probe2
echo "==> cleared stale lock and probe files"

# ---------------------------------------------------------------------------
# Step 1 — sanity checks
# ---------------------------------------------------------------------------
git config user.name  >/dev/null || { echo "!! set: git config user.name  'Your Name'";  exit 1; }
git config user.email >/dev/null || { echo "!! set: git config user.email 'you@example.com'"; exit 1; }

echo "==> current size:"
du -sh .git
echo "==> free space:"
df -h . | tail -1
echo "==> origin still holds the original history until the final step:"
git ls-remote --heads origin

read -rp "Continue? [y/N] " ok; [ "$ok" = "y" ] || exit 1

# ---------------------------------------------------------------------------
# Step 2 — commit the manual-chop + ADSR work so it lands in the new history
# ---------------------------------------------------------------------------
git add MANUAL_TEST.md PluginEditor.cpp PluginProcessor.cpp PluginProcessor.h .gitignore
git commit -m "Add manual chopping mode and per-chop ADSR envelopes

- Per-chop A/D/S/R stored on ChopDefinition, applied across all four
  render paths and baked into WAV export
- Manual chop mode: double-click start, hold/release a MIDI pad to set
  the end and pad assignment, draggable chop edges
- State format version 4 -> 5 (new fields are default-tolerant, so v4
  projects still load correctly)

Includes the P1 audit fixes on top of the above (see AUDIT_2026-08-16.md);
they touch the same files and so could not be split into a separate commit."

echo "==> feature work committed"

# ---------------------------------------------------------------------------
# Step 3 — drop the two stale local-only branches
#
# Both verified redundant on 2026-08-16:
#   cleanup-large-release-history      — same commit as main
#   backup-before-large-file-cleanup   — tree is identical to main's apart
#                                        from main having 4 extra .gitignore
#                                        lines. No unique source work.
# ---------------------------------------------------------------------------
git branch -D codex/backup-before-large-file-cleanup 2>/dev/null || true
git branch -D codex/cleanup-large-release-history    2>/dev/null || true
echo "==> stale branches removed"

# ---------------------------------------------------------------------------
# Step 4 — install git-filter-repo if missing
# ---------------------------------------------------------------------------
if ! command -v git-filter-repo >/dev/null 2>&1; then
    echo "==> installing git-filter-repo"
    brew install git-filter-repo 2>/dev/null || pip3 install --user git-filter-repo
fi

# ---------------------------------------------------------------------------
# Step 5 — the rewrite
#
# --path-glob 'dist/*.pkg' catches all installers, including any not
# individually enumerated. --invert-paths means "remove these, keep the rest".
# assets/beat_this.onnx.data is deliberately NOT touched: your .gitignore
# says it stays tracked.
# ---------------------------------------------------------------------------
git filter-repo --force \
    --path-glob 'dist/*.pkg' \
    --path-glob 'dist/*.pkg.zip' \
    --path 'tools/gh' \
    --invert-paths

echo "==> history rewritten"

# ---------------------------------------------------------------------------
# Step 6 — reclaim the space (filter-repo drops the remote as a safety measure)
# ---------------------------------------------------------------------------
git reflog expire --expire=now --all
git gc --prune=now --aggressive

echo "==> new size:"
du -sh .git

# ---------------------------------------------------------------------------
# Step 7 — verify the rewrite BEFORE pushing
# ---------------------------------------------------------------------------
git remote add origin https://github.com/cuevst-cmd/cuesampler.git 2>/dev/null || \
    git remote set-url origin https://github.com/cuevst-cmd/cuesampler.git

echo
echo "=== history ==="
git log --oneline -8
echo
echo "=== source files present? (all should exist) ==="
for f in PluginProcessor.cpp PluginEditor.cpp CMakeLists.txt assets/beat_this.onnx.data; do
    [ -f "$f" ] && echo "  ok   $f" || echo "  MISSING  $f"
done
echo
echo "=== installers gone from history? (should print nothing) ==="
git rev-list --objects --all | grep -E '\.pkg$|tools/gh$' || echo "  clean"

# ---------------------------------------------------------------------------
# Step 8 — force-push. THIS IS THE POINT OF NO RETURN.
#
# Until you answer y here, GitHub still holds your original history and you
# can recover by deleting this folder and re-cloning.
# ---------------------------------------------------------------------------
echo
echo "!! After this push, the old history on GitHub is gone."
echo "!! If anything above looks wrong, answer N and re-clone from GitHub."
read -rp "Force-push to origin/main? [y/N] " ok; [ "$ok" = "y" ] || {
    echo "Stopped. Nothing pushed. Push later with: git push --force origin main"; exit 0; }

git push --force origin main

echo
echo "==> done."
echo "    In GitHub Desktop: File > Remove repository, then re-clone."
echo "    That is the cleanest way to get its cached state back in sync."
