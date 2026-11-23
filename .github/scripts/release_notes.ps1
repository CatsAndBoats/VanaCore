# Writes release notes for the commits since the last release's tag (or the branch's last 30 before the first), sorted
# under ### New, ### Changed, ### Fixed and ### Misc by their wording. Read along the branch's own line (--first-parent):
# a merged pull request is one line with its number and title, a commit pushed straight to the branch one of its own;
# any other merge is replaced by the commits it brought in. A version bump ("VanaCore 0.9.4") isn't listed.
# -NoHashes leaves the commit hashes out (for CHANGELOG.md). Output is the lines, nothing else.
param([switch]$NoHashes)

$previous = git tag --list 'v*' --sort=-creatordate | Select-Object -First 1
# @( ) around the whole if: a one-item array comes out of it as a bare string, and splatting a string passes its characters
$range = @(if ($previous) { "$previous..HEAD" } else { '-n', '30', 'HEAD' })
$commits = @()
$rs = [char]0x1e
$us = [char]0x01
$bump = '^(- )?VanaCore \d+(\.\d+)+( \(\w+\))?$'
$log = (git log @range --first-parent --pretty="format:%h$us%p$us%s$us%b$rs") -join "`n"
foreach ($entry in $log.Split($rs) | Where-Object { $_.Trim() }) {
  $hash, $parents, $subject, $body = $entry.Trim().Split($us)
  $parents = @($parents -split ' ' | Where-Object { $_ })
  if ($subject -match $bump) { continue }
  $tail = if ($NoHashes) { '' } else { " ($hash)" }
  if ($parents.Count -lt 2) {
    $commits += "- $subject$tail"
  } elseif ($subject -match '^Merge pull request #(\d+) from ') {
    # the pull request's title is the first line of the merge commit's message body
    $title = ($body -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 1)
    if (-not $title) { $title = $subject }
    $commits += if ($NoHashes) { "- $($title.Trim()) (#$($Matches[1]))" } else { "- $($title.Trim()) (#$($Matches[1]), $hash)" }
  } else {
    $fmt = if ($NoHashes) { '- %s' } else { '- %s (%h)' }
    $commits += @(git log "$($parents[0])..$($parents[1])" --no-merges --pretty=format:$fmt |
      Where-Object { $_ -notmatch $bump })
  }
}
$groups = [ordered]@{ New = @(); Changed = @(); Fixed = @(); Misc = @() }
foreach ($line in $commits) {
  $kind = if ($line -match '^- (README|Docs?|CI|Lint|Release|Promote|Tools|Contributing|Third-party|Synced)\b') { 'Misc' }
    elseif ($line -match "(?i)\b(no longer|isn't|aren't|doesn't|don't|wasn't|weren't|again|fix(es|ed)?|crash(es|ed)?)\b") { 'Fixed' }
    elseif ($line -match '(?i)\b(new|can now|added)\b') { 'New' }
    else { 'Changed' }
  $groups[$kind] += $line
}
foreach ($kind in $groups.Keys) {
  if ($groups[$kind].Count) { "### $kind"; $groups[$kind]; '' }
}
if (-not $commits.Count) { 'No changes.' }
