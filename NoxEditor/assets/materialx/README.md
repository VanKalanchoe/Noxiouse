# MaterialX definitions (third party)

From the MaterialX project (https://github.com/AcademySoftwareFoundation/MaterialX, commit df6174a3e9d0a394b8ad094eda6b04c11b23c7f0),
Apache License 2.0 (`LICENSE.txt`), unmodified:

- `standard_surface.mtlx`, `open_pbr_surface.mtlx`, `gltf_pbr.mtlx`, `usd_preview_surface.mtlx` (libraries/bxdf): the shaders'
  input defaults.
- `standard_surface_to_gltf_pbr.mtlx`, `open_pbr_to_standard_surface.mtlx` (libraries/bxdf/translation): the official translation
  graphs the MaterialX importer (`NoxCore/src/NoxCore/Asset/MaterialXImporter.cpp`) evaluates to map a .mtlx material onto Nox's
  glTF-based material.
