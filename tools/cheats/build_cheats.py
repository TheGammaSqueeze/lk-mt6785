#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 TheGammaSqueeze
"""Build the card's cheat packs (gammaos/cheats/<system>.gcd) from libretro-database.

    build_cheats.py <card dir> [<cache dir>]

libretro-database (github.com/libretro/libretro-database, CC BY-SA 4.0) keeps RetroArch's cheat
files (cht/<system>/<game> (<format>).cht) and the No-Intro lists of every game's dumps
(metadat/no-intro/<system>.dat). It is fetched at the commit pinned below, only those folders, into
the cache dir, and each system's cheats are packed into one file the LK reads with a game running:

  header   "GCD1", version 2, then the count and place of the four tables below (u32 each)
  games    one record per set of cheats: where its text is, its packed and its plain size
  crcs     (CRC-32 of a dump, record), sorted: a dump the No-Intro list gives for the game
  names    (FNV-1a of a name key, record), sorted: the game's names, for a dump no list has
  titles   (place of a name, record), in alphabetical order: each record's name, to choose one by
           hand when a game is not found (the names after the table, each ended by a 0)

A record is its text, raw deflate: "K<name key>" lines (the names it is filed under, so a hash
that two games share is told apart), then for each cheat file a "#<format>" line (Code Breaker,
Game Genie, ... or nothing) and its cheats, "C<description>\t<code>" a line. Cheats with nothing to
send to a core are left out: RetroArch's own memory cheats (no code), and codes with a value still
to fill (XX, ??), which the LK has no way to ask for.

A cheat file goes with the No-Intro games of its title that share a region with it, those with
the most of its tags (the lists add language tags and split regions over the years, the cheat
files keep the older names): a code is only right for the dump it was made for, so a file with no
game of its region stays filed under its own name only.
"""
import os
import re
import struct
import subprocess
import sys
import zlib

REPO = "https://github.com/libretro/libretro-database.git"
COMMIT = "90601bff67dba7057381292f17e08ed2cecbd6dc"   # 2026-11-20

# pack name: (cheat folders, No-Intro lists). The Game Boy packs take both folders: a Color game
# that also runs on the first Game Boy can be filed in either, and the LK looks in its own pack.
SYSTEMS = {
	"gb":   (["Nintendo - Game Boy"], ["Nintendo - Game Boy"]),
	"gbc":  (["Nintendo - Game Boy Color"], ["Nintendo - Game Boy Color"]),
	"gba":  (["Nintendo - Game Boy Advance"], ["Nintendo - Game Boy Advance"]),
	"nes":  (["Nintendo - Nintendo Entertainment System"], ["Nintendo - Nintendo Entertainment System"]),
	"snes": (["Nintendo - Super Nintendo Entertainment System"], ["Nintendo - Super Nintendo Entertainment System"]),
	"md":   (["Sega - Mega Drive - Genesis"], ["Sega - Mega Drive - Genesis"]),
	"sms":  (["Sega - Master System - Mark III"], ["Sega - Master System - Mark III"]),
	"gg":   (["Sega - Game Gear"], ["Sega - Game Gear"]),
}
# the last tag of a cheat file's name when it names the kind of code rather than the game
FORMATS = {"code breaker": "Code Breaker", "codebreaker": "Code Breaker", "gameshark": "GameShark",
	   "game shark": "GameShark", "game genie": "Game Genie", "gamegenie": "Game Genie",
	   "xploder": "Xploder", "action replay": "Action Replay", "pro action replay": "Pro Action Replay",
	   "rumbles": "Rumbles", "par": "Pro Action Replay", "goldfinger": "Goldfinger"}
DESC_MAX = 120
CODE_MAX = 2000


