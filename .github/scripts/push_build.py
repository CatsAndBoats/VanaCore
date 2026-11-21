#!/usr/bin/env python3
"""Pushes a build folder to R2 the way CatsEyeXI-Manager's Build Release > PUSH does, for .github/workflows/release.yaml
(a build, to vcdev) and .github/workflows/promote.yaml (vcdev, to vanacore).

    push_build.py vcdev/vanacore --folder <dir>       push the folder as the vcdev build (VanaCore's test build)
    push_build.py vcdev/vanacore --folder <dir> --externals-from vanacore/vanacore
                                                      ... and bring its EXTERNAL files up to vanacore's
    push_build.py vanacore/vanacore --promote-from vcdev/vanacore
                                                      push vcdev's files, as they are on R2, as the vanacore build
    --dry-run                                         list what would be pushed, change nothing
    --deletes                                         also remove files the folder no longer has

A build is its launcher.json entry's tag/name (vcdev/vanacore, vanacore/vanacore), or its name when that's unique.
vcdev is VanaCore's test build: the release workflow pushes every build there, and testers get it by setting the
launcher's custom game build to vcdev (its builds then replace the same-named vanacore one). vanacore is what every
player gets: promote.yaml copies vcdev's files to it, the exact files tested, once a build is ready.

The same steps as push-build.py, in the Manager's order:

  1. reads launcher.json and the build's state (launcher/build_<tag>_<name>.gz) from R2
  2. hashes the folder (SHA1, first 6 hex: Utils.GetFileHash) and diffs it against the state,
     skipping .git paths and DEPLOY_IGNORE
  3. uploads new and changed files to <build.folder>/..., each verified on R2
  4. uploads the new state, then purges its Cloudflare cache entry
  5. updates launcher.json (recent_updates/recent_deletes, build times), uploads it, purges its cache entry
  6. adds a "Build Release" entry to launcher_changelog.json + launcher/changelog/<id>.txt

What differs: credentials come from the environment (the addon updater's secrets) rather than manager.json,
the folder is --folder rather than the build's folder beside the script, and nothing is written locally
(no launcher\\ copies, no manager.json last_build_change_synced). Run it on Windows: state paths are
compared as Windows paths. Standard library only.
"""
import argparse
import concurrent.futures as cf
import datetime
import gzip
import hashlib
import hmac
import http.client
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

EMPTY_SHA = hashlib.sha256(b"").hexdigest()

KEY_LAUNCHER = "launcher/launcher.json"
KEY_CHANGELOG = "launcher/launcher_changelog.json"
KEY_STATE = "launcher/build_{tag}_{name}.gz"
KEY_ENTRIES = "launcher/changelog/{id}.txt"

PUBLIC_ORIGIN = "https://launcher.catseyexi.com"

# What other workflows publish into the vanacore build (substring matches, case-insensitive): plugins\vanacore\
# (CatsEyeXI's plugins, released from their own repositories), addons\ashita\, which the addon updater publishes,
# CatsEyeXI's VanaCore addons, which CatsAndBoats/cexi-addons publishes (its Publish to R2 workflow), and the VanaCore
# native addons the addon updater publishes into addons\vanacore\ ("native": true in addons.json). A push never
# uploads or deletes them; --externals-from copies them from another build on R2 (vcdev takes vanacore's).
EXTERNAL = [
    "plugins\\vanacore\\", "addons\\ashita\\",
    "addons\\vanacore\\partyfinder-vanacore\\", "addons\\vanacore\\auctionhouse-vanacore\\",
    "addons\\vanacore\\clawmark-vanacore\\", "addons\\vanacore\\yunagearopt\\",
]

# manager.json's deploy_ignore on the machine that releases, plus EXTERNAL. Ignored paths are neither uploaded nor
# deleted.
DEPLOY_IGNORE = [
    "TEMP\\", "USER\\", "Vekien\\", "Vekien_", "Premium_", "defaults\\", "default.txt", "scripts\\",
    "config\\", "cache\\shaders", "cache\\flicker.txt", "cache\\flicker_gfx.txt", "cache\\pipelines.d3d12.v1",
    "logs\\vanacore.log", "vanacore.ini",
] + EXTERNAL

