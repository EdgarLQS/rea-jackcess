# Jackcess 5.2.0 empty database templates

These two binary templates are copied from the `jackcess-5.2.0` tag at commit
`6159c680933625ffa952334b5e0e6b24f37bfdfe` in
[`spannm/jackcess`](https://github.com/spannm/jackcess):

| Local file | Upstream resource | Size | SHA-256 |
|---|---|---:|---|
| `empty-v2000.mdb` | `src/main/resources/io/github/spannm/jackcess/empty.mdb` | 98,304 bytes | `521c6f8a60f94273866367f69c772eb590fc7fcb8bc91bcb134d567b1e9b6566` |
| `empty-v2003.mdb` | `src/main/resources/io/github/spannm/jackcess/empty2003.mdb` | 204,800 bytes | `c7dc939c0afb5706d209ea54cfd1dfc7e67996767a0ed357c0fd981b9012c955` |

The upstream project distributes these resources under the Apache License 2.0;
the license text is included as `LICENSE.txt`. CMake embeds the files into the
core static library, so database creation has no runtime dependency on the
Jackcess JAR or on external template paths.

## Text index weights

The General Legacy, General, and General 97 index-code tables are copied from
the same upstream commit and resource directory under the Apache License 2.0.
CMake checks their hashes and embeds the tables into the static library.

| Resource | SHA-256 |
|---|---|
| `index_codes_genleg.txt` | `acc97846553560c0b2cdde822c990cea2a2bea733ca071e850e180b9ce3cd9ee` |
| `index_codes_ext_genleg.txt` | `d60378601aebc22f579b8c5dd35a4c0d3e77c371275372f25511d73fe3e92ae7` |
| `index_codes_gen.txt` | `a0554f81d328732d3d6fcac5d44476130efeaccb96bdc1eda30590dd71ecb8b5` |
| `index_codes_ext_gen.txt` | `0f54f68c0592ba5066d505cb3b6782e64edb344da6a0a24959277dea9a478c28` |
| `index_codes_gen_97.txt` | `ad74d251bd8cf441976446e50d84bc7366c28e0ba2c970ada1b52904d816bad9` |
| `index_mappings_ext_gen_97.txt` | `60cb99d942c69c778bc4f2162af54168b6b3d631db502bce5d4b3a318883935c` |