def fetch(cache):
	"""libretro-database at COMMIT, only cht/, metadat/no-intro/ and its licence"""
	repo = os.path.join(cache, "libretro-database")
	if not os.path.isdir(os.path.join(repo, ".git")):
		os.makedirs(cache, exist_ok=True)
		subprocess.check_call(["git", "clone", "-q", "--filter=blob:none", "--no-checkout", REPO, repo])
	run = lambda *a: subprocess.check_call(["git", "-C", repo] + list(a))
	paths = ["/LICENSE"] + ["/metadat/no-intro/%s.dat" % d for v in SYSTEMS.values() for d in v[1]]
	paths += ["/cht/%s/*" % d for v in SYSTEMS.values() for d in v[0]]
	run("sparse-checkout", "init", "--no-cone")
	run("sparse-checkout", "set", "--no-cone", *paths)
	if subprocess.run(["git", "-C", repo, "cat-file", "-e", COMMIT + "^{commit}"]).returncode:
		run("fetch", "-q", "origin", COMMIT)
	run("checkout", "-q", "--detach", COMMIT)
	return repo


def norm(s):
	"""a name as both sides compare it: lower case ASCII letters and digits only, the rest kept
	byte for byte (the LK does the same with the card's file name, cheats.c)"""
	out = bytearray()
	for b in s.encode("utf-8"):
		if 0x41 <= b <= 0x5A:
			out.append(b + 32)
		elif 0x61 <= b <= 0x7A or 0x30 <= b <= 0x39 or b >= 0x80:
			out.append(b)
	return bytes(out)


def fnv1a(b):
	h = 0x811C9DC5
	for c in b:
		h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
	return h


# the one letter regions older names use
REGION_LETTERS = {"u": "usa", "e": "europe", "j": "japan", "g": "germany", "f": "france", "s": "spain",
		  "i": "italy", "k": "korea", "w": "world", "ue": "usa, europe", "ju": "usa, japan"}


def split_name(s):
	"""'Title (A) (B, C) [!]' -> ('Title', ['a', 'b, c', '!']): the title is what comes before the
	first bracket, the tags are what each pair of brackets holds"""
	m = re.match(r"^(.*?)\s*([\(\[].*)?$", s)
	title, rest = m.group(1), m.group(2) or ""
	groups = [g.strip().lower() for g in re.findall(r"[\(\[]([^\)\]]*)[\)\]]", rest)]
	return title, [REGION_LETTERS.get(g, g) for g in groups if g]


# the words No-Intro names a region with (and "world" and "unknown", which go with any)
REGION_WORDS = {"usa", "europe", "japan", "world", "unknown", "germany", "france", "spain", "italy",
		"korea", "australia", "brazil", "asia", "china", "taiwan", "hong kong", "canada", "sweden",
		"netherlands", "uk", "scandinavia", "russia", "denmark", "finland", "norway", "portugal",
		"greece", "latin america", "mexico", "argentina", "india", "poland"}


def region_tag(groups):
	"""the tag that names the regions ("usa, europe"), or None: the first whose every part is a
	region, wherever it stands ("Game (v1.1) (USA)" has it second)"""
	for g in groups:
		parts = [r.strip() for r in g.split(",")]
		if parts and all(p in REGION_WORDS for p in parts):
			return g
	return None


def regions_of(groups):
	"""the regions a name's region tag gives (none: the name has no region tag)"""
	g = region_tag(groups)
	return set(r.strip() for r in g.split(",")) if g else set()


def read_dat(path):
	"""[(game name, [crc, ...])]"""
	games = []
	text = open(path, encoding="utf-8", errors="replace").read()
	for m in re.finditer(r'^game \(\n(.*?)^\)', text, re.M | re.S):
		body = m.group(1)
		n = re.search(r'^\tname "(.*)"$', body, re.M)
		if not n:
			continue
		crcs = [int(c, 16) for c in re.findall(r'\bcrc ([0-9A-Fa-f]{8})\b', body)]
		games.append((n.group(1), crcs))
	return games


def read_cht(path):
	"""[(description, code)] of the cheats that have a code to send"""
	vals = {}
	for line in open(path, encoding="utf-8", errors="replace"):
		m = re.match(r'^\s*cheat(\d+)_(desc|code)\s*=\s*"?(.*?)"?\s*$', line)
		if m:
			vals.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
	out, seen = [], set()
	for i in sorted(vals):
		code = re.sub(r"\s+", " ", vals[i].get("code", "")).strip()
		desc = re.sub(r"\s+", " ", vals[i].get("desc", "")).strip() or code
		if not code or len(code) > CODE_MAX or re.search(r"[xX]{2}|\?\?", code.replace("0x", "")):
			continue
		if not re.fullmatch(r"[0-9A-Za-z+:;,.=?/ -]+", code):
			continue
		if len(desc.encode("utf-8")) > DESC_MAX:
			desc = desc.encode("utf-8")[:DESC_MAX].decode("utf-8", "ignore").rstrip() + "..."
		if code in seen:
			continue
		seen.add(code)
		out.append((desc, code))
	return out