# Files a build on R2 has that no package does: never deleted by a push, uploaded when the folder has one (a
# promotion's). DLSS 5's DLL is NVIDIA's and not released yet, so it isn't in the repository or the package; it was
# put on R2 by hand.
KEEP = [
    "dlss\\nvngx_dlssnr.dll",
]

UPLOAD_WORKERS = 8
UPLOAD_RETRIES = 3
VERIFY_RETRIES = 3


def log(msg=""):
    print(msg, flush=True)


def die(msg, code=1):
    log(f"push_build: {msg}")
    sys.exit(code)


# ---- JSON, written the way Newtonsoft's Formatting.Indented does (2 spaces, CRLF, no BOM, no trailing newline)

def dumps(obj):
    return json.dumps(obj, indent=2, ensure_ascii=False).replace("\n", "\r\n")


def loads(raw):
    return json.loads(raw.decode("utf-8-sig"))


# ---- R2 (S3 API, SigV4, path-style; the same PutObject the Manager sends with payload signing off)

class Transient(Exception):
    pass


class R2:
    def __init__(self, cfg):
        self.host = urllib.parse.urlparse(cfg["r2_api"]).netloc
        self.bucket = cfg["r2_bucket"]
        self.access = cfg["r2_access_key"]
        self.secret = cfg["r2_secret_key"]

    def _request(self, method, key, body=None, length=0, content_type=None):
        payload = "UNSIGNED-PAYLOAD" if method == "PUT" else EMPTY_SHA
        amz = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        day = amz[:8]
        path = "/" + self.bucket + "/" + urllib.parse.quote(key, safe="/-_.~")
        headers = {"host": self.host, "x-amz-content-sha256": payload, "x-amz-date": amz}
        signed = ";".join(sorted(headers))
        canonical = "\n".join([
            method, path, "",
            "".join(f"{k}:{headers[k]}\n" for k in sorted(headers)),
            signed, payload,
        ])
        scope = f"{day}/auto/s3/aws4_request"
        to_sign = "\n".join(["AWS4-HMAC-SHA256", amz, scope, hashlib.sha256(canonical.encode()).hexdigest()])
        k = ("AWS4" + self.secret).encode()
        for part in (day, "auto", "s3", "aws4_request"):
            k = hmac.new(k, part.encode(), hashlib.sha256).digest()
        signature = hmac.new(k, to_sign.encode(), hashlib.sha256).hexdigest()
        headers["authorization"] = (
            f"AWS4-HMAC-SHA256 Credential={self.access}/{scope}, SignedHeaders={signed}, Signature={signature}")
        if method == "PUT":
            headers["content-length"] = str(length)
            headers["content-type"] = content_type or "application/octet-stream"

        conn = http.client.HTTPSConnection(self.host, timeout=300)
        conn.blocksize = 1 << 20
        try:
            conn.request(method, path, body=body, headers=headers)
            resp = conn.getresponse()
            return resp.status, resp.read()
        except (OSError, http.client.HTTPException) as e:
            raise Transient(str(e))
        finally:
            conn.close()

    @staticmethod
    def _check(status, data, what):
        if status in (408, 429) or status >= 500:
            raise Transient(f"{what}: HTTP {status}")
        if status >= 300:
            raise RuntimeError(f"{what}: HTTP {status} {data[:300].decode('utf-8', 'replace')}")

    def _retry(self, fn, what):
        for attempt in range(1, UPLOAD_RETRIES + 1):
            try:
                return fn()
            except Transient as e:
                if attempt == UPLOAD_RETRIES:
                    raise RuntimeError(f"{what} failed after {attempt} attempts: {e}")
                time.sleep(0.25 * 2 ** (attempt - 1))

    def get(self, key):
        """The object's bytes, or None when it isn't there."""
        def go():
            status, data = self._request("GET", key)
            if status == 404:
                return None
            self._check(status, data, f"GET {key}")
            return data
        return self._retry(go, f"GET {key}")

    def exists(self, key):
        status, _ = self._request("HEAD", key)
        return status == 200

    def delete(self, key):
        def go():
            status, data = self._request("DELETE", key)
            if status != 404:
                self._check(status, data, f"DELETE {key}")
        self._retry(go, f"DELETE {key}")

    def put(self, key, data=None, path=None):
        """Uploads bytes or a file, then checks it's on R2 (Cloudflare.Upload + VerifyObjectExists)."""
        ctype = mimetypes.guess_type(key)[0]
        if path is not None:
            length = os.path.getsize(path)
        else:
            length = len(data)

        def go():
            if path is not None:
                with open(path, "rb") as f:
                    status, body = self._request("PUT", key, body=f, length=length, content_type=ctype)
            else:
                status, body = self._request("PUT", key, body=data, length=length, content_type=ctype)
            self._check(status, body, f"PUT {key}")
        self._retry(go, f"PUT {key}")

        for attempt in range(1, VERIFY_RETRIES + 1):
            if self.exists(key):
                return
            time.sleep(0.25 * attempt)
        raise RuntimeError(f"upload verification failed for {key}")


