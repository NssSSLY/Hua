"""Package the dependency-free Hua extension using the VSIX container format."""
import argparse
import json
import zipfile
from pathlib import Path
from xml.etree.ElementTree import Element, SubElement, tostring

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path)
args = parser.parse_args()
source = root / "editors" / "vscode"
package = json.loads((source / "package.json").read_text(encoding="utf-8"))
output = args.output or root / "build" / f"{package['name']}-{package['version']}.vsix"
output.parent.mkdir(parents=True, exist_ok=True)
ns = "http://schemas.microsoft.com/developer/vsx-schema/2011"
manifest = Element("PackageManifest", {"Version": "2.0.0", "xmlns": ns})
metadata = SubElement(manifest, "Metadata")
SubElement(metadata, "Identity", {"Language": "en-US", "Id": package['name'], "Version": package['version'], "Publisher": package['publisher']})
SubElement(metadata, "DisplayName").text = package['displayName']
SubElement(metadata, "Description", {"xml:space": "preserve"}).text = package['description']
SubElement(metadata, "Tags").text = "hua,language"
SubElement(metadata, "Categories").text = "Programming Languages"
SubElement(metadata, "GalleryFlags").text = "Public"
properties = SubElement(metadata, "Properties")
SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.Engine", "Value": package['engines']['vscode']})
SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.ExtensionDependencies", "Value": ""})
SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.ExtensionPack", "Value": ""})
SubElement(properties, "Property", {"Id": "Microsoft.VisualStudio.Code.ExecutesCode", "Value": "true"})
targets = SubElement(manifest, "Installation")
SubElement(targets, "InstallationTarget", {"Id": "Microsoft.VisualStudio.Code"})
SubElement(manifest, "Dependencies")
assets = SubElement(manifest, "Assets")
SubElement(assets, "Asset", {"Type": "Microsoft.VisualStudio.Code.Manifest", "Path": "extension/package.json", "Addressable": "true"})
SubElement(assets, "Asset", {"Type": "Microsoft.VisualStudio.Services.Content.Details", "Path": "extension/README.md", "Addressable": "true"})
SubElement(assets, "Asset", {"Type": "Microsoft.VisualStudio.Services.Content.License", "Path": "extension/LICENSE", "Addressable": "true"})
content = Element("Types", {"xmlns": "http://schemas.openxmlformats.org/package/2006/content-types"})
for extension, mime in [("json", "application/json"), ("cjs", "application/javascript"), ("md", "text/markdown"), ("vsixmanifest", "text/xml")]:
    SubElement(content, "Default", {"Extension": extension, "ContentType": mime})
SubElement(content, "Override", {"PartName": "/extension/LICENSE", "ContentType": "text/plain"})
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("extension.vsixmanifest", tostring(manifest, encoding="utf-8", xml_declaration=True))
    archive.writestr("[Content_Types].xml", tostring(content, encoding="utf-8", xml_declaration=True))
    for path in sorted(source.rglob("*")):
        if path.is_file() and not path.is_symlink():
            archive.write(path, "extension/" + path.relative_to(source).as_posix())
print(output)
