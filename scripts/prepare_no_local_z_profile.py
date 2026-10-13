"""Create a benchmark-only 3MF copy with global and recipe-triggered Local-Z off.

Gradients independently enable Local-Z in this implementation. Disable their
legacy r1 flags while retaining the component IDs, weights and other recipe
fields. This changes the intended toolpaths; it is a feature-cost comparison,
not an equivalent-output optimization benchmark. Never overwrite the source.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or output.exists():
        parser.error("Output must be a new file distinct from the source.")
    member = "Metadata/project_settings.config"
    original_hash = file_hash(source)
    with zipfile.ZipFile(source) as archive:
        original = json.loads(archive.read(member))
        modified = dict(original)
        modified["dithering_local_z_mode"] = "0"
        modified["mixed_filament_gradient_mode"] = "0"
        recipes = original["mixed_filament_definitions"].split(";")
        changed_recipes = []
        for index, recipe in enumerate(recipes):
            fields = recipe.split(",")
            changes = []
            for i, field in enumerate(fields):
                match = re.fullmatch(r"r([01])/(.+)", field)
                if match and match[1] == "1":
                    fields[i] = "r0/" + match[2]
                    changes.append({"before": field, "after": fields[i]})
            recipes[index] = ",".join(fields)
            if changes:
                changed_recipes.append({"row_1_based": index + 1, "changes": changes})
        modified["mixed_filament_definitions"] = ";".join(recipes)
        settings_changes = {k: {"before": original.get(k), "after": v}
                            for k, v in modified.items() if v != original.get(k)}
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as stream, zipfile.ZipFile(stream, "w") as target:
            target.comment = archive.comment
            for info in archive.infolist():
                data = archive.read(info.filename)
                if info.filename == member:
                    data = json.dumps(modified, ensure_ascii=False, indent=2).encode("utf-8")
                target.writestr(info, data)
    members = []
    with zipfile.ZipFile(source) as before, zipfile.ZipFile(output) as after:
        assert before.namelist() == after.namelist()
        for name in before.namelist():
            a, b = before.read(name), after.read(name)
            identical = a == b
            assert name == member or identical, name
            members.append({"name": name, "identical_uncompressed": identical,
                            "before_sha256": hashlib.sha256(a).hexdigest(),
                            "after_sha256": hashlib.sha256(b).hexdigest()})
        assert json.loads(after.read(member)) == modified
    assert file_hash(source) == original_hash
    manifest = {"source": str(source), "output": str(output), "source_sha256": original_hash,
                "output_sha256": file_hash(output), "settings_changes": settings_changes,
                "changed_recipes": changed_recipes, "recipe_count": len(recipes), "members": members}
    output.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps({k: manifest[k] for k in ("source_sha256", "output_sha256", "changed_recipes", "recipe_count")}, indent=2))


if __name__ == "__main__":
    main()