# ---- Cloudflare cache (Cloudflare.PurgeCacheForFileIfNeeded: only launcher.json and the build state files)

def should_purge(key):
    k = key.lower()
    return k == KEY_LAUNCHER or (k.startswith("launcher/build_") and k.endswith(".gz"))


def purge(cfg, key):
    """True when purged, or the key doesn't need it."""
    if not should_purge(key):
        return True
    zone = cfg.get("cloudflare_zone_id")
    token = cfg.get("cloudflare_api_token")
    host = (cfg.get("r2_url") or "").replace("https://", "").replace("http://", "").rstrip("/")
    if not (zone and token and host):
        log(f"  ! cache NOT purged for {key}: CLOUDFLARE_ZONE_ID / CLOUDFLARE_API_TOKEN missing")
        return False
    url = f"https://{host}/{key}"
    req = urllib.request.Request(
        f"https://api.cloudflare.com/client/v4/zones/{zone}/purge_cache",
        data=json.dumps({"files": [url]}, separators=(",", ":")).encode(),
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json",
                 "User-Agent": "CatsEyeXI-BuildPush/1.0"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
    except Exception as e:
        log(f"  ! cache NOT purged for {url}: {e}")
        return False
    try:
        ok = json.loads(body).get("success") is True
    except ValueError:
        ok = False
    if not ok:
        log(f"  ! cache NOT purged for {url}: {body[:300]}")
        return False
    log(f"  cache purged: {url}")
    return True


# ---- Local files

def hash_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:6]


def normalise_ignore(ignore):
    return [i.replace("/", "\\").lower() for i in ignore]


def is_ignored(rel, ignore):
    low = rel.lower()
    return any(i in low for i in ignore)


def scan(folder, ignore):
    """{relative path with backslashes: full path}, minus .git paths and DEPLOY_IGNORE matches."""
    found = {}
    for dirpath, _, files in os.walk(folder):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, folder).replace("/", "\\")
            if ".git" in rel.lower() or is_ignored(rel, ignore):
                continue
            found[rel] = full
    return found


def parse_state(raw):
    state = {}
    if raw:
        for line in gzip.decompress(raw).decode("utf-8").split("\n"):
            if line.strip():
                k, _, v = line.partition("=")
                state[k.replace("/", "\\")] = v
    return state


def make_state(state):
    text = "\n".join(f"{k.replace('/', chr(92))}={v}" for k, v in state.items())
    return gzip.compress(text.encode("utf-8"), compresslevel=9, mtime=0)


# ---- launcher.json / changelog bookkeeping

def clean_recent(launcher):
    """Launcher.CleanRecentFileChanges."""
    valid = {f"{b['tag']}_{b['name']}".lower() for b in launcher["builds"]}
    for d in (launcher["recent_updates"], launcher["recent_deletes"]):
        for k in [k for k in d if k.lower() not in valid]:
            del d[k]
        for k in [k for k, v in d.items() if not v]:
            del d[k]
        for k in d:
            d[k] = [f for f in d[k] if "/" not in f]


def add_unique(lst, files):
    seen = {f.lower() for f in lst}
    for f in files:
        if f.lower() not in seen:
            lst.append(f)
            seen.add(f.lower())


def _try(fn, arg):
    """None when fn(arg) ran, else what it raised."""
    try:
        fn(arg)
        return None
    except Exception as e:
        return e