def build(repo, key, out_dir, stats):
	folders, dats = SYSTEMS[key]
	games = []
	for d in dats:
		games += read_dat(os.path.join(repo, "metadat/no-intro/%s.dat" % d))
	by_title = {}
	for gi, (name, _) in enumerate(games):
		t, groups = split_name(name)
		by_title.setdefault(norm(t), []).append((gi, groups))
	# each cheat file: its format, its cheats, the games it goes with, the names it is filed under
	records = {}       # frozenset of (folder, file) -> record index
	rec_files = []     # record -> [(format, cheats)]
	rec_keys = []      # record -> set of name keys
	rec_title = []     # record -> the name it is listed under for choosing by hand
	crc_of = {}        # crc -> record
	name_of = {}       # name key -> record
	game_files = {}    # game index -> [(folder, file)]
	file_info = {}
	for folder in folders:
		cdir = os.path.join(repo, "cht", folder)
		if not os.path.isdir(cdir):
			continue
		for f in sorted(os.listdir(cdir)):
			if not f.endswith(".cht"):
				continue
			cheats = read_cht(os.path.join(cdir, f))
			if not cheats:
				stats["empty"] += 1
				continue
			stem = f[:-4]
			title, groups = split_name(stem)
			fmt = ""
			for g in groups:
				if g in FORMATS:
					fmt = FORMATS[g]
			groups = [g for g in groups if g not in FORMATS]
			# the name without its format, as the card may name the game, and as the file writes it
			shown = re.sub(r"\s*[\(\[]([^\)\]]*)[\)\]]", lambda m: "" if m.group(1).strip().lower() in FORMATS else m.group(0), stem).strip()
			stem = (title + "".join(" (%s)" % g for g in groups)) if fmt else stem
			file_info[(folder, f)] = (fmt, cheats, stem, shown)
			best, hits = 0, []
			regions = regions_of(groups)
			anywhere = regions <= {"world", "unknown"}
			for gi, dgroups in by_title.get(norm(title), []):
				if regions and not anywhere and not regions & regions_of(dgroups):
					continue   # another region's dump: its addresses are not these
				# the same tags first, then a region the two share ("USA, Europe" for "USA")
				rt = region_tag(groups)
				score = 1 + (rt is not None and rt == region_tag(dgroups)) + \
					sum(1 for g in groups if g != rt and g in dgroups)
				if score > best:
					best, hits = score, [gi]
				elif score == best:
					hits.append(gi)
			for gi in hits:
				game_files.setdefault(gi, []).append((folder, f))
			stats["matched" if hits else "by_name"] += 1
	# a record per set of files; a game's CRCs and names point at it
	def record_for(files):
		k = frozenset(files)
		if k not in records:
			records[k] = len(rec_files)
			rec_files.append([file_info[x][:2] for x in sorted(files)])
			rec_keys.append(set())
			rec_title.append(file_info[sorted(files)[0]][3])
		return records[k]
	for gi, files in sorted(game_files.items()):
		r = record_for(files)
		name, crcs = games[gi]
		for c in crcs:
			if c in crc_of and crc_of[c] != r:
				stats["crc_shared"] += 1
				continue
			crc_of[c] = r
		rec_keys[r].add(norm(name))
	# the file's own name too, for a card whose games are named as the cheat files are: the first
	# record holding the file (several formats of one game share a name, and a record)
	owner = {}
	for files, r in sorted(records.items(), key=lambda x: x[1]):
		for fk in files:
			owner.setdefault(fk, r)
	claimed = set(k for keys in rec_keys for k in keys)
	for fk in sorted(file_info):
		k = norm(file_info[fk][2])
		if k in claimed:
			continue
		claimed.add(k)
		r = owner[fk] if fk in owner else record_for([fk])
		rec_keys[r].add(k)
	for r, keys in enumerate(rec_keys):
		for k in keys:
			name_of.setdefault(k, r)
	# the file
	blobs = []
	for r, files in enumerate(rec_files):
		lines = ["K" + k.decode("utf-8", "replace") for k in sorted(rec_keys[r])]
		for fmt, cheats in files:
			lines.append("#" + fmt)
			lines += ["C%s\t%s" % (d.replace("\t", " "), c) for d, c in cheats]
		text = ("\n".join(lines) + "\n").encode("utf-8")
		co = zlib.compressobj(9, zlib.DEFLATED, -15)
		blobs.append((co.compress(text) + co.flush(), len(text)))
		stats["cheats"] += sum(len(c) for _, c in files)
		stats["max_plain"] = max(stats["max_plain"], len(text))
	crcs = sorted(crc_of.items())
	names = sorted((fnv1a(k), r) for k, r in name_of.items())
	# the titles, for choosing a list by hand: in the order a menu shows them (case aside)
	titles = sorted(((t, r) for r, t in enumerate(rec_title)), key=lambda x: (x[0].lower(), x[0]))
	hdr = 40
	games_off = hdr
	crc_off = games_off + 12 * len(blobs)
	name_off = crc_off + 8 * len(crcs)
	title_off = name_off + 8 * len(names)
	text_off = title_off + 8 * len(titles)
	title_text = bytearray()
	title_at = []
	for t, r in titles:
		title_at.append((text_off + len(title_text), r))
		title_text += t.encode("utf-8") + b"\0"
	data_off = text_off + ((len(title_text) + 3) & ~3)
	out = bytearray(struct.pack("<4s9I", b"GCD1", 2, len(blobs), games_off, len(crcs), crc_off,
				    len(names), name_off, len(titles), title_off))
	at = data_off
	for b, plain in blobs:
		out += struct.pack("<3I", at, len(b), plain)
		at += len(b)
	for c, r in crcs:
		out += struct.pack("<2I", c, r)
	for h, r in names:
		out += struct.pack("<2I", h, r)
	for a, r in title_at:
		out += struct.pack("<2I", a, r)
	out += title_text + b"\0" * (((len(title_text) + 3) & ~3) - len(title_text))
	assert len(out) == data_off
	for b, _ in blobs:
		out += b
	path = os.path.join(out_dir, key + ".gcd")
	with open(path, "wb") as fh:
		fh.write(out)
	stats["records"] += len(blobs)
	return path, len(out), len(blobs), len(crcs)


