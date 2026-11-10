#!/bin/sh

# Validate the machine-readable production/fallback/rejected codec contract.
set -eu
root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
python3 - "$root/data/iris-codec-capabilities.json" "$root" <<'PY'
import json, pathlib, re, sys

path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
doc = json.loads(path.read_text())
assert doc["schema_version"] == 2
assert doc["policy"]["unknown_codec_action"] == "software-fallback"
entries = {item["codec"]: item for item in doc["codecs"]}
required = {"h264", "hevc", "vp9", "av1"}
assert set(entries) == required, (set(entries), required)
for codec, item in entries.items():
    assert item["production"] in {"supported", "fallback", "rejected"}, codec
    assert isinstance(item["va_profiles"], list), codec
    assert isinstance(item["v4l2_output_formats"], list), codec
    assert isinstance(item["capture_formats"], list), codec
    assert item["fallback"] in {"software", "native-v4l2-or-software"}, codec
    assert isinstance(item["rejected"], list) and item["rejected"], codec
    assert isinstance(item["experimental"], list), codec
    assert item["publish_import"] in {"qualified", "fallback"}, codec
assert entries["h264"]["production"] == "supported"
assert entries["hevc"]["production"] == "supported"
assert entries["vp9"]["production"] == "supported" and "VP9Profile0" in entries["vp9"]["va_profiles"]
assert entries["av1"]["production"] == "supported" and "AV1Profile0" in entries["av1"]["va_profiles"]
# Encode is a separate entrypoint with its own qualification: a codec may
# decode without encoding, and only what the encoder node exposes may be
# advertised.
for codec, item in entries.items():
    enc = item["encode"]
    assert enc["production"] in {"supported", "absent"}, codec
    assert isinstance(enc["va_profiles"], list), codec
    assert isinstance(enc["v4l2_capture_formats"], list), codec
    assert isinstance(enc["raw_formats"], list), codec
    assert isinstance(enc["rate_control"], list), codec
    assert isinstance(enc["rejected"], list) and enc["rejected"], codec
    assert ("NV12" in enc["raw_formats"]) == (enc["production"] == "supported"), codec
assert entries["vp9"]["encode"]["production"] == "absent"
assert entries["av1"]["encode"]["production"] == "absent"
# The driver decides the publish policy from its own import table; it must
# agree with this document in both directions.
driver = (pathlib.Path(sys.argv[1]).parent.parent / "src/va/driver.cc").read_text()
# The decode profiles the driver advertises are exactly the qualified ones
# this document lists for a supported codec.
block = driver[driver.index("profile-qualified-begin"):driver.index("profile-qualified-end")]
decode_profiles = set(re.findall(r"VAProfile([A-Za-z0-9_]+)", block))
want_decode = {p for item in entries.values() if item["production"] == "supported"
               for p in item["va_profiles"]}
assert decode_profiles == want_decode, (decode_profiles, want_decode)
block = driver[driver.index("import-qualified-begin"):driver.index("import-qualified-end")]
names = {name.upper() for name in re.findall(r"V4L2_PIX_FMT_([A-Za-z0-9_]+)", block)}
want = {codec.upper() for codec, item in entries.items() if item["publish_import"] == "qualified"}
assert names == want, (names, want)
# A codec decodes into P010 exactly when one of its profiles is in the
# driver's 10-bit set. AV1 carries both depths inside Profile 0, so the set
# is not derivable from the profile name alone.
p010 = driver[driver.index("bool p010_decode_profile"):driver.index("iris::Publish publish_policy")]
p010_profiles = set(re.findall(r"VAProfile([A-Za-z0-9_]+)", p010))
for codec, item in entries.items():
    has_p010 = "P010" in item["capture_formats"]
    assert has_p010 == bool(set(item["va_profiles"]) & p010_profiles), (codec, has_p010, p010_profiles)
# Same rule for the encode entrypoint: the advertised profiles and capture
# formats are exactly the qualified ones in this document.
block = driver[driver.index("encode-qualified-begin"):driver.index("encode-qualified-end")]
encode_profiles = set(re.findall(r"VAProfile([A-Za-z0-9_]+)", block))
encode_formats = set(re.findall(r"V4L2_PIX_FMT_([A-Za-z0-9_]+)", block))
want_profiles = {p for item in entries.values() for p in item["encode"]["va_profiles"]}
want_formats = {f for item in entries.values() for f in item["encode"]["v4l2_capture_formats"]}
assert encode_profiles == want_profiles, (encode_profiles, want_profiles)
assert encode_formats == want_formats, (encode_formats, want_formats)
# The English and Chinese README capability tables are prose, but they make
# the same publish-policy claim. A table that still says AV1 is Copy while the
# table says qualified is the drift this check exists to catch: the capability
# JSON is the single source of truth, and the driver is checked against it
# above. Read both tables and bind their Import/Copy words to it.
for name in ("README.md", "README.zh-CN.md"):
    text = (root / name).read_text()
    # Decode table only: the encode table has the same codec names but its
    # second and third columns are the entrypoint and input format. The
    # first table in the Capabilities section is the decode table in both
    # languages, so stop at the next heading rather than translating names.
    decode = text[text.index("### "):].split("\n### ", 1)[0]
    rows = dict(re.findall(r"^\|\s*(H\.264|HEVC|VP9|AV1)\s*\|[^|]*\|([^|]*)\|", decode, re.M))
    assert set(rows) == {"H.264", "HEVC", "VP9", "AV1"}, (name, rows)
    for codec, item in entries.items():
        label = {"h264": "H.264", "hevc": "HEVC", "vp9": "VP9", "av1": "AV1"}[codec]
        mode = rows[label]
        qualified = item["publish_import"] == "qualified"
        if qualified:
            assert "Import" in mode and "Copy" not in mode, (name, label, mode, "qualified")
        else:
            assert "Copy" in mode and "Import" not in mode, (name, label, mode, "fallback")
print(f"PASS codec capability matrix entries={len(entries)} profiles={sorted(decode_profiles)}"
      f" import-qualified={sorted(names)} encode-qualified={sorted(encode_profiles)}")
PY