def find_build(launcher, spec):
    """The launcher.json build named spec: its tag/name, or its name when that's unique."""
    want = spec.lower()
    matches = [b for b in launcher["builds"]
               if want in (b["name"].lower(), f"{b['tag']}/{b['name']}".lower())]
    if len(matches) != 1:
        names = ", ".join(f"{b['tag']}/{b['name']}" for b in launcher["builds"])
        die(f"expected one build named '{spec}' in launcher.json, found {len(matches)} (builds: {names})")
    return matches[0]


def fetch(r2, prefix, rel, want):
    """A file of a build on R2 (S3, not the public URL, so never a stale CDN copy), checked against its state's hash."""
    data = r2.get(f"{prefix}/{rel.replace(chr(92), '/')}")
    if data is None:
        raise RuntimeError(f"{rel} is in the state but not on R2")
    got = hashlib.sha1(data).hexdigest()[:6]
    if got.lower() != want.lower():
        raise RuntimeError(f"{rel} on R2 hashes {got}, its state says {want}")
    return data


def config_from_env():
    env = {"r2_api": "R2_S3_ENDPOINT", "r2_access_key": "R2_ACCESS_KEY_ID",
           "r2_secret_key": "R2_SECRET_ACCESS_KEY", "r2_bucket": "R2_BUCKET"}
    cfg = {k: os.environ.get(v, "").strip() for k, v in env.items()}
    missing = [env[k] for k, v in cfg.items() if not v]
    if missing:
        die("missing in the environment: " + ", ".join(missing))
    cfg["r2_url"] = PUBLIC_ORIGIN
    cfg["cloudflare_zone_id"] = os.environ.get("CLOUDFLARE_ZONE_ID", "").strip()
    cfg["cloudflare_api_token"] = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
    return cfg