def main():
	if len(sys.argv) < 2:
		sys.exit(__doc__)
	card = sys.argv[1]
	cache = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "out", "cheats-cache")
	repo = fetch(os.path.abspath(cache))
	out_dir = os.path.join(card, "gammaos", "cheats")
	os.makedirs(out_dir, exist_ok=True)
	for key in SYSTEMS:
		stats = dict.fromkeys(["matched", "by_name", "empty", "crc_shared", "cheats", "records", "max_plain"], 0)
		path, size, nrec, ncrc = build(repo, key, out_dir, stats)
		print("%-5s %8d bytes  %5d records  %6d dumps  %6d cheats  files: %d matched, %d by name, %d empty  (largest %d)"
		      % (key, size, nrec, ncrc, stats["cheats"], stats["matched"], stats["by_name"], stats["empty"], stats["max_plain"]))
	lic = os.path.join(card, "licences", "cheats")
	os.makedirs(lic, exist_ok=True)
	with open(os.path.join(repo, "LICENSE"), "rb") as src, open(os.path.join(lic, "LICENSE"), "wb") as dst:
		dst.write(src.read())
	with open(os.path.join(lic, "SOURCE.txt"), "w") as fh:
		fh.write("The cheat packs in gammaos/cheats are made from libretro-database's cheat files (cht/) and\n"
			 "No-Intro lists (metadat/no-intro/), %s at commit %s, by\n"
			 "tools/cheats/build_cheats.py in the LK tree. They are under the Creative Commons Attribution-\n"
			 "ShareAlike 4.0 licence (LICENSE), as libretro-database is: the cheats were gathered by the\n"
			 "libretro-database contributors. The packs hold the same cheats in another form, under the same\n"
			 "licence.\n" % (REPO, COMMIT))


if __name__ == "__main__":
	main()
