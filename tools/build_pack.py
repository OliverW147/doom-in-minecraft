"""Zips pack/doomgl into pack/doomgl.zip (the resource pack the server pushes to the client) and prints its SHA-1.
usage: build_pack.py"""
import hashlib, os, sys, zipfile
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pack', 'doomgl')
out = os.path.join(root, '..', 'doomgl.zip')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dp, dn, fn in os.walk(root):
        for f in sorted(fn):
            full = os.path.join(dp, f)
            rel = os.path.relpath(full, root).replace(os.sep, '/')
            if rel in ('layout.json',): continue
            zi = zipfile.ZipInfo(rel, (2026, 1, 1, 0, 0, 0))      # fixed timestamp: the zip is reproducible
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, open(full, 'rb').read())
data = open(out, 'rb').read()
print('size', len(data), 'sha1', hashlib.sha1(data).hexdigest())