def main():
    ap = argparse.ArgumentParser(description="Push a build folder to R2 like CatsEyeXI-Manager.")
    ap.add_argument("build", nargs="?", default="vanacore",
                    help="build name in launcher.json, or tag/name when the name isn't unique (default: vanacore)")
    ap.add_argument("--folder", required=True, help="the folder to push, as the build's root")
    ap.add_argument("--dry-run", action="store_true", help="list what would be pushed and change nothing")
    ap.add_argument("--deletes", action="store_true", help="also remove files the folder no longer has")
    ap.add_argument("--externals-from", metavar="BUILD",
                    help="also bring the EXTERNAL files (plugins, the addon updater's addons) up to BUILD's")
    ap.add_argument("--promote-from", metavar="BUILD",
                    help="push BUILD's files as they are on R2 (fetched into --folder, which must be empty or new)")
    args = ap.parse_args()

    cfg = config_from_env()
    r2 = R2(cfg)

    # 1. What R2 holds now
    raw = r2.get(KEY_LAUNCHER)
    if raw is None:
        die("launcher/launcher.json isn't on R2")
    launcher = loads(raw)
    build = find_build(launcher, args.build)
    tag, name = build["tag"], build["name"]
    map_key = f"{tag}_{name}"
    prefix = build["folder"].replace("\\", "/")
    folder = os.path.abspath(args.folder)
    state_key = KEY_STATE.format(tag=tag, name=name)
    ignore = normalise_ignore(DEPLOY_IGNORE)
    keep = normalise_ignore(KEEP)
    external = normalise_ignore(EXTERNAL)

    # A promotion: the source build's files, as R2 has them, into the folder, then pushed as any folder is
    if args.promote_from:
        source = find_build(launcher, args.promote_from)
        if (source["tag"], source["name"]) == (tag, name):
            die("a build can't be promoted to itself")
        if os.path.isdir(folder) and os.listdir(folder):
            die(f"--promote-from fetches into --folder, which must be empty: {folder}")
        source_prefix = source["folder"].replace("\\", "/")
        source_state = parse_state(r2.get(KEY_STATE.format(tag=source["tag"], name=source["name"])))
        wanted = {rel: h for rel, h in source_state.items() if not is_ignored(rel, ignore)}
        if not wanted:
            die(f"{source['tag']}/{source['name']} has no files on R2 to promote")
        # A whole build only: one that was never pushed whole (vcdev before its first release) would take every
        # file it hasn't from the build it's promoted to, with --deletes
        lacking = [f for f in ("VanaCore.exe", "VanaCore.Core.dll") if f.lower() not in {k.lower() for k in wanted}]
        if lacking:
            die(f"{source['tag']}/{source['name']} on R2 has no {', '.join(lacking)}: not a whole build, not promoted")
        log(f"Fetching {len(wanted)} file(s) of {source['tag']}/{source['name']} from R2 into {folder}...")

        def get_one(rel):
            data = fetch(r2, source_prefix, rel, wanted[rel])
            path = os.path.join(folder, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)

        with cf.ThreadPoolExecutor(max_workers=UPLOAD_WORKERS) as pool:
            failed = [f"{rel}: {e}" for rel, e in zip(wanted, pool.map(lambda r: _try(get_one, r), wanted)) if e]
        if failed:
            die("fetching failed, nothing pushed:\n  " + "\n  ".join(failed))

    if not os.path.isdir(folder):
        die(f"build folder not found: {folder}")

    remote = parse_state(r2.get(state_key))
    remote_lower = {k.lower(): k for k in remote}

    # The EXTERNAL files another build has that this one hasn't (or has otherwise), and those it has that the other
    # doesn't: copied or deleted with the push
    ext_copy, ext_delete, ext_source = [], [], None
    if args.externals_from:
        source = find_build(launcher, args.externals_from)
        if (source["tag"], source["name"]) == (tag, name):
            die("a build can't take its external files from itself")
        ext_source = (source["folder"].replace("\\", "/"),
                      {k: v for k, v in parse_state(r2.get(KEY_STATE.format(tag=source["tag"], name=source["name"]))).items()
                       if is_ignored(k, external)})
        theirs_lower = {k.lower(): k for k in ext_source[1]}
        ext_copy = sorted((k for k, v in ext_source[1].items()
                           if k.lower() not in remote_lower or remote[remote_lower[k.lower()]].lower() != v.lower()),
                          key=str.lower)
        ext_delete = sorted((k for k in remote if is_ignored(k, external) and k.lower() not in theirs_lower), key=str.lower)

    # 2. Local files against the state
    log(f"Build {tag} - {name}: {folder} -> {prefix}/")
    local = scan(folder, ignore)
    if not local:
        die(f"{folder} holds no files to push")
    with cf.ThreadPoolExecutor(max_workers=16) as pool:
        hashes = dict(zip(local, pool.map(hash_file, local.values())))

    new = sorted((r for r in local if r.lower() not in remote_lower), key=str.lower)
    changed = sorted((r for r in local if r.lower() in remote_lower
                      and remote[remote_lower[r.lower()]].lower() != hashes[r].lower()), key=str.lower)
    deleted = sorted((k for k in remote if not os.path.exists(os.path.join(folder, k))
                      and not is_ignored(k, ignore) and not is_ignored(k, keep)), key=str.lower)
    push_deletes = deleted if args.deletes else []

    log(f"{len(local)} local files, {len(remote)} in the remote state: "
        f"{len(new)} new, {len(changed)} changed, {len(deleted)} deleted"
        + ("" if args.deletes or not deleted else " (deletes skipped: --deletes to remove them)"))
    for label, files in (("new", new), ("changed", changed), ("deleted", deleted)):
        for f in files:
            log(f"  {label:8} {f}")
    if args.externals_from:
        log(f"External files from {args.externals_from}: {len(ext_copy)} to copy, {len(ext_delete)} to delete")
        for label, files in (("copy", ext_copy), ("delete", ext_delete)):
            for f in files:
                log(f"  {label:8} {f}")

    if not (new or changed or push_deletes or ext_copy or ext_delete):
        log("Nothing to push.")
        return 0
    if args.dry_run:
        log("Dry run: nothing changed.")
        return 0

    unsafe = [f for f in new + changed if "#" in f or "?" in f]
    if unsafe:
        die("file names with # or ? can't be served from R2:\n  " + "\n  ".join(unsafe))

    # 3. Upload (hashed again first, like the Manager, in case a file changed since the scan)
    upload = new + changed
    for f in upload:
        hashes[f] = hash_file(local[f])
    failures = []
    done = 0

    def up(rel):
        r2.put(f"{prefix}/{rel.replace(chr(92), '/')}", path=local[rel])
        return rel

    if upload:
        log(f"Uploading {len(upload)} file(s)...")
        with cf.ThreadPoolExecutor(max_workers=UPLOAD_WORKERS) as pool:
            futures = {pool.submit(up, rel): rel for rel in upload}
            for fut in cf.as_completed(futures):
                try:
                    fut.result()
                    done += 1
                    log(f"  [{done}/{len(upload)}] {futures[fut]}")
                except Exception as e:
                    failures.append(f"{futures[fut]}: {e}")
        if failures:
            die("uploads failed, state and launcher.json left as they were:\n  " + "\n  ".join(failures))

    if push_deletes:
        log(f"Deleting {len(push_deletes)} file(s) from R2...")
        for rel in push_deletes:
            r2.delete(f"{prefix}/{rel.replace(chr(92), '/')}")

    if ext_copy:
        log(f"Copying {len(ext_copy)} external file(s) from {args.externals_from}...")

        def copy_one(rel):
            data = fetch(r2, ext_source[0], rel, ext_source[1][rel])
            r2.put(f"{prefix}/{rel.replace(chr(92), '/')}", data=data)

        with cf.ThreadPoolExecutor(max_workers=UPLOAD_WORKERS) as pool:
            failed = [f"{rel}: {e}" for rel, e in zip(ext_copy, pool.map(lambda r: _try(copy_one, r), ext_copy)) if e]
        if failed:
            die("copying the external files failed, state and launcher.json left as they were:\n  " + "\n  ".join(failed))
    if ext_delete:
        log(f"Deleting {len(ext_delete)} external file(s) {args.externals_from} no longer has...")
        for rel in ext_delete:
            r2.delete(f"{prefix}/{rel.replace(chr(92), '/')}")

    # 4. The build's state
    for rel in upload:
        remote[remote_lower.get(rel.lower(), rel)] = hashes[rel]
    for rel in ext_copy:
        remote[remote_lower.get(rel.lower(), rel)] = ext_source[1][rel]
    for rel in push_deletes + ext_delete:
        remote.pop(rel, None)
    upload = upload + ext_copy
    push_deletes = push_deletes + ext_delete
    log("Uploading build state...")
    r2.put(state_key, data=make_state(remote))
    purged = purge(cfg, state_key)

    # 5. launcher.json, read again so what other edits landed meanwhile aren't lost
    now = int(time.time())
    launcher = loads(r2.get(KEY_LAUNCHER))
    add_unique(launcher["recent_updates"].setdefault(map_key, []), upload)
    if push_deletes:
        add_unique(launcher["recent_deletes"].setdefault(map_key, []), push_deletes)
    for b in launcher["builds"]:        # Launcher.Save stamps every build
        b["updated"] = float(now)
    clean_recent(launcher)
    log("Uploading launcher.json...")
    r2.put(KEY_LAUNCHER, data=dumps(launcher).encode("utf-8"))
    purged = purge(cfg, KEY_LAUNCHER) and purged

    # 6. Change log
    raw = r2.get(KEY_CHANGELOG)
    changelog = loads(raw) if raw else {"build_changes": []}
    change_id = hashlib.sha1(
        f"{name}{tag}{now}Build Release{len(upload)}{len(push_deletes)}".encode("utf-8")).hexdigest()
    changelog["build_changes"].append({
        "id": change_id, "name": name, "tag": tag, "updated": float(now),
        "type": "Build Release", "uploads": len(upload), "deletions": len(push_deletes)})
    changelog["build_changes"].sort(key=lambda c: c["updated"], reverse=True)
    r2.put(KEY_CHANGELOG, data=dumps(changelog).encode("utf-8"))

    entries = [{"filename": f, "action": "uploaded"} for f in upload] + \
              [{"filename": f, "action": "deleted"} for f in push_deletes]
    r2.put(KEY_ENTRIES.format(id=change_id), data=dumps(entries).encode("utf-8"))

    log(f"Pushed {tag} - {name}: {len(upload)} uploaded ({len(new)} new, {len(changed)} changed, "
        f"{len(ext_copy)} external), {len(push_deletes)} deleted.")
    if not purged:
        log("! Cloudflare cache was NOT fully purged (see above): players may see the old launcher.json / "
            "build state until it expires. Purge it from the Manager (Cloudflare tab) or re-run.")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
