# The Flatpak

`com.jacksonv.Semibreve.yml` builds Semibreve on KDE's Flatpak runtime; the
main README has the build commands.

## Python packages

The packages from PyPI are pinned in `requirements.txt`. `python3-deps.yaml`
is generated from it by flatpak-builder-tools'
[flatpak-pip-generator](https://github.com/flatpak/flatpak-builder-tools/tree/master/pip),
with a checksum for every download, so the build itself needs no network.
After changing a pin, regenerate it from the repository root, in the SDK the
app builds with:

```bash
mkdir -p build/pipgen
curl -fsSLo build/pipgen/flatpak-pip-generator.py \
    https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/master/pip/flatpak-pip-generator.py
flatpak run --share=network --filesystem="$PWD" --command=sh org.kde.Sdk//6.11 -c '
    cd flatpak
    python3 -m pip install -q --target ../build/pipgen/deps packaging requirements-parser pyyaml
    PYTHONPATH=../build/pipgen/deps python3 ../build/pipgen/flatpak-pip-generator.py \
        --yaml --requirements-file requirements.txt --output python3-deps'
```
